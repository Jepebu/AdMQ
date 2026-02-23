#include "heartbeat.h"
#include "client_manager.h"
#include "pubsub.h"
#include <unistd.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>

void* heartbeat_thread_loop(void* arg) {
    while (1) {
        sleep(10); // Run the sweep every 10 seconds
        time_t now = time(NULL);

        for (int i = 0; i < MAX_CLIENTS; i++) {
            // Thread Safety: Only disconnect clients sitting idle in the epoll() loop.
            // If they are actively processing a command, we skip them this round.
            if (client_get_state(i) == STATE_IDLE) {
                time_t last_active = client_get_activity(i);

                // If they haven't spoken in 60 seconds, cut them loose.
                if (now - last_active > 60) {
                    printf("\n[Heartbeat] Disconnecting ID %d. Disconnecting...\nadmq> ", i);
                    fflush(stdout); // Redraw the CLI prompt properly

                    pubsub_unsubscribe_all(i);
                    client_remove(i);
                }
            }
        }
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
