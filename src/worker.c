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

extern int epoll_fd;

void* worker_thread(void* arg) {
    int my_id = *((int*)arg);

    while (1) {
        Task* task;
        if (!queue_read(&task_queue, (void**)&task)) break;

        if (task->conn_type == CONN_VAULT) {

            // Retrieve the Client pointer & automatically lock its individual mutex
            Client* c = client_get_and_lock_by_fd(task->client_fd);
            if (!c) {
                free(task);
                continue;
            }

            if (c->ssl == NULL) {
                c->ssl = SSL_new(tls_get_context());
                SSL_set_fd(c->ssl, c->fd);
            }

            if (c->auth_status != AUTH_SUCCESS) {

                int ret = SSL_accept(c->ssl);
                if (ret <= 0) {
                    int err = SSL_get_error(c->ssl, ret);
                    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                        // Handshake is pending, re-arm safely
                        struct epoll_event ev;
                        ev.events = EPOLLIN | EPOLLONESHOT;
                        ev.data.fd = c->fd;
                        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, c->fd, &ev);

                        client_unlock(c);
                        free(task);
                        continue;
                    } else {
                        printf("[Worker %d] ERROR: TLS Handshake failed on fd %d (Err: %d).\n", my_id, c->fd, err);
                        client_unlock(c);
                        client_remove(task->client_fd);
                        free(task);
                        continue;
                    }
                } else {
                    char client_cn[256] = {0};
                    if (auth_verify_mtls(c->fd, c->ssl, client_cn, sizeof(client_cn))) {
                        c->auth_status = AUTH_SUCCESS;
                        c->state = STATE_IDLE;
                        client_unlock(c);

                        // Handle external mapping outside the client lock to prevent any lock contention
                        client_set_hostname(task->client_fd, client_cn);

                        struct epoll_event ev;
                        ev.events = EPOLLIN | EPOLLONESHOT;
                        ev.data.fd = task->client_fd;
                        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, task->client_fd, &ev);
                    } else {
                        printf("[Worker %d] ERROR: mTLS Identity Verification failed for %s.\n", my_id, client_cn);
                        client_unlock(c);
                        client_remove(task->client_fd);
                    }
                    free(task);
                    continue;
                }
            } else {
                char temp_buf[1024];
                int bytes_read = SSL_read(c->ssl, temp_buf, sizeof(temp_buf) - 1);

                if (bytes_read <= 0) {
                    int err = SSL_get_error(c->ssl, bytes_read);
                    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                        struct epoll_event ev;
                        ev.events = EPOLLIN | EPOLLONESHOT;
                        ev.data.fd = c->fd;
                        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, c->fd, &ev);

                        client_unlock(c);
                        free(task);
                        continue;

                    } else {
                        printf("[Worker %d] Client disconnected or SSL Error: %d\n", my_id, err);
                        client_unlock(c);
                        pubsub_unsubscribe_all(task->client_fd);
                        client_remove(task->client_fd);
                        free(task);
                        continue;
                    }
                }

                temp_buf[bytes_read] = '\0';
                c->last_activity = time(NULL);
                client_buffer_append(c, temp_buf, bytes_read);

                char complete_message[1024];
                int should_disconnect = 0;

                while (client_buffer_extract_line(c, complete_message, sizeof(complete_message))) {
                    complete_message[strcspn(complete_message, "\r")] = 0;
                    if (strlen(complete_message) == 0) continue;

                    char command[32] = {0};
                    char topic[64] = {0};
                    char payload[800] = {0};
                    int parsed_items = sscanf(complete_message, "%31s %63s %799[^\n]", command, topic, payload);
                    char response[512];

                    if (parsed_items == 3 && strcmp(command, "SET") == 0) {
                        if (!rbac_can_set(c->hostname, topic)) {
                            SSL_write(c->ssl, "ERROR: Access denied.\n", 22);
                            continue;
                        }
                        db_set_device_state(c->hostname, topic, payload);
                        snprintf(response, sizeof(response), "SUCCESS: State '%s' updated.\n", topic);
                        SSL_write(c->ssl, response, strlen(response));

                    } else if (parsed_items == 2 && strcmp(command, "GET") == 0) {
                        char value[256] = {0};
                        if (db_get_device_state(c->hostname, topic, value, sizeof(value))) {
                            snprintf(response, sizeof(response), "VALUE: %s=%s\n", topic, value);
                        } else {
                            snprintf(response, sizeof(response), "ERROR: Key '%s' not found.\n", topic);
                        }
                        db_log_message(c->hostname, topic, payload);
                        SSL_write(c->ssl, response, strlen(response));

                    } else if (parsed_items >= 1 && strcmp(command, "PING") == 0) {
                        SSL_write(c->ssl, "PONG\n", 5);

                    } else if (parsed_items >= 1 && strcmp(command, "PONG") == 0) {
                        continue;

                    } else if (parsed_items >= 2 && strcmp(command, "SUBSCRIBE") == 0) {
                        if (!rbac_can_subscribe(c->hostname, topic)) {
                            SSL_write(c->ssl, "ERROR: Access denied.\n", 22);
                            continue;
                        }
                        db_log_message(c->hostname, topic, payload);
                        pubsub_subscribe(c->fd, topic);

                        snprintf(response, sizeof(response), "Subscribed to %s\n", topic);
                        SSL_write(c->ssl, response, strlen(response));

                    } else if (parsed_items >= 2 && strcmp(command, "UNSUBSCRIBE") == 0) {
                        if (!rbac_can_unsubscribe(c->hostname, topic)) {
                            SSL_write(c->ssl, "ERROR: Access denied.\n", 22);
                            continue;
                        }
                        pubsub_unsubscribe(c->fd, topic);
                        snprintf(response, sizeof(response), "Unsubscribed from %s\n", topic);
                        SSL_write(c->ssl, response, strlen(response));

                    } else if (parsed_items == 3 && strcmp(command, "PUBLISH") == 0) {
                        if (!rbac_can_publish(c->hostname, topic)) {
                            SSL_write(c->ssl, "ERROR: Access denied.\n", 22);
                            continue;
                        }
                        db_log_message(c->hostname, topic, payload);

                        // We must explicitly drop the client's mutex lock here to prevent thread deadlocks
                        // when pubsub searches over other active users' SSL pipes that may be writing.
                        client_unlock(c);
                        pubsub_publish(topic, payload);
                        c = client_get_and_lock_by_fd(task->client_fd);
                        if (!c) { should_disconnect = 1; break; }

                        snprintf(response, sizeof(response), "Published to %s\n", topic);
                        SSL_write(c->ssl, response, strlen(response));

                    } else {
                        snprintf(response, sizeof(response), "ERROR: Invalid command.\n");
                        SSL_write(c->ssl, response, strlen(response));
                    }
                }

                if (should_disconnect) {
                    client_unlock(c);
                    pubsub_unsubscribe_all(task->client_fd);
                    client_remove(task->client_fd);
                } else {
                    struct epoll_event ev;
                    ev.events = EPOLLIN | EPOLLONESHOT;
                    ev.data.fd = c->fd;
                    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, c->fd, &ev);
                    client_unlock(c);
                }
            }

        } else if (task->conn_type == CONN_LOBBY) {

            char temp_buf[4096];
            int bytes_read = read(task->client_fd, temp_buf, sizeof(temp_buf) - 1);

            if (bytes_read > 0) {
               temp_buf[bytes_read] = '\0';
               process_enrollment(task->client_fd, temp_buf);
            }

            close(task->client_fd);
        }

        free(task);
    }
    return NULL;
}
