#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static char* trim_whitespace(char* str) {
    char* end;
    while(isspace((unsigned char)*str)) str++;
    if(*str == 0) return str;

    end = str + strlen(str) - 1;
    while(end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return str;
}

int config_load(const char* filepath, BrokerConfig* config) {
    // Set safe defaults just in case the file is missing or a key is deleted
    config->vault_port = 35565;
    config->lobby_port = 35566;
    strncpy(config->cert_path, "certs/server.crt", 255);
    strncpy(config->key_path, "certs/server.key", 255);
    strncpy(config->ca_path, "certs/ca.crt", 255);
    strncpy(config->db_path, "broker_audit.db", 255);

    FILE* file = fopen(filepath, "r");
    if (!file) {
        printf("[Config] Warning: Could not open '%s'. Using default settings.\n", filepath);
        return 0; // Fall back to defaults
    }

    char line[512];
    while (fgets(line, sizeof(line), file)) {
        char* trimmed = trim_whitespace(line);

        // Skip comments, empty lines, and section headers
        if (trimmed[0] == '\0' || trimmed[0] == ';' || trimmed[0] == '#' || trimmed[0] == '[') {
            continue;
        }

        char* equals_sign = strchr(trimmed, '=');
        if (equals_sign) {
            *equals_sign = '\0'; // Split the string

            char* key = trim_whitespace(trimmed);
            char* val = trim_whitespace(equals_sign + 1);

            // Strip optional quotes
            if (val[0] == '"' && val[strlen(val)-1] == '"') {
                val[strlen(val)-1] = '\0';
                val++;
            }

            // Map keys to our struct
            if (strcmp(key, "vault_port") == 0) config->vault_port = atoi(val);
            else if (strcmp(key, "lobby_port") == 0) config->lobby_port = atoi(val);
            else if (strcmp(key, "cert_path") == 0) strncpy(config->cert_path, val, sizeof(config->cert_path) - 1);
            else if (strcmp(key, "key_path") == 0) strncpy(config->key_path, val, sizeof(config->key_path) - 1);
            else if (strcmp(key, "ca_path") == 0) strncpy(config->ca_path, val, sizeof(config->ca_path) - 1);
            else if (strcmp(key, "db_path") == 0) strncpy(config->db_path, val, sizeof(config->db_path) - 1);
        }
    }

    fclose(file);
    return 1;
}
