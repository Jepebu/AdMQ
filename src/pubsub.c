#include "pubsub.h"
#include "client_manager.h"

#include <openssl/ssl.h>
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

void pubsub_subscribe(int client_fd, const char* topic_name) {
    pthread_mutex_lock(&pubsub_lock);
    Topic* target_topic = NULL;

    for (int i = 0; i < topic_count; i++) {
        if (strcmp(topics[i].name, topic_name) == 0) {
            target_topic = &topics[i];
            break;
        }
    }

    if (target_topic == NULL && topic_count < MAX_TOPICS) {
        target_topic = &topics[topic_count];
        strncpy(target_topic->name, topic_name, 63);
        target_topic->name[63] = '\0';
        target_topic->sub_count = 0;
        topic_count++;
    }

    if (target_topic != NULL && target_topic->sub_count < MAX_SUBSCRIBERS_PER_TOPIC) {
        int already_subscribed = 0;
        for (int i = 0; i < target_topic->sub_count; i++) {
            if (target_topic->subscribers[i] == client_fd) {
                already_subscribed = 1;
                break;
            }
        }
        if (!already_subscribed) {
            target_topic->subscribers[target_topic->sub_count] = client_fd;
            target_topic->sub_count++;
        }
    }
    pthread_mutex_unlock(&pubsub_lock);
}

void pubsub_unsubscribe(int client_fd, const char* topic_name) {
    pthread_mutex_lock(&pubsub_lock);

    for (int i = 0; i < topic_count; i++) {
        if (strcmp(topics[i].name, topic_name) == 0) {
            for (int j = 0; j < topics[i].sub_count; j++) {
                if (topics[i].subscribers[j] == client_fd) {
                    for (int k = j; k < topics[i].sub_count - 1; k++) {
                        topics[i].subscribers[k] = topics[i].subscribers[k + 1];
                    }
                    topics[i].sub_count--;
                    break;
                }
            }
            break;
        }
    }
    pthread_mutex_unlock(&pubsub_lock);
}

void pubsub_unsubscribe_all(int client_fd) {
    pthread_mutex_lock(&pubsub_lock);

    for (int i = 0; i < topic_count; i++) {
        for (int j = 0; j < topics[i].sub_count; j++) {
            if (topics[i].subscribers[j] == client_fd) {
                for (int k = j; k < topics[i].sub_count - 1; k++) {
                    topics[i].subscribers[k] = topics[i].subscribers[k + 1];
                }
                topics[i].sub_count--;
                break;
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
            for (int j = 0; j < topics[i].sub_count; j++) {
                int client_fd = topics[i].subscribers[j];

                // Safely lock the specific user struct inside the publication loop
                Client* c = client_get_and_lock_by_fd(client_fd);
                if (c != NULL) {
                    if (c->ssl != NULL) {
                        SSL_write(c->ssl, formatted_msg, msg_len);
                    } else if (c->fd > 0) {
                        write(c->fd, formatted_msg, msg_len);
                    }
                    client_unlock(c);
                }
            }
            break;
        }
    }
    pthread_mutex_unlock(&pubsub_lock);
}

void pubsub_print_status() {
    pthread_mutex_lock(&pubsub_lock);
    printf("\n=== ACTIVE TOPICS ===\n");
    int count = 0;
    for (int i = 0; i < topic_count; i++) {
        if (topics[i].sub_count > 0) {
            count++;
            printf("  [%s]: ", topics[i].name);
            for (int j = 0; j < topics[i].sub_count; j++) {
                // Determine Hostname cleanly if possible
                Client* c = client_get_and_lock_by_fd(topics[i].subscribers[j]);
                if (c) {
                    printf("%s ", (strlen(c->hostname) > 0) ? c->hostname : "Pending");
                    client_unlock(c);
                } else {
                    printf("FD:%d ", topics[i].subscribers[j]);
                }
            }
            printf("\n");
        }
    }
    if (count == 0) printf("  No active subscriptions.\n");
    printf("=====================\n\n");
    pthread_mutex_unlock(&pubsub_lock);
}
