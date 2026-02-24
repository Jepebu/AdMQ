#ifndef PUBSUB_H
#define PUBSUB_H

#define MAX_TOPICS 50
#define MAX_SUBSCRIBERS_PER_TOPIC 100

void pubsub_init();
void pubsub_subscribe(int client_fd, const char* topic_name);
void pubsub_unsubscribe(int client_fd, const char* topic_name);
void pubsub_unsubscribe_all(int client_fd);
void pubsub_publish(const char* topic_name, const char* message);
void pubsub_print_status();

#endif
