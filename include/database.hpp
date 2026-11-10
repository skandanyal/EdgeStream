#ifndef DATABASE_HPP
#define DATABASE_HPP

#include <iostream>
#include <sqlite3.h>
#include <string>

class Database {
private:
    sqlite3 *db = nullptr;
    std::string db_name;
    int exit;
    char *err_msg;
    std::string query;

    int create_table();

public:
  /*
   * functions to implement -
   *     1. constructor
   *     2. create table (if not exists)
   *     3. insert into table
   *     4. display from table
   *     5. destructor
   */

    explicit Database(const std::string &db_name = "database/EdgeStream_data.db");

    // Automatically captures current system timestamp in ms to keep main/consumer clean
    int log_frame(int frame_no, int queue_size, float fps, int dropped_frames = 0);

    int insert_telemetry(int frame_no, long long timestamp_ms, int queue_size,
                        float fps, int dropped_frames = 0);

    int display_telemetry();

    int display_telemetry_metrics();

    ~Database();
};

#endif