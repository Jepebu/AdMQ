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



void client_manager_init() {
    pthread_mutex_init(&clients_lock, NULL);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_sockets[i] = 0;
        client_buffer_lens[i] = 0;
        client_states[i] = STATE_DISCONNECTED;
    }
}

int client_add(int fd) {
    pthread_mutex_lock(&clients_lock);
    int assigned_index = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_states[i] == STATE_DISCONNECTED) {
            client_sockets[i] = fd;
            client_states[i] = STATE_IDLE;
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
        if (client_sockets[index] > 0) {
            close(client_sockets[index]);
            client_sockets[index] = 0;
            client_buffer_lens[index] = 0;
        }
        client_states[index] = STATE_DISCONNECTED;
    }
    pthread_mutex_unlock(&clients_lock);
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


void client_buffer_append(int index, const char* data, int len) {
    // Note: We don't need a mutex here! Because the client is in STATE_PROCESSING,
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

    // Now, we must remove this message from the buffer by sliding the remaining data to the left!
    int bytes_to_remove = msg_len + 1; // +1 to remove the '\n' itself
    int remaining_bytes = current_len - bytes_to_remove;

    if (remaining_bytes > 0) {
        // memmove is safe for overlapping memory regions
        memmove(client_buffers[index], &client_buffers[index][bytes_to_remove], remaining_bytes);
    }

    client_buffer_lens[index] = remaining_bytes;
    client_buffers[index][remaining_bytes] = '\0';

    return 1; // Success! We extracted a line.
}
