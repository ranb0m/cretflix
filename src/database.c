#include <sqlite3.h>
#include <stdlib.h>
#include "logger.h"

// globally shared db file descriptor 
sqlite3 *global_db = NULL;

void init_metadata_engine() {
    int rc = sqlite3_open("media_metadata.db", &global_db);
    if (rc != SQLITE_OK) {
        DISPATCH_LOG("Fatal: Cannot mount B-Tree: %s", sqlite3_errmsg(global_db), 0, 0, 0);
        exit(1);
    }

    // Enforce MVCC to physically decouple the Ingestion Daemon from the Worker Matrix
    sqlite3_exec(global_db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    // Relax the fsync() requirements for the writer since this is derived metadata
    sqlite3_exec(global_db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);

    // relational schema
    const char *schema = "CREATE TABLE IF NOT EXISTS media ("
                         "file_id INTEGER PRIMARY KEY,"
                         "filename TEXT UNIQUE,"
                         "size INTEGER,"
                         "bitrate INTEGER);";
    
    char *err_msg = 0;
    rc = sqlite3_exec(global_db, schema, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        DISPATCH_LOG("Fatal: Schema compilation failed: %s", err_msg, 0, 0, 0);
        sqlite3_free(err_msg);
        exit(1);
    }
}
