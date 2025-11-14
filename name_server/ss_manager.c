#define _GNU_SOURCE
#include "name_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ============================================================================
// HASH TABLE IMPLEMENTATION FOR EFFICIENT FILE SEARCH (O(1))
// ============================================================================

// Initialize hash table
void init_file_hash_table() {
    memset(&nm_state->file_index.buckets, 0, sizeof(nm_state->file_index.buckets));
    pthread_mutex_init(&nm_state->file_index.hash_mutex, NULL);
    printf("[Hash Table] Initialized with %d buckets\n", FILE_HASH_TABLE_SIZE);
}

// Hash function using djb2 algorithm (Dan Bernstein)
unsigned int hash_filename(const char *filename) {
    unsigned long hash = 5381;
    int c;
    
    while ((c = *filename++)) {
        hash = ((hash << 5) + hash) + c; // hash * 33 + c
    }
    
    return hash % FILE_HASH_TABLE_SIZE;
}

// Insert file into hash table
void hash_insert_file(FileInfo *file) {
    pthread_mutex_lock(&nm_state->file_index.hash_mutex);
    
    unsigned int index = hash_filename(file->filename);
    
    // Create new node
    FileHashNode *node = (FileHashNode *)malloc(sizeof(FileHashNode));
    if (node == NULL) {
        fprintf(stderr, "[Hash Table] Failed to allocate memory for hash node\n");
        pthread_mutex_unlock(&nm_state->file_index.hash_mutex);
        return;
    }
    
    strncpy(node->filename, file->filename, MAX_FILENAME - 1);
    node->filename[MAX_FILENAME - 1] = '\0';
    node->file_ptr = file;
    node->next = nm_state->file_index.buckets[index];
    
    nm_state->file_index.buckets[index] = node;
    
    pthread_mutex_unlock(&nm_state->file_index.hash_mutex);
    
    printf("[Hash Table] Inserted file '%s' at bucket %u\n", file->filename, index);
}

// Remove file from hash table
void hash_remove_file(const char *filename) {
    pthread_mutex_lock(&nm_state->file_index.hash_mutex);
    
    unsigned int index = hash_filename(filename);
    FileHashNode *current = nm_state->file_index.buckets[index];
    FileHashNode *prev = NULL;
    
    while (current != NULL) {
        if (strcmp(current->filename, filename) == 0) {
            if (prev == NULL) {
                nm_state->file_index.buckets[index] = current->next;
            } else {
                prev->next = current->next;
            }
            free(current);
            printf("[Hash Table] Removed file '%s' from bucket %u\n", filename, index);
            pthread_mutex_unlock(&nm_state->file_index.hash_mutex);
            return;
        }
        prev = current;
        current = current->next;
    }
    
    pthread_mutex_unlock(&nm_state->file_index.hash_mutex);
}

// Find file in hash table - O(1) average case
FileInfo* hash_find_file(const char *filename) {
    pthread_mutex_lock(&nm_state->file_index.hash_mutex);
    
    unsigned int index = hash_filename(filename);
    FileHashNode *current = nm_state->file_index.buckets[index];
    
    while (current != NULL) {
        if (strcmp(current->filename, filename) == 0) {
            FileInfo *file = current->file_ptr;
            pthread_mutex_unlock(&nm_state->file_index.hash_mutex);
            return file;
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&nm_state->file_index.hash_mutex);
    return NULL;
}

// ============================================================================
// FILE LOCATION CACHE IMPLEMENTATION (LRU Cache for Recent Searches)
// ============================================================================

// Initialize the cache
void init_file_cache() {
    memset(&nm_state->search_cache.entries, 0, sizeof(nm_state->search_cache.entries));
    nm_state->search_cache.next_evict_index = 0;
    pthread_mutex_init(&nm_state->search_cache.cache_mutex, NULL);
    printf("[Cache] Initialized LRU cache with %d entries (TTL: %d seconds)\n", 
           CACHE_SIZE, CACHE_TTL);
}

// Simple hash function for cache indexing
static unsigned int cache_hash(const char *filename) {
    unsigned long hash = 5381;
    int c;
    while ((c = *filename++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash % CACHE_SIZE;
}

// Insert or update cache entry
void cache_insert(const char *filename, int primary_ss_id, int backup_ss_id) {
    pthread_mutex_lock(&nm_state->search_cache.cache_mutex);
    
    // First, try to find if entry already exists
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (nm_state->search_cache.entries[i].valid &&
            strcmp(nm_state->search_cache.entries[i].filename, filename) == 0) {
            // Update existing entry
            nm_state->search_cache.entries[i].primary_ss_id = primary_ss_id;
            nm_state->search_cache.entries[i].backup_ss_id = backup_ss_id;
            nm_state->search_cache.entries[i].timestamp = time(NULL);
            pthread_mutex_unlock(&nm_state->search_cache.cache_mutex);
            printf("[Cache] Updated entry for '%s'\n", filename);
            return;
        }
    }
    
    // Find empty slot or use round-robin eviction
    int index = -1;
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (!nm_state->search_cache.entries[i].valid) {
            index = i;
            break;
        }
    }
    
    if (index == -1) {
        // No empty slot, use round-robin eviction
        index = nm_state->search_cache.next_evict_index;
        nm_state->search_cache.next_evict_index = (index + 1) % CACHE_SIZE;
    }
    
    // Insert new entry
    strncpy(nm_state->search_cache.entries[index].filename, filename, MAX_FILENAME - 1);
    nm_state->search_cache.entries[index].filename[MAX_FILENAME - 1] = '\0';
    nm_state->search_cache.entries[index].primary_ss_id = primary_ss_id;
    nm_state->search_cache.entries[index].backup_ss_id = backup_ss_id;
    nm_state->search_cache.entries[index].timestamp = time(NULL);
    nm_state->search_cache.entries[index].valid = 1;
    
    pthread_mutex_unlock(&nm_state->search_cache.cache_mutex);
    printf("[Cache] Inserted entry for '%s' at index %d (primary: SS%d, backup: SS%d)\n", 
           filename, index, primary_ss_id, backup_ss_id);
}

// Lookup cache entry - returns 1 if found and valid, 0 otherwise
int cache_lookup(const char *filename, int *primary_ss_id, int *backup_ss_id) {
    pthread_mutex_lock(&nm_state->search_cache.cache_mutex);
    
    time_t now = time(NULL);
    
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (nm_state->search_cache.entries[i].valid &&
            strcmp(nm_state->search_cache.entries[i].filename, filename) == 0) {
            
            // Check if entry is still valid (not expired)
            if ((now - nm_state->search_cache.entries[i].timestamp) < CACHE_TTL) {
                *primary_ss_id = nm_state->search_cache.entries[i].primary_ss_id;
                *backup_ss_id = nm_state->search_cache.entries[i].backup_ss_id;
                pthread_mutex_unlock(&nm_state->search_cache.cache_mutex);
                printf("[Cache] HIT for '%s' (primary: SS%d, backup: SS%d)\n", 
                       filename, *primary_ss_id, *backup_ss_id);
                return 1;  // Cache hit
            } else {
                // Entry expired, invalidate it
                nm_state->search_cache.entries[i].valid = 0;
                pthread_mutex_unlock(&nm_state->search_cache.cache_mutex);
                printf("[Cache] MISS for '%s' (expired entry)\n", filename);
                return 0;  // Cache miss (expired)
            }
        }
    }
    
    pthread_mutex_unlock(&nm_state->search_cache.cache_mutex);
    printf("[Cache] MISS for '%s' (not found)\n", filename);
    return 0;  // Cache miss (not found)
}

// Invalidate specific cache entry (e.g., when file is deleted or moved)
void cache_invalidate(const char *filename) {
    pthread_mutex_lock(&nm_state->search_cache.cache_mutex);
    
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (nm_state->search_cache.entries[i].valid &&
            strcmp(nm_state->search_cache.entries[i].filename, filename) == 0) {
            nm_state->search_cache.entries[i].valid = 0;
            pthread_mutex_unlock(&nm_state->search_cache.cache_mutex);
            printf("[Cache] Invalidated entry for '%s'\n", filename);
            return;
        }
    }
    
    pthread_mutex_unlock(&nm_state->search_cache.cache_mutex);
}

// Clear entire cache (e.g., when SS fails or during recovery)
void cache_clear() {
    pthread_mutex_lock(&nm_state->search_cache.cache_mutex);
    memset(&nm_state->search_cache.entries, 0, sizeof(nm_state->search_cache.entries));
    nm_state->search_cache.next_evict_index = 0;
    pthread_mutex_unlock(&nm_state->search_cache.cache_mutex);
    printf("[Cache] Cleared all entries\n");
}

// ============================================================================
// STORAGE SERVER MANAGEMENT
// ============================================================================

int register_storage_server(Message *msg) {
    pthread_mutex_lock(&nm_state->ss_list_mutex);
    
    // Check if SS already exists
    int existing_index = find_storage_server(msg->ss_id);
    
    if (existing_index >= 0) {
        // Server reconnecting - update info
        StorageServerInfo *ss = &nm_state->storage_servers[existing_index];
        
        pthread_mutex_lock(&ss->ss_mutex);
        int was_offline = (ss->status == SS_STATUS_OFFLINE);
        strcpy(ss->ip, msg->ip);
        ss->nm_port = msg->port1;
        ss->client_port = msg->port2;
        int is_primary_server = ss->is_primary;
        int backup_id = ss->backup_ss_id;
        
        // CRITICAL FIX: Check if backup is acting as primary
        int backup_is_acting = 0;
        if (was_offline && is_primary_server && backup_id > 0) {
            int backup_index = find_storage_server(backup_id);
            if (backup_index >= 0) {
                StorageServerInfo *backup = &nm_state->storage_servers[backup_index];
                pthread_mutex_lock(&backup->ss_mutex);
                backup_is_acting = (backup->status == SS_STATUS_ACTING_PRIMARY);
                pthread_mutex_unlock(&backup->ss_mutex);
            }
        }
        
        if (backup_is_acting) {
            // Backup became primary during outage - mark recovering primary for sync
            ss->status = SS_STATUS_RECOVERING;
            ss->last_heartbeat = time(NULL);
            pthread_mutex_unlock(&ss->ss_mutex);
            
            printf("[SS Registration] Primary SS%d reconnecting - backup SS%d is ACTING_PRIMARY\n", 
                   msg->ss_id, backup_id);
            printf("[SS Registration] Marking SS%d as RECOVERING, will request sync from backup\n", 
                   msg->ss_id);
            log_operation("SS_RECOVERY_START", "Primary server recovering, needs sync from backup");
        } else {
            // Normal reconnection - no sync needed
            ss->status = SS_STATUS_ONLINE;
            ss->last_heartbeat = time(NULL);
            pthread_mutex_unlock(&ss->ss_mutex);
            
            printf("[SS Registration] Storage Server %d reconnected\n", msg->ss_id);
            log_operation("SS_RECONNECT", msg->data);
        }
    } else {
        // New server registration
        if (nm_state->ss_count >= MAX_STORAGE_SERVERS) {
            pthread_mutex_unlock(&nm_state->ss_list_mutex);
            printf("[SS Registration] Error: Maximum storage servers reached\n");
            return ERR_SERVER_ERROR;
        }
        
        StorageServerInfo *ss = &nm_state->storage_servers[nm_state->ss_count];
        
        pthread_mutex_lock(&ss->ss_mutex);
        ss->ss_id = msg->ss_id;
        strcpy(ss->ip, msg->ip);
        ss->nm_port = msg->port1;
        ss->client_port = msg->port2;
        ss->status = SS_STATUS_ONLINE;
        ss->last_heartbeat = time(NULL);
        ss->file_count = 0;
        ss->backup_ss_id = 0;
        ss->is_primary = (msg->ss_id % 2 == 1) ? 1 : 0;  // Odd = primary, Even = backup
        pthread_mutex_unlock(&ss->ss_mutex);
        
        nm_state->ss_count++;
        
        printf("[SS Registration] Storage Server %d registered as %s\n", 
               msg->ss_id, ss->is_primary ? "PRIMARY" : "BACKUP");
        
        // Parse file list from data field
        if (strlen(msg->data) > 0) {
            char *file_list = strdup(msg->data);
            char *filename = strtok(file_list, ",");
            
            while (filename != NULL && ss->file_count < MAX_FILES_PER_SERVER) {
                // Parse filename:owner format
                char *colon = strchr(filename, ':');
                char owner[MAX_USERNAME] = "system";  // Default owner
                
                if (colon != NULL) {
                    *colon = '\0';  // Split at colon
                    strncpy(owner, colon + 1, MAX_USERNAME - 1);
                    owner[MAX_USERNAME - 1] = '\0';
                }
                
                strcpy(ss->files[ss->file_count].filename, filename);
                strcpy(ss->files[ss->file_count].owner, owner);
                ss->files[ss->file_count].primary_ss_id = msg->ss_id;
                ss->files[ss->file_count].backup_ss_id = 0;  // Will be set during pairing
                ss->files[ss->file_count].created_time = time(NULL);
                ss->files[ss->file_count].modified_time = time(NULL);
                
                // Add file to hash table for O(1) lookup
                hash_insert_file(&ss->files[ss->file_count]);
                
                ss->file_count++;
                
                printf("[SS Registration] Registered file: %s (owner: %s)\n", filename, owner);
                filename = strtok(NULL, ",");
            }
            
            free(file_list);
        }
        
        log_operation("SS_REGISTER", msg->data);
    }
    
    // Setup backup pairing if this is a primary server
    StorageServerInfo *ss = &nm_state->storage_servers[existing_index >= 0 ? existing_index : nm_state->ss_count - 1];
    if (ss->is_primary) {
        setup_backup_pairing(msg->ss_id);
    }
    
    pthread_mutex_unlock(&nm_state->ss_list_mutex);
    
    print_server_status();
    return ERR_SUCCESS;
}

void handle_storage_server_registration(int socket, Message *msg) {
    printf("[SS Handler] Processing Storage Server registration\n");
    
    int result = register_storage_server(msg);
    
    Message response = {0};
    response.msg_type = MSG_ACK;
    response.operation = OP_SS_REGISTER;
    response.error_code = result;
    
    if (result == ERR_SUCCESS) {
        strcpy(response.data, "Registration successful");
    } else {
        strcpy(response.data, "Registration failed");
    }
    
    send_message(socket, &response);
    
    // CRITICAL FIX: Check if this server needs recovery sync
    int ss_index = find_storage_server(msg->ss_id);
    if (ss_index >= 0) {
        StorageServerInfo *ss = &nm_state->storage_servers[ss_index];
        pthread_mutex_lock(&ss->ss_mutex);
        int needs_recovery = (ss->status == SS_STATUS_RECOVERING);
        int backup_id = ss->backup_ss_id;
        pthread_mutex_unlock(&ss->ss_mutex);
        
        if (needs_recovery && backup_id > 0) {
            // Send recovery sync command to primary
            int backup_index = find_storage_server(backup_id);
            if (backup_index >= 0) {
                StorageServerInfo *backup_ss = &nm_state->storage_servers[backup_index];
                
                Message recovery_cmd = {0};
                recovery_cmd.msg_type = MSG_REQUEST;
                recovery_cmd.operation = OP_RECOVERY_SYNC;
                recovery_cmd.ss_id = backup_id;
                strcpy(recovery_cmd.backup_ip, backup_ss->ip);
                recovery_cmd.backup_port = backup_ss->nm_port;
                
                send_message(socket, &recovery_cmd);
                
                printf("[SS Recovery] Sent recovery sync command to SS%d: sync from SS%d at %s:%d\n",
                       msg->ss_id, backup_id, backup_ss->ip, backup_ss->nm_port);
                log_operation("RECOVERY_SYNC_CMD", "Commanded primary to sync from backup");
            }
            return; // Don't send backup info for normal pairing yet
        }
    }
    
    // If this is a primary server and has a backup, send backup info
    if (result == ERR_SUCCESS && (msg->ss_id % 2 == 1)) {  // Primary server
        int backup_ss_id = msg->ss_id + 1;
        int backup_index = find_storage_server(backup_ss_id);
        
        if (backup_index >= 0) {
            StorageServerInfo *backup_ss = &nm_state->storage_servers[backup_index];
            
            if (backup_ss->status == SS_STATUS_ONLINE) {
                // Send backup info to primary
                Message backup_info = {0};
                backup_info.msg_type = MSG_REQUEST;
                backup_info.operation = OP_NM_BACKUP_INFO;
                backup_info.ss_id = backup_ss_id;
                strcpy(backup_info.backup_ip, backup_ss->ip);
                backup_info.backup_port = backup_ss->nm_port;  // Use nm_port for replication (backup listens on nm_port)
                
                send_message(socket, &backup_info);
                
                printf("[Backup Pairing] Sent backup info to SS%d: backup is SS%d at %s:%d\n",
                       msg->ss_id, backup_ss_id, backup_ss->ip, backup_ss->client_port);
            }
        }
    }
}

int find_storage_server(int ss_id) {
    for (int i = 0; i < nm_state->ss_count; i++) {
        if (nm_state->storage_servers[i].ss_id == ss_id) {
            return i;
        }
    }
    return -1;  // Not found
}

void setup_backup_pairing(int primary_ss_id) {
    int backup_ss_id = primary_ss_id + 1;  // Backup is always primary + 1
    
    int primary_index = find_storage_server(primary_ss_id);
    int backup_index = find_storage_server(backup_ss_id);
    
    if (primary_index >= 0 && backup_index >= 0) {
        StorageServerInfo *primary_ss = &nm_state->storage_servers[primary_index];
        StorageServerInfo *backup_ss = &nm_state->storage_servers[backup_index];
        
        pthread_mutex_lock(&primary_ss->ss_mutex);
        pthread_mutex_lock(&backup_ss->ss_mutex);
        
        primary_ss->backup_ss_id = backup_ss_id;
        backup_ss->backup_ss_id = primary_ss_id;  // Backup points to primary
        
        // Update file info to include backup
        for (int i = 0; i < primary_ss->file_count; i++) {
            primary_ss->files[i].backup_ss_id = backup_ss_id;
        }
        
        pthread_mutex_unlock(&backup_ss->ss_mutex);
        pthread_mutex_unlock(&primary_ss->ss_mutex);
        
        // Create enhanced replication pair
        create_replication_pair(primary_ss_id, backup_ss_id);
        
        printf("[Backup Pairing] SS%d (primary) paired with SS%d (backup)\n", 
               primary_ss_id, backup_ss_id);
    }
}

int get_available_primary_server() {
    pthread_mutex_lock(&nm_state->assignment_mutex);
    
    // Round-robin assignment among primary servers
    int start_ss = nm_state->next_primary_ss;
    int selected_ss = -1;
    
    for (int attempts = 0; attempts < MAX_STORAGE_SERVERS; attempts++) {
        int current_ss = start_ss + (attempts * 2);  // Only check odd numbers (primaries)
        
        if (current_ss > MAX_STORAGE_SERVERS) {
            current_ss = 1 + (attempts % 2) * 2;  // Wrap around to 1, 3, 5...
        }
        
        int ss_index = find_storage_server(current_ss);
        if (ss_index >= 0) {
            StorageServerInfo *ss = &nm_state->storage_servers[ss_index];
            
            pthread_mutex_lock(&ss->ss_mutex);
            if (ss->status == SS_STATUS_ONLINE && ss->is_primary) {
                selected_ss = current_ss;
                nm_state->next_primary_ss = current_ss + 2;  // Next primary
                if (nm_state->next_primary_ss > MAX_STORAGE_SERVERS) {
                    nm_state->next_primary_ss = 1;
                }
                pthread_mutex_unlock(&ss->ss_mutex);
                break;
            }
            pthread_mutex_unlock(&ss->ss_mutex);
        }
    }
    
    pthread_mutex_unlock(&nm_state->assignment_mutex);
    
    if (selected_ss > 0) {
        printf("[Load Balancing] Assigned primary server: SS%d\n", selected_ss);
    } else {
        printf("[Load Balancing] Error: No available primary servers\n");
    }
    
    return selected_ss;
}

int add_file_to_server(int ss_id, const char *filename, const char *owner) {
    int ss_index = find_storage_server(ss_id);
    if (ss_index < 0) {
        return ERR_SERVER_ERROR;
    }
    
    StorageServerInfo *ss = &nm_state->storage_servers[ss_index];
    
    pthread_mutex_lock(&ss->ss_mutex);
    
    if (ss->file_count >= MAX_FILES_PER_SERVER) {
        pthread_mutex_unlock(&ss->ss_mutex);
        return ERR_SERVER_ERROR;
    }
    
    // Check if file already exists
    for (int i = 0; i < ss->file_count; i++) {
        if (strcmp(ss->files[i].filename, filename) == 0) {
            pthread_mutex_unlock(&ss->ss_mutex);
            return ERR_FILE_EXISTS;
        }
    }
    
    // Add new file
    FileInfo *file = &ss->files[ss->file_count];
    strcpy(file->filename, filename);
    strcpy(file->owner, owner);
    file->primary_ss_id = ss_id;
    file->backup_ss_id = ss->backup_ss_id;
    file->created_time = time(NULL);
    file->modified_time = time(NULL);
    
    // Add to hash table for O(1) lookup
    hash_insert_file(file);
    
    ss->file_count++;
    
    pthread_mutex_unlock(&ss->ss_mutex);
    
    printf("[File Management] Added file '%s' to SS%d (owner: %s)\n", filename, ss_id, owner);
    return ERR_SUCCESS;
}

int remove_file_from_server(int ss_id, const char *filename) {
    int ss_index = find_storage_server(ss_id);
    if (ss_index < 0) {
        return ERR_SERVER_ERROR;
    }
    
    StorageServerInfo *ss = &nm_state->storage_servers[ss_index];
    
    pthread_mutex_lock(&ss->ss_mutex);
    
    // Find and remove file
    for (int i = 0; i < ss->file_count; i++) {
        if (strcmp(ss->files[i].filename, filename) == 0) {
            // Remove from hash table first
            hash_remove_file(filename);
            
            // Shift remaining files
            for (int j = i; j < ss->file_count - 1; j++) {
                ss->files[j] = ss->files[j + 1];
            }
            ss->file_count--;
            
            pthread_mutex_unlock(&ss->ss_mutex);
            printf("[File Management] Removed file '%s' from SS%d\n", filename, ss_id);
            return ERR_SUCCESS;
        }
    }
    
    pthread_mutex_unlock(&ss->ss_mutex);
    return ERR_FILE_NOT_FOUND;
}

FileInfo* find_file(const char *filename) {
    // Use hash table for O(1) lookup instead of O(N*M) linear search
    return hash_find_file(filename);
}

int find_primary_for_file(const char *filename) {
    FileInfo *file = find_file(filename);
    if (file) {
        return file->primary_ss_id;
    }
    return -1;
}

// CRITICAL FIX: Handle recovery sync completion
void handle_recovery_sync_complete(int ss_id) {
    printf("[Recovery Sync] SS%d has completed recovery sync from backup\n", ss_id);
    
    pthread_mutex_lock(&nm_state->ss_list_mutex);
    
    int ss_index = find_storage_server(ss_id);
    if (ss_index < 0) {
        pthread_mutex_unlock(&nm_state->ss_list_mutex);
        return;
    }
    
    StorageServerInfo *ss = &nm_state->storage_servers[ss_index];
    pthread_mutex_lock(&ss->ss_mutex);
    
    if (ss->status != SS_STATUS_RECOVERING) {
        printf("[Recovery Sync] Warning: SS%d is not in RECOVERING state\n", ss_id);
        pthread_mutex_unlock(&ss->ss_mutex);
        pthread_mutex_unlock(&nm_state->ss_list_mutex);
        return;
    }
    
    // Mark primary as ONLINE now that sync is complete
    ss->status = SS_STATUS_ONLINE;
    int backup_id = ss->backup_ss_id;
    pthread_mutex_unlock(&ss->ss_mutex);
    
    // Demote backup from ACTING_PRIMARY back to normal backup
    if (backup_id > 0) {
        int backup_index = find_storage_server(backup_id);
        if (backup_index >= 0) {
            StorageServerInfo *backup = &nm_state->storage_servers[backup_index];
            pthread_mutex_lock(&backup->ss_mutex);
            
            if (backup->status == SS_STATUS_ACTING_PRIMARY) {
                backup->status = SS_STATUS_ONLINE;
                printf("[Recovery Sync] Demoted backup SS%d from ACTING_PRIMARY to ONLINE\n", backup_id);
            }
            
            pthread_mutex_unlock(&backup->ss_mutex);
        }
    }
    
    pthread_mutex_unlock(&nm_state->ss_list_mutex);
    
    printf("[Recovery Sync] SS%d is now ONLINE and ready to serve requests\n", ss_id);
    log_operation("RECOVERY_SYNC_COMPLETE", "Primary server recovery complete, restored to service");
    
    // Clear cache to force fresh lookups
    cache_clear();
    print_server_status();
}
