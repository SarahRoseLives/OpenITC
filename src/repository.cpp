#include "repository.h"

#include <ctime>

namespace openitc {

// Unix seconds for a UTC calendar time. Returns 0 on an invalid date.
double itc_time_to_unix(const TimeInfo& t) {
    std::tm tm{};
    tm.tm_year = t.year - 1900;
    tm.tm_mon = t.month - 1;
    tm.tm_mday = t.day;
    tm.tm_hour = t.hour;
    tm.tm_min = t.minute;
    tm.tm_sec = t.second;
#ifdef _WIN32
    const time_t v = _mkgmtime(&tm);
#else
    const time_t v = timegm(&tm);
#endif
    if (v == (time_t)-1) return 0.0;
    return (double)v;
}

void Repository::observe(const PacketEvent& e) {
    std::lock_guard<std::mutex> lk(mu_);
    ++total_;
    type_counts_[e.type]++;
    channel_counts_[e.channel]++;
    recent_.push_back(e.t_unix);
    while (!recent_.empty() && e.t_unix - recent_.front() > 60.0) recent_.pop_front();

    switch (e.type) {
        case 0x02: {
            if (!e.beacon.valid) break;
            BeaconRecord& r = beacons_[e.beacon.radio_id];
            if (r.count == 0) r.first_seen = e.t_unix;
            r.info = e.beacon;
            r.last_seen = e.t_unix;
            r.channel = e.channel;
            r.mode = e.mode;
            r.count++;
            break;
        }
        case 0x33:
        case 0x34: {
            if (!e.wiu.valid) break;
            WiuRecord& r = wius_[e.wiu.wiu_id];
            if (r.count == 0) r.first_seen = e.t_unix;
            r.info = e.wiu;
            r.last_seen = e.t_unix;
            r.channel = e.channel;
            r.mode = e.mode;
            r.count++;
            break;
        }
        case 0x06: {
            if (!e.time.valid) break;
            TimeRecord& r = times_[e.time.tid];
            r.info = e.time;
            r.last_seen = e.t_unix;
            r.channel = e.channel;
            r.mode = e.mode;
            r.count++;
            const double itc = itc_time_to_unix(e.time);
            if (itc > 0.0) {
                r.itc_unix = itc;
                r.drift = itc - e.t_unix;
            }
            break;
        }
        default:
            break;
    }
}

void Repository::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    beacons_.clear();
    wius_.clear();
    times_.clear();
    type_counts_.clear();
    channel_counts_.clear();
    recent_.clear();
    total_ = 0;
}

std::vector<BeaconRecord> Repository::beacons() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<BeaconRecord> out;
    out.reserve(beacons_.size());
    for (const auto& kv : beacons_) out.push_back(kv.second);
    return out;
}

std::vector<WiuRecord> Repository::wius() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<WiuRecord> out;
    out.reserve(wius_.size());
    for (const auto& kv : wius_) out.push_back(kv.second);
    return out;
}

std::vector<TimeRecord> Repository::times() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<TimeRecord> out;
    out.reserve(times_.size());
    for (const auto& kv : times_) out.push_back(kv.second);
    return out;
}

long Repository::total() const {
    std::lock_guard<std::mutex> lk(mu_);
    return total_;
}

double Repository::packets_per_min() const {
    std::lock_guard<std::mutex> lk(mu_);
    return (double)recent_.size();
}

std::map<int, long> Repository::type_counts() const {
    std::lock_guard<std::mutex> lk(mu_);
    return type_counts_;
}

std::map<int, long> Repository::channel_counts() const {
    std::lock_guard<std::mutex> lk(mu_);
    return channel_counts_;
}

}  // namespace openitc
