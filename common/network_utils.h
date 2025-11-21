#ifndef NETWORK_UTILS_H
#define NETWORK_UTILS_H

#include "protocol.h"

// Create a TCP socket
int create_socket();

// Bind socket to a specific port
int bind_socket(int sockfd, int port);

// Put socket in listening mode
int listen_socket(int sockfd, int backlog);

// Accept an incoming connection
int accept_connection(int sockfd, char *client_ip);

// Connect to a server
int connect_to_server(const char *ip, int port);

// Convert Message fields to network byte order (before sending)
void message_to_network_order(Message *msg);

// Convert Message fields to host byte order (after receiving)
void message_to_host_order(Message *msg);

// Send a Message structure over socket
int send_message(int sockfd, Message *msg);

// Receive a Message structure from socket
int receive_message(int sockfd, Message *msg);

// Close socket
void close_socket(int sockfd);

// Print error message
void print_error(const char *msg);

#endif // NETWORK_UTILS_H
