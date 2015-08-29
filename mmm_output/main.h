#ifndef MMM_MAIN_H
#define MMM_MAIN_H
//Global includes.
//Ideally should be empty.
//typedef struct PlaylistPlayer_ {} PlaylistPlayer_;
#include <stdbool.h>
#include <sndfile.h>

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

typedef enum {
	UpdatePlaylist,
	ReplacePlaylist,
	//Seek,            //TODO: Don't do via explicit Seek command, just improve update/replace_playlist
	Pause,
	Resume,
	GetCurrentState,
	Halt,
	//,
} CommandType;
#define NUM_COMMANDS 6

typedef struct {
	char *url;
	//
} Track;

typedef struct {
	char *guid_str;
} GUID;

typedef struct {
	GUID guid;
	Track track;
} TrackPlayIdentifier;

typedef struct TrackPlayIdentifierNode_ {
	struct TrackPlayIdentifierNode_ *prev;
	struct TrackPlayIdentifierNode_ *next;
	TrackPlayIdentifier track;
} TrackPlayIdentifierNode;
typedef struct {
	bool dirty;
	TrackPlayIdentifierNode *front;
	TrackPlayIdentifierNode *back;
	//unsigned int start_index;
	//unsigned int end_index;
} Playlist;

typedef struct {
	bool done;
} DoneFlag;

struct Command {
	struct Command *prev;
	struct Command *next;
	CommandType type;
	union {
		Playlist new_playlist;
		DoneFlag done;
	} data;
};

typedef struct {
	//void init(char const *url);
	//void start_cache(size_t pos);
	//void set_highwater(size_t max_cache_size);
	//void set_lowwater(size_t min_cache_size);
	//void get_frames(Uint8 *const stream, int const len);
	//prev_non_positive, prev_non_negative
	SNDFILE *file;
} FileCache;

typedef struct FileCacheNode_ {
	struct FileCacheNode_ *next;
	struct FileCacheNode_ *prev;
	GUID track_guid;
	FileCache cache;
} FileCacheNode;

typedef struct {
	FileCacheNode *front;
	FileCacheNode *back;
} FileCacheList;
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
void add_track(Playlist *playlist, TrackPlayIdentifierNode *track);
void free_command(struct Command *command);
void delete_forwards_FileCacheNode(FileCacheNode *file);
void destroy_TrackPlayState(TrackPlayState *state);
#endif //MMM_MAIN_H
