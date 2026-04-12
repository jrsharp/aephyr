# Æther Modem Sample

This sample implements an AT command interface for the Æther mesh network, allowing retrocomputers and other serial-based systems to access the network.

## Overview

The Æther modem provides:
- Standard AT command interface over USB CDC ACM or UART
- Full access to Æther mesh networking capabilities
- Message store-and-forward functionality
- 9P/Styx protocol client
- BBS and chat service access
- Optimized for resource-constrained retrocomputers

## Requirements

### Hardware
- RP2040-based board (Raspberry Pi Pico recommended)
- SX1262 LoRa module
- USB cable or UART connection

### Software
- Zephyr RTOS with Æther module
- Terminal program supporting AT commands

## Building

### For Raspberry Pi Pico

```bash
west build -p auto -b rpi_pico samples/aether_modem
west flash
```

### For Generic RP2040

```bash
west build -p auto -b rp2040 samples/aether_modem -- -DBOARD_ROOT=.
west flash
```

### For ESP32-S3 (alternative)

```bash
west build -p auto -b esp32s3_devkitc samples/aether_modem
west flash
```

## Hardware Connections

### RP2040 to SX1262

| RP2040 Pin | SX1262 Pin | Function |
|------------|------------|----------|
| GP2 (SPI0 SCK) | SCK | SPI Clock |
| GP3 (SPI0 TX) | MOSI | SPI Data Out |
| GP4 (SPI0 RX) | MISO | SPI Data In |
| GP5 | NSS | SPI Chip Select |
| GP6 | RESET | Radio Reset |
| GP7 | BUSY | Radio Busy |
| GP8 | DIO1 | Radio Interrupt |
| 3V3 | VCC | Power |
| GND | GND | Ground |

## Usage

### Basic Connection

1. Connect via USB or UART (9600 baud, 8N1)
2. Send `AT` to verify connection
3. Initialize with `AT+INIT`
4. Join network with `AT+NETJOIN`

### Example Session

```
AT
OK

AT+INIT
OK

AT+NETJOIN
OK
+NETJOIN: CONNECTED

AT+NETALIAS=PICO1
OK

AT+MSGSEND=BROADCAST,13
>Hello, World!
OK
+MSGSEND: 1

AT+MSGRECV
+MSG: 2,NODE42,11
+MSG: Hello back!
OK
```

## AT Command Reference

See [AT_COMMAND_INTERFACE.md](../../AT_COMMAND_INTERFACE.md) for complete command documentation.

### Essential Commands

| Command | Description |
|---------|-------------|
| `AT` | Test connection |
| `AT+INIT` | Initialize radio |
| `AT+NETJOIN` | Join Æther network |
| `AT+MSGSEND` | Send message |
| `AT+MSGRECV` | Check for messages |
| `AT+9PATTACH` | Connect to 9P server |
| `AT+BBSLS` | List BBS boards |

## Configuration

### Kconfig Options

Key configuration options in `prj.conf`:

```kconfig
# Network settings
CONFIG_HEYMAC_DEFAULT_NET_ID=0x915E
CONFIG_AETHER_HELLO_INTERVAL=60

# AT interface settings
CONFIG_AT_CMD_BUFFER_SIZE=256
CONFIG_AT_MAX_MSG_SIZE=128

# Message store
CONFIG_MSG_STORE_SIZE=10
CONFIG_MSG_MAX_AGE=3600
```

### Board-Specific Configuration

Create board overlays for custom hardware:

`boards/myboard.overlay`:
```dts
&spi0 {
    lora0: sx1262@0 {
        reset-gpios = <&gpio0 10 GPIO_ACTIVE_LOW>;
        busy-gpios = <&gpio0 11 GPIO_ACTIVE_HIGH>;
        dio1-gpios = <&gpio0 12 GPIO_ACTIVE_HIGH>;
    };
};
```

## Memory Usage

Typical memory footprint on RP2040:

- Flash: ~120KB
- RAM: ~40KB
  - Network stack: 16KB
  - Message store: 8KB
  - Thread stacks: 8KB
  - AT buffers: 2KB
  - Heap: 6KB

## Power Consumption

- Active RX: ~15mA @ 3.3V
- Active TX: ~90mA @ 3.3V (14dBm)
- Sleep mode: <1mA
- USB enumerated: +20mA

## Integration Examples

### Apple II

```basic
10 PRINT CHR$(4);"PR#2"
20 PRINT "AT+INIT"
30 INPUT A$
40 PRINT "AT+NETJOIN"
50 INPUT B$
60 PRINT "AT+BBSLS"
70 INPUT C$: PRINT C$
80 IF LEFT$(C$,1)="+" THEN 70
90 PRINT CHR$(4);"PR#0"
```

### Terminal Software

Works with any terminal supporting:
- 9600 baud (configurable up to 115200)
- 8 data bits, no parity, 1 stop bit
- Optional XON/XOFF or RTS/CTS flow control

## Troubleshooting

### No Response to AT Commands
- Check USB enumeration or UART connection
- Verify baud rate (default 9600)
- Try hardware reset

### Network Join Fails
- Verify LoRa module connections
- Check antenna is connected
- Ensure another node is in range

### Message Send Errors
- Check duty cycle with `AT+DUTYCYCLE?`
- Verify destination address/alias
- Ensure network is joined

## Advanced Features

### Store-and-Forward
The modem automatically stores messages when destination is unreachable and forwards when route becomes available.

### 9P/Styx File Access
Access remote filesystems through the mesh network using standard 9P commands.

### Service Integration
Built-in clients for BBS, chat, and other Æther services.

## License

SPDX-License-Identifier: MIT