#include "client.h"
#include "../common/network_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Helper to connect to Name Server
static int connect_to_nm() {
    int sockfd = connect_to_server(client_config.nm_ip, NM_PORT);
    if (sockfd < 0) {
        fprintf(stderr, "Failed to connect to Name Server\n");
        return -1;
    }
    return sockfd;
}

// Send VIEW request to Name Server
int send_view_request(int view_flags) {
    int sockfd = connect_to_nm();
    if (sockfd < 0) return -1;
    
    Message request;
    memset(&request, 0, sizeof(Message));
    request.msg_type = MSG_REQUEST;
    request.operation = OP_VIEW;
    request.sentence_index = view_flags;  // Use sentence_index field for flags
    strncpy(request.username, client_config.username, MAX_USERNAME - 1);
    
    if (send_message(sockfd, &request) < 0) {
        fprintf(stderr, "Failed to send VIEW request\n");
        close_socket(sockfd);
        return -1;
    }
    
    Message response;
    if (receive_message(sockfd, &response) < 0) {
        fprintf(stderr, "Failed to receive VIEW response\n");
        close_socket(sockfd);
        return -1;
    }
    
    if (response.msg_type == MSG_ERROR || response.error_code != ERR_SUCCESS) {
        fprintf(stderr, "Error: %s\n", response.data);
        close_socket(sockfd);
        return -1;
    }
    
    // Display the file list
    printf("%s\n", response.data);
    
    close_socket(sockfd);
    return 0;
}

// Send LIST request to Name Server
int send_list_request() {
    int sockfd = connect_to_nm();
    if (sockfd < 0) return -1;
    
    Message request;
    memset(&request, 0, sizeof(Message));
    request.msg_type = MSG_REQUEST;
    request.operation = OP_LIST;
    strncpy(request.username, client_config.username, MAX_USERNAME - 1);
    
    if (send_message(sockfd, &request) < 0) {
        fprintf(stderr, "Failed to send LIST request\n");
        close_socket(sockfd);
        return -1;
    }
    
    Message response;
    if (receive_message(sockfd, &response) < 0) {
        fprintf(stderr, "Failed to receive LIST response\n");
        close_socket(sockfd);
        return -1;
    }
    
    if (response.msg_type == MSG_ERROR || response.error_code != ERR_SUCCESS) {
        fprintf(stderr, "Error: %s\n", response.data);
        close_socket(sockfd);
        return -1;
    }
    
    // Display the user list
    printf("%s\n", response.data);
    
    close_socket(sockfd);
    return 0;
}

// Send INFO request to Name Server
int send_info_request(const char *filename) {
    int sockfd = connect_to_nm();
    if (sockfd < 0) return -1;
    
    Message request;
    memset(&request, 0, sizeof(Message));
    request.msg_type = MSG_REQUEST;
    request.operation = OP_INFO;
    strncpy(request.username, client_config.username, MAX_USERNAME - 1);
    strncpy(request.filename, filename, MAX_FILENAME - 1);
    
    if (send_message(sockfd, &request) < 0) {
        fprintf(stderr, "Failed to send INFO request\n");
        close_socket(sockfd);
        return -1;
    }
    
    Message response;
    if (receive_message(sockfd, &response) < 0) {
        fprintf(stderr, "Failed to receive INFO response\n");
        close_socket(sockfd);
        return -1;
    }
    
    if (response.msg_type == MSG_ERROR || response.error_code != ERR_SUCCESS) {
        if (response.error_code == ERR_FILE_NOT_FOUND) {
            fprintf(stderr, "Error: File '%s' not found\n", filename);
        } else {
            fprintf(stderr, "Error: %s\n", response.data);
        }
        close_socket(sockfd);
        return -1;
    }
    
    // Display file info
    printf("%s\n", response.data);
    
    close_socket(sockfd);
    return 0;
}

// Send ADDACCESS request to Name Server
int send_addaccess_request(const char *filename, const char *target_user, int access_type) {
    int sockfd = connect_to_nm();
    if (sockfd < 0) return -1;
    
    printf("[DEBUG] Sending ADDACCESS request for file: %s, user: %s, type: %d\n", 
           filename, target_user, access_type);
    
    Message request;
    memset(&request, 0, sizeof(Message));
    request.msg_type = MSG_REQUEST;
    request.operation = OP_ADDACCESS;
    strncpy(request.username, client_config.username, MAX_USERNAME - 1);
    strncpy(request.filename, filename, MAX_FILENAME - 1);
    strncpy(request.data, target_user, MAX_USERNAME - 1);
    request.sentence_index = access_type;  // Use sentence_index for access type
    
    if (send_message(sockfd, &request) < 0) {
        fprintf(stderr, "Failed to send ADDACCESS request\n");
        close_socket(sockfd);
        return -1;
    }
    
    printf("[DEBUG] ADDACCESS request sent, waiting for response...\n");
    
    Message response;
    if (receive_message(sockfd, &response) < 0) {
        fprintf(stderr, "Failed to receive ADDACCESS response\n");
        close_socket(sockfd);
        return -1;
    }
    
    printf("[DEBUG] ADDACCESS response received: msg_type=%d, error_code=%d\n", 
           response.msg_type, response.error_code);
    
    if (response.msg_type == MSG_ERROR || response.error_code != ERR_SUCCESS) {
        if (response.error_code == ERR_NOT_OWNER) {
            fprintf(stderr, "Error: Only the owner can modify access\n");
        } else if (response.error_code == ERR_FILE_NOT_FOUND) {
            fprintf(stderr, "Error: File '%s' not found\n", filename);
        } else {
            fprintf(stderr, "Error: %s\n", response.data);
        }
        close_socket(sockfd);
        return -1;
    }
    
    printf("Access granted successfully!\n");
    
    close_socket(sockfd);
    return 0;
}

// Send REMACCESS request to Name Server
int send_remaccess_request(const char *filename, const char *target_user) {
    int sockfd = connect_to_nm();
    if (sockfd < 0) return -1;
    
    printf("[DEBUG] Sending REMACCESS request for file: %s, user: %s\n", 
           filename, target_user);
    
    Message request;
    memset(&request, 0, sizeof(Message));
    request.msg_type = MSG_REQUEST;
    request.operation = OP_REMACCESS;
    strncpy(request.username, client_config.username, MAX_USERNAME - 1);
    strncpy(request.filename, filename, MAX_FILENAME - 1);
    strncpy(request.data, target_user, MAX_USERNAME - 1);
    
    if (send_message(sockfd, &request) < 0) {
        fprintf(stderr, "Failed to send REMACCESS request\n");
        close_socket(sockfd);
        return -1;
    }
    
    printf("[DEBUG] REMACCESS request sent, waiting for response...\n");
    
    Message response;
    if (receive_message(sockfd, &response) < 0) {
        fprintf(stderr, "Failed to receive REMACCESS response\n");
        close_socket(sockfd);
        return -1;
    }
    
    printf("[DEBUG] REMACCESS response received: msg_type=%d, error_code=%d\n", 
           response.msg_type, response.error_code);
    
    if (response.msg_type == MSG_ERROR || response.error_code != ERR_SUCCESS) {
        if (response.error_code == ERR_NOT_OWNER) {
            fprintf(stderr, "Error: Only the owner can modify access\n");
        } else if (response.error_code == ERR_FILE_NOT_FOUND) {
            fprintf(stderr, "Error: File '%s' not found\n", filename);
        } else {
            fprintf(stderr, "Error: %s\n", response.data);
        }
        close_socket(sockfd);
        return -1;
    }
    
    printf("Access removed successfully!\n");
    
    close_socket(sockfd);
    return 0;
}

// Send CREATE request to Name Server
int send_create_request(const char *filename) {
    int sockfd = connect_to_nm();
    if (sockfd < 0) return -1;
    
    Message request;
    memset(&request, 0, sizeof(Message));
    request.msg_type = MSG_REQUEST;
    request.operation = OP_CREATE;
    strncpy(request.username, client_config.username, MAX_USERNAME - 1);
    strncpy(request.filename, filename, MAX_FILENAME - 1);
    
    if (send_message(sockfd, &request) < 0) {
        fprintf(stderr, "Failed to send CREATE request\n");
        close_socket(sockfd);
        return -1;
    }
    
    Message response;
    if (receive_message(sockfd, &response) < 0) {
        fprintf(stderr, "Failed to receive CREATE response\n");
        close_socket(sockfd);
        return -1;
    }
    
    if (response.msg_type == MSG_ERROR || response.error_code !=ERR_SUCCESS) {
        if (response.error_code == ERR_FILE_EXISTS) {
            fprintf(stderr, "Error: File '%s' already exists\n", filename);
        } else {
            fprintf(stderr, "Error: %d\n", response.data);
        }
        close_socket(sockfd);
        return -1;
    }
    
    printf("File Created Successfully!\n");
    
    close_socket(sockfd);
    return 0;
}

// Send DELETE request to Name Server
int send_delete_request(const char *filename) {
    int sockfd = connect_to_nm();
    if (sockfd < 0) return -1;
    
    Message request;
    memset(&request, 0, sizeof(Message));
    request.msg_type = MSG_REQUEST;
    request.operation = OP_DELETE;
    strncpy(request.username, client_config.username, MAX_USERNAME - 1);
    strncpy(request.filename, filename, MAX_FILENAME - 1);
    
    if (send_message(sockfd, &request) < 0) {
        fprintf(stderr, "Failed to send DELETE request\n");
        close_socket(sockfd);
        return -1;
    }
    
    Message response;
    if (receive_message(sockfd, &response) < 0) {
        fprintf(stderr, "Failed to receive DELETE response\n");
        close_socket(sockfd);
        return -1;
    }
    
    if (response.msg_type == MSG_ERROR || response.error_code != ERR_SUCCESS) {
        if (response.error_code == ERR_FILE_NOT_FOUND) {
            fprintf(stderr, "Error: File '%s' not found\n", filename);
        } else if (response.error_code == ERR_NOT_OWNER) {
            fprintf(stderr, "Error: Only the owner can delete the file\n");
        } else {
            fprintf(stderr, "Error: %s\n", response.data);
        }
        close_socket(sockfd);
        return -1;
    }
    
    printf("File '%s' deleted successfully!\n", filename);
    
    close_socket(sockfd);
    return 0;
}

// Send EXEC request to Name Server
int send_exec_request(const char *filename) {
    printf("=== EXEC: Executing commands from file '%s' ===\n", filename);
    
    // CRITICAL FIX: Send OP_EXEC to Name Server for SERVER-SIDE execution
    // The specification requires execution on the Name Server, NOT the client
    // Previous implementation was a SECURITY VIOLATION (Remote File Execution on client)
    
    int sockfd = connect_to_nm();
    if (sockfd < 0) return -1;
    
    Message request;
    memset(&request, 0, sizeof(Message));
    request.msg_type = MSG_REQUEST;
    request.operation = OP_EXEC;
    strncpy(request.username, client_config.username, MAX_USERNAME - 1);
    strncpy(request.filename, filename, MAX_FILENAME - 1);
    
    if (send_message(sockfd, &request) < 0) {
        fprintf(stderr, "Failed to send EXEC request\n");
        close_socket(sockfd);
        return -1;
    }
    
    // Receive execution output from Name Server
    Message response;
    if (receive_message(sockfd, &response) < 0) {
        fprintf(stderr, "Failed to receive EXEC response\n");
        close_socket(sockfd);
        return -1;
    }
    
    close_socket(sockfd);
    
    if (response.msg_type == MSG_ERROR) {
        if (response.error_code == ERR_FILE_NOT_FOUND) {
            fprintf(stderr, "Error: File '%s' not found\n", filename);
        } else if (response.error_code == ERR_NO_READ_ACCESS) {
            fprintf(stderr, "Error: No read access to file '%s'\n", filename);
        } else if (response.error_code == ERR_SERVER_ERROR) {
            fprintf(stderr, "Error: Execution failed on server\n");
            if (strlen(response.data) > 0) {
                fprintf(stderr, "Details: %s\n", response.data);
            }
        } else {
            fprintf(stderr, "Error: %d - %s\n", response.error_code, response.data);
        }
        return -1;
    }
    
    // Display output from server-side execution
    printf("\n--- Execution Output (from Name Server) ---\n");
    printf("%s", response.data);
    if (response.data[strlen(response.data) - 1] != '\n') {
        printf("\n");
    }
    printf("--- Execution Complete ---\n");
    
    return 0;
}

// Get Storage Server info for a file (for direct operations)
int get_ss_info(const char *filename, char *ss_ip, int *ss_port) {
    int sockfd = connect_to_nm();
    if (sockfd < 0) return -1;
    
    Message request;
    memset(&request, 0, sizeof(Message));
    request.msg_type = MSG_REQUEST;
    request.operation = OP_GET_SS_INFO;
    strncpy(request.username, client_config.username, MAX_USERNAME - 1);
    strncpy(request.filename, filename, MAX_FILENAME - 1);
    
    if (send_message(sockfd, &request) < 0) {
        fprintf(stderr, "Failed to send SS info request\n");
        close_socket(sockfd);
        return -1;
    }
    
    Message response;
    if (receive_message(sockfd, &response) < 0) {
        fprintf(stderr, "Failed to receive SS info response\n");
        close_socket(sockfd);
        return -1;
    }
    
    if (response.msg_type == MSG_ERROR || response.error_code != ERR_SUCCESS) {
        if (response.error_code == ERR_FILE_NOT_FOUND) {
            fprintf(stderr, "Error: File '%s' not found\n", filename);
        } else {
            fprintf(stderr, "Error: %s\n", response.data);
        }
        close_socket(sockfd);
        return -1;
    }
    
    // Extract SS IP and port from response
    strncpy(ss_ip, response.ip, MAX_IP_LEN - 1);
    *ss_port = response.port1;  // Client port of SS (Name Server sends it in port1)
    
    close_socket(sockfd);
    return 0;
}

// Send CREATEFOLDER request to Name Server
int send_createfolder_request(const char *foldername) {
    int sockfd = connect_to_nm();
    if (sockfd < 0) return -1;
    
    Message request;
    memset(&request, 0, sizeof(Message));
    request.msg_type = MSG_REQUEST;
    request.operation = OP_CREATEFOLDER;
    strncpy(request.username, client_config.username, MAX_USERNAME - 1);
    strncpy(request.filename, foldername, MAX_FILENAME - 1);
    
    if (send_message(sockfd, &request) < 0) {
        fprintf(stderr, "Failed to send CREATEFOLDER request\n");
        close_socket(sockfd);
        return -1;
    }
    
    Message response;
    if (receive_message(sockfd, &response) < 0) {
        fprintf(stderr, "Failed to receive CREATEFOLDER response\n");
        close_socket(sockfd);
        return -1;
    }
    
    if (response.msg_type == MSG_ERROR || response.error_code != ERR_SUCCESS) {
        if (response.error_code == ERR_FILE_EXISTS) {
            fprintf(stderr, "Error: Folder '%s' already exists\n", foldername);
        } else {
            fprintf(stderr, "Error: %s\n", response.data);
        }
        close_socket(sockfd);
        return -1;
    }
    
    printf("Folder '%s' created successfully\n", foldername);
    close_socket(sockfd);
    return 0;
}

// Send MOVE request to Name Server
int send_move_request(const char *filename, const char *target_folder) {
    int sockfd = connect_to_nm();
    if (sockfd < 0) return -1;
    
    Message request;
    memset(&request, 0, sizeof(Message));
    request.msg_type = MSG_REQUEST;
    request.operation = OP_MOVE;
    strncpy(request.username, client_config.username, MAX_USERNAME - 1);
    strncpy(request.filename, filename, MAX_FILENAME - 1);
    strncpy(request.target_path, target_folder, MAX_PATH - 1);
    
    if (send_message(sockfd, &request) < 0) {
        fprintf(stderr, "Failed to send MOVE request\n");
        close_socket(sockfd);
        return -1;
    }
    
    Message response;
    if (receive_message(sockfd, &response) < 0) {
        fprintf(stderr, "Failed to receive MOVE response\n");
        close_socket(sockfd);
        return -1;
    }
    
    if (response.msg_type == MSG_ERROR) {
        if (response.error_code == ERR_FILE_NOT_FOUND) {
            fprintf(stderr, "Error: File '%s' not found\n", filename);
        } else if (response.error_code == ERR_ACCESS_DENIED) {
            fprintf(stderr, "Error: Access denied for '%s'\n", filename);
        } else {
            fprintf(stderr, "Error: %d\n", response.error_code);
        }
        close_socket(sockfd);
        return -1;
    }
    
    printf("File '%s' moved to '%s' successfully\n", filename, target_folder);
    close_socket(sockfd);
    return 0;
}

// Send VIEWFOLDER request to Name Server
int send_viewfolder_request(const char *foldername) {
    int sockfd = connect_to_nm();
    if (sockfd < 0) return -1;
    
    Message request;
    memset(&request, 0, sizeof(Message));
    request.msg_type = MSG_REQUEST;
    request.operation = OP_VIEWFOLDER;
    strncpy(request.username, client_config.username, MAX_USERNAME - 1);
    strncpy(request.filename, foldername, MAX_FILENAME - 1);
    
    if (send_message(sockfd, &request) < 0) {
        fprintf(stderr, "Failed to send VIEWFOLDER request\n");
        close_socket(sockfd);
        return -1;
    }
    
    Message response;
    if (receive_message(sockfd, &response) < 0) {
        fprintf(stderr, "Failed to receive VIEWFOLDER response\n");
        close_socket(sockfd);
        return -1;
    }
    
    if (response.msg_type == MSG_ERROR) {
        if (response.error_code == ERR_FILE_NOT_FOUND) {
            fprintf(stderr, "Error: Folder '%s' not found\n", foldername);
        } else {
            fprintf(stderr, "Error: %d\n", response.error_code);
        }
        close_socket(sockfd);
        return -1;
    }
    
    // Display folder contents
    printf("Contents of folder '%s':\n%s\n", foldername, response.data);
    close_socket(sockfd);
    return 0;
}

// Send CHECKPOINT request to Name Server
int send_checkpoint_request(const char *filename, const char *checkpoint_tag) {
    int sockfd = connect_to_nm();
    if (sockfd < 0) return -1;
    
    Message request;
    memset(&request, 0, sizeof(Message));
    request.msg_type = MSG_REQUEST;
    request.operation = OP_CHECKPOINT;
    strncpy(request.username, client_config.username, MAX_USERNAME - 1);
    strncpy(request.filename, filename, MAX_FILENAME - 1);
    strncpy(request.checkpoint_tag, checkpoint_tag, MAX_FILENAME - 1);
    
    if (send_message(sockfd, &request) < 0) {
        fprintf(stderr, "Failed to send CHECKPOINT request\n");
        close_socket(sockfd);
        return -1;
    }
    
    Message response;
    if (receive_message(sockfd, &response) < 0) {
        fprintf(stderr, "Failed to receive CHECKPOINT response\n");
        close_socket(sockfd);
        return -1;
    }
    
    if (response.msg_type == MSG_ERROR) {
        if (response.error_code == ERR_FILE_NOT_FOUND) {
            fprintf(stderr, "Error: File '%s' not found\n", filename);
        } else if (response.error_code == ERR_NO_READ_ACCESS) {
            fprintf(stderr, "Error: No read access to file '%s'\n", filename);
        } else {
            fprintf(stderr, "Error: %d\n", response.error_code);
        }
        close_socket(sockfd);
        return -1;
    }
    
    printf("Checkpoint '%s' created for file '%s'\n", checkpoint_tag, filename);
    close_socket(sockfd);
    return 0;
}

// Send VIEWCHECKPOINT request to Name Server
int send_viewcheckpoint_request(const char *filename, const char *checkpoint_tag) {
    int sockfd = connect_to_nm();
    if (sockfd < 0) return -1;
    
    Message request;
    memset(&request, 0, sizeof(Message));
    request.msg_type = MSG_REQUEST;
    request.operation = OP_VIEWCHECKPOINT;
    strncpy(request.username, client_config.username, MAX_USERNAME - 1);
    strncpy(request.filename, filename, MAX_FILENAME - 1);
    strncpy(request.checkpoint_tag, checkpoint_tag, MAX_FILENAME - 1);
    
    if (send_message(sockfd, &request) < 0) {
        fprintf(stderr, "Failed to send VIEWCHECKPOINT request\n");
        close_socket(sockfd);
        return -1;
    }
    
    Message response;
    if (receive_message(sockfd, &response) < 0) {
        fprintf(stderr, "Failed to receive VIEWCHECKPOINT response\n");
        close_socket(sockfd);
        return -1;
    }
    
    if (response.msg_type == MSG_ERROR) {
        if (response.error_code == ERR_FILE_NOT_FOUND) {
            fprintf(stderr, "Error: Checkpoint '%s' not found for file '%s'\n", checkpoint_tag, filename);
        } else if (response.error_code == ERR_NO_READ_ACCESS) {
            fprintf(stderr, "Error: No read access to file '%s'\n", filename);
        } else {
            fprintf(stderr, "Error: %d\n", response.error_code);
        }
        close_socket(sockfd);
        return -1;
    }
    
    // Display checkpoint content
    printf("=== Checkpoint '%s' for '%s' ===\n%s\n", checkpoint_tag, filename, response.data);
    close_socket(sockfd);
    return 0;
}

// Send REVERT request to Name Server
int send_revert_request(const char *filename, const char *checkpoint_tag) {
    int sockfd = connect_to_nm();
    if (sockfd < 0) return -1;
    
    Message request;
    memset(&request, 0, sizeof(Message));
    request.msg_type = MSG_REQUEST;
    request.operation = OP_REVERT;
    strncpy(request.username, client_config.username, MAX_USERNAME - 1);
    strncpy(request.filename, filename, MAX_FILENAME - 1);
    strncpy(request.checkpoint_tag, checkpoint_tag, MAX_FILENAME - 1);
    
    if (send_message(sockfd, &request) < 0) {
        fprintf(stderr, "Failed to send REVERT request\n");
        close_socket(sockfd);
        return -1;
    }
    
    Message response;
    if (receive_message(sockfd, &response) < 0) {
        fprintf(stderr, "Failed to receive REVERT response\n");
        close_socket(sockfd);
        return -1;
    }
    
    if (response.msg_type == MSG_ERROR ) {
        if (response.error_code == ERR_FILE_NOT_FOUND) {
            fprintf(stderr, "Error: Checkpoint '%s' not found for file '%s'\n", checkpoint_tag, filename);
        } else if (response.error_code == ERR_NO_WRITE_ACCESS) {
            fprintf(stderr, "Error: No write access to file '%s'\n", filename);
        } else {
            fprintf(stderr, "Error: %d\n", response.error_code);
        }
        close_socket(sockfd);
        return -1;
    }
    
    printf("File '%s' reverted to checkpoint '%s' successfully\n", filename, checkpoint_tag);
    close_socket(sockfd);
    return 0;
}

// Send LISTCHECKPOINTS request to Name Server
int send_listcheckpoints_request(const char *filename) {
    int sockfd = connect_to_nm();
    if (sockfd < 0) return -1;
    
    Message request;
    memset(&request, 0, sizeof(Message));
    request.msg_type = MSG_REQUEST;
    request.operation = OP_LISTCHECKPOINTS;
    strncpy(request.username, client_config.username, MAX_USERNAME - 1);
    strncpy(request.filename, filename, MAX_FILENAME - 1);
    
    if (send_message(sockfd, &request) < 0) {
        fprintf(stderr, "Failed to send LISTCHECKPOINTS request\n");
        close_socket(sockfd);
        return -1;
    }
    
    Message response;
    if (receive_message(sockfd, &response) < 0) {
        fprintf(stderr, "Failed to receive LISTCHECKPOINTS response\n");
        close_socket(sockfd);
        return -1;
    }
    
    if (response.msg_type == MSG_ERROR) {
        if (response.error_code == ERR_FILE_NOT_FOUND) {
            fprintf(stderr, "Error: File '%s' not found\n", filename);
        } else if (response.error_code == ERR_NO_READ_ACCESS) {
            fprintf(stderr, "Error: No read access to file '%s'\n", filename);
        } else {
            fprintf(stderr, "Error: %d\n", response.error_code);
        }
        close_socket(sockfd);
        return -1;
    }
    
    // Display checkpoints list
    printf("Checkpoints for file '%s':\n%s\n", filename, response.data);
    close_socket(sockfd);
    return 0;
}

// Send REQUESTACCESS request to Name Server
int send_requestaccess_request(const char *filename, int access_type) {
    int sockfd = connect_to_nm();
    if (sockfd < 0) return -1;
    
    Message request;
    memset(&request, 0, sizeof(Message));
    request.msg_type = MSG_REQUEST;
    request.operation = OP_REQUESTACCESS;
    strncpy(request.username, client_config.username, MAX_USERNAME - 1);
    strncpy(request.filename, filename, MAX_FILENAME - 1);
    request.sentence_index = access_type;  // Use sentence_index for access type
    
    if (send_message(sockfd, &request) < 0) {
        fprintf(stderr, "Failed to send REQUESTACCESS request\n");
        close_socket(sockfd);
        return -1;
    }
    
    Message response;
    if (receive_message(sockfd, &response) < 0) {
        fprintf(stderr, "Failed to receive REQUESTACCESS response\n");
        close_socket(sockfd);
        return -1;
    }
    
    if (response.msg_type == MSG_ERROR) {
        if (response.error_code == ERR_FILE_NOT_FOUND) {
            fprintf(stderr, "Error: File '%s' not found\n", filename);
        } else {
            fprintf(stderr, "Error: %d - %s\n", response.error_code, response.data);
        }
        close_socket(sockfd);
        return -1;
    }
    
    printf("%s\n", response.data);
    close_socket(sockfd);
    return 0;
}

// Send VIEWREQUESTS request to Name Server
int send_viewrequests_request() {
    int sockfd = connect_to_nm();
    if (sockfd < 0) return -1;
    
    Message request;
    memset(&request, 0, sizeof(Message));
    request.msg_type = MSG_REQUEST;
    request.operation = OP_VIEWREQUESTS;
    strncpy(request.username, client_config.username, MAX_USERNAME - 1);
    
    if (send_message(sockfd, &request) < 0) {
        fprintf(stderr, "Failed to send VIEWREQUESTS request\n");
        close_socket(sockfd);
        return -1;
    }
    
    Message response;
    if (receive_message(sockfd, &response) < 0) {
        fprintf(stderr, "Failed to receive VIEWREQUESTS response\n");
        close_socket(sockfd);
        return -1;
    }
    
    if (response.msg_type == MSG_ERROR) {
        fprintf(stderr, "Error: %d - %s\n", response.error_code, response.data);
        close_socket(sockfd);
        return -1;
    }
    
    printf("=== Pending Access Requests ===\n%s\n", response.data);
    close_socket(sockfd);
    return 0;
}

// Send APPROVEREQUEST request to Name Server
int send_approverequest_request(const char *request_id) {
    int sockfd = connect_to_nm();
    if (sockfd < 0) return -1;
    
    Message request;
    memset(&request, 0, sizeof(Message));
    request.msg_type = MSG_REQUEST;
    request.operation = OP_APPROVEREQUEST;
    strncpy(request.username, client_config.username, MAX_USERNAME - 1);
    strncpy(request.checkpoint_tag, request_id, MAX_FILENAME - 1);  // Reuse checkpoint_tag for request_id
    
    if (send_message(sockfd, &request) < 0) {
        fprintf(stderr, "Failed to send APPROVEREQUEST request\n");
        close_socket(sockfd);
        return -1;
    }
    
    Message response;
    if (receive_message(sockfd, &response) < 0) {
        fprintf(stderr, "Failed to receive APPROVEREQUEST response\n");
        close_socket(sockfd);
        return -1;
    }
    
    if (response.msg_type == MSG_ERROR) {
        fprintf(stderr, "Error: %d - %s\n", response.error_code, response.data);
        close_socket(sockfd);
        return -1;
    }
    
    printf("%s\n", response.data);
    close_socket(sockfd);
    return 0;
}

// Send REJECTREQUEST request to Name Server
int send_rejectrequest_request(const char *request_id) {
    int sockfd = connect_to_nm();
    if (sockfd < 0) return -1;
    
    Message request;
    memset(&request, 0, sizeof(Message));
    request.msg_type = MSG_REQUEST;
    request.operation = OP_REJECTREQUEST;
    strncpy(request.username, client_config.username, MAX_USERNAME - 1);
    strncpy(request.checkpoint_tag, request_id, MAX_FILENAME - 1);  // Reuse checkpoint_tag for request_id
    
    if (send_message(sockfd, &request) < 0) {
        fprintf(stderr, "Failed to send REJECTREQUEST request\n");
        close_socket(sockfd);
        return -1;
    }
    
    Message response;
    if (receive_message(sockfd, &response) < 0) {
        fprintf(stderr, "Failed to receive REJECTREQUEST response\n");
        close_socket(sockfd);
        return -1;
    }
    
    if (response.msg_type == MSG_ERROR) {
        fprintf(stderr, "Error: %d - %s\n", response.error_code, response.data);
        close_socket(sockfd);
        return -1;
    }
    
    printf("%s\n", response.data);
    close_socket(sockfd);
    return 0;
}
