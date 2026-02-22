#ifndef CONFIG_H
#define CONFIG_H

// The struct holding all our runtime variables
typedef struct {
    int vault_port;
    int lobby_port;
    char cert_path[256];
    char key_path[256];
    char ca_path[256];
    char db_path[256];
} BrokerConfig;

// Parses the INI file and populates the struct.
// Returns 1 on success, 0 if the file is missing (falls back to defaults).
int config_load(const char* filepath, BrokerConfig* config);

#endif
