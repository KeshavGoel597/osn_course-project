#include "storage_server_all.h"
#include "../common/network_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>

// Mutex for backup operations (synchronous operations only)
static pthread_mutex_t backup_mutex = PTHREAD_MUTEX_INITIALIZER;

// Replication worker thread
static pthread_t replication_thread;

// ============================================================================
// ASYNCHRONOUS REPLICATION IMPLEMENTATION
// ============================================================================

// Initialize asynchronous replication queue and worker thread
int init_async_replication() {
    memset(&replication_queue, 0, sizeof(ReplicationQueue));
    
    pthread_mutex_init(&replication_queue.queue_mutex, NULL);
    pthread_cond_init(&replication_queue.queue_not_empty, NULL);
    pthread_cond_init(&replication_queue.queue_not_full, NULL);
    
    replication_queue.head = 0;
    replication_queue.tail = 0;
    replication_queue.count = 0;
    replication_queue.running = 1;
    
    // Start replication worker thread
    if (pthread_create(&replication_thread, NULL, async_replication_worker, NULL) != 0) {
        fprintf(stderr, "[Async Replication] Failed to create worker thread\n");
        return -1;
    }
    
    printf("[Async Replication] Worker thread started\n");
    return 0;
}

// Enqueue a replication task (non-blocking, asynchronous)
int enqueue_replication_task(ReplicationOpType op_type, const char *filename, const char *owner) {
    if (!server_config.is_primary || server_config.backup_sockfd < 0) {
        // Not a primary server or backup not available
        return 0;
    }
    
    // CRITICAL FIX: Verify backup connection is actually healthy
    // Use MSG_PEEK to check if socket is still valid without consuming data
    char probe_byte;
    int result = recv(server_config.backup_sockfd, &probe_byte, 1, MSG_PEEK | MSG_DONTWAIT);
    if (result == 0) {
        // Connection closed by backup server
        fprintf(stderr, "[Async Replication] Backup connection closed, skipping enqueue\n");
        server_config.backup_sockfd = -1;
        return -1;
    } else if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        // Socket error (not just empty buffer)
        fprintf(stderr, "[Async Replication] Backup socket error: %s, skipping enqueue\n", strerror(errno));
        server_config.backup_sockfd = -1;
        return -1;
    }
    // If EAGAIN/EWOULDBLOCK or result > 0, socket is healthy
    
    pthread_mutex_lock(&replication_queue.queue_mutex);
    
    // Check if queue is full
    while (replication_queue.count >= REPLICATION_QUEUE_SIZE && replication_queue.running) {
        fprintf(stderr, "[Async Replication] Queue full, waiting...\n");
        pthread_cond_wait(&replication_queue.queue_not_full, &replication_queue.queue_mutex);
    }
    
    if (!replication_queue.running) {
        pthread_mutex_unlock(&replication_queue.queue_mutex);
        return -1;
    }
    
    // Add task to queue
    ReplicationTask *task = &replication_queue.tasks[replication_queue.tail];
    task->op_type = op_type;
    strncpy(task->filename, filename, MAX_FILENAME - 1);
    task->filename[MAX_FILENAME - 1] = '\0';
    
    if (owner) {
        strncpy(task->owner, owner, MAX_USERNAME - 1);
        task->owner[MAX_USERNAME - 1] = '\0';
    } else {
        task->owner[0] = '\0';
    }
    
    replication_queue.tail = (replication_queue.tail + 1) % REPLICATION_QUEUE_SIZE;
    replication_queue.count++;
    
    // Signal worker thread
    pthread_cond_signal(&replication_queue.queue_not_empty);
    pthread_mutex_unlock(&replication_queue.queue_mutex);
    
    printf("[Async Replication] Enqueued %s for file '%s' (queue size: %d)\n",
           op_type == REP_OP_CREATE ? "CREATE" :
           op_type == REP_OP_DELETE ? "DELETE" :
           op_type == REP_OP_SYNC ? "SYNC" :
           op_type == REP_OP_UNDO_BACKUP ? "UNDO_BACKUP" : "METADATA",
           filename, replication_queue.count);
    
    return 0;  // Return immediately without waiting for ACK
}

// Asynchronous replication worker thread
void* async_replication_worker(void *arg) {
    (void)arg;
    printf("[Async Replication] Worker thread running\n");
    
    while (1) {
        pthread_mutex_lock(&replication_queue.queue_mutex);
        
        // Wait for tasks
        while (replication_queue.count == 0 && replication_queue.running) {
            pthread_cond_wait(&replication_queue.queue_not_empty, &replication_queue.queue_mutex);
        }
        
        if (!replication_queue.running && replication_queue.count == 0) {
            pthread_mutex_unlock(&replication_queue.queue_mutex);
            break;
        }
        
        // Get task from queue
        ReplicationTask task = replication_queue.tasks[replication_queue.head];
        replication_queue.head = (replication_queue.head + 1) % REPLICATION_QUEUE_SIZE;
        replication_queue.count--;
        
        // Signal that queue has space
        pthread_cond_signal(&replication_queue.queue_not_full);
        pthread_mutex_unlock(&replication_queue.queue_mutex);
        
        // Process task (outside mutex to allow concurrent enqueuing)
        printf("[Async Replication] Processing task: %s for '%s'\n",
               task.op_type == REP_OP_CREATE ? "CREATE" :
               task.op_type == REP_OP_DELETE ? "DELETE" :
               task.op_type == REP_OP_SYNC ? "SYNC" :
               task.op_type == REP_OP_UNDO_BACKUP ? "UNDO_BACKUP" : "METADATA",
               task.filename);
        
        switch (task.op_type) {
            case REP_OP_CREATE:
                replicate_create(task.filename, task.owner);
                break;
            case REP_OP_DELETE:
                replicate_delete(task.filename);
                break;
            case REP_OP_SYNC:
                replicate_sync(task.filename);
                break;
            case REP_OP_METADATA:
                replicate_metadata();
                break;
            case REP_OP_UNDO_BACKUP:
                replicate_undo_backup(task.filename);
                break;
        }
    }
    
    printf("[Async Replication] Worker thread exiting\n");
    return NULL;
}

// ============================================================================
// ORIGINAL SYNCHRONOUS BACKUP FUNCTIONS (now called by async worker)
// ============================================================================

// Initialize backup handler
int init_backup_handler() {
    // Backup connection will be established when NM provides backup server info
    server_config.backup_sockfd = -1;
    
    printf("[Backup Handler] Initialized (SS_ID=%d, %s)\n", 
           server_config.ss_id, 
           server_config.is_primary ? "PRIMARY" : "BACKUP");
    
    return 0;
}

// Connect to backup server (called by primary server)
int connect_to_backup_server(const char *backup_ip, int backup_port) {
    if (!server_config.is_primary) {
        printf("[Backup Handler] Not a primary server, skipping backup connection\n");
        return 0;
    }
    
    pthread_mutex_lock(&backup_mutex);
    
    // If already connected, close old connection
    if (server_config.backup_sockfd > 0) {
        close(server_config.backup_sockfd);
    }
    
    printf("[Backup Handler] Connecting to backup server at %s:%d\n", backup_ip, backup_port);
    
    // Connect to backup server (creates socket internally)
    server_config.backup_sockfd = connect_to_server(backup_ip, backup_port);
    if (server_config.backup_sockfd < 0) {
        fprintf(stderr, "[Backup Handler] Failed to connect to backup server\n");
        server_config.backup_sockfd = -1;
        pthread_mutex_unlock(&backup_mutex);
        return -1;
    }
    
    // Store backup server info
    strncpy(server_config.backup_ip, backup_ip, MAX_IP_LEN - 1);
    server_config.backup_port = backup_port;
    
    printf("[Backup Handler] Successfully connected to backup server\n");
    pthread_mutex_unlock(&backup_mutex);
    
    return 0;
}

// Replicate file creation to backup server
int replicate_create(const char *filename, const char *owner) {
    if (!server_config.is_primary || server_config.backup_sockfd < 0) {
        // Not a primary server or backup not available
        return 0;
    }
    
    printf("[Backup Handler] Replicating CREATE for file: %s\n", filename);
    
    pthread_mutex_lock(&backup_mutex);
    
    // Prepare message
    Message msg;
    memset(&msg, 0, sizeof(Message));
    msg.msg_type = MSG_REQUEST;
    msg.operation = OP_BACKUP_CREATE;
    strncpy(msg.filename, filename, MAX_FILENAME - 1);
    strncpy(msg.username, owner, MAX_USERNAME - 1);
    
    // Send to backup
    if (send_message(server_config.backup_sockfd, &msg) < 0) {
        fprintf(stderr, "[Backup Handler] Failed to send CREATE to backup\n");
        pthread_mutex_unlock(&backup_mutex);
        return -1;
    }
    
    // Wait for acknowledgment
    Message response;
    if (receive_message(server_config.backup_sockfd, &response) < 0) {
        fprintf(stderr, "[Backup Handler] Failed to receive ACK from backup\n");
        pthread_mutex_unlock(&backup_mutex);
        return -1;
    }
    
    if (response.msg_type == MSG_ERROR) {
        fprintf(stderr, "[Backup Handler] Backup server returned error: %d\n", response.error_code);
        pthread_mutex_unlock(&backup_mutex);
        return -1;
    }
    
    // Now send the file content
    send_file_to_backup(filename);
    
    printf("[Backup Handler] File creation replicated successfully\n");
    pthread_mutex_unlock(&backup_mutex);
    
    return 0;
}

// Replicate file deletion to backup server
int replicate_delete(const char *filename) {
    if (!server_config.is_primary || server_config.backup_sockfd < 0) {
        return 0;
    }
    
    printf("[Backup Handler] Replicating DELETE for file: %s\n", filename);
    
    pthread_mutex_lock(&backup_mutex);
    
    Message msg;
    memset(&msg, 0, sizeof(Message));
    msg.msg_type = MSG_REQUEST;
    msg.operation = OP_BACKUP_DELETE;
    strncpy(msg.filename, filename, MAX_FILENAME - 1);
    
    if (send_message(server_config.backup_sockfd, &msg) < 0) {
        fprintf(stderr, "[Backup Handler] Failed to send DELETE to backup\n");
        pthread_mutex_unlock(&backup_mutex);
        return -1;
    }
    
    // Wait for acknowledgment
    Message response;
    if (receive_message(server_config.backup_sockfd, &response) < 0) {
        fprintf(stderr, "[Backup Handler] Failed to receive ACK from backup\n");
        pthread_mutex_unlock(&backup_mutex);
        return -1;
    }
    
    printf("[Backup Handler] File deletion replicated successfully\n");
    pthread_mutex_unlock(&backup_mutex);
    
    return 0;
}

// Replicate file content to backup server
int replicate_sync(const char *filename) {
    if (!server_config.is_primary || server_config.backup_sockfd < 0) {
        return 0;
    }
    
    printf("[Backup Handler] Replicating SYNC for file: %s\n", filename);
    
    pthread_mutex_lock(&backup_mutex);
    
    Message msg;
    memset(&msg, 0, sizeof(Message));
    msg.msg_type = MSG_REQUEST;
    msg.operation = OP_BACKUP_SYNC;
    strncpy(msg.filename, filename, MAX_FILENAME - 1);
    
    if (send_message(server_config.backup_sockfd, &msg) < 0) {
        fprintf(stderr, "[Backup Handler] Failed to send SYNC to backup\n");
        pthread_mutex_unlock(&backup_mutex);
        return -1;
    }
    
    // Send the file content
    int result = send_file_to_backup(filename);
    
    // Wait for acknowledgment
    Message response;
    if (receive_message(server_config.backup_sockfd, &response) < 0) {
        fprintf(stderr, "[Backup Handler] Failed to receive ACK from backup\n");
        pthread_mutex_unlock(&backup_mutex);
        return -1;
    }
    
    printf("[Backup Handler] File sync replicated successfully\n");
    pthread_mutex_unlock(&backup_mutex);
    
    return result;
}

// Replicate undo backup file to backup server
// CRITICAL FIX: Ensures UNDO works even after server failover
int replicate_undo_backup(const char *filename) {
    if (!server_config.is_primary || server_config.backup_sockfd < 0) {
        return 0;
    }
    
    printf("[Backup Handler] Replicating UNDO backup for file: %s\n", filename);
    
    pthread_mutex_lock(&backup_mutex);
    
    // Build undo file path
    char undo_filepath[MAX_PATH];
    snprintf(undo_filepath, MAX_PATH, "%s/undo/%s", server_config.storage_dir, filename);
    
    // Check if undo file exists
    FILE *undo_file = fopen(undo_filepath, "r");
    if (undo_file == NULL) {
        fprintf(stderr, "[Backup Handler] Undo file not found: %s\n", undo_filepath);
        pthread_mutex_unlock(&backup_mutex);
        return -1;
    }
    fclose(undo_file);
    
    // Send undo backup operation message
    Message msg;
    memset(&msg, 0, sizeof(Message));
    msg.msg_type = MSG_REQUEST;
    msg.operation = OP_BACKUP_UNDO_FILE;
    strncpy(msg.filename, filename, MAX_FILENAME - 1);
    
    if (send_message(server_config.backup_sockfd, &msg) < 0) {
        fprintf(stderr, "[Backup Handler] Failed to send UNDO backup request\n");
        pthread_mutex_unlock(&backup_mutex);
        return -1;
    }
    
    // Send the undo file content
    int result = send_undo_file_to_backup(filename);
    
    // Wait for acknowledgment
    Message response;
    if (receive_message(server_config.backup_sockfd, &response) < 0) {
        fprintf(stderr, "[Backup Handler] Failed to receive ACK for undo backup\n");
        pthread_mutex_unlock(&backup_mutex);
        return -1;
    }
    
    if (response.error_code != ERR_SUCCESS) {
        fprintf(stderr, "[Backup Handler] Backup server reported error for undo file\n");
        pthread_mutex_unlock(&backup_mutex);
        return -1;
    }
    
    printf("[Backup Handler] Undo backup replicated successfully for '%s'\n", filename);
    pthread_mutex_unlock(&backup_mutex);
    
    return result;
}

// Send undo file content to backup server
int send_undo_file_to_backup(const char *filename) {
    char undo_filepath[MAX_PATH];
    snprintf(undo_filepath, MAX_PATH, "%s/undo/%s", server_config.storage_dir, filename);
    
    // Open undo file
    FILE *file = fopen(undo_filepath, "r");
    if (file == NULL) {
        fprintf(stderr, "[Backup Handler] Failed to open undo file: %s\n", undo_filepath);
        return -1;
    }
    
    // Get file size
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    // Send file size first
    Message size_msg;
    memset(&size_msg, 0, sizeof(Message));
    size_msg.msg_type = MSG_RESPONSE;
    size_msg.operation = OP_BACKUP_UNDO_FILE;
    snprintf(size_msg.data, MAX_DATA_SIZE, "%ld", file_size);
    
    if (send_message(server_config.backup_sockfd, &size_msg) < 0) {
        fprintf(stderr, "[Backup Handler] Failed to send undo file size\n");
        fclose(file);
        return -1;
    }
    
    // Send file content in chunks
    char buffer[MAX_DATA_SIZE];
    size_t bytes_sent = 0;
    
    while (bytes_sent < (size_t)file_size) {
        size_t to_read = (file_size - bytes_sent > MAX_DATA_SIZE - 100) ? 
                         MAX_DATA_SIZE - 100 : file_size - bytes_sent;
        
        size_t bytes_read = fread(buffer, 1, to_read, file);
        if (bytes_read == 0) break;
        
        Message chunk_msg;
        memset(&chunk_msg, 0, sizeof(Message));
        chunk_msg.msg_type = MSG_RESPONSE;
        chunk_msg.operation = OP_BACKUP_UNDO_FILE;
        memcpy(chunk_msg.data, buffer, bytes_read);
        chunk_msg.sentence_index = bytes_read; // Use this field for chunk size
        
        if (send_message(server_config.backup_sockfd, &chunk_msg) < 0) {
            fprintf(stderr, "[Backup Handler] Failed to send undo file chunk\n");
            fclose(file);
            return -1;
        }
        
        bytes_sent += bytes_read;
    }
    
    fclose(file);
    printf("[Backup Handler] Sent undo backup: %zu bytes\n", bytes_sent);
    
    return 0;
}

// Send file content to backup server
int send_file_to_backup(const char *filename) {
    char filepath[MAX_PATH];
    snprintf(filepath, MAX_PATH, "%s/files/%s", server_config.storage_dir, filename);
    
    // Open file
    FILE *file = fopen(filepath, "r");
    if (file == NULL) {
        fprintf(stderr, "[Backup Handler] Failed to open file for backup: %s\n", filepath);
        return -1;
    }
    
    // Get file size
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    // Send file size first
    Message size_msg;
    memset(&size_msg, 0, sizeof(Message));
    size_msg.msg_type = MSG_RESPONSE;
    size_msg.operation = OP_BACKUP_SYNC;
    snprintf(size_msg.data, MAX_DATA_SIZE, "%ld", file_size);
    
    if (send_message(server_config.backup_sockfd, &size_msg) < 0) {
        fprintf(stderr, "[Backup Handler] Failed to send file size\n");
        fclose(file);
        return -1;
    }
    
    // Send file content in chunks
    char buffer[MAX_DATA_SIZE];
    size_t bytes_sent = 0;
    
    while (bytes_sent < (size_t)file_size) {
        size_t to_read = (file_size - bytes_sent > MAX_DATA_SIZE - 100) ? 
                         MAX_DATA_SIZE - 100 : file_size - bytes_sent;
        
        size_t bytes_read = fread(buffer, 1, to_read, file);
        if (bytes_read == 0) break;
        
        Message chunk_msg;
        memset(&chunk_msg, 0, sizeof(Message));
        chunk_msg.msg_type = MSG_RESPONSE;
        chunk_msg.operation = OP_BACKUP_SYNC;
        memcpy(chunk_msg.data, buffer, bytes_read);
        chunk_msg.sentence_index = bytes_read; // Use this field for chunk size
        
        if (send_message(server_config.backup_sockfd, &chunk_msg) < 0) {
            fprintf(stderr, "[Backup Handler] Failed to send file chunk\n");
            fclose(file);
            return -1;
        }
        
        bytes_sent += bytes_read;
    }
    
    fclose(file);
    printf("[Backup Handler] Sent %zu bytes to backup\n", bytes_sent);
    
    return 0;
}

// Receive file content from primary server
int receive_file_from_primary(const char *filename, const char *owner) {
    (void)owner; // Suppress unused parameter warning
    char filepath[MAX_PATH];
    snprintf(filepath, MAX_PATH, "%s/files/%s", server_config.storage_dir, filename);
    
    // Receive file size
    Message size_msg;
    if (receive_message(server_config.backup_sockfd, &size_msg) < 0) {
        fprintf(stderr, "[Backup Handler] Failed to receive file size\n");
        return -1;
    }
    
    long file_size = atol(size_msg.data);
    printf("[Backup Handler] Receiving file of size: %ld bytes\n", file_size);
    
    // Open file for writing
    FILE *file = fopen(filepath, "w");
    if (file == NULL) {
        fprintf(stderr, "[Backup Handler] Failed to create backup file: %s\n", filepath);
        return -1;
    }
    
    // Receive file content in chunks
    size_t bytes_received = 0;
    
    while (bytes_received < (size_t)file_size) {
        Message chunk_msg;
        if (receive_message(server_config.backup_sockfd, &chunk_msg) < 0) {
            fprintf(stderr, "[Backup Handler] Failed to receive file chunk\n");
            fclose(file);
            return -1;
        }
        
        size_t chunk_size = chunk_msg.sentence_index;
        fwrite(chunk_msg.data, 1, chunk_size, file);
        bytes_received += chunk_size;
    }
    
    fclose(file);
    printf("[Backup Handler] Received %zu bytes from primary\n", bytes_received);
    
    // Metadata was already created by create_file_ll in handle_backup_request
    
    return 0;
}

// Receive undo file content from primary server
int receive_undo_file_from_primary(const char *filename) {
    char undo_filepath[MAX_PATH];
    snprintf(undo_filepath, MAX_PATH, "%s/undo/%s", server_config.storage_dir, filename);
    
    // Create undo directory if needed (for nested folders)
    char undo_dir_path[MAX_PATH];
    strncpy(undo_dir_path, undo_filepath, MAX_PATH - 1);
    char *last_slash = strrchr(undo_dir_path, '/');
    if (last_slash != NULL) {
        *last_slash = '\0';  // Truncate to get directory path
        // Create directory recursively
        char temp_path[MAX_PATH];
        char *p = undo_dir_path;
        
        // Skip leading slash if present
        if (*p == '/') p++;
        
        for (char *ptr = p; *ptr; ptr++) {
            if (*ptr == '/') {
                *ptr = '\0';
                snprintf(temp_path, MAX_PATH, "%s", undo_dir_path);
                mkdir(temp_path, 0755);
                *ptr = '/';
            }
        }
        mkdir(undo_dir_path, 0755);
    }
    
    // Receive file size
    Message size_msg;
    if (receive_message(server_config.backup_sockfd, &size_msg) < 0) {
        fprintf(stderr, "[Backup Handler] Failed to receive undo file size\n");
        return -1;
    }
    
    long file_size = atol(size_msg.data);
    printf("[Backup Handler] Receiving undo file of size: %ld bytes\n", file_size);
    
    // Open undo file for writing
    FILE *file = fopen(undo_filepath, "w");
    if (file == NULL) {
        fprintf(stderr, "[Backup Handler] Failed to create undo backup file: %s\n", undo_filepath);
        return -1;
    }
    
    // Receive file content in chunks
    size_t bytes_received = 0;
    
    while (bytes_received < (size_t)file_size) {
        Message chunk_msg;
        if (receive_message(server_config.backup_sockfd, &chunk_msg) < 0) {
            fprintf(stderr, "[Backup Handler] Failed to receive undo file chunk\n");
            fclose(file);
            return -1;
        }
        
        size_t chunk_size = chunk_msg.sentence_index;
        fwrite(chunk_msg.data, 1, chunk_size, file);
        bytes_received += chunk_size;
    }
    
    fclose(file);
    printf("[Backup Handler] Received undo backup: %zu bytes\n", bytes_received);
    
    return 0;
}

// Handle backup request from primary server
int handle_backup_request(int sockfd) {
    Message request;
    if (receive_message(sockfd, &request) < 0) {
        fprintf(stderr, "[Backup Handler] Failed to receive backup request\n");
        return -1;
    }
    
    printf("[Backup Handler] Received backup operation: %d for file: %s\n", 
           request.operation, request.filename);
    
    Message response;
    memset(&response, 0, sizeof(Message));
    response.msg_type = MSG_ACK;
    response.operation = request.operation;
    response.error_code = ERR_SUCCESS;
    
    switch (request.operation) {
        case OP_BACKUP_CREATE:
            // Create file on backup
            if (create_file_ll(request.filename, request.username) < 0) {
                response.msg_type = MSG_ERROR;
                response.error_code = ERR_SERVER_ERROR;
            } else {
                // Send ACK, then receive file content
                send_message(sockfd, &response);
                
                // Store this sockfd temporarily for receiving file
                int old_sockfd = server_config.backup_sockfd;
                server_config.backup_sockfd = sockfd;
                receive_file_from_primary(request.filename, request.username);
                server_config.backup_sockfd = old_sockfd;
                return 0;
            }
            break;
            
        case OP_BACKUP_DELETE:
            // Delete file from backup
            if (delete_file_ll(request.filename) < 0) {
                response.msg_type = MSG_ERROR;
                response.error_code = ERR_SERVER_ERROR;
            }
            break;
            
        case OP_BACKUP_SYNC:
            // Sync file content from primary
            send_message(sockfd, &response);
            
            // Receive updated file content
            int old_sockfd = server_config.backup_sockfd;
            server_config.backup_sockfd = sockfd;
            
            // Delete existing file and create new one
            delete_file_ll(request.filename);
            receive_file_from_primary(request.filename, "system");
            
            server_config.backup_sockfd = old_sockfd;
            
            // Send final ACK
            response.msg_type = MSG_ACK;
            response.error_code = ERR_SUCCESS;
            send_message(sockfd, &response);
            return 0;
            
        case OP_BACKUP_UNDO_FILE:
            // Receive undo backup file from primary
            printf("[Backup] Receiving undo backup for file: %s\n", request.filename);
            send_message(sockfd, &response);
            
            // Receive undo file content
            old_sockfd = server_config.backup_sockfd;
            server_config.backup_sockfd = sockfd;
            receive_undo_file_from_primary(request.filename);
            server_config.backup_sockfd = old_sockfd;
            
            // Send final ACK
            response.msg_type = MSG_ACK;
            response.error_code = ERR_SUCCESS;
            send_message(sockfd, &response);
            return 0;
            
        case OP_BACKUP_INIT_SYNC:
            // Start of bulk sync
            printf("[Backup] Received INIT_SYNC, preparing for bulk transfer\n");
            send_message(sockfd, &response);
            
            // Receive all files
            old_sockfd = server_config.backup_sockfd;
            server_config.backup_sockfd = sockfd;
            receive_bulk_sync(sockfd);
            server_config.backup_sockfd = old_sockfd;
            return 0;
            
        case OP_BACKUP_METADATA: {
            // Receive metadata file - handled in receive_bulk_sync
            // This is called outside bulk sync for incremental updates
            long file_size = atol(request.data);
            printf("[Backup] Receiving metadata update (%ld bytes)...\n", file_size);
            
            char metadata_path[MAX_PATH];
            snprintf(metadata_path, MAX_PATH, "%s/metadata.txt", server_config.storage_dir);
            
            FILE *file = fopen(metadata_path, "w");
            if (file == NULL) {
                response.msg_type = MSG_ERROR;
                response.error_code = ERR_SERVER_ERROR;
                send_message(sockfd, &response);
                return -1;
            }
            
            // Send ACK first
            send_message(sockfd, &response);
            
            // Receive chunks
            size_t bytes_received = 0;
            while (bytes_received < (size_t)file_size) {
                Message chunk;
                if (receive_message(sockfd, &chunk) < 0) {
                    fclose(file);
                    return -1;
                }
                
                size_t chunk_size = chunk.sentence_index;
                fwrite(chunk.data, 1, chunk_size, file);
                bytes_received += chunk_size;
            }
            
            fclose(file);
            load_metadata_ll();
            
            // Send final ACK
            response.msg_type = MSG_ACK;
            send_message(sockfd, &response);
            return 0;
        }
            
        default:
            response.msg_type = MSG_ERROR;
            response.error_code = ERR_INVALID_OPERATION;
            break;
    }
    
    send_message(sockfd, &response);
    return 0;
}

// Check if backup server is available
int is_backup_available() {
    return (server_config.is_primary && server_config.backup_sockfd > 0);
}

// Handle backup info from Name Server
int handle_nm_backup_info(const char *backup_ip, int backup_port) {
    printf("[Backup Handler] Received backup info from NM: %s:%d\n", backup_ip, backup_port);
    
    // Connect to backup server
    if (connect_to_backup_server(backup_ip, backup_port) < 0) {
        fprintf(stderr, "[Backup Handler] Failed to connect to backup\n");
        return -1;
    }
    
    // Perform initial bulk sync
    printf("[Backup Handler] Starting initial bulk sync...\n");
    if (perform_bulk_sync() < 0) {
        fprintf(stderr, "[Backup Handler] Bulk sync failed\n");
        return -1;
    }
    
    server_config.bulk_sync_complete = 1;
    printf("[Backup Handler] Initial bulk sync complete\n");
    
    return 0;
}

// Perform bulk sync to backup server
int perform_bulk_sync() {
    if (!server_config.is_primary || server_config.backup_sockfd < 0) {
        return 0;
    }
    
    pthread_mutex_lock(&backup_mutex);
    
    // Send INIT_SYNC message
    Message init_msg;
    memset(&init_msg, 0, sizeof(Message));
    init_msg.msg_type = MSG_REQUEST;
    init_msg.operation = OP_BACKUP_INIT_SYNC;
    
    if (send_message(server_config.backup_sockfd, &init_msg) < 0) {
        fprintf(stderr, "[Bulk Sync] Failed to send INIT_SYNC\n");
        pthread_mutex_unlock(&backup_mutex);
        return -1;
    }
    
    printf("[Bulk Sync] Sent INIT_SYNC\n");
    
    // Wait for ACK
    Message ack;
    if (receive_message(server_config.backup_sockfd, &ack) < 0) {
        fprintf(stderr, "[Bulk Sync] Failed to receive ACK\n");
        pthread_mutex_unlock(&backup_mutex);
        return -1;
    }
    
    // Sync metadata first
    printf("[Bulk Sync] Syncing metadata...\n");
    replicate_metadata();
    
    // Get list of all files
    char file_list[MAX_DATA_SIZE];
    if (get_file_list_ll(file_list, MAX_DATA_SIZE) < 0) {
        printf("[Bulk Sync] No files to sync\n");
    } else {
        // Parse file list and send each file
        char *filename = strtok(file_list, ",");
        while (filename != NULL) {
            printf("[Bulk Sync] Syncing file: %s\n", filename);
            send_bulk_file(filename, 0);  // Regular file
            send_bulk_file(filename, 1);  // Undo file (if exists)
            filename = strtok(NULL, ",");
        }
    }
    
    // Send SYNC_COMPLETE message
    Message complete_msg;
    memset(&complete_msg, 0, sizeof(Message));
    complete_msg.msg_type = MSG_REQUEST;
    complete_msg.operation = OP_BACKUP_SYNC_COMPLETE;
    
    if (send_message(server_config.backup_sockfd, &complete_msg) < 0) {
        fprintf(stderr, "[Bulk Sync] Failed to send SYNC_COMPLETE\n");
        pthread_mutex_unlock(&backup_mutex);
        return -1;
    }
    
    // Wait for final ACK
    if (receive_message(server_config.backup_sockfd, &ack) < 0) {
        fprintf(stderr, "[Bulk Sync] Failed to receive final ACK\n");
        pthread_mutex_unlock(&backup_mutex);
        return -1;
    }
    
    printf("[Bulk Sync] Complete\n");
    pthread_mutex_unlock(&backup_mutex);
    
    return 0;
}

// Replicate metadata.txt to backup server
int replicate_metadata() {
    if (!server_config.is_primary || server_config.backup_sockfd < 0) {
        return 0;
    }
    
    pthread_mutex_lock(&backup_mutex);
    
    // Read metadata file
    char metadata_path[MAX_PATH];
    snprintf(metadata_path, MAX_PATH, "%s/metadata.txt", server_config.storage_dir);
    
    FILE *file = fopen(metadata_path, "r");
    if (file == NULL) {
        fprintf(stderr, "[Metadata Sync] No metadata file found\n");
        pthread_mutex_unlock(&backup_mutex);
        return -1;
    }
    
    // Get file size
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    // Send metadata operation message
    Message msg;
    memset(&msg, 0, sizeof(Message));
    msg.msg_type = MSG_REQUEST;
    msg.operation = OP_BACKUP_METADATA;
    snprintf(msg.data, MAX_DATA_SIZE, "%ld", file_size);
    
    if (send_message(server_config.backup_sockfd, &msg) < 0) {
        fprintf(stderr, "[Metadata Sync] Failed to send metadata message\n");
        fclose(file);
        pthread_mutex_unlock(&backup_mutex);
        return -1;
    }
    
    // Send file content in chunks
    char buffer[MAX_DATA_SIZE];
    size_t bytes_sent = 0;
    
    while (bytes_sent < (size_t)file_size) {
        size_t to_read = (file_size - bytes_sent > MAX_DATA_SIZE - 100) ? 
                         MAX_DATA_SIZE - 100 : file_size - bytes_sent;
        
        size_t bytes_read = fread(buffer, 1, to_read, file);
        if (bytes_read == 0) break;
        
        Message chunk_msg;
        memset(&chunk_msg, 0, sizeof(Message));
        chunk_msg.msg_type = MSG_RESPONSE;
        chunk_msg.operation = OP_BACKUP_METADATA;
        memcpy(chunk_msg.data, buffer, bytes_read);
        chunk_msg.sentence_index = bytes_read;
        
        if (send_message(server_config.backup_sockfd, &chunk_msg) < 0) {
            fprintf(stderr, "[Metadata Sync] Failed to send chunk\n");
            fclose(file);
            pthread_mutex_unlock(&backup_mutex);
            return -1;
        }
        
        bytes_sent += bytes_read;
    }
    
    fclose(file);
    
    // Wait for ACK
    Message ack;
    if (receive_message(server_config.backup_sockfd, &ack) < 0) {
        fprintf(stderr, "[Metadata Sync] Failed to receive ACK\n");
        pthread_mutex_unlock(&backup_mutex);
        return -1;
    }
    
    printf("[Metadata Sync] Successfully synced metadata (%zu bytes)\n", bytes_sent);
    pthread_mutex_unlock(&backup_mutex);
    
    return 0;
}

// Send a single file to backup during bulk sync
int send_bulk_file(const char *filename, int is_undo_file) {
    char filepath[MAX_PATH];
    
    if (is_undo_file) {
        snprintf(filepath, MAX_PATH, "%s/undo/%s", server_config.storage_dir, filename);
    } else {
        snprintf(filepath, MAX_PATH, "%s/files/%s", server_config.storage_dir, filename);
    }
    
    // Check if file exists
    FILE *file = fopen(filepath, "r");
    if (file == NULL) {
        // Undo file might not exist, that's okay
        if (is_undo_file) {
            return 0;
        }
        fprintf(stderr, "[Bulk Sync] File not found: %s\n", filepath);
        return -1;
    }
    
    // Get file size
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    // Send file message
    Message file_msg;
    memset(&file_msg, 0, sizeof(Message));
    file_msg.msg_type = MSG_REQUEST;
    file_msg.operation = is_undo_file ? OP_BACKUP_UNDO_FILE : OP_BACKUP_FILE;
    strncpy(file_msg.filename, filename, MAX_FILENAME - 1);
    snprintf(file_msg.data, MAX_DATA_SIZE, "%ld", file_size);
    
    if (send_message(server_config.backup_sockfd, &file_msg) < 0) {
        fprintf(stderr, "[Bulk Sync] Failed to send file message\n");
        fclose(file);
        return -1;
    }
    
    // Send file content in chunks
    char buffer[MAX_DATA_SIZE];
    size_t bytes_sent = 0;
    
    while (bytes_sent < (size_t)file_size) {
        size_t to_read = (file_size - bytes_sent > MAX_DATA_SIZE - 100) ? 
                         MAX_DATA_SIZE - 100 : file_size - bytes_sent;
        
        size_t bytes_read = fread(buffer, 1, to_read, file);
        if (bytes_read == 0) break;
        
        Message chunk_msg;
        memset(&chunk_msg, 0, sizeof(Message));
        chunk_msg.msg_type = MSG_RESPONSE;
        chunk_msg.operation = is_undo_file ? OP_BACKUP_UNDO_FILE : OP_BACKUP_FILE;
        memcpy(chunk_msg.data, buffer, bytes_read);
        chunk_msg.sentence_index = bytes_read;
        
        if (send_message(server_config.backup_sockfd, &chunk_msg) < 0) {
            fprintf(stderr, "[Bulk Sync] Failed to send file chunk\n");
            fclose(file);
            return -1;
        }
        
        bytes_sent += bytes_read;
    }
    
    fclose(file);
    
    // Wait for ACK
    Message ack;
    if (receive_message(server_config.backup_sockfd, &ack) < 0) {
        fprintf(stderr, "[Bulk Sync] Failed to receive file ACK\n");
        return -1;
    }
    
    printf("[Bulk Sync] Sent %s: %s (%zu bytes)\n", 
           is_undo_file ? "undo file" : "file", filename, bytes_sent);
    
    return 0;
}

// Receive bulk sync from primary (backup server side)
int receive_bulk_sync(int sockfd) {
    printf("[Backup] Starting to receive bulk sync...\n");
    
    while (1) {
        Message msg;
        if (receive_message(sockfd, &msg) < 0) {
            fprintf(stderr, "[Backup] Failed to receive bulk sync message\n");
            return -1;
        }
        
        Message ack;
        memset(&ack, 0, sizeof(Message));
        ack.msg_type = MSG_ACK;
        ack.operation = msg.operation;
        ack.error_code = ERR_SUCCESS;
        
        switch (msg.operation) {
            case OP_BACKUP_METADATA: {
                // Receive metadata file
                long file_size = atol(msg.data);
                printf("[Backup] Receiving metadata (%ld bytes)...\n", file_size);
                
                char metadata_path[MAX_PATH];
                snprintf(metadata_path, MAX_PATH, "%s/metadata.txt", server_config.storage_dir);
                
                FILE *file = fopen(metadata_path, "w");
                if (file == NULL) {
                    fprintf(stderr, "[Backup] Failed to create metadata file\n");
                    ack.error_code = ERR_SERVER_ERROR;
                    send_message(sockfd, &ack);
                    break;
                }
                
                size_t bytes_received = 0;
                while (bytes_received < (size_t)file_size) {
                    Message chunk;
                    if (receive_message(sockfd, &chunk) < 0) {
                        fprintf(stderr, "[Backup] Failed to receive metadata chunk\n");
                        fclose(file);
                        return -1;
                    }
                    
                    size_t chunk_size = chunk.sentence_index;
                    fwrite(chunk.data, 1, chunk_size, file);
                    bytes_received += chunk_size;
                }
                
                fclose(file);
                printf("[Backup] Received metadata (%zu bytes)\n", bytes_received);
                
                // Reload metadata
                load_metadata_ll();
                
                send_message(sockfd, &ack);
                break;
            }
            
            case OP_BACKUP_FILE:
            case OP_BACKUP_UNDO_FILE: {
                // Receive regular or undo file
                long file_size = atol(msg.data);
                int is_undo = (msg.operation == OP_BACKUP_UNDO_FILE);
                
                printf("[Backup] Receiving %s: %s (%ld bytes)...\n",
                       is_undo ? "undo file" : "file", msg.filename, file_size);
                
                char filepath[MAX_PATH];
                if (is_undo) {
                    snprintf(filepath, MAX_PATH, "%s/undo/%s", 
                            server_config.storage_dir, msg.filename);
                } else {
                    snprintf(filepath, MAX_PATH, "%s/files/%s", 
                            server_config.storage_dir, msg.filename);
                }
                
                FILE *file = fopen(filepath, "w");
                if (file == NULL) {
                    fprintf(stderr, "[Backup] Failed to create file: %s\n", filepath);
                    ack.error_code = ERR_SERVER_ERROR;
                    send_message(sockfd, &ack);
                    break;
                }
                
                size_t bytes_received = 0;
                while (bytes_received < (size_t)file_size) {
                    Message chunk;
                    if (receive_message(sockfd, &chunk) < 0) {
                        fprintf(stderr, "[Backup] Failed to receive file chunk\n");
                        fclose(file);
                        return -1;
                    }
                    
                    size_t chunk_size = chunk.sentence_index;
                    fwrite(chunk.data, 1, chunk_size, file);
                    bytes_received += chunk_size;
                }
                
                fclose(file);
                printf("[Backup] Received %s: %s (%zu bytes)\n",
                       is_undo ? "undo file" : "file", msg.filename, bytes_received);
                
                send_message(sockfd, &ack);
                break;
            }
            
            case OP_BACKUP_SYNC_COMPLETE:
                printf("[Backup] Bulk sync complete\n");
                send_message(sockfd, &ack);
                return 0;
                
            default:
                fprintf(stderr, "[Backup] Unknown bulk sync operation: %d\n", msg.operation);
                ack.error_code = ERR_INVALID_OPERATION;
                send_message(sockfd, &ack);
                break;
        }
    }
    
    return 0;
}

// Cleanup backup handler
void cleanup_backup_handler() {
    pthread_mutex_lock(&backup_mutex);
    
    if (server_config.backup_sockfd > 0) {
        close(server_config.backup_sockfd);
        server_config.backup_sockfd = -1;
    }
    
    printf("[Backup Handler] Cleanup complete\n");
    pthread_mutex_unlock(&backup_mutex);
}

// ============================================================================
// FAULT TOLERANCE FUNCTIONS
// ============================================================================

// Sync files between two storage servers (for recovery)
// ============================================================================
// BULK SYNCHRONIZATION FOR RECOVERY
// ============================================================================

int perform_bulk_sync_to_backup() {
    if (!server_config.is_primary || server_config.backup_sockfd < 0) {
        printf("[Bulk Sync] Not a primary server or backup not connected\n");
        return -1;
    }
    
    printf("[Bulk Sync] Starting bulk synchronization to backup server\n");
    
    // First sync metadata
    if (replicate_metadata() < 0) {
        printf("[Bulk Sync] Metadata sync failed\n");
        return -1;
    }
    
    // Get list of all files and sync them
    char file_list[MAX_DATA_SIZE];
    if (get_file_list_ll(file_list, MAX_DATA_SIZE) < 0) {
        printf("[Bulk Sync] Failed to get file list\n");
        return -1;
    }
    
    // Parse file list and sync each file
    char *file = strtok(file_list, "\n");
    int files_synced = 0;
    
    while (file != NULL) {
        if (strlen(file) > 0) {
            printf("[Bulk Sync] Syncing file: %s\n", file);
            
            if (send_file_to_backup(file) < 0) {
                printf("[Bulk Sync] Failed to sync file: %s\n", file);
                // Continue with other files
            } else {
                files_synced++;
            }
        }
        file = strtok(NULL, "\n");
    }
    
    // Send sync complete message
    Message complete_msg = {0};
    complete_msg.msg_type = MSG_REQUEST;
    complete_msg.operation = OP_BACKUP_SYNC_COMPLETE;
    complete_msg.ss_id = server_config.ss_id;
    
    if (send_message(server_config.backup_sockfd, &complete_msg) < 0) {
        printf("[Bulk Sync] Failed to send sync complete message\n");
        return -1;
    }
    
    // Wait for acknowledgment
    Message response = {0};
    if (receive_message(server_config.backup_sockfd, &response) < 0) {
        printf("[Bulk Sync] Failed to receive sync complete acknowledgment\n");
        return -1;
    }
    
    printf("[Bulk Sync] Bulk synchronization completed: %d files synced\n", files_synced);
    return 0;
}

// ============================================================================
// CRITICAL FIX: Recovery Sync - Primary pulls data from backup after failure
// ============================================================================

int request_recovery_sync_from_backup(const char *backup_ip, int backup_port) {
    printf("[Recovery Sync] Initiating recovery sync from backup at %s:%d\n", backup_ip, backup_port);
    
    // Clear all existing files and metadata - backup is source of truth
    printf("[Recovery Sync] Clearing stale local data...\n");
    char clear_cmd[MAX_PATH];
    snprintf(clear_cmd, MAX_PATH, "rm -rf %s/files/* %s/metadata.txt", 
             server_config.storage_dir, server_config.storage_dir);
    system(clear_cmd);
    
    // Recreate files directory
    char files_dir[MAX_PATH];
    snprintf(files_dir, MAX_PATH, "%s/files", server_config.storage_dir);
    mkdir(files_dir, 0755);
    
    printf("[Recovery Sync] Cleared local data, connecting to backup...\n");
    
    // Connect to backup server (which is currently acting as primary)
    int backup_sockfd = connect_to_server(backup_ip, backup_port);
    if (backup_sockfd < 0) {
        fprintf(stderr, "[Recovery Sync] Failed to connect to backup server\n");
        return -1;
    }
    
    printf("[Recovery Sync] Connected to backup, requesting bulk sync...\n");
    
    // Request bulk sync from backup (reuse existing bulk sync protocol)
    Message sync_request = {0};
    sync_request.msg_type = MSG_REQUEST;
    sync_request.operation = OP_BACKUP_INIT_SYNC;
    sync_request.ss_id = server_config.ss_id;
    strcpy(sync_request.data, "RECOVERY_SYNC");
    
    if (send_message(backup_sockfd, &sync_request) < 0) {
        fprintf(stderr, "[Recovery Sync] Failed to send sync request\n");
        close(backup_sockfd);
        return -1;
    }
    
    // Receive bulk sync data (same as normal backup would do)
    int result = receive_bulk_sync(backup_sockfd);
    close(backup_sockfd);
    
    if (result < 0) {
        fprintf(stderr, "[Recovery Sync] Bulk sync from backup failed\n");
        return -1;
    }
    
    printf("[Recovery Sync] Successfully synced all data from backup\n");
    printf("[Recovery Sync] Reloading metadata...\n");
    
    // Reload metadata after sync
    load_metadata_ll();
    
    printf("[Recovery Sync] Recovery sync complete - server is up to date\n");
    return 0;
}
