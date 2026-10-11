#pragma once

#include <cstdint>
#include <vector>

#include "dsp.h"

namespace openitc {

// 64-bit ITC unique word, expanded from the ASCII string "f337eeb637660672"
// exactly as s2p's FUN_100401b60 does (MSB-first per hex nibble).
inline constexpr const char* kUniqueWordBits =
    "1111001100110111111011101011011000110111011001100000011001110010";
inline constexpr int kUniqueWordLen = 64;

struct SyncHit {
    size_t bit_pos;  // index in the demodulated bit array where the UW starts
    int errors;      // Hamming distance to the unique word
};

// Differential pi/4-DQPSK demodulation. `offset` selects the symbol sampling
// phase in [0, sps). Produces two bits per symbol (b0 then b1), matching s2p.
void pi4dqpsk_demod(const std::vector<cf>& baseband, int sps, int offset,
                    std::vector<uint8_t>& bits);

// Slide the 64-bit unique word over `bits`, return hits with <= max_errors.
// Correlates both polarities; also detects the bit-reversed orientation.
std::vector<SyncHit> find_sync(const std::vector<uint8_t>& bits, int max_errors);

// Pack bits[start .. start+nbits) MSB-first into bytes (as s2p's bit packer).
std::vector<uint8_t> pack_bits_msb(const std::vector<uint8_t>& bits, size_t start,
                                   size_t nbits);

// Decoded ITC L1 header (24 bits, after the 64-bit unique word).
struct ItcHeader {
    int length = 0;  // bits 23..15
    int mode = 0;    // bits 14..11
    int type = 0;    // bits 10..3
    bool valid = false;
};

// Reconstructs s2p's header FEC (FUN_1004017e0): a K=7 rate-1/2 convolutional
// decoder over 6 coded bytes -> 24 bits, then parses length/mode/type.
// `in6` is the 6 packed header bytes; returns false if the header check fails.
bool decode_header(const uint8_t in6[6], ItcHeader& out);

// A fully FEC-decoded ITC packet (unwrapped from the RS/CRC layer).
struct ItcPacket {
    int type = 0;
    std::vector<uint8_t> payload;  // the "I<len><type><0><payload>" body
    bool valid = false;
};

// Decodes a packed bit-buffer (bytes: 8 = unique word, 6 = header, rest = data)
// through s2p's FUN_1004024b0: header Viterbi, block de-interleave, RS(16,12)
// per 16-byte block, CRC-16/ARC, then strips the length/CRC trailer.
bool decode_packet(const std::vector<uint8_t>& packed, ItcPacket& out);

}  // namespace openitc
