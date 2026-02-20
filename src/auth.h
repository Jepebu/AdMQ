#ifndef AUTH_H
#define AUTH_H

#define AUTH_PENDING 0
#define AUTH_SUCCESS 1

// Verifies the socket IP against the DNS record of the claimed hostname.
// Returns 1 if verified, 0 if it fails.
int auth_verify_identity(int client_fd, const char* claimed_hostname);

#endif
