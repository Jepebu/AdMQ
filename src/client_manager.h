#ifndef CLIENT_MANAGER_H
#define CLIENT_MANAGER_H

#define MAX_CLIENTS 100

#define STATE_DISCONNECTED 0
#define STATE_IDLE 1
#define STATE_PROCESSING 2

#define CONN_VAULT 0
#define CONN_LOBBY 1

#include <openssl/ssl.h>
#include <time.h>

// Initialize the manager
void client_manager_init();

// Update the client_add prototype to accept the connection type
int client_add(int fd, int conn_type);

// Safely removes a client
void client_remove(int index);

// Thread-safe getter/setter methods

void client_set_state(int index, int state);
int client_get_state(int index);
//
void client_buffer_append(int index, const char* data, int len);
int client_buffer_extract_line(int index, char* out_message, int max_len);
//
int client_get_auth(int index);
void client_set_auth(int index, int status);
//
void client_set_ssl(int index, SSL* ssl);
SSL* client_get_ssl(int index);
//
void client_set_hostname(int index, const char* hostname);
void client_get_hostname(int index, char* out_name, int max_len);
//
void client_update_activity(int index);
time_t client_get_activity(int index);

int client_get_socket(int index);

int client_get_conn_type(int index);

// Method for printing status info
void client_manager_print_status();


#endif
