# How the ITC Radio System Works

A plain-language explanation of the **ITC** (Interoperable Train Control) radio
system used for Positive Train Control (PTC), written for anyone curious how the
220 MHz train network actually behaves on the air.

This is a best-effort synthesis of two things:

1. The public concept-of-operations report **PTC Communications: Cybersecurity
   Technology Review and Concept of Operations** (Meteorcomm for the FRA,
   *DOT/FRA/ORD-23/39*, December 2023) — this is where most of the "what and
   why" comes from.
2. What we have actually observed and verified on the air while building
   **OpenITC** (the decoder in this repository), cross-checked against the
   reference ITCMon tool.

It is an **independent, unofficial** description. Where something is inferred
from the air interface rather than documented, it is marked *(observed)*.

---

## 1. The big picture

Positive Train Control is the safety system that can stop a train before it
runs a switch, enters a work zone, or exceeds authority. It needs three things
to work: **where the train is**, **where it is allowed to go**, and **what the
railroad's hardware is doing right now**. ITC is the set of standards and
equipment that carry that information around.

ITC is really **three layers stacked on top of each other**:

```
   applications         train management, wayside logic, back-office servers
        |  (messages)   EMP / Class D  messages
   messaging (ITCM)     brokers, routers, ELM/CM gateways  (AMQP/Qpid)
        |  (frames)     ELP / ITCnet  radio link
   radio transport      the 220 MHz radios  (base, locomotive, wayside)
        |  (RF)
   220 MHz spectrum
```

OpenITC lives at the **bottom** of that stack: it listens to the radio and
decodes the link-layer frames. The ConOps report mostly describes the layers
above (and the security on top of everything).

---

## 2. The players

- **Base radios** live along the railroad and connect back to the railroad's
  **Back Office (BO)** servers. They are the local hub. Railroad engineers often
  call one a "base" or "BOS connection."
- **Locomotive radios** ride on the trains. Their computer is the **TMC** (Train
  Management Computer), also called the On-Board Computer.
- **Wayside radios** sit next to signals and switches, wired to a **WIU**
  (Wayside Interface Unit). The WIU reads the state of the hardware (which way a
  switch is lined, what a signal is showing) and broadcasts it.
- The **Back Office** hosts the application servers, the messaging system, and
  the key/security services.

Locomotives and waysides are collectively called **remotes**.

---

## 3. The radio band

ITC uses channels in the **220 MHz** band, spaced **25 kHz** apart. In OpenITC we
model the frequency of a channel as *(observed)*:

```
frequency (Hz) = 217,587,500 + channel × 25,000
```

Channel numbers run roughly **101–176**; the working set we scan is 27 channels
spanning about **1.875 MHz**. **Channel 102 (220.1375 MHz) is the national /
common channel** — bases announce themselves there and it is where a train
looks when it is new to an area. Each base also tells the world its own **local
channel** in its beacon, so a train can move to it.

A single 25 kHz channel carries one of two data rates *(observed, and consistent
with the report)*:

| mode | name in docs | samples/symbol | symbols/s | bits/s |
|---|---|---|---|---|
| **HR** | half-rate | 12 | 8,000 | **16 kbps** |
| **FR** | full-rate | 12 | 16,000 | **32 kbps** |

The report confirms the common channel and the wayside frames use **half-rate,
16 kbps**. A next generation of radios is planned to support full-rate, adaptive
coding/modulation, adaptive power, and wider channels — because the 220 MHz band
is already getting crowded in places like Chicago.

---

## 4. How one radio burst is built

This is the part OpenITC reverse-engineered and reproduces exactly. Every
transmission is a short **burst** built like this *(observed)*:

```
[ 64-bit unique word ][ coded header ][ interleaved data payload ][ CRC ]
```

- **Modulation:** π/4-DQPSK (differential). Each symbol shifts the carrier phase
  by ±45° or ±135° and carries **two bits**. (The report just calls it
  "differential quadrature phase shift keying"; we confirmed the π/4 variant.)
- **Unique word:** a fixed 64-bit sync pattern (the ASCII string
  `f337eeb637660672`). The receiver slides this over the incoming bits to find
  the start of a burst. On a clean signal it matches with zero errors.
- **Header:** 24 useful bits — **length**, a **mode** field (always 1), a
  **type** byte, and a small 3-bit check. It is protected by a **K=7, rate-1/2
  convolutional code** decoded with a Viterbi decoder (48 coded bits → 24).
- **Data:** the payload is chopped into 12-byte groups, each protected by a
  **Reed–Solomon (16,12)** code over GF(2⁸) (adds 4 parity bytes per group),
  then the groups are **interleaved** (shuffled across the burst so a burst of
  noise damages different codewords instead of one), and the whole thing ends
  with a **CRC-16** checksum.

So even a noisy burst can usually be reconstructed: the sync finds it, the
Viterbi fixes the header, Reed–Solomon fixes the data, and the CRC says "yes,
this is correct."

Rates and structure line up with the report's numbers: **30-millisecond
slots** at **16 kbps** hold "slightly less than 500 bytes", and a typical wayside
message is **under 50 bytes**.

---

## 5. The air-link structures

The report describes three ways the 220 MHz channel is shared:

- **The common channel** — a single 25 kHz channel used for the peer-to-peer
  housekeeping messages (beacon-on, "get WIU status", and its response). It uses
  **CSMA/CA** (listen before you talk) so multiple transmitters don't collide.
- **F-frame slots** — many **30 ms slots across multiple channels**, set aside
  *only* for wayside status messages, scheduled by **slot and frequency
  planning** so transmitters don't collide.
- **D-frame** — the part of the radio protocol that carries **"Dynamic TDMA"**
  traffic (the non-peer-to-peer, base-to-remote signalling and data). The report
  names it directly: *"all Dynamic Time Division Multiple Access traffic
  transferred in the D-Frame of ITCnet."*

The base (or the radio planning) assigns transmitting slots to remotes, which is
what the QSTAT / CNTL / ACK messages in OpenITC's packet log are doing. This is
a **TDMA** system: the base hands out a repeating structure of slots and remotes
fill their turn.

---

## 6. The link-layer packet types

OpenITC labels each decoded frame with a **type** byte. These are radio
link-layer types (not to be confused with the *application* message IDs like
"5100" in the next section!). What we know:

| type | name | what it appears to be |
|---|---|---|
| `0x01` | QSTAT | a remote asking the base for extra TDMA slots |
| `0x02` | Beacon | base broadcasting its GPS position + local channel list |
| `0x03` | ACK | acknowledgement |
| `0x04` | ACQ | a locomotive/mobile acquiring a new base |
| `0x05` | CNTL | base control, sent at the start of a superframe (slot allocation) |
| `0x06` | TIME | the base's UTC date/time broadcast |
| `0x07` | T07 | **unknown** — even the reference tool's author lists it as unknown |
| `0x33` | WIU | wayside status (periodic broadcast) |
| `0x34` | WIU | wayside status (sent in response to a request) |
| `0x70` | All Others | mostly unicast; EMP / ATCS-over-ITC traffic |

### Base beacons (`0x02`)

A beacon is how a base introduces itself. It carries a 24-bit radio ID, a signed
**latitude/longitude** (degrees = raw ÷ 600,000), the base's **local channel**,
a rate/utilization figure, and a **six-channel list** of nearby channels a train
should consider. These are the messages OpenITC plots on its map:

```
type=02 Beacon  id=38282d lat=41.831817 lon=-80.768800 ch=142 util=17
                list=167,166,165,153,142,127
```

### The clock (`0x06`)

Bases broadcast **UTC** over the air. OpenITC reads it, displays it next to your
local clock, and shows the **drift** (how far the base's clock is from your PC).

### Base control (`0x05`)

The base sends CNTL packets to organize the channel — a transmitter ID, a
sequence number, and a small data field that encodes the current slot plan.

---

## 7. The messaging layer above the radio

The radio is just the transport. The actual railroad logic rides on **ITCM**
(Interoperable Train Control Messaging), which the report describes as a
messaging system built on Red Hat MRG and **AMQP/Qpid** brokers. The important
pieces:

- **Applications** connect to an **Application Gateway (AG)** either with a
  **Class D** connection carrying an **EMP** message directly, or with an
  **AMQP** connection carrying the EMP message inside the AMQP body.
- **EMP** = Edge Message Protocol (AAR standard **S-9354**); **Class D** =
  S-9356. Every EMP message has a header, a body, and a footer with an integrity
  value.
- A **Message Router (MR)** picks which transport to use (radio, IP, cellular,
  satellite, Wi-Fi).
- **External Link Managers (ELM)** in the Back Office and **Connection Managers
  (CM)** on remotes move messages to and from the radios using the **External
  Link Protocol (ELP)**. The Back Office counterpart on IP is the **IP-ELM**, and
  the protocol between a remote CM and an IP-ELM is **RBX** (Remote Back Office
  Exchange).
- **ITCSM** (Systems Management) is a separate secure layer for status,
  configuration, software loads, and security artifacts, using a **SMG**
  (gateway) and **SMAs** (agents).

So the stack on the air is roughly:

```
app message (EMP, id like 5100)  ->  ITCM routing  ->  ELP  ->  ITCnet frame  ->  RF
```

The **EMP header** has a protocol version (8 bits), a **message type** (16 bits,
the application message ID), and a **message version** (8 bits). It also has
flags that matter for security (see §10): an **encryption flag**, and a
**data-integrity flag** (0 = none, 1 = CRC-32, 2 = HMAC, 3 = reserved) whose
value is appended in the footer.

**Documented wayside-related message IDs** (EMP application layer):

| id | message |
|---|---|
| 5200 | Beacon On |
| 5100 | WIU Status Message |
| 5201 | Get WIU Status |
| 5101 | Get WIU Status Response |

Note the two different "type" systems: OpenITC's `0x33`/`0x34` are the radio
link types that carry wayside status; `5100`/`5101` are the EMP application IDs
for the same information one layer up.

---

## 8. Wayside status in depth

Wayside status is the workhorse of the whole system.

- A **WIU** watches signals and switches and reports their state. That report is
  a **WSM** (Wayside Status Message).
- WSMs are the **majority of all traffic** — each WIU beacons at least **once
  every 4 seconds**, and broadcasts are duplicated on purpose to improve the
  chance a passing train hears them.
- In a dense area the on-board system can be handling around **120 WSMs per
  second**; the Wayside Status Repeater Service in the Back Office handles
  thousands per second.
- WSMs are **peer-to-peer**: a wayside can talk straight to a nearby locomotive
  without going through the base — and may be picked up by *any* train in range
  (they are broadcasts, not addressed to one receiver).
- Their usefulness is **short-lived**: a stale wayside reading must be ignored,
  so WSMs carry a short time-to-live. This makes the wayside-to-train path the
  most delay-sensitive link in PTC.

### What's inside a WSM

The report's packet capture shows the body of a wayside message containing:

- the **WIU identifier** (its "Advanced Train Control System ID") — sent **in
  the clear**, because it is needed for filtering;
- a **device status array** — the actual signal/switch states;
- a **CRC**;
- a **vital data integrity value** — an **HMAC** signature.

OpenITC decodes the WIU ID and does a best-effort read of the indicator bits.
The exact per-WIU layout (how many switches and signals, and what each signal
value is *named*) is configured per device by the railroad, so a general decoder
can only show the raw bits plus a "typical 1 switch + 3 signals" reading.

### Why the WIU ID stays in the clear

Two capacity-saving features depend on reading it without decrypting anything:
base-radio **whitelisting**, and **WSM filtering** in the onboard messaging
system. Both use the WIU ID to decide which wayside messages a given train
actually needs, so the ID must always be readable.

---

## 9. Beacons, time, and the TDMA heartbeat

Putting the radio observations and the report together, a base runs a repeating
cycle:

1. It **beacons** (type `0x02`) on the common channel so trains can find it.
2. It broadcasts **CNTL** (`0x05`) at the start of each superframe, telling
   remotes which TDMA slots are theirs.
3. It broadcasts **TIME** (`0x06`) so everyone shares a UTC clock.
4. Remotes ask for slots with **QSTAT** (`0x01`), the base acknowledges
   (**ACK** `0x03`), and locomotives entering the base's territory announce
   themselves with **ACQ** (`0x04`).
5. Waysides keep streaming **status** (`0x33`/`0x34`) in their scheduled
   F-frame slots.

OpenITC's `monitor` shows exactly this rhythm when you watch a busy local
channel: a steady tick of CNTL, a WIU report every few seconds, occasional TIME
and beacon.

---

## 10. Security

This is what the ConOps report is really about, and it shapes the design.

- The railroad data classification standard is **AAR S-9010**. The report's
  conclusion: **most PTC messages already meet it**, and the only message the
  ITC committee has flagged as needing confidentiality is **crew login**.
  Wayside status was judged *not* to need encryption, because signal/switch
  states are physically observable anyway.
- Today's protection is **integrity and authentication**, not secrecy. Wayside
  status is signed with an **HMAC** using a shared symmetric secret, the
  **Wayside Operational Private Key (OPK)** (about **20 bytes**). Locomotives
  carry copies of the OPKs for the subdivisions they run, distributed inside the
  **subdiv files** (the onboard track map) and via a **Key Exchange Service**.
- The OPK **is the foundation of PTC security**, and its weakness is that the
  *same long-lived key is shared with every locomotive* that might need that
  wayside. If it leaks, an attacker can forge status messages.
- **OpenITC can receive the HMAC but cannot verify it** — we do not have the
  OPKs — so we show message contents but cannot prove authenticity.

### The proposed future

The report evaluates ways to add **confidentiality** without breaking anything,
and recommends:

- **NIST lightweight authenticated encryption** (AEAD) algorithms for
  non-wayside application messages. The four recommended finalists are
  **Ascon, Spark, TinyJAMBU, and Xoodyak**.
- A **hybrid** approach for keys: use a lightweight **asymmetric** algorithm
  (**ELLI**, ISO/IEC 29192-4) to securely generate and hand out short-lived
  symmetric keys, then keep using fast symmetric HMACs/encryption on the bulk
  traffic. This keeps the small, frequent messages cheap while removing the
  long-lived shared-key problem.
- **Radio D-frame encryption** is possible but expensive: it would need ITCnet
  and ELP protocol changes plus certificate exchange at connection setup, so it
  would be done **inside the messaging system, not the radios**.

None of this changes the on-air format OpenITC decodes today; it is the
direction of travel.

---

## 11. Addresses and identity

- Radios are identified by a **24-bit radio ID** (you'll see these as the `TID`
  / `id` values in OpenITC's output, e.g. `38282d`).
- A **WIU** additionally has its **ATCS ID**, used for filtering (see §8).
- The security system ties keys to identities: the report stresses that
  "confidentiality without identity assurance is not valuable."

---

## 12. What we know, and what we don't

Honest boundaries, so nobody over-trusts the decoder:

- **Unknown:** link type `0x07` ("T07") appears on the air but is unknown even
  to the author of the reference tool.
- **Best-effort:** wayside indicator decoding (per-device layouts are
  configured, not standardized).
- **Cannot verify:** HMAC-authenticated payloads, because we don't hold the OPKs.
- **Inferred:** the exact mapping between some on-air link types and the ITCM
  messages above them.
- **Secure assumptions:** everything in §4–§6 has been reproduced byte-for-byte
  against the reference decoder on real captures, so the physical and
  link layers are on solid ground.

---

## 13. Glossary

| term | meaning |
|---|---|
| **PTC** | Positive Train Control — the safety system |
| **ITC** | Interoperable Train Control — the standards/equipment for PTC |
| **ITCM** | ITC Messaging — the AMQP/Qpid message layer |
| **ITCSM** | ITC Systems Management — status/config/software/security |
| **I-ETMS** | Interoperable Electronic Train Management System |
| **EMP** | Edge Message Protocol (AAR S-9354) |
| **Class D** | AAR S-9356 message carrier |
| **ELP** | External Link Protocol — CM/ELM ↔ radio |
| **RBX** | Remote Back Office Exchange — CM ↔ IP-ELM |
| **ELM / IP-ELM** | External Link Manager (radio / IP) |
| **CM** | Connection Manager (on a remote) |
| **MR** | Message Router |
| **AG** | Application Gateway |
| **BO** | Back Office |
| **TMC / OBC** | Train Management Computer / On-Board Computer |
| **WIU** | Wayside Interface Unit |
| **WSM** | Wayside Status Message |
| **WSRS** | Wayside Status Repeater Service |
| **OPK** | Operational Private Key (symmetric secret) |
| **HMAC** | Hash-based Message Authentication Code |
| **KES** | Key Exchange Service |
| **AEAD** | Authenticated Encryption with Associated Data |
| **ELLI** | Elliptic Light — lightweight asymmetric algorithm |
| **TDMA** | Time Division Multiple Access |
| **CSMA/CA** | Carrier Sense Multiple Access / Collision Avoidance |
| **HR / FR** | half-rate (16 kbps) / full-rate (32 kbps) |
| **DUA** | Dense Urban Area |

---

## 14. Sources

- Meteorcomm, LLC for the Federal Railroad Administration, *PTC Communications:
  Cybersecurity Technology Review and Concept of Operations*,
  **DOT/FRA/ORD-23/39**, December 2023.
- The ITCMon author's protocol notes (`github.com/katsojuna/itcmon`).
- Over-the-air observations and the clean-room decoder in this repository
  (`src/itc.cpp`, `src/parse.cpp`, `docs/reverse-engineering.md`).

*Independent, unofficial project. Not affiliated with or endorsed by any
railroad, ITC, Meteorcomm, or vendor.*
