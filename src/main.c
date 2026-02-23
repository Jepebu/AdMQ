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
#include <sys/epoll.h>
#include <fcntl.h>
#include <errno.h>


#include "rbac.h"
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


#define THREAD_POOL_SIZE 10 // Number of threads to use for workers.
#define MAX_EVENTS 64 // Number of events that can be active at the same time.

int epoll_fd; // Global scope so worker threads can re-arm their sockets.

ts_queue_t task_queue; // Global scope for task queue.

volatile sig_atomic_t keep_running = 1;

void handle_sigint(int sig) {
    printf("\n[AdMQ Server] Caught SIGINT - shutting down...\n");
    keep_running = 0;
}

void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags != -1) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
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


    // Initialize subsystems dynamically
    tls_init(config.cert_path, config.key_path, config.ca_path);
    db_init(config.db_path);
    rbac_init("rbac.ini");


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


    // Initialize epoll
    epoll_fd = epoll_create1(0);
    struct epoll_event ev, events[MAX_EVENTS];

    // Set listening sockets to non-blocking
    set_nonblocking(vault_sockfd);
    set_nonblocking(lobby_sockfd);

    // Add listening sockets to the epoll watch list
    ev.events = EPOLLIN;
    ev.data.fd = vault_sockfd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, vault_sockfd, &ev);

    ev.events = EPOLLIN;
    ev.data.fd = lobby_sockfd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, lobby_sockfd, &ev);

    // epoll event loop
    while (keep_running) {
        // Wait for activity - timeout is 1000ms so the loop can check keep_running cleanly.
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, 1000);

        if (nfds < 0) {
            if (errno == EINTR) continue; // Caught SIGINT gracefully
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++) {

            // Activity on vault port
            if (events[i].data.fd == vault_sockfd) {
                while (1) {
                    struct sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    int client_fd = accept(vault_sockfd, (struct sockaddr*)&client_addr, &client_len);

                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        break;
                    }

                    set_nonblocking(client_fd);
                    int client_index = client_add(client_fd, CONN_VAULT); // Sets up SSL natively

                    struct epoll_event client_ev;
                    client_ev.events = EPOLLIN | EPOLLONESHOT;
                    client_ev.data.ptr = (void*)(intptr_t)client_index;
                    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &client_ev);
                }
            }
            // Activity on lobby port
            else if (events[i].data.fd == lobby_sockfd) {
                while (1) {
                    struct sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    int client_fd = accept(lobby_sockfd, (struct sockaddr*)&client_addr, &client_len);

                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        break;
                    }

                    set_nonblocking(client_fd);

                    // We don't add Lobby clients to the mTLS client_manager.
                    // Instead, we track them in epoll using a negative FD pointer.
                    struct epoll_event lobby_ev;
                    lobby_ev.events = EPOLLIN | EPOLLONESHOT;
                    lobby_ev.data.ptr = (void*)(intptr_t)(-client_fd);
                    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &lobby_ev);
                }
            }
            // Activity on an existing connection (Vault or Lobby)
            else {
                intptr_t ptr_val = (intptr_t)events[i].data.ptr;

                // Allocate the Task dynamically on the heap
                Task* task = malloc(sizeof(Task));

                if (ptr_val < 0) {
                    // Negative pointer means this is a pending Lobby request
                    task->client_index = -1;
                    task->client_fd = (int)(-ptr_val);
                } else {
                    // Positive pointer means this is an established Vault client
                    task->client_index = (int)ptr_val;
                }

                queue_write(&task_queue, task); // Safely pass the heap pointer
            }
        }
    }





    // SHUTDOWN SEQUENCE
    printf("\n[AdMQ Server] Shutting down...\n");

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

    printf("[AdMQ Server] Shutdown complete.\n");
    return 0;

}
