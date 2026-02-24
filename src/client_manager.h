#ifndef CLIENT_MANAGER_H
#define CLIENT_MANAGER_H

#define STATE_DISCONNECTED 0
#define STATE_IDLE 1

#define CONN_VAULT 0
#define CONN_LOBBY 1

#include <openssl/ssl.h>
#include <pthread.h>
#include <time.h>

// Client struct holding all individual device information and its internal mutex
typedef struct Client {
    int fd;
    int state;
    int conn_type;
    int auth_status;
    SSL* ssl;
    char hostname[128];
    time_t last_activity;

    char buffer[2048];
    int buffer_len;

    pthread_mutex_t lock;
} Client;

void client_manager_init();
void client_add(int fd, int conn_type);
void client_remove(int fd);

// These retrieve a pointer to the client and automatically lock c->lock for safe multithreaded operations
Client* client_get_and_lock_by_fd(int fd);
Client* client_get_and_lock_by_hostname(const char* hostname);
void client_unlock(Client* c);

void client_set_hostname(int fd, const char* hostname);
void client_manager_sweep_inactive(int timeout_seconds);
void client_manager_print_status();

// Buffer management (should only be called when c->lock is held)
void client_buffer_append(Client* c, const char* data, int len);
int client_buffer_extract_line(Client* c, char* out_message, int max_len);

#endif
