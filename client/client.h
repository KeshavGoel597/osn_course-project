#ifndef CLIENT_H
#define CLIENT_H

#include "../common/protocol.h"
#include <pthread.h>

// Client configuration
typedef struct {
    char username[MAX_USERNAME];
    char nm_ip[MAX_IP_LEN];      // Name Server IP address
    char client_ip[MAX_IP_LEN];
    int nm_port;           // Port for Name Server connection
    int ss_port;           // Port for Storage Server connection
    int nm_sockfd;         // Socket for NM connection (may be used multiple times)
} ClientConfig;

// Global client configuration
extern ClientConfig client_config;

// =============================================================================
// COMMAND PARSER TYPES AND FUNCTIONS
// =============================================================================

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
    CMD_CREATEFOLDER,
    CMD_MOVE,
    CMD_VIEWFOLDER,
    CMD_CHECKPOINT,
    CMD_VIEWCHECKPOINT,
    CMD_REVERT,
    CMD_LISTCHECKPOINTS,
    CMD_REQUESTACCESS,
    CMD_VIEWREQUESTS,
    CMD_APPROVEREQUEST,
    CMD_REJECTREQUEST,
    CMD_EXIT,
    CMD_HELP,
    CMD_UNKNOWN
} CommandType;

// Parsed command structure
typedef struct {
    CommandType type;
    char filename[MAX_FILENAME];
    char target_user[MAX_USERNAME];
    char checkpoint_tag[MAX_FILENAME];  // For checkpoint operations
    char target_path[MAX_PATH];         // For MOVE and folder operations
    char request_id[MAX_FILENAME];      // For access request operations
    int access_type;       // For ADDACCESS: ACCESS_READ or ACCESS_WRITE
    int view_flags;        // For VIEW: VIEW_FLAG_ALL, VIEW_FLAG_LONG, etc.
    int sentence_index;    // For WRITE
} ParsedCommand;

// Parse a command line input
CommandType parse_command(const char *input, ParsedCommand *cmd);

// Display help message
void display_help();

// =============================================================================
// CLIENT CORE FUNCTIONS
// =============================================================================

// Initialize client
int init_client(const char *username);

// Register with Name Server
int register_with_nm();

// Start client interactive shell
void start_client_shell();

// Cleanup client
void cleanup_client();

// =============================================================================
// NAME SERVER COMMUNICATION FUNCTIONS
// =============================================================================

// Send VIEW request to Name Server
int send_view_request(int view_flags);

// Send LIST request to Name Server
int send_list_request();

// Send INFO request to Name Server
int send_info_request(const char *filename);

// Send ADDACCESS request to Name Server
int send_addaccess_request(const char *filename, const char *target_user, int access_type);

// Send REMACCESS request to Name Server
int send_remaccess_request(const char *filename, const char *target_user);

// Send CREATE request to Name Server
int send_create_request(const char *filename);

// Send DELETE request to Name Server
int send_delete_request(const char *filename);

// Send EXEC request to Name Server
int send_exec_request(const char *filename);

// Get Storage Server info for a file (for direct operations)
int get_ss_info(const char *filename, char *ss_ip, int *ss_port);

// =============================================================================
// STORAGE SERVER COMMUNICATION FUNCTIONS
// =============================================================================

// Send READ request directly to Storage Server
int send_read_request(const char *filename);

// Send WRITE request directly to Storage Server
int send_write_request(const char *filename, int sentence_index);

// Send STREAM request directly to Storage Server
int send_stream_request(const char *filename);

// Send UNDO request (through Name Server, but may involve SS)
int send_undo_request(const char *filename);

// Hierarchical folder operations
int send_createfolder_request(const char *foldername);
int send_move_request(const char *filename, const char *target_folder);
int send_viewfolder_request(const char *foldername);

// Checkpoint operations
int send_checkpoint_request(const char *filename, const char *checkpoint_tag);
int send_viewcheckpoint_request(const char *filename, const char *checkpoint_tag);
int send_revert_request(const char *filename, const char *checkpoint_tag);
int send_listcheckpoints_request(const char *filename);

// Access request operations
int send_requestaccess_request(const char *filename, int access_type);
int send_viewrequests_request();
int send_approverequest_request(const char *request_id);
int send_rejectrequest_request(const char *request_id);

#endif // CLIENT_H
