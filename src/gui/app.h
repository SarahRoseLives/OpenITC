#pragma once

#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "engine.h"
#include "model.h"
#include "repository.h"
#include "store.h"
#include "tiles.h"

namespace openitc::gui {

using openitc::PacketEvent;

// Bounded, thread-safe recent-packet log fed by the engine thread.
class PacketLog {
public:
    void add(const PacketEvent& e) {
        std::lock_guard<std::mutex> lk(mu_);
        items_.push_back(e);
        if (items_.size() > kCap) items_.pop_front();
    }
    std::vector<PacketEvent> snapshot(size_t max_items) const {
        std::lock_guard<std::mutex> lk(mu_);
        const size_t n = items_.size() < max_items ? items_.size() : max_items;
        return std::vector<PacketEvent>(items_.end() - n, items_.end());
    }
    void clear() {
        std::lock_guard<std::mutex> lk(mu_);
        items_.clear();
    }

private:
    static constexpr size_t kCap = 50000;
    mutable std::mutex mu_;
    std::deque<PacketEvent> items_;
};

struct Filters {
    char text[64] = "";
    int channel = 0;  // 0 = any
    int type = -1;    // -1 = any
    bool match(const PacketEvent& e) const;
    void reset() {
        text[0] = '\0';
        channel = 0;
        type = -1;
    }
};

struct AppState {
    EngineConfig ec;
    Repository repo;
    PacketLog log;
    Store store;
    std::unique_ptr<MonitorEngine> engine;
    bool running = false;
    bool recording = true;
    std::string status;
    std::string db_path;
    std::vector<std::string> sdr_devices;  // probed at startup

    Filters filters;

    // Terminal view controls.
    bool paused = false;
    std::vector<PacketEvent> frozen;  // snapshot captured when paused

    // Map state.
    TileCache tiles;
    MapView map;
    uint32_t map_selected = 0;  // radio_id of the clicked beacon (0 = none)
    bool map_autofit = true;    // fit the map once the first beacon arrives

    void subscribe(MonitorEngine& eng);
    void start();
    void stop();
};

// Shared formatting helpers.
std::string hexs(const std::vector<uint8_t>& d, size_t from, size_t maxn);
std::string summarize(const PacketEvent& e);
void local_hms(double t_unix, char* out, size_t n);
std::string default_db_path();
std::string tile_cache_dir();

// Panels.
void draw_header(AppState& s);
void draw_sidebar(AppState& s);
void draw_map(AppState& s, const ImVec2& size);
void draw_terminal(AppState& s, const ImVec2& size);

}  // namespace openitc::gui
