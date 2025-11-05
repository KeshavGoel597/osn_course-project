#include "client_nm_comm.h"
#include "client.h"
#include "../common/network_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Helper to connect to Name Server
static int connect_to_nm() {
    int sockfd = connect_to_server(NM_IP, NM_PORT);
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
    
    if (response.msg_type == MSG_ERROR) {
        fprintf(stderr, "Error: %d\n", response.error_code);
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
    
    if (response.msg_type == MSG_ERROR) {
        fprintf(stderr, "Error: %d\n", response.error_code);
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
    
    if (response.msg_type == MSG_ERROR) {
        if (response.error_code == ERR_FILE_NOT_FOUND) {
            fprintf(stderr, "Error: File '%s' not found\n", filename);
        } else {
            fprintf(stderr, "Error: %d\n", response.error_code);
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
    
    Message response;
    if (receive_message(sockfd, &response) < 0) {
        fprintf(stderr, "Failed to receive ADDACCESS response\n");
        close_socket(sockfd);
        return -1;
    }
    
    if (response.msg_type == MSG_ERROR) {
        if (response.error_code == ERR_NOT_OWNER) {
            fprintf(stderr, "Error: Only the owner can modify access\n");
        } else if (response.error_code == ERR_FILE_NOT_FOUND) {
            fprintf(stderr, "Error: File '%s' not found\n", filename);
        } else {
            fprintf(stderr, "Error: %d\n", response.error_code);
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
    
    Message response;
    if (receive_message(sockfd, &response) < 0) {
        fprintf(stderr, "Failed to receive REMACCESS response\n");
        close_socket(sockfd);
        return -1;
    }
    
    if (response.msg_type == MSG_ERROR) {
        if (response.error_code == ERR_NOT_OWNER) {
            fprintf(stderr, "Error: Only the owner can modify access\n");
        } else if (response.error_code == ERR_FILE_NOT_FOUND) {
            fprintf(stderr, "Error: File '%s' not found\n", filename);
        } else {
            fprintf(stderr, "Error: %d\n", response.error_code);
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
    
    if (response.msg_type == MSG_ERROR) {
        if (response.error_code == ERR_FILE_EXISTS) {
            fprintf(stderr, "Error: File '%s' already exists\n", filename);
        } else {
            fprintf(stderr, "Error: %d\n", response.error_code);
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
    
    if (response.msg_type == MSG_ERROR) {
        if (response.error_code == ERR_FILE_NOT_FOUND) {
            fprintf(stderr, "Error: File '%s' not found\n", filename);
        } else if (response.error_code == ERR_NOT_OWNER) {
            fprintf(stderr, "Error: Only the owner can delete the file\n");
        } else {
            fprintf(stderr, "Error: %d\n", response.error_code);
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
    
    Message response;
    if (receive_message(sockfd, &response) < 0) {
        fprintf(stderr, "Failed to receive EXEC response\n");
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
    
    // Display execution output
    printf("%s\n", response.data);
    
    close_socket(sockfd);
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
    
    if (response.msg_type == MSG_ERROR) {
        if (response.error_code == ERR_FILE_NOT_FOUND) {
            fprintf(stderr, "Error: File '%s' not found\n", filename);
        } else {
            fprintf(stderr, "Error: %d\n", response.error_code);
        }
        close_socket(sockfd);
        return -1;
    }
    
    // Extract SS IP and port from response
    strncpy(ss_ip, response.ip, MAX_IP_LEN - 1);
    *ss_port = response.port2;  // Client port of SS
    
    close_socket(sockfd);
    return 0;
}
