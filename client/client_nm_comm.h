#ifndef CLIENT_NM_COMM_H
#define CLIENT_NM_COMM_H

#include "../common/protocol.h"
#include "command_parser.h"

// Send VIEW request to Name Server
int send_view_request(int view_flags);

// Send LIST request to Name Server
int send_list_request();

// Send INFO request to Name Server
int send_info_request(const char *filename);

// Send ADDACCESS request to Name Server
int send_addaccess_request(const char *filename, const char *target_user, int access_type);

// Send REMACCESS request to Name Server
int send_remaccess_request(const char *filename, const char *target_user);

// Send CREATE request to Name Server
int send_create_request(const char *filename);

// Send DELETE request to Name Server
int send_delete_request(const char *filename);

// Send EXEC request to Name Server
int send_exec_request(const char *filename);

// Get Storage Server info for a file (for direct operations)
int get_ss_info(const char *filename, char *ss_ip, int *ss_port);

#endif // CLIENT_NM_COMM_H
