#include "heartbeat.h"
#include "client_manager.h"
#include <unistd.h>
#include <stdio.h>
#include <pthread.h>

void* heartbeat_thread_loop(void* arg) {
    while (1) {
        sleep(10); // Run the sweep every 10 seconds

        // Pass the command gracefully down into the manager so it can safely readlock the maps
        client_manager_sweep_inactive(60);
    }
    return NULL;
}

void heartbeat_init() {
    pthread_t tid;
    if (pthread_create(&tid, NULL, heartbeat_thread_loop, NULL) != 0) {
        perror("Failed to start heartbeat thread");
    }
    pthread_detach(tid);
}
