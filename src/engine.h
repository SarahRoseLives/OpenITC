#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "model.h"
#include "rtl_source.h"

namespace openitc {

struct EngineConfig {
    RtlConfig rtl{};
    std::vector<int> channels;  // empty => default_channels()
    int chunk_ms = 250;
    int max_errors = 8;
    bool decode_fr = true;
    bool decode_hr = true;
};

struct EngineStats {
    unsigned long long chunks = 0;
    unsigned long long packets = 0;
    unsigned long long sync_hits = 0;
};

// The 27 on-air PTC channels.
std::vector<int> default_channels();

// Owns the SDR + channelizer + per-channel/per-rate decode loop and runs it on
// a worker thread, emitting a PacketEvent for every decoded packet. Consumers
// (console printer, repository, GUI) subscribe via the callback. The callback
// runs on the worker thread and must not block.
class MonitorEngine {
public:
    using Callback = std::function<void(const PacketEvent&)>;

    explicit MonitorEngine(EngineConfig cfg);
    ~MonitorEngine();

    MonitorEngine(const MonitorEngine&) = delete;
    MonitorEngine& operator=(const MonitorEngine&) = delete;

    void set_callback(Callback cb) { cb_ = std::move(cb); }

    // Opens the device and spawns the worker thread. Returns false on error;
    // see last_error().
    bool start();
    void stop();
    bool running() const { return running_.load(); }

    const EngineConfig& config() const { return cfg_; }
    std::string device_name() const;
    std::string last_error() const;
    EngineStats stats() const;

private:
    void run();
    void emit(const PacketEvent& e);

    EngineConfig cfg_;
    Callback cb_;
    std::unique_ptr<RtlSource> src_;
    std::thread th_;
    std::atomic<bool> running_{false};
    mutable std::mutex mu_;
    std::string last_error_;
    EngineStats stats_;
};

}  // namespace openitc
