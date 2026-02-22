#ifndef DB_H
#define DB_H

// Opens the database file and creates the table if it doesn't exist
void db_init(const char* filepath);

// Safely inserts a new record into the audit log
void db_log_message(const char* sender, const char* topic, const char* message);

// Closes the database connection
void db_close();

#endif
