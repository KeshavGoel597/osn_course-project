#include "command_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// Helper to trim whitespace
static char* trim(char *str) {
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return str;
    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return str;
}

// Parse a command line input
CommandType parse_command(const char *input, ParsedCommand *cmd) {
    if (input == NULL || cmd == NULL) {
        return CMD_UNKNOWN;
    }
    
    // Clear the command structure
    memset(cmd, 0, sizeof(ParsedCommand));
    
    // Copy input for parsing
    char buffer[1024];
    strncpy(buffer, input, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';
    
    // Tokenize
    char *token = strtok(buffer, " \t\n");
    if (token == NULL) {
        return CMD_UNKNOWN;
    }
    
    // Convert to uppercase for comparison
    char command[64];
    strncpy(command, token, sizeof(command) - 1);
    for (int i = 0; command[i]; i++) {
        command[i] = toupper(command[i]);
    }
    
    // Parse VIEW command
    if (strcmp(command, "VIEW") == 0) {
        cmd->type = CMD_VIEW;
        cmd->view_flags = VIEW_FLAG_NONE;
        
        // Check for flags
        token = strtok(NULL, " \t\n");
        if (token != NULL && token[0] == '-') {
            if (strstr(token, "a") && strstr(token, "l")) {
                cmd->view_flags = VIEW_FLAG_ALL_LONG;
            } else if (strstr(token, "a")) {
                cmd->view_flags = VIEW_FLAG_ALL;
            } else if (strstr(token, "l")) {
                cmd->view_flags = VIEW_FLAG_LONG;
            }
        }
        return CMD_VIEW;
    }
    
    // Parse READ command
    if (strcmp(command, "READ") == 0) {
        cmd->type = CMD_READ;
        token = strtok(NULL, " \t\n");
        if (token != NULL) {
            strncpy(cmd->filename, token, MAX_FILENAME - 1);
            return CMD_READ;
        }
        return CMD_UNKNOWN;
    }
    
    // Parse CREATE command
    if (strcmp(command, "CREATE") == 0) {
        cmd->type = CMD_CREATE;
        token = strtok(NULL, " \t\n");
        if (token != NULL) {
            strncpy(cmd->filename, token, MAX_FILENAME - 1);
            return CMD_CREATE;
        }
        return CMD_UNKNOWN;
    }
    
    // Parse WRITE command
    if (strcmp(command, "WRITE") == 0) {
        cmd->type = CMD_WRITE;
        token = strtok(NULL, " \t\n");
        if (token != NULL) {
            strncpy(cmd->filename, token, MAX_FILENAME - 1);
            token = strtok(NULL, " \t\n");
            if (token != NULL) {
                cmd->sentence_index = atoi(token);
                return CMD_WRITE;
            }
        }
        return CMD_UNKNOWN;
    }
    
    // Parse DELETE command
    if (strcmp(command, "DELETE") == 0) {
        cmd->type = CMD_DELETE;
        token = strtok(NULL, " \t\n");
        if (token != NULL) {
            strncpy(cmd->filename, token, MAX_FILENAME - 1);
            return CMD_DELETE;
        }
        return CMD_UNKNOWN;
    }
    
    // Parse INFO command
    if (strcmp(command, "INFO") == 0) {
        cmd->type = CMD_INFO;
        token = strtok(NULL, " \t\n");
        if (token != NULL) {
            strncpy(cmd->filename, token, MAX_FILENAME - 1);
            return CMD_INFO;
        }
        return CMD_UNKNOWN;
    }
    
    // Parse STREAM command
    if (strcmp(command, "STREAM") == 0) {
        cmd->type = CMD_STREAM;
        token = strtok(NULL, " \t\n");
        if (token != NULL) {
            strncpy(cmd->filename, token, MAX_FILENAME - 1);
            return CMD_STREAM;
        }
        return CMD_UNKNOWN;
    }
    
    // Parse LIST command
    if (strcmp(command, "LIST") == 0) {
        cmd->type = CMD_LIST;
        return CMD_LIST;
    }
    
    // Parse ADDACCESS command
    if (strcmp(command, "ADDACCESS") == 0) {
        cmd->type = CMD_ADDACCESS;
        token = strtok(NULL, " \t\n");
        if (token != NULL && token[0] == '-') {
            if (token[1] == 'R' || token[1] == 'r') {
                cmd->access_type = ACCESS_READ;
            } else if (token[1] == 'W' || token[1] == 'w') {
                cmd->access_type = ACCESS_WRITE;
            } else {
                return CMD_UNKNOWN;
            }
            
            token = strtok(NULL, " \t\n");
            if (token != NULL) {
                strncpy(cmd->filename, token, MAX_FILENAME - 1);
                token = strtok(NULL, " \t\n");
                if (token != NULL) {
                    strncpy(cmd->target_user, token, MAX_USERNAME - 1);
                    return CMD_ADDACCESS;
                }
            }
        }
        return CMD_UNKNOWN;
    }
    
    // Parse REMACCESS command
    if (strcmp(command, "REMACCESS") == 0) {
        cmd->type = CMD_REMACCESS;
        token = strtok(NULL, " \t\n");
        if (token != NULL) {
            strncpy(cmd->filename, token, MAX_FILENAME - 1);
            token = strtok(NULL, " \t\n");
            if (token != NULL) {
                strncpy(cmd->target_user, token, MAX_USERNAME - 1);
                return CMD_REMACCESS;
            }
        }
        return CMD_UNKNOWN;
    }
    
    // Parse EXEC command
    if (strcmp(command, "EXEC") == 0) {
        cmd->type = CMD_EXEC;
        token = strtok(NULL, " \t\n");
        if (token != NULL) {
            strncpy(cmd->filename, token, MAX_FILENAME - 1);
            return CMD_EXEC;
        }
        return CMD_UNKNOWN;
    }
    
    // Parse UNDO command
    if (strcmp(command, "UNDO") == 0) {
        cmd->type = CMD_UNDO;
        token = strtok(NULL, " \t\n");
        if (token != NULL) {
            strncpy(cmd->filename, token, MAX_FILENAME - 1);
            return CMD_UNDO;
        }
        return CMD_UNKNOWN;
    }
    
    // Parse EXIT command
    if (strcmp(command, "EXIT") == 0 || strcmp(command, "QUIT") == 0) {
        cmd->type = CMD_EXIT;
        return CMD_EXIT;
    }
    
    // Parse HELP command
    if (strcmp(command, "HELP") == 0) {
        cmd->type = CMD_HELP;
        return CMD_HELP;
    }
    
    return CMD_UNKNOWN;
}

// Display help message
void display_help() {
    printf("\n=== Available Commands ===\n\n");
    printf("File Operations:\n");
    printf("  VIEW [-a] [-l] [-al]     - List files (use -a for all, -l for details)\n");
    printf("  READ <filename>          - Read and display file content\n");
    printf("  CREATE <filename>        - Create a new file\n");
    printf("  WRITE <filename> <sent#> - Write to file at sentence index\n");
    printf("  DELETE <filename>        - Delete a file\n");
    printf("  INFO <filename>          - Display file information\n");
    printf("  STREAM <filename>        - Stream file content word-by-word\n");
    printf("  UNDO <filename>          - Undo last change to file\n");
    printf("\n");
    printf("Access Control:\n");
    printf("  ADDACCESS -R <file> <user>  - Grant read access\n");
    printf("  ADDACCESS -W <file> <user>  - Grant write access\n");
    printf("  REMACCESS <file> <user>     - Remove user access\n");
    printf("\n");
    printf("Other:\n");
    printf("  LIST                     - List all users\n");
    printf("  EXEC <filename>          - Execute file as shell commands\n");
    printf("  HELP                     - Display this help message\n");
    printf("  EXIT                     - Exit the client\n");
    printf("\n");
}
