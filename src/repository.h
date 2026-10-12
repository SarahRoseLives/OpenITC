#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

#include "model.h"

namespace openitc {

// ITC UTC calendar time (from a TIME packet) as unix seconds; 0 if invalid.
double itc_time_to_unix(const TimeInfo& t);

struct BeaconRecord {
    BeaconInfo info;
    double first_seen = 0.0;
    double last_seen = 0.0;
    int count = 0;
    int channel = 0;  // channel the beacon was heard on
    int mode = 0;     // 0 = FR, 1 = HR
};

struct WiuRecord {
    WiuInfo info;
    double first_seen = 0.0;
    double last_seen = 0.0;
    int count = 0;
    int channel = 0;
    int mode = 0;
};

struct TimeRecord {
    TimeInfo info;
    double last_seen = 0.0;  // PC wall-clock time of the latest sighting
    int count = 0;
    double itc_unix = 0.0;   // ITC UTC stamp as unix seconds
    double drift = 0.0;      // itc_unix - PC unix time (clock offset, seconds)
    int channel = 0;
    int mode = 0;
};

// Live, thread-safe "latest by key" state fed by PacketEvents. The GUI reads
// snapshots; the engine thread only calls observe().
class Repository {
public:
    void observe(const PacketEvent& e);
    void clear();

    std::vector<BeaconRecord> beacons() const;
    std::vector<WiuRecord> wius() const;
    std::vector<TimeRecord> times() const;

    long total() const;
    double packets_per_min() const;
    std::map<int, long> type_counts() const;
    std::map<int, long> channel_counts() const;

private:
    mutable std::mutex mu_;
    std::map<uint32_t, BeaconRecord> beacons_;
    std::map<uint32_t, WiuRecord> wius_;
    std::map<uint32_t, TimeRecord> times_;
    std::map<int, long> type_counts_;
    std::map<int, long> channel_counts_;
    long total_ = 0;
    std::deque<double> recent_;  // timestamps within the last 60 s
};

}  // namespace openitc
