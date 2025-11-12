#ifndef BACKUP_HANDLER_H
#define BACKUP_HANDLER_H

#include "../common/protocol.h"
#include <pthread.h>

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

/**
 * Initialize backup handler
 * Sets up connection to backup server if this is a primary server (odd ID)
 * Or sets up listening for primary server if this is a backup server (even ID)
 */
int init_backup_handler();

/**
 * Initialize asynchronous replication queue and worker thread
 * Returns 0 on success, -1 on failure
 */
int init_async_replication();

/**
 * Asynchronous replication worker thread
 * Processes tasks from replication queue
 */
void* async_replication_worker(void *arg);

/**
 * Enqueue a replication task (non-blocking, asynchronous)
 * Returns immediately without waiting for ACK
 */
int enqueue_replication_task(ReplicationOpType op_type, const char *filename, const char *owner);

/**
 * Connect to backup server (called by primary server)
 * Returns 0 on success, -1 on failure
 */
int connect_to_backup_server(const char *backup_ip, int backup_port);

/**
 * Replicate file creation to backup server
 * Called after successful file creation on primary server
 */
int replicate_create(const char *filename, const char *owner);

/**
 * Replicate file deletion to backup server
 * Called after successful file deletion on primary server
 */
int replicate_delete(const char *filename);

/**
 * Replicate file content to backup server
 * Called after successful write operation on primary server
 */
int replicate_sync(const char *filename);

/**
 * Handle backup request from primary server
 * Called by backup server when receiving replication request
 */
int handle_backup_request(int sockfd);

/**
 * Send file content to backup server
 * Helper function to transfer entire file
 */
int send_file_to_backup(const char *filename);

/**
 * Receive file content from primary server
 * Helper function to receive and store file
 */
int receive_file_from_primary(const char *filename, const char *owner);

/**
 * Check if backup server is available
 * Returns 1 if connected, 0 otherwise
 */
int is_backup_available();

/**
 * Handle backup info from Name Server
 * Called when NM sends OP_NM_BACKUP_INFO with backup server details
 */
int handle_nm_backup_info(const char *backup_ip, int backup_port);

/**
 * Perform bulk sync to backup server
 * Syncs all files, undo files, and metadata
 */
int perform_bulk_sync();

/**
 * Request recovery sync from backup server (primary SS that was offline)
 * This is called when a primary server comes back online after failure
 * It requests all files from its backup to catch up on changes
 */
int request_recovery_sync_from_backup();

/**
 * Replicate metadata.txt to backup server
 * Called when metadata changes (ADDACCESS, REMACCESS, etc.)
 */
int replicate_metadata();

/**
 * Send a single file to backup during bulk sync
 */
int send_bulk_file(const char *filename, int is_undo_file);

/**
 * Receive bulk sync from primary (backup server side)
 */
int receive_bulk_sync(int sockfd);

/**
 * Cleanup backup handler
 * Close backup connections
 */
void cleanup_backup_handler();

#endif // BACKUP_HANDLER_H
