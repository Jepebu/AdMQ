#ifndef RBAC_H
#define RBAC_H

// Loads the RBAC configurations from the specified file
void rbac_init(const char* filepath);

// Permission Checkers: Returns 1 if authorized, 0 if denied
int rbac_can_subscribe(const char* hostname, const char* topic);
int rbac_can_unsubscribe(const char* hostname, const char* topic);
int rbac_can_publish(const char* hostname, const char* topic);
int rbac_can_set(const char* hostname, const char* key);

#endif
