#include "rbac.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_ROLES 20
#define MAX_TOPICS 20
#define MAX_MAPPINGS 50

typedef struct {
    char name[64];
    char sub_topics[MAX_TOPICS][64];  int sub_count;  int sub_wildcard;
    char pub_topics[MAX_TOPICS][64];  int pub_count;  int pub_wildcard;
    char set_keys[MAX_TOPICS][64];    int set_count;  int set_wildcard;
} Role;

typedef struct {
    char pattern[128];
    char role_name[64];
} Mapping;

static Role roles[MAX_ROLES];
static int role_count = 0;

static Mapping mappings[MAX_MAPPINGS];
static int mapping_count = 0;

static char* trim_whitespace(char* str) {
    char* end;
    while(isspace((unsigned char)*str)) str++;
    if(*str == 0) return str; 
    end = str + strlen(str) - 1;
    while(end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return str;
}

// Parses a comma-separated list into the target array
static void parse_list(char* list_str, char dest[][64], int* count, int* wildcard) {
    *count = 0;
    *wildcard = 0;
    char* token = strtok(list_str, ",");
    while (token && *count < MAX_TOPICS) {
        char* trimmed = trim_whitespace(token);
        if (strcmp(trimmed, "*") == 0) {
            *wildcard = 1;
        } else if (strlen(trimmed) > 0) {
            strncpy(dest[*count], trimmed, 63);
            (*count)++;
        }
        token = strtok(NULL, ",");
    }
}

void rbac_init(const char* filepath) {
    FILE* file = fopen(filepath, "r");
    if (!file) {
        printf("[RBAC] Warning: Could not open '%s'. All access will be denied!\n", filepath);
        return;
    }

    char line[512];
    int parsing_map = 0;
    Role* current_role = NULL;

    while (fgets(line, sizeof(line), file)) {
        char* trimmed = trim_whitespace(line);
        if (trimmed[0] == '\0' || trimmed[0] == ';') continue;

        // Check for section headers
        if (trimmed[0] == '[') {
            if (strncmp(trimmed, "[role:", 6) == 0) {
                parsing_map = 0;
                char* end = strchr(trimmed, ']');
                if (end && role_count < MAX_ROLES) {
                    *end = '\0';
                    current_role = &roles[role_count++];
                    memset(current_role, 0, sizeof(Role));
                    strncpy(current_role->name, trimmed + 6, 63);
                }
            } else if (strcmp(trimmed, "[map]") == 0) {
                parsing_map = 1;
                current_role = NULL;
            }
            continue;
        }

        // Parse key-value pairs
        char* equals = strchr(trimmed, '=');
        if (equals) {
            *equals = '\0';
            char* key = trim_whitespace(trimmed);
            char* val = trim_whitespace(equals + 1);

            if (parsing_map && mapping_count < MAX_MAPPINGS) {
                strncpy(mappings[mapping_count].pattern, key, 127);
                strncpy(mappings[mapping_count].role_name, val, 63);
                mapping_count++;
            } else if (current_role) {
                if (strcmp(key, "SUBSCRIBE") == 0) parse_list(val, current_role->sub_topics, &current_role->sub_count, &current_role->sub_wildcard);
                else if (strcmp(key, "PUBLISH") == 0) parse_list(val, current_role->pub_topics, &current_role->pub_count, &current_role->pub_wildcard);
                else if (strcmp(key, "SET") == 0) parse_list(val, current_role->set_keys, &current_role->set_count, &current_role->set_wildcard);
            }
        }
    }
    fclose(file);
    printf("[RBAC] Loaded %d roles and %d mappings from '%s'\n", role_count, mapping_count, filepath);
}

// Checks if a string matches a pattern (supports ending with '*')
static int match_pattern(const char* pattern, const char* str) {
    int len = strlen(pattern);
    if (len == 0) return 0;
    if (strcmp(pattern, "*") == 0) return 1;
    if (pattern[len - 1] == '*') return strncmp(pattern, str, len - 1) == 0;
    return strcmp(pattern, str) == 0;
}

// Resolves the hostname to a Role
static Role* get_role(const char* hostname) {
    char target_role[64] = "DEFAULT"; 
    for (int i = 0; i < mapping_count; i++) {
        if (match_pattern(mappings[i].pattern, hostname)) {
            strncpy(target_role, mappings[i].role_name, 63);
            break; 
        }
    }
    for (int i = 0; i < role_count; i++) {
        if (strcmp(roles[i].name, target_role) == 0) return &roles[i];
    }
    return NULL;
}

int rbac_can_subscribe(const char* hostname, const char* topic) {
    Role* r = get_role(hostname);
    if (!r) return 0;
    if (r->sub_wildcard) return 1;
    for (int i=0; i<r->sub_count; i++) if (strcmp(r->sub_topics[i], topic) == 0) return 1;
    return 0;
}

int rbac_can_publish(const char* hostname, const char* topic) {
    Role* r = get_role(hostname);
    if (!r) return 0;
    if (r->pub_wildcard) return 1;
    for (int i=0; i<r->pub_count; i++) if (strcmp(r->pub_topics[i], topic) == 0) return 1;
    return 0;
}

int rbac_can_set(const char* hostname, const char* key) {
    Role* r = get_role(hostname);
    if (!r) return 0;
    if (r->set_wildcard) return 1;
    for (int i=0; i<r->set_count; i++) if (strcmp(r->set_keys[i], key) == 0) return 1;
    return 0;
}
