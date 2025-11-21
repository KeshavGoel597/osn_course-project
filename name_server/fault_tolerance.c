#include "name_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

// Static threads for fault tolerance monitoring
static pthread_t replication_monitor_thread_id;
static pthread_t recovery_manager_thread_id;

// ============================================================================
// ENHANCED REPLICATION SYSTEM INITIALIZATION
// ============================================================================

int initialize_replication_system() {
    printf("[Fault Tolerance] Initializing enhanced replication system\n");
    
    // Initialize replication tracking
    memset(nm_state->replication_pairs, 0, sizeof(nm_state->replication_pairs));
    nm_state->replication_pair_count = 0;
    pthread_mutex_init(&nm_state->replication_mutex, NULL);
    
    // Initialize recovery tracking
    memset(nm_state->recovery_sessions, 0, sizeof(nm_state->recovery_sessions));
    nm_state->active_recoveries = 0;
    pthread_mutex_init(&nm_state->recovery_mutex, NULL);
    
    // Start monitoring threads
    if (pthread_create(&replication_monitor_thread_id, NULL, replication_monitor_thread, NULL) != 0) {
        fprintf(stderr, "[Fault Tolerance] Failed to create replication monitor thread\n");
        return -1;
    }
    
    if (pthread_create(&recovery_manager_thread_id, NULL, ss_recovery_manager, NULL) != 0) {
        fprintf(stderr, "[Fault Tolerance] Failed to create recovery manager thread\n");
        return -1;
    }
    
    printf("[Fault Tolerance] Enhanced replication system initialized successfully\n");
    return 0;
}

// ============================================================================
// ENHANCED REPLICATION PAIRING AND MANAGEMENT
// ============================================================================

int create_replication_pair(int primary_ss_id, int backup_ss_id) {
    printf("[Fault Tolerance] Creating replication pair: Primary SS%d <-> Backup SS%d\n", 
           primary_ss_id, backup_ss_id);
    
    pthread_mutex_lock(&nm_state->replication_mutex);
    
    // Check if pair already exists
    for (int i = 0; i < nm_state->replication_pair_count; i++) {
        ReplicationPairInfo *pair = &nm_state->replication_pairs[i];
        if (pair->primary_ss_id == primary_ss_id && pair->backup_ss_id == backup_ss_id) {
            pthread_mutex_unlock(&nm_state->replication_mutex);
            return 0; // Already exists
        }
    }
    
    // Create new replication pair
    if (nm_state->replication_pair_count >= MAX_STORAGE_SERVERS) {
        pthread_mutex_unlock(&nm_state->replication_mutex);
        fprintf(stderr, "[Fault Tolerance] Maximum replication pairs reached\n");
        return -1;
    }
    
    ReplicationPairInfo *pair = &nm_state->replication_pairs[nm_state->replication_pair_count++];
    pair->primary_ss_id = primary_ss_id;
    pair->backup_ss_id = backup_ss_id;
    pair->replication_status = REPLICATION_STATUS_SYNCED;
    pair->last_sync_time = time(NULL);
    pair->failed_replications = 0;
    pair->auto_failover_enabled = 1;
    
    pthread_mutex_unlock(&nm_state->replication_mutex);
    
    // Update storage server records
    int primary_index = find_storage_server(primary_ss_id);
    int backup_index = find_storage_server(backup_ss_id);
    
    if (primary_index >= 0 && backup_index >= 0) {
        StorageServerInfo *primary_ss = &nm_state->storage_servers[primary_index];
        StorageServerInfo *backup_ss = &nm_state->storage_servers[backup_index];
        
        pthread_mutex_lock(&primary_ss->ss_mutex);
        pthread_mutex_lock(&backup_ss->ss_mutex);
        
        primary_ss->backup_ss_id = backup_ss_id;
        backup_ss->backup_ss_id = primary_ss_id;
        
        pthread_mutex_unlock(&backup_ss->ss_mutex);
        pthread_mutex_unlock(&primary_ss->ss_mutex);
        
        // Start initial full synchronization
        start_ss_recovery_sync(backup_ss_id, primary_ss_id);
    }
    
    printf("[Fault Tolerance] Replication pair created successfully\n");
    return 0;
}

// ============================================================================
// ENHANCED FAILURE DETECTION AND RECOVERY
// ============================================================================

int handle_ss_reconnection(int ss_id) {
    printf("[Fault Tolerance] Handling reconnection of SS%d\n", ss_id);
    
    int ss_index = find_storage_server(ss_id);
    if (ss_index < 0) {
        fprintf(stderr, "[Fault Tolerance] Unknown storage server SS%d reconnecting\n", ss_id);
        return -1;
    }
    
    StorageServerInfo *ss = &nm_state->storage_servers[ss_index];
    
    pthread_mutex_lock(&ss->ss_mutex);
    
    if (ss->status == SS_STATUS_ONLINE) {
        pthread_mutex_unlock(&ss->ss_mutex);
        return 0; // Already online
    }
    
    // Mark as recovering
    ss->status = SS_STATUS_RECOVERING;
    ss->last_heartbeat = time(NULL);
    
    pthread_mutex_unlock(&ss->ss_mutex);
    
    // Find partner for synchronization
    int partner_ss_id = 0;
    pthread_mutex_lock(&nm_state->replication_mutex);
    
    for (int i = 0; i < nm_state->replication_pair_count; i++) {
        ReplicationPairInfo *pair = &nm_state->replication_pairs[i];
        if (pair->primary_ss_id == ss_id) {
            partner_ss_id = pair->backup_ss_id;
            break;
        } else if (pair->backup_ss_id == ss_id) {
            partner_ss_id = pair->primary_ss_id;
            break;
        }
    }
    
    pthread_mutex_unlock(&nm_state->replication_mutex);
    
    if (partner_ss_id > 0) {
        // Start recovery synchronization
        if (start_ss_recovery_sync(ss_id, partner_ss_id) == 0) {
            log_operation("SS_RECONNECT", "Storage server reconnected and sync started");
            return 0;
        }
    }
    
    // If no partner or sync failed, mark as online but out of sync
    pthread_mutex_lock(&ss->ss_mutex);
    ss->status = SS_STATUS_ONLINE;
    pthread_mutex_unlock(&ss->ss_mutex);
    
    log_operation("SS_RECONNECT", "Storage server reconnected without sync");
    return 0;
}

// ============================================================================
// RECOVERY AND SYNCHRONIZATION IMPLEMENTATION
// ============================================================================

int start_ss_recovery_sync(int recovering_ss_id, int partner_ss_id) {
    printf("[Fault Tolerance] Starting recovery sync: SS%d <- SS%d (PULL model)\n", 
           recovering_ss_id, partner_ss_id);
    
    // Check if partner is available
    int partner_index = find_storage_server(partner_ss_id);
    if (partner_index < 0) {
        fprintf(stderr, "[Fault Tolerance] Partner SS%d not found for recovery\n", partner_ss_id);
        return -1;
    }
    
    StorageServerInfo *partner_ss = &nm_state->storage_servers[partner_index];
    
    pthread_mutex_lock(&partner_ss->ss_mutex);
    if (partner_ss->status != SS_STATUS_ONLINE && partner_ss->status != SS_STATUS_ACTING_PRIMARY) {
        pthread_mutex_unlock(&partner_ss->ss_mutex);
        fprintf(stderr, "[Fault Tolerance] Partner SS%d is not available for sync\n", partner_ss_id);
        return -1;
    }
    pthread_mutex_unlock(&partner_ss->ss_mutex);
    
    // Check if recovering SS is available to receive the command
    int recovering_index = find_storage_server(recovering_ss_id);
    if (recovering_index < 0) {
        fprintf(stderr, "[Fault Tolerance] Recovering SS%d not found\n", recovering_ss_id);
        return -1;
    }
    
    StorageServerInfo *recovering_ss = &nm_state->storage_servers[recovering_index];
    
    // CRITICAL FIX: Use PULL model (same as registration path)
    // Send OP_RECOVERY_SYNC to the recovering SS, instructing it to PULL from partner
    // This avoids the connection problem where partner doesn't have active socket to recovering node
    
    int recovering_socket = connect_to_server(recovering_ss->ip, recovering_ss->nm_port);
    if (recovering_socket < 0) {
        fprintf(stderr, "[Fault Tolerance] Cannot connect to recovering SS%d\n", recovering_ss_id);
        return -1;
    }
    
    // Send recovery sync command to recovering SS
    Message recovery_cmd = {0};
    recovery_cmd.msg_type = MSG_REQUEST;
    recovery_cmd.operation = OP_RECOVERY_SYNC;
    recovery_cmd.ss_id = partner_ss_id;  // Partner SS ID to sync from
    strncpy(recovery_cmd.backup_ip, partner_ss->ip, MAX_IP_LEN - 1);
    recovery_cmd.backup_port = partner_ss->client_port;  // Use client_port for replication
    
    if (send_message(recovering_socket, &recovery_cmd) < 0) {
        fprintf(stderr, "[Fault Tolerance] Failed to send recovery command to SS%d\n", recovering_ss_id);
        close(recovering_socket);
        return -1;
    }
    
    close(recovering_socket);
    
    printf("[Fault Tolerance] Sent OP_RECOVERY_SYNC to SS%d: PULL from SS%d at %s:%d\n",
           recovering_ss_id, partner_ss_id, partner_ss->ip, partner_ss->client_port);
    
    // Initialize recovery session for tracking
    pthread_mutex_lock(&nm_state->recovery_mutex);
    
    SSRecoveryInfo *recovery = NULL;
    for (int i = 0; i < nm_state->active_recoveries; i++) {
        if (nm_state->recovery_sessions[i].ss_id == recovering_ss_id) {
            recovery = &nm_state->recovery_sessions[i];
            break;
        }
    }
    
    if (!recovery && nm_state->active_recoveries < MAX_STORAGE_SERVERS) {
        recovery = &nm_state->recovery_sessions[nm_state->active_recoveries++];
    }
    
    if (recovery) {
        recovery->ss_id = recovering_ss_id;
        recovery->recovery_start_time = time(NULL);
        recovery->sync_progress = 0;
        recovery->total_files_to_sync = partner_ss->file_count;
        recovery->files_synced = 0;
        recovery->sync_complete = 0;
    }
    
    pthread_mutex_unlock(&nm_state->recovery_mutex);
    
    if (!recovery) {
        fprintf(stderr, "[Fault Tolerance] Failed to create recovery session\n");
        return -1;
    }
    
    // CRITICAL FIX: Removed PUSH model synchronization
    // The recovering SS will now PULL data from partner using OP_RECOVERY_SYNC
    // No need for Name Server to orchestrate metadata/file sync with PUSH operations
    // The Storage Server handles the entire sync process internally
    
    printf("[Fault Tolerance] Recovery sync command sent, SS%d will PULL from SS%d\n",
           recovering_ss_id, partner_ss_id);
    
    // Note: Status update to SS_STATUS_ONLINE happens when SS sends completion notification
    // via OP_RECOVERY_SYNC response message
    
    return 0;
}

// ============================================================================
// ENHANCED WRITE REPLICATION (ASYNCHRONOUS)
// ============================================================================

int replicate_all_writes_async(const char *filename, const char *operation, const char *data) {
    (void)data; // Suppress unused parameter warning
    printf("[Fault Tolerance] Replicating operation '%s' for file '%s'\n", operation, filename);
    
    // Find the file's primary server
    FileInfo *file = find_file(filename);
    if (!file) {
        return 0; // File doesn't exist, nothing to replicate
    }
    
    int primary_ss_id = file->primary_ss_id;
    int backup_ss_id = file->backup_ss_id;
    
    if (backup_ss_id <= 0) {
        return 0; // No backup configured
    }
    
    // Check backup server availability
    int backup_index = find_storage_server(backup_ss_id);
    if (backup_index < 0) {
        update_replication_status(primary_ss_id, backup_ss_id, REPLICATION_STATUS_FAILED);
        return -1;
    }
    
    StorageServerInfo *backup_ss = &nm_state->storage_servers[backup_index];
    
    pthread_mutex_lock(&backup_ss->ss_mutex);
    if (backup_ss->status == SS_STATUS_OFFLINE) {
        pthread_mutex_unlock(&backup_ss->ss_mutex);
        update_replication_status(primary_ss_id, backup_ss_id, REPLICATION_STATUS_OUT_OF_SYNC);
        return -1;
    }
    pthread_mutex_unlock(&backup_ss->ss_mutex);
    
    // Send asynchronous replication request to primary SS
    int primary_index = find_storage_server(primary_ss_id);
    if (primary_index >= 0) {
        StorageServerInfo *primary_ss = &nm_state->storage_servers[primary_index];
        
        // Connect to primary SS and instruct it to replicate
        int primary_socket = connect_to_server(primary_ss->ip, primary_ss->nm_port);
        if (primary_socket >= 0) {
            Message repl_msg = {0};
            repl_msg.msg_type = MSG_REQUEST;
            repl_msg.operation = OP_BACKUP_SYNC;
            strncpy(repl_msg.filename, filename, MAX_FILENAME - 1);
            strncpy(repl_msg.data, operation, MAX_DATA_SIZE - 1);
            repl_msg.ss_id = backup_ss_id;
            
            if (send_message(primary_socket, &repl_msg) >= 0) {
                update_replication_status(primary_ss_id, backup_ss_id, REPLICATION_STATUS_SYNCED);
                printf("[Fault Tolerance] Async replication initiated successfully\n");
                close(primary_socket);
                return 0;
            }
            close(primary_socket);
        }
    }
    
    update_replication_status(primary_ss_id, backup_ss_id, REPLICATION_STATUS_FAILED);
    return -1;
}

// ============================================================================
// REPLICATION MONITORING AND HEALTH CHECKING
// ============================================================================

void* replication_monitor_thread(void* arg) {
    (void)arg;
    printf("[Fault Tolerance] Replication monitor thread started\n");
    
    while (nm_state->running) {
        sleep(30); // Check every 30 seconds
        
        pthread_mutex_lock(&nm_state->replication_mutex);
        
        for (int i = 0; i < nm_state->replication_pair_count; i++) {
            ReplicationPairInfo *pair = &nm_state->replication_pairs[i];
            
            // Check if replication is healthy
            if (pair->replication_status == REPLICATION_STATUS_FAILED) {
                if (pair->failed_replications > 3 && pair->auto_failover_enabled) {
                    printf("[Fault Tolerance] Initiating emergency failover for SS%d\n", 
                           pair->primary_ss_id);
                    perform_emergency_failover(pair->primary_ss_id);
                }
            } else if (pair->replication_status == REPLICATION_STATUS_OUT_OF_SYNC) {
                // Try to restore synchronization
                if (is_storage_server_alive(pair->primary_ss_id) && 
                    is_storage_server_alive(pair->backup_ss_id)) {
                    
                    printf("[Fault Tolerance] Attempting to restore sync between SS%d and SS%d\n",
                           pair->primary_ss_id, pair->backup_ss_id);
                    
                    if (validate_data_consistency(pair->primary_ss_id, pair->backup_ss_id) == 0) {
                        pair->replication_status = REPLICATION_STATUS_SYNCED;
                        pair->last_sync_time = time(NULL);
                        pair->failed_replications = 0;
                    }
                }
            }
        }
        
        pthread_mutex_unlock(&nm_state->replication_mutex);
    }
    
    printf("[Fault Tolerance] Replication monitor thread exiting\n");
    return NULL;
}

void* ss_recovery_manager(void* arg) {
    (void)arg;
    printf("[Fault Tolerance] Recovery manager thread started\n");
    
    while (nm_state->running) {
        sleep(60); // Check every minute
        
        pthread_mutex_lock(&nm_state->recovery_mutex);
        
        // Clean up completed recovery sessions
        for (int i = 0; i < nm_state->active_recoveries; i++) {
            SSRecoveryInfo *recovery = &nm_state->recovery_sessions[i];
            
            if (recovery->sync_complete) {
                time_t elapsed = time(NULL) - recovery->recovery_start_time;
                if (elapsed > 300) { // Keep record for 5 minutes after completion
                    // Remove from active recoveries
                    for (int j = i; j < nm_state->active_recoveries - 1; j++) {
                        nm_state->recovery_sessions[j] = nm_state->recovery_sessions[j + 1];
                    }
                    nm_state->active_recoveries--;
                    i--; // Adjust index after removal
                }
            } else {
                // Check for stalled recoveries
                time_t elapsed = time(NULL) - recovery->recovery_start_time;
                if (elapsed > 1800) { // 30 minutes timeout
                    printf("[Fault Tolerance] Recovery timeout for SS%d, marking as failed\n", 
                           recovery->ss_id);
                    
                    // Mark recovery as failed and clean up
                    recovery->sync_complete = -1; // Failed
                }
            }
        }
        
        pthread_mutex_unlock(&nm_state->recovery_mutex);
    }
    
    printf("[Fault Tolerance] Recovery manager thread exiting\n");
    return NULL;
}

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

void update_replication_status(int primary_ss_id, int backup_ss_id, int status) {
    pthread_mutex_lock(&nm_state->replication_mutex);
    
    for (int i = 0; i < nm_state->replication_pair_count; i++) {
        ReplicationPairInfo *pair = &nm_state->replication_pairs[i];
        if (pair->primary_ss_id == primary_ss_id && pair->backup_ss_id == backup_ss_id) {
            pair->replication_status = status;
            pair->last_sync_time = time(NULL);
            
            if (status == REPLICATION_STATUS_FAILED) {
                pair->failed_replications++;
            } else if (status == REPLICATION_STATUS_SYNCED) {
                pair->failed_replications = 0;
            }
            break;
        }
    }
    
    pthread_mutex_unlock(&nm_state->replication_mutex);
}

int validate_data_consistency(int primary_ss_id, int backup_ss_id) {
    // This is a simplified validation - in a real system, this would
    // compare file checksums, metadata, etc.
    printf("[Fault Tolerance] Validating data consistency between SS%d and SS%d\n",
           primary_ss_id, backup_ss_id);
    
    int primary_index = find_storage_server(primary_ss_id);
    int backup_index = find_storage_server(backup_ss_id);
    
    if (primary_index < 0 || backup_index < 0) {
        return -1;
    }
    
    StorageServerInfo *primary_ss = &nm_state->storage_servers[primary_index];
    StorageServerInfo *backup_ss = &nm_state->storage_servers[backup_index];
    
    pthread_mutex_lock(&primary_ss->ss_mutex);
    pthread_mutex_lock(&backup_ss->ss_mutex);
    
    // Simple check: compare file counts and basic metadata
    int consistent = (primary_ss->file_count == backup_ss->file_count);
    
    pthread_mutex_unlock(&backup_ss->ss_mutex);
    pthread_mutex_unlock(&primary_ss->ss_mutex);
    
    return consistent ? 0 : -1;
}

int perform_emergency_failover(int failed_ss_id) {
    printf("[Fault Tolerance] Performing emergency failover for SS%d\n", failed_ss_id);
    
    // Use existing failover logic but with enhanced logging
    handle_storage_server_failure(failed_ss_id);
    
    // Update replication status
    pthread_mutex_lock(&nm_state->replication_mutex);
    for (int i = 0; i < nm_state->replication_pair_count; i++) {
        ReplicationPairInfo *pair = &nm_state->replication_pairs[i];
        if (pair->primary_ss_id == failed_ss_id) {
            pair->auto_failover_enabled = 0; // Disable auto-failover after emergency
            break;
        }
    }
    pthread_mutex_unlock(&nm_state->replication_mutex);
    
    log_operation("EMERGENCY_FAILOVER", "Emergency failover completed");
    return 0;
}

int sync_metadata_between_ss(int source_ss_id, int target_ss_id) {
    printf("[Fault Tolerance] Syncing metadata from SS%d to SS%d\n", source_ss_id, target_ss_id);
    
    // Connect to source SS and request metadata
    int source_index = find_storage_server(source_ss_id);
    if (source_index < 0) return -1;
    
    StorageServerInfo *source_ss = &nm_state->storage_servers[source_index];
    
    int source_socket = connect_to_server(source_ss->ip, source_ss->nm_port);
    if (source_socket < 0) return -1;
    
    // Send metadata sync request
    Message sync_msg = {0};
    sync_msg.msg_type = MSG_REQUEST;
    sync_msg.operation = OP_BACKUP_METADATA;
    sync_msg.ss_id = target_ss_id;
    
    if (send_message(source_socket, &sync_msg) < 0) {
        close(source_socket);
        return -1;
    }
    
    close(source_socket);
    printf("[Fault Tolerance] Metadata sync request sent\n");
    return 0;
}

int sync_files_between_ss(int source_ss_id, int target_ss_id) {
    printf("[Fault Tolerance] Syncing files from SS%d to SS%d\n", source_ss_id, target_ss_id);
    
    // Connect to source SS and request full sync
    int source_index = find_storage_server(source_ss_id);
    if (source_index < 0) return -1;
    
    StorageServerInfo *source_ss = &nm_state->storage_servers[source_index];
    
    int source_socket = connect_to_server(source_ss->ip, source_ss->nm_port);
    if (source_socket < 0) return -1;
    
    // Send bulk sync request
    Message sync_msg = {0};
    sync_msg.msg_type = MSG_REQUEST;
    sync_msg.operation = OP_BACKUP_INIT_SYNC;
    sync_msg.ss_id = target_ss_id;
    
    if (send_message(source_socket, &sync_msg) < 0) {
        close(source_socket);
        return -1;
    }
    
    close(source_socket);
    printf("[Fault Tolerance] File sync request sent\n");
    return 0;
}
