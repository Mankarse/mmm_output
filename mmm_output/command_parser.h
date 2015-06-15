#ifndef MMM_COMMAND_PARSER_H
#define MMM_COMMAND_PARSER_H
#include "main.h"
#include "string.h"
struct Command;
//command may be `null` in the case of a parse error.
//(Or report on the problematic line?)
//In this case the parser will reset and continue from the next line.
//If the callback itself has an error, store it through `ud`, and return an error code. In this case
//CommandParserStartState_execute will immediately return, /*reporting on the number of chars consumed so far.*/
typedef int (*command_read_callback)(struct Command *command, void *ud);

struct CommandParser;
struct CommandParser *malloc_CommandParser();
struct CommandParser *new_CommandParser(command_read_callback cb, void *ud);
int create_CommandParser(struct CommandParser *uninitialisedMemory, command_read_callback cb, void *ud);
void destroy_CommandParser(struct CommandParser *commandParser);
void delete_CommandParser(struct CommandParser *commandParser);


//Returns error-code.
int CommandParser_execute(
	struct CommandParser *parser,
	char const /*restrict*/ *str,
	size_t len//,
	//size_t /*restrict*/ *chars_consumed, //TODO: For the sake of a simpler interface, perhaps report on this through a different channel, or don't report on it at all?
	//command_read_callback cb,
	//void *ud
);


#endif /*MMM_COMMAND_PARSER_H*/
