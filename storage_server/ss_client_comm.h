#ifndef SS_CLIENT_COMM_H
#define SS_CLIENT_COMM_H

#include "../common/protocol.h"

// Handle client connection (runs in separate thread)
void* handle_client_connection(void *arg);

// Handle READ operation from client
int handle_read_request(int client_sockfd, Message *msg);

// Handle WRITE operation from client
int handle_write_request(int client_sockfd, Message *msg);

// Handle STREAM operation from client
int handle_stream_request(int client_sockfd, Message *msg);

#endif // SS_CLIENT_COMM_H
