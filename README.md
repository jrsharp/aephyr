# Æther Stack for Zephyr RTOS

A complete implementation of the HeyMac Layer 2 protocol and Æther mesh networking stack for Zephyr RTOS, optimized for LoRa networks.

## Overview

The Æther Stack provides:

- **HeyMac Layer 2**: MAC layer protocol for LoRa networks with flexible addressing and frame control
- **Æther Mesh**: Multi-hop mesh networking with neighbor discovery and routing
- **LoRa Integration**: Native integration with Zephyr's LoRa subsystem (SX126X)
- **ESP32-S3 Optimized**: Thread configurations tuned for ESP32-S3 requirements

## Features

### Protocol Stack
- 4-layer architecture: LoRa PHY → HeyMac L2 → Æther Mesh → Application
- IEEE 802.15.4-style frame format with LoRa optimizations
- Conditional network ID transmission (12.5% header reduction)
- Short (2-byte) and long (6-byte) addressing modes
- TTL-based loop prevention and hop limiting

### Mesh Networking
- Neighbor discovery via hello messages
- Distance-vector routing with route caching
- Route request/reply protocol for path discovery
- Configurable routing table size (4-64 routes)
- Power-aware topology management

### Performance
- Sub-100ms message forwarding latency
- Support for 50+ node networks
- 1% duty cycle compliance for regulatory limits
- Optimized for low-power, long-range communication

## DECT NR+ Port (nRF9151 / Thingy:91 X)

Beyond the original LoRa target, the Æther/HeyMac stack has been ported to
**DECT NR+** ("non-cellular 5G") on Nordic's nRF9151 SiP, running directly on the
bare DECT PHY via the `nrf_modem_dect_phy` API. The mesh layer is unchanged; a
parallel net-device driver (`drivers/dect/`) replaces the LoRa PHY, and **HONR16**
structured tree routing (`subsys/net/aether/aether_honr.c`) provides
self-organizing, stateless multi-hop addressing — runtime root election, smarter
(shallowest-parent) join, and root-loss self-healing, so identical firmware on
every node converges to a rooted tree on its own. See
[docs/DECT_HONR_ROUTING.md](docs/DECT_HONR_ROUTING.md).

Build (requires the licensed DECT NR+ PHY modem firmware, obtained from Nordic Sales):

```bash
west build -b nrf9151dk/nrf9151/ns samples/aether_basic -- -DCONF_FILE=prj_dect.conf
```

### ⚠️ Regulatory & RF compliance — read before transmitting

DECT NR+ operates on **regulated** (though licence-exempt) spectrum, *not* a
general ISM band. **Operating these devices on-air is your responsibility, at the
development site and in any deployment region.**

**Band / spectrum.** `CONFIG_AETHER_DECT_CARRIER` selects an absolute DECT channel
number; the modem firmware restricts operation to DECT bands 1/2/3/4/9/22, all in
the ~1.9 GHz DECT allocation. Common settings:
- `1677` = band 1 = **1880–1900 MHz** (EU; channels per **ETSI EN 301 406‑2**, table 4.3.2.3‑2).
- `1711` = band 9 = **1920–1930 MHz** (US/Canada; the UPCS band under **47 CFR §15.323** — the same spectrum US DECT 6.0 cordless phones use).

You have channel-level control *within* the allowed band only (≈5–6 channels in US
band 9 at 1.728 MHz spacing); you cannot operate off-band or at an arbitrary
frequency. (Band 4 is further restricted to carriers 525–551 to avoid LTE.)

**Power.** The US limit is lower than the EU's (~10 dBm vs ~19 dBm). This build
defaults conservatively (`CONFIG_AETHER_DECT_TX_POWER=10`); verify EIRP including
antenna gain against the applicable rule.

**Channel access / "duty cycle".** The DECT band uses a TDMA frame plus a
listen-before-talk **channel-access etiquette**, not a fixed percentage duty-cycle
cap. LBT is implemented (`CONFIG_AETHER_DECT_LBT_*`). Actual airtime is light here
(tree announce ~5 s, hello ~30 s, chat on demand — each a single short slot, well
under 1% per node); continuous receive is unregulated.

**Compliance status.** This is a **research / development** stack intended for use
on development kits:
- It runs a **custom MAC** (HeyMac/HONR) directly on the DECT PHY. DECT NR+
  certification requires the *standard DECT MAC*, so this is **not a certifiable
  DECT NR+ product** and must not be marketed or sold as one.
- The LBT is a good-faith implementation, not a verified §15.323 access etiquette.
- If you cannot ensure local access rules are met, operate only inside an RF chamber.

**Amateur radio.** 1.9 GHz DECT is **not** an amateur allocation, and the nRF9151
DECT modem is band-locked to the DECT bands, so an amateur licence grants no extra
privileges here. (A forthcoming Nordic nRF9151 variant runs DECT NR+ on
**902–928 MHz**, which *is* a secondary amateur allocation — that part, not this
1.9 GHz hardware, is where Part 97 latitude could apply.)

> **Not legal advice.** Confirm against the current text of 47 CFR §15.323 / ETSI
> EN 301 406‑2 and your local regulator; for any product, engage an FCC TCB or
> compliance lab. Certification of a DECT NR+ end product also requires a
> compliant MAC, which this research stack deliberately does not implement.

## Quick Start

### 1. Add to Your Zephyr Project

Add to your `west.yml`:

```yaml
manifest:
  projects:
    - name: aether-zephyr
      url: https://github.com/aephyr/aether-zephyr
      revision: main
      path: modules/aether
```

### 2. Configure Your Project

In your `prj.conf`:

```kconfig
# Enable networking
CONFIG_NETWORKING=y
CONFIG_NET_L2_CUSTOM=y

# Enable LoRa
CONFIG_LORA=y
CONFIG_LORA_SX126X=y

# Enable Æther stack
CONFIG_NET_L2_HEYMAC=y
CONFIG_AETHER_MESH=y
CONFIG_AETHER_LORA=y
CONFIG_AETHER_SHELL=y

# ESP32-S3 optimizations
CONFIG_AETHER_RX_STACK_SIZE=3072
CONFIG_AETHER_TX_STACK_SIZE=2048
CONFIG_MAIN_STACK_SIZE=4096

# Logging
CONFIG_LOG=y
CONFIG_NET_L2_HEYMAC_LOG_LEVEL=4
CONFIG_AETHER_MESH_LOG_LEVEL=4
```

### 3. Device Tree Configuration

Create an overlay file (e.g., `esp32s3_devkitc.overlay`):

```dts
/ {
    chosen {
        zephyr,lora = &lora0;
    };
    
    aether_config: aether-mesh-net {
        compatible = "aether-mesh-net";
        network-id = <0x915E>;      // "915E" for 915MHz network
        node-id = <0x0001>;         // Unique node ID
        mesh-enabled = <1>;
        max-hops = <6>;
        hello-interval = <30>;
        route-timeout = <300>;
    };
};

&spi2 {
    status = "okay";
    cs-gpios = <&gpio0 10 GPIO_ACTIVE_LOW>;
    
    lora0: sx1262@0 {
        compatible = "semtech,sx1262";
        reg = <0>;
        spi-max-frequency = <1000000>;
        
        /* ESP32-S3 pin assignments */
        reset-gpios = <&gpio0 4 GPIO_ACTIVE_LOW>;
        busy-gpios = <&gpio0 5 GPIO_ACTIVE_HIGH>;
        dio1-gpios = <&gpio0 6 GPIO_ACTIVE_HIGH>;
        
        /* Power and RF control */
        tcxo-power-gpios = <&gpio0 7 GPIO_ACTIVE_HIGH>;
        rf-switch-rx-gpios = <&gpio0 8 GPIO_ACTIVE_HIGH>;
        rf-switch-tx-gpios = <&gpio0 9 GPIO_ACTIVE_HIGH>;
        
        status = "okay";
    };
};
```

### 4. Simple Application

```c
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/heymac.h>
#include <zephyr/net/aether_mesh.h>

int main(void)
{
    struct net_if *iface;
    
    /* Wait for network interface */
    iface = net_if_get_default();
    while (!net_if_is_up(iface)) {
        k_sleep(K_MSEC(100));
    }
    
    /* Initialize Æther mesh */
    aether_mesh_init(iface, 0x915E);
    
    /* Your application logic */
    while (1) {
        aether_mesh_send_hello(iface);
        k_sleep(K_SECONDS(30));
    }
    
    return 0;
}
```

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

### ESP32-S3 Integration
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

## Configuration Options

### HeyMac Layer 2
- `CONFIG_NET_L2_HEYMAC`: Enable HeyMac protocol
- `CONFIG_HEYMAC_DEFAULT_NET_ID`: Default network ID (0x915E)
- `CONFIG_HEYMAC_USE_SHORT_ADDR`: Use 2-byte addresses (default: y)
- `CONFIG_HEYMAC_MAX_FRAME_SIZE`: Maximum frame size (default: 255)

### Æther Mesh
- `CONFIG_AETHER_MESH`: Enable mesh networking
- `CONFIG_AETHER_MAX_ROUTES`: Routing table size (4-64, default: 16)
- `CONFIG_AETHER_MAX_NEIGHBORS`: Neighbor table size (4-32, default: 8)
- `CONFIG_AETHER_DEFAULT_TTL`: Default TTL (1-15, default: 6)
- `CONFIG_AETHER_HELLO_INTERVAL`: Hello interval in seconds (10-300, default: 30)
- `CONFIG_AETHER_CONDITIONAL_NET_ID`: Network ID optimization (default: y)

### LoRa Driver
- `CONFIG_AETHER_LORA`: Enable LoRa integration
- `CONFIG_AETHER_RX_STACK_SIZE`: RX thread stack (default: 2048/3072 ESP32)
- `CONFIG_AETHER_TX_STACK_SIZE`: TX thread stack (default: 1536/2048 ESP32)
- `CONFIG_AETHER_LORA_SF`: Spreading factor (7-12, default: 7)
- `CONFIG_AETHER_LORA_TX_POWER`: TX power in dBm (0-22, default: 14)

## Shell Commands

When `CONFIG_AETHER_SHELL=y`:

```bash
# Show node status
aether status

# Send hello message
aether hello

# Send mesh message
aether send 01:02:03:04:05:06 "Hello World"
```

## Architecture

### Protocol Layers
1. **LoRa PHY**: SX1262 radio (915MHz, SF7, 125kHz BW, 4/5 CR)
2. **HeyMac L2**: Frame format, addressing, sequence numbers
3. **Æther Mesh**: Multi-hop routing, neighbor discovery
4. **Application**: User protocols (ÆtherStyx, custom)

### Threading Model
- **RX Thread**: High priority (2), processes incoming packets
- **TX Thread**: Medium priority (3), handles outgoing packets  
- **Mesh Work Queue**: Cooperative threads for hello/maintenance
- **Application**: Main thread for user logic

### Memory Usage
- HeyMac context: ~100 bytes per interface
- Neighbor table: 32 bytes × max neighbors
- Route table: 32 bytes × max routes
- Packet buffers: Configurable via Zephyr network stack

## Protocol Specifications

This implementation follows the Æther Protocol Specification v1.2, including:

- HeyMac frame format with conditional fields
- Æther mesh header optimization (16-18 bytes variable)
- Network ID optimization for 12.5% bandwidth savings
- ÆtherStyx application protocol support

## Development Guidelines

### Adding Features
1. Follow Zephyr coding standards and conventions
2. All Kconfig options must have help text and sensible defaults
3. Use existing logging infrastructure (`LOG_MODULE_REGISTER`)
4. Thread stack sizes must account for ESP32-S3 requirements

### File Organization
- Public APIs: `include/zephyr/net/`
- HeyMac L2: `subsys/net/l2/heymac/`
- Æther mesh: `subsys/net/aether/`
- LoRa driver: `drivers/lora/`

### Testing
- Unit tests: `tests/subsys/`
- Integration tests: `tests/integration/`
- Sample applications: `samples/`

## Acknowledgments

The HeyMac protocol was designed and specified by **Dean Hall**. This implementation is based on his original work:

- [HeyMac Protocol Specification](https://github.com/dwhall/HeyMac) - Original protocol design and Python reference implementation
- HeyMac provides the Layer 2 foundation that enables Æther's mesh networking capabilities

Thanks to Dean for creating an elegant, LoRa-optimized MAC protocol and making it available to the community.

## License

SPDX-License-Identifier: MIT

## Contributing

Please follow Zephyr project conventions and submit pull requests for review.

## Support

- Issues: GitHub issue tracker
- Documentation: See CLAUDE.md for development guidance
- Protocol specification: AETHER_PROTOCOL_SPEC.md