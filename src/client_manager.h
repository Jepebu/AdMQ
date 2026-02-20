#ifndef CLIENT_MANAGER_H
#define CLIENT_MANAGER_H

#define MAX_CLIENTS 100

#define STATE_DISCONNECTED 0
#define STATE_IDLE 1
#define STATE_PROCESSING 2

// Initialize the manager (clears arrays and inits mutex)
void client_manager_init();

// Adds a new client and returns their index (or -1 if full)
int client_add(int fd);

// Safely updates a client's state
void client_set_state(int index, int state);

// Safely removes a client
void client_remove(int index);

// Thread-safe getters
int client_get_socket(int index);
int client_get_state(int index);

void client_buffer_append(int index, const char* data, int len);
int client_buffer_extract_line(int index, char* out_message, int max_len);

int client_get_auth(int index);
void client_set_auth(int index, int status);

#endif
