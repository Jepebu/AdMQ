#include "enroll.h"
#include "auth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void process_enrollment(int client_fd, const char* request_buffer) {
    char command[32] = {0};
    char hostname[128] = {0};

    // Parse the first line to get the command and the claimed hostname
    char first_line[256] = {0};
    const char* newline_pos = strchr(request_buffer, '\n');

    if (newline_pos == NULL) {
        write(client_fd, "ERROR: Invalid request format.\n", 31);
        return;
    }

    int first_line_len = newline_pos - request_buffer;
    if (first_line_len >= sizeof(first_line)) first_line_len = sizeof(first_line) - 1;
    strncpy(first_line, request_buffer, first_line_len);

    if (sscanf(first_line, "%31s %127s", command, hostname) != 2 || strcmp(command, "ENROLL") != 0) {
        write(client_fd, "ERROR: Lobby only accepts ENROLL <hostname> commands.\n", 54);
        return;
    }

    // Validate Identity (Does the IP match the DNS for this hostname?)
    printf("[Enroll] Validating enrollment request for %s...\n", hostname);
    if (!auth_verify_identity(client_fd, hostname)) {
        write(client_fd, "ERROR: Security violation. IP does not match DNS.\n", 50);
        return;
    }

    // Extract the CSR block from the buffer
    const char* csr_start = strstr(request_buffer, "-----BEGIN CERTIFICATE REQUEST-----");
    if (csr_start == NULL) {
        write(client_fd, "ERROR: No valid CSR block found in request.\n", 44);
        return;
    }

    // Write the CSR to a temporary file (named using the socket FD to avoid collisions)
    char csr_filename[64], crt_filename[64];
    snprintf(csr_filename, sizeof(csr_filename), "/tmp/csr_%d.pem", client_fd);
    snprintf(crt_filename, sizeof(crt_filename), "/tmp/crt_%d.pem", client_fd);

    FILE* csr_file = fopen(csr_filename, "w");
    if (!csr_file) {
        write(client_fd, "ERROR: Internal server error (CSR write).\n", 42);
        return;
    }
    fputs(csr_start, csr_file);
    fclose(csr_file);

    // Ask the OS to sign the CSR using our Root CA
    // The -CAcreateserial flag manages the serial numbers for us automatically
    char sign_cmd[512];
    snprintf(sign_cmd, sizeof(sign_cmd),
             "openssl x509 -req -in %s -CA ../certs/ca.crt -CAkey ../certs/ca.key -CAcreateserial -out %s -days 365 2>/dev/null",
             csr_filename, crt_filename);

    if (system(sign_cmd) != 0) {
        write(client_fd, "ERROR: Certificate signing failed.\n", 35);
        unlink(csr_filename); // Clean up
        return;
    }

    // Read the newly generated Certificate and send it back to the client
    FILE* crt_file = fopen(crt_filename, "r");
    if (crt_file) {
        char output_buffer[1024];
        size_t bytes_read;

        write(client_fd, "SUCCESS: Certificate generated.\n", 32);

        while ((bytes_read = fread(output_buffer, 1, sizeof(output_buffer), crt_file)) > 0) {
            write(client_fd, output_buffer, bytes_read);
        }
        fclose(crt_file);
        printf("[Enroll] Certificate successfully issued to %s\n", hostname);
    } else {
        write(client_fd, "ERROR: Internal server error (CRT read).\n", 41);
    }

    // Clean up the temporary files
    unlink(csr_filename);
    unlink(crt_filename);
}
