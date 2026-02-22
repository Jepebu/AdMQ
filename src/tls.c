#include "tls.h"
#include <stdio.h>
#include <stdlib.h>

// The global SSL context
static SSL_CTX *server_ctx = NULL;

void tls_init(const char* cert_path, const char* key_path, const char* ca_path) {
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();

    const SSL_METHOD *method = TLS_server_method();
    server_ctx = SSL_CTX_new(method);
    if (!server_ctx) {
        fprintf(stderr, "Unable to create SSL context\n");
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }

    if (SSL_CTX_use_certificate_file(server_ctx, cert_path, SSL_FILETYPE_PEM) <= 0) {
        fprintf(stderr, "Failed to load Server Certificate: %s\n", cert_path);
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }

    if (SSL_CTX_use_PrivateKey_file(server_ctx, key_path, SSL_FILETYPE_PEM) <= 0 ) {
        fprintf(stderr, "Failed to load Server Private Key: %s\n", key_path);
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }

    if (!SSL_CTX_check_private_key(server_ctx)) {
        fprintf(stderr, "Fatal Error: Private key does not match the public certificate\n");
        exit(EXIT_FAILURE);
    }

    // mTLS Configuration
    if (SSL_CTX_load_verify_locations(server_ctx, ca_path, NULL) <= 0) {
        fprintf(stderr, "Failed to load CA Certificate: %s\n", ca_path);
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }

    SSL_CTX_set_verify(server_ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);

    printf("[TLS] OpenSSL Context initialized. mTLS strictly enforced.\n");
}

void tls_cleanup() {
    if (server_ctx) {
        SSL_CTX_free(server_ctx);
    }
    EVP_cleanup();
    printf("[TLS] OpenSSL cleaned up.\n");
}

SSL_CTX* tls_get_context() {
    return server_ctx;
}
