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

    printf("[DB] SQLite Audit Database initialized at '%s'\n", filepath);
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
