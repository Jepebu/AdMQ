#ifndef TLS_H
#define TLS_H

#include <openssl/ssl.h>
#include <openssl/err.h>

// Initializes the OpenSSL library and loads the certificates
void tls_init(const char* cert_path, const char* key_path, const char* ca_path);

// Cleans up the global context on shutdown
void tls_cleanup();

// Returns the global SSL Context (so workers can use it to create connections)
SSL_CTX* tls_get_context();

#endif
