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
#include <errno.h>

#include "config.h"
#include "heartbeat.h"
#include "db.h"
#include "cli.h"
#include "pubsub.h"
#include "tls.h"
#include "ts_queue.h"
#include "client_manager.h"
#include "worker.h"
#include "pubsub.h"

#define THREAD_POOL_SIZE 10

// Define the global queue here
ts_queue_t task_queue;

volatile sig_atomic_t keep_running = 1;

void handle_sigint(int sig) {
    printf("\n[Broker] Caught SIGINT - shutting down...\n");
    keep_running = 0;
}

// Helper function for creating listening sockets.
int create_listening_socket(int port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
        perror("ERROR on binding");
        exit(1);
    }
    listen(sockfd, 5);
    return sockfd;
}


int main(int argc, char* argv[]) {
    BrokerConfig config;
    config_load("broker.ini", &config);

    // Register our graceful shutdown handler
    signal(SIGINT, handle_sigint);
    signal(SIGPIPE, SIG_IGN);

    if (isatty(STDIN_FILENO)) {
        cli_init();
    } else {
        printf("[Broker] Starting in daemon mode.\n");
    }


    // --- 2. Initialize Subsystems Dynamically ---
    tls_init(config.cert_path, config.key_path, config.ca_path);
    db_init(config.db_path);


    queue_init(&task_queue);
    client_manager_init();
    pubsub_init();
    heartbeat_init();

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


    int vault_sockfd = create_listening_socket(config.vault_port);
    printf("Vault (mTLS) listening on port %d...\n", config.vault_port);

    int lobby_sockfd = create_listening_socket(config.lobby_port);
    printf("Lobby (Plaintext) listening on port %d...\n", config.lobby_port);

    int opt = 1;
    setsockopt(vault_sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)); // Reusable ports for testing
    setsockopt(lobby_sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)); // Reusable ports for testing


    while (keep_running) {
        fd_set readfds;
        FD_ZERO(&readfds);

        // Add BOTH server sockets to the radar
        FD_SET(vault_sockfd, &readfds);
        FD_SET(lobby_sockfd, &readfds);
        int max_fd = (vault_sockfd > lobby_sockfd) ? vault_sockfd : lobby_sockfd;

        // Add IDLE clients
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (client_get_state(i) == STATE_IDLE) {
                int sd = client_get_socket(i);
                FD_SET(sd, &readfds);
                if (sd > max_fd) max_fd = sd;
            }
        }

        struct timeval timeout = {0, 10000};
        int activity = select(max_fd + 1, &readfds, NULL, NULL, &timeout);

        // --- Handle Interrupted System Calls (EINTR) ---
        if (activity < 0) {
            if (errno == EINTR) {
                // select() was interrupted by our Ctrl+C signal.
                // The loop will automatically break on the next iteration.
                continue;
            }
            perror("select error");
            break;
        }

        if (activity == 0) continue;


        // Handle Vault Connections
        if (FD_ISSET(vault_sockfd, &readfds)) {
            int new_fd = accept(vault_sockfd, NULL, NULL);
            if (new_fd >= 0) client_add(new_fd, CONN_VAULT);
        }

        // Handle Lobby Connections
        if (FD_ISSET(lobby_sockfd, &readfds)) {
            int new_fd = accept(lobby_sockfd, NULL, NULL);
            if (new_fd >= 0) client_add(new_fd, CONN_LOBBY);
        }

        // Handle incoming data from clients (This block stays exactly the same!)
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

    // --- SHUTDOWN SEQUENCE ---
    printf("\n[Broker] Shutting down...\n");

    // Close network sockets
    close(vault_sockfd);
    close(lobby_sockfd);

    // Disconnect all clients properly
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_remove(i);
    }

    // Clean up subsystems
    db_close();
    tls_cleanup();
    cli_cleanup();

    printf("[Broker] Shutdown complete.\n");
    return 0;

}
