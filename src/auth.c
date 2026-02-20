#include "auth.h"
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

int auth_verify_identity(int client_fd, const char* claimed_hostname) {
    // 1. Get the actual IP address of the connected socket
    struct sockaddr_in peer_addr;
    socklen_t peer_len = sizeof(peer_addr);
    char peer_ip[INET_ADDRSTRLEN] = {0};

    if (getpeername(client_fd, (struct sockaddr*)&peer_addr, &peer_len) != 0) {
        printf("Auth Error: Could not get peer name for socket %d\n", client_fd);
        return 0; // Fail
    }

    inet_ntop(AF_INET, &peer_addr.sin_addr, peer_ip, sizeof(peer_ip));
    printf("Auth: Socket IP is %s. Verifying against claim '%s'...\n", peer_ip, claimed_hostname);

    // 2. Perform DNS Lookup on the claimed hostname
    struct addrinfo hints, *res, *p;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; // IPv4 only for this example
    hints.ai_socktype = SOCK_STREAM;

    int ip_match = 0;

    if (getaddrinfo(claimed_hostname, NULL, &hints, &res) == 0) {
        char resolved_ip[INET_ADDRSTRLEN];

        // Loop through all IP addresses the DNS server returns
        for (p = res; p != NULL; p = p->ai_next) {
            struct sockaddr_in* ipv4 = (struct sockaddr_in*)p->ai_addr;
            inet_ntop(AF_INET, &ipv4->sin_addr, resolved_ip, sizeof(resolved_ip));

            // 3. Compare the DNS IP to the Socket IP
            if (strcmp(peer_ip, resolved_ip) == 0) {
                ip_match = 1;
                break;
            }
        }
        freeaddrinfo(res); // Clean up DNS memory
    } else {
        printf("Auth Error: DNS resolution failed for %s\n", claimed_hostname);
    }

    return ip_match;
}
