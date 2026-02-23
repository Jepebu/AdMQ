#include "agent_config.h"
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

int agent_config_load(const char* filepath, AgentConfig* config) {

    // Defaults
    strncpy(config->broker_ip, "127.0.0.1", 63);
    config->broker_port = 35565;
    strncpy(config->cert_path, "certs/client.crt", 255);
    strncpy(config->key_path, "certs/client.key", 255);
    strncpy(config->ca_path, "certs/ca.crt", 255);
    strncpy(config->command_group, "CMD-GRP-1", 63);
    strncpy(config->action_dir, "./actions", 255);

    FILE* file = fopen(filepath, "r");
    if (!file) {
        printf("[Agent Config] Warning: Could not open '%s'. Using defaults.\n", filepath);
        return 0;
    }

    char line[512];
    while (fgets(line, sizeof(line), file)) {
        char* trimmed = trim_whitespace(line);
        if (trimmed[0] == '\0' || trimmed[0] == ';' || trimmed[0] == '#' || trimmed[0] == '[') continue;

        char* equals_sign = strchr(trimmed, '=');
        if (equals_sign) {
            *equals_sign = '\0';
            char* key = trim_whitespace(trimmed);
            char* val = trim_whitespace(equals_sign + 1);

            if (val[0] == '"' && val[strlen(val)-1] == '"') {
                val[strlen(val)-1] = '\0';
                val++;
            }

            if (strcmp(key, "broker_ip") == 0) strncpy(config->broker_ip, val, sizeof(config->broker_ip) - 1);
            else if (strcmp(key, "broker_port") == 0) config->broker_port = atoi(val);
            else if (strcmp(key, "cert_path") == 0) strncpy(config->cert_path, val, sizeof(config->cert_path) - 1);
            else if (strcmp(key, "key_path") == 0) strncpy(config->key_path, val, sizeof(config->key_path) - 1);
            else if (strcmp(key, "ca_path") == 0) strncpy(config->ca_path, val, sizeof(config->ca_path) - 1);
            else if (strcmp(key, "command_group") == 0) strncpy(config->command_group, val, sizeof(config->command_group) - 1);
            else if (strcmp(key, "action_dir") == 0) strncpy(config->action_dir, val, sizeof(config->action_dir) - 1);
        }
    }

    fclose(file);
    return 1;
}
