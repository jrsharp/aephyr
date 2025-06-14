# Zephyr RTOS Wireless Mesh Networking Stack Implementation Analysis

## Overview

This document analyzes how wireless mesh networking stacks are conventionally implemented in Zephyr RTOS, providing guidance for implementing the HeyMac/Æther mesh networking stack.

## 1. L2 (Layer 2) Implementation Structure

### Core Architecture

Zephyr's L2 implementation follows a clear architectural pattern:

1. **L2 Registration**: Custom L2 layers use the `NET_L2_INIT()` macro to register with the network stack
2. **Interface Abstraction**: The `net_if` structure provides abstraction between upper layers and L2
3. **Driver API**: Each L2 implementation defines its driver API requirements

### Key Components

```c
// L2 registration pattern
NET_L2_INIT(L2_NAME, recv_fn, send_fn, enable_fn, flags_fn);
```

The L2 layer is responsible for:
- Hiding networking link-layer details from upper layers
- Managing device driver communication
- Providing a consistent API through `net_l2.h`

### Directory Structure

```
subsys/net/l2/
├── ethernet/       # Ethernet L2 implementation
├── ieee802154/     # IEEE 802.15.4 implementation
├── openthread/     # OpenThread mesh implementation
├── bluetooth/      # Bluetooth L2 (separate from BT Mesh)
└── custom/         # Custom L2 implementations
```

## 2. Network Interface Registration and Packet Handling

### net_if_api Structure

The `net_if_api` is the core interface between network drivers and the L2 layer:

```c
struct net_if_api {
    void (*init)(struct net_if *iface);
    int (*send)(struct net_if *iface, struct net_pkt *pkt);
    // Additional L2-specific methods
};
```

### Device Registration Patterns

Network devices register using macros:
- `NET_DEVICE_INIT_INSTANCE()` - General network device
- `ETH_NET_DEVICE_INIT()` - Ethernet-specific
- Custom macros for specialized L2 layers

### Packet Buffer Management

Key patterns for packet handling:

1. **Allocation**:
   - TX: `net_pkt_alloc_with_buffer()` from TX pool
   - RX: `net_pkt_rx_alloc_with_buffer()` from RX pool

2. **Pool Configuration**:
   - `CONFIG_NET_PKT_RX_COUNT` - RX packet pool size
   - `CONFIG_NET_PKT_TX_COUNT` - TX packet pool size
   - `CONFIG_NET_BUF_DATA_SIZE` - Fixed buffer size

3. **Data Access**:
   - Cursor-based read/write operations
   - Support for fragmented buffers
   - Network-aware buffer management

## 3. Mesh Routing Integration Patterns

### OpenThread Integration

OpenThread demonstrates a sophisticated mesh integration:

1. **Threading Model**:
   - `openthread` thread - Frame reception and processing
   - `rx_workq` - L2 frame routing decisions
   - `tx_workq` - Transmission scheduling

2. **Data Flow**:
   - RX: Radio → OpenThread stack → `net_recv_data()` → Zephyr IP stack
   - TX: Application → IP stack → OpenThread → Radio driver

3. **Key Functions**:
   - `openthread_recv()` - L2 receive handler
   - `openthread_send()` - L2 transmit handler
   - `ot_receive_handler()` - Injection into Zephyr stack

### Bluetooth Mesh Architecture

Bluetooth Mesh uses a different pattern:

1. **Layer Structure**:
   - Network layer (`net.c`) - IV management, caching
   - Transport layer (`transport.c`) - SAR, security
   - Advertising layer (`adv.c`) - Mesh-specific advertising

2. **Security Integration**:
   - Provisioning as separate process
   - Mesh-specific security APIs
   - Persistent storage via settings subsystem

## 4. RX/TX Thread Patterns and Queue Management

### Common Threading Patterns

1. **Separate RX/TX Threads**:
   ```c
   // RX thread pattern
   K_THREAD_DEFINE(rx_thread, RX_STACK_SIZE,
                   rx_thread_fn, NULL, NULL, NULL,
                   RX_PRIORITY, 0, 0);
   
   // TX thread pattern
   K_THREAD_DEFINE(tx_thread, TX_STACK_SIZE,
                   tx_thread_fn, NULL, NULL, NULL,
                   TX_PRIORITY, 0, 0);
   ```

2. **Work Queue Pattern**:
   - Used by OpenThread and others
   - Allows deferred processing
   - Better for power management

3. **FIFO/Queue Usage**:
   - `k_fifo` for packet queuing
   - `k_work` for deferred processing
   - `k_sem` for synchronization

### Priority Considerations

- RX threads typically higher priority than TX
- Time-critical operations in ISR or high-priority threads
- Background tasks in lower-priority work queues

## 5. Custom L2 Integration with net_if and net_pkt APIs

### Implementation Requirements

1. **Mandatory Functions**:
   - `init()` - Interface initialization
   - `send()` - Packet transmission
   - `recv()` - Packet reception handler
   - `enable()` - Interface enable/disable
   - `flags()` - Capability flags

2. **Packet Processing**:
   ```c
   // RX path
   struct net_pkt *pkt = net_pkt_rx_alloc_with_buffer(...);
   // Fill packet with received data
   net_recv_data(iface, pkt);
   
   // TX path
   int send(struct net_if *iface, struct net_pkt *pkt) {
       // Process packet for transmission
       // Send to hardware
       net_pkt_unref(pkt);
   }
   ```

3. **Extended APIs**:
   - Some L2 layers (like IEEE 802.15.4) extend the basic API
   - Additional methods for L2-specific operations
   - Hardware offloading capabilities

### Integration Best Practices

1. **Buffer Management**:
   - Always check allocation success
   - Properly reference count packets
   - Clean up on error paths

2. **Thread Safety**:
   - Use appropriate locking mechanisms
   - Consider ISR vs thread context
   - Avoid blocking in critical paths

3. **Configuration**:
   - Kconfig for compile-time options
   - Device tree for hardware configuration
   - Runtime configuration via shell (optional)

## 6. Recommendations for HeyMac/Æther Implementation

Based on this analysis, here are recommendations for implementing HeyMac/Æther:

### 1. Follow the IEEE 802.15.4 Pattern

Since HeyMac is a MAC layer for LoRa (similar to how IEEE 802.15.4 is a MAC for 2.4GHz radios):
- Create a custom radio API similar to `ieee802154_radio_api`
- Extend `net_if_api` for LoRa-specific operations
- Implement proper L1→L2 callbacks

### 2. Threading Architecture

```c
// Recommended thread structure
- lora_rx_thread    // High priority RX processing
- aether_mesh_thread // Medium priority routing decisions
- lora_tx_thread    // Lower priority TX handling
- aether_maint_thread // Lowest priority maintenance (beacons, etc.)
```

### 3. Packet Flow

```
RX: LoRa ISR → RX FIFO → lora_rx_thread → heymac_process → 
    aether_route → net_recv_data()

TX: net_if_send → aether_send → heymac_frame → 
    TX queue → lora_tx_thread → Radio
```

### 4. Configuration Structure

```
CONFIG_NET_L2_HEYMAC=y          # Enable HeyMac L2
CONFIG_AETHER_MESH=y            # Enable Æther routing
CONFIG_AETHER_ROUTING_TABLE_SIZE=16  # Routing entries
CONFIG_HEYMAC_ADDR_SIZE=2       # Short addresses by default
```

### 5. Key Implementation Files

```
subsys/net/l2/heymac/
├── heymac.c          # L2 implementation
├── heymac_frame.c    # Frame formatting
├── heymac_mgmt.c     # Management frames
└── Kconfig           # Configuration

subsys/net/aether/
├── aether.c          # Mesh routing core
├── aether_route.c    # Routing table
├── aether_neighbor.c # Neighbor discovery
├── aether_beacon.c   # Beacon management
└── Kconfig           # Configuration

drivers/lora/
├── sx126x_aether.c   # LoRa driver integration
└── Kconfig           # Driver configuration
```

## Conclusion

Zephyr provides well-established patterns for implementing wireless mesh networking stacks. The key is to:
1. Follow existing L2 implementation patterns
2. Use appropriate threading models for RX/TX separation
3. Properly integrate with net_if and net_pkt APIs
4. Provide clear separation between MAC and routing layers
5. Support both compile-time and runtime configuration

The OpenThread and IEEE 802.15.4 implementations provide excellent examples to follow for the HeyMac/Æther implementation.