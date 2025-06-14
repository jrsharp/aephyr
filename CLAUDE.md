# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is the Æther Stack - a Zephyr RTOS module implementing HeyMac Layer 2 protocol and Æther mesh networking for LoRa networks. The project provides multi-hop mesh networking capabilities optimized for low-power, long-range communication.

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