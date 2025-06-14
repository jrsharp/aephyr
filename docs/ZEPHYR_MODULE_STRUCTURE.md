# Æther Stack Zephyr Module Structure
*West module for HeyMac + Æther protocol stack*

---

## Repository Structure

```
aether-zephyr/
├── zephyr/
│   └── module.yml                 # Module definition
├── west.yml                       # West manifest (optional)
├── Kconfig                        # Module configuration options
├── CMakeLists.txt                 # Build system integration
├── subsys/
│   └── net/
│       ├── l2/
│       │   └── heymac/            # HeyMac L2 implementation
│       │       ├── Kconfig
│       │       ├── CMakeLists.txt
│       │       ├── heymac.h
│       │       └── heymac.c
│       └── aether/                # Æther mesh networking
│           ├── Kconfig
│           ├── CMakeLists.txt
│           ├── aether_mesh.h
│           ├── aether_mesh.c
│           ├── aether_route.c
│           └── aether_shell.c
├── drivers/
│   └── lora/
│       ├── Kconfig
│       ├── CMakeLists.txt
│       └── aether_lora.c          # LoRa integration driver
├── include/
│   └── zephyr/
│       └── net/
│           ├── heymac.h           # Public HeyMac API
│           └── aether_mesh.h      # Public Æther API
├── dts/
│   └── bindings/
│       └── net/
│           └── aether-mesh-net.yaml # Device tree binding
├── samples/
│   ├── aether_basic/              # Basic mesh node
│   ├── aether_gateway/            # WiFi-LoRa gateway
│   └── aether_sensor/             # Sensor node example
├── tests/
│   ├── subsys/
│   │   ├── heymac/               # HeyMac unit tests
│   │   └── aether/               # Æther mesh tests
│   └── integration/              # Integration tests
└── README.md
```

---

## Core Module Files

### 1. Module Definition (`zephyr/module.yml`)

```yaml
build:
  cmake: .
  kconfig: Kconfig
  settings:
    dts_root: .
    
name: aether
```

### 2. West Manifest (`west.yml`) - Optional

```yaml
# Optional: If you want this to be a west workspace
manifest:
  version: "0.13"
  
  defaults:
    remote: origin
    
  remotes:
    - name: origin
      url-base: https://github.com/your-org
      
  projects:
    - name: zephyr
      remote: zephyrproject-rtos
      url-base: https://github.com/zephyrproject-rtos
      revision: main
      import: true
```

### 3. Module Kconfig (`Kconfig`)

```kconfig
# SPDX-License-Identifier: Apache-2.0

if NET_L2_CUSTOM

config NET_L2_HEYMAC
	bool "HeyMac Layer 2 protocol support"
	depends on NET_L2_CUSTOM
	depends on LORA
	help
	  Enable HeyMac Layer 2 protocol for LoRa mesh networking.
	  HeyMac provides the MAC layer for Æther mesh networks.

if NET_L2_HEYMAC

config NET_L2_HEYMAC_LOG_LEVEL
	int "HeyMac log level"
	depends on LOG
	default 3
	range 0 4
	help
	  Set log level for HeyMac protocol.

config HEYMAC_DEFAULT_NET_ID
	hex "Default HeyMac network ID"
	default 0x1234
	help
	  Default network ID for HeyMac frames.

config HEYMAC_USE_SHORT_ADDR
	bool "Use short addressing by default"
	default y
	help
	  Use 2-byte short addresses instead of 6-byte long addresses.

endif # NET_L2_HEYMAC

config AETHER_MESH
	bool "Æther mesh networking support"
	depends on NET_L2_HEYMAC
	help
	  Enable Æther mesh networking protocol for multi-hop LoRa networks.

if AETHER_MESH

config AETHER_MESH_LOG_LEVEL
	int "Æther mesh log level"
	depends on LOG
	default 3
	range 0 4
	help
	  Set log level for Æther mesh protocol.

config AETHER_MAX_ROUTES
	int "Maximum number of routes in routing table"
	default 16
	range 4 64
	help
	  Maximum number of destination routes to maintain.

config AETHER_DEFAULT_TTL
	int "Default time-to-live for mesh packets"
	default 6
	range 1 15
	help
	  Default TTL value for originated packets.

config AETHER_HELLO_INTERVAL
	int "Hello message interval (seconds)"
	default 30
	range 10 300
	help
	  Interval between hello messages for neighbor discovery.

config AETHER_ROUTE_TIMEOUT
	int "Route timeout (seconds)"
	default 300
	range 60 3600
	help
	  Time before inactive routes are expired.

config AETHER_CONDITIONAL_NET_ID
	bool "Use conditional mesh network ID optimization"
	default y
	help
	  Enable conditional mesh network ID to save 2 bytes when mesh
	  network ID matches MAC network ID (12.5% header reduction).

config AETHER_SHELL
	bool "Æther shell commands"
	depends on SHELL
	help
	  Enable shell commands for Æther mesh debugging and control.

endif # AETHER_MESH

config AETHER_LORA
	bool "Æther LoRa driver integration"
	depends on LORA_SX126X
	help
	  LoRa driver integration for Æther mesh networking.

if AETHER_LORA

config AETHER_LORA_LOG_LEVEL
	int "Æther LoRa log level"
	depends on LOG
	default 3
	range 0 4

config AETHER_RX_QUEUE_SIZE
	int "RX message queue size"
	default 8
	range 4 32

config AETHER_TX_QUEUE_SIZE
	int "TX message queue size"
	default 4
	range 2 16

config AETHER_RX_STACK_SIZE
	int "RX thread stack size"
	default 2048
	range 1024 4096

config AETHER_TX_STACK_SIZE
	int "TX thread stack size"
	default 1536
	range 1024 4096

config AETHER_RX_THREAD_PRIO
	int "RX thread priority"
	default 2
	range 0 NUM_PREEMPT_PRIORITIES

config AETHER_TX_THREAD_PRIO
	int "TX thread priority"
	default 3
	range 0 NUM_PREEMPT_PRIORITIES

endif # AETHER_LORA

endif # NET_L2_CUSTOM
```

### 4. Module CMakeLists.txt

```cmake
# SPDX-License-Identifier: Apache-2.0

if(CONFIG_NET_L2_HEYMAC OR CONFIG_AETHER_MESH OR CONFIG_AETHER_LORA)
  zephyr_include_directories(include)
  add_subdirectory(subsys)
  
  if(CONFIG_AETHER_LORA)
    add_subdirectory(drivers)
  endif()
endif()
```

### 5. Device Tree Binding (`dts/bindings/net/aether-mesh-net.yaml`)

```yaml
# SPDX-License-Identifier: Apache-2.0

description: Æther Mesh Network Configuration

compatible: "aether-mesh-net"

properties:
  network-id:
    type: int
    description: HeyMac network identifier
    default: 0x1234

  mesh-network-id:
    type: int
    description: Æther mesh network identifier (optional)

  node-id:
    type: int
    description: Node identifier for this device

  mesh-enabled:
    type: boolean
    description: Enable mesh networking features

  max-hops:
    type: int
    description: Maximum hop count for mesh packets
    default: 6

  hello-interval:
    type: int
    description: Hello message interval in seconds
    default: 30

  route-timeout:
    type: int
    description: Route timeout in seconds
    default: 300

  sleep-enabled:
    type: boolean
    description: Enable power management features

  beacon-interval:
    type: int
    description: Beacon interval in milliseconds (for coordinators)
    default: 1000
```

---

## Integration Example

### Your ESP32-S3 Project Integration

**1. Add to your project's `west.yml`:**

```yaml
manifest:
  projects:
    - name: aether-zephyr
      url: https://github.com/your-org/aether-zephyr
      revision: main
      path: modules/aether
```

**2. Your ESP32-S3 project `prj.conf`:**

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

# Configure for ESP32-S3
CONFIG_AETHER_RX_STACK_SIZE=3072
CONFIG_AETHER_TX_STACK_SIZE=2048
CONFIG_MAIN_STACK_SIZE=4096

# Logging
CONFIG_LOG=y
CONFIG_NET_L2_HEYMAC_LOG_LEVEL=4
CONFIG_AETHER_MESH_LOG_LEVEL=4
```

**3. Your ESP32-S3 device tree overlay (`esp32s3_devkitc.overlay`):**

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

**4. Simple application using the Æther stack:**

```c
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/heymac.h>
#include <zephyr/net/aether_mesh.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(aether_app, LOG_LEVEL_INF);

int main(void)
{
    struct net_if *iface;
    
    LOG_INF("Æther ESP32-S3 Node Starting");
    
    /* Wait for network interface */
    iface = net_if_get_default();
    while (!net_if_is_up(iface)) {
        k_sleep(K_MSEC(100));
    }
    
    LOG_INF("Æther mesh network ready");
    
    /* Your application logic here */
    while (1) {
        /* Send periodic hello to neighbors */
        aether_send_hello(iface);
        k_sleep(K_SECONDS(30));
    }
    
    return 0;
}
```

---

## Development Workflow

### 1. Create Module Repository

```bash
# Create new repository for Æther module
git init aether-zephyr
cd aether-zephyr

# Add module structure
mkdir -p zephyr subsys/net/{l2/heymac,aether} drivers/lora include/zephyr/net
mkdir -p dts/bindings/net samples tests

# Implement files according to structure above
```

### 2. Add to Existing ESP32 Project

```bash
# In your ESP32-S3 project
west config manifest.group-filter "+aether"
west update

# Or manually add as submodule
git submodule add https://github.com/your-org/aether-zephyr modules/aether
```

### 3. Build and Test

```bash
# Build your ESP32-S3 project with Æther support
west build -p auto -b esp32s3_devkitc -- -DEXTRA_CONF_FILE=aether.conf

# Flash and test
west flash
```

---

## Module Benefits

✅ **Independent Development**: Maintain Æther stack separately  
✅ **Reusable Component**: Use across multiple projects  
✅ **Version Control**: Tag releases for stability  
✅ **Easy Integration**: Simple west.yml addition  
✅ **Proper Zephyr Integration**: Follows all Zephyr module conventions  
✅ **ESP32-S3 Optimized**: Thread stack sizes and configurations tuned for ESP32  

This structure gives you a professional, maintainable Zephyr module that integrates seamlessly with your existing ESP32-S3 projects while preserving all the protocol optimizations from your Plan 9 implementation! 