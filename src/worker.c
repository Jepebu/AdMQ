#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <sys/epoll.h>
#include <errno.h>

#include "rbac.h"
#include "db.h"
#include "enroll.h"
#include "tls.h"
#include "auth.h"
#include "worker.h"
#include "client_manager.h"
#include "pubsub.h"

extern int epoll_fd; // Pull the global epoll instance from main.c

void* worker_thread(void* arg) {
    int my_id = *((int*)arg);

    while (1) {
        Task* task;
        if (!queue_read(&task_queue, (void**)&task)) break;

        int conn_type = client_get_conn_type(task->client_index);

        if (conn_type == CONN_VAULT) {

            // Get the correct fd for the client
            task->client_fd = client_get_fd(task->client_index);

            // Fetch the SSL session for this client
            SSL *ssl = client_get_ssl(task->client_index);

            // If this is a brand new connection, initialize SSL
            if (ssl == NULL) {
                ssl = SSL_new(tls_get_context());
                SSL_set_fd(ssl, task->client_fd);

                // CRITICAL: We must save it immediately because the handshake
                // is non-blocking and will take multiple epoll cycles to complete.
                client_set_ssl(task->client_index, ssl);
            }

            // Check if we are still authenticating (Handshake phase)
            if (client_get_auth(task->client_index) != AUTH_SUCCESS) {

                int ret = SSL_accept(ssl);

                if (ret <= 0) {
                    int err = SSL_get_error(ssl, ret);

                    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                        // Handshake is pending, re-arm the socket and yield the thread
                        struct epoll_event ev;
                        ev.events = EPOLLIN | EPOLLONESHOT;
                        // Use the u64 packing structure
                        ev.data.u64 = ((uint64_t)task->client_fd << 32) | (uint32_t)task->client_index;
                        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, task->client_fd, &ev);

                        free(task);
                        continue;
                    } else {
                        // Real error during handshake
                        printf("[Worker %d] ERROR: TLS Handshake failed on fd %d (Err: %d).\n", my_id, task->client_fd, err);
                        client_remove(task->client_index);
                        free(task);
                        continue;
                    }
                } else {
                    // Handshake success -> verify identity
                    char client_cn[256] = {0};
                    if (auth_verify_mtls(task->client_fd, ssl, client_cn, sizeof(client_cn))) {

                        client_set_auth(task->client_index, AUTH_SUCCESS);
                        client_set_state(task->client_index, STATE_IDLE);
                        client_set_hostname(task->client_index, client_cn);

                        // Re-arm to wait for the first real command
                        struct epoll_event ev;
                        ev.events = EPOLLIN | EPOLLONESHOT;
                        ev.data.u64 = ((uint64_t)task->client_fd << 32) | (uint32_t)task->client_index;
                        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, task->client_fd, &ev);
                    } else {
                        printf("[Worker %d] ERROR: mTLS Identity Verification failed for %s.\n", my_id, client_cn);
                        client_remove(task->client_index);
                    }
                    free(task);
                    continue;
                }
            }
            else {
                // Session is established and authenticated, now read data
                char temp_buf[1024];

                // Because the socket is non-blocking, this returns instantly
                int bytes_read = SSL_read(ssl, temp_buf, sizeof(temp_buf) - 1);

                if (bytes_read <= 0) {
                    int err = SSL_get_error(ssl, bytes_read);

                    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                        // SLOWLORIS MITIGATION: Re-arm the socket using the u64 struct
                        struct epoll_event ev;
                        ev.events = EPOLLIN | EPOLLONESHOT;
                        ev.data.u64 = ((uint64_t)task->client_fd << 32) | (uint32_t)task->client_index;
                        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, task->client_fd, &ev);

                        free(task);
                        continue; // Skip the rest of the loop, go to the next task

                    } else {
                        // A real error or graceful disconnect occurred
                        printf("[Worker %d] Client disconnected or SSL Error: %d\n", my_id, err);
                        client_remove(task->client_index);
                        free(task);
                        continue;
                    }
                }

                temp_buf[bytes_read] = '\0';
                client_update_activity(task->client_index);

                // Append decrypted data to our command buffer
                client_buffer_append(task->client_index, temp_buf, bytes_read);

                char complete_message[1024];
                int should_disconnect = 0;

                while (client_buffer_extract_line(task->client_index, complete_message, sizeof(complete_message))) {
                    // Strip carriage returns (\r)
                    complete_message[strcspn(complete_message, "\r")] = 0;

                    // Ignore empty lines
                    if (strlen(complete_message) == 0) continue;

                    char command[32] = {0};
                    char topic[64] = {0};
                    char payload[800] = {0};

                    int parsed_items = sscanf(complete_message, "%31s %63s %799[^\n]", command, topic, payload);
                    char response[512];


                    // SET command to set keys in database
                    if (parsed_items == 3 && strcmp(command, "SET") == 0) {

                        char sender_name[128];
                        client_get_hostname(task->client_index, sender_name, sizeof(sender_name));

                        if (!rbac_can_set(sender_name, topic)) {
                            SSL_write(ssl, "ERROR: Access denied.\n", 22);
                            printf("[Worker %d] RBAC Blocked %s from setting state '%s'\n", my_id, sender_name, topic);
                            continue;
                        }

                        db_set_device_state(sender_name, topic, payload);

                        char ack[128];
                        snprintf(ack, sizeof(ack), "SUCCESS: State '%s' updated.\n", topic);
                        SSL_write(ssl, ack, strlen(ack));

                    // GET command to query a specific key in the database
                    } else if (parsed_items == 2 && strcmp(command, "GET") == 0) {

                        char sender_name[128];
                        client_get_hostname(task->client_index, sender_name, sizeof(sender_name));

                        char value[256] = {0};

                        if (db_get_device_state(sender_name, topic, value, sizeof(value))) {
                            snprintf(response, sizeof(response), "VALUE: %s=%s\n", topic, value);
                        } else {
                            snprintf(response, sizeof(response), "ERROR: Key '%s' not found.\n", topic);
                        }

                        db_log_message(sender_name, topic, payload);
                        SSL_write(ssl, response, strlen(response));


                    // Heartbeat messages
                    } else if (parsed_items >= 1 && strcmp(command, "PING") == 0) {
                        SSL_write(ssl, "PONG\n", 5);
                        continue;

                    } else if (parsed_items >= 1 && strcmp(command, "PONG") == 0) {
                        continue;

                    // SUBSCRIBE to a specific channel
                    } else if (parsed_items >= 2 && strcmp(command, "SUBSCRIBE") == 0) {
                        char sender_name[128];
                        client_get_hostname(task->client_index, sender_name, sizeof(sender_name));

                        if (!rbac_can_subscribe(sender_name, topic)) {
                            SSL_write(ssl, "ERROR: Access denied.\n", 22);
                            printf("[Worker %d] RBAC Blocked %s from subscribing to %s\n", my_id, sender_name, topic);
                            continue;
                        }

                        db_log_message(sender_name, topic, payload);
                        pubsub_subscribe(task->client_index, topic);

                        snprintf(response, sizeof(response), "Subscribed to %s\n", topic);
                        SSL_write(ssl, response, strlen(response));

                    // UNSUBSCRIBE from a specific channel
                    } else if (parsed_items >= 2 && strcmp(command, "UNSUBSCRIBE") == 0) {
                        char sender_name[128];
                        client_get_hostname(task->client_index, sender_name, sizeof(sender_name));

                        if (!rbac_can_unsubscribe(sender_name, topic)) {
                            SSL_write(ssl, "ERROR: Access denied.\n", 22);
                            printf("[Worker %d] RBAC Blocked %s from unsubscribing to %s\n", my_id, sender_name, topic);
                            continue;
                        }

                        pubsub_unsubscribe(task->client_index, topic);
                        snprintf(response, sizeof(response), "Unsubscribed from %s\n", topic);
                        SSL_write(ssl, response, strlen(response));

                    // PUBLISH to a specific channel
                    } else if (parsed_items == 3 && strcmp(command, "PUBLISH") == 0) {
                        char sender_name[128];
                        client_get_hostname(task->client_index, sender_name, sizeof(sender_name));

                        if (!rbac_can_publish(sender_name, topic)) {
                            SSL_write(ssl, "ERROR: Access denied.\n", 22);
                            printf("[Worker %d] RBAC Blocked %s from publishing to %s\n", my_id, sender_name, topic);
                            continue;
                        }

                        db_log_message(sender_name, topic, payload);
                        pubsub_publish(topic, payload);

                        snprintf(response, sizeof(response), "Published to %s\n", topic);
                        SSL_write(ssl, response, strlen(response));

                    } else {
                        snprintf(response, sizeof(response), "ERROR: Invalid command.\n");
                        SSL_write(ssl, response, strlen(response));
                    }
                } // End of inner while loop

                // Safely handle the aftermath
                if (should_disconnect) {
                    pubsub_unsubscribe_all(task->client_index);
                    client_remove(task->client_index);
                } else {
                    // Re-arm using the u64 format
                    struct epoll_event ev;
                    ev.events = EPOLLIN | EPOLLONESHOT;
                    ev.data.u64 = ((uint64_t)task->client_fd << 32) | (uint32_t)task->client_index;
                    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, task->client_fd, &ev);
                }
            }

        } else if (conn_type == CONN_LOBBY) {

            char temp_buf[4096];
            int bytes_read = read(task->client_fd, temp_buf, sizeof(temp_buf) - 1);

            if (bytes_read > 0) {
               temp_buf[bytes_read] = '\0';
                process_enrollment(task->client_fd, temp_buf);
            }

            client_remove(task->client_index);
        }

        free(task); // Always free the memory
    }

    return NULL;
}
