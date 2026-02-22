#ifndef AGENT_CONFIG_H
#define AGENT_CONFIG_H

typedef struct {
    char broker_ip[64];
    int broker_port;
    char cert_path[256];
    char key_path[256];
    char ca_path[256];
    char command_group[64];
    char action_dir[256];
} AgentConfig;

int agent_config_load(const char* filepath, AgentConfig* config);

#endif
