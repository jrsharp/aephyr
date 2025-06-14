# Æther Protocol Specification

## Version Information
**Version:** 1.2
**Last Updated:** June 2025  
**Status:** Complete Implementation Specification  

**Changes in this version:**
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
