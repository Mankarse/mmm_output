#include "command_parser.h"
#include "stdlib.h"
#include "assert.h"
#include "stdbool.h"
#include "stdio.h"
#include "unistd.h"
#include "main.h"
#define ONE_LINE_PER_COMMAND //This is buggy, since our command format demands multi-line commands to play files with filenames containing newlines.
#ifdef ONE_LINE_PER_COMMAND
struct Command *parse_command(char const * const str, size_t const len);
struct LineManager {
	void *ud;// = (struct InputCallbackData){.audioCallback = &callbackData};
	void (*inputCallback)(size_t in_line_size, char const *in_line, void *ud);
	size_t input_line_capacity;// = 1;
	size_t input_line_size;// = 0;
	char *input_line;// = malloc(input_line_capacity);
};
int create_LineManager(struct LineManager *uninitialisedMemory, void (*inputCallback)(size_t in_line_size, char const *in_line, void *ud), void *ud) {
	uninitialisedMemory->ud = ud;
	uninitialisedMemory->inputCallback = inputCallback;
	
	uninitialisedMemory->input_line_capacity = 1;
	uninitialisedMemory->input_line_size = 0;
	uninitialisedMemory->input_line = malloc(uninitialisedMemory->input_line_capacity);
	if (!uninitialisedMemory->input_line) {
		return -1;
	}
	return 0;
}

void destroy_LineManager(struct LineManager *lineManager) {
	free(lineManager->input_line);
}

int LineManager_read(struct LineManager *manager, char const *in_data, size_t in_data_size) {
	while (manager->input_line_capacity < manager->input_line_size + in_data_size) {
		manager->input_line = realloc(manager->input_line, manager->input_line_capacity*2);
		if (!manager->input_line) {
			printf("Couldn't allocate memory for input line");//TODO: remove ad-hoc printf.
			return -1;
		}
		manager->input_line_capacity = manager->input_line_capacity*2;
	}
	memcpy(&manager->input_line[manager->input_line_size], in_data, in_data_size);
	bool foundNewLine = false;
	for (size_t i = 0; i < in_data_size; ++i) {
		if (manager->input_line[manager->input_line_size + i] == '\n') {
			foundNewLine = true;
			manager->inputCallback(manager->input_line_size+i, manager->input_line, manager->ud);
			memcpy(&manager->input_line[0], &manager->input_line[manager->input_line_size + i + 1], in_data_size - i - 1);
			manager->input_line_size = in_data_size - i - 1;
			break;
		}
	}
	if (!foundNewLine) {
		manager->input_line_size += in_data_size;
	}
	return 0;
}

struct CommandParser {
	command_read_callback cb;
	void *command_read_ud;
	struct LineManager inLineManager;
};

static void eat_line(size_t len, char const *line, void *ud) {
	struct CommandParser *parser = ud;
	struct Command *command = parse_command(line,len);
	//if (!command) {
		//printf("Error parsing command\n");
		//exit(-1);//TODO proper error handling?
	//}
	parser->cb(command, parser->command_read_ud);
}
struct CommandParser *new_CommandParser(command_read_callback cb, void *ud) {
	struct CommandParser *command_parser = malloc(sizeof *command_parser);
	if (!command_parser) return NULL;
	if (create_CommandParser(command_parser, cb, ud) != 0) {
		free(command_parser);
		return NULL;
	}
	return command_parser;
}
int create_CommandParser(struct CommandParser *uninitialisedMemory, command_read_callback cb, void *ud) {
	if (create_LineManager(&uninitialisedMemory->inLineManager, eat_line, uninitialisedMemory)) {
		printf("Couldn't initialise LineManager\n");
		return -1;
	}
	uninitialisedMemory->cb = cb;
	uninitialisedMemory->command_read_ud = ud;
	return 0;
}
void destroy_CommandParser(struct CommandParser *commandParser) {
	destroy_LineManager(&commandParser->inLineManager);
}
void delete_CommandParser(struct CommandParser *commandParser) {
	destroy_CommandParser(commandParser);
	free(commandParser);
}

bool consume_space(char const * const str, size_t *pos, size_t const len) {
	if (*pos >= len || str[*pos] != ' ') {
		return false;
	}
	else {
		++*pos;
		return true;
	}
}
char *parse_GUID(char const * const str, size_t *pos, size_t const len) {
	size_t const initial_pos = *pos;

	for ( ;*pos != len && str[*pos] != ' '; ++*pos);

	size_t const GUID_len = *pos - initial_pos;
	char *GUID = malloc(GUID_len + 1);
	if (!GUID) {
		//TODO -- Report Out of Memory to higher layer, rather than crashing
		assert(false && "Internal Server Error, couldn't allocate memory");
		return 0;
	}
	for (size_t i = 0; i != GUID_len; ++i) {
		GUID[i] = str[initial_pos+i];
	}
	GUID[GUID_len] = '\0';
	//printf("GUID: %s\n", GUID);
	return GUID;
}

bool parse_LongStringStart(char const * str, size_t *pos, size_t const len) {
	size_t const initial_pos = *pos;
	if (*pos == len) {
		return false;
	}
	if (str[*pos] != '[') {
		return false;
	}
	++*pos;
	if (*pos == len) goto fail;
	for (; *pos != len; ++*pos) {
		if (str[*pos] == '[') {
			++*pos;
			return true;
		}
		if (str[*pos] != '=') goto fail;
	}
	fail:
	*pos = initial_pos;
	return false;
}

char *parse_LongString(char const * const str, size_t *pos, size_t const len) {
	//fprintf(stderr, "Parsing LongString\n");
	size_t const initial_pos = *pos;

	bool success = parse_LongStringStart(str, pos, len);
	if (!success) {
		return 0;
	}

	size_t const string_start = *pos;
	size_t bracket_len = *pos - initial_pos - 2;

	//fprintf(stderr, "Parsed LongStringStart. bracket_len = %zu, string_start = %zu\n", bracket_len, *pos);

	size_t string_end = 0;
	bool in_bracket = false;
	bool string_complete = false;
	for (; *pos != len; ++*pos) {
		if (in_bracket) {
			if (*pos - string_end - 1 == bracket_len) {
				if (str[*pos] == ']') {
					string_complete = true;
					++*pos;
					break;
				}
				else {
					string_end = 0;
					in_bracket = false;
					continue;
				}
			}
			else if (str[*pos] == '=') {
				continue;
			}
			else if (str[*pos] == ']' ){
				string_end = *pos;
				continue;
			}
			assert(false && "Internal Server Error - Invalid State in parse_LongString");
		}
		else {
			if (str[*pos] == ']') {
				//printf("Found opening ] at pos: %zu\n", *pos);
				string_end = *pos;
				in_bracket = true;
				continue;
			}
		}
	}
	if (!string_complete) {
		*pos = initial_pos;
		return 0;
	}
	char *out_str = malloc(string_end-string_start + 1);
	if (!out_str) {
		//TODO- Remove this assert; report the error instead.
		assert(false && "Internal Server Error - Out of Memory");
		*pos = initial_pos;
		return 0;
	}
	for (size_t i = 0; i != string_end - string_start; ++i) {
		out_str[i] = str[string_start + i];
	}
	out_str[string_end - string_start] = '\0';
	printf("LongString: %s\n", out_str);
	return out_str;
}

TrackPlayIdentifierNode *parse_TrackPlayIdentifierNode(char const * const str, size_t *pos, size_t const len) {
	size_t const initial_pos = *pos;
	char *url = 0;
	TrackPlayIdentifierNode *track = 0;

	char *guid = parse_GUID(str, pos, len);
	if (!guid) {
		fprintf(stderr, "Couldn't parse GUID\n");
		goto fail;
	}
	bool success = consume_space(str, pos, len);
	if (!success) {
		fprintf(stderr, "Couldn't find track-name after GUID: %s\n", guid);
		goto fail;
	}

	url = parse_LongString(str,pos,len);
	if (!url) {
		fprintf(stderr, "Couldn't understand track-name after GUID: %s\n", guid);
		goto fail;
	}
	track = malloc(sizeof *track);
	if (!track) {
		//TODO - report Out of Memory to higher layer, rather than failing here.
		assert(false && "Internal Server Error, Out of Memory.");
		goto fail;
	}
	track->next = 0;
	track->prev = 0;
	track->track = (TrackPlayIdentifier){.guid = {.guid_str = guid}, .track = {.url = url}};
	return track;
	fail:;
	free(guid);
	free(url);
	free(track);
	*pos = initial_pos;
	return 0;
}

bool parse_playlist(Playlist *playlist, char const * const str, size_t *pos, size_t const len) {
	playlist->front = 0;
	playlist->back = 0;
	while (*pos != len) {
		bool success = consume_space(str, pos, len);
		if (!success) return false;
		TrackPlayIdentifierNode *track = parse_TrackPlayIdentifierNode(str, pos, len);
		if (!track) return false;
		add_track(playlist, track);
	}
	return true;
}

struct Command *parse_command(char const * const str, size_t const len) {
	/*char *string = malloc(len+1);
	if (string) {
		memcpy(string,str,len);
		string[len] = 0;
		fprintf(stderr,"%s\n",string);
	}
	free(string);*/
	size_t pos = 0;
	struct Command *command = malloc(sizeof *command);
	if (!command) {return 0;}
	command->prev = 0;
	command->next = 0;
	if (len >= 4 && memcmp(str, "halt", 4) == 0) {
		command->type = Halt;
		//command->data.done.cond = SDL_CreateCond();//TODO re-add
		//command->data.done.mutex = SDL_CreateMutex();
		command->data.done.done = false;
		
		//if (!command->data.done.cond || !command->data.done.mutex) {//TODO re-add
		//	free_command(command);
		//	return 0;
		//}
	}
	else if (len >= 5 && memcmp(str, "pause", 5) == 0) {
		command->type = Pause;
	}
	else if (len >= 6 && memcmp(str, "resume", 6) == 0) {
		command->type = Resume;
	}
	else if (len >= 15 && memcmp(str, "update_playlist", 15) == 0) {
		//printf("Updating Playlist\n");
		pos = 15;
		command->type = UpdatePlaylist;
		bool success = parse_playlist(&command->data.new_playlist, str, &pos, len);
		if (!success) {
			free_command(command);
			return 0;
		}
	}
	else if (len >= 16 && memcmp(str, "replace_playlist", 16) == 0) {
		//printf("Replacing Playlist\n");
		pos = 16;
		command->type = ReplacePlaylist;
		bool success = parse_playlist(&command->data.new_playlist, str, &pos, len);
		if (!success) {
			free_command(command);
			return 0;
		}
	}
	else {
		free(command);
		return 0;
	}
	return command;
}

int CommandParser_execute(
	struct CommandParser *parser, char const /*restrict*/ *str, size_t len
	/*, size_t *chars_consumed,command_read_callback cb, void *ud*/ )
{
	return LineManager_read(&parser->inLineManager, str, len);
}



#endif /*ONE_LINE_PER_COMMAND*/






#ifdef MULTI_LINE_COMMANDS
enum CommandParserState {
	CommandParserErrorState,
	CommandParserStartState,
	//CommandParserCommandNameState, //Part of StartState
	ParsePlaylist,
	ParsePlaylistItem,
	ParseGUID,
	ParseString,
	ParseLongString
};
struct CommandParserStartState {
	int step; //0: Initial (need to initialise output Command command,
	          //            read first character to determine possible command type)
	          //1: Initialised (need to read command type).
	          //    (call sub-levels here, if required for the particular command.)
	          //2: Finish (read data from sub-levels, write to output callback).
	Command *command;
	//If we need to support multiple commands with a common prefix, this will have to
	//changed to a custom enum.
	bool possiblePrefixTypes[NUM_COMMANDS];
	int num_chars_seen; //-1 for done
};

struct CommandParserCommandNameState {
	
};
struct CommandParserData {
	CommandParserState type;
	void *return_val; //Filled by sub-levels to provide return-values fromm procedures.
	union {
		CommandParserStartState start;
	};
};

struct CommandParser {
	//In principle the parser state should be stored in a stack, but
    //there is no recursion and the sub-levels are uniquely identified.
	enum CommandParserState state;
	Command *partial_command;
	
	//The full content of the command currently being parsed.
	//Used for error messages.
	char *command_string;
	size_t command_string_len;
	size_t command_string_capacity;
	
	char *error_message;
	
	Playlist playlist;
	char *string_data;
	size_t string_len;
	size_t string_capacity;
	int num_opening_brackets; //0 for [[foo]], 1 for [=[foo]=], etc, -1 for short-string 
	int num_closing_brackets;
	
	struct CommandParserData state_stack[5];
	size_t stack_size;
};

struct CommandString {
	char const *const key;
	CommandType const value;
};

CommandString const command_strings[] {
	{"pause", Pause},
	{"resume", Resume},
	{"halt", Halt},
	{"update_playlist", UpdatePlaylist},
	{"replace_playlist", ReplacePlaylist},
};

int CommandParserStartState_execute(struct CommandParser *parser, char const *const str, size_t const len, command_read_callback cb, size_t *chars_consumed, void *ud) {
	for (size_t i = 0; i < len;) {
		CommandParserStartState *state = &parser->stateStack[parser->stackSize-1].start;
		switch (state->step) {
		case 0: {//Just started.
			assert(!state->command);
			assert(stack_size == 0);
			state->command = malloc(sizeof *state->command);
			if (!state->command) {
				*chars_consumed = i;
				return -1; //Out of Memory error.
			}
			state->step = 1;
			break;
		}
		case 1: {//Reading command name
			if (str[i] == '\n' || str[i] == ' ') {
				for (size_t j = 0; j < NUM_COMMANDS; ++j) {
					CommandString const *command_str = &command_strings[j];
					if (state->possiblePrefixTypes[command_str->value] && strlen(command_str->value)==state->num_chars_seen) {
						state->command->type = command_str->value;
						switch (state->command->type) {
							case UpdatePlaylist:
							case ReplacePlaylist:
								//Parse new playlist
								
							break;
							case Pause:
							case Resume:
							case Halt:
								//Check that the line has ended,
								//send the output,
								//and we are done.
								if (str[i] != '\n') {
									goto unrecognised;
								}
								command_read_callback(state->command, ud);
								//clean up:
								state
							break;
						}
					}
				}
				unrecognised:
				if (state->step == 1) {
					//if nothing matched, command was unrecognised TODO.
				}
			}
			for (size_t j = 0; j < NUM_COMMANDS; ++j) {			
				if (str[i] != command_strings[j].key[state->num_chars_seen]) {
					state->possiblePrefixTypes[command_strings[j].value] = false;
				}
			}
		}
		case 2: {//Reading parsed playlist (if any).
			
		}
		}
	}
			//===========================
#if 0
			char *desired_match = NULL;
			switch (state->prefixType) {
				case AllPrefix:
				switch (str[i]) {
					case 'h': //halt
					state->prefixType = HaltPrefix;
					break;
					case 'p': //pause
					state->prefixType = PausePrefix;
					break;
					case 'r': //resume/replace_playlist
					state->prefixType = ReplaceOrResumePrefix;
					break;
					case 'u': //update_playlist
					state->prefixType = UpdatePlaylistPrefix;
					break;
					default: //unknown command
					assert("TODO: error for unknown command"); //TODO
					break;
				}
				break;
				case ReplaceOrResumePrefix:
					switch (state->num_chars_seen) {
						case 1:
						if (str[i] == 'e') {
							++state->num_chars_seen;
						}
						else {
							assert("TODO: error for unknown command");//TODO
							break;
						}
						break;
						case 2:
						switch (str[i]) {
							case 'p':
								state->prefixType = ReplacePlaylistPrefix;
							break;
							case 's':
								state->prefixType = ResumePrefix;
							break;
							default:
								assert("TODO: error for unknown command");
							break;
						}
						++state->num_chars_seen;
						break;
						default: assert(false);
						break;
					}
				break;
				case UpdatePlaylistPrefix: desired_match = "update_playlist"; break;
				case ReplacePlaylistPrefix: desired_match = "update_playlist"; break;
				case PausePrefix: desired_match = "pause"; break;
				case ResumePrefix: desired_match = "resume"; break;
				case HaltPrefix: desired_match = "halt"; break; //Perhaps use a different channel for Halt commands,
																//so that the server can't be shut down by normal commands?
			}
			if (desired_match) {
				if (str[i] == desired_match[state->num_chars_seen+1]) {
					++state->num_chars_seen;
					if (state->num_chars_seen == strlen(desired_match)) {
						
					}
				}
				else {
					assert("TODO; handle invalid commands");
				}
			}
		}
		case 2: {
			break;
		}
		}
	}
	#endif
}

int CommandParser_parse_command(struct CommandParser *parser, char const *const str, size_t const len, command_read_callback cb, size_t *chars_consumed, void *ud) {
	for (int i = 0 ; i < len;) {
		switch (parser->stateStack[parser->stackSize-1]->type) {
			case CommandParserStartState: {
				CommandParserStartState_execute(parser, str, len, cb, chars_consumed, ud);
				break;
			}
		}
	}
}
#endif /*MULTI_LINE_COMMANDS*/
