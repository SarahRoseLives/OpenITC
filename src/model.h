#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace openitc {

// ITC link-layer packet type name (see docs/reverse-engineering.md).
inline const char* type_name(int t) {
    switch (t) {
        case 0x01: return "QSTAT";
        case 0x02: return "beacon";
        case 0x03: return "ACK";
        case 0x04: return "ACQ";
        case 0x05: return "CNTL";
        case 0x06: return "TIME";
        case 0x07: return "T07";
        case 0x31: return "31";
        case 0x33: return "WIU";
        case 0x34: return "WIU";
        case 0x70: return "unicast";
        default: return "";
    }
}

// Engine mode codes: 0 = FR (16 ksym/s), 1 = HR (8 ksym/s).
inline const char* mode_name(int m) { return m == 1 ? "HR" : "FR"; }

// 24-bit big-endian transmitter/radio ID at the start of a payload.
inline uint32_t tid24(const std::vector<uint8_t>& p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

// Decoded base-beacon (type 0x02) fields. Offsets per the reference decoder:
// flags u8, radio_id u24 BE, lat/lon i32 BE / 600000, ch/rate/util/flag1 u8,
// 6-channel list.
struct BeaconInfo {
    uint8_t flags = 0;
    uint32_t radio_id = 0;
    double lat = 0.0;
    double lon = 0.0;
    int channel = 0;
    int rate = 0;
    int util = 0;
    int flag1 = 0;
    int ch_list[6] = {0, 0, 0, 0, 0, 0};
    bool valid = false;
};

// Decoded TIME (type 0x06) broadcast: TID then UTC date/time (year-2000).
struct TimeInfo {
    uint32_t tid = 0;
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    bool valid = false;
};

// Decoded CNTL (type 0x05): TID, sequence number, and payload data.
struct CntlInfo {
    uint32_t tid = 0;
    int seq = 0;
    std::vector<uint8_t> data;
    bool valid = false;
};

// Decoded wayside status (types 0x33/0x34): WIU ID plus indicator bits.
// `has_typical` marks a best-effort decode of the common 1-switch + 3-signal
// layout (exact layout is per-WIU and configured in ITCMon).
struct WiuInfo {
    uint32_t wiu_id = 0;
    std::vector<uint8_t> indicators;  // raw indicator bytes (after the ID)
    bool has_typical = false;
    int sw = 0;          // 0=I 1=N 2=R 3=X
    int sig[3] = {0, 0, 0};
    bool valid = false;
};

// One fully decoded packet as produced by the monitor engine.
struct PacketEvent {
    double t_unix = 0.0;  // PC wall-clock time (unix seconds, UTC-based) decoded
    int channel = 0;      // ITC channel number
    int mode = 0;         // 0 = FR, 1 = HR
    int type = 0;         // ITC link-layer type
    int sync_errors = 0;  // unique-word Hamming distance
    std::vector<uint8_t> payload;

    // Decoded views; only the one matching `type` has valid == true.
    BeaconInfo beacon;
    TimeInfo time;
    CntlInfo cntl;
    WiuInfo wiu;
};

}  // namespace openitc
