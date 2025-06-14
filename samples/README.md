# Æther Stack Sample Applications

This directory contains sample applications demonstrating the Æther mesh networking stack on Zephyr RTOS.

## Available Samples

### aether_basic
**Purpose**: Basic mesh node demonstration  
**Features**:
- Initializes HeyMac L2 and Æther mesh layers
- Periodic neighbor discovery
- Shell commands for sending messages and viewing network state
- Suitable for testing basic connectivity

**Use Case**: First node to flash when testing the mesh network

### aether_gateway
**Purpose**: Internet gateway node  
**Features**:
- All basic node functionality
- Ethernet/WiFi uplink support
- MQTT bridge for cloud connectivity
- Route aggregation and announcement
- Network monitoring and statistics

**Use Case**: Deploy as edge nodes that bridge LoRa mesh to internet

### aether_sensor
**Purpose**: Low-power sensor node  
**Features**:
- Periodic sensor readings (temperature, humidity, etc.)
- Power-optimized sleep scheduling
- Automatic mesh routing to nearest gateway
- Configurable reporting intervals
- Battery voltage monitoring

**Use Case**: Deploy as battery-powered environmental monitoring nodes

### aether_modem
**Purpose**: AT command modem interface  
**Features**:
- Hayes AT-style command set for mesh access
- Serial/UART host interface
- Transparent data mode for streaming
- Configuration persistence
- Compatible with existing modem-based applications
- Power management commands

**Use Case**: Drop-in LoRa mesh modem for embedded systems or PCs via serial port

## Hardware Requirements

All samples require:
- Zephyr-supported board (ESP32-S3, nRF52840, etc.)
- Semtech SX126x LoRa transceiver (SX1261/SX1262)
- Appropriate antenna for your frequency band

## Building Samples

```bash
# Navigate to Aether module root
cd /path/to/aephyr

# Build for ESP32-S3 DevKit
west build -p auto -b esp32s3_devkitc samples/<sample_name>

# Build for nRF52840 DK
west build -p auto -b nrf52840dk_nrf52840 samples/<sample_name>

# Flash and monitor
west flash
west espressif monitor  # For ESP32
# or
west attach             # For nRF/JLink
```

## Configuration

Each sample includes:
- `prj.conf` - Base configuration
- `boards/*.overlay` - Board-specific device tree overlays
- `src/main.c` - Application code

Customize network parameters in `prj.conf`:
- `CONFIG_AETHER_MESH_NODE_ID` - Unique node identifier
- `CONFIG_AETHER_MESH_NETWORK_ID` - Mesh network ID (default: 0x915E)
- `CONFIG_AETHER_LORA_FREQ` - LoRa frequency in Hz
- `CONFIG_AETHER_LORA_TX_POWER` - Transmit power in dBm

## Testing Multi-Node Networks

1. Flash `aether_gateway` on one or more edge nodes
2. Flash `aether_basic` on intermediate routing nodes  
3. Flash `aether_sensor` on leaf nodes
4. Use shell commands to verify connectivity:
   ```
   aether status
   aether neighbors
   aether routes
   aether send <node_id> "test message"
   ```

## Troubleshooting

- Ensure antennas are connected before powering on
- Check SPI wiring if SX126x is not detected
- Verify all nodes use the same network ID and frequency
- Enable debug logging in `prj.conf` for detailed diagnostics