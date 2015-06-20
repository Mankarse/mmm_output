#if 1
//
#include "main.h"
#include "command_parser.h"

#include <alsa/asoundlib.h>
#include <sndfile.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <errno.h>
#include <getopt.h>

#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <sys/time.h>

#include <limits.h>
#include <string.h>

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <math.h>

#include <stdbool.h>

#include <systemd/sd-daemon.h>

int inputfd = -1;
int outputfd = -1;
/*
static FILE *input_source = 0;//stdin
static FILE *output_sink = 0;//stdout
static FILE *error_sink = 0;//stderr
static FILE *log_sink = 0;//log file
*/
//Number that is printed and incremented on each write to error_sink or log_sink;
//Used to determine when lines in each file were printed relative to each other.
//static unsigned long long output_count = 0;


static char *device = "default";         /* playback device */
static snd_pcm_format_t format = SND_PCM_FORMAT_S16;    /* sample format */
static unsigned int rate = 44100;           /* stream rate */
static unsigned int channels = 2;           /* count of channels */
static unsigned int buffer_time = 500000;       /* ring buffer length in us */
static unsigned int period_time = 100000;       /* period time in us */
static int verbose = 0;                 /* verbose flag */
static int resample = 1;                /* enable alsa-lib resampling */
static int period_event = 0;                /* produce poll event after each period */
static snd_pcm_sframes_t buffer_size;
static snd_pcm_sframes_t period_size;
static snd_output_t *output = NULL;

//Need a strategy for handling errors:
// Which errors to attempt to recover from?
// At what level?
// How to report errors?
// How to consistently manage errors in callbacks?

//Code with very high correctness requirements (e.g. avionics) generally follows these rules:
// No Recursion
// Fixed maximum loop length
// No memory allocation apart from at initialisation
// Use 'assert' liberally
// Use warnings and static analysis
// Avoid obtuse code

//This is not possible for us, since we are writing application software, for which
//functionallity beats absolute non-failure.
//The following features are not compatible with all of the above rules:
// Arbitrary length URL/Tracks
// Arbitrary length playlist (this is not strictly necessary)
// Other stuff idk

//TODO (up next):
// Seek
// GetState
// Gap-free track transitions.
// Delays (and volume and other effects?)
// Multiple outputs (file/stream/test/DAC) (switched on per-track basis or global basis? (or both))
//  Synchronisation of multiple outputs.
//  Perhaps as another part of the system, a 'broadcaster' concept,
//   so there can be multiple players over the same database.
//  (A player can subscribe to a broadcaster. Each broadcaster has an independent playlist.
//    For example, one player could use the sound output of the browser,
//    while another uses the USB output on the music server. If each were subscribed to a different broadcaster they would play different things.
//   If there is a 'broadcaster' concept, perhaps each player would only need one output (but having multiple outputs from one player
//   simplifies synchronisation perhaps))
// Multiple data formats (requires starting/stopping output devices dynamically.)
// Direct Stream Digital support (requires some sort of rethinking of how we treat samples)
// Sequencer, support for rendering of MIDI (and similar formats)


//TODO: dynamic number of channels, rather than hardcoding 2;
//      will require changes to the rest of the code.
//      In particular, a significant restructuring would
//      allow for dynamic output formats (channels, sample rate).
//      This is only a slightly useful feature. Generally, hardware
//      only supports a small number of formats, and input files are only in a small number of formats.
//      So the format could work if it is set up at compile time or at initialisation time.
//      However, ideally the format could dynamically change, to allow bit-perfect output whenever
//      the hardware/input combination allows, and to allow playing of non-hardware supported
//      formats through resampling.
#define NUM_CHANNELS 2

void destroy_GUID(GUID *guid) {
	free(guid->guid_str);
}
void destroy_Track(Track *track) {
	free(track->url);
}

void destroy_TrackPlayIdentifier(TrackPlayIdentifier *track) {
	destroy_GUID(&track->guid);
	destroy_Track(&track->track);
}
void delete_TrackPlayIdentifier(TrackPlayIdentifier *track) {
	destroy_TrackPlayIdentifier(track);
	free(track);
}

void delete_TrackPlayIdentifierNode(TrackPlayIdentifierNode *node) {
	destroy_TrackPlayIdentifier(&node->track);
	free(node);
}
void delete_forwards_TrackPlayIdentifierNode(TrackPlayIdentifierNode *track) {
	while (track) {
		TrackPlayIdentifierNode *next_track = track->next;
		delete_TrackPlayIdentifierNode(track);
		track = next_track;
	}
}


void free_command(struct Command *command) {
	if (command) {
		switch (command->type) {
			case UpdatePlaylist:
			case ReplacePlaylist:
				delete_forwards_TrackPlayIdentifierNode(command->data.new_playlist.front);
			break;
			case Halt:
			case Pause:
			case Resume:
			break;
			//default:break;
		}
		free(command);
	}
}

typedef struct {
	struct Command *front;
	struct Command *back;
} CommandQueue;

int queue_command(CommandQueue *commandQueue, struct Command* command) {
	//Adds `command` to the front of the queue;
	if (!commandQueue->front) {
		commandQueue->front = command;
		commandQueue->back = command;
		command->next = 0;
		command->prev = 0;
	}
	else {
		command->next = commandQueue->front;
		commandQueue->front->prev = command;
		commandQueue->front = command;
		command->prev = 0;
	}
	//printf("Enqueued command\n");
	return 0;
}

struct Command *dequeue_command(CommandQueue *commandQueue) {
	//Removes the the command from the end of the queue
	//and returns it.
	//Returns NULL on the queue being empty;
	//printf("Trying command dequeue\n");
	struct Command *command = commandQueue->back;
	if (!command) goto end;

	//printf("Doing command dequeue\n");
	commandQueue->back = commandQueue->back->prev;

	if (commandQueue->back == 0) {
		commandQueue->front = 0;
	}
	else {
		commandQueue->back->next = 0;
	}
	command->next = 0;
	command->prev = 0;

	end:
	return command;
}
typedef struct {
	FileCache cache;
	TrackPlayIdentifier *track;

	//Describe the positivity/negativity of the previous
	//frame of the track. Used to determine whether the
	//track can be muted/unmuted without a pop.
	bool prev_non_negative[NUM_CHANNELS]; //Maybe put these into FileCache
	bool prev_non_positive[NUM_CHANNELS];
	bool muted[NUM_CHANNELS];
} TrackPlayState;
void nullify_TrackPlayState(TrackPlayState *state);
typedef struct {
	//bool left_prev_positive;
	//bool left_prev_negative;
	//bool right_prev_positive;
	//bool right_prev_negative;
	//bool left_muted;
	//bool right_muted;
	//TrackPlayIdentifier *old;
	//TrackPlayIdentifier *new;
	//Other stuff.. Remember that a single get_frames *could* go through multiple tracks,
	//in the worst case.
	//bool active;
	//bool pausing;
	//bool resuming;


	//float volume;
	//(start frame, end frame, muting/unmuting)

	bool reached_end_of_track_when_pausing;

	TrackPlayState *currentTrack;
	//Playlist *playlist;
	//FileCacheList *cachelist;
} PauseResumeManager;
enum PlayerStatePhase {
	PLAYING, //Currently playing something.
	PAUSING, //Currently looking for break in track (to stop current track).
	RESUMING,//Currently looking for break in track (to resume current track).
	SILENCE  //Currently playing nothing.
};
typedef struct {
	enum PlayerStatePhase phase;
	bool want_silence;  //True if the system is in the state where it doesn't want to play anything (paused)
	                    //False if the system is in the state where it wants to play every track in its buffer (resumed)

} PlayerState;
//typedef struct {} CacheManager;
typedef struct {
	Playlist playlist; //Contains every track in the playlist, including the currently
	                   //playing track. If the playlist has just been updated,
	                   //it may be the case that the currently playing track does not
	                   //exist as the first element of the playlist.
	                   //In this case, the player should detect this and stop
	                   //the currentTrack and move on to the first element of the playlist.

	//CacheManager cache_manager; //manages shared resources for the cachelist,
	                              //e.g., for now, the file_caches would
	                              //perform their loading on a separate thread, so
	                              //the cache_manager would manage the lifetime of that thread.
	FileCacheList cachelist; //Contains caches for upcoming tracks.
	                         //The cache for the current track is not here,
	                         //but rather is in currentTrack (it is moved there when the
	                         //current track starts playing).
	                         //Tracks that are a long way off may not have an element in this list
	                         //For now, only the 'next' track
	                         //will be in the cachelist, and it will simply load on a separate thread.
	                         //Once I've figured out non-blocking audio-reading/decoding, the loading
	                         //may be moved into a central event loop.
	//The track that is *actually* playing, regardless of whether
	//or not a pause/skip/whatever has been requested.
	//(Contains the reference to the cache, the TrackPlayIdentifier of the track, and
	//  (implicitly, in the current implementation) the information about how far through the
	//  track the player currently is.)
	TrackPlayState currentTrack;
	PauseResumeManager play_resume_manager;
	PlayerState state;
	//void get_frames(Uint8 *const stream, int const len);
	//void pause();
	//void resume();
	//void update_playlist(Playlist *new_playlist);
} PlaylistPlayer;

void init_PlaylistPlayer(PlaylistPlayer *player) {
	player->playlist.front = 0;
	player->playlist.back = 0;
	player->playlist.dirty = false;

	player->cachelist.front = 0;
	player->cachelist.back = 0;

	nullify_TrackPlayState(&player->currentTrack);

	player->play_resume_manager.currentTrack = &player->currentTrack;
	player->play_resume_manager.reached_end_of_track_when_pausing = false;

	player->state.phase = SILENCE;
	player->state.want_silence = true;
}


void destroy_Playlist(Playlist *playlist) {
	//For now, this is more like 'clear_Playlist' function.
	if (playlist->front) {
		playlist->dirty = true;
		delete_forwards_TrackPlayIdentifierNode(playlist->front);
		playlist->front = 0;
		playlist->back = 0;
	}
}
void destroy_CacheList(FileCacheList *cachelist) {
	delete_forwards_FileCacheNode(cachelist->front);
}

void free_PlaylistPlayer(PlaylistPlayer *player) {
	//assert(false);
	destroy_Playlist(&player->playlist);
	destroy_CacheList(&player->cachelist);
	//destroy_TrackPlayState 	//TODO? Or these don't actually require any deallocation.
	//destroy_PlayResumeManager
	//destroy_PlayerState
}
#if 0
//typedef struct {} Input;
struct ResumingInput {
    Input *nextInput;
};
struct PausingInput {
    Input *nextInput;
};
struct SilenceInput {

};
struct PlaylistInput {
    Playlist *playlist;
};
#endif
//States:
//Playing Playlist
//Going to pause from playing
//  Paused at that point
//  Empty/new playlist once paused
//Going to resume from paused
//  Playing Track
//Going to empty playlist from playing
//
//Going to non-empty playlist from not playing
//Going to new playlist from playing
//Going to start playing track from middle of physical track
//  Playing Track
//Going to stop playing track from middle of physical track
//

bool TrackPlayIdentifier_compatible_with(TrackPlayIdentifier const *l, TrackPlayIdentifier const *r) {
	if (l == 0 || r == 0) return l == 0 && r == 0;
	return strcmp(l->guid.guid_str, r->guid.guid_str) == 0 && strcmp(l->track.url, r->track.url) == 0;
}

void destroy_FileCache(FileCache *cache) {
	sf_close(cache->file);
}

void destroy_TrackPlayState(TrackPlayState *state) {
	if (state->track) {
		destroy_FileCache(&state->cache);
		delete_TrackPlayIdentifier(state->track);
	}
}

void nullify_TrackPlayState(TrackPlayState *state) {
	//destroy_TrackPlayState(state);
	state->track = 0;
}

void move_FileCache(FileCache *l, FileCache *r) {
	*l = *r;
	r->file = 0;
}

void delete_FileCacheNode(FileCacheNode *p) {
	destroy_FileCache(&p->cache);
	destroy_GUID(&p->track_guid);
	free(p);
}
int create_TrackCache(FileCache *cache, Track *track) {
	SF_INFO file_info;
	cache->file = sf_open(track->url, SFM_READ, &file_info);
	if (!cache->file) {
		fprintf(stderr, "unable to open file %s\n", track->url);
		return -1;
	}
	else {
		printf("Opened file: %s\n", track->url);
	}
	if (file_info.channels != NUM_CHANNELS) {
		fprintf(stderr, "Unsupported number of channels: %d We only support %d\n", file_info.channels, NUM_CHANNELS);
		return -2;
	}
	if (file_info.samplerate != 44100) {
		fprintf(stderr, "Unsupported sample rate: %d We only support %d\n", file_info.channels, 44100);
		return -3;
	}
	fprintf(stderr, "File contains %ld frames\n",file_info.frames);
	return 0;
}

TrackPlayIdentifier *copy_TrackPlayIdentifier(TrackPlayIdentifier *old) {
	if (!old) return 0;

	TrackPlayIdentifier *ret = malloc(sizeof *ret);
	if (!ret) return 0;
	ret->guid.guid_str = strdup(old->guid.guid_str);
	if (!ret->guid.guid_str) {
		free(ret);
		return 0;
	}
	ret->track.url = strdup(old->track.url);
	if (!ret->track.url) {
		free(ret->guid.guid_str);
		free(ret);
		return 0;
	}
	return ret;
}

void create_TrackPlayState(TrackPlayState *state, TrackPlayIdentifier *track, FileCache *file) {
	move_FileCache(&state->cache, file);

	state->track = copy_TrackPlayIdentifier(track);

	if (track && !state->track) {
		assert(false && "ERROR: Couldn't allocate memory. TODO-- properly report on these errors");
	}

	//TODO: Don't access FileCache members directly:
	sf_count_t offset = sf_seek(state->cache.file, -1, SEEK_CUR);
	if (offset != -1) {
		short prev_samples[NUM_CHANNELS];
		sf_count_t frame_was_read = sf_readf_short(state->cache.file, prev_samples, 1);
		if (frame_was_read) {
			for (size_t i = 0; i < NUM_CHANNELS; ++i) {
				state->prev_non_negative[i] = prev_samples[i] >= 0;
				state->prev_non_positive[i] = prev_samples[i] <= 0;
			}
		}
		else {
			assert(false && "Couldn't read section that was only just rewound...");
		}
	}
	else {
		//At start of file:
		for (size_t i = 0; i < NUM_CHANNELS; ++i) {
			state->prev_non_negative[i] = true;
			state->prev_non_positive[i] = true;
		}
	}

	for (size_t i = 0; i < NUM_CHANNELS; ++i) {
		state->muted[i] = true;
	}
}

bool all(bool *arr, size_t const len) {
	for (size_t i = 0; i != len; ++i) {
		if (!arr[i]) return false;
	}
	return true;
}

//TODO some way of signalling an error in do_pause.
size_t PauseResumeManager_do_pause(
    PauseResumeManager *pauser, size_t pos, short (*const out)[NUM_CHANNELS], size_t const out_len, bool *const pause_finished)
{
	//Current algorithm:
	// for each channel
	//  Search for zero crossing
	//  mute after zero crossing found
	//  if 0.1 seconds pass with no zero crossing found, spend next 0.1 seconds
	//  smoothly decreasing volume to 0
	// (Frames beyond the end of the track are taken to contain silence)
	// once all channels are muted, the pause is complete, so return.

	for (;pos != out_len && !all(pauser->currentTrack->muted, NUM_CHANNELS); ++pos) {
		//TODO -- Add 'volume-down' fallback path.
		short frames[NUM_CHANNELS];
		
		sf_count_t frames_read = sf_readf_short(pauser->currentTrack->cache.file, frames, 1);
		pauser->reached_end_of_track_when_pausing = frames_read == 0;
		if (frames_read == 0) {
			for (int chan = 0; chan != NUM_CHANNELS; ++chan) {
				frames[chan] = 0;
			}
		}
		for (int chan = 0; chan != NUM_CHANNELS; ++chan) {
			if (!pauser->currentTrack->muted[chan]) {
				if (
					(pauser->currentTrack->prev_non_negative[chan] && frames[chan] <= 0)
					||(pauser->currentTrack->prev_non_positive[chan] && frames[chan] >= 0))
				{
					pauser->currentTrack->muted[chan] = true;
					out[pos][chan] = 0;
				}
				else {
					out[pos][chan] = frames[chan];
				}
			}
			else {
				out[pos][chan] = 0;
			}
			
		}
		if (!all(pauser->currentTrack->muted, NUM_CHANNELS)) {
			for (int chan = 0; chan != NUM_CHANNELS; ++chan) {
				if (frames_read != 0) {
					pauser->currentTrack->prev_non_negative[chan] = frames[chan] >= 0;
					pauser->currentTrack->prev_non_positive[chan] = frames[chan] <= 0;
				}
			}
		}
	}
	if (all(pauser->currentTrack->muted, NUM_CHANNELS)) {
		//Move cur_pos in track backwards, so we start on exactly the frame where the music was muted.
		if (!pauser->reached_end_of_track_when_pausing) {
			sf_seek(pauser->currentTrack->cache.file,-1,SEEK_CUR);
		}
		pauser->reached_end_of_track_when_pausing = false;
		*pause_finished = true;
		//TODO? Other cleanup. The pause is over.
	}
	else {
		*pause_finished = false;
	}
	return pos;
}
bool any(bool *arr, size_t const len) {
	for (size_t i = 0; i != len; ++i) {
		if (arr[i]) return true;
	}
	return false;
}

size_t PauseResumeManager_do_resume(
    PauseResumeManager *resumer, size_t pos, short (* const out)[NUM_CHANNELS], size_t const out_len, bool * const resume_finished)
{
	//Current algorithm:
	// for each channel
	//  mute until zero crossing found
	//  Search for zero crossing
	//
	//  Once unmuted, set volume smoothly to 100% (if it is not already at 100%)
	// (Frames beyond the start/end of the track are taken to contain silence)
	// once all channels are unmuted, the pause is complete, so return.

	for (;pos != out_len && any(resumer->currentTrack->muted, NUM_CHANNELS); ++pos) {
		short frames[NUM_CHANNELS];
		
		sf_count_t frames_read = sf_readf_short(resumer->currentTrack->cache.file, frames, 1);
		if (frames_read == 0) {
			for (int chan = 0; chan != NUM_CHANNELS; ++chan) {
				frames[chan] = 0;
			}
		}
		for (int chan = 0; chan != NUM_CHANNELS; ++chan) {
			if (resumer->currentTrack->muted[chan]) {
				if (
					(resumer->currentTrack->prev_non_negative[chan] && frames[chan] <= 0)
					||(resumer->currentTrack->prev_non_positive[chan] && frames[chan] >= 0))
				{
					resumer->currentTrack->muted[chan] = false;
					out[pos][chan] = frames[chan];
				}
				else {
					out[pos][chan] = 0;
				}
			}
			else {
				out[pos][chan] = frames[chan];
			}
			if (frames_read != 0) {
				resumer->currentTrack->prev_non_negative[chan] = frames[chan] >= 0;
				resumer->currentTrack->prev_non_positive[chan] = frames[chan] <= 0;
			}
		}
	}
	if (!any(resumer->currentTrack->muted, NUM_CHANNELS)) {
		*resume_finished = true;
		//TODO? Other cleanup. The resume is over.
	}
	else {
		*resume_finished = false;
	}
	return pos;
}




bool TrackPlayState_is_null(TrackPlayState *state) {
	return state->track == 0;
}

size_t PlaylistPlayer_do_silence(size_t current_frame, short (* const buf)[NUM_CHANNELS], size_t buf_len)
{
	for (;current_frame != buf_len; ++current_frame) {
		for (size_t chan = 0; chan != NUM_CHANNELS; ++chan) {
			buf[current_frame][chan] = 0;
		}
	}
	return current_frame;
}

size_t PlaylistPlayer_do_play(
    PlaylistPlayer *player, size_t current_frame, short (* const buf)[NUM_CHANNELS], size_t buf_len, bool * const finished_playing)
{
	sf_count_t frames_read = sf_readf_short(player->currentTrack.cache.file,buf[current_frame],buf_len - current_frame);
	current_frame += frames_read;

	if (frames_read != 0) {
		for (size_t chan = 0; chan != NUM_CHANNELS; ++chan) {
			player->currentTrack.prev_non_negative[chan] = buf[current_frame-1][chan] >= 0;
			player->currentTrack.prev_non_positive[chan] = buf[current_frame-1][chan] <= 0;
		}
	}

	*finished_playing = current_frame != buf_len;

	return  current_frame;
}
//UpNext:
// Paused
// PlayPlaylist
void PlaylistPlayer_get_frames(PlaylistPlayer *player, unsigned char *const stream, int const len) {
	short (* const buf)[NUM_CHANNELS] = (short(*)[NUM_CHANNELS])stream;
	size_t current_frame = 0;
	size_t buf_len = len/sizeof (short[NUM_CHANNELS]);

	if (player->playlist.front) {
		
	}

	//TODO -- move this logic somewhere else, so it does not need
	//to be rerun every time get_frames is called.
	//This cannot be done in a totally straightforward way,
	//since currently the logic assumes that it gets called at least every time that
	//the phase or the playlist is changed.
	if (!TrackPlayIdentifier_compatible_with(player->currentTrack.track, &player->playlist.front->track)
		|| player->playlist.dirty) {
		switch (player->state.phase) {
		case PAUSING:
			//Don't need to do anything.
			//If want_silence, the new playlist will be picked up by the
			// silence state that would follow the pause
			//If !want_silence, the new playlist will be picked up
			// as soon as the pause is complete.
		break;
		case RESUMING:
			player->state.phase = PAUSING;
		break;
		case SILENCE:
			destroy_TrackPlayState(&player->currentTrack);
			nullify_TrackPlayState(&player->currentTrack);
			if (player->playlist.front) {
				FileCache track_cache;
				if (player->cachelist.front) {
					FileCacheNode *old_front = player->cachelist.front;
					move_FileCache(&track_cache, &old_front->cache);
					player->cachelist.front = old_front->next;
					if (player->cachelist.front) {
						player->cachelist.front->prev = 0;
					}
					else {
						player->cachelist.back = 0;
					}
					delete_FileCacheNode(old_front);
				}
				else {
					//TODO - warn if track not precached (maybe)
					//TODO - play silence when cache not up-to-date.?
					//TODO - handle construction failure.
					if (create_TrackCache(&track_cache, &player->playlist.front->track.track) != 0) {
						assert(false && "Couldn't open file, todo add support.");
					}
				}
				create_TrackPlayState(&player->currentTrack, &player->playlist.front->track, &track_cache);
				if (!player->state.want_silence) {
					player->state.phase = RESUMING;
				}
				player->playlist.dirty = false;
			}
		break;
		case PLAYING:
			player->state.phase = PAUSING;
		break;
		}
	}
	//Update track-cache-list:
	// TODO
	//foreach (track : queue) {
	//	if (timeToStartOfTrack(track) < precacheTime) {
	//		startCaching(track);
	//		if (currentAudioDeviceNotCompatibleOrRequiredLater()) {
	//			startAudioDevice(track) //TODO: Support multiple audio devices
	//                                  //(needed for variable output formats and
	//                                  // to support switching and/or multicasting output device).
	//		}
	//}
	while (current_frame != buf_len) {
		switch (player->state.phase) {
		case SILENCE:
			current_frame = PlaylistPlayer_do_silence(current_frame, buf, buf_len);
		break;
		case PLAYING:
			assert(!TrackPlayState_is_null(&player->currentTrack));
			if (TrackPlayState_is_null(&player->currentTrack)) {
				player->state.phase = SILENCE;
			}
			else {
				//somewhere inside do_play:
				//if (track_end_not_physical_end and currentTime_near_end) {
				//    pausing = true;
				//    currentTrack.ended; //Something like this?
				//    break;
				//}
				bool track_completed;
				current_frame = PlaylistPlayer_do_play(player, current_frame, buf, buf_len, &track_completed);
				if (track_completed) {
					player->state.phase = PAUSING;
					printf("Finished Track, going next.\n");
					
					//TODO: Maybe something like this:
					//earliest_stop = current_frame
					//target_stop = track_end
				}
			}
		break;
		case PAUSING:{
			assert(!TrackPlayState_is_null(&player->currentTrack));
			bool pause_finished = false;
			current_frame =
				PauseResumeManager_do_pause(&player->play_resume_manager, current_frame, buf, buf_len, &pause_finished);
			
			if (pause_finished) {
				if (player->state.want_silence) {
					player->state.phase = SILENCE;
				}
				else {
					if (TrackPlayIdentifier_compatible_with(
							player->playlist.front?&player->playlist.front->track:0,
							player->currentTrack.track) && !player->playlist.dirty)
					{
						assert(
							player->playlist.front
						&& "currentTrack exists and is compatible with playlist.front, therefore playlist.front must exist");
						TrackPlayIdentifierNode *old_front = player->playlist.front;
						player->playlist.front = old_front->next;
						if (player->playlist.front) {
							player->playlist.front->prev = 0;
						} else {
							player->playlist.back = 0;
						}
						delete_TrackPlayIdentifierNode(old_front);
					}
					destroy_TrackPlayState(&player->currentTrack);
					nullify_TrackPlayState(&player->currentTrack);
					if (player->playlist.front) {
						FileCache track_cache;
						if (player->cachelist.front) {
							FileCacheNode *old_front = player->cachelist.front;
							move_FileCache(&track_cache, &old_front->cache);
							player->cachelist.front = old_front->next;
							if (player->cachelist.front) {
								player->cachelist.front->prev = 0;
							}
							else {
								player->cachelist.back = 0;
							}
							delete_FileCacheNode(old_front);
						}
						else {
							//TODO - warn if track not precached (maybe)
							//TODO - play silence when cache not up-to-date.?
							//TODO - handle construction failure.
							if (create_TrackCache(&track_cache, &player->playlist.front->track.track) != 0) {
								assert(false && "Couldn't open file, todo add support.");
							}
						}
						
						create_TrackPlayState(&player->currentTrack, &player->playlist.front->track, &track_cache);
						player->state.phase = RESUMING;
					}
					else {
						player->state.phase = SILENCE;
					}
				}
				player->playlist.dirty = false;
			}
		}break;
		case RESUMING: {
			assert(!TrackPlayState_is_null(&player->currentTrack));
			bool resume_finished;
			current_frame = PauseResumeManager_do_resume(&player->play_resume_manager, current_frame, buf, buf_len, &resume_finished);
							//It must be ok for do_resume to not be called until
							//resume_finished, and for do_pause to be called instead.
							//do_resume should also work properly when it cuts off the operation of do_pause.
			if (resume_finished) {
				player->state.phase = PLAYING;
			}
		}break;
		}
	}
	#if 0
	if (currentTrack not compatible with playlist.front) {
		if (pausing) {
			//Don't need to do anything.
			//If want_silence, the new playlist will be picked up by the
			// silence state that would follow the pause
			//If !want_silence, the new playlist will be picked up
			// as soon as the pause is complete.
		}
		else if (resuming) {
			//pause again, transition to next track.
			pausing = true;
			pause_reason = want_transition_to_next_track;
			//TODO: some sort of special handling when re-pausing
			//a track that is being resumed
			//This should be handled internally by do_pause and do_resume.
		}
		else if (silence) {
			currentTrack = playlist.front;
		}
		else if (playing_normally) {
			pausing = true;
			//pause_reason = want_transition_to_next_track;
		}
		else {
			assert(false);
		}
	}
	//Update track-cache-list:
	// TODO
	//foreach (track : queue) {
	//	if (timeToStartOfTrack(track) < precacheTime) {
	//		startCaching(track);
	//		if (currentAudioDeviceNotCompatibleOrRequiredLater()) {
	//			startAudioDevice(track) //TODO: Support multiple audio devices
	//                                  //(needed for variable output formats and
	//                                  // to support switching and/or multicasting output device).
	//		}
	//}
	while (not_all_frames_written) {
		if (silence) {
			do_silence();//fill output with 0
		}
		else if (playing_normally) {
			if (currentTrackNull) {
				state = paused;
			}
			else {
				//somewhere inside do_play:
				//if (track_end_not_physical_end and currentTime_near_end) {
				//    pausing = true;
				//    currentTrack.ended; //Something like this?
				//    break;
				//}
				do_play();
				if (track_completed) {
					if (track_end_not_physical_end) {
						pausing = true;
						//earliest_stop = current_frame
						//target_stop = track_end
						//(If this is after the end of the physical track, the logical
						// track is assumed to contain silence once the physical track is ended)
						//latest_stop = track_end + max_allowed_stop_time
					
					}
				}
			}
		}
		else if (pausing) {
			do_pause();//It must be ok for do_pause to not be called until
						//pause_finished, and for do_resume to be called instead.
						//do_pause should also work properly when it cuts off the operation of do_resume.
			if (pause_finished) {
				if (want_silence) {
					state = silence;
				}
				else {
					//If currentTrack is compatible, then the pause
					//must have been because the track was ending.
					//Otherwise the pause was due to
					//(the start of) the playlist changing.
					//TODO, this logic is kind-of duplicate with the compatible
					//check at the top of the function, maybe move it to a central place.
					if (playlist.front compatible with currentTrack){
						//assert(currentTrack.ended); //something like this?
						playlist.pop_front
					}
					currentTrack = playlist.front;
				}
			}
		}
		else if (resuming) {
			//somewhere in do_resume:
			//if (track_end_not_physical_end and currentTime_near_end) {
			//    pausing = true;
			//    pause_reason = want_transition_to_next_track;
			//    break;
			//}
			do_resume(); //It must be ok for do_resume to not be called until
							//resume_finished, and for do_pause to be called instead.
							//do_resume should also work properly when it cuts off the operation of do_pause.
			if (resume_finished) {
				state = playing_normally;
			}
		}
		else {
			assert(false);
		}
	}

	#endif
	#if 0

	//while (i != len) {
	//    stream[i] = input.getNext();
	//}




	assert(false);
	if (player->play_resume_manager.pausing) {
		//Write the 'pausing' data to stream, return the number of frames used and the resulting resume-point.
		//If all frames are used, the process is assumed to not yet be complete, and so require another call
		//to do_pause.
		//
		//There should be a fixed upper bound on the number of frames used to pause.
		//The do_pause could take multiple calls to complete, if (stream,len) does not contain
		//enough space to make the nice sounding pause.
		//
		//Pausing should *never* go over multiple tracks. It is assumed
		//that a physical track always has a nice sounding start/end, and so there is no need
		//to go to the next track to find a nice stop/start point.
		//The exception to this is tracks that have custom start/end points.
		//In this case, the custom start/end point is treated as a pause/resume point,
		//and a nice sounding transition is searched for and used
		//(and so the pause itself still needs no data from the subsequent track).
		//
		//do_pause does not manage the actual track transitions/etc.
		int const used_frames = PlayResumeManager_do_pause(&player->play_resume_manager, stream, len);
		if (used_frames != len) {
			if (player->state.currentTrack_outdated) {
				free_TrackPlayState(player->currentTrack);
				
				player->currentTrack = new_TrackPlayState();
			}
		}
	}
	if (player->play_resume_manager.resuming) {
		PlayResumeManager_do_resume(&player->play_resume_manager, stream, len);
	}
	#endif

}
void PlaylistPlayer_pause(PlaylistPlayer *player) {
	//TODO, maybe abstract this, so the design isn't so fragile. (?)
	player->state.want_silence = true;
	if (player->state.phase != SILENCE) {
		player->state.phase = PAUSING;
	}
}
void PlaylistPlayer_resume(PlaylistPlayer *player) {
	//TODO, maybe abstract this, so the design isn't so fragile. (?)
	player->state.want_silence = false;
	if (player->state.phase != PLAYING && player->playlist.front) {
		player->state.phase = RESUMING;
	}
}

void delete_forwards_FileCacheNode(FileCacheNode *file) {
	while (file) {
		FileCacheNode *next_file = file->next;
		delete_FileCacheNode(file);
		file = next_file;
	}
}

//Takes ownership of all elements of new_playlist. Empties new_playlist.
void PlaylistPlayer_update_playlist(PlaylistPlayer *player, Playlist *new_playlist, CommandType updateOrReplace) {
	assert(updateOrReplace == UpdatePlaylist || updateOrReplace == ReplacePlaylist);
	if (updateOrReplace == ReplacePlaylist) {
		player->playlist.dirty = true;
	}
	//TODO: Add 'match' flag.
	//If 'match' flag true, the first element of the new playlist is meant to match an
	//existing element, so if no match is found, discard the first element of the new playlist
	// (on the assumption that the matched element already completed, and so shouldn't be played again.)
	//Otherwise, don't look for a match, and just discard the entire old playlist
	size_t change_point = 0;
	if (updateOrReplace == UpdatePlaylist) {
		bool match_found = false;
		if (new_playlist->front) {
			GUID *new_start_guid = &new_playlist->front->track.guid;
			size_t i = 0;
			for (TrackPlayIdentifierNode *old_p = player->playlist.front; old_p; old_p = old_p->next, ++i) {
				if (strcmp(new_start_guid->guid_str, old_p->track.guid.guid_str) == 0) {
					change_point = i;
					match_found = true;
					break;
					//update playlist from this point forwards:
					
					//delete cachelist elements that are later on in the cachelist than the change-point:
					
					
					assert(false);
					//TODO Later maybe, if the design changes
					//== Need to 'change' track, if new track is not compatible with the current state of the player ==
					//== Need to change NextTrack, if currently transitioning ==
				}
			}
		}
		else {
			printf("Warning: update_playlist called with no inital element, so nothing to match up to");
		}
		if (updateOrReplace == UpdatePlaylist && !match_found) {
			//new_playlist.pop_front()
			if (new_playlist->front) {
				TrackPlayIdentifierNode *prev_front = new_playlist->front;
				new_playlist->front = prev_front->next;
				if (prev_front->next) {
					prev_front->next->prev = 0;
				}
				else {
					new_playlist->back = 0;
				}
				delete_TrackPlayIdentifierNode(prev_front);
			}
		}
	}
	TrackPlayIdentifierNode *playlist_replace_point = player->playlist.front;
	FileCacheNode *cache_deletion_point = player->cachelist.front;
	for (size_t i = 0; i != change_point; ++i) {
		assert(playlist_replace_point && "change_point is either an element of the playlist (if it came from the above loop), or it 0. If it is 0, then this loop will never be entered, and if it is non-zero, then this loop will end as soon as change_point is reached. Therefore, we can't get to null playlist_replace_point inside this loop");
		
		playlist_replace_point = playlist_replace_point->next;
		if (cache_deletion_point) {
			cache_deletion_point = cache_deletion_point->next;
		}
	}

	if (playlist_replace_point) {
		if (playlist_replace_point->prev) {
			playlist_replace_point->prev->next = new_playlist->front;
			if (playlist_replace_point->prev->next) {
				playlist_replace_point->prev->next->prev = playlist_replace_point->prev;
			}
		}
		else {
			player->playlist.front = new_playlist->front;
			if (player->playlist.front) {
				player->playlist.front->prev = 0;
			}
		}
		player->playlist.back = new_playlist->back;
		delete_forwards_TrackPlayIdentifierNode(playlist_replace_point);
	}
	else {
		player->playlist.front = new_playlist->front;
		player->playlist.back = new_playlist->back;
	}

	if (cache_deletion_point) {
		if (cache_deletion_point->prev) {
			cache_deletion_point->prev->next = 0;
			player->cachelist.back = cache_deletion_point->prev;
		}
		else {
			player->cachelist.back = 0;
			player->cachelist.front = 0;
		}
		delete_forwards_FileCacheNode(cache_deletion_point);
	}

	new_playlist->front = 0;
	new_playlist->back = 0;

	return;




	//Only here if no match found:
	assert(false); // TODO - deallocate old nodes.
	player->playlist = *new_playlist;
	//== Need to change track ==
	if (player->playlist.front == 0) {
			//Nothing to do? Both playlists empty.
	}
	else {
		//Delete the old playlist. New playlist empty:
		player->playlist.front = 0;
		player->playlist.back = 0;
		assert(false && "Need to deallocate orphaned nodes!");//TODO
		//== Need to stop music ==
	}


	#if 0
	//Change old to new, starting from the element in old which matches the first element of new
	//If no match is found, replace old with new entirely.

	//This is the desired behaviour, since the updating of a playlist is done according to the logic
	//of "I like the elements up to element X", I don't like elements "X" and later.

	//Element X itself is a special case. If possible, we would like to continue playing element X
	//uninterrupted; however, in the new playlist it may have some properties changed (e.g., different
	//delay, different start/end time, etc.; not yet implemented...).
	//This means that we may or may not have to stop playing the old element X when transitioning to the new
	//playlist. Currently, if the URL is the same, then element X continues unchanged, otherwise it is stopped
	//and a new track is played instead.
	//^ Is this really a useful feature (i.e. being able to change element X,
	//while still playing it even though it is changed, when possible)??.
	//I can't think of any time when it will be useful ):

	//If the above feature is not needed, then we could get rid of the GUID system altogether,
	//and instead have a range of integers assigned to each playlist by the player, and then the new playlist
	//would just have a single 'replacement point' parameter.
	//Even if the above feature *is* needed, the GUID system could be replaced, but it would need
	//special handling of the above situation (e.g. a special command format for 'keep playing track X if possible')

	//find first match in old playlist, change old to new from that point forward.
	//If no match is found, use new playlist entirely.
	//Do the 'right thing' if something is currently playing.
	// Note whether the current track must change.

	//Remove caches and audio devices for things no longer in the queue.

	if (new_playlist->front) {
		GUID *new_start_guid = &new_playlist->front->track.guid;
		for (TrackPlayIdentifierNode *old_p = player->playlist.front; old_p; old_p = old_p->next) {
			if (strcmp(new_start_guid->guid_str, old_p->track.guid.guid_str) == 0) {
				//TODO - update list from this point forwards.
				assert(false);
				//== Need to 'change' track, if new track is not compatible with the current state of the player ==
				//== Need to change NextTrack, if currently transitioning ==
				return;
			}
		}
	}
	//Only here if no match found:
	assert(false); // TODO - deallocate old nodes.
	player->playlist = *new_playlist;
	//== Need to change track ==
	if (player->playlist.front == 0) {
			//Nothing to do? Both playlists empty.
	}
	else {
		//Delete the old playlist. New playlist empty:
		player->playlist.front = 0;
		player->playlist.back = 0;
		assert(false && "Need to deallocate orphaned nodes!");//TODO
		//== Need to stop music ==
	}
	#endif

}

enum AudioState {
	PAUSED,
	WANT_PAUSE,
	WANT_RESUME,
	RUNNING
};

struct AudioCallbackData {
	CommandQueue commandQueue;
	PlaylistPlayer player;
	struct Command *halt_command;
};
#if 0
//Returns the first frame in buf
static sf_count_t find_zero_crossing(int *buf, sf_count_t buf_frames, int channels, int channel, bool prev_positive, bool prev_negative) {
    for (int i = 0; i < buf_frames; ++i) {
        int index = i*channels + channel;
        if ((prev_negative && buf[index] >= 0) || (prev_positive && buf[index] <= 0)) {
            return i;
        }
        prev_positive = buf[index] >= 0;
        prev_negative = buf[index] <= 0;
    }
    return buf_frames;
}
#endif




//Need to never make a sudden jump in audio volume.
//Such jumps probably damage the speakers, and do not sound good.
//A second requirement is that whatever technique we use to avoid sudden jumps
//should sound good.

//When pausing:
//Must ensure that the signal is stopped in <= some small maximum time
//(so that the sound can be shut down in the case of a bad output signal)
//Find zero crossings for each track
//Mute track from zero crossing forward
//Save resume point as latest position in track

//When resuming:
//Seek through the track from the resume point for zero crossings.
//When a zero crossing is detected on a channel, unmute that channel
//(the channel that was last to pause should always immediately unpause).

//Alternatives to zero-crossings search:
//Zero-crossings search
// + Does not alter audio at all
// + Already implemented
// - Different channels mute at different times
// - For 'bad' input, could never terminate;
//   so a backup is needed anyway.
//Linear amplitude attenuation adjustment (aka: automatically turning the volume knob 100%->0% or 0%->100%).
// + Fixed time-delay
// + Simple to implement and to analyse
// - Possible audio artefacts at frequency of the interpolation
//Sinusoidal interpolation from sample->0 or 0->sample
//(sinc interpolation?) (Whittaker–Shannon interpolation?)
// + Possibly will sound the best.
// + Fixed upper bound on time taken
// - 'Invents' signal
// - Not sure what algorithm to use
static void AudioCallback(void *const userdata,
                          unsigned char *const stream,
                          int    const len)
{
	//printf("StreamLen: %zu, %d\n", len/(sizeof(int[NUM_CHANNELS])), rand());
	struct AudioCallbackData *const ud = userdata;
	struct Command *command;
	while (!ud->halt_command && (command = dequeue_command(&ud->commandQueue))) {
		printf("Dequeued Command\n");
		switch(command->type){
		case UpdatePlaylist:{
			char const outstr[] = "Updating Playlist\n";
			write(outputfd, outstr, sizeof outstr -1);
			PlaylistPlayer_update_playlist(&ud->player, &command->data.new_playlist, command->type);
		}break;
		case ReplacePlaylist:{
			char const outstr[] = "Replacing Playlist\n";
			write(outputfd, outstr, sizeof outstr -1);
			PlaylistPlayer_update_playlist(&ud->player, &command->data.new_playlist, command->type);
		}break;
		case Pause:{
			char const outstr[] = "Pausing\n";
			write(outputfd, outstr, sizeof outstr -1);
			PlaylistPlayer_pause(&ud->player);
		}break;
		case Resume:{
			char const outstr[] = "Resuming\n";
			write(outputfd, outstr, sizeof outstr -1);
			PlaylistPlayer_resume(&ud->player);
		}break;
		case Halt:{
			char const outstr[] = "Halting\n";
			write(outputfd, outstr, sizeof outstr -1);
			ud->halt_command = command;
			command = 0;
			PlaylistPlayer_pause(&ud->player);
		}break;
		//case SeekTrack:
		//break;
		//case CetCurrentState:
		//break;
		}
		free_command(command);
	}
	if (ud->halt_command && ud->player.state.phase == SILENCE) {
		//SDL_LockMutex(ud->halt_command->data.done.mutex);//TODO re-add
		ud->halt_command->data.done.done = true;
		//SDL_CondSignal(ud->halt_command->data.done.cond);
		//SDL_UnlockMutex(ud->halt_command->data.done.mutex);
	}
	PlaylistPlayer_get_frames(&ud->player, stream, len);
	#if 0
	foreach (track : queue) {
		if (timeToStartOfTrack(track) < precacheTime) {
			startCaching(track);
			if (currentAudioDeviceNotCompatibleOrRequiredLater()) {
				startAudioDevice(track)
			}
		}
	}

	while (trackQueueNotEmpty && outBufferNotFull) {
		if (got_to_end_of_track()) {
			if (this_device_not_used_later_in_queue) {
				stop_this_device(); //Obviously only stop after output has flushed to device.
			}
		}
		if (starting_new_track_on_new_device()) {
			wait_for_notification_that_old_track_completed();
		}
		playCurrentTrack;
	}

	#endif
}

//Challenges:
// Overlap-free (and small-gap) playback when changing sampling rate (or even when changing sample format etc).

//Data:
// syncid: GUID
// trackid: URL, Start-Time, End-Time, Effects
// Output Control:
//  CurrentState
//  InputReaderUseHandle
//  


//Interfaces:
// Controller:
//  Reads control signals.
//  Sends Cache hints to Input cacher
//  Forwards control signals to Output control
//  Requests preparations for files at appropriate times
// Control Signals:
//  Playlist Control:
//   UpdateQueue(List<(syncid, trackid)>)
//   SeekTrack(syncid, time)
//   PausePlayback
//   GetCurrentState -> syncid, trackid, time
//  Effects Control:
//   TODO
//  Output Selection:
//   TODO
// Input reading and caching
//  Control:
//   HintCacheFile
//   DropCache
//  Usage:
//   ReadMetadata(trackid)
//   ReadData(trackid, starttime)
// Output control
//  Same Control Signals as above, but reads data from Input cache
//  PrepareForFile(trackid)
// ==== Maybe ====
// Output sink:
//  OpenDevice(samplerate) -> deviceid
//  EnqueueData(deviceid, data)
//  GetQueueSize(deviceid)
//  GetMaxQueueSize(deviceid)
//  UpdateQueuedData(deviceid, StartPoint, Callback(QueueData))


void add_track(Playlist *playlist, TrackPlayIdentifierNode *track) {
	if (playlist->back) {
		playlist->back->next = track;
		track->prev = playlist->back;
		playlist->back = track;
	}
	else {
		playlist->front = track;
		playlist->back = track;
	}
}



#if 0
struct AudioDeviceManager {
	Vector<AudioDevice>;
};
#endif


void file_loader() {
	//TODO

}


void delete_forwards_Command(struct Command *command) {
	while (command) {
		struct Command *next_command = command->next;
		free_command(command);
		command = next_command;
	}
}

int init_command_queue(CommandQueue *commandQueue) {
	//SDL_mutex *mutex = SDL_CreateMutex();//TODO re-add
	//if (!mutex) return 1;
	//commandQueue->mutex = mutex;
	commandQueue->front = 0;
	commandQueue->back = 0;
	return 0;
}

void free_command_queue(CommandQueue *commandQueue) {
	if (commandQueue->front) {
		delete_forwards_Command(commandQueue->front);
	}
}

//Control Abilities:
// Play a sequence of tracks without gaps
// Change the sequence of tracks while the system is running
//  (without interrupting the currently playing track, if the user desires)
// Skip the current track (without pops (or gaps?))
// Seek in the current track (without pops (or gaps?))
// Pause
// Play
// Get the current state of the player.
// TODO:
//  Multiple outputs
//  Effects/Volume control

//Implementation of above controls:
// UpdateTrackQueue(TrackQueue<TrackQueueItem> newQueue)
//  TackQueueItem contains the following:
//   UUID
//   Track Identifier
//    (Type of input (file/stream/etc),
//     location of input(path/url),
//     modifications to input (start/end point, volume, effects, etc))
//   Track Skip Distance
//
//  UpdateTrackQueue finds the first element of the old queue
//  that has a UUID matching an element of newQueue, and updates
//  old queue from that point forward.
//
//  If the currently playing track was part of the update and has the same UUID
//  in the old and new queue, it continues to play without change.
//
//Usage:
//
//Risks:
// UUID conflict:
//  Probability: Exceedingly rare
//  Behaviour: Makes the old queue get updated in a strange way
//  Assessment: This problem will not cause database corruption,
//   and is exceedingly rare. The chance of a conflict here is even
//   lower than usual with a UUID, since the queues would never have more than ~100 elements.
//   If the problem does occur and mmm_output starts acting very weirdly, we can just restart it.
//   Furthermore, it is likely that the problem would just cause temporary weirdness which would resolve itself.
//  Response: Keep logs that would allow this problem to be identified in a post-mortem. Otherwise don't worry.
//  Perhaps consider using sequential numbers (and a number-reset protocol) rather than random numbers in
//  order to guarantee an absence of conflicts.
//  Alternatively, have mmm_output assign ids to elements of the queue,
//  and report them back to the clients, rather than the clients
//  producing the ids. This is what mpd does, for example. (This is probably a better idea)
//
// Track unable to be opened/seems corrupted
//  Probability: Rare in everyday use. Very likely to happen eventually.
//  Possible Causes:
//   Track concurrently modified
//   Random file corruption
//   Unknown file type
//   Network error
//  Mitigation Strategies:
//   Some sort of file locking to avoid concurrent modifications
//   In the case of file inaccessibility/corruption/unknown type
//    the trackid knows how long the track is, so play silence for
//    the length of the track, and report the error (when quereied for current state).
//    Also, retry at reasonable intervals, in case the network comes back up/whatever.
//
// Internal programming bug.
//  Probability: Very likely.
//  Effects:
//   In theory anything could happen...
//   In practice:
//    Crash
//    Sending bad output to speakers
//    Doing the wrong thing at the wrong time
//    Becoming unresponsive.
//  Possible Mitigation Strategies:
//   Run the program in a sandbox (chroot, limited hardware access, virtualisation, OS container, etc)
//   Selection of appropriate programming language/technique
//   Tests
//   Lint/type checker/static analysis
//   Line-by-line code review.
//   Explicit output checking code ('speaker protector')
//   Heartbeat monitor with kill/restart
//   High quality reporting on bad internal states. (debugger, logging)
//
// Malicious/Careless user.
//  Probability: Unlikely. Possibly caused by
//   carelessness, or by intrusion by an attacker.
//  Effects:
//   Similar to internal programming bug, since a malicious user
//   could deliberately trigger bugs.
//  Mitigation Strategies:
//   Same as Internal Programming bug
//   Add security controls (we probably won't do this, as we don't have any projected threats in the current setup)
//   Make the API hard to misuse.

bool has_terminating_newline(char const *line, size_t len) {
	return len != 0 && line[len-1] == '\n';
}
/*
struct InputCallbackData {
	struct AudioCallbackData *audioCallback;
	struct ParseState parseState;
};*/
#if 0
void handle_input(size_t in_line_len, char const *in_line, void *ud) {
	struct InputCallbackData *data = ud;
	//getline(&in_line,&in_line_len,stdin);// fgetln(stdin, &in_line_len);//TODO re-add
	if (has_terminating_newline(in_line, in_line_len)) {
		--in_line_len;
	}
	Command *command = parse_command(in_line, in_line_len);
	if (!command) {
		fprintf(stderr, "Invalid command: ");
		//sizeof(size_t)*CHAR_BIT gives number of bits,
		//which is strictly less than number of decimal
		//digits for positive numbers of bits. +4 because of %, s, \n and \0
		//Strictly speaking this should be ceil(sizeof(size_t)*CHAR_BIT*log_10(2))+4,
		//but the C preprocesor can't do floating point arithmetic.
		char format_str[sizeof (size_t) * CHAR_BIT + 4];
		sprintf(format_str, "%%.%zus\n", in_line_len);
		fprintf(stderr, format_str, in_line);
		return;
		//continue;//TODO re-add
	}
	switch (command->type) {
		#if 0 //TODO
		case GetCurrentState:
			PlayState state = get_current_state();
			char const *state_string = serialise_PlayState(&state);
			puts(state_string);
			free(state_string);
			free_command(command);
		break;
		#endif
		default:
		if (queue_command(&data->audioCallback->commandQueue, command) != 0) {
			free_command(command);
		}
		break;
	}
}
#endif
#endif


static int set_hwparams(
	snd_pcm_t *handle,
	snd_pcm_hw_params_t *params,
	snd_pcm_access_t access)
{
	unsigned int rrate = 0;
	snd_pcm_uframes_t size = 0;
	int err = 0, dir = 0;
	/* choose all parameters */
	err = snd_pcm_hw_params_any(handle, params);
	if (err < 0) {
		printf("Broken configuration for playback: no configurations available: %s\n", snd_strerror(err));
		return err;
	}
	/* set hardware resampling */
	err = snd_pcm_hw_params_set_rate_resample(handle, params, resample);
	if (err < 0) {
		printf("Resampling setup failed for playback: %s\n", snd_strerror(err));
		return err;
	}
	/* set the interleaved read/write format */
	err = snd_pcm_hw_params_set_access(handle, params, access);
	if (err < 0) {
		printf("Access type not available for playback: %s\n", snd_strerror(err));
		return err;
	}
	/* set the sample format */
	err = snd_pcm_hw_params_set_format(handle, params, format);
	if (err < 0) {
		printf("Sample format not available for playback: %s\n", snd_strerror(err));
		return err;
	}
	/* set the count of channels */
	err = snd_pcm_hw_params_set_channels(handle, params, channels);
	if (err < 0) {
		printf("Channels count (%i) not available for playbacks: %s\n", channels, snd_strerror(err));
		return err;
	}
	/* set the stream rate */
	rrate = rate;
	err = snd_pcm_hw_params_set_rate_near(handle, params, &rrate, 0);
	if (err < 0) {
		printf("Rate %iHz not available for playback: %s\n", rate, snd_strerror(err));
		return err;
	}
	if (rrate != rate) {
		printf("Rate doesn't match (requested %iHz, get %iHz)\n", rate, err);
		return -EINVAL;
	}
	/* set the buffer time */
	err = snd_pcm_hw_params_set_buffer_time_near(handle, params, &buffer_time, &dir);
	if (err < 0) {
		printf("Unable to set buffer time %i for playback: %s\n", buffer_time, snd_strerror(err));
		return err;
	}
	err = snd_pcm_hw_params_get_buffer_size(params, &size);
	if (err < 0) {
		printf("Unable to get buffer size for playback: %s\n", snd_strerror(err));
		return err;
	}
	buffer_size = size;
	/* set the period time */
	err = snd_pcm_hw_params_set_period_time_near(handle, params, &period_time, &dir);
	if (err < 0) {
		printf("Unable to set period time %i for playback: %s\n", period_time, snd_strerror(err));
		return err;
	}
	err = snd_pcm_hw_params_get_period_size(params, &size, &dir);
	if (err < 0) {
		printf("Unable to get period size for playback: %s\n", snd_strerror(err));
		return err;
	}
	period_size = size;
	/* write the parameters to device */
	err = snd_pcm_hw_params(handle, params);
	if (err < 0) {
		printf("Unable to set hw params for playback: %s\n", snd_strerror(err));
		return err;
	}
	return 0;
}
static int set_swparams(snd_pcm_t *handle, snd_pcm_sw_params_t *swparams)
{
	int err;
	/* get the current swparams */
	err = snd_pcm_sw_params_current(handle, swparams);
	if (err < 0) {
		printf("Unable to determine current swparams for playback: %s\n", snd_strerror(err));
		return err;
	}
	/* start the transfer when the buffer is almost full: */
	/* (buffer_size / avail_min) * avail_min */
	err = snd_pcm_sw_params_set_start_threshold(handle, swparams, (buffer_size / period_size) * period_size);
	if (err < 0) {
		printf("Unable to set start threshold mode for playback: %s\n", snd_strerror(err));
		return err;
	}
	/* allow the transfer when at least period_size samples can be processed */
	/* or disable this mechanism when period event is enabled (aka interrupt like style processing) */
	err = snd_pcm_sw_params_set_avail_min(handle, swparams, period_event ? buffer_size : period_size);
	if (err < 0) {
		printf("Unable to set avail min for playback: %s\n", snd_strerror(err));
		return err;
	}
	/* enable period events when requested */
	if (period_event) {
		err = snd_pcm_sw_params_set_period_event(handle, swparams, 1);
		if (err < 0) {
			printf("Unable to set period event: %s\n", snd_strerror(err));
			return err;
		}
	}
	/* write the parameters to the playback device */
	err = snd_pcm_sw_params(handle, swparams);
	if (err < 0) {
		printf("Unable to set sw params for playback: %s\n", snd_strerror(err));
		return err;
	}
	return 0;
}
/*
 *   Underrun and suspend recovery
 */
static int xrun_recovery(snd_pcm_t *handle, int err)
{
	if (verbose)
		printf("stream recovery\n");
	if (err == -EPIPE) {    /* under-run */
		err = snd_pcm_prepare(handle);
		if (err < 0)
			printf("Can't recovery from underrun, prepare failed: %s\n", snd_strerror(err));
		return 0;
	} else if (err == -ESTRPIPE) {
		while ((err = snd_pcm_resume(handle)) == -EAGAIN)
			sleep(1);   /* wait until the suspend flag is released */
		if (err < 0) {
			err = snd_pcm_prepare(handle);
			if (err < 0)
				printf("Can't recovery from suspend, prepare failed: %s\n", snd_strerror(err));
		}
		return 0;
	}
	return err;
}
struct InputReadData {
	//void *ud;
	//command_read_callback command_callback; //= command_was_read
	struct CommandParser *command_parser;
};
void inputReadCallback(char const *in_buf, size_t in_buf_size, void *ud) {
	struct InputReadData *userData = ud;
	if (CommandParser_execute(userData->command_parser, in_buf, in_buf_size/*, userData->command_callback, userData->ud*/) != 0) {
		assert(false && "Error parsing input");//TODO
	}
}

struct StdInManager {
	void *ud; /*= struct InputReadData {
		void *ud;
		command_read_callback command_callback;
		CommandParser *command_parser;
	};*/
	void (*readCallback)(char const *in_buf, size_t in_buf_size, void *ud); //= inputReadCallback

/*
	void *ud;// = (struct InputCallbackData){.audioCallback = &callbackData};
	void (*inputCallback)(size_t in_line_size, char const *in_line, void *ud);
	size_t input_line_capacity;// = 1;
	size_t input_line_size;// = 0;
	char *input_line;/// = malloc(input_line_capacity);
*/
	int inputfd;
};

void StdInManager_read(struct StdInManager *manager) {
	char input_data[256];
	ssize_t bytes_read = read(manager->inputfd, input_data, sizeof input_data);
	if (bytes_read == -1) {
		printf("Error reading from stdin\n");
		assert(false && "Error reading from stdin\n");
		exit(1);//TODO report error to highter layer.
	}
	manager->readCallback(input_data, (size_t)bytes_read, manager->ud);
#if 0
	if (manager->input_line_capacity == manager->input_line_size) {
		manager->input_line = realloc(manager->input_line, manager->input_line_capacity*2);
		if (!manager->input_line) {
			printf("Couldn't allocate memory for input line");
			exit(1);
		}
		manager->input_line_capacity = manager->input_line_capacity*2;
	}
	ssize_t bytes_read = read(manager->inputfd, &manager->input_line[manager->input_line_size], manager->input_line_capacity - manager->input_line_size);
	if (bytes_read == -1) {
		printf("Error reading from stdin");
		exit(1);
	}
	bool foundNewLine = false;
	for (ssize_t i = 0; i < bytes_read; ++i) {
		if (manager->input_line[manager->input_line_size + i] == '\n') {
			foundNewLine = true;
			manager->inputCallback(manager->input_line_size+i, manager->input_line, manager->ud);
			memcpy(&manager->input_line[0], &manager->input_line[manager->input_line_size + i + 1], bytes_read - i - 1);
			manager->input_line_size = bytes_read - i - 1;
			break;
		}
	}
	if (!foundNewLine) {
		manager->input_line_size += bytes_read;
	}
#endif
}
#if 0
void StdInManager_init(struct StdInManager *manager, void (*inputCallback)(size_t in_line_size, char const *in_line, void *ud), void *ud) {
	manager->ud = ud;
	manager->inputCallback = inputCallback;
	manager->input_line_capacity = 1;
	manager->input_line_size = 0;
	manager->input_line = malloc(manager->input_line_capacity);
	if (!manager->input_line) {
		fprintf(stderr, "Couldn't allocate memory for StdInManager\n");
		exit(-1);
	}
	manager->inputfd = inputfd;
}
#endif

void StdInManager_init(struct StdInManager *manager, void (*readCallback)(char const *in_buf, size_t in_buf_size, void *ud), void *ud) {
	manager->ud = ud;
	manager->readCallback = readCallback;
	manager->inputfd = inputfd;
}

void StdInManager_fill_pollfd(struct StdInManager *manager, struct pollfd *ufd) {
	ufd->fd = manager->inputfd;
	ufd->events = POLLIN;//= (struct pollfd){.fd = manager->stdinfd, .events = POLLIN, .revents = 0};
}
#if 0
struct SocketInManager {
	void *ud;
	void (*inputCallback)
};
#endif
struct CommandReadData {
	struct AudioCallbackData *audioCallback;
	//OutputBuffer outBuffer;
};
int command_was_read(struct Command *command, void *ud) {
	//Push command into command queue
	//Or error?
	struct CommandReadData *data = ud;
	if (!command) {
		fprintf(stderr, "Command not understood\n");
		//OutputBuffer_appendLine(&data->outBuffer, "Command not understood");
		return 0;
#if 0 //Need some sort of error recovery.
		//sizeof(size_t)*CHAR_BIT gives number of bits,
		//which is strictly less than number of decimal
		//digits for positive numbers of bits. +4 because of %, s, \n and \0
		//Strictly speaking this should be ceil(sizeof(size_t)*CHAR_BIT*log_10(2))+4,
		//but the C preprocesor can't do floating point arithmetic.
		char format_str[sizeof (size_t) * CHAR_BIT + 4];
		sprintf(format_str, "%%.%zus\n", in_line_len);
		fprintf(stderr, format_str, in_line);
		return;
		//continue;//TODO re-add
#endif
		//assert(false && "Command not read correctly");
		//exit(-1);
	}
	switch (command->type) {
		#if 0 //TODO
		case GetCurrentState:
			PlayState state = get_current_state();
			char const *state_string = serialise_PlayState(&state);
			puts(state_string); //need access to i/o file descriptor.
			                    //should defer output until it is ready to read.
			free(state_string);
			free_command(command);
		break;
		#endif
		default:
		if (queue_command(&data->audioCallback->commandQueue, command) != 0) {
			free_command(command);
			return -1;
		}
		break;
	}
	return 0;
}

static int poll_and_dispatch_events(struct pollfd *ufds, int num_pcm_descriptors, struct StdInManager *stdin_manager, bool *init, snd_pcm_t *handle) {
	int err = 0;
	unsigned short revents = 0;
	bool gotEvent = false;
	while (!gotEvent) {
		poll(ufds, num_pcm_descriptors+1, -1);
		snd_pcm_poll_descriptors_revents(handle, ufds, num_pcm_descriptors, &revents);
		if (revents & POLLERR) {
			err = -EIO;
			if (snd_pcm_state(handle) == SND_PCM_STATE_XRUN ||
				snd_pcm_state(handle) == SND_PCM_STATE_SUSPENDED) {
				err = snd_pcm_state(handle) == SND_PCM_STATE_XRUN ? -EPIPE : -ESTRPIPE;
				if (xrun_recovery(handle, err) < 0) {
					printf("Write error: %s\n", snd_strerror(err));
					exit(EXIT_FAILURE);
				}
				*init = 1;
			} else {
				printf("Wait for poll failed\n");
				return err;
			}
			gotEvent = true;
		}
		if (revents & POLLOUT) {
			err = 0;
			gotEvent = true;
		}
		if (ufds[num_pcm_descriptors].revents & POLLERR) {
			fprintf(stderr, "Error reading stdin\n");
			exit(-1);
		}
		if (ufds[num_pcm_descriptors].revents & POLLIN) {
			StdInManager_read(stdin_manager);
			//gotEvent = true;
		}
		/*
		 * for (int i = 0; i < num_in_connections; ++i) {
		 *     if(ufds[first_in_connection+i].revents & POLLIN) {
		 *         Connection_read(&connections[i]);
		 *     }
		 * }
		 * 
		 */
	}
	return err;
}

static int write_and_poll_loop(snd_pcm_t *handle)
{
	//TODO: Design+Implement error recovery scheme (especially for all the callbacks).
	//TODO; Perhaps make the halt-command separate? (Rationale:
	// - Normal operation shouldn't be able to cause the system to quit, this should be a separate channel.
	// - When a halt command is received, the system shuts down and everything stops (and in particular, future commands are ignored),
	//   this is a fundamentally different behaviour from all the other commands).
	struct AudioCallbackData callbackData;
	init_command_queue(&callbackData.commandQueue);
	init_PlaylistPlayer(&callbackData.player);
	callbackData.halt_command = 0;
	
	struct CommandReadData commandReadData = (struct CommandReadData){
		.audioCallback = &callbackData
	};
	
	struct CommandParser *parser = new_CommandParser(command_was_read, &commandReadData);
	if (!parser) {
		printf("Couldn't allocate command parser");//TODO Proper error handling
		exit(-1);
	}
	
	//struct InputCallbackData inputCallback = (struct InputCallbackData){.audioCallback = &callbackData};
	struct InputReadData inputReadData = (struct InputReadData){
		//.ud = &commandReadData,
		//.command_callback = command_was_read, //These are internally referenced by the CommandParser,
		//                                      //so are unneeded for now, but when we support arbitrary numbers of connections
		//                                      //we will need to *allocate/deallocate* a CommandReadData and CommandParser per connection.
		.command_parser = parser,
	};	/*void *ud;
	command_read_callback command_callback;
	CommandParser *command_parser;*/
	//assert(false);//TODO
	
	//
	//Produce fd-readers+parsers for each incoming connection
	// Read data in via fd-reader (StdInManager)
	//  Each read produces callback into Parser.
	//   Each command produces callback into command_read

	//
	//data {
	//  Socket data {
	//    Socket fd
	//    Connection fd list? (or something like that)
	//  }
	//  AudioCommandQueue (Command queue && Halt-command)
	//  fd_reader_callback (calls parser)
	//  Array of: (One-per-fd) {
	//    fd_read_data {
	//      ParserData
	//      Command Read callback
	//      AudioCommandQueue (pointer)
	//    }
	//  }
	//  //TODO: Error data?
	//}

	//struct InConnectionManager in_connection_manager;
	//InConnectionManager_init(&in_connection_manager, socketfd);//TODO
	
	//int in_connection_poll_descriptors = InConnectionManager_poll_descriptors_count(&in_connection_manager);
	
	struct StdInManager stdin_manager;
	StdInManager_init(&stdin_manager, &inputReadCallback, &inputReadData);

	struct pollfd *ufds;
	signed short *ptr;
	int err, num_pcm_descriptors, cptr;
	bool init = false;
	num_pcm_descriptors = snd_pcm_poll_descriptors_count(handle);
	if (num_pcm_descriptors <= 0) {
		printf("Invalid snd_pcm_poll_descriptors_count\n");
		return num_pcm_descriptors;
	}
	//int num_poll_descriptors = num_pcm_descriptors+1; //pcm+socket.
	                                    //As connections occur on the socket, more
	                                    //file descriptors will be added to the list.
	ufds = malloc(sizeof(struct pollfd) * (num_pcm_descriptors+1));
	if (ufds == NULL) {
		printf("No enough memory\n");
		return -ENOMEM;
	}
	if ((err = snd_pcm_poll_descriptors(handle, ufds, num_pcm_descriptors)) < 0) {
		printf("Unable to obtain poll descriptors for playback: %s\n", snd_strerror(err));
		return err;
	}
	StdInManager_fill_pollfd(&stdin_manager, &ufds[num_pcm_descriptors]);
	init = 1;
	while (!(callbackData.halt_command && callbackData.halt_command->data.done.done)) {
		if (!init) {
			poll_and_dispatch_events(ufds, num_pcm_descriptors, &stdin_manager, &init, handle);
#if 0
			{
				unsigned short revents;
				bool gotEvent = false;
				while (!gotEvent) {
					poll(ufds, num_pcm_descriptors+1, -1);
					snd_pcm_poll_descriptors_revents(handle, ufds, num_pcm_descriptors, &revents);
					if (revents & POLLERR) {
						err = -EIO;
						if (snd_pcm_state(handle) == SND_PCM_STATE_XRUN ||
							snd_pcm_state(handle) == SND_PCM_STATE_SUSPENDED) {
							err = snd_pcm_state(handle) == SND_PCM_STATE_XRUN ? -EPIPE : -ESTRPIPE;
							if (xrun_recovery(handle, err) < 0) {
								printf("Write error: %s\n", snd_strerror(err));
								exit(EXIT_FAILURE);
							}
							init = 1;
						} else {
							printf("Wait for poll failed\n");
							return err;
						}
						gotEvent = true;
					}
					if (revents & POLLOUT) {
						err = 0;
						gotEvent = true;
					}
					if (ufds[num_pcm_descriptors].revents & POLLERR) {
						fprintf(stderr, "Error reading stdin\n");
						exit(-1);
					}
					if (ufds[num_pcm_descriptors].revents & POLLIN) {
						StdInManager_read(&stdin_manager);
						//gotEvent = true;
					}
				}	
			}
#endif
		}
		int avail = snd_pcm_avail_update(handle);
		short * const buf = malloc(avail * channels * snd_pcm_format_physical_width(format)/CHAR_BIT);
		ptr = buf;
		AudioCallback(&callbackData, (unsigned char*)ptr, avail * channels * snd_pcm_format_physical_width(format)/CHAR_BIT);
		//generate_sine(areas, 0, period_size, &phase);
		//ptr = buf;
		cptr = avail;
		while (cptr > 0) {
			err = snd_pcm_writei(handle, ptr, cptr);
			if (err < 0) {
				if (xrun_recovery(handle, err) < 0) {
					printf("Write error: %s\n", snd_strerror(err));
					exit(EXIT_FAILURE);
				}
				init = 1;
				break;  /* skip one period */
			}
			if (snd_pcm_state(handle) == SND_PCM_STATE_RUNNING) init = 0;
			ptr += err * channels;
			cptr -= err;
			if (cptr == 0) break;
			/* it is possible, that the initial buffer cannot store */
			/* all data from the last period, so wait awhile */
			//err = wait_for_poll(handle, ufds, num_pcm_descriptors);
			poll_and_dispatch_events(ufds, num_pcm_descriptors, &stdin_manager, &init, handle);
#if 0
			{
				unsigned short revents;
				bool gotEvent = false;
				while (!gotEvent) {
					poll(ufds, num_pcm_descriptors+1, -1);
					snd_pcm_poll_descriptors_revents(handle, ufds, num_pcm_descriptors, &revents);
					if (revents & POLLERR) {
						err = -EIO;
							if (snd_pcm_state(handle) == SND_PCM_STATE_XRUN ||
							snd_pcm_state(handle) == SND_PCM_STATE_SUSPENDED) {
							err = snd_pcm_state(handle) == SND_PCM_STATE_XRUN ? -EPIPE : -ESTRPIPE;
							if (xrun_recovery(handle, err) < 0) {
								printf("Write error: %s\n", snd_strerror(err));
								exit(EXIT_FAILURE);
							}
							init = 1;
						} else {
							printf("Wait for poll failed\n");
							return err;
						}
						gotEvent = true;
					}
					if (revents & POLLOUT) {
						err = 0;
						gotEvent = true;
					}
					if (ufds[num_pcm_descriptors].revents & POLLERR) {
						fprintf(stderr, "Error reading stdin\n");
						exit(-1);
					}
					if (ufds[num_pcm_descriptors].revents & POLLIN) {
						StdInManager_read(&stdin_manager);
						//gotEvent = true;
					}
				}	
			}
#endif
		}
		free(buf);
	}
	free_command_queue(&callbackData.commandQueue);
	free_PlaylistPlayer(&callbackData.player);
	free_command(callbackData.halt_command);
	free(ufds);
	delete_CommandParser(parser);
	return 0;
}
int do_main(void) {
	int n = sd_listen_fds(0);
	if (n > 1) {
		fprintf(stderr, "Too many file descriptors received.\n");
		return 1;
	}
	else if (n == 1) {
		inputfd = SD_LISTEN_FDS_START + 0;
		outputfd = SD_LISTEN_FDS_START + 0;
	}
	else {
		inputfd = fileno(stdin);
		outputfd = fileno(stdout);
	}

	snd_pcm_t *handle;
	int err;
	snd_pcm_hw_params_t *hwparams;
	snd_pcm_sw_params_t *swparams;
	snd_pcm_hw_params_alloca(&hwparams);
	snd_pcm_sw_params_alloca(&swparams);
	err = snd_output_stdio_attach(&output, stdout, 0);
	if (err < 0) {
		printf("Output failed: %s\n", snd_strerror(err));
		return 0;
	}
	printf("Playback device is %s\n", device);
	printf("Stream parameters are %iHz, %s, %i channels\n", rate, snd_pcm_format_name(format), channels);
	if ((err = snd_pcm_open(&handle, device, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
		printf("Playback open error: %s\n", snd_strerror(err));
		return 0;
	}

	if ((err = set_hwparams(handle, hwparams, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
		printf("Setting of hwparams failed: %s\n", snd_strerror(err));
		exit(EXIT_FAILURE);
	}
	if ((err = set_swparams(handle, swparams)) < 0) {
		printf("Setting of swparams failed: %s\n", snd_strerror(err));
		exit(EXIT_FAILURE);
	}
	//if (verbose > 0)
		snd_pcm_dump(handle, output);
	snd_config_update_free_global();
	err = write_and_poll_loop(handle);
	if (err < 0)
		printf("Transfer failed: %s\n", snd_strerror(err));

	snd_pcm_close(handle);
	return 0;
}
int main(void)
{
	return do_main();
}
