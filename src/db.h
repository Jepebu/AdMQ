#ifndef DB_H
#define DB_H

// Opens the database file and creates the table if it doesn't exist
void db_init(const char* filepath);

// Safely inserts a new record into the audit log
void db_log_message(const char* sender, const char* topic, const char* message);

// Closes the database connection
void db_close();

// Set a specific key in the database
void db_set_device_state(const char* hostname, const char* key, const char* value);

// Retrieves a value from the state table. Returns 1 if found, 0 if not.
int db_get_device_state(const char* hostname, const char* key, char* out_value, int max_len);

#endif
