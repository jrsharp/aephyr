# Æther Protocol Specification

## Version Information
**Version:** 1.3
**Last Updated:** June 2026  
**Status:** Complete Implementation Specification  

**Changes in this version:**
- Added §4.6 Reliable Delivery (DELIV_RELIABLE): the per-packet reliable
  datagram algorithm — soft per-peer state, the Acknowledgment (0x01) wire
  format, delayed/coalesced/piggybacked ACK policy, selective
  retransmission, per-PHY parameters. The fields existed since 1.2; 1.3
  specifies the delivery algorithm that uses them.
- Added §4.6.10 PHY applicability: the PHY/MAC reliability split (PHY coding
  gain vs MAC HARQ/ARQ), why §4.6 is the sole ARQ on the HeyMac-on-PHY DECT
  NR+ stack (no DECT MAC, no HARQ), and the "same algorithm, different job"
  relationship across the LoRa and DECT NR+ PHYs.

**Changes in version 1.2:**
- Corrected HeyMac header structure and field sizes
- Fixed ÆtherStyx message format and type numbering
- Added comprehensive field definitions and bit layouts
- Go notes

---


## 1. Protocol Stack Overview

The Æther protocol consists of four layers:

1. **LoRa PHY Layer** - SX1262 radio configuration and operation
2. **HeyMac Layer** - Physical frame format and addressing
3. **Æther Mesh Layer** - Network routing and forwarding  
4. **ÆtherStyx Layer** - Application protocol (9P-inspired)

---

## 2. LoRa PHY Layer (SX1262 Radio)

### 2.1 Default Configuration

**Go Implementation:**
```go
type LoRaPHYConfig struct {
    Frequency       uint32  // 915000000 Hz (915 MHz)
    SpreadingFactor uint8   // 7 (SF7)
    Bandwidth       uint8   // 0 (125 kHz)
    CodingRate      uint8   // 1 (4/5 coding rate)
    TxPower         int8    // 14 dBm
    PreambleLength  uint8   // 8 symbols
    SyncWord        uint8   // 0x12
    CRCEnabled      bool    // true
    ImplicitHeader  bool    // false (explicit header mode)
    InvertIQ        bool    // false
}
```

**C Implementation:**
```c
typedef struct LoRaPHYConfig {
    ulong frequency;        /* 915000000 Hz (915 MHz) */
    uchar spreading_factor; /* 7 (SF7) */
    uchar bandwidth;        /* 0 (125 kHz) */
    uchar coding_rate;      /* 1 (4/5 coding rate) */
    char tx_power;          /* 14 dBm */
    uchar preamble_length;  /* 8 symbols */
    uchar sync_word;        /* 0x12 */
    uchar crc_enabled;      /* 1 = true */
    uchar implicit_header;  /* 0 = false (explicit header mode) */
    uchar invert_iq;        /* 0 = false */
} LoRaPHYConfig;

/* Default configuration values */
#define LORA_DEFAULT_FREQ       915000000
#define LORA_DEFAULT_SF         7
#define LORA_DEFAULT_BW         0
#define LORA_DEFAULT_CR         1
#define LORA_DEFAULT_POWER      14
#define LORA_DEFAULT_PREAMBLE   8
#define LORA_DEFAULT_SYNCWORD   0x12
```

### 2.2 Spreading Factor Options

| SF  | Sensitivity | Approx Range | Data Rate |
|-----|-------------|--------------|-----------|
| SF7 | -123 dBm    | 2 km         | Highest   |
| SF8 | -126 dBm    | 4 km         | ↑         |
| SF9 | -129 dBm    | 6 km         | ↑         |
| SF10| -132 dBm    | 8 km         | ↑         |
| SF11| -134.5 dBm  | 10 km        | ↑         |
| SF12| -137 dBm    | 12+ km       | Lowest    |

### 2.3 Bandwidth Options

| Index | Bandwidth | Usage           |
|-------|-----------|-----------------|
| 0     | 125 kHz   | Default, balanced |
| 1     | 250 kHz   | Higher data rate |
| 2     | 500 kHz   | Highest data rate |

### 2.4 Coding Rate Options

| Index | Coding Rate | Overhead | Error Correction |
|-------|-------------|----------|------------------|
| 1     | 4/5         | 25%      | Lowest           |
| 2     | 4/6         | 50%      | ↑                |
| 3     | 4/7         | 75%      | ↑                |
| 4     | 4/8         | 100%     | Highest          |

### 2.5 Regional Frequency Bands

| Region        | Frequency  | Default Channel |
|---------------|------------|-----------------|
| North America | 915 MHz    | 915.0 MHz       |
| Europe        | 868 MHz    | 868.1 MHz       |
| Asia          | 433 MHz    | 433.5 MHz       |
| India         | 866 MHz    | 866.5 MHz       |
| Japan/Korea   | 923 MHz    | 923.2 MHz       |

### 2.6 SX1262 Command Interface

**Go Constants:**
```go
const (
    CMD_SET_PACKET_TYPE         = 0x8A
    CMD_SET_RF_FREQUENCY        = 0x86
    CMD_SET_MODULATION_PARAMS   = 0x8B
    CMD_SET_PACKET_PARAMS       = 0x8C
    CMD_SET_TX_PARAMS           = 0x8E
    CMD_SET_RX                  = 0x82
    CMD_SET_TX                  = 0x83
    CMD_GET_PACKET_STATUS       = 0x14
    CMD_GET_RSSI_INST           = 0x15
)
```

**C Constants:**
```c
/* SX1262 Command Interface */
#define CMD_SET_PACKET_TYPE         0x8A
#define CMD_SET_RF_FREQUENCY        0x86
#define CMD_SET_MODULATION_PARAMS   0x8B
#define CMD_SET_PACKET_PARAMS       0x8C
#define CMD_SET_TX_PARAMS           0x8E
#define CMD_SET_RX                  0x82
#define CMD_SET_TX                  0x83
#define CMD_GET_PACKET_STATUS       0x14
#define CMD_GET_RSSI_INST           0x15
```

**Packet Types (Go):**
```go
const (
    PACKET_TYPE_GFSK    = 0x00
    PACKET_TYPE_LORA    = 0x01
    PACKET_TYPE_RANGING = 0x02
    PACKET_TYPE_FLRC    = 0x03
    PACKET_TYPE_BLE     = 0x04
)
```

**Packet Types (C):**
```c
/* SX1262 Packet Types */
#define PACKET_TYPE_GFSK    0x00
#define PACKET_TYPE_LORA    0x01
#define PACKET_TYPE_RANGING 0x02
#define PACKET_TYPE_FLRC    0x03
#define PACKET_TYPE_BLE     0x04
```

### 2.7 Physical Layer Constraints

- **Maximum Frame Size**: 255 bytes (LoRa limit)
- **Minimum Frame Size**: 8 bytes
- **Power Range**: 0-22 dBm (implementation uses 0-14 dBm)
- **Frequency Tolerance**: ±25 ppm
- **Channel Activity Detection**: Supported via CAD mode

---

## 3. HeyMac Layer

### 3.1 Frame Control Field (1 byte)

```
Bit 7 6 5 4 3 2 1 0
    L N D S I M P X

L = Long addressing mode (0=16-bit, 1=48-bit addresses)
N = Network ID present (1=present, 0=absent)
D = Destination address present (1=present, 0=absent)  
S = Source address present (1=present, 0=absent)
I = Information Elements present (1=present, 0=absent)
M = MAC payload present (1=present, 0=absent)
P = Pending frame (1=pending, 0=not pending)
X = Extended header (1=extended, 0=standard)
```

**Constants (Go):**
```go
const (
    HEYMAC_FCTL_L = 0x80  // Long addressing
    HEYMAC_FCTL_N = 0x40  // Network ID present
    HEYMAC_FCTL_D = 0x20  // Destination present
    HEYMAC_FCTL_S = 0x10  // Source present
    HEYMAC_FCTL_I = 0x08  // Information Elements
    HEYMAC_FCTL_M = 0x04  // MAC payload present
    HEYMAC_FCTL_P = 0x02  // Pending frame
    HEYMAC_FCTL_X = 0x01  // Extended header
)
```

**Constants (C):**
```c
/* HeyMac Frame Control Bits */
#define HEYMAC_FCTL_L   0x80  /* Long addressing */
#define HEYMAC_FCTL_N   0x40  /* Network ID present */
#define HEYMAC_FCTL_D   0x20  /* Destination present */
#define HEYMAC_FCTL_S   0x10  /* Source present */
#define HEYMAC_FCTL_I   0x08  /* Information Elements */
#define HEYMAC_FCTL_M   0x04  /* MAC payload present */
#define HEYMAC_FCTL_P   0x02  /* Pending frame */
#define HEYMAC_FCTL_X   0x01  /* Extended header */
```

### 3.2 Frame Types (4-bit field)

**Go Constants:**
```go
const (
    HEYMAC_FRM_TYPE_BEACON = 0x00
    HEYMAC_FRM_TYPE_DATA   = 0x01
    HEYMAC_FRM_TYPE_ACK    = 0x02
    HEYMAC_FRM_TYPE_MACCMD = 0x03
)
```

**C Constants:**
```c
/* HeyMac Frame Types */
#define HEYMAC_FRM_TYPE_BEACON  0x00
#define HEYMAC_FRM_TYPE_DATA    0x01
#define HEYMAC_FRM_TYPE_ACK     0x02
#define HEYMAC_FRM_TYPE_MACCMD  0x03
```

### 3.3 HeyMac Frame Structure

```
+----------+-----------+----------+----------+----------+----------+
| Version  | Frame     | Frame    | Net ID   | Dest     | Source   |
|  (4b)    | Type (4b) | Control  | (2 bytes)| Address  | Address  |
|          |           | (1 byte) |          |          |          |
+----------+-----------+----------+----------+----------+----------+
| Seq Num  | Payload                                              |
| (1 byte) | (0-220 bytes)                                        |
+----------+------------------------------------------------------+
```

**Field Details:**
- **Version/Frame Type**: Combined into 1 byte (version in upper 4 bits, type in lower 4 bits)
- **Frame Control**: 1 byte with bit flags as defined above
- **Network ID**: 2 bytes (uint16), present only if `HEYMAC_FCTL_N` is set
- **Addresses**: Variable length based on `HEYMAC_FCTL_L`, `HEYMAC_FCTL_D`, `HEYMAC_FCTL_S`
  - Short addresses: 2 bytes each
  - Long addresses: 6 bytes each (48-bit MAC)
- **Sequence Number**: 1 byte
- **Payload**: Variable length, maximum 220 bytes

### 3.4 Address Format

**Go Implementation:**
```go
type HeyMacAddr struct {
    Type int    // 0=short, 1=long
    Addr []byte // 2 bytes for short, 6 bytes for long
}
```

**C Implementation:**
```c
typedef struct HeyMacAddr {
    uchar type;     /* 0=short, 1=long */
    uchar addr[6];  /* Address bytes (2 used for short, 6 for long) */
} HeyMacAddr;
```

### 3.5 Constants

**Go:**
```go
const (
    HEYMAC_VERSION         = 0x01
    HEYMAC_MAX_FRAME_SIZE  = 255
    HEYMAC_MAX_PAYLOAD     = 220
    HEYMAC_ADDR_SHORT_LEN  = 2
    HEYMAC_ADDR_LONG_LEN   = 6
)
```

**C:**
```c
/* HeyMac Constants */
#define HEYMAC_VERSION         0x01
#define HEYMAC_MAX_FRAME_SIZE  255
#define HEYMAC_MAX_PAYLOAD     220
#define HEYMAC_ADDR_SHORT_LEN  2
#define HEYMAC_ADDR_LONG_LEN   6
```

---

## 4. Æther Mesh Layer

The Æther mesh layer provides multi-hop routing over LoRa with air-time optimization.

### 4.1 Frame Control Flags

**Go Constants:**
```go
const (
    AETHER_FCTL_MESH_NET_ID  = 0x01  // Mesh network ID present
    AETHER_FCTL_EXTENDED     = 0x02  // Extended format (reserved)
    AETHER_FCTL_COMPRESSED   = 0x04  // Compressed addresses (reserved)
    AETHER_FCTL_RESERVED     = 0xF8  // Reserved for future use
)
```

**C Constants:**
```c
/* Æther Mesh Frame Control Flags */
#define AETHER_FCTL_MESH_NET_ID  0x01  /* Mesh network ID present */
#define AETHER_FCTL_EXTENDED     0x02  /* Extended format (reserved) */
#define AETHER_FCTL_COMPRESSED   0x04  /* Compressed addresses (reserved) */
#define AETHER_FCTL_RESERVED     0xF8  /* Reserved for future use */
```

### 4.2 Mesh Header Structure (Optimized)

**Size**: 16-18 bytes (variable, optimized for air-time)

| Field | Size | Description |
|-------|------|-------------|
| flags | 1 | Frame control flags |
| type | 1 | Message type (0-15) |
| ttl | 1 | Time to live / hop count |
| src | 6 | Source MAC address |
| dst | 6 | Destination MAC address |  
| seq_no | 1 | Sequence number |
| mesh_net_id* | 2 | Mesh network ID (conditional - see below) |
| qos_security | 1 | Priority(2) \| Delivery(1) \| Security(4) \| Reserved(1) |
| length | 2 | Payload length |

**\*Conditional Field**: The `mesh_net_id` field is only transmitted when the `AETHER_FCTL_MESH_NET_ID` flag is set. This occurs when the mesh network ID differs from the HeyMac layer network ID.

**Go Structure:**
```go
type MeshHeader struct {
    Flags       uint8      // Frame control flags
    Type        uint8      // Message type (0-15)
    TTL         uint8      // Time to live / hop count
    Source      [6]uint8   // Source MAC address
    Destination [6]uint8   // Destination MAC address
    SequenceNo  uint8      // Sequence number
    MeshNetID   uint16     // Conditional: only when Flags & MESH_NET_ID_FLAG
    QoSSecurity uint8      // Priority(2) | Delivery(1) | Security(4) | Reserved(1)
    Length      uint16     // Payload length
}
```

**C Structure:**
```c
typedef struct MeshHeader {
    uchar flags;           /* Frame control flags */
    uchar type;           /* Message type */
    uchar ttl;            /* Time to live / hop count */
    uchar src[6];         /* Source address */
    uchar dst[6];         /* Destination address */
    uchar seq_no;         /* Sequence number */
    /* Conditional field - only present if AETHER_FCTL_MESH_NET_ID set: */
    ushort mesh_net_id;   /* Mesh network ID (when != MAC network ID) */
    /* Combined flags field: */
    uchar qos_security;   /* Priority(2) | Delivery(1) | Security(4) | Reserved(1) */
    ushort length;        /* Payload length */
} MeshHeader;
```

### 4.3 Air-Time Optimization

To minimize precious LoRa air-time, the protocol implements conditional network ID transmission:

- **Common case**: When mesh network ID = MAC network ID → 16-byte header (saves 2 bytes)
- **Multi-network case**: When mesh network ID ≠ MAC network ID → 18-byte header (full header)

This optimization saves 2 bytes (12.5% header reduction) in typical single-network deployments while preserving full multi-network capability for complex topologies.

### 4.4 Message Types
- **0x00**: Data packet
- **0x01**: Acknowledgment  
- **0x02**: Route request (RREQ)
- **0x03**: Route reply (RREP)
- **0x04**: Route error (RERR)
- **0x05**: Hello (neighbor discovery)
- **0x06**: Tree announcement
- **0x07**: Tree join request
- **0x08**: Tree join reply
- **0x09**: Network announcement
- **0x0A**: Time sync
- **0x0B**: Key exchange
- **0x0C**: Multicast
- **0x0D**: Emergency
- **0x0E**: Reserved
- **0x0F**: Reserved

### 4.5 Mesh Constants

**Go Constants:**
```go
// Protocol IDs
const (
    AETHER_PROTO_DATA = 0x01
    AETHER_PROTO_CTRL = 0x02
    AETHER_PROTO_STYX = 0x03
    AETHER_PROTO_SEC  = 0x04
)

// Priority Levels
const (
    PRIORITY_LOW    = 0
    PRIORITY_NORMAL = 1
    PRIORITY_HIGH   = 2
)

// Delivery Guarantees
const (
    DELIV_BESTEFFORT = 0
    DELIV_RELIABLE   = 1
)

// Security Flags
const (
    SEC_FLAG_NONE      = 0x00
    SEC_FLAG_SIGNED    = 0x01
    SEC_FLAG_ENCRYPTED = 0x02
    SEC_FLAG_CGA       = 0x04
)

// Limits
const (
    MAX_NODES   = 128
    MAX_HOPS    = 6
    MAX_PAYLOAD = 64
)
```

**C Constants:**
```c
/* Protocol IDs */
#define AETHER_PROTO_DATA   0x01
#define AETHER_PROTO_CTRL   0x02
#define AETHER_PROTO_STYX   0x03
#define AETHER_PROTO_SEC    0x04

/* Priority Levels */
#define PRIORITY_LOW        0
#define PRIORITY_NORMAL     1
#define PRIORITY_HIGH       2

/* Delivery Guarantees */
#define DELIV_BESTEFFORT    0
#define DELIV_RELIABLE      1

/* Security Flags */
#define SEC_FLAG_NONE       0x00
#define SEC_FLAG_SIGNED     0x01
#define SEC_FLAG_ENCRYPTED  0x02
#define SEC_FLAG_CGA        0x04

/* Limits */
#define MAX_NODES           128
#define MAX_HOPS            6
#define MAX_PAYLOAD         64
```

### 4.6 Reliable Delivery (DELIV_RELIABLE)

Reliable delivery is a **mesh-layer service**: an end-to-end (source↔
destination, across mesh hops) **reliable datagram** with per-packet
sequencing, explicit acknowledgment (Message Type `0x01`), and selective
retransmission. It applies per packet, selected by the `Delivery` bit of
`qos_security` (`DELIV_RELIABLE`); `DELIV_BESTEFFORT` packets are
fire-and-forget and unaffected.

It is **not** connection-oriented — there is no handshake, circuit, or
teardown. The only session concept in the stack is ÆtherStyx (§5), which
rides on top. Reliable delivery runs over **best-effort mesh forwarding**
(a single end-to-end reliability layer; lower layers are not assumed
reliable).

#### 4.6.1 Per-peer soft state

Each node keeps soft state per remote peer with which it exchanges reliable
packets. It is created lazily on the first reliable packet to/from a peer
and reclaimed after `RELIABLE_IDLE_MS` of inactivity. Because it is soft
state (not a connection), losing it simply forces re-synchronization through
the sequence/ACK rules below.

```c
struct aether_reliable_tx_slot {
    uint8_t  seq_no;        /* mesh seq_no assigned to this packet */
    uint16_t len;
    uint32_t sent_ms;       /* for RTO / RTT (Karn) */
    uint8_t  retries;
    uint8_t  in_use;
    /* payload buffered elsewhere (PSRAM ring), referenced by seq_no */
};

struct aether_reliable_peer {
    uint8_t  addr[6];
    /* TX */
    uint8_t  tx_next_seq;   /* next seq_no to assign (mod 256)         */
    struct aether_reliable_tx_slot tx_win[RELIABLE_WINDOW_MAX]; /* in-flight */
    uint32_t rto_ms;        /* current RTO (per-PHY base, backed off)  */
    /* RX */
    uint8_t  rx_expected;   /* next in-order seq_no expected (mod 256) */
    uint8_t  rx_ooo;        /* selective bitmap of out-of-order rxd    */
    uint8_t  rwnd;          /* free RX slots advertised (packets)      */
    /* ACK */
    uint8_t  ack_pending;
    uint32_t ack_due_ms;    /* coalesce/delay deadline                 */
    uint32_t idle_ms;       /* for soft-state GC                       */
};
```

#### 4.6.2 Sequence numbers

The existing 1-byte mesh `seq_no` is used, namespaced per reliable
(`src`,`dst`) flow and incremented per reliable DATA packet (mod 256).
Best-effort packets do not consume the reliable sequence space. The in-flight
window MUST be `< 64` packets so selective repeat is unambiguous in mod-256
space.

> **Note (wide windows):** high-rate PHYs that want windows ≥ 64 packets must
> widen the sequence space — carry an extended 16-bit sequence under the
> `AETHER_FCTL_EXTENDED` flag rather than overrun the 1-byte field. LoRa
> profiles (§4.6.7) stay well under 64 and use the base 1-byte field.

#### 4.6.3 Acknowledgment format (Message Type `0x01`)

A standalone ACK is a mesh packet with `type = AETHER_MSG_ACK` whose payload
is the 3-byte block below. The same block is appended to a DATA packet when
the ACK is piggybacked (§4.6.4), flagged by `AETHER_FCTL_EXTENDED`.

```c
struct aether_ack {
    uint8_t cum_ack;   /* highest seq_no received in order; all <= it delivered */
    uint8_t rwnd;      /* receiver free buffer slots, in packets (flow control)  */
    uint8_t sack;      /* selective bitmap: bit i set => seq (cum_ack+2+i) rx'd  */
                       /* out of order. 0 => no holes. (1 byte covers 8 ahead.)  */
};
```

```go
type AetherAck struct {
    CumAck uint8
    Rwnd   uint8
    Sack   uint8
}
```

```c
#define AETHER_ACK_LEN          3
```

#### 4.6.4 Acknowledgment policy (delayed, coalesced, piggybacked)

- A receiver **MUST NOT** send one ACK per packet. It coalesces: hold a
  pending ACK up to `RELIABLE_ACK_DELAY_MS` (per-PHY) and emit one ACK
  covering the burst.
- It **SHOULD** emit immediately (not wait for the timer) on an out-of-order
  arrival (signals a gap → fast retransmit), on `rwnd` pressure, or at the
  end of an ÆtherStyx message.
- **Piggyback:** when the receiver has reverse DATA queued to the same peer,
  it **SHOULD** carry the ACK block inline by setting `AETHER_FCTL_EXTENDED`
  on that DATA packet and appending `struct aether_ack`, instead of emitting a
  standalone `0x01` packet. This recovers airtime on half-duplex links
  **without** coupling the ACK to any application semantics (it is a transport
  ACK, not "the ÆtherStyx R-message is the ACK").

#### 4.6.5 Sender algorithm

```
send_reliable(dst, payload):
    p = peer_state(dst)
    seq = p.tx_next_seq++; 
    set type=DATA, Delivery=RELIABLE, seq_no=seq
    buffer (seq, payload, now, retries=0) in p.tx_win
    transmit best-effort
    arm RTO if not already armed

on ACK(cum_ack, sack) from peer p:
    free every buffered slot with seq <= cum_ack            (cumulative)
    for each buffered slot above the gap NOT marked in sack:
        retransmit it; reset its timer (selective repair)
    Karn: sample RTT only from slots acked on first try; never from
          retransmitted slots. (Per-PHY base RTO if SRTT not maintained.)

on RTO(slot):
    if slot.retries < RELIABLE_MAX_RETRIES:
        retransmit; slot.retries++; p.rto_ms *= 2        (exp. backoff)
    else:
        drop slot; signal delivery-failure upward (the ÆtherStyx Tag
        times out). No RST — there is no connection to reset.
```

#### 4.6.6 Receiver algorithm

```
on reliable DATA(seq, payload) from peer p:
    if seq already delivered (within the mod-256 past window):
        duplicate -> (re)schedule ACK; drop payload
    else if seq == p.rx_expected:
        deliver upward; p.rx_expected++
        drain any now-contiguous buffered packets
        schedule (coalesced) ACK
    else if seq within (rx_expected, rx_expected+window):
        buffer out-of-order; set bit in p.rx_ooo
        schedule IMMEDIATE ACK (gap signal)
    else:
        outside window -> drop; ACK current cumulative

ACK reflects: cum_ack = rx_expected - 1, sack = p.rx_ooo, rwnd = free slots
```

#### 4.6.7 Fragmentation & message bursts

ÆtherStyx messages (§5) larger than the reliable MTU are split into N mesh
packets, each a reliable DATA packet with consecutive `seq_no`; reassembly
uses the ÆtherStyx `Size` field. A sender **SHOULD** burst all fragments of a
message, then await one coalesced ACK and selectively repair holes. The
in-flight window is a per-PHY parameter (§4.6.8): half-duplex LoRa **SHOULD**
use one message's worth of fragments (effectively stop-and-wait at message
granularity); higher-rate PHYs **MAY** keep multiple messages in flight.

#### 4.6.8 Per-PHY reliability parameters

The algorithm is fixed; these constants are tuned per PHY. The same reliable
datagram serves every PHY — only the timing/window change.

| Parameter | LoRa SF10/BW125 | High-rate PHY (e.g. DECT NR+) |
|-----------|-----------------|-------------------------------|
| Reliable MTU (≤ `MAX_PAYLOAD`) | ~one frame | large |
| In-flight window | 1 message | multi-message |
| Base RTO | seconds | tens of ms |
| `RELIABLE_ACK_DELAY_MS` | 200–500 (coalesce hard) | ~0 (ACK promptly) |
| `RELIABLE_MAX_RETRIES` | ~5 | ~8 |
| Forwarding | best-effort | best-effort |

```c
struct aether_reliable_cfg {
    uint16_t mtu;             /* reliable payload bytes per packet      */
    uint8_t  window;          /* in-flight packets (< 64)               */
    uint32_t rto_base_ms;     /* initial RTO before backoff             */
    uint16_t ack_delay_ms;    /* coalesce/delay window                  */
    uint8_t  max_retries;     /* before delivery-failure                */
};

#define RELIABLE_WINDOW_MAX     32   /* compile-time cap (< 64)          */
#define RELIABLE_IDLE_MS        60000
```

#### 4.6.9 Relationship to ÆtherStyx

The reliable datagram carries any payload; ÆtherStyx (`AETHER_PROTO_STYX`,
§5) is the primary client. ÆtherStyx provides the **session** (attach/fids);
reliable delivery provides **ordered, gap-free packet delivery** beneath it.
The best-effort datagram path (`DELIV_BESTEFFORT`) remains for broadcast and
unacknowledged traffic (chat, Hello/beacons, time-sync).

#### 4.6.10 PHY applicability (LoRa and DECT NR+)

Reliable delivery is defined **once**, at the Æther Mesh layer, and is
intentionally PHY-independent: HeyMac is the common MAC over every supported
PHY, so the algorithm above is identical whether HeyMac runs on the LoRa
(SX1262) PHY or on the DECT NR+ PHY. Only the per-PHY constants of §4.6.8
change. What *does* differ between PHYs is how much work the algorithm has to
do — and understanding why keeps the layering honest.

**The PHY/MAC reliability split.** Error *correction* begins in the PHY;
error *recovery by retransmission* belongs to the MAC:

- **PHY layer (both PHYs):** channel coding / FEC, CRC, synchronization and
  (on OFDM PHYs) equalization. The PHY corrects most bit errors and hands up
  only CRC-valid frames; corrupted frames are simply dropped. LoRa does this
  with chirp spread-spectrum + light coding; DECT NR+ does it with stronger,
  selectable coding (Turbo / multiple MCS) on a far faster, lower-latency
  link. This is "coding gain" — it reduces the *loss rate* the ARQ sees.
- **MAC layer:** retransmission, acknowledgment, sequencing — i.e. ARQ, and
  in cellular-style stacks **HARQ** (ARQ with soft-combining of failed
  transmissions). HARQ uses PHY hardware hooks (soft buffers, redundancy
  versions) but is *controlled* by the MAC.

**Æther runs HeyMac directly on the DECT NR+ PHY — the DECT NR+ MAC is not
used.** Therefore Æther obtains the DECT NR+ PHY's coding gain but **not**
HARQ. §4.6 is consequently the **sole** retransmission/ARQ layer on every
PHY — never redundant with, and never racing, a lower-layer ARQ. (This is the
single-reliability-layer property of §4.6's best-effort-forwarding premise,
preserved across PHYs.)

**Consequence — same algorithm, different job:**

| | LoRa PHY | DECT NR+ PHY (HeyMac-on-PHY) |
|---|---|---|
| Link handed to HeyMac | slow, lossy, best-effort | fast, FEC-clean, **still best-effort** |
| §4.6's role | the reliability layer — frequent retransmit, hard ACK amortization | end-to-end integrity + uniform contract — rare retransmit, cheap ACKs |
| Who retransmits | §4.6 (software) | §4.6 (software) — there is no HARQ |

On DECT NR+ the PHY shrinks both the *residual loss* §4.6 must repair and the
*cost per operation* (fast retransmit, prompt ACK, tight RTO). The ARQ logic
still executes in software, but it fires rarely and cheaply — it effectively
runs in the background. It is fair to think of this as the PHY
**accelerating the conditions** under which §4.6 operates; it is *not* the
PHY executing reliability on §4.6's behalf (it performs no retransmission or
acknowledgment). The algorithm is unchanged; only its workload is offloaded
to silicon.

**Tuning consequences** (beyond the §4.6.8 table):

- The wide-window option of §4.6.2 (16-bit sequence under
  `AETHER_FCTL_EXTENDED`) is *optional on LoRa* (W=1 message) but
  *recommended on DECT NR+*, where the bandwidth-delay product needs a window
  larger than the 1-byte sequence space allows.
- DECT NR+ profiles SHOULD raise the reliable MTU well above LoRa's
  `MAX_PAYLOAD`, so large DECT frames are not chopped into LoRa-sized
  fragments (this also reduces sequence-space pressure).
- Adaptive RTO (Karn + SRTT/RTTVAR) is *nice-to-have* rather than required on
  DECT NR+: a near-lossless link rarely triggers an RTO at all.

**Out of scope (future).** True hardware-accelerated reliability —
HARQ-style *soft-combining*, where a failed transmission's energy is retained
and combined with its retransmission rather than discarded — is **not** part
of §4.6, which is plain selective-repeat (discard-and-resend) ARQ. Adding a
soft-combining mode would require PHY-driver support for soft buffers and
redundancy versions and is deferred; §4.6 as specified is sufficient on both
PHYs.

---

## 5. ÆtherStyx Layer

### 5.1 Message Types (4-bit encoding, 0-15 range)

**Go Constants:**
```go
const (
    Tversion = 0
    Rversion = 1
    Tattach  = 2
    Rattach  = 3
    Twalk    = 4
    Rwalk    = 5
    Topen    = 6
    Ropen    = 7
    Tread    = 8
    Rread    = 9
    Twrite   = 10
    Rwrite   = 11
    Tstat    = 12
    Rstat    = 13
    Tclunk   = 14
    Rclunk   = 15
)
```

**C Constants:**
```c
/* ÆtherStyx Message Types */
#define Tversion    0
#define Rversion    1
#define Tattach     2
#define Rattach     3
#define Twalk       4
#define Rwalk       5
#define Topen       6
#define Ropen       7
#define Tread       8
#define Rread       9
#define Twrite      10
#define Rwrite      11
#define Tstat       12
#define Rstat       13
#define Tclunk      14
#define Rclunk      15
```

### 5.2 Wire Format

```
+----------+----------+----------+----------+
| Size (2 bytes, big-endian)    | Type+Tag |
|                               | (1 byte) |
+-------------------------------+----------+
| FID (4 bytes, big-endian)                |
+------------------------------------------+
| Data (variable length)                   |
+------------------------------------------+
```

### 5.3 Structure Definitions

**Go Structure:**
```go
type ÆtherStyxMsg struct {
    Size    uint16    // Message size (2 bytes, big-endian)
    Type    uint8     // Message type (4 bits) 
    Tag     uint8     // Message tag (4 bits)
    FID     uint32    // File identifier (4 bytes, big-endian)
    Data    []byte    // Message data
}
```

**C Structure:**
```c
typedef struct ÆtherStyxMsg {
    ushort size;        /* Message size (2 bytes, big-endian) */
    uchar type;         /* Message type (4 bits) */
    uchar tag;          /* Message tag (4 bits) */
    ulong fid;          /* File identifier (4 bytes, big-endian) */
    uchar *data;        /* Message data */
    int data_len;       /* Data length */
} ÆtherStyxMsg;
```

### 5.4 ÆtherStyx Constants

**Go:**
```go
const (
    MAX_MSG_SIZE = 128
    OREAD        = 0
    OWRITE       = 1
    ORDWR        = 2
    OEXEC        = 3
)
```

**C:**
```c
/* ÆtherStyx Constants */
#define MAX_MSG_SIZE    128
#define OREAD           0
#define OWRITE          1
#define ORDWR           2
#define OEXEC           3
```

---

## 6. Implementation Examples

### 6.1 Optimal Header Preparation

**Go Example:**
```go
// Optimal header preparation
func PrepareHeader(macNetID, meshNetID uint16, msgType uint8, src, dst [6]uint8, payloadLen uint16) MeshHeader {
    header := MeshHeader{
        Type:        msgType,
        TTL:         64,
        Source:      src,
        Destination: dst,
        Length:      payloadLen,
    }
    
    // Conditional network ID for air-time optimization
    if meshNetID != macNetID {
        header.Flags |= AETHER_FCTL_MESH_NET_ID
        header.MeshNetID = meshNetID
    }
    // If meshNetID == macNetID, omit MeshNetID field (saves 2 bytes)
    
    return header
}

// Variable header size calculation
func (h MeshHeader) Size() int {
    size := 16  // Base header size
    if h.Flags & AETHER_FCTL_MESH_NET_ID != 0 {
        size += 2  // Add mesh network ID
    }
    return size
}
```

**C Example:**
```c
/*
 * Prepare a mesh header with optimal network ID handling
 * If mesh_net_id matches mac_net_id, omit mesh_net_id to save bytes
 */
void
aether_prepare_header(MeshHeader *header, ushort mac_net_id, ushort mesh_net_id,
                     uchar type, uchar *src, uchar *dst, ushort payload_len)
{
    if(header == nil)
        return;
        
    memset(header, 0, sizeof(MeshHeader));
    
    /* Set basic fields */
    header->type = type;
    header->ttl = 64;  /* Default TTL */
    header->seq_no = 0; /* Will be set by caller */
    header->length = payload_len;
    
    /* Copy addresses */
    if(src != nil)
        memmove(header->src, src, 6);
    if(dst != nil)
        memmove(header->dst, dst, 6);
    
    /* Optimize network ID handling */
    if(mesh_net_id != mac_net_id) {
        /* Mesh network differs from MAC - include it */
        header->flags |= AETHER_FCTL_MESH_NET_ID;
        header->mesh_net_id = mesh_net_id;
    } else {
        /* Same network - omit mesh_net_id to save 2 bytes */
        header->flags &= ~AETHER_FCTL_MESH_NET_ID;
        header->mesh_net_id = 0; /* Not transmitted */
    }
}

/*
 * Calculate the actual size of a mesh header based on its flags
 */
int
aether_header_size(MeshHeader *header)
{
    int size = 16; /* Base header size */
    
    if(header != nil && (header->flags & AETHER_FCTL_MESH_NET_ID))
        size += 2; /* Add 2 bytes for mesh network ID */
        
    return size;
}
```

### 6.2 Network ID Consistency
- **All layers now use 2-byte network IDs** (`uint16` in Go, `ushort` in C)
- HeyMac layer uses `net_id` (2 bytes)
- Mesh layer uses `network_id` (2 bytes) - **CONSISTENT**

### 6.3 Addressing
- Primary addressing uses 6-byte MAC addresses (`[6]uint8` in Go, `uchar[6]` in C)
- Short addressing (2 bytes) available as optimization
- Address resolution between short/long forms required

### 6.4 Byte Order
- Multi-byte fields use **big-endian** encoding on wire
- Size fields, FIDs, and length fields are big-endian
- Address fields are transmitted in natural byte order

### 6.5 Frame Encapsulation

```
[LoRa PHY @ 915MHz SF7 BW125]
  └─ [HeyMac Frame]
       └─ [Æther Mesh Header + Payload]
            └─ [ÆtherStyx Message] (if Type=AETHER_PROTO_STYX)
```

### 6.6 Security Implementation
- Uses HeyMac's CGA (Cryptographically Generated Addresses)
- Signature field optional, variable length (up to 128 bytes)
- Public key distribution via security protocol messages

### 6.7 PHY Layer Simulation
- **Frequency**: 915000000 Hz (915 MHz, North America)
- **Range Calculation**: Use SF-based sensitivity table for link budget
- **Time-on-Air**: Calculate based on SF, BW, CR, and packet size
- **Power Consumption**: Model based on TX power and radio state
- **Error Rates**: Model CRC errors, timeouts based on RSSI/SNR

**Air-time Impact**: In typical single-network deployments, this optimization reduces header overhead from 18 to 16 bytes, providing 12.5% bandwidth savings - significant for LoRa's limited capacity.

---

*This specification reflects the actual implementation as of the June 2025 cleanup and standardization effort.* 
