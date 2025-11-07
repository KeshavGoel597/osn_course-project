#ifndef SS_NM_COMM_H
#define SS_NM_COMM_H

#include "../common/protocol.h"

// Register storage server with Name Server
int register_with_nm(int nm_port, int client_port, const char *ss_ip);

// Handle requests from Name Server
void* handle_nm_connection(void *arg);

// Send acknowledgment to Name Server
int send_ack_to_nm(int nm_sockfd, int operation, int error_code);

#endif // SS_NM_COMM_H
