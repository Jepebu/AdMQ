#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "auth.h"
#include "worker.h"
#include "client_manager.h"
#include "pubsub.h"

void* worker_thread(void* arg) {
    int my_id = *((int*)arg);

    while (1) {
        Task* task;
        if (!queue_read(&task_queue, (void**)&task)) break;

        char temp_buf[1024];
        ssize_t bytes_read = read(task->client_fd, temp_buf, sizeof(temp_buf));

        if (bytes_read <= 0) {
            printf("[Worker %d] Client %d disconnected.\n", my_id, task->client_index);
            pubsub_unsubscribe_all(task->client_index);
            client_remove(task->client_index);
        } else {

            // 1. Append the raw chunks of data to the client's persistent buffer
            client_buffer_append(task->client_index, temp_buf, bytes_read);

            char complete_message[1024];
            int should_disconnect = 0;

            // 2. Loop to extract AS MANY complete messages as we can find
            while (client_buffer_extract_line(task->client_index, complete_message, sizeof(complete_message))) {

                // Strip carriage returns (\r) if the client is using Windows Telnet/netcat
                complete_message[strcspn(complete_message, "\r")] = 0;

                // Ignore empty lines
                if (strlen(complete_message) == 0) continue;

                printf("[Worker %d] Parsed Command: '%s'\n", my_id, complete_message);

                char command[32] = {0};
                char topic[64] = {0};
                char payload[800] = {0};

                int parsed_items = sscanf(complete_message, "%31s %63s %799[^\n]", command, topic, payload);
                char response[256];

                int is_authenticated = (client_get_auth(task->client_index) == AUTH_SUCCESS);

                if (parsed_items >= 2 && strcmp(command, "REGISTER") == 0) {
                    if (is_authenticated) {
                        snprintf(response, sizeof(response), "Already registered.\n");
                        write(task->client_fd, response, strlen(response));
                        continue;
                    }

                    char* claimed_hostname = topic;

                    // Call our new clean, abstracted auth function
                    if (auth_verify_identity(task->client_fd, claimed_hostname)) {

                        // UPGRADE THEIR STATUS TO SUCCESS!
                        client_set_auth(task->client_index, AUTH_SUCCESS);

                        snprintf(response, sizeof(response), "SUCCESS: Identity verified for %s. Welcome!\n", claimed_hostname);
                        write(task->client_fd, response, strlen(response));

                    } else {
                        snprintf(response, sizeof(response), "ERROR: Security violation. IP mismatch or DNS failure for %s.\n", claimed_hostname);
                        write(task->client_fd, response, strlen(response));

                        printf("[Worker %d] Disconnecting imposter.\n", my_id);
                        should_disconnect = 1;
                        break;
                    }

                } else if (!is_authenticated) {
                    // IF THEY ARE NOT AUTHENTICATED, REJECT EVERYTHING ELSE
                    snprintf(response, sizeof(response), "ERROR: Unauthorized. You must REGISTER first.\n");
                    write(task->client_fd, response, strlen(response));

                } else if (parsed_items >= 2 && strcmp(command, "SUBSCRIBE") == 0) {
                    pubsub_subscribe(task->client_index, topic);
                    snprintf(response, sizeof(response), "Subscribed to %s\n", topic);
                    write(task->client_fd, response, strlen(response));

                } else if (parsed_items >= 2 && strcmp(command, "UNSUBSCRIBE") == 0) {
                    pubsub_unsubscribe(task->client_index, topic);
                    snprintf(response, sizeof(response), "Unsubscribed from %s\n", topic);
                    write(task->client_fd, response, strlen(response));

                } else if (parsed_items == 3 && strcmp(command, "PUBLISH") == 0) {
                    pubsub_publish(topic, payload);
                    snprintf(response, sizeof(response), "Published to %s\n", topic);
                    write(task->client_fd, response, strlen(response));

                } else {
                    snprintf(response, sizeof(response), "ERROR: Invalid command.\n");
                    write(task->client_fd, response, strlen(response));
                }
            } // End of inner while loop


            // 3. Safely handle the aftermath
            if (should_disconnect) {
                // Clean up their routing and close their socket
                pubsub_unsubscribe_all(task->client_index);
                client_remove(task->client_index);
                // Notice we do NOT set them to IDLE
            } else {
                // Only return valid clients to the select() radar
                client_set_state(task->client_index, STATE_IDLE);
            }
        }
        free(task);
    }
    return NULL;
}
