# ITC reverse-engineering notes

Findings from the reference `s2p.exe` (PE32+, x86-64, image base `0x100400000`)
obtained with Ghidra + GhidraMCP, and `itcmon.pyc` (PyInstaller, Python 3.13).

## Band / channels

`frequency = 217587500 + channel * 25000` Hz.

- Channel **102** (220.1375 MHz) = national/common channel. Bases **beacon** here.
- Channels are right-sized at 25 kHz; a base announces its local channel + a
  6-channel list in its beacon.

## MCR -> S2P transport

MCR captures the SDR, decimates (e.g. 960 kSa/s -> 192 kSa/s), channelizes,
and sends baseband I/Q to one `s2p` process per channel/rate over **UDP**
(ports `171xx` = FR, `191xx` = HR; ch102 FR = 17102, ch102 HR = 19102).

Each UDP datagram: `[flag:1][0:1][seq:u16 BE][complex float32 I/Q...]`.
- 4-byte header. `flag` bit0 = last fragment of a burst. `seq` = 0 begins a new
  burst, then increments (1, 2, ...) across fragments.
- S2P's receive buffer is **4096 bytes**, so at most **511 complexes**
  (4 + 511*8 = 4092 bytes) per datagram; larger datagrams are truncated and
  dropped with "not a multiple of 8-byte I/Q complexes".
- Samples are **complex float32** with a very small amplitude (~0.003–0.03).
  **Amplitude matters:** feeding values ~100x larger breaks the decoder
  (empirically verified). MCR is **squelch-gated**, so it only forwards RF
  bursts, not continuous I/Q.

S2P publishes decoded packets over **ZeroMQ PUB** on TCP `181xx` (FR) /
`201xx` (HR); ch102 FR = 18102, ch102 HR = 20102.

## S2P pipeline (`main` = `FUN_100404cf0`)

- Args: `-r <udp port>` (default 17000), `-D` debug, `-m HR|FR`.
- `init_rs_char(8, 0x11d, 0, 1, nroots=4, pad=239)`  -> **RS(16,12)** over GF(2^8).
- Mode params: **SPS = 12** samples/symbol.
  - HR: input 96000 Hz  -> 8000 sym/s
  - FR: input 192000 Hz -> 16000 sym/s
- Sync-word table built by `FUN_100401b60` from the string
  `"f337eeb637660672"` -> 64 bits (1 bit per byte), stored at `0x10044b340`.
- Convolutional code tables built by `FUN_100401750` from seeds
  `0x5b` (0o133) and `0x79` (0o171) -> **K=7, rate 1/2** Viterbi.

## Demod (`FUN_100402b70`)

**The modulation is pi/4-DQPSK** (differential), NOT GMSK. The symbol decision
regions are centred on +/-pi/4 and +/-3pi/4 (constants at `0x100407630..`), i.e.
each symbol changes the carrier phase by +/-45 or +/-135 degrees and carries two
bits (`uVar18` = b0, `uVar19` = b1).

Native implementation (`src/itc.cpp`):
- Differential phase between consecutive symbol samples (`atan2`), unwrapped.
- 4 regions -> bits (**resolved against s2p's live bit buffer**):
  `b0 = (ph > 0)` (positive phase -> 1), `b1 = |ph| > pi/2`.
  So `(0,pi/2]->10`, `(pi/2,pi]->11`, `(-pi/2,0]->00`, `(-pi,pi/2]->01`.
  (Earlier code had the **b0 polarity inverted** — that was the whole M3 bug.)
- Bitstream is the two bits interleaved (b0, b1) per symbol; pack MSB-first.
- SPS = 12 for both rates (96 kHz/8 ksym and 192 kHz/16 ksym).
- 64-bit unique word correlated directly -> **0-bit errors** at the correct
  symbol timing, and the packed UW+header now matches s2p byte-for-byte.

### M3 header FEC — RESOLVED

Verified by dumping s2p's own intermediate state with gdb while replaying a
captured burst (`cap_19102`, known base beacon), then reproducing it exactly:

- s2p's header Viterbi (`FUN_1004017e0`) output tables were dumped at runtime
  (`0x10040b000` / `0x10040b080`). They are K=7 rate-1/2 with taps
  **`TA[i] = parity(i & 0x6D)`**, **`TB[i] = parity(i & 0x4F)`** — NOT the
  0o133/0o171 the seeds suggested.
- Decode: 48 coded bits (6 bytes) = 24 info bits. Per step
  `idx=(state<<1)|u`, `next=idx&0x3f`, branch metric = `(TA[idx]^r0)+(TB[idx]^r1)`
  where `(r0,r1)` is the MSB-first bit pair. Init state 0 (flat also converges for
  clean input). Traceback with **predecessor pointers** (not `u`, the state MSB is
  lost). Ground-truth pairs (in -> out):
  `b0e5c2330f6c -> 0e8810`, `b0e5ca332f7c -> 0e8810`.
- Header field layout (`FUN_1004023c0`, big-endian 24-bit `V`):
  `length = V>>15` (9b), `mode = (V>>11)&0xf` (4b), `type = (V>>3)&0xff` (8b),
  `check = V&7` (3b). The 3-bit check LFSR-reduces bits 23..3 with poly `0xb`
  and requires a zero remainder. Valid iff `mode==1`, `type!=0`, `type>>4<=7`
  and `type>>4 ∉ {1,2,4,6}`, `length<=0xC0`.
- For the beacon: `V=0x0E8810` -> `len=29, mode=1, type=0x02`.

`openitc demod -f <baseband.cf32> [-m HR|FR]` now decodes the header natively
(12/13 hits -> `len=29 mode=1 type=02 OK`), matching s2p.

## Data FEC (`FUN_1004024b0`) — RESOLVED

Packed buffer layout: bytes 0..7 = 64-bit unique word, 8..13 = Viterbi-coded
header, 14.. = interleaved RS data. Let `L = header.length`, `B = ceil(L/12)`,
`ebp = L + 4*B` (total interleaved data bytes).

1. **Block de-interleave** into a `B*16` buffer `c78`: enumerate `edi = 0..15`
   (outer) x `b = 0..B-1` (inner); position `p = edi + 16*b`; if `p < ebp`,
   `c78[p] = data[k++]` (k counts sequentially).
2. **4-byte rotation**: for `j = 0..3`, `c78[B*16-1-j] = c78[ebp-1-j]` then zero
   the source — relocates the last block's RS parity to the block end.
3. **RS(16,12)** per 16-byte block `c78[j*16 .. j*16+16)`, using
   `init_rs_char(8, 0x11d, fcr=0, prim=1, nroots=4, pad=239)` (`FUN_100403550`).
   Corrects up to 2 symbol errors; s2p retries with 2 then 4 erasures from the
   soft-value array `0x10044a978` (the demod's per-bit confidence).
4. Concatenate each corrected block's first 12 bytes -> `a78` (length `12*B`).
5. **CRC-16/ARC** (reflected poly `0xA001`, init 0) over `a78[0..L)`; must be 0.
6. Payload: `L-2` bytes (type <= 5) or `L-6` (type > 5; strips a 4-byte trailer);
   type `0x70` takes a separate unicast path.

Output envelope to ITCMON: `0x49 'I' | len:u16 BE | type:u8 | 0x00 | payload`.

`openitc demod -f <baseband.cf32>` now runs the whole chain natively and prints
e.g. `BEACON id=38282d lat=41.831817 lon=-80.768800 ch=142 ...`. Verified against
both the golden s2p packet bytes and the s2p RS blocks (byte-for-byte).

## Output framing (S2P -> ITCMON)

`0x49 ('I') | length:u16 BE | type:u8 | 0x00 | payload[...]`

## Packet: base beacon (type 0x02)

From `itcmon.pyc::ITC_MON_GUI.decode_base_beacon` (payload after the 5-byte
envelope; requires len >= 21):

| offset | field | notes |
|---|---|---|
| 0 | flags | u8 |
| 1..3 | radio_id | u24 BE |
| 4..7 | lat_raw | i32 BE, degrees = lat_raw / 600000.0 |
| 8..11 | lon_raw | i32 BE, degrees = lon_raw / 600000.0 |
| 12 | ch | local channel (80..220) |
| 13 | rate | u8 |
| 14 | util | u8 |
| 15 | flag1 | u8 |
| 16..21 | ch_list | 6 x u8 (each 80..220) |

Validity requires `21 <= lat <= 70` and `-170 <= lon <= -50`.

### Observed beacons (2026-10, national channel 102 HR)

| radio_id | lat | lon | local ch | rate | util | ch_list |
|---|---|---|---|---|---|---|
| 0x38282D | 41.831817 | -80.768800 | 142 | 1 | 18 | 167,166,165,153,142,127 |
| 0x38282E | 41.946383 | -80.568917 | 166 | 1 | 11 | 166,153,142,141,127,126 |

## Validation

- Replaying MCR's captured UDP datagrams into a standalone `s2p -r <port> -m HR` decodes packets.
- OpenITC's own native capture + NCO channelizer + FIR decimation (scaled to ~0.001)
  produces baseband that the reference `s2p` decodes (55 packets in 20 s on ch142 FR).
- ch102 is the sparse national/beacon channel; ch142 is the base's busy local channel.

