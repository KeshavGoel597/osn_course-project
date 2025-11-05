#ifndef COMMAND_PARSER_H
#define COMMAND_PARSER_H

#include "../common/protocol.h"

// Command types
typedef enum {
    CMD_VIEW,
    CMD_READ,
    CMD_CREATE,
    CMD_WRITE,
    CMD_DELETE,
    CMD_INFO,
    CMD_STREAM,
    CMD_LIST,
    CMD_ADDACCESS,
    CMD_REMACCESS,
    CMD_EXEC,
    CMD_UNDO,
    CMD_EXIT,
    CMD_HELP,
    CMD_UNKNOWN
} CommandType;

// Parsed command structure
typedef struct {
    CommandType type;
    char filename[MAX_FILENAME];
    char target_user[MAX_USERNAME];
    int access_type;       // For ADDACCESS: ACCESS_READ or ACCESS_WRITE
    int view_flags;        // For VIEW: VIEW_FLAG_ALL, VIEW_FLAG_LONG, etc.
    int sentence_index;    // For WRITE
} ParsedCommand;

// Parse a command line input
CommandType parse_command(const char *input, ParsedCommand *cmd);

// Display help message
void display_help();

#endif // COMMAND_PARSER_H
