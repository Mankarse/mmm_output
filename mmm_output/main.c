#if 1
//#define _POSIX_C_SOURCE 200809L
#include "main.h"
#include "command_parser.h"
//When compiling with std=c11 on linux,
//ALSA incorrectly redefined timespec and timeval.
//These defines stop <time.h> from defining timespec or timeval,
//and so allow us to use ALSA's definitions
//ALSA also requires alloca for 
#include <alloca.h>
//#define __timespec_defined 1
#include <alsa/asoundlib.h>
//#undef __timespec_defined
//#define _STRUCT_TIMEVAL 1
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

#include <sys/socket.h>
#include<sys/socket.h>
#include<arpa/inet.h>	//inet_addr

static int inputfd = -1;
static int outputfd = -1;
/*
static FILE *input_source = 0;//stdin
static FILE *output_sink = 0;//stdout
static FILE *error_sink = 0;//stderr
static FILE *log_sink = 0;//log file
*/
//Number that is printed and incremented on each write to error_sink or log_sink;
//Used to determine when lines in each file were printed relative to each other.
//static unsigned long long output_count = 0;

//These global constants are initial suggestions for parameters.
//During initialisation, they are automatically updated to actually achieveable values
//by ALSA. The glboal constants are updated during this process.
static char *device = "default";         /* playback device */
static snd_pcm_format_t format = SND_PCM_FORMAT_S16;    /* sample format */
static unsigned int rate = 44100;           /* stream rate */
static unsigned int channels = 2;           /* count of channels */
//These two parameters affect latency, likelihood of underrun and power usage.
//Choose wisely.
static unsigned int buffer_time = 50000;       /* Ring buffer length in us. Determines the latency of the system after data has been transfered. (?)*/
static unsigned int period_time = 50000;       /* Period time in us. Determines how much data is requested at a time. Capped to approx period_time <= buffer_time/3 (?)*/
//static int verbose = 0;                 /* verbose flag */
static unsigned int resample = 1;                /* enable alsa-lib resampling */
static int period_event = 1;                /* produce poll event after each period */ /*TODO: was 0*/
static snd_pcm_sframes_t buffer_size; //Number of frames in buffer_time at the given sample rate
static snd_pcm_sframes_t period_size; //Number of frames in period_time at the given sample rate
//static snd_output_t *output = NULL;

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
// Gracefully handle Hardware disconnection.



static void destroy_GUID(GUID *guid) {
	free(guid->guid_str);
}
static void destroy_Track(Track *track) {
	free(track->url);
}

static void destroy_TrackPlayIdentifier(TrackPlayIdentifier *track) {
	destroy_GUID(&track->guid);
	destroy_Track(&track->track);
}
static void delete_TrackPlayIdentifier(TrackPlayIdentifier *track) {
	destroy_TrackPlayIdentifier(track);
	free(track);
}

static void delete_TrackPlayIdentifierNode(TrackPlayIdentifierNode *node) {
	destroy_TrackPlayIdentifier(&node->track);
	free(node);
}
static void delete_forwards_TrackPlayIdentifierNode(TrackPlayIdentifierNode *track) {
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

static int queue_command(CommandQueue *commandQueue, struct Command* command) {
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

static struct Command *dequeue_command(CommandQueue *commandQueue) {
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

static void init_PlaylistPlayer(PlaylistPlayer *player) {
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


static void destroy_Playlist(Playlist *playlist) {
	//For now, this is more like 'clear_Playlist' function.
	if (playlist->front) {
		playlist->dirty = true;
		delete_forwards_TrackPlayIdentifierNode(playlist->front);
		playlist->front = 0;
		playlist->back = 0;
	}
}
static void destroy_CacheList(FileCacheList *cachelist) {
	delete_forwards_FileCacheNode(cachelist->front);
}

static void free_PlaylistPlayer(PlaylistPlayer *player) {
	//assert(false);
	destroy_Playlist(&player->playlist);
	destroy_CacheList(&player->cachelist);
	destroy_TrackPlayState(&player->currentTrack); 	//TODO? Or these don't actually require any deallocation.
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

static bool TrackPlayIdentifier_compatible_with(TrackPlayIdentifier const *l, TrackPlayIdentifier const *r) {
	if (l == 0 || r == 0) return l == 0 && r == 0;
	return strcmp(l->guid.guid_str, r->guid.guid_str) == 0 && strcmp(l->track.url, r->track.url) == 0;
}

static void destroy_FileCache(FileCache *cache) {
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

static void move_FileCache(FileCache *l, FileCache *r) {
	*l = *r;
	r->file = 0;
}

static void delete_FileCacheNode(FileCacheNode *p) {
	destroy_FileCache(&p->cache);
	destroy_GUID(&p->track_guid);
	free(p);
}
static int create_TrackCache(FileCache *cache, Track *track) {
	SF_INFO file_info;
	memset(&file_info, 0, sizeof file_info);
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

static TrackPlayIdentifier *copy_TrackPlayIdentifier(TrackPlayIdentifier *old) {
	if (!old) return 0;

	TrackPlayIdentifier *ret = malloc(sizeof *ret);
	if (!ret) return 0;
	ret->guid.guid_str = strdup(old->guid.guid_str); //TODO: write our own strdup? strdup not part of C11
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

static void create_TrackPlayState(TrackPlayState *state, TrackPlayIdentifier *track, FileCache *file) {
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

static bool all(bool *arr, size_t const len) {
	for (size_t i = 0; i != len; ++i) {
		if (!arr[i]) return false;
	}
	return true;
}

//TODO some way of signalling an error in do_pause.
static size_t PauseResumeManager_do_pause(
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
static bool any(bool *arr, size_t const len) {
	for (size_t i = 0; i != len; ++i) {
		if (arr[i]) return true;
	}
	return false;
}

static size_t PauseResumeManager_do_resume(
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




static bool TrackPlayState_is_null(TrackPlayState *state) {
	return state->track == 0;
}

static size_t PlaylistPlayer_do_silence(size_t current_frame, short (* const buf)[NUM_CHANNELS], size_t buf_len)
{
	for (;current_frame != buf_len; ++current_frame) {
		for (size_t chan = 0; chan != NUM_CHANNELS; ++chan) {
			buf[current_frame][chan] = 0;
		}
	}
	return current_frame;
}

static size_t PlaylistPlayer_do_play(
    PlaylistPlayer *player, size_t current_frame, short (* const buf)[NUM_CHANNELS], size_t buf_len, bool * const finished_playing)
{
	sf_count_t frames_read = sf_readf_short(player->currentTrack.cache.file,buf[current_frame], (sf_count_t)(buf_len - current_frame));//TODO integer overflow
	current_frame += (size_t)frames_read;

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
static void PlaylistPlayer_get_frames(PlaylistPlayer *player, unsigned char *const stream, int const len) {
	short (* const buf)[NUM_CHANNELS] = (short(*)[NUM_CHANNELS])stream;
	assert(len >= 0);
	size_t current_frame = 0;
	size_t buf_len = ((size_t)len)/sizeof (short[NUM_CHANNELS]);

	if (player->playlist.front) {
		
	}

	//TODO -- move this logic somewhere else, so it does not need
	//to be rerun every time get_frames is called.
	//This cannot be done in a totally straightforward way,
	//since currently the logic assumes that it gets called at least every time that
	//the phase or the playlist is changed.
	if (!TrackPlayIdentifier_compatible_with(player->currentTrack.track, player->playlist.front ? &player->playlist.front->track : 0)
	    || player->playlist.dirty)
	{
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
static void PlaylistPlayer_pause(PlaylistPlayer *player) {
	//TODO, maybe abstract this, so the design isn't so fragile. (?)
	player->state.want_silence = true;
	if (player->state.phase != SILENCE) {
		player->state.phase = PAUSING;
	}
}
static void PlaylistPlayer_resume(PlaylistPlayer *player) {
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
static void PlaylistPlayer_update_playlist(PlaylistPlayer *player, Playlist *new_playlist, CommandType updateOrReplace) {
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

#if 0
staic void file_loader() {
	//TODO

}
#endif

static void delete_forwards_Command(struct Command *command) {
	while (command) {
		struct Command *next_command = command->next;
		free_command(command);
		command = next_command;
	}
}

static int init_command_queue(CommandQueue *commandQueue) {
	//SDL_mutex *mutex = SDL_CreateMutex();//TODO re-add
	//if (!mutex) return 1;
	//commandQueue->mutex = mutex;
	commandQueue->front = 0;
	commandQueue->back = 0;
	return 0;
}

static void free_command_queue(CommandQueue *commandQueue) {
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
#if 0
static bool has_terminating_newline(char const *line, size_t len) {
	return len != 0 && line[len-1] == '\n';
}
#endif
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
	printf("buffer_time: %d\n", buffer_time);
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
	printf("period_time: %d\n", period_time);
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
#if 0
/*
 *   Underrun and suspend recovery
 */
static int xrun_recovery(snd_pcm_t *handle, int err)
{
	//if (verbose)
	//	printf("stream recovery\n");
	if (err == -EPIPE) {    /* under-run */
		err = snd_pcm_prepare(handle);
		if (err < 0) {
			printf("Can't recovery from underrun, prepare failed: %s\n", snd_strerror(err));
		}
		return 0;
	} else if (err == -ESTRPIPE) { /*Suspended (?)*/
		while ((err = snd_pcm_resume(handle)) == -EAGAIN)
			sleep(1);   /* wait until the suspend flag is released *///TODO: Don't block other tasks!!!
		if (err < 0) {
			err = snd_pcm_prepare(handle);
			if (err < 0)
				printf("Can't recovery from suspend, prepare failed: %s\n", snd_strerror(err));
		}
		return 0;
	}
	return err;
}
#endif
#if 0
struct InputReadData {
	//void *ud;
	//command_read_callback command_callback; //= command_was_read
	struct CommandParser *command_parser;
};
static void inputReadCallback(char const *in_buf, size_t in_buf_size, void *ud) {
	struct InputReadData *userData = ud;
	if (CommandParser_execute(userData->command_parser, in_buf, in_buf_size/*, userData->command_callback, userData->ud*/) != 0) {
		assert(false && "Error parsing input");//TODO
	}
}
#endif
#if 0
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

static void StdInManager_read(struct StdInManager *manager) {
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
#endif
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
#if 0
static void StdInManager_init(struct StdInManager *manager, void (*readCallback)(char const *in_buf, size_t in_buf_size, void *ud), void *ud) {
	manager->ud = ud;
	manager->readCallback = readCallback;
	manager->inputfd = inputfd;
}

static void StdInManager_fill_pollfd(struct StdInManager *manager, struct pollfd *ufd) {
	ufd->fd = manager->inputfd;
	ufd->events = POLLIN;//= (struct pollfd){.fd = manager->stdinfd, .events = POLLIN, .revents = 0};
}
#endif
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
static int command_was_read(struct Command *command, void *ud) {
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
#if 0
//Should probably use a more sophisticated scheme for output-rate matching.
//(e.g. have the writer gradually send the data, internally managing how far through the data it is,
// rather than sending the data all at once and using the buffer to perform the rate matching).
struct RingBuffer{
	char *buf;
	size_t len;
	size_t startPos;//Read Point
	size_t endPos;  //Write Point
	
	//To add data, copy into ring if ring size allows, otherwise increase ring size.
	
	int write(char *data, size_t len) {
		memcpy(data, endPos);
		endPos += len;
	}
	
	void get_data() {
		return buf[startPos], startPos - endPos;//Modulo details about ringbuffering
	}
	void data_read(int nChars) {
		startPos+=nChars;//Modulo details about ringbuffering
	}
};
#endif
struct ConnectionData {
	struct CommandParser *parser;
	//bool hasPendingOutput; //Virtual field, obtained from within parserState.(?)
	int connection_fd;
};

static void ConnectionData_free(struct ConnectionData *connection) {
	while (close(connection->connection_fd) == -1) {
		switch (errno) {
			default:
			case EBADFD:
				assert(false && "connection->connection_fd must be valid at this point");
				goto end;
			case EINTR:
				continue;
			case EIO:
				printf("IO error when closing connection->connection_fd\n");
				goto end; //TODO: Figure out how to properly handle this.
		}
	}
	end:;
	delete_CommandParser(connection->parser);
}

struct SocketManager {
	//Socket fd
	//List of Connections {
	//	Parser
	//	OutBuffer?
	//	connection_fd
	//}
	
	int socket_fd;
	
	struct ConnectionData *connections;
	size_t connections_len;
	size_t connections_capacity;
	
	command_read_callback command_read_callback;
	void *command_read_callback_ud;
	//Init
	//Destroy
	
	//HandleSocketRevent
	//GetSocketFD
	//GetConnectionFDs
	//HandleConnectionRevent
};

static int init_CommandServerSocket() {
	//Create and Bind socket.
	//Begin listening later, once we are ready to handle connections.
	int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (socket_fd < 0) return socket_fd;
	struct sockaddr_in server;
	server.sin_family = AF_INET;
	server.sin_addr.s_addr = INADDR_ANY;
	server.sin_port = htons(9898);
	//Bind
	if(bind(socket_fd, (struct sockaddr*)&server , sizeof(server)) < 0)
	{
		//print the error message
		printf("Couldn't bind CommandServerSocket.");
		return -1;
	}
	return socket_fd;
}

//socket_fd must be a streaming socket server.
static int SocketManager_init(struct SocketManager *manager, int socket_fd, command_read_callback command_read_callback, void *command_read_callback_ud) {
	int flags = fcntl(socket_fd, F_GETFL);
	if (flags == -1) return -1;
	int ret = fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK);
	assert(ret == 0 && "fcntl should always return 0 when performing the F_SETFL operation");
	manager->socket_fd = socket_fd;
	
	manager->connections = 0;
	manager->connections_len = 0;
	manager->connections_capacity = 0;
	
	manager->command_read_callback = command_read_callback;
	manager->command_read_callback_ud = command_read_callback_ud;

	return 0;
}

static void SocketManager_free(struct SocketManager *manager) {
	//TODO Destroy each connection, before freeing the array.
	for (size_t i = 0; i != manager->connections_len; ++i) {
		ConnectionData_free(&manager->connections[i]);
	}
	free(manager->connections);
}

static int SocketManager_start(struct SocketManager *manager) {
	int flags = fcntl(manager->socket_fd, F_GETFL);
	if (flags == -1) return flags;
	assert(flags & O_NONBLOCK);
	return listen(manager->socket_fd, 512);
}
static void SocketManager_fill_socket_poll_descriptor(struct SocketManager *manager, struct pollfd *descriptor) {
	assert(manager->socket_fd >= 0);
	descriptor->fd = manager->socket_fd;
	descriptor->events = POLLIN;
}

static size_t SocketManager_getNumConnections(struct SocketManager *manager) {
	return manager->connections_len;
}

static void SocketManager_fill_connections_poll_descriptors(struct SocketManager *manager, struct pollfd *descriptors) {
	for (size_t i = 0; i != manager->connections_len; ++i) {
		descriptors[i] = (struct pollfd) {
			.fd = manager->connections[i].connection_fd,
			.events = POLLIN, //TODO: Handle POLLOUT too when the CommandParser has output to send.
			.revents = 0,
		};
	}
#if 0
	descriptors[0] = (struct pollfd){
		.fd = fileno(stdin),
		.events = POLLIN,
		.revents = 0,
	};
	//Should only listen for POLLOUT if there is buffered output data.
	descriptors[1] = (struct pollfd){
		.fd = fileno(stdout),
		.events = POLLOUT,
		.revents = 0,
	};
#endif
	//assert(false);
}
static int SocketManager_handleSocketRevent(struct SocketManager *manager, int revents) {
	//TODO: Handle connections becoming closed.
	if (revents & POLLERR) {
		assert(false && "input socket got POLLERR");//TODO: fix error handling.
		return -1;
	}
	if (revents & POLLOUT) {
		assert(false &&  "input socket got POLLOUT");//TODO: fix error handling.
		return -1;
	}
	if (revents & POLLIN) {
		int newConnection_fd = accept(manager->socket_fd, 0, 0);
		if (newConnection_fd < 0) {
			printf("Connection Failed\n");
			return -1;
		}
		else {
			printf("Got Connection\n");
			//close(newConnection_fd);
			//push_back connection:
			if (manager->connections_len == manager->connections_capacity) {
				size_t newCapacity = manager->connections_capacity == 0 ? 1 : manager->connections_capacity*2;
				struct ConnectionData *newConnections = realloc(manager->connections, newCapacity*sizeof *newConnections);
				if (!newConnections) {
					printf("Couldn't allocate memory for new connection\n");
					return -1;
				}
				manager->connections = newConnections;
				manager->connections_capacity = newCapacity;
			}
			
			manager->connections[manager->connections_len].connection_fd = newConnection_fd;
			manager->connections[manager->connections_len].parser = new_CommandParser(manager->command_read_callback, manager->command_read_callback_ud);
			if (!manager->connections[manager->connections_len].parser) {
				printf("Couldn't create parser for new connection\n");
				return -1;
			}
			manager->connections_len++;
			printf("New connection_fd == %d\n", manager->connections[manager->connections_len-1].connection_fd);
		}
	}
	return 0;
}
//#if 0
struct pcm_handler_data {
	snd_pcm_t *pcm_handle;
	bool pcm_underrun; //Underrun.
	
	struct AudioCallbackData *audioCallbackData;
	
	char *buf;
	//size_t buf_capacity; //Allocated capacity of buf.
	//size_t buf_size; //Amount of real data in buf.
	//size_t buf_pos; //Current position within buf that we are reading from.
};
static int pcm_handler(unsigned short revents, void *ud) {
	struct pcm_handler_data *userData = ud;
	if (!revents) {return 0;}
	if (revents & POLLERR) {
		printf("pcm_handler POLLERR\n");
		//We've stopped handling Suspended, since we don't know what causes it (and as far as we know, it might never be triggered in our system).
		//We need to find out, and perhaps re-add. See pcm_try_recover.
		if (snd_pcm_state(userData->pcm_handle) == SND_PCM_STATE_XRUN/* || snd_pcm_state(userData->pcm_handle) == SND_PCM_STATE_SUSPENDED*/)
		{
			//TODO: After underrun, perform soft-start again (that is, find zero-crossing in new data, since the output was forced to silence by the underrun).
			userData->pcm_underrun = true;
			return 0;
			/*
			err = snd_pcm_state(userData->pcm_handle) == SND_PCM_STATE_XRUN ? -EPIPE : -ESTRPIPE;
			if (xrun_recovery(userData->pcm_handle, err) < 0) {
				printf("Write error: %s\n", snd_strerror(err));
				exit(EXIT_FAILURE);
			}
			*init = true;*/
		}
		else {
			printf("Wait for poll failed\n");
			return -1;
		}
	}
	else if (revents & POLLIN) {
		assert(false && "PCM incorrectly configured, should be an output PCM but got POLLIN event");
		printf("PCM incorrectly configured\n");
		return -1;
	}
	else if (revents & POLLOUT) {
		//read avail frames into buf:
		snd_pcm_sframes_t avail;
		if ((avail = snd_pcm_avail_update(userData->pcm_handle)) < 0) {
			printf("snd_pcm_avail_update failed!\n");
			userData->pcm_underrun = true;
			return 0;//TODO: Make this negative(?), properly handle when it is
		}
		//TODO: Properly handle possible integer overflow
		size_t buf_len = avail * channels * snd_pcm_format_physical_width(format)/CHAR_BIT;
		void *n_buf = realloc(userData->buf, buf_len);
		if (!n_buf) {
			printf("Couldn't allocate memory\n");
			return -1;
		}
		userData->buf = n_buf;
		//userData->buf_size = avail;
		//userData->buf_pos = 0;
		//Get data from callback.
		//TODO: Make callback do soft-start after an underrun. 
		AudioCallback(userData->audioCallbackData, (unsigned char*)userData->buf, buf_len);
		//Write data to output.
		int written = snd_pcm_writei(userData->pcm_handle, userData->buf, avail);
		if (written < 0) {
			//TODO more thorough error checking
			
			//We got an underrun.
			//Set underrun flag to true
			//discard buf(?) (i.e., skip a bit of the track)
			// (This is controversial. Is it better to go silent and then come back where you were, or
			//  to come back ~where you would have got to?)
			userData->pcm_underrun = true;
		}
		else {
			assert(written == avail && "This code assumes that it is always possible to write 'avail' frames worth of data. If this is not true, change the code.");
		}
		
#if 0
		///The ALSA example code seems to think that avail = snd_pcm_avail_update followed by written = snd_pcm_writei(avail) might not always lead to written==avail.
		///I'd like to see that happen before adopting the more complicated version below.
		if (buf_pos == buf_size) {
			//snd_pcm_sframes_t avail = snd_pcm_avail_update(handle);
			//read avail frames into buf,
			//buf_size = avail;
			//buf_pos = 0;
			AudioCallback(&callbackData, (unsigned char*)buf, avail * channels * snd_pcm_format_physical_width(format)/CHAR_BIT);
			
		}
		if (buf_pos < buf_size) {
			//Write from buf
			
			int frames_written = snd_pcm_writei(handle, ptr, cptr);
			if (frames_written < 0) {
				userData->pcm_underrun = true;
				
				/*
				if (xrun_recovery(handle, err) < 0) {
					printf("Write error: %s\n", snd_strerror(err));
					exit(EXIT_FAILURE);
				}
				init = true;
				break;*/  /* skip one period */
			}
			
		}
#endif
	}
	return 0;
}
//#endif
//#if 0
static int pcm_try_recover(snd_pcm_t *pcm_handle) {
	//if (verbose)
	//	printf("stream recovery\n");
	/*
	 * TODO: For the 'illegal states', ensure that they actually can't occur, and/or figure out the conditions where
	 *       they will occur and the correct responses to those conditions.
	 * 
	 * SND_PCM_STATE_OPEN         Open                                                            Illegal state, should have already started pcm before calling pcm_try_recover.
	 * SND_PCM_STATE_SETUP        Setup installed                                                 Illegal state, should have already started pcm before calling pcm_try_recover.
	 * SND_PCM_STATE_PREPARED     Ready to start                                                  Illegal state, should have already started pcm before calling pcm_try_recover.
	 * SND_PCM_STATE_RUNNING      Running                                                         Do nothing. Problem has resolved itself.
	 * SND_PCM_STATE_XRUN         Stopped: underrun (playback) or overrun (capture) detected      snd_pcm_prepare();
	 * SND_PCM_STATE_DRAINING     Draining: running (playback) or stopped (capture)               Illegal state(?), we never want to call drain on the pcm
	 * SND_PCM_STATE_PAUSED       Paused                                                          Illegal state(?), we never want to call 'pause' on the pcm
	 * SND_PCM_STATE_SUSPENDED    Hardware is suspended                                           Illegal state(?), we never want to call 'suspend' on the pcm
	 * SND_PCM_STATE_DISCONNECTED Hardware is disconnected                                        Need to find a new output stream. TODO: requires rearchitecture of output system.
	 */
	
	printf("Attempting Underrun Recovery\n");
	snd_pcm_state_t pcm_state = snd_pcm_state(pcm_handle);
	switch (pcm_state) {
		case SND_PCM_STATE_XRUN:{
			int err = snd_pcm_prepare(pcm_handle);
			if (err < 0) {
				printf("Underrun recovery failed: %s\n", snd_strerror(err));
				return -1;
			}
			break;
		}
		//case SND_PCM_STATE_SUSPENDED: //For now, let's not handle suspension. I don't even know what it is or what could trigger it.
		//	snd_pcm_resume(), snd_pcm_prepare(); //The example code does this, looping on snd_pcm_resume until it succeeds.
		case SND_PCM_STATE_RUNNING:{
			//do nothing

			break;
		}
		default:{
			printf("PCM Recovery failed. Illegal state: %d\n", pcm_state);
			break;
		}
	}
	printf("Underrun Recovery Complete\n");
	return 0;
#if 0
	if (err == -EPIPE) {    /* under-run */
		err = snd_pcm_prepare(handle);
		if (err < 0) {
			printf("Can't recovery from underrun, prepare failed: %s\n", snd_strerror(err));
		}
		return 0;
	} else if (err == -ESTRPIPE) { /*Suspended (?)*/
		while ((err = snd_pcm_resume(handle)) == -EAGAIN)
			sleep(1);   /* wait until the suspend flag is released *///TODO: Don't block other tasks!!!
		if (err < 0) {
			err = snd_pcm_prepare(handle);
			if (err < 0)
				printf("Can't recovery from suspend, prepare failed: %s\n", snd_strerror(err));
		}
		return 0;
	}
	return err;
#endif
}

static int connections_handler(short *revents, size_t num, void *ud) {
	struct SocketManager *socket_manager = ud;
	for (size_t i = 0; i != num; ++i) {
		if (revents[i] & POLLERR) {
			//TODO: Close the connection or something?
			continue;
		}
		if (revents[i] & POLLIN) {
			char input_data[256];
			ssize_t bytes_read = read(socket_manager->connections[i].connection_fd, input_data, sizeof input_data);
			if (bytes_read == -1) {
				printf("Error reading from socket\n");
				//TODO: Close the connection or something?
				continue;
			}
			if (CommandParser_execute(socket_manager->connections[i].parser, input_data, (size_t)bytes_read) < 0) {
				//TODO: Close the connection or something?
				printf("Command Parser error on connection #%zu\n", i);
			}
		}
		if (revents[i] & POLLOUT) {
			//TODO: Send buffered replies, if any.
			continue;
		}
	}
	return 0;
#if 0
	assert(num == 1);
	if (revents[0] & POLLERR) {
		printf("Error reading stdin\n");
		return -1;
	}
	if (revents[0] & POLLOUT) {
		printf("stdin incorrectly configured, producing output events.");
		return -1;
	}
	if (revents[0] & POLLIN) {
		StdInManager_read(ud);
	}
	return 0;
#endif
}
struct socket_handler_data {
	struct SocketManager *socket_manager;
};
static int socket_handler(short revent, void *ud) {
	//(void)revent; (void)ud;
	return SocketManager_handleSocketRevent(((struct socket_handler_data*)ud)->socket_manager, revent);
}

//#endif
//#if 0
static int poll_and_dispatch_events_v2(
	int (*pcm_handler)(unsigned short revents, void *ud),
	void *pcm_handler_ud,
	int (*connections_handler)(short *revents, size_t num, void *ud),
	void *connections_handler_ud,
	int (*socket_handler)(short revent, void *ud),
	void *socket_handler_ud,
	struct pollfd *pcm_pollfds,
	size_t pcm_pollfds_len,
	struct pollfd *connections_pollfds,
	size_t connections_pollfds_len,
	struct pollfd *socket_pollfd,
	snd_pcm_t *pcm_handle)
{
	//TODO: Poll for output as well.
	//      Update command_parser to send output to an output buffer
	//      and send the output when POLLOUT events arrive.
	
	//printf("poll_and_dispatch_events_v2\n");
	int err = 0;
	/*
	pollfds = concat(pcm_pollfds, socket_pollfd, connections_pollfds);
	poll(pollfds);
	demangled_pcm = demangle_pcm(pcm_pollfds);
	pcm_handler(demangled_pcm);
	//Handle connections before the socket, since the socket_handler could alter the list of connections.
	connections_handler(connections_pollfds);
	socket_handler(socket_pollfd);
	*/
	size_t num_pollfds = pcm_pollfds_len+connections_pollfds_len+1;
	struct pollfd *pollfds = calloc(num_pollfds, sizeof *pollfds);
	if (num_pollfds > 0 && !pollfds) return -1;
	//Explicitly handle 0 size allocation to keep clang analyzer happy.
	short *connections_revents = connections_pollfds_len > 0 ? calloc(connections_pollfds_len, sizeof *connections_revents) : 0;
	if (connections_pollfds_len > 0 && !connections_revents) {
		err = -1;
		goto end;
	}
	size_t first_pcm_pollfd = 0;
	size_t first_connections_pollfd = first_pcm_pollfd+pcm_pollfds_len;
	size_t first_socket_pollfd = first_connections_pollfd+connections_pollfds_len;
	if (pcm_pollfds_len > 0) {
		memcpy(pollfds+first_pcm_pollfd, pcm_pollfds, pcm_pollfds_len*sizeof *pcm_pollfds);
	}
	if (connections_pollfds_len > 0) {
		memcpy(pollfds+first_connections_pollfd, connections_pollfds, connections_pollfds_len*sizeof *connections_pollfds);
	}
	memcpy(pollfds+first_socket_pollfd, socket_pollfd, sizeof *socket_pollfd);
	
	//Repoll 5 times per second.
	//Using explicit timeout to handle the case where
	//the pcm is suspended and we need to repeatedly call
	//snd_pcm_resume.
	//Don't just do this in pcm_handler, since POLLERR will be continuously generated
	//and would use excessive CPU (I think?).
	//Instead, remove pcm_pollfds from the poll list until the pcm_handle has recovered,
	//and call snd_pcm_resume separately.
	if (poll(pollfds, num_pollfds, 200) < 0) {//TODO: Not all <0 return values from poll indicate fatal errors, should update the error checking.
		err = -1;
		printf("Poll failed");
		goto end;
	}
	//printf("Got Events\n");

	
	//If no pcm_pollfds, the pcm must be in a suspended state.
	//Need to have a relatively short timeout on the poll() call, to also call snd_pcm_resume in the top level loop.
	//
	if (pcm_pollfds) {
		//Just to be different, ALSA uses unsigned short for revents, even though
		//poll uses short.
		unsigned short pcm_revents = 0;
		err = snd_pcm_poll_descriptors_revents(pcm_handle, pollfds+first_pcm_pollfd, (unsigned)pcm_pollfds_len, &pcm_revents);
		if (err < 0) {
			printf("snd_pcm_poll_descriptors_revents failed!\n");
			goto end;
		}
		if (pcm_handler(pcm_revents, pcm_handler_ud) < 0) {
			printf("pcm_handler failed!\n");
			err = -1;
			goto end;
		}
	}
	else {
		printf("no pcm_pollfds\n");
	}
	for (size_t i = 0; i != connections_pollfds_len; ++i) {
		connections_revents[i] = (pollfds+first_connections_pollfd)[i].revents;
	}
	//connections_handler MUST be called before socket_handler, since socket_handler
	//could modify the list of active connections.
	if (connections_handler(connections_revents, connections_pollfds_len, connections_handler_ud) < 0) {
		err = -1;
		goto end;
	}
	if (socket_handler((pollfds+first_socket_pollfd)->revents, socket_handler_ud) < 0) {
		err = -1;
		goto end;
	}
	
	end:;
	free(connections_revents);
	free(pollfds);//TODO?:Cache these lists, avoid unnecessary dynamic memory activity
	return err;
}
//#endif
#if 0
static int poll_and_dispatch_events(
	struct pollfd *pcm_ufds,
	int num_pcm_descriptors,
	struct pollfd *stdin_pollfd,
	struct pollfd *server_pollfd,
	struct StdInManager *stdin_manager,
	struct SocketManager *socket_manager,
	bool *init,
	snd_pcm_t *handle)
{
	struct pollfd *ufds = malloc((num_pcm_descriptors+2)*sizeof *ufds);
	if (!ufds) return -1;
	
	for (int i = 0; i != num_pcm_descriptors; ++i) {
		ufds[i] = pcm_ufds[i];
	}
	ufds[num_pcm_descriptors+0] = *stdin_pollfd;
	ufds[num_pcm_descriptors+1] = *server_pollfd;
	int num_pollfds = num_pcm_descriptors+2;
	
	int err = 0;
	unsigned short revents = 0;
	bool gotEvent = false;
	while (!gotEvent) {
		//TODO: Also wait for SIGTERM, and
		//cleanly shut down if it is received.
		poll(ufds, num_pollfds, -1);
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
				*init = true;
			} else {
				printf("Wait for poll failed\n");
				goto end;
			}
			gotEvent = true;
		}
		if (revents & POLLOUT) {
			err = 0;
			gotEvent = true;
		}
		if (ufds[num_pcm_descriptors+0].revents & POLLERR) {
			fprintf(stderr, "Error reading stdin\n");
			exit(-1);
		}
		if (ufds[num_pcm_descriptors+0].revents & POLLIN) {
			StdInManager_read(stdin_manager);
			//gotEvent = true;
		}
		SocketManager_handleSocketRevent(socket_manager, &ufds[num_pcm_descriptors+1]);
		/*
		 * for (int i = 0; i < num_in_connections; ++i) {
		 *     if(ufds[first_in_connection+i].revents & POLLIN) {
		 *         Connection_read(&connections[i]);
		 *     }
		 * }
		 * 
		 */
	}
	end:;
	free(ufds);
	return err;
}
#endif
//#if 0
static int write_and_poll_loop_v2(snd_pcm_t *pcm_handle, int control_server_socket_fd) {
	int err = 0;
	//TODO: Design+Implement error recovery scheme (especially for all the callbacks).
	//TODO; Perhaps make the halt-command separate? (Rationale:
	// - Normal operation shouldn't be able to cause the system to quit, this should be a separate channel.
	// - When a halt command is received, the system shuts down and everything stops (and in particular, future commands are ignored),
	//   this is a fundamentally different behaviour from all the other commands).
	
	//TODO: Fix structure of deallocation handling when initialisation fails halfway through.
	//      Perhaps put allocation/deallocation in a separate function that can succeed or fail as
	//      an atomic unit.
	struct AudioCallbackData callbackData;
	if (init_command_queue(&callbackData.commandQueue) < 0) {
		printf("Couldn't init command_queue\n");
		err = -1;
		goto end_end;
	}
	init_PlaylistPlayer(&callbackData.player); //TODO: Error handling for init_PlaylistPlayer
	callbackData.halt_command = 0;
	
	struct CommandReadData commandReadData = (struct CommandReadData){
		.audioCallback = &callbackData
	};
	
	struct pcm_handler_data pcm_handler_data = (struct pcm_handler_data){
		.pcm_handle = pcm_handle,
	    .pcm_underrun = false,
	    .audioCallbackData = &callbackData,
		.buf = 0
	};
	{
	struct SocketManager socket_manager;
	if (SocketManager_init(&socket_manager, control_server_socket_fd, command_was_read, &commandReadData) != 0) {
		printf("Couldn't init SocketManager\n");
		err = -1;
		goto end_pcm_handler_data;
	}
	
	{
	struct pollfd *pcm_pollfds;
	int pcm_pollfds_len = snd_pcm_poll_descriptors_count(pcm_handle);
	//printf("Num pcm_descriptors: %d\n", pcm_pollfds_len);
	if (pcm_pollfds_len <= 0) {
		printf("Invalid snd_pcm_poll_descriptors_count\n");
		err = pcm_pollfds_len;
		goto end_SocketManager;
	}
	pcm_pollfds = malloc(sizeof(struct pollfd) * (size_t)pcm_pollfds_len);
	if (!pcm_pollfds) {
		printf("No enough memory\n");
		err = -ENOMEM;
		goto end_SocketManager;
	}
	if ((err = snd_pcm_poll_descriptors(pcm_handle, pcm_pollfds, (unsigned int)pcm_pollfds_len)) < 0) {
		printf("Unable to obtain poll descriptors for playback: %s\n", snd_strerror(err));
		err = -1;
		goto end_pcm_pollfds;
	}
	struct pollfd server_pollfd;
	SocketManager_fill_socket_poll_descriptor(&socket_manager, &server_pollfd);
#if 0
	for (int i=0;i!=pcm_pollfds_len;++i) {
		EventManager_add(pcm_pollfds[i]);
	}
#endif
	if (SocketManager_start(&socket_manager) != 0) {
		printf("Couldn't start control server\n");
		err = -1;
		goto end_pcm_pollfds;
	}
	{
	struct pollfd *connections_pollfds = 0;
	while (!(callbackData.halt_command && callbackData.halt_command->data.done.done)) {
		size_t connections_pollfds_len = SocketManager_getNumConnections(&socket_manager);
		//TODO: Integer overflow handling.
		//printf("connections_pollfds_len: %uld\n", connections_pollfds_len);
		if (connections_pollfds_len > 0) {
			struct pollfd *new_connections_pollfds = realloc(connections_pollfds, connections_pollfds_len * sizeof *connections_pollfds);
			if (connections_pollfds_len != 0 && !new_connections_pollfds) {
				printf("Couldn't allocate memory for socket connection's pollfds\n");
				err = -1;
				goto end_all;
			}
			connections_pollfds = new_connections_pollfds;
		}
		SocketManager_fill_connections_poll_descriptors(&socket_manager, connections_pollfds);
		if (pcm_handler_data.pcm_underrun) {
			if (pcm_try_recover(pcm_handle) < 0) {
				printf("PCM recovery failed");
				err = -1;
				goto end_all;
			}
			pcm_handler_data.pcm_underrun = false;
		}
#if 0
	static int poll_and_dispatch_events_v2(
		int (*pcm_handler)(unsigned short revents, void *ud),
		void *pcm_handler_ud,
		int (*connections_handler)(short *revents, size_t num, void *ud),
		void *connections_handler_ud,
		int (*socket_handler)(short revent, void *ud),
		void *socket_handler_ud,
		struct pollfd *pcm_pollfds,
		size_t pcm_pollfds_len,
		struct pollfd *connections_pollfds,
		size_t connections_pollfds_len,
		struct pollfd *socket_pollfd,
		snd_pcm_t *pcm_handle)
#endif
		if (poll_and_dispatch_events_v2(
		    	pcm_handler, &pcm_handler_data,
		    	connections_handler, &socket_manager,
		    	socket_handler, &(struct socket_handler_data){.socket_manager = &socket_manager},
		    	pcm_handler_data.pcm_underrun ? 0 : pcm_pollfds, (size_t)(pcm_handler_data.pcm_underrun ? 0 : pcm_pollfds_len),
		    	connections_pollfds, connections_pollfds_len,
		    	&server_pollfd,
		    	pcm_handle)
		  < 0)
		{
			printf("poll_and_dispatch_events_v2 failed!\n");
			err = -1;
			goto end_all;
		}
	}
	end_all:;
	//end_connections_pollfds:;
	free(connections_pollfds);
	}
	end_pcm_pollfds:;
	free(pcm_pollfds);
	}
	end_SocketManager:;
	SocketManager_free(&socket_manager);
	}
	end_pcm_handler_data:;
	free(pcm_handler_data.buf);
	
	free_command(callbackData.halt_command);
	//end_PlaylistPlayer:;
	free_PlaylistPlayer(&callbackData.player);
	//end_command_queue:;
	free_command_queue(&callbackData.commandQueue);
	end_end:;
	return err;
}
//#endif
#if 0
static int write_and_poll_loop(snd_pcm_t *handle, int control_server_socket_fd)
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
		return -1;
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
#if 0
	struct EventManager ev_manager;
	
#endif
	struct SocketManager socket_manager;
	if (SocketManager_init(&socket_manager, control_server_socket_fd) != 0) {
		return -1;
	}
	
	struct StdInManager stdin_manager;
	StdInManager_init(&stdin_manager, &inputReadCallback, &inputReadData);

	struct pollfd *ufds;
	
	struct pollfd stdin_pollfd;
	struct pollfd server_pollfd;
	
	signed short *ptr;
	int err, num_pcm_descriptors, cptr;
	bool init = false;
	num_pcm_descriptors = snd_pcm_poll_descriptors_count(handle);
	printf("Num pcm_descriptors: %d\n", num_pcm_descriptors);
	if (num_pcm_descriptors <= 0) {
		printf("Invalid snd_pcm_poll_descriptors_count\n");
		return num_pcm_descriptors;
	}
	//int num_poll_descriptors = num_pcm_descriptors+1; //pcm+socket.
	                                    //As connections occur on the socket, more
	                                    //file descriptors will be added to the list.
	ufds = malloc(sizeof(struct pollfd) * num_pcm_descriptors);
	if (ufds == NULL) {
		printf("No enough memory\n");
		return -ENOMEM;
	}
	if ((err = snd_pcm_poll_descriptors(handle, ufds, num_pcm_descriptors)) < 0) {
		printf("Unable to obtain poll descriptors for playback: %s\n", snd_strerror(err));
		return err;
	}
	StdInManager_fill_pollfd(&stdin_manager, &stdin_pollfd);
	SocketManager_fill_socket_poll_descriptor(&socket_manager, &server_pollfd);
#if 0
	for (int i=0;i!=num_pcm_descriptors;++i) {
		EventManager_add(ufds[i]);
	}
#endif
	if (SocketManager_start(&socket_manager) != 0) {
		printf("Couldn't start control server\n");
		return -1;
	}
	init = true;
	while (!(callbackData.halt_command && callbackData.halt_command->data.done.done)) {
		if (!init) {
			poll_and_dispatch_events(ufds, num_pcm_descriptors, &stdin_pollfd, &server_pollfd, &stdin_manager, &socket_manager, &init, handle);
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
				init = true;
				break;  /* skip one period */
			}
			if (snd_pcm_state(handle) == SND_PCM_STATE_RUNNING) init = false;
			ptr += err * channels;
			cptr -= err;
			if (cptr == 0) break;
			/* it is possible, that the initial buffer cannot store */
			/* all data from the last period, so wait awhile */
			//err = wait_for_poll(handle, ufds, num_pcm_descriptors);
			poll_and_dispatch_events(ufds, num_pcm_descriptors, &stdin_pollfd, &server_pollfd, &stdin_manager, &socket_manager, &init, handle);
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
#endif
static int do_main(void) {
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
	
	//TODO: Read these from systemd/other external process, to allow easy switching of server address.
	int control_server_socket_fd = init_CommandServerSocket();
	if (control_server_socket_fd < 0) {
		printf("Couldn't open server socket\n");
		return -1;
	}
	snd_output_t *output = 0;
	snd_pcm_t *handle = 0;
	int err = 0;
	snd_pcm_hw_params_t *hwparams = 0;
	snd_pcm_sw_params_t *swparams = 0;
	snd_pcm_hw_params_alloca(&hwparams);
	snd_pcm_sw_params_alloca(&swparams);
	err = snd_output_stdio_attach(&output, stdout, 0);
	if (err < 0) {
		printf("Couldn't attach pcm to stdout\n");
		return -1;
	}
	/*if (err < 0) {
		printf("Output failed: %s\n", snd_strerror(err));
		return 0;
	}*/
	//printf("Playback device is %s\n", device);
	
	//printf("Stream parameters are %iHz, %s, %i channels\n", rate, snd_pcm_format_name(format), channels);
	if ((err = snd_pcm_open(&handle, device, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK)) < 0) {
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
	
	//TODO: Error handling on close. Is it even possible?
	snd_config_update_free_global();
	err = write_and_poll_loop_v2(handle, control_server_socket_fd);
	if (err < 0)
		printf("Transfer failed: %s\n", snd_strerror(err));
	snd_pcm_hw_free(handle);
	snd_pcm_close(handle);
	while (close(control_server_socket_fd) == -1) {
		switch (errno) {
			case EBADFD:
				assert(false && "control_server_socket_fd must be valid at this point");
				return -1;
			case EINTR:
				continue;
			case EIO:
				printf("IO error when closing control_server_socket_fd\n");
				return -1; //TODO: Figure out how to properly handle this.
		}
	}
	snd_output_close(output);
	return err;
}
int main(void)
{
	int retv = do_main();
	printf("Done!\n");
	return retv;
}
