#ifndef PUBSUB_H
#define PUBSUB_H

#define MAX_TOPICS 50
#define MAX_SUBSCRIBERS_PER_TOPIC 100

void pubsub_init();

// Adds a client index to a topic's subscriber list
void pubsub_subscribe(int client_index, const char* topic_name);

// Removes a client from a specific topic
void pubsub_unsubscribe(int client_index, const char* topic_name);

// Removes a client from all topics (mainly for disconnects)
void pubsub_unsubscribe_all(int client_index);

// Sends a message to all clients subscribed to the topic
void pubsub_publish(const char* topic_name, const char* message);

#endif
