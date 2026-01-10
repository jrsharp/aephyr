#!/bin/bash
# Quick setup script for Zephyr + Aether workspace

echo "Setting up Zephyr + Aether workspace..."

# Initialize west using the local manifest
echo "Initializing west..."
west init -l .

# Update all modules (this will clone Zephyr and your Aether module)
echo "Updating modules (this may take a while)..."
west update

# After update, replace the cloned aether with a symlink for development
echo "Setting up development symlink for Aether module..."
if [ -d "modules/aether" ]; then
    rm -rf modules/aether
fi
ln -s /Users/jrsharp/src/aephyr modules/aether

# Export Zephyr CMake package
echo "Exporting Zephyr CMake package..."
west zephyr-export

# Install Python dependencies if needed
echo "Checking Python dependencies..."
pip3 install -r zephyr/scripts/requirements.txt

echo "Setup complete! You can now build with:"
echo "  west build -p auto -b esp32s3_devkitc modules/aether/samples/aether_basic"