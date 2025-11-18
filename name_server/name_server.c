#include "name_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>

// Global state
NameServerState *nm_state = NULL;

// Global flag to control server running state
volatile sig_atomic_t server_running = 1;

int init_name_server() {
    printf("=== Name Server Initialization ===\n");
    
    // Allocate and initialize global state
    nm_state = malloc(sizeof(NameServerState));
    if (!nm_state) {
        printf("Error: Failed to allocate memory for Name Server state\n");
        return -1;
    }
    
    memset(nm_state, 0, sizeof(NameServerState));
    
    // Initialize mutexes
    if (pthread_mutex_init(&nm_state->ss_list_mutex, NULL) != 0) {
        printf("Error: Failed to initialize ss_list_mutex\n");
        free(nm_state);
        return -1;
    }
    
    if (pthread_mutex_init(&nm_state->client_list_mutex, NULL) != 0) {
        printf("Error: Failed to initialize client_list_mutex\n");
        pthread_mutex_destroy(&nm_state->ss_list_mutex);
        free(nm_state);
        return -1;
    }
    
    if (pthread_mutex_init(&nm_state->assignment_mutex, NULL) != 0) {
        printf("Error: Failed to initialize assignment_mutex\n");
        pthread_mutex_destroy(&nm_state->ss_list_mutex);
        pthread_mutex_destroy(&nm_state->client_list_mutex);
        free(nm_state);
        return -1;
    }
    
    if (pthread_mutex_init(&nm_state->request_mutex, NULL) != 0) {
        printf("Error: Failed to initialize request_mutex\n");
        pthread_mutex_destroy(&nm_state->ss_list_mutex);
        pthread_mutex_destroy(&nm_state->client_list_mutex);
        pthread_mutex_destroy(&nm_state->assignment_mutex);
        free(nm_state);
        return -1;
    }
    
    // Initialize storage server mutexes
    for (int i = 0; i < MAX_STORAGE_SERVERS; i++) {
        if (pthread_mutex_init(&nm_state->storage_servers[i].ss_mutex, NULL) != 0) {
            printf("Error: Failed to initialize storage server mutex %d\n", i);
            // Cleanup already initialized mutexes
            for (int j = 0; j < i; j++) {
                pthread_mutex_destroy(&nm_state->storage_servers[j].ss_mutex);
            }
            pthread_mutex_destroy(&nm_state->ss_list_mutex);
            pthread_mutex_destroy(&nm_state->client_list_mutex);
            pthread_mutex_destroy(&nm_state->assignment_mutex);
            free(nm_state);
            return -1;
        }
    }
    
    // Initialize state
    nm_state->ss_count = 0;
    nm_state->client_count = 0;
    nm_state->request_count = 0;
    nm_state->next_primary_ss = 0;  // CRITICAL FIX: Start with index 0, not SS ID 1
    nm_state->running = 1;
    
    // Initialize hash table for efficient file search (O(1))
    init_file_hash_table();
    
    // Initialize file location cache for recent searches
    init_file_cache();
    
    // Initialize enhanced fault tolerance system
    if (initialize_replication_system() < 0) {
        printf("Error: Failed to initialize fault tolerance system\n");
        cleanup_name_server();
        return -1;
    }
    
    // Create server socket
    nm_state->server_socket = create_socket();
    if (nm_state->server_socket < 0) {
        printf("Error: Failed to create server socket\n");
        cleanup_name_server();
        return -1;
    }
    
    // Bind to Name Server port
    if (bind_socket(nm_state->server_socket, NM_PORT) < 0) {
        printf("Error: Failed to bind to port %d\n", NM_PORT);
        cleanup_name_server();
        return -1;
    }
    
    // Start listening
    if (listen_socket(nm_state->server_socket, 50) < 0) {
        printf("Error: Failed to listen on socket\n");
        cleanup_name_server();
        return -1;
    }
    
    printf("Name Server initialized successfully\n");
    printf("Listening on port %d\n", NM_PORT);
    
    return 0;
}

void start_name_server() {
    printf("=== Starting Name Server ===\n");
    
    // Start heartbeat monitoring thread
    pthread_t heartbeat_thread;
    if (pthread_create(&heartbeat_thread, NULL, heartbeat_monitor, NULL) != 0) {
        printf("Error: Failed to create heartbeat thread\n");
        return;
    }
    pthread_detach(heartbeat_thread);
    
    printf("Name Server is running...\n");
    print_server_status();
    
    // Main accept loop
    while (server_running) {
        char client_ip[MAX_IP_LEN];
        int client_socket = accept_connection(nm_state->server_socket, client_ip);
        
        if (client_socket < 0) {
            if (!server_running) break;  // Exit gracefully if shutting down
            if (nm_state->running) {
                printf("Error accepting connection: %s\n", strerror(errno));
            }
            continue;
        }
        
        printf("New connection from %s\n", client_ip);
        
        // Create thread to handle this connection
        pthread_t handler_thread;
        int *socket_ptr = malloc(sizeof(int));
        *socket_ptr = client_socket;
        
        if (pthread_create(&handler_thread, NULL, (void*)handle_connection, socket_ptr) != 0) {
            printf("Error: Failed to create handler thread\n");
            close(client_socket);
            free(socket_ptr);
        } else {
            pthread_detach(handler_thread);
        }
    }
    
    printf("Name Server stopped\n");
}

void stop_name_server() {
    if (nm_state) {
        nm_state->running = 0;
        close_socket(nm_state->server_socket);
    }
}

void cleanup_name_server() {
    if (!nm_state) return;
    
    printf("Cleaning up Name Server...\n");
    
    // Close server socket
    if (nm_state->server_socket >= 0) {
        close_socket(nm_state->server_socket);
    }
    
    // Destroy mutexes
    pthread_mutex_destroy(&nm_state->ss_list_mutex);
    pthread_mutex_destroy(&nm_state->client_list_mutex);
    pthread_mutex_destroy(&nm_state->assignment_mutex);
    
    for (int i = 0; i < MAX_STORAGE_SERVERS; i++) {
        pthread_mutex_destroy(&nm_state->storage_servers[i].ss_mutex);
    }
    
    // Free memory
    free(nm_state);
    nm_state = NULL;
    
    printf("Name Server cleanup complete\n");
}

void handle_connection(void *arg) {
    int socket = *(int*)arg;
    free(arg);
    
    Message msg;
    
    // Receive first message to determine connection type
    if (receive_message(socket, &msg) < 0) {
        printf("Error: Failed to receive initial message\n");
        close(socket);
        return;
    }
    
    printf("Received message: type=%d, operation=%d\n", msg.msg_type, msg.operation);
    
    // Route based on operation type
    switch (msg.operation) {
        case OP_SS_REGISTER:
            handle_storage_server_registration(socket, &msg);
            break;
            
        case OP_RECOVERY_SYNC:
            // Storage server completed recovery sync
            printf("[Recovery Sync] SS%d completed recovery sync\n", msg.ss_id);
            handle_recovery_sync_complete(msg.ss_id);
            {
                Message ack = {0};
                ack.msg_type = MSG_ACK;
                ack.operation = OP_RECOVERY_SYNC;
                ack.error_code = ERR_SUCCESS;
                strcpy(ack.data, "Recovery sync acknowledged");
                send_message(socket, &ack);
            }
            break;
            
        case OP_CLIENT_REGISTER:
            handle_client_registration(socket, &msg);
            break;
            
        case OP_GET_SS_INFO:
            handle_get_ss_info(socket, &msg);
            break;
            
        case OP_CREATE:
            handle_create_file(socket, &msg);
            break;
            
        case OP_DELETE:
            handle_delete_file(socket, &msg);
            break;
            
        case OP_ADDACCESS:
            printf("[DEBUG] Routing to handle_addaccess\n");
            handle_addaccess(socket, &msg);
            printf("[DEBUG] handle_addaccess completed\n");
            break;
            
        case OP_REMACCESS:
            printf("[DEBUG] Routing to handle_remaccess\n");
            handle_remaccess(socket, &msg);
            printf("[DEBUG] handle_remaccess completed\n");
            break;
            
        case OP_LIST:
            handle_list_files(socket, &msg);
            break;
            
        case OP_VIEW:
            handle_view_files(socket, &msg);
            break;
            
        case OP_INFO:
            handle_info_request(socket, &msg);
            break;
            
        case OP_EXEC:
            handle_exec(socket, &msg);
            break;
        
        case OP_CREATEFOLDER:
            handle_createfolder(socket, &msg);
            break;
            
        case OP_MOVE:
            handle_move_file(socket, &msg);
            break;
            
        case OP_VIEWFOLDER:
            handle_viewfolder(socket, &msg);
            break;
            
        case OP_CHECKPOINT:
            handle_checkpoint(socket, &msg);
            break;
            
        case OP_VIEWCHECKPOINT:
            handle_viewcheckpoint(socket, &msg);
            break;
            
        case OP_REVERT:
            handle_revert(socket, &msg);
            break;
            
        case OP_LISTCHECKPOINTS:
            handle_listcheckpoints(socket, &msg);
            break;
        
        case OP_REQUESTACCESS:
            handle_requestaccess(socket, &msg);
            break;
            
        case OP_VIEWREQUESTS:
            handle_viewrequests(socket, &msg);
            break;
            
        case OP_APPROVEREQUEST:
            handle_approverequest(socket, &msg);
            break;
            
        case OP_REJECTREQUEST:
            handle_rejectrequest(socket, &msg);
            break;
        
        default:
            printf("Unknown operation: %d\n", msg.operation);
            Message error_msg = {0};
            error_msg.msg_type = MSG_ERROR;
            error_msg.error_code = ERR_INVALID_OPERATION;
            send_message(socket, &error_msg);
    }
    
    close(socket);
}

// Signal handler for graceful shutdown
void signal_handler(int sig) {
    printf("\nReceived signal %d, shutting down...\n", sig);
    server_running = 0;
    
    // Close listening socket to unblock accept() call
    if (nm_state && nm_state->server_socket >= 0) {
        shutdown(nm_state->server_socket, SHUT_RDWR);
    }
    
    sleep(1);
    stop_name_server();
}

int main() {
    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    printf("=== Name Server (Control Plane) ===\n");
    printf("OSN Course Project - Distributed File System\n");
    printf("Author: Control Plane Developer\n");
    printf("Date: November 6, 2025\n\n");
    
    // Initialize Name Server
    if (init_name_server() < 0) {
        printf("Failed to initialize Name Server\n");
        return 1;
    }
    
    // Start Name Server
    start_name_server();
    
    // Cleanup
    cleanup_name_server();
    
    return 0;
}
