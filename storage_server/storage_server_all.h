#ifndef STORAGE_SERVER_ALL_H
#define STORAGE_SERVER_ALL_H

// Consolidated header file for all storage server modules
// Combines: storage_server.h, file_handler_ll.h, backup_handler.h, ss_client_comm.h, ss_nm_comm.h

#include "../common/protocol.h"
#include <pthread.h>
#include <sys/socket.h>
#include <errno.h>

// ============================================================================
// FILE HANDLER (Linked List Implementation) - from file_handler_ll.h
// ============================================================================

// Word node - stores a single word
typedef struct WordNode {
    char word[256];              // The actual word
    struct WordNode *next;       // Next word in the sentence
} WordNode;

// Sentence node - stores a sentence (linked list of words)
typedef struct SentenceNode {
    WordNode *words_head;           // Head of word linked list
    char delimiter;                 // '.', '!', '?', or '\0' for last/incomplete sentence
    pthread_mutex_t sentence_lock;  // Lock for this specific sentence
    int is_locked;                  // Flag to indicate if locked
    char locked_by[MAX_USERNAME];   // Username who holds the lock
    struct SentenceNode *next;      // Next sentence
} SentenceNode;

// File structure - represents entire file in memory
typedef struct LoadedFile {
    char filename[MAX_FILENAME];
    SentenceNode *sentences_head;   // Head of sentence linked list
    int sentence_count;
    int is_loaded;                  // Flag: 1 if loaded, 0 if not
    pthread_rwlock_t file_rwlock;   // Reader-writer lock for READ vs WRITE
    struct LoadedFile *next;        // For hash table chaining or linked list
} LoadedFile;

// File metadata structure (same as before, but separate from in-memory structure)
typedef struct {
    char filename[MAX_FILENAME];
    char owner[MAX_USERNAME];
    char created_time[64];
    char modified_time[64];
    char accessed_time[64];
    long file_size;
    int word_count;
    int char_count;
    char access_list[MAX_DATA_SIZE];
} FileMetadata;

// File Handler Functions
int init_file_handler_ll(const char *storage_dir);
LoadedFile* load_file_into_memory(const char *filename);
int unload_file_from_memory(const char *filename);
LoadedFile* get_file_from_cache(const char *filename);
int create_file_ll(const char *filename, const char *owner);
int delete_file_ll(const char *filename);
int read_file_ll(const char *filename, char *content, int max_size);
int write_to_file_ll(const char *filename, int sentence_index, int word_index, 
                     const char *content, const char *username);
int lock_sentence_ll(const char *filename, int sentence_index, const char *username);
int unlock_sentence_ll(const char *filename, int sentence_index, const char *username);
int force_unlock_all_sentences_ll(const char *filename);
int sync_file_to_disk(const char *filename);
int file_has_locked_sentences(const char *filename);
int read_file_from_disk_ll(const char *filename, char *content, int max_size);
int undo_file_change_ll(const char *filename);
int get_file_metadata_ll(const char *filename, FileMetadata *metadata);
int update_file_metadata_ll(const char *filename, FileMetadata *metadata);
int update_file_modified_time_ll(const char *filename);
int update_file_statistics_ll(const char *filename);
int update_file_accessed_time_ll(const char *filename, const char *username);
int has_read_access_ll(const char *filename, const char *username);
int has_write_access_ll(const char *filename, const char *username);
void get_file_path(const char *filename, char *path, size_t size);
void get_undo_path(const char *filename, char *path, size_t size);
int add_user_access_ll(const char *filename, const char *username, int access_type);
int remove_user_access_ll(const char *filename, const char *username);
int get_file_list_ll(char *file_list, int max_size);
int save_undo_backup_ll(const char *filename);
int ensure_sentence_delimiter_ll(const char *filename, int sentence_index);
int load_metadata_ll();
int save_metadata_ll();
int add_metadata_ll(FileMetadata *metadata);
void get_timestamp(char *buffer, size_t size);
void cleanup_file_handler_ll();

// ============================================================================
// BACKUP HANDLER - from backup_handler.h
// ============================================================================

// Asynchronous Replication Queue Types
#define REPLICATION_QUEUE_SIZE 1000

typedef enum {
    REP_OP_CREATE,
    REP_OP_DELETE,
    REP_OP_SYNC,
    REP_OP_METADATA
} ReplicationOpType;

typedef struct {
    ReplicationOpType op_type;
    char filename[MAX_FILENAME];
    char owner[MAX_USERNAME];
} ReplicationTask;

typedef struct {
    ReplicationTask tasks[REPLICATION_QUEUE_SIZE];
    int head;
    int tail;
    int count;
    pthread_mutex_t queue_mutex;
    pthread_cond_t queue_not_empty;
    pthread_cond_t queue_not_full;
    int running;
} ReplicationQueue;

// Backup Handler Functions
int init_backup_handler();
int init_async_replication();
void* async_replication_worker(void *arg);
int enqueue_replication_task(ReplicationOpType op_type, const char *filename, const char *owner);
int connect_to_backup_server(const char *backup_ip, int backup_port);
int replicate_create(const char *filename, const char *owner);
int replicate_delete(const char *filename);
int replicate_sync(const char *filename);
int handle_backup_request(int sockfd);
int send_file_to_backup(const char *filename);
int receive_file_from_primary(const char *filename, const char *owner);
int is_backup_available();
int handle_nm_backup_info(const char *backup_ip, int backup_port);
int perform_bulk_sync();
int perform_bulk_sync_to_backup();
int request_recovery_sync_from_backup(const char *backup_ip, int backup_port);
int replicate_metadata();
int send_bulk_file(const char *filename, int is_undo_file);
int receive_bulk_sync(int sockfd);
void cleanup_backup_handler();

// ============================================================================
// STORAGE SERVER CLIENT COMMUNICATION - from ss_client_comm.h
// ============================================================================

// Client Communication Functions
void* handle_client_connection(void *arg);
int handle_read_request(int client_sockfd, Message *msg);
int handle_write_request(int client_sockfd, Message *msg);
int handle_stream_request(int client_sockfd, Message *msg);

// ============================================================================
// STORAGE SERVER NAME MANAGER COMMUNICATION - from ss_nm_comm.h
// ============================================================================

// Name Manager Communication Functions
int register_with_nm(int nm_port, int client_port, const char *ss_ip);
void* handle_nm_connection(void *arg);
int send_ack_to_nm(int nm_sockfd, int operation, int error_code);

// ============================================================================
// STORAGE SERVER MAIN - from storage_server.h
// ============================================================================

// Global configuration
typedef struct {
    int nm_port;           // Port for Name Server connections
    int client_port;       // Port for Client connections
    char storage_dir[MAX_PATH];  // Directory to store files
    char ss_ip[MAX_IP_LEN];      // Storage Server IP
    int nm_sockfd;         // Socket for NM connection
    int client_sockfd;     // Socket for client connections
    
    // Backup/Replication configuration
    int ss_id;             // Storage Server ID (1, 2, 3, ...)
    int is_primary;        // 1 if odd (primary), 0 if even (backup)
    int is_acting_primary; // 1 if backup is acting as primary (after failover)
    char backup_ip[MAX_IP_LEN];  // Backup server IP
    int backup_port;       // Backup server port for replication
    int backup_sockfd;     // Socket for backup connection (-1 if not connected)
    int bulk_sync_complete; // 1 if initial bulk sync is done
} SSConfig;

// Global server configuration
extern SSConfig server_config;

// Global replication queue for asynchronous backups
extern ReplicationQueue replication_queue;

// Thread argument structure
typedef struct {
    int sockfd;
    char client_ip[MAX_IP_LEN];
} ThreadArg;

// Storage Server Main Functions
int init_storage_server(int nm_port, int client_port, const char *storage_dir);
int start_server();
void* handle_nm_connections(void *arg);
void* handle_client_connections(void *arg);
void shutdown_server();
void signal_handler(int signum);

#endif // STORAGE_SERVER_ALL_H
