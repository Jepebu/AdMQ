#include "pubsub.h"
#include "client_manager.h" // We need this to get the sockets to write to
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

typedef struct {
    char name[64];
    int subscribers[MAX_SUBSCRIBERS_PER_TOPIC];
    int sub_count;
} Topic;

Topic topics[MAX_TOPICS];
int topic_count = 0;
pthread_mutex_t pubsub_lock;

void pubsub_init() {
    pthread_mutex_init(&pubsub_lock, NULL);
    topic_count = 0;
}

void pubsub_subscribe(int client_index, const char* topic_name) {
    pthread_mutex_lock(&pubsub_lock);

    Topic* target_topic = NULL;

    // 1. Check if the topic already exists
    for (int i = 0; i < topic_count; i++) {
        if (strcmp(topics[i].name, topic_name) == 0) {
            target_topic = &topics[i];
            break;
        }
    }

    // 2. If it doesn't exist, create it (if we have space)
    if (target_topic == NULL && topic_count < MAX_TOPICS) {
        target_topic = &topics[topic_count];
        strncpy(target_topic->name, topic_name, 63);
        target_topic->name[63] = '\0';
        target_topic->sub_count = 0;
        topic_count++;
    }

    // 3. Add the client to the topic (avoiding duplicates)
    if (target_topic != NULL && target_topic->sub_count < MAX_SUBSCRIBERS_PER_TOPIC) {
        int already_subscribed = 0;
        for (int i = 0; i < target_topic->sub_count; i++) {
            if (target_topic->subscribers[i] == client_index) {
                already_subscribed = 1;
                break;
            }
        }

        if (!already_subscribed) {
            target_topic->subscribers[target_topic->sub_count] = client_index;
            target_topic->sub_count++;
            printf("Client %d subscribed to '%s'\n", client_index, topic_name);
        }
    }

    pthread_mutex_unlock(&pubsub_lock);
}


void pubsub_unsubscribe(int client_index, const char* topic_name) {
    pthread_mutex_lock(&pubsub_lock);

    // 1. Find the requested topic
    for (int i = 0; i < topic_count; i++) {
        if (strcmp(topics[i].name, topic_name) == 0) {

            // 2. Found the topic! Now find the client in the subscriber list
            for (int j = 0; j < topics[i].sub_count; j++) {
                if (topics[i].subscribers[j] == client_index) {

                    // 3. Found the client! Shift the rest of the array down to fill the gap
                    for (int k = j; k < topics[i].sub_count - 1; k++) {
                        topics[i].subscribers[k] = topics[i].subscribers[k + 1];
                    }

                    topics[i].sub_count--; // Shrink the active subscriber count
                    printf("Client %d unsubscribed from '%s'\n", client_index, topic_name);
                    break; // We removed the client, no need to keep checking this topic's list
                }
            }
            break; // We found the topic and handled it, no need to check other topics
        }
    }

    pthread_mutex_unlock(&pubsub_lock);
}


void pubsub_unsubscribe_all(int client_index) {
    pthread_mutex_lock(&pubsub_lock);

    // Loop through all topics and remove this client if they are in the list
    for (int i = 0; i < topic_count; i++) {
        for (int j = 0; j < topics[i].sub_count; j++) {
            if (topics[i].subscribers[j] == client_index) {
                // Shift the rest of the array down to fill the gap
                for (int k = j; k < topics[i].sub_count - 1; k++) {
                    topics[i].subscribers[k] = topics[i].subscribers[k + 1];
                }
                topics[i].sub_count--;
                break; // Found and removed from this topic, move to the next topic
            }
        }
    }

    pthread_mutex_unlock(&pubsub_lock);
}

void pubsub_publish(const char* topic_name, const char* message) {
    pthread_mutex_lock(&pubsub_lock);

    char formatted_msg[1024];
    snprintf(formatted_msg, sizeof(formatted_msg), "[%s] %s\n", topic_name, message);
    int msg_len = strlen(formatted_msg);

    for (int i = 0; i < topic_count; i++) {
        if (strcmp(topics[i].name, topic_name) == 0) {
            // Found the topic! Send the message to all subscribers.
            for (int j = 0; j < topics[i].sub_count; j++) {
                int client_idx = topics[i].subscribers[j];
                int fd = client_get_socket(client_idx);

                if (fd > 0) {
                    write(fd, formatted_msg, msg_len);
                }
            }
            break;
        }
    }

    pthread_mutex_unlock(&pubsub_lock);
}
