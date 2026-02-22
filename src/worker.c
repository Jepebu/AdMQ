#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#include "db.h"
#include "enroll.h"
#include "tls.h"
#include "auth.h"
#include "worker.h"
#include "client_manager.h"
#include "pubsub.h"

void* worker_thread(void* arg) {
    int my_id = *((int*)arg);

    while (1) {
        Task* task;
        if (!queue_read(&task_queue, (void**)&task)) break;

        int conn_type = client_get_conn_type(task->client_index);


        if (conn_type == CONN_VAULT) {

            // Fetch the SSL session for this client
            SSL *ssl = client_get_ssl(task->client_index);


            if (ssl == NULL) {
                //printf("[Worker %d] Initiating TLS Handshake for client %d...\n", my_id, task->client_index);

                // Create a new SSL session from our global factory
                ssl = SSL_new(tls_get_context());

                // Bind the raw network socket to the SSL session
                SSL_set_fd(ssl, task->client_fd);

                // Perform the Handshake (this blocks until the client sends their certificate)
                if (SSL_accept(ssl) <= 0) {
                    ERR_print_errors_fp(stderr);
                    printf("[Worker %d] ERROR: TLS Handshake failed.\n", my_id);

                    SSL_free(ssl); // Free the failed session
                    client_remove(task->client_index); // Disconnect them
                } else {

                    // AUTO-REGISTRATION
                    char client_cn[256] = {0};

                    if (auth_verify_mtls(task->client_fd, ssl, client_cn, sizeof(client_cn))) {
                        // printf("[Worker %d] mTLS SUCCESS! Secure tunnel established for %s.\n", my_id, client_cn);

                        // Save the session
                        client_set_ssl(task->client_index, ssl);

                        // GRANT FULL ACCESS IF THEY HAVE A CERT
                        client_set_auth(task->client_index, AUTH_SUCCESS);
                        client_set_state(task->client_index, STATE_IDLE);
                        client_set_hostname(task->client_index, client_cn);

                        // Send a welcome message
                        //char welcome[512];
                        //snprintf(welcome, sizeof(welcome), "SUCCESS: Auto-registered as %s via mTLS.\n", client_cn);
                        //SSL_write(ssl, welcome, strlen(welcome));

                    } else {
                        printf("[Worker %d] ERROR: mTLS Identity Verification failed for %s.\n", my_id, client_cn);
                        SSL_shutdown(ssl);
                        SSL_free(ssl);
                        client_remove(task->client_index);
                    }
                }

            } else {
                char temp_buf[1024];

                int bytes_read = SSL_read(ssl, temp_buf, sizeof(temp_buf));

                if (bytes_read <= 0) {
                    //printf("[Worker %d] Client %d disconnected (or TLS error).\n", my_id, task->client_index);
                    pubsub_unsubscribe_all(task->client_index);
                    client_remove(task->client_index);
                } else {
                    client_update_activity(task->client_index);


                    // Append decrypted data to our command buffer
                    client_buffer_append(task->client_index, temp_buf, bytes_read);

                    char complete_message[1024];
                    int should_disconnect = 0;

                    while (client_buffer_extract_line(task->client_index, complete_message, sizeof(complete_message))) {
                        // Strip carriage returns (\r) if the client is using Windows Telnet/netcat
                        complete_message[strcspn(complete_message, "\r")] = 0;

                        // Ignore empty lines
                        if (strlen(complete_message) == 0) continue;

                        char command[32] = {0};
                        char topic[64] = {0};
                        char payload[800] = {0};

                        int parsed_items = sscanf(complete_message, "%31s %63s %799[^\n]", command, topic, payload);
                        char response[256];

                        int is_authenticated = (client_get_auth(task->client_index) == AUTH_SUCCESS);

                        if (!is_authenticated) {

                            // Because of mTLS, a client should TECHNICALLY never reach this line.
                            // I'm not taking that chance though
                            snprintf(response, sizeof(response), "ERROR: Unauthorized.\n");
                            SSL_write(ssl, response, strlen(response));
                            should_disconnect = 1;
                            break;

                        } else if (parsed_items == 3 && strcmp(command, "SET") == 0) {

                             // 1. Get the securely verified identity of the sender
                             char sender_name[128];
                             client_get_hostname(task->client_index, sender_name, sizeof(sender_name));

                             // 2. Update the state in the database
                             // (Here, 'topic' acts as the Key, and 'payload' acts as the Value)
                             db_set_device_state(sender_name, topic, payload);

                             // 3. Acknowledge the update
                             char ack[128];
                             snprintf(ack, sizeof(ack), "SUCCESS: State '%s' updated.\n", topic);
                             SSL_write(ssl, ack, strlen(ack));

                             //printf("[Worker %d] State updated for %s: %s = %s\n", my_id, sender_name, topic, payload);

                       } else if (parsed_items == 2 && strcmp(command, "GET") == 0) {

                             // 1. Get the securely verified identity of the sender
                             char sender_name[128];
                             client_get_hostname(task->client_index, sender_name, sizeof(sender_name));

                             char value[256] = {0};
                             char response[512];

                             // 2. Look up the key specifically for this sender
                             if (db_get_device_state(sender_name, topic, value, sizeof(value))) {
                                 snprintf(response, sizeof(response), "VALUE: %s=%s\n", topic, value);
                             } else {
                                 snprintf(response, sizeof(response), "ERROR: Key '%s' not found.\n", topic);
                             }

                             // 3. Send the result back to the agent script
                             SSL_write(ssl, response, strlen(response));
                             // printf("[Worker %d] State queried by %s: %s\n", my_id, sender_name, topic);


                        } else if (parsed_items >= 1 && strcmp(command, "PING") == 0) {
                            SSL_write(ssl, "PONG\n", 5);
                            continue; // Skip the rest of the loop

                        } else if (parsed_items >= 1 && strcmp(command, "PONG") == 0) {
                            continue; // Just consume the pong, timestamp is already updated

                        } else if (parsed_items >= 2 && strcmp(command, "SUBSCRIBE") == 0) {
                            pubsub_subscribe(task->client_index, topic);
                            // snprintf(response, sizeof(response), "Subscribed to %s\n", topic);
                            SSL_write(ssl, response, strlen(response));

                        } else if (parsed_items >= 2 && strcmp(command, "UNSUBSCRIBE") == 0) {
                            pubsub_unsubscribe(task->client_index, topic);
                            // snprintf(response, sizeof(response), "Unsubscribed from %s\n", topic);
                            SSL_write(ssl, response, strlen(response));

                        } else if (parsed_items == 3 && strcmp(command, "PUBLISH") == 0) {
                            pubsub_publish(topic, payload);
                            // snprintf(response, sizeof(response), "Published to %s\n", topic);
                            SSL_write(ssl, response, strlen(response));

                            // Database logging
                            char sender_name[128];
                            client_get_hostname(task->client_index, sender_name, sizeof(sender_name));
                            db_log_message(sender_name, topic, payload);

                        } else {
                            snprintf(response, sizeof(response), "ERROR: Invalid command.\n");
                            SSL_write(ssl, response, strlen(response));
                        }
                    } // End of inner while loop


                    // Safely handle the aftermath
                    if (should_disconnect) {
                        // Clean up their routing and close their socket
                        pubsub_unsubscribe_all(task->client_index);
                        client_remove(task->client_index);
                    } else {
                       // Only return valid clients to the select() radar
                        client_set_state(task->client_index, STATE_IDLE);
                    }
               }
           }

        } else if (conn_type == CONN_LOBBY) {

            // We use a 4096-byte buffer here to ensure we catch the entire CSR
            // text in a single read (CSRs are usually around 1KB).
            char temp_buf[4096];
            int bytes_read = read(task->client_fd, temp_buf, sizeof(temp_buf) - 1);

            if (bytes_read <= 0) {
                // printf("[Worker %d] Lobby client disconnected.\n", my_id);
            } else {
                temp_buf[bytes_read] = '\0';

                // Pass the raw text off to our new enrollment module
                process_enrollment(task->client_fd, temp_buf);
            }

            // The enrollment port is strictly one-and-done.
            // Whether it succeeded or failed, always boot them out immediately.
            client_remove(task->client_index);
        }
        free(task);
    }
    return NULL;
}
