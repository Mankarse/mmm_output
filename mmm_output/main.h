#ifndef MMM_MAIN_H
#define MMM_MAIN_H
//Global includes.
//Ideally should be empty.
//typedef struct PlaylistPlayer_ {} PlaylistPlayer_;
#include <stdbool.h>

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
void add_track(Playlist *playlist, TrackPlayIdentifierNode *track);
void free_command(struct Command *command);
#endif //MMM_MAIN_H
