#ifndef MMM_MAIN_H
#define MMM_MAIN_H
//Global includes.
//Ideally should be empty.
//typedef struct PlaylistPlayer_ {} PlaylistPlayer_;
#include <stdbool.h>
#include <sndfile.h>
typedef enum {
	UpdatePlaylist,     //TODO
	ReplacePlaylist,
	//Seek,            //TODO
	Pause,
	Resume,
	//GetCurrentState, //TODO
	Halt, //TODO
	//,
} CommandType;
#define NUM_COMMANDS 5

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
void add_track(Playlist *playlist, TrackPlayIdentifierNode *track);
void free_command(struct Command *command);
void delete_forwards_FileCacheNode(FileCacheNode *file);
#endif //MMM_MAIN_H
