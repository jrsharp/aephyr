# ESP32-S3 + SX1262 Build & Flash Instructions

## Hardware Connections

Connect your SX1262 module to ESP32-S3 DevKit:

| ESP32-S3 | SX1262  | Wire Color (suggestion) |
|----------|---------|-------------------------|
| GPIO 11  | MOSI    | Blue                    |
| GPIO 13  | MISO    | Green                   |
| GPIO 12  | SCLK    | Yellow                  |
| GPIO 10  | CS/NSS  | Orange                  |
| GPIO 14  | RESET   | White                   |
| GPIO 15  | BUSY    | Gray                    |
| GPIO 16  | DIO1    | Purple                  |
| 3.3V     | VCC     | Red                     |
| GND      | GND     | Black                   |

⚠️ **IMPORTANT**: Always connect an antenna to the SX1262 before powering on!

## Build & Flash Commands

```bash
# 1. Navigate to the Aether module directory
cd /Users/jrsharp/src/aephyr

# 2. Build the sample application
west build -p auto -b esp32s3_devkitc samples/aether_basic

# 3. Flash to your ESP32-S3
west flash

# 4. Monitor the output
west espressif monitor
```

## What You Should See

After flashing, the serial monitor should show:
- Aether stack initialization
- SX1262 detection and configuration
- Neighbor discovery messages
- Shell prompt for interactive commands

## Shell Commands

Once running, try these commands:

```bash
# Check device status
device list

# View Aether configuration
aether status

# Send a test message
aether send 0x0002 "Hello from node 1!"

# View routing table
aether routes

# View neighbors
aether neighbors
```

## Troubleshooting

If the SX1262 isn't detected:
1. Double-check your wiring connections
2. Verify 3.3V power to the module
3. Check that an antenna is connected
4. Try reducing SPI speed in the overlay file

Press Ctrl+] to exit the monitor.