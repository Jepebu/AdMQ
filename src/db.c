#include "db.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>

static sqlite3 *db = NULL;
static pthread_mutex_t db_lock = PTHREAD_MUTEX_INITIALIZER;

void db_init(const char* filepath) {
    if (sqlite3_open(filepath, &db) != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        exit(1);
    }

    // Create the table (DATETIME DEFAULT CURRENT_TIMESTAMP automatically logs the exact time)
    const char *sql_create_table =
        "CREATE TABLE IF NOT EXISTS audit_log ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "timestamp DATETIME DEFAULT CURRENT_TIMESTAMP, "
        "sender TEXT, "
        "topic TEXT, "
        "message TEXT);";

    char *err_msg = NULL;
    if (sqlite3_exec(db, sql_create_table, 0, 0, &err_msg) != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
        exit(1);
    }

    // --- NEW: Device State Table ---
    // PRIMARY KEY (hostname, key) ensures we overwrite old values instead of making duplicates
    const char *sql_create_state_table =
        "CREATE TABLE IF NOT EXISTS device_state ("
        "hostname TEXT, "
        "key TEXT, "
        "value TEXT, "
        "last_updated DATETIME DEFAULT CURRENT_TIMESTAMP, "
        "PRIMARY KEY(hostname, key));";

    if (sqlite3_exec(db, sql_create_state_table, 0, 0, &err_msg) != SQLITE_OK) {
        fprintf(stderr, "SQL error (device_state): %s\n", err_msg);
        sqlite3_free(err_msg);
        exit(1);
    }



    // printf("[DB] SQLite Audit Database initialized at '%s'\n", filepath);
}

void db_set_device_state(const char* hostname, const char* key, const char* value) {
    if (!db) return;
    pthread_mutex_lock(&db_lock);

    // INSERT OR REPLACE will update the row if the hostname+key combination already exists
    const char *sql = "INSERT OR REPLACE INTO device_state (hostname, key, value, last_updated) VALUES (?, ?, ?, CURRENT_TIMESTAMP);";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, hostname, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, key, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, value, -1, SQLITE_STATIC);

        if (sqlite3_step(stmt) != SQLITE_DONE) {
            fprintf(stderr, "Failed to execute state update: %s\n", sqlite3_errmsg(db));
        }
        sqlite3_finalize(stmt);
    }
    pthread_mutex_unlock(&db_lock);
}

int db_get_device_state(const char* hostname, const char* key, char* out_value, int max_len) {
    if (!db) return 0;
    int found = 0;

    pthread_mutex_lock(&db_lock);

    const char *sql = "SELECT value FROM device_state WHERE hostname = ? AND key = ?;";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, hostname, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, key, -1, SQLITE_STATIC);

        // If SQLITE_ROW is returned, it means we found a match!
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* val = sqlite3_column_text(stmt, 0);
            if (val) {
                strncpy(out_value, (const char*)val, max_len - 1);
                out_value[max_len - 1] = '\0';
                found = 1;
            }
        }
        sqlite3_finalize(stmt);
    }

    pthread_mutex_unlock(&db_lock);
    return found;
}



void db_log_message(const char* sender, const char* topic, const char* message) {
    if (!db) return;

    pthread_mutex_lock(&db_lock);

    // Prepare the SQL statement with placeholders (?)
    const char *sql = "INSERT INTO audit_log (sender, topic, message) VALUES (?, ?, ?);";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        // Bind the variables to the placeholders safely
        // SQLITE_STATIC tells SQLite we aren't freeing these strings until the query runs
        sqlite3_bind_text(stmt, 1, sender, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, topic, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, message, -1, SQLITE_STATIC);

        // Execute the statement
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            fprintf(stderr, "Failed to execute statement: %s\n", sqlite3_errmsg(db));
        }

        // Clean up the statement object
        sqlite3_finalize(stmt);
    } else {
        fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db));
    }

    pthread_mutex_unlock(&db_lock);
}

void db_close() {
    if (db) {
        sqlite3_close(db);
        printf("[DB] SQLite database closed.\n");
    }
}
