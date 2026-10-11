# OpenITC

An open-source, console (no-GUI) RTL-SDR receiver for **ITC** — the 220 MHz
Positive Train Control radio system used by North American railroads.

OpenITC captures I/Q directly from an RTL-SDR (with **bias tee** support for
inline LNAs), tunes to ITC channels, and **fully decodes the ITC air interface**
in-process: demodulation, sync, forward error correction, and link-layer packets
such as **base beacons (type 0x02)**. No external binaries are required.

This is a clean-room reimplementation. It is an independent, unofficial project
and is not affiliated with or endorsed by any railroad, ITC, or vendor.

## Features

- Native RTL-SDR capture: tuner, gain, ppm, and bias tee.
- Channelizer: mix a 25 kHz ITC channel to baseband and decimate (HR 96 kHz, FR 192 kHz).
- π/4-DQPSK differential demodulation with 64-bit unique-word sync.
- K=7 rate-1/2 convolutional (Viterbi) header FEC.
- Block de-interleave, RS(16,12) Reed–Solomon over GF(2^8), CRC-16/ARC.
- Packet decode, including full **0x02 base-beacon** fields.
- `monitor`: wideband scan of the whole band, listing packets live until stopped.

## Status

- [x] M0 — native RTL-SDR capture with bias tee, tuner/gain/ppm
- [x] M1 — decimation + channelizer (960 kSa/s → 192 kSa/s baseband)
- [x] M2 — π/4-DQPSK demodulator + 64-bit sync
- [x] M3 — K=7 Viterbi header FEC
- [x] M4 — RS(16,12) + CRC-16/ARC + header/packet decode
- [x] M5 — 0x02 base beacon decode & display
- [x] M6 — all-channel `monitor`

## ITC channel map

`frequency = 217587500 + channel * 25000` (Hz). Channel 102 (220.1375 MHz) is
the national/common channel; bases beacon there announcing their local channel.
The 27 on-air channels span 220.1125–221.9875 MHz (1.875 MHz).

## Protocol summary

- Modulation: **π/4-DQPSK**, 12 samples/symbol (HR 8 ksym/s, FR 16 ksym/s).
- 64-bit unique word: ASCII `f337eeb637660672` (hex, MSB-first).
- Header: 48 coded bits → K=7 rate-1/2 Viterbi → 24 bits
  (`length` 9b, `mode` 4b, `type` 8b, 3b LFSR check, poly `0xb`).
- Data: block de-interleave → RS(16,12) (GF(2^8), poly `0x11d`, fcr 0, 4 roots)
  → CRC-16/ARC (`0xA001`) → payload.

Full details, including the reverse-engineering notes, are in
[`docs/reverse-engineering.md`](docs/reverse-engineering.md).

## Build (MSYS2 MinGW-w64)

```sh
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake \
    mingw-w64-x86_64-ninja mingw-w64-x86_64-rtl-sdr mingw-w64-x86_64-zeromq
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The runtime DLLs (`librtlsdr`, `libusb-1.0`, `libzmq`, `libsodium`, GNU runtime)
must sit next to `openitc.exe` or on `PATH`. On Windows the RTL-SDR dongle also
needs a WinUSB driver (e.g. via Zadig).

## Usage

```sh
# List RTL devices
./build/openitc -l

# Monitor the whole PTC band: channelize + decode every channel, list packets
# live until Ctrl-C.
./build/openitc monitor -g 40.2 -b 1

# Decode one channel live (FR or HR):
./build/openitc demod -c 142 -m FR -s 960000 -g 40.2 -b 1 -n 3840000

# Decode an offline baseband capture (interleaved complex float32):
./build/openitc demod -f ch142.cf32 -m HR -E 4
```

`monitor` prints, as packets arrive:

```
[15:09:15] ch142 FR  type=05 5       len=12  38282dbd00004040c0000000
[15:09:17] ch142 FR  type=34 34      len=11  a5e904ce44820ac7c58440
[15:09:32] ch142 FR  type=02 beacon  len=27  id=38282d lat=41.831817 lon=-80.768800 ch=142 rate=1 util=17 list=167,166,165,153,142,127
```

`monitor` options: `-t <ms>` chunk size (default 250), `-C <s>` per-packet repeat
cooldown (default 8), `-E <n>` max sync errors (default 8), plus `-d/-s/-g/-e/-b`.

### Capture / channelize

```sh
# Capture raw I/Q on channel 102 with bias tee enabled
./build/openitc -c 102 -s 960000 -g 40.2 -b 1 -n 960000 -o ch102.iq

# Channelize a channel to baseband (complex float32)
./build/openitc -c 142 -s 960000 -R 192000 -n 19200000 -o ch142.cf32
```

> `openitc decode` is a legacy path that shells out to a reference binary; the
> native `demod`/`monitor` commands above are self-contained.

## License

GPL-2.0-or-later (see [`LICENSE`](LICENSE)).
