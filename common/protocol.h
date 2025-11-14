#ifndef PROTOCOL_H
#define PROTOCOL_H

// Maximum size limits
#define MAX_FILENAME 256
#define MAX_USERNAME 64
#define MAX_DATA_SIZE 4096
#define MAX_IP_LEN 16
#define MAX_PATH 1024       // Increased from 512 to avoid truncation warnings

// Name Server connection details (hardcoded as per requirements)
#define NM_IP "127.0.0.1"
#define NM_PORT 8080

// Message Types
#define MSG_REQUEST 1
#define MSG_RESPONSE 2
#define MSG_ACK 3
#define MSG_ERROR 4

// Operation Types
// Client Operations
#define OP_CREATE 100
#define OP_READ 101
#define OP_WRITE 102
#define OP_DELETE 103
#define OP_INFO 104
#define OP_STREAM 105
#define OP_LIST 106
#define OP_VIEW 107
#define OP_ADDACCESS 108
#define OP_REMACCESS 109
#define OP_EXEC 110
#define OP_UNDO 111
#define OP_CREATEFOLDER 112
#define OP_MOVE 113
#define OP_VIEWFOLDER 114
#define OP_CHECKPOINT 115
#define OP_VIEWCHECKPOINT 116
#define OP_REVERT 117
#define OP_LISTCHECKPOINTS 118
#define OP_REQUESTACCESS 119
#define OP_VIEWREQUESTS 120
#define OP_APPROVEREQUEST 121
#define OP_REJECTREQUEST 122

// Registration Operations
#define OP_SS_REGISTER 200
#define OP_CLIENT_REGISTER 201

// Internal Operations
#define OP_GET_SS_INFO 300      // Client asks NM for SS details
#define OP_SS_CREATE_FILE 301   // NM instructs SS to create file
#define OP_SS_DELETE_FILE 302   // NM instructs SS to delete file
#define OP_STREAM_WORD 303      // SS sends individual word during streaming
#define OP_STOP 304             // Signal to stop streaming or operation
#define OP_NM_BACKUP_INFO 305   // NM sends backup server info to primary SS
#define OP_SS_ADDACCESS 306     // NM instructs SS to add access permissions
#define OP_SS_REMACCESS 307     // NM instructs SS to remove access permissions
#define OP_READ_CHUNK 308       // SS sends file chunk during chunked READ
#define OP_EXEC_CHUNK 309       // SS sends script chunk during chunked EXEC

// Backup/Replication Operations
#define OP_BACKUP_CREATE 400    // Primary SS instructs backup SS to create file
#define OP_BACKUP_DELETE 401    // Primary SS instructs backup SS to delete file
#define OP_BACKUP_SYNC 402      // Primary SS syncs file content to backup SS
#define OP_BACKUP_REGISTER 403  // Backup SS registers with primary SS
#define OP_BACKUP_METADATA 404  // Primary SS syncs metadata.txt to backup SS
#define OP_BACKUP_INIT_SYNC 405 // Primary SS starts bulk sync to backup SS
#define OP_BACKUP_FILE 406      // Primary SS sends a file during bulk sync
#define OP_BACKUP_UNDO_FILE 407 // Primary SS sends an undo file during bulk sync
#define OP_BACKUP_SYNC_COMPLETE 408 // Primary SS completes bulk sync
#define OP_RECOVERY_SYNC 409    // NM instructs recovering primary to sync from backup

// Error Codes
#define ERR_SUCCESS 0
#define ERR_FILE_NOT_FOUND 1001
#define ERR_ACCESS_DENIED 1002
#define ERR_SENTENCE_LOCKED 1003
#define ERR_INVALID_INDEX 1004
#define ERR_FILE_EXISTS 1005
#define ERR_NOT_OWNER 1006
#define ERR_NO_WRITE_ACCESS 1007
#define ERR_NO_READ_ACCESS 1008
#define ERR_SENTENCE_OUT_OF_RANGE 1009
#define ERR_WORD_OUT_OF_RANGE 1010
#define ERR_INVALID_OPERATION 1011
#define ERR_SERVER_ERROR 1012
#define ERR_CONNECTION_FAILED 1013
#define ERR_INVALID_COMMAND 1014
#define ERR_USER_NOT_FOUND 1015

// Access Types
#define ACCESS_READ 1
#define ACCESS_WRITE 2
#define ACCESS_READ_WRITE 3

// View Flags
#define VIEW_FLAG_NONE 0
#define VIEW_FLAG_ALL 1      // -a flag
#define VIEW_FLAG_LONG 2     // -l flag
#define VIEW_FLAG_ALL_LONG 3 // -al flag

// Message structure
typedef struct {
    int msg_type;        // MSG_REQUEST, MSG_RESPONSE, MSG_ACK, MSG_ERROR
    int operation;       // OP_CREATE, OP_READ, etc.
    int error_code;      // ERR_FILE_NOT_FOUND, etc.

    char username[MAX_USERNAME];
    char filename[MAX_FILENAME];
    char checkpoint_tag[MAX_FILENAME];  // For checkpoint operations
    char target_path[MAX_PATH];         // For MOVE and folder operations
    
    // For WRITE operation
    int sentence_index;
    int word_index;

    // For Registration (both SS and Client)
    char ip[MAX_IP_LEN]; 
    int port1;  // SS: port for NM connection | Client: port for NM connection
    int port2;  // SS: port for Client connection | Client: port for SS connection

    // For Backup/Replication
    int ss_id;          // Storage Server ID (1, 2, 3, 4, ...) for backup pairing
    char backup_ip[MAX_IP_LEN];  // Backup server IP
    int backup_port;    // Backup server port for replication

    // For WRITE content, READ response, file lists, etc.
    char data[MAX_DATA_SIZE]; 
} Message;

#endif // PROTOCOL_H
