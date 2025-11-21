#include "network_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

// Create a TCP socket
int create_socket() {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        print_error("Error creating socket");
        return -1;
    }
    
    // Set socket option to reuse address
    int opt = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        print_error("Error setting socket options");
        close(sockfd);
        return -1;
    }
    
    return sockfd;
}

// Bind socket to a specific port
int bind_socket(int sockfd, int port) {
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    
    if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        print_error("Error binding socket");
        return -1;
    }
    
    return 0;
}

// Put socket in listening mode
int listen_socket(int sockfd, int backlog) {
    if (listen(sockfd, backlog) < 0) {
        print_error("Error listening on socket");
        return -1;
    }
    return 0;
}

// Accept an incoming connection
int accept_connection(int sockfd, char *client_ip) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    int client_sockfd = accept(sockfd, (struct sockaddr*)&client_addr, &client_len);
    if (client_sockfd < 0) {
        print_error("Error accepting connection");
        return -1;
    }
    
    // Store client IP if buffer is provided
    if (client_ip != NULL) {
        inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, MAX_IP_LEN);
    }
    
    return client_sockfd;
}

// Connect to a server
int connect_to_server(const char *ip, int port) {
    int sockfd = create_socket();
    if (sockfd < 0) {
        return -1;
    }
    
    // CRITICAL FIX: Set generous timeout to handle slow operations
    // 60-second timeout for both send and receive operations
    // This allows large file transfers and slow user input during WRITE operations
    struct timeval timeout;
    timeout.tv_sec = 60;  // Increased from 5 to 60 seconds
    timeout.tv_usec = 0;
    
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        print_error("Warning: Failed to set receive timeout");
        // Don't fail - continue with no timeout
    }
    
    if (setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
        print_error("Warning: Failed to set send timeout");
        // Don't fail - continue with no timeout
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0) {
        print_error("Invalid IP address");
        close(sockfd);
        return -1;
    }
    
    if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        print_error("Connection failed");
        close(sockfd);
        return -1;
    }
    
    return sockfd;
}

// CRITICAL FIX: Endianness conversion for cross-architecture support
// Convert Message integer fields to network byte order (Big Endian) before sending
void message_to_network_order(Message *msg) {
    if (msg == NULL) return;
    
    msg->msg_type = htonl(msg->msg_type);
    msg->operation = htonl(msg->operation);
    msg->error_code = htonl(msg->error_code);
    msg->sentence_index = htonl(msg->sentence_index);
    msg->word_index = htonl(msg->word_index);
    msg->port1 = htonl(msg->port1);
    msg->port2 = htonl(msg->port2);
    msg->ss_id = htonl(msg->ss_id);
    msg->backup_port = htonl(msg->backup_port);
}

// Convert Message integer fields to host byte order (native) after receiving
void message_to_host_order(Message *msg) {
    if (msg == NULL) return;
    
    msg->msg_type = ntohl(msg->msg_type);
    msg->operation = ntohl(msg->operation);
    msg->error_code = ntohl(msg->error_code);
    msg->sentence_index = ntohl(msg->sentence_index);
    msg->word_index = ntohl(msg->word_index);
    msg->port1 = ntohl(msg->port1);
    msg->port2 = ntohl(msg->port2);
    msg->ss_id = ntohl(msg->ss_id);
    msg->backup_port = ntohl(msg->backup_port);
}

// Send a Message structure over socket
int send_message(int sockfd, Message *msg) {
    if (msg == NULL) {
        fprintf(stderr, "Error: NULL message pointer\n");
        return -1;
    }
    
    // CRITICAL FIX: Create a copy and convert to network byte order
    // This prevents modifying the caller's message structure
    Message network_msg;
    memcpy(&network_msg, msg, sizeof(Message));
    message_to_network_order(&network_msg);
    
    int total_sent = 0;
    int bytes_to_send = sizeof(Message);
    char *msg_ptr = (char*)&network_msg;
    
    while (total_sent < bytes_to_send) {
        int sent = send(sockfd, msg_ptr + total_sent, bytes_to_send - total_sent, 0);
        if (sent < 0) {
            // CRITICAL FIX: Retry on EINTR (interrupted system call)
            if (errno == EINTR) {
                continue;  // Retry the send
            }
            print_error("Error sending message");
            return -1;
        }
        if (sent == 0) {
            fprintf(stderr, "Connection closed by peer\n");
            return -1;
        }
        total_sent += sent;
    }
    
    return total_sent;
}

// Receive a Message structure from socket
int receive_message(int sockfd, Message *msg) {
    if (msg == NULL) {
        fprintf(stderr, "Error: NULL message pointer\n");
        return -1;
    }
    
    int total_received = 0;
    int bytes_to_receive = sizeof(Message);
    char *msg_ptr = (char*)msg;
    
    while (total_received < bytes_to_receive) {
        int received = recv(sockfd, msg_ptr + total_received, bytes_to_receive - total_received, 0);
        if (received < 0) {
            // CRITICAL FIX: Handle different error types appropriately
            if (errno == EINTR) {
                continue;  // Retry on interrupted system call
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Timeout occurred - this is rare with 60s timeout
                // Could indicate network congestion or very slow operation
                fprintf(stderr, "Warning: Receive timeout (60s) - connection may be slow\n");
                continue;  // Retry once more
            }
            print_error("Error receiving message");
            return -1;
        }
        if (received == 0) {
            fprintf(stderr, "Connection closed by peer\n");
            return -1;
        }
        total_received += received;
    }
    
    // CRITICAL FIX: Convert from network byte order to host byte order
    message_to_host_order(msg);
    
    return total_received;
}

// Close socket
void close_socket(int sockfd) {
    if (sockfd >= 0) {
        close(sockfd);
    }
}

// Print error message
void print_error(const char *msg) {
    perror(msg);
}
