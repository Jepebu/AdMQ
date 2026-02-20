#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/time.h>
#include <signal.h>

#include "ts_queue.h"
#include "client_manager.h"
#include "worker.h"
#include "pubsub.h"

#define THREAD_POOL_SIZE 10

// Define the global queue here
ts_queue_t task_queue;

int main(int argc, char* argv[]) {
    const int portno = 35565;

    queue_init(&task_queue);
    client_manager_init();
    pubsub_init();

    pthread_t thread_pool[THREAD_POOL_SIZE];
    int thread_ids[THREAD_POOL_SIZE];


    signal(SIGPIPE, SIG_IGN); // Ignore broken pipe crashes

    printf("Starting Thread Pool...\n");
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        thread_ids[i] = i;
        if (pthread_create(&thread_pool[i], NULL, worker_thread, &thread_ids[i]) != 0) {
            perror("Failed to create worker thread");
            return 1;
        }
    }

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(portno);

    if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
        perror("ERROR on binding"); return 1;
    }

    listen(sockfd, 5);
    printf("Listening on port %d...\n", portno);

    while (true) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        int max_fd = sockfd;

        // Add IDLE clients to the radar using our clean manager methods
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (client_get_state(i) == STATE_IDLE) {
                int sd = client_get_socket(i);
                FD_SET(sd, &readfds);
                if (sd > max_fd) max_fd = sd;
            }
        }

        struct timeval timeout = {0, 10000}; // 10ms timeout
        int activity = select(max_fd + 1, &readfds, NULL, NULL, &timeout);

        if (activity < 0) { perror("select error"); break; }
        if (activity == 0) continue;

        // Handle new connections
        if (FD_ISSET(sockfd, &readfds)) {
            int new_fd = accept(sockfd, NULL, NULL);
            if (new_fd >= 0) {
                if (client_add(new_fd) == -1) {
                    printf("Server full. Rejecting connection.\n");
                    close(new_fd);
                } else {
                    printf("New client connected.\n");
                }
            }
        }

        // Handle incoming data
        for (int i = 0; i < MAX_CLIENTS; i++) {
            int sd = client_get_socket(i);
            if (client_get_state(i) == STATE_IDLE && sd > 0 && FD_ISSET(sd, &readfds)) {

                client_set_state(i, STATE_PROCESSING);

                Task* new_task = malloc(sizeof(Task));
                new_task->client_fd = sd;
                new_task->client_index = i;
                queue_write(&task_queue, new_task);
            }
        }
    }

    queue_shutdown(&task_queue);
    for (int i = 0; i < THREAD_POOL_SIZE; i++) pthread_join(thread_pool[i], NULL);
    queue_destroy(&task_queue);

    return 0;
}
