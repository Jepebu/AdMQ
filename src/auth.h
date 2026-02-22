#ifndef AUTH_H
#define AUTH_H

#define AUTH_PENDING 0
#define AUTH_SUCCESS 1

#include <openssl/ssl.h>

// Verifies the socket IP against the DNS record of the claimed hostname.
// Returns 1 if verified, 0 if it fails.
int auth_verify_identity(int client_fd, const char* claimed_hostname);

// Extracts the CN from the certificate and runs the DNS/IP check
int auth_verify_mtls(int client_fd, SSL* ssl, char* out_cn, int max_len);

#endif
