#include "store.h"

#include <chrono>
#include <cstdio>
#include <ctime>

#include <sqlite3.h>

namespace openitc {
namespace {

constexpr const char* kSchema =
    "CREATE TABLE IF NOT EXISTS packets("
    "  id INTEGER PRIMARY KEY,"
    "  t_unix REAL NOT NULL,"
    "  channel INTEGER NOT NULL,"
    "  mode INTEGER NOT NULL,"
    "  type INTEGER NOT NULL,"
    "  sync_errors INTEGER NOT NULL,"
    "  payload BLOB);"
    "CREATE INDEX IF NOT EXISTS idx_packets_t ON packets(t_unix);"
    "CREATE INDEX IF NOT EXISTS idx_packets_type ON packets(type);"
    "CREATE TABLE IF NOT EXISTS beacons("
    "  radio_id INTEGER NOT NULL,"
    "  t_unix REAL NOT NULL,"
    "  lat REAL, lon REAL,"
    "  channel INTEGER, rate INTEGER, util INTEGER,"
    "  heard_channel INTEGER,"
    "  ch_list TEXT);"
    "CREATE INDEX IF NOT EXISTS idx_beacons_radio ON beacons(radio_id);"
    "CREATE TABLE IF NOT EXISTS wius("
    "  wiu_id INTEGER NOT NULL,"
    "  t_unix REAL NOT NULL,"
    "  heard_channel INTEGER, mode INTEGER,"
    "  indicators BLOB);"
    "CREATE INDEX IF NOT EXISTS idx_wius_id ON wius(wiu_id);"
    "CREATE TABLE IF NOT EXISTS time_reports("
    "  tid INTEGER NOT NULL,"
    "  t_unix REAL NOT NULL,"
    "  itc_unix REAL, drift REAL,"
    "  heard_channel INTEGER);"
    "CREATE INDEX IF NOT EXISTS idx_time_tid ON time_reports(tid);";

}  // namespace

Store::~Store() { close(); }

bool Store::open(const std::string& path) {
    if (db_) return true;
    sqlite3* db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
        std::fprintf(stderr, "store: cannot open %s: %s\n", path.c_str(),
                     db ? sqlite3_errmsg(db) : "?");
        if (db) sqlite3_close(db);
        return false;
    }
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    char* err = nullptr;
    if (sqlite3_exec(db, kSchema, nullptr, nullptr, &err) != SQLITE_OK) {
        std::fprintf(stderr, "store: schema error: %s\n", err ? err : "?");
        sqlite3_free(err);
        sqlite3_close(db);
        return false;
    }
    db_ = db;
    path_ = path;
    stop_ = false;
    th_ = std::thread([this] { writer_loop(); });
    return true;
}

void Store::observe(const PacketEvent& e) {
    if (!db_) return;
    {
        std::lock_guard<std::mutex> lk(qmu_);
        q_.push(e);
    }
    cv_.notify_one();
}

void Store::close() {
    if (!db_) return;
    {
        std::lock_guard<std::mutex> lk(qmu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
    std::lock_guard<std::mutex> lk(db_mu_);
    sqlite3_close(db_);
    db_ = nullptr;
}

void Store::writer_loop() {
    for (;;) {
        std::vector<PacketEvent> batch;
        {
            std::unique_lock<std::mutex> lk(qmu_);
            cv_.wait_for(lk, std::chrono::milliseconds(500),
                         [this] { return stop_ || !q_.empty(); });
            while (!q_.empty()) {
                batch.push_back(std::move(q_.front()));
                q_.pop();
            }
        }
        if (!batch.empty()) write_batch(batch);
        if (stop_ && batch.empty()) break;
    }
}

void Store::write_batch(const std::vector<PacketEvent>& batch) {
    std::lock_guard<std::mutex> lk(db_mu_);
    if (!db_) return;
    sqlite3_exec(db_, "BEGIN;", nullptr, nullptr, nullptr);

    sqlite3_stmt* pkt = nullptr;
    sqlite3_stmt* bcn = nullptr;
    sqlite3_stmt* wiu = nullptr;
    sqlite3_stmt* tim = nullptr;
    sqlite3_prepare_v2(db_,
                       "INSERT INTO packets(t_unix,channel,mode,type,sync_errors,payload)"
                       " VALUES(?,?,?,?,?,?);",
                       -1, &pkt, nullptr);
    sqlite3_prepare_v2(db_,
                       "INSERT INTO beacons(radio_id,t_unix,lat,lon,channel,rate,util,"
                       "heard_channel,ch_list) VALUES(?,?,?,?,?,?,?,?,?);",
                       -1, &bcn, nullptr);
    sqlite3_prepare_v2(db_,
                       "INSERT INTO wius(wiu_id,t_unix,heard_channel,mode,indicators)"
                       " VALUES(?,?,?,?,?);",
                       -1, &wiu, nullptr);
    sqlite3_prepare_v2(db_,
                       "INSERT INTO time_reports(tid,t_unix,itc_unix,drift,heard_channel)"
                       " VALUES(?,?,?,?,?);",
                       -1, &tim, nullptr);

    for (const PacketEvent& e : batch) {
        if (pkt) {
            sqlite3_bind_double(pkt, 1, e.t_unix);
            sqlite3_bind_int(pkt, 2, e.channel);
            sqlite3_bind_int(pkt, 3, e.mode);
            sqlite3_bind_int(pkt, 4, e.type);
            sqlite3_bind_int(pkt, 5, e.sync_errors);
            sqlite3_bind_blob(pkt, 6, e.payload.data(), (int)e.payload.size(), SQLITE_STATIC);
            sqlite3_step(pkt);
            sqlite3_reset(pkt);
        }
        if (e.type == 0x02 && e.beacon.valid && bcn) {
            const BeaconInfo& b = e.beacon;
            char list[32];
            std::snprintf(list, sizeof(list), "%d,%d,%d,%d,%d,%d", b.ch_list[0], b.ch_list[1],
                          b.ch_list[2], b.ch_list[3], b.ch_list[4], b.ch_list[5]);
            sqlite3_bind_int(bcn, 1, (int)b.radio_id);
            sqlite3_bind_double(bcn, 2, e.t_unix);
            sqlite3_bind_double(bcn, 3, b.lat);
            sqlite3_bind_double(bcn, 4, b.lon);
            sqlite3_bind_int(bcn, 5, b.channel);
            sqlite3_bind_int(bcn, 6, b.rate);
            sqlite3_bind_int(bcn, 7, b.util);
            sqlite3_bind_int(bcn, 8, e.channel);
            sqlite3_bind_text(bcn, 9, list, -1, SQLITE_TRANSIENT);
            sqlite3_step(bcn);
            sqlite3_reset(bcn);
        }
        if ((e.type == 0x33 || e.type == 0x34) && e.wiu.valid && wiu) {
            sqlite3_bind_int(wiu, 1, (int)e.wiu.wiu_id);
            sqlite3_bind_double(wiu, 2, e.t_unix);
            sqlite3_bind_int(wiu, 3, e.channel);
            sqlite3_bind_int(wiu, 4, e.mode);
            sqlite3_bind_blob(wiu, 5, e.wiu.indicators.data(), (int)e.wiu.indicators.size(),
                              SQLITE_STATIC);
            sqlite3_step(wiu);
            sqlite3_reset(wiu);
        }
        if (e.type == 0x06 && e.time.valid && tim) {
            // Reconstruct the ITC UTC stamp the same way the repository does.
            std::tm tm{};
            tm.tm_year = e.time.year - 1900;
            tm.tm_mon = e.time.month - 1;
            tm.tm_mday = e.time.day;
            tm.tm_hour = e.time.hour;
            tm.tm_min = e.time.minute;
            tm.tm_sec = e.time.second;
#ifdef _WIN32
            const time_t itc = _mkgmtime(&tm);
#else
            const time_t itc = timegm(&tm);
#endif
            sqlite3_bind_int(tim, 1, (int)e.time.tid);
            sqlite3_bind_double(tim, 2, e.t_unix);
            if (itc != (time_t)-1) {
                sqlite3_bind_double(tim, 3, (double)itc);
                sqlite3_bind_double(tim, 4, (double)itc - e.t_unix);
            } else {
                sqlite3_bind_null(tim, 3);
                sqlite3_bind_null(tim, 4);
            }
            sqlite3_bind_int(tim, 5, e.channel);
            sqlite3_step(tim);
            sqlite3_reset(tim);
        }
    }

    sqlite3_finalize(pkt);
    sqlite3_finalize(bcn);
    sqlite3_finalize(wiu);
    sqlite3_finalize(tim);
    sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
}

long Store::count() const {
    std::lock_guard<std::mutex> lk(db_mu_);
    if (!db_) return 0;
    sqlite3_stmt* st = nullptr;
    long n = 0;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM packets;", -1, &st, nullptr) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) n = (long)sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    return n;
}

std::vector<PacketRow> Store::recent_packets(int limit) const {
    std::lock_guard<std::mutex> lk(db_mu_);
    std::vector<PacketRow> out;
    if (!db_) return out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "SELECT id,t_unix,channel,mode,type,sync_errors,payload FROM packets"
                           " ORDER BY id DESC LIMIT ?;",
                           -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, limit);
        while (sqlite3_step(st) == SQLITE_ROW) {
            PacketRow r;
            r.id = (long)sqlite3_column_int64(st, 0);
            r.t_unix = sqlite3_column_double(st, 1);
            r.channel = sqlite3_column_int(st, 2);
            r.mode = sqlite3_column_int(st, 3);
            r.type = sqlite3_column_int(st, 4);
            r.sync_errors = sqlite3_column_int(st, 5);
            const void* blob = sqlite3_column_blob(st, 6);
            const int nb = sqlite3_column_bytes(st, 6);
            if (blob && nb > 0)
                r.payload.assign((const uint8_t*)blob, (const uint8_t*)blob + nb);
            out.push_back(std::move(r));
        }
        sqlite3_finalize(st);
    }
    return out;
}

}  // namespace openitc
