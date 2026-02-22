#include "client_manager.h"
#include "auth.h"
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

char client_buffers[MAX_CLIENTS][2048];
int client_buffer_lens[MAX_CLIENTS];
int client_sockets[MAX_CLIENTS];
int client_states[MAX_CLIENTS];
pthread_mutex_t clients_lock;
int client_auth_status[MAX_CLIENTS];
SSL* client_ssl[MAX_CLIENTS];
int client_conn_types[MAX_CLIENTS];
char client_hostnames[MAX_CLIENTS][128];
time_t client_last_activity[MAX_CLIENTS];

void client_manager_init() {
    pthread_mutex_init(&clients_lock, NULL);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_sockets[i] = 0;
        client_buffer_lens[i] = 0;
        client_states[i] = STATE_DISCONNECTED;
        client_ssl[i] = NULL;
        client_conn_types[i] = CONN_VAULT;
    }
}

int client_add(int fd, int conn_type) {
    pthread_mutex_lock(&clients_lock);
    int assigned_index = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_states[i] == STATE_DISCONNECTED) {
            client_sockets[i] = fd;
            client_states[i] = STATE_IDLE;
            client_conn_types[i] = conn_type;
            assigned_index = i;
            break;
        }
    }
    pthread_mutex_unlock(&clients_lock);
    return assigned_index;
}

void client_set_state(int index, int state) {
    pthread_mutex_lock(&clients_lock);
    if (index >= 0 && index < MAX_CLIENTS) {
        client_states[index] = state;
    }
    pthread_mutex_unlock(&clients_lock);
}

void client_remove(int index) {
    pthread_mutex_lock(&clients_lock);
    if (index >= 0 && index < MAX_CLIENTS) {

        // Free OpenSSL Session
        if (client_ssl[index] != NULL) {
            SSL_shutdown(client_ssl[index]); // Politely tell the client we are closing the tunnel
            SSL_free(client_ssl[index]);     // Free the memory
            client_ssl[index] = NULL;
        }

        if (client_sockets[index] > 0) {
            close(client_sockets[index]);
            client_sockets[index] = 0;
            client_buffer_lens[index] = 0;
        }

        client_states[index] = STATE_DISCONNECTED;
    }
    pthread_mutex_unlock(&clients_lock);
}

// Getter & Setter methods


void client_get_hostname(int index, char* out_name, int max_len) {
    pthread_mutex_lock(&clients_lock);
    if (index >= 0 && index < MAX_CLIENTS && strlen(client_hostnames[index]) > 0) {
        strncpy(out_name, client_hostnames[index], max_len - 1);
        out_name[max_len - 1] = '\0';
    } else {
        strncpy(out_name, "Unknown", max_len - 1);
        out_name[max_len - 1] = '\0';
    }
    pthread_mutex_unlock(&clients_lock);
}

void client_set_hostname(int index, const char* hostname) {
    pthread_mutex_lock(&clients_lock);
    if (index >= 0 && index < MAX_CLIENTS) {
        strncpy(client_hostnames[index], hostname, 127);
        client_hostnames[index][127] = '\0'; // Ensure null-termination
    }
    pthread_mutex_unlock(&clients_lock);
}


void client_update_activity(int index) {
    pthread_mutex_lock(&clients_lock);
    if (index >= 0 && index < MAX_CLIENTS) {
        client_last_activity[index] = time(NULL);
    }
    pthread_mutex_unlock(&clients_lock);
}

time_t client_get_activity(int index) {
    pthread_mutex_lock(&clients_lock);
    time_t activity = 0;
    if (index >= 0 && index < MAX_CLIENTS) {
        activity = client_last_activity[index];
    }
    pthread_mutex_unlock(&clients_lock);
    return activity;
}

int client_get_conn_type(int index) {
    pthread_mutex_lock(&clients_lock);
    int type = CONN_VAULT;
    if (index >= 0 && index < MAX_CLIENTS) {
        type = client_conn_types[index];
    }
    pthread_mutex_unlock(&clients_lock);
    return type;
}

int client_get_socket(int index) {
    pthread_mutex_lock(&clients_lock);
    int fd = client_sockets[index];
    pthread_mutex_unlock(&clients_lock);
    return fd;
}

int client_get_state(int index) {
    pthread_mutex_lock(&clients_lock);
    int state = client_states[index];
    pthread_mutex_unlock(&clients_lock);
    return state;
}

void client_set_auth(int index, int status) {
    pthread_mutex_lock(&clients_lock);
    if (index >= 0 && index < MAX_CLIENTS) {
        client_auth_status[index] = status;
    }
    pthread_mutex_unlock(&clients_lock);
}

int client_get_auth(int index) {
    pthread_mutex_lock(&clients_lock);
    int status = AUTH_PENDING;
    if (index >= 0 && index < MAX_CLIENTS) {
        status = client_auth_status[index];
    }
    pthread_mutex_unlock(&clients_lock);
    return status;
}

void client_set_ssl(int index, SSL* ssl) {
    pthread_mutex_lock(&clients_lock);
    if (index >= 0 && index < MAX_CLIENTS) {
        client_ssl[index] = ssl;
    }
    pthread_mutex_unlock(&clients_lock);
}

SSL* client_get_ssl(int index) {
    pthread_mutex_lock(&clients_lock);
    SSL* ssl = NULL;
    if (index >= 0 && index < MAX_CLIENTS) {
        ssl = client_ssl[index];
    }
    pthread_mutex_unlock(&clients_lock);
    return ssl;
}


void client_buffer_append(int index, const char* data, int len) {
    // Note: We don't need a mutex here - because the client is in STATE_PROCESSING
    // only ONE worker thread is allowed to touch this specific index at a time.

    int current_len = client_buffer_lens[index];

    // Prevent buffer overflow
    if (current_len + len >= 2048) {
        printf("Warning: Client %d buffer overflow. Dropping data.\n", index);
        client_buffer_lens[index] = 0; // Reset on overflow
        return;
    }

    // Copy the new data to the end of the existing buffer
    memcpy(&client_buffers[index][current_len], data, len);
    client_buffer_lens[index] += len;

    // Ensure it's null-terminated for safety during string searches
    client_buffers[index][client_buffer_lens[index]] = '\0';
}

int client_buffer_extract_line(int index, char* out_message, int max_len) {
    int current_len = client_buffer_lens[index];
    if (current_len == 0) return 0; // Buffer is empty

    // Search for the newline character
    char* newline_ptr = strchr(client_buffers[index], '\n');

    if (newline_ptr == NULL) {
        // No complete message yet. We must wait for the client to send more data.
        return 0;
    }

    // Calculate how long this specific message is
    int msg_len = newline_ptr - client_buffers[index];

    // Copy the message into the output variable (safely)
    int copy_len = (msg_len < max_len - 1) ? msg_len : max_len - 1;
    strncpy(out_message, client_buffers[index], copy_len);
    out_message[copy_len] = '\0'; // Null-terminate the extracted string

    // Remove this message from the buffer by sliding the remaining data to the left.
    int bytes_to_remove = msg_len + 1; // +1 to remove the '\n' itself
    int remaining_bytes = current_len - bytes_to_remove;

    if (remaining_bytes > 0) {
        // memmove is safe for overlapping memory regions
        memmove(client_buffers[index], &client_buffers[index][bytes_to_remove], remaining_bytes);
    }

    client_buffer_lens[index] = remaining_bytes;
    client_buffers[index][remaining_bytes] = '\0';

    return 1; // Success
}


void client_manager_print_status() {
    pthread_mutex_lock(&clients_lock);
    printf("\n=== CONNECTED AGENTS ===\n");
    int count = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_states[i] != STATE_DISCONNECTED) {
            count++;
            char* name = (strlen(client_hostnames[i]) > 0) ? client_hostnames[i] : "Unknown/Pending";

            // Print the internal ID, the Hostname, and the raw Socket FD
            printf("  [ID: %d] %s (FD: %d)\n", i, name, client_sockets[i]);
        }
    }
    if (count == 0) printf("  No agents connected.\n");
    printf("========================\n");
    pthread_mutex_unlock(&clients_lock);
}
