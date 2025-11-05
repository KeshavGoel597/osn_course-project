#ifndef CLIENT_SS_COMM_H
#define CLIENT_SS_COMM_H

#include "../common/protocol.h"

// Send READ request directly to Storage Server
int send_read_request(const char *filename);

// Send WRITE request directly to Storage Server
int send_write_request(const char *filename, int sentence_index);

// Send STREAM request directly to Storage Server
int send_stream_request(const char *filename);

// Send UNDO request (through Name Server, but may involve SS)
int send_undo_request(const char *filename);

#endif // CLIENT_SS_COMM_H
