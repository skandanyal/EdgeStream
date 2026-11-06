#include "../include/database.hpp"
#include <syncstream>

int telemetry_callback (void *data, int argc, char **argv, char **column_name) {
    for (int i=0; i<argc; i++) {
        std::cout << column_name[i] << ": " << (argv[i] ? argv[i] : "NULL") << "\n";
    }
    std::osyncstream(std::cout) << "/*------------------*/\n";
    return 0;
}

/*
* functions to implement -
*     1. constructor
*     2. create table (if not exists)
*     3. insert into table
*     4. display from table
*     5. destructor
*/

/*
 * return values -
 * 0 - successful
 * 1 - failure
 */

Database::Database(const std::string &db_name) {
    exit = sqlite3_open(db_name.c_str(), &db);
    if (exit != SQLITE_OK) {
        std::osyncstream(std::cout) << "Failed to open database: " << db << ": " << sqlite3_errmsg(db) << "\n";
    } else {
        create_table();
    }
}


int Database::create_table() {
    query = "CREATE TABLE IF NOT EXISTS telemetry ("
            "ID INTEGER PRIMARY KEY AUTOINCREMENT,"
            "FRAME_NO INTEGER NOT NULL,"
            "TIME_STAMP_MS INTEGER NOT NULL,"
            "QUEUE_SIZE INTEGER NOT NULL,"
            "FPS REAL);";

    err_msg = nullptr, exit = -1;
    exit = sqlite3_exec(db, query.c_str(), nullptr, nullptr, &err_msg);

    if (exit != SQLITE_OK) {
        std::osyncstream(std::cerr) << "Database error (create_table): " << err_msg << "\n";
        sqlite3_free(err_msg);
        return 1;
    }
    return 0;
}

int Database::insert_telemetry(int frame_no, long long timestamp_ms, int queue_size, float fps) {
    query = "INSERT INTO telemetry (FRAME_NO, TIME_STAMP_MS, QUEUE_SIZE, FPS) VALUES ("
    + std::to_string(frame_no) + ", "
    + std::to_string(timestamp_ms) + ", "
    + std::to_string(queue_size) + ", "
    + std::to_string(fps) + ");";

    err_msg = nullptr, exit = -1;
    exit = sqlite3_exec(db, query.c_str(), nullptr, nullptr, &err_msg);

    if (exit != SQLITE_OK) {
        std::osyncstream(std::cerr) << "Database error (create_table): " << err_msg << "\n";
        sqlite3_free(err_msg);
        return 1;
    }
    return 0;
}

int Database::display_telemetry() {
    query = "SELECT * FROM telemetry;";
    err_msg = nullptr, exit = -1;
    exit = sqlite3_exec(db, query.c_str(), telemetry_callback, nullptr, &err_msg);

    if (exit != SQLITE_OK) {
        std::osyncstream(std::cerr) << "Database error (create_table): " << err_msg << "\n";
        sqlite3_free(err_msg);
        return 1;
    }
    return 0;
}

int Database::display_telemetry_metrics() {
    query = "SELECT "
            "MIN(TIME_STAMP_MS) AS min_time_stamp_ms, "  // min, max time_stamp
            "MAX(TIME_STAMP_MS) AS max_time_stamp_ms, "
            "MAX(QUEUE_SIZE) AS max_queue_size, "        // max, avg queue_size
            "AVG(QUEUE_SIZE) AS avg_queue_size, "
            "MAX(FPS) AS max_fps, "                      // max, avg fps
            "AVG(FPS) AS avg_fps "
            "FROM telemetry";
    err_msg = nullptr, exit = -1;
    exit = sqlite3_exec(db, query.c_str(), telemetry_callback, nullptr, &err_msg);

    if (exit != SQLITE_OK) {
        std::osyncstream(std::cerr) << "Database error (display_telemetry metrics): " << err_msg << "\n";
        sqlite3_free(err_msg);
        return 1;
    }
    return 0;
}

Database::~Database() {
    if (db) {
        sqlite3_close(db);
        db = nullptr;
    }
}



