# Setting Up West Workspace for Æther Module Development

## Overview

The Æther stack is a Zephyr module. To use it, you need to create a West workspace that includes both Zephyr and the Æther module. There are two approaches:

## Option 1: Local Development Workspace (Recommended)

Create a new workspace directory and initialize it:

```bash
# Create workspace directory (outside of aephyr)
mkdir -p ~/zephyr-aether-workspace
cd ~/zephyr-aether-workspace

# Create west.yml manifest
cat > west.yml << 'EOF'
manifest:
  remotes:
    - name: zephyrproject-rtos
      url-base: https://github.com/zephyrproject-rtos
    - name: local
      url-base: file:///Users/jrsharp/src

  projects:
    - name: zephyr
      remote: zephyrproject-rtos
      revision: v3.7.0  # or main for latest
      import:
        name-allowlist:
          - hal_espressif
          - mbedtls
          - mcuboot

    - name: aephyr
      remote: local
      revision: main
      path: modules/aether

  self:
    path: manifest-repo
EOF

# Initialize the workspace
west init -l .
west update

# Install Python dependencies
pip3 install -r zephyr/scripts/requirements.txt
```

## Option 2: Using Aether Module from GitHub

Once you push your module to GitHub:

```bash
# Create workspace
mkdir -p ~/zephyr-aether-workspace
cd ~/zephyr-aether-workspace

# Create west.yml
cat > west.yml << 'EOF'
manifest:
  remotes:
    - name: zephyrproject-rtos
      url-base: https://github.com/zephyrproject-rtos
    - name: your-github
      url-base: https://github.com/your-username

  projects:
    - name: zephyr
      remote: zephyrproject-rtos
      revision: v3.7.0
      import: true

    - name: aephyr
      remote: your-github
      revision: main
      path: modules/aether

  self:
    path: manifest-repo
EOF

west init -l .
west update
```

## Option 3: T2 Star Topology (Alternative)

For existing Zephyr workspaces, add Aether as an external module:

```bash
# In existing workspace root
cd ~/zephyrproject

# Clone the module
git clone /Users/jrsharp/src/aephyr modules/aether

# Or add to west.yml in your manifest repository
```

## Building with the Module

Once your workspace is set up:

```bash
# Export Zephyr CMake package
west zephyr-export

# Build a sample application
cd ~/zephyr-aether-workspace
west build -p auto -b esp32s3_devkitc modules/aether/samples/aether_basic

# Flash to device
west flash

# Monitor output
west espressif monitor
```

## Environment Setup

Add to your shell profile (~/.zshrc or ~/.bashrc):

```bash
# Zephyr SDK path (adjust for your system)
export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-0.16.8

# ESP-IDF tools (for ESP32 support)
export IDF_PATH=~/zephyrproject/modules/hal/espressif/esp-idf
export ESP_IDF_PATH=$IDF_PATH
```

## Verifying Module Integration

Check that the module is recognized:

```bash
# List all modules
west list

# Should show:
# ...
# modules/aether    /path/to/workspace/modules/aether
# ...

# Build with verbose output to see module being included
west build -v -p auto -b native_sim modules/aether/samples/aether_basic -- -DCONFIG_AETHER_SHELL=y
```

## Module Structure Requirements

Your module (already configured correctly) needs:

1. ✅ `zephyr/module.yml` - Module metadata
2. ✅ `CMakeLists.txt` - Root CMake configuration  
3. ✅ `Kconfig` - Configuration options
4. ✅ Module code in proper directories

## Common Issues

### Module Not Found
- Ensure `west update` was run after adding the module
- Check that paths in west.yml are correct
- Verify module.yml exists in zephyr/ subdirectory

### Build Errors
- Run `west zephyr-export` after west update
- Check Zephyr SDK is installed and ZEPHYR_SDK_INSTALL_DIR is set
- For ESP32: Install ESP-IDF tools with `west espressif install`

### Version Conflicts
- Use specific Zephyr release tags (e.g., v3.7.0) instead of main
- Ensure module Kconfig doesn't conflict with Zephyr's

## Development Workflow

1. Edit code in `/Users/jrsharp/src/aephyr`
2. Build from workspace root: `west build -p auto -b <board> modules/aether/samples/<sample>`
3. Changes are immediately reflected (no need to update)
4. Commit changes in the module directory
5. Push to GitHub when ready to share