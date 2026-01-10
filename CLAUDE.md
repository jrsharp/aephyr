# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is the Æther Stack - a Zephyr RTOS module implementing HeyMac Layer 2 protocol and Æther mesh networking for LoRa networks. The project provides multi-hop mesh networking capabilities optimized for low-power, long-range communication.

## Using as a Library

This module is designed to be consumed by other Zephyr projects (e.g., cyberterminal firmware).

### Integration via ZEPHYR_EXTRA_MODULES

In your project's **CMakeLists.txt** (before `find_package(Zephyr)`):
```cmake
list(APPEND ZEPHYR_EXTRA_MODULES /path/to/aephyr)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
```

### Required Kconfig Options

Add to your project's **prj.conf**:
```kconfig
# Networking stack
CONFIG_NETWORKING=y
CONFIG_NET_L2_CUSTOM=y

# HeyMac L2 (MAC layer)
CONFIG_NET_L2_HEYMAC=y

# Aether mesh networking
CONFIG_AETHER_MESH=y

# LoRa driver integration
CONFIG_AETHER_LORA=y

# LoRa hardware (you likely have this already)
CONFIG_LORA=y
CONFIG_LORA_SX126X=y
```

### API Usage

```c
#include <zephyr/net/aether_mesh.h>

/* Receive callback - called when messages arrive */
void on_message(struct net_if *iface, const uint8_t src[6],
                const uint8_t *data, size_t len, void *user_data)
{
    printk("From %02x:%02x:%02x:%02x:%02x:%02x: %.*s\n",
           src[0], src[1], src[2], src[3], src[4], src[5],
           (int)len, data);
}

/* Initialization (after network interface is up) */
struct net_if *iface = net_if_get_default();
net_if_up(iface);
aether_mesh_init(iface, 0x915E);  /* 0x915E = 915MHz network */
aether_mesh_register_recv_callback(iface, on_message, NULL);

/* Send to specific address */
uint8_t dest[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
aether_mesh_send(iface, dest, "Hello", 5, AETHER_PRIORITY_NORMAL);

/* Broadcast to all neighbors */
uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
aether_mesh_send(iface, bcast, "Hello all", 9, AETHER_PRIORITY_NORMAL);

/* Query mesh status */
int neighbors = aether_mesh_get_neighbor_count(iface);
int routes = aether_mesh_get_route_count(iface);
```

### Key Header Files

- `<zephyr/net/aether_mesh.h>` - Mesh networking API (send, receive, routing)
- `<zephyr/net/heymac.h>` - Low-level MAC layer (address management, network ID)

## Build Commands

### Module Development
```bash
# Initialize module structure
mkdir -p zephyr subsys/net/{l2/heymac,aether} drivers/lora include/zephyr/net
mkdir -p dts/bindings/net samples tests

# Build tests (when implemented)
west build -p auto -b native_sim tests/subsys/heymac
west build -p auto -b native_sim tests/subsys/aether
```

### Integration Testing (ESP32-S3)
```bash
# Build for ESP32-S3 DevKit
west build -p auto -b esp32s3_devkitc samples/aether_basic
west build -p auto -b esp32s3_devkitc samples/aether_gateway
west build -p auto -b esp32s3_devkitc samples/aether_sensor

# Flash to device
west flash

# Monitor output
west espressif monitor
```

### Code Quality
```bash
# Run Zephyr compliance checks
scripts/checkpatch.pl -f <file>

# Static analysis (when available)
west build -t static
```

## Architecture

### Module Structure
- **HeyMac Layer 2** (`subsys/net/l2/heymac/`): MAC layer protocol for LoRa, provides addressing and framing
- **Æther Mesh** (`subsys/net/aether/`): Multi-hop routing protocol with neighbor discovery and route management
- **LoRa Driver** (`drivers/lora/`): Integration layer for SX126X transceivers with RX/TX thread management

### Key Design Decisions
1. **Conditional Network ID**: Mesh network ID can be omitted when it matches MAC network ID (12.5% header reduction)
2. **Thread Architecture**: Separate RX and TX threads with configurable priorities and stack sizes
3. **Routing**: Simple distance-vector routing with configurable table size (4-64 routes)
4. **Power Management**: Beacon-based coordination for sleep scheduling

### Configuration Hierarchy
1. Kconfig options provide compile-time defaults
2. Device tree properties override Kconfig at build time
3. Runtime configuration via shell commands (if enabled)

## Development Guidelines

### Adding New Features
1. Follow Zephyr coding standards and conventions
2. All new Kconfig options must have help text and sensible defaults
3. Use existing logging infrastructure (LOG_MODULE_REGISTER)
4. Thread stack sizes must account for ESP32-S3 requirements (larger than Nordic)

### File Organization
- Public APIs go in `include/zephyr/net/`
- Implementation files in appropriate `subsys/` directory
- Keep HeyMac and Æther layers properly separated
- Driver code isolated in `drivers/lora/`

### Testing Requirements
- Unit tests for protocol logic in `tests/subsys/`
- Integration tests for full stack in `tests/integration/`
- Sample applications demonstrate key use cases
- All samples must work on ESP32-S3 DevKit

### Key Configurations
```kconfig
# Essential configs for development
CONFIG_NETWORKING=y
CONFIG_NET_L2_CUSTOM=y
CONFIG_NET_L2_HEYMAC=y
CONFIG_AETHER_MESH=y
CONFIG_AETHER_LORA=y
CONFIG_AETHER_SHELL=y     # Enable for debugging
CONFIG_LOG=y
CONFIG_NET_L2_HEYMAC_LOG_LEVEL=4
CONFIG_AETHER_MESH_LOG_LEVEL=4
```

## Protocol Implementation Notes

### HeyMac Frame Format
- Network ID: 2 bytes (0x915E for 915MHz)
- Address: 2 bytes (short) or 6 bytes (long)
- Frame type and flags in header
- CRC handled by LoRa hardware

### Æther Mesh Extensions
- TTL field for hop limiting
- Next-hop routing
- Hello messages for neighbor discovery
- Route announcements for path establishment

### Performance Targets
- Sub-100ms message forwarding latency
- Support for 50+ node networks
- 1% duty cycle compliance for regulatory limits
- Battery life measured in months for sensor nodes