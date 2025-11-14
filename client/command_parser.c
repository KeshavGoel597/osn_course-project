#include "client.h"
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
        
        // Check for flags - FIXED: Exact match validation to reject invalid flags
        token = strtok(NULL, " \t\n");
        if (token != NULL && token[0] == '-') {
            // Only accept exact matches: -al, -la, -a, -l
            if (strcmp(token, "-al") == 0 || strcmp(token, "-la") == 0) {
                cmd->view_flags = VIEW_FLAG_ALL_LONG;
            } else if (strcmp(token, "-a") == 0) {
                cmd->view_flags = VIEW_FLAG_ALL;
            } else if (strcmp(token, "-l") == 0) {
                cmd->view_flags = VIEW_FLAG_LONG;
            } else {
                // Invalid flag - reject command
                fprintf(stderr, "Invalid flag '%s'. Valid flags: -a, -l, -al\n", token);
                return CMD_UNKNOWN;
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
    
    // Parse CREATEFOLDER command
    if (strcmp(command, "CREATEFOLDER") == 0) {
        cmd->type = CMD_CREATEFOLDER;
        token = strtok(NULL, " \t\n");
        if (token != NULL) {
            strncpy(cmd->filename, token, MAX_FILENAME - 1);
            return CMD_CREATEFOLDER;
        }
        return CMD_UNKNOWN;
    }
    
    // Parse MOVE command
    if (strcmp(command, "MOVE") == 0) {
        cmd->type = CMD_MOVE;
        token = strtok(NULL, " \t\n");
        if (token != NULL) {
            strncpy(cmd->filename, token, MAX_FILENAME - 1);
            token = strtok(NULL, " \t\n");
            if (token != NULL) {
                strncpy(cmd->target_path, token, MAX_PATH - 1);
                return CMD_MOVE;
            }
        }
        return CMD_UNKNOWN;
    }
    
    // Parse VIEWFOLDER command
    if (strcmp(command, "VIEWFOLDER") == 0) {
        cmd->type = CMD_VIEWFOLDER;
        token = strtok(NULL, " \t\n");
        if (token != NULL) {
            strncpy(cmd->filename, token, MAX_FILENAME - 1);
            return CMD_VIEWFOLDER;
        }
        return CMD_UNKNOWN;
    }
    
    // Parse CHECKPOINT command
    if (strcmp(command, "CHECKPOINT") == 0) {
        cmd->type = CMD_CHECKPOINT;
        token = strtok(NULL, " \t\n");
        if (token != NULL) {
            strncpy(cmd->filename, token, MAX_FILENAME - 1);
            token = strtok(NULL, " \t\n");
            if (token != NULL) {
                strncpy(cmd->checkpoint_tag, token, MAX_FILENAME - 1);
                return CMD_CHECKPOINT;
            }
        }
        return CMD_UNKNOWN;
    }
    
    // Parse VIEWCHECKPOINT command
    if (strcmp(command, "VIEWCHECKPOINT") == 0) {
        cmd->type = CMD_VIEWCHECKPOINT;
        token = strtok(NULL, " \t\n");
        if (token != NULL) {
            strncpy(cmd->filename, token, MAX_FILENAME - 1);
            token = strtok(NULL, " \t\n");
            if (token != NULL) {
                strncpy(cmd->checkpoint_tag, token, MAX_FILENAME - 1);
                return CMD_VIEWCHECKPOINT;
            }
        }
        return CMD_UNKNOWN;
    }
    
    // Parse REVERT command
    if (strcmp(command, "REVERT") == 0) {
        cmd->type = CMD_REVERT;
        token = strtok(NULL, " \t\n");
        if (token != NULL) {
            strncpy(cmd->filename, token, MAX_FILENAME - 1);
            token = strtok(NULL, " \t\n");
            if (token != NULL) {
                strncpy(cmd->checkpoint_tag, token, MAX_FILENAME - 1);
                return CMD_REVERT;
            }
        }
        return CMD_UNKNOWN;
    }
    
    // Parse LISTCHECKPOINTS command
    if (strcmp(command, "LISTCHECKPOINTS") == 0) {
        cmd->type = CMD_LISTCHECKPOINTS;
        token = strtok(NULL, " \t\n");
        if (token != NULL) {
            strncpy(cmd->filename, token, MAX_FILENAME - 1);
            return CMD_LISTCHECKPOINTS;
        }
        return CMD_UNKNOWN;
    }
    
    // Parse REQUESTACCESS command
    if (strcmp(command, "REQUESTACCESS") == 0) {
        cmd->type = CMD_REQUESTACCESS;
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
                return CMD_REQUESTACCESS;
            }
        }
        return CMD_UNKNOWN;
    }
    
    // Parse VIEWREQUESTS command
    if (strcmp(command, "VIEWREQUESTS") == 0) {
        cmd->type = CMD_VIEWREQUESTS;
        return CMD_VIEWREQUESTS;
    }
    
    // Parse APPROVEREQUEST command
    if (strcmp(command, "APPROVEREQUEST") == 0) {
        cmd->type = CMD_APPROVEREQUEST;
        token = strtok(NULL, " \t\n");
        if (token != NULL) {
            strncpy(cmd->request_id, token, MAX_FILENAME - 1);
            return CMD_APPROVEREQUEST;
        }
        return CMD_UNKNOWN;
    }
    
    // Parse REJECTREQUEST command
    if (strcmp(command, "REJECTREQUEST") == 0) {
        cmd->type = CMD_REJECTREQUEST;
        token = strtok(NULL, " \t\n");
        if (token != NULL) {
            strncpy(cmd->request_id, token, MAX_FILENAME - 1);
            return CMD_REJECTREQUEST;
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
    printf("  CREATEFOLDER <foldername> - Create a new folder\n");
    printf("  MOVE <filename> <path>   - Move file to new location\n");
    printf("  VIEWFOLDER <foldername>  - View contents of a folder\n");
    printf("  CHECKPOINT <filename> <tag> - Create a checkpoint for a file\n");
    printf("  VIEWCHECKPOINT <filename> <tag> - View a specific checkpoint\n");
    printf("  REVERT <filename> <tag>  - Revert file to a checkpoint\n");
    printf("\n");
    printf("Access Control:\n");
    printf("  ADDACCESS -R <file> <user>  - Grant read access\n");
    printf("  ADDACCESS -W <file> <user>  - Grant write access\n");
    printf("  REMACCESS <file> <user>     - Remove user access\n");
    printf("  REQUESTACCESS -R <file>     - Request read access\n");
    printf("  REQUESTACCESS -W <file>     - Request write access\n");
    printf("  VIEWREQUESTS                - View access requests\n");
    printf("  APPROVEREQUEST <request_id> - Approve an access request\n");
    printf("  REJECTREQUEST <request_id>  - Reject an access request\n");
    printf("\n");
    printf("Other:\n");
    printf("  LIST                     - List all users\n");
    printf("  EXEC <filename>          - Execute file as shell commands\n");
    printf("  HELP                     - Display this help message\n");
    printf("  EXIT                     - Exit the client\n");
    printf("\n");
}
