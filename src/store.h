#pragma once

#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "model.h"

struct sqlite3;

namespace openitc {

struct PacketRow {
    long id = 0;
    double t_unix = 0.0;
    int channel = 0;
    int mode = 0;
    int type = 0;
    int sync_errors = 0;
    std::vector<uint8_t> payload;
};

// Persists packet events to a SQLite database from a background writer thread.
// observe() is cheap and thread-safe (it just enqueues); a dedicated thread
// batches inserts in transactions so the decode loop never blocks on disk.
class Store {
public:
    Store() = default;
    ~Store();

    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    bool open(const std::string& path);
    void close();
    bool is_open() const { return db_ != nullptr; }
    const std::string& path() const { return path_; }

    // Enqueue a decoded packet (no-op if not open). Thread-safe.
    void observe(const PacketEvent& e);

    long count() const;
    std::vector<PacketRow> recent_packets(int limit) const;

private:
    void writer_loop();
    void write_batch(const std::vector<PacketEvent>& batch);

    sqlite3* db_ = nullptr;
    std::string path_;

    std::thread th_;
    std::mutex qmu_;
    std::condition_variable cv_;
    std::queue<PacketEvent> q_;
    bool stop_ = false;

    mutable std::mutex db_mu_;  // serializes all sqlite calls
};

}  // namespace openitc
