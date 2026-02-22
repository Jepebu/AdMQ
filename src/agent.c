#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <pthread.h>

#include "agent_config.h"

volatile sig_atomic_t keep_running = 1;

void handle_sigint(int sig) {
    printf("\n[Agent] Caught SIGINT - disconnecting from broker...\n");
    keep_running = 0;
}


// Struct to hold the parsed execution instructions
typedef struct {
    char cmd[128];
    char target[256];
    char arguments[256];
} ActionConfig;


// Helper function to strip leading and trailing whitespace from strings
char* trim_whitespace(char* str) {
    char* end;
    // Trim leading space
    while(isspace((unsigned char)*str)) str++;
    if(*str == 0) return str; // All spaces

    // Trim trailing space
    end = str + strlen(str) - 1;
    while(end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';

    return str;
}


// Action INI file parser
int parse_ini_action(const char* action_dir, const char* command, const char* arg, ActionConfig* config) {
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s.ini", action_dir, command);

    FILE* file = fopen(filepath, "r");
    if (!file) {
        printf("ERROR: Could not find action file: %s\n", filepath);
        return 0; // Failure
    }

    char line[512];
    int in_target_section = 0;
    int found_config = 0;

    char target_section[256];
    char safe_arg[128];
    strncpy(safe_arg, arg, sizeof(safe_arg) - 1);
    // Convert spaces to underscores to match INI conventions
    for (int i = 0; safe_arg[i]; i++) { if (safe_arg[i] == ' ') safe_arg[i] = '_'; }
    snprintf(target_section, sizeof(target_section), "[%s]", safe_arg);

    while (fgets(line, sizeof(line), file)) {
        char* trimmed = trim_whitespace(line);

        // Skip comments and completely empty lines
        if (trimmed[0] == '\0' || trimmed[0] == ';' || trimmed[0] == '#') {
            continue;
        }

        // Check for section headers
        if (trimmed[0] == '[') {
            if (strcmp(trimmed, target_section) == 0) {
                in_target_section = 1; // We found our block
            } else {
                if (in_target_section) {
                    break;
                }
                in_target_section = 0; // Wrong block, keep searching
            }
            continue;
        }

        // Parse key=value pairs if we are inside the correct section
        if (in_target_section) {
            char* equals_sign = strchr(trimmed, '=');
            if (equals_sign) {
                *equals_sign = '\0'; // Split the string into two halves

                char* key = trim_whitespace(trimmed);
                char* val = trim_whitespace(equals_sign + 1);

                // Strip surrounding quotes from the value
                if (val[0] == '"' && val[strlen(val)-1] == '"') {
                    val[strlen(val)-1] = '\0';
                    val++;
                }

                // Copy the values into our struct securely
                if (strcmp(key, "cmd") == 0) {
                    strncpy(config->cmd, val, sizeof(config->cmd) - 1);
                } else if (strcmp(key, "target") == 0) {
                    strncpy(config->target, val, sizeof(config->target) - 1);
                } else if (strcmp(key, "arguments") == 0) {
                    strncpy(config->arguments, val, sizeof(config->arguments) - 1);
                }

                found_config = 1;
            }
        }
    }

    fclose(file);
    return found_config;
}

void* agent_ping_thread(void* arg) {
    SSL* ssl = (SSL*)arg;
    while (1) {
        sleep(30); // Ping every 30 seconds
        char* ping_msg = "PING\n";
        SSL_write(ssl, ping_msg, strlen(ping_msg));
    }
    return NULL;
}


SSL_CTX* create_client_context(const char* cert_path, const char* key_path, const char* ca_path) {
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();

    const SSL_METHOD *method = TLS_client_method();
    SSL_CTX *ctx = SSL_CTX_new(method);
    if (!ctx) {
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }

    // Load the client's certificate and private key
    if (SSL_CTX_use_certificate_file(ctx, cert_path, SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }

    if (SSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM) <= 0 ) {
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }

    // Load the Root CA
    if (SSL_CTX_load_verify_locations(ctx, ca_path, NULL) <= 0) {
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }

    return ctx;
}


int main() {
    signal(SIGCHLD, SIG_IGN); // Prevent zombie processes
    signal(SIGINT, handle_sigint); // SIGINT handler

    // sigaction to get around SSL_read
    struct sigaction sa;
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // Explicitely turn off SA_RESTART
    sigaction(SIGINT, &sa, NULL);


    // Load Configuration
    AgentConfig config;
    agent_config_load("agent.ini", &config);

    // Initialize OpenSSL with Config Paths
    SSL_CTX *ctx = create_client_context(config.cert_path, config.key_path, config.ca_path);
    SSL *ssl;

    int sockfd;
    struct sockaddr_in serv_addr;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("ERROR opening socket"); return 1; }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(BROKER_PORT); // Ensure this is the right port
    inet_pton(AF_INET, BROKER_IP, &serv_addr.sin_addr);

    // Connect the raw TCP socket
    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("ERROR connecting to broker"); return 1;
    }

    // Upgrade to an Encrypted TLS Tunnel
    ssl = SSL_new(ctx);
    SSL_set_fd(ssl, sockfd);

    if (SSL_connect(ssl) <= 0) {
        ERR_print_errors_fp(stderr);
        printf("Agent TLS Handshake Failed.\n");
        exit(EXIT_FAILURE);
    }

    printf("Agent securely connected to broker via mTLS.\n");

    // Subscribe to group from the config file
    char sub_msg[128];
    snprintf(sub_msg, sizeof(sub_msg), "SUBSCRIBE %s\n", config.command_group);
    SSL_write(ssl, sub_msg, strlen(sub_msg));

    // Subscribe to the global broadcast channel
    char* sub_broadcast = "SUBSCRIBE BROADCAST\n";
    SSL_write(ssl, sub_broadcast, strlen(sub_broadcast));

    pthread_t ping_tid;
    pthread_create(&ping_tid, NULL, agent_ping_thread, (void*)ssl);
    pthread_detach(ping_tid);

    char buffer[1024];
    // Change the loop condition
    while (keep_running) {
        memset(buffer, 0, sizeof(buffer));

        // SSL_read will unblock and return <= 0 if interrupted by the signal
        int bytes_read = SSL_read(ssl, buffer, sizeof(buffer) - 1);

        if (bytes_read <= 0) {
            if (!keep_running) {
                // We caught a SIGINT and intended to break. Just exit the loop quietly.
                break;
            }
            // Otherwise, the broker actually died or the network dropped.
            printf("Broker disconnected. Agent shutting down.\n");
            break;
        }
        buffer[strcspn(buffer, "\r\n")] = 0;
        printf("\n[Agent] Secure message received: %s\n", buffer);

        char topic[64] = {0};
        char command[64] = {0};
        char argument[128] = {0};

        if (sscanf(buffer, "[%63[^]]] %63s %127[^\n]", topic, command, argument) >= 3) {

            ActionConfig act_config = {0};
            if (parse_ini_action(config.action_dir, command, argument, &act_config)) {

                char exec_cmd[1024];
                snprintf(exec_cmd, sizeof(exec_cmd), "%s %s %s", act_config.cmd, act_config.target, act_config.arguments);

                pid_t pid = fork();

                if (pid == 0) {
                    execl("/bin/sh", "sh", "-c", exec_cmd, (char *)NULL);
                    perror("execl failed");
                    exit(1);
                } else if (pid > 0) {
                    char report[512];
                    snprintf(report, sizeof(report),
                             "PUBLISH agent-status SUCCESS: Task '%s %s' started (PID %d).\n",
                             command, argument, pid);

                    // Send the status report
                    SSL_write(ssl, report, strlen(report));
                }
            } else {
                char report[512];
                snprintf(report, sizeof(report),
                         "PUBLISH agent-status ERROR: Unknown action '%s %s'\n",
                         command, argument);
                SSL_write(ssl, report, strlen(report));
            }
        }
    }

    // SHUTDOWN SEQUENCE
    printf("[Agent] Cleaning up TLS session...\n");
    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(sockfd);
    SSL_CTX_free(ctx);

    printf("[Agent] Shutdown complete.\n");
    return 0;
}


