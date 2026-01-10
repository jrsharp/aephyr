#!/bin/bash
#
# Run multiple QEMU instances with connected virtual UARTs
#
# This script launches 2 or 3 QEMU instances with their secondary UARTs
# connected via Unix sockets, allowing Aether mesh communication.
#
# Usage:
#   ./run-multinode.sh [2|3]    # Number of nodes (default: 2)
#
# Topology:
#   2 nodes:  A <---> B
#   3 nodes:  A <---> B <---> C
#
# The script will:
#   1. Build the test application
#   2. Create Unix socket connections
#   3. Launch QEMU instances in separate terminals
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
AEPHYR_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
NUM_NODES="${1:-2}"

# Workspace location (adjust as needed)
WORKSPACE="${ZEPHYR_WORKSPACE:-$HOME/zephyr-aether-workspace}"
BUILD_DIR="$WORKSPACE/build"

# Socket directory
SOCK_DIR="/tmp/aether-test-$$"

cleanup() {
    echo "Cleaning up..."
    rm -rf "$SOCK_DIR"
    # Kill any lingering QEMU processes from this test
    pkill -f "aether-node-" 2>/dev/null || true
}

trap cleanup EXIT

echo "=========================================="
echo "Aether Multinode Test"
echo "Nodes: $NUM_NODES"
echo "=========================================="

# Create socket directory
mkdir -p "$SOCK_DIR"

# Build the test application
echo ""
echo "Building test application..."
cd "$WORKSPACE"
source .venv/bin/activate 2>/dev/null || true

west build -p auto -b qemu_x86 "$SCRIPT_DIR" -- \
    -DOVERLAY_CONFIG="$SCRIPT_DIR/prj.conf"

if [ $? -ne 0 ]; then
    echo "Build failed!"
    exit 1
fi

echo ""
echo "Build successful!"
echo ""

# Get the QEMU binary and base arguments
QEMU_BIN="${QEMU:-qemu-system-i386}"
ZEPHYR_ELF="$BUILD_DIR/zephyr/zephyr.elf"

# Function to launch a node
launch_node() {
    local node_id=$1
    local uart_args=$2
    local node_name="aether-node-$node_id"

    echo "Launching $node_name..."

    # QEMU arguments
    # - First serial (uart0) goes to console
    # - Second serial (uart1) is for mesh communication
    $QEMU_BIN \
        -cpu qemu32,+nx,+pae \
        -machine q35 \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
        -no-reboot \
        -nographic \
        -kernel "$ZEPHYR_ELF" \
        -serial mon:stdio \
        $uart_args \
        -name "$node_name" &
}

# Create socket pairs and launch nodes based on topology
case $NUM_NODES in
    2)
        # A <---> B
        # Node A: uart1 server on socket
        # Node B: uart1 client on same socket
        SOCK_AB="$SOCK_DIR/ab.sock"

        echo "Creating socket: $SOCK_AB"

        # Launch in separate terminal windows if available
        if command -v gnome-terminal &>/dev/null; then
            gnome-terminal --title="Node A" -- bash -c "$QEMU_BIN -cpu qemu32,+nx,+pae -machine q35 -device isa-debug-exit,iobase=0xf4,iosize=0x04 -no-reboot -nographic -kernel '$ZEPHYR_ELF' -serial mon:stdio -serial unix:$SOCK_AB,server,nowait; read"
            sleep 1
            gnome-terminal --title="Node B" -- bash -c "$QEMU_BIN -cpu qemu32,+nx,+pae -machine q35 -device isa-debug-exit,iobase=0xf4,iosize=0x04 -no-reboot -nographic -kernel '$ZEPHYR_ELF' -serial mon:stdio -serial unix:$SOCK_AB; read"
        elif command -v xterm &>/dev/null; then
            xterm -title "Node A" -e "$QEMU_BIN -cpu qemu32,+nx,+pae -machine q35 -device isa-debug-exit,iobase=0xf4,iosize=0x04 -no-reboot -nographic -kernel '$ZEPHYR_ELF' -serial mon:stdio -serial unix:$SOCK_AB,server,nowait" &
            sleep 1
            xterm -title "Node B" -e "$QEMU_BIN -cpu qemu32,+nx,+pae -machine q35 -device isa-debug-exit,iobase=0xf4,iosize=0x04 -no-reboot -nographic -kernel '$ZEPHYR_ELF' -serial mon:stdio -serial unix:$SOCK_AB" &
        else
            # No terminal available - run in foreground with instructions
            echo ""
            echo "No terminal emulator found (xterm or gnome-terminal)."
            echo "Run these commands in separate terminals:"
            echo ""
            echo "Terminal 1 (Node A):"
            echo "  $QEMU_BIN -cpu qemu32,+nx,+pae -machine q35 -device isa-debug-exit,iobase=0xf4,iosize=0x04 -no-reboot -nographic -kernel '$ZEPHYR_ELF' -serial mon:stdio -serial unix:$SOCK_AB,server,nowait"
            echo ""
            echo "Terminal 2 (Node B):"
            echo "  $QEMU_BIN -cpu qemu32,+nx,+pae -machine q35 -device isa-debug-exit,iobase=0xf4,iosize=0x04 -no-reboot -nographic -kernel '$ZEPHYR_ELF' -serial mon:stdio -serial unix:$SOCK_AB"
            echo ""
            echo "Press Ctrl+C to exit and cleanup sockets."

            # Keep script running to maintain socket directory
            while true; do sleep 1; done
        fi
        ;;

    3)
        # A <---> B <---> C
        SOCK_AB="$SOCK_DIR/ab.sock"
        SOCK_BC="$SOCK_DIR/bc.sock"

        echo "Creating sockets: $SOCK_AB, $SOCK_BC"
        echo ""
        echo "For 3-node topology, run these commands in separate terminals:"
        echo ""
        echo "Terminal 1 (Node A - connected to B):"
        echo "  $QEMU_BIN ... -serial unix:$SOCK_AB,server,nowait"
        echo ""
        echo "Terminal 2 (Node B - connected to A and C):"
        echo "  # Note: B needs 2 UART connections - requires custom board config"
        echo ""
        echo "Terminal 3 (Node C - connected to B):"
        echo "  $QEMU_BIN ... -serial unix:$SOCK_BC"
        echo ""
        echo "3-node topology requires board support for multiple UARTs."
        echo "Start with 2-node test first."
        ;;

    *)
        echo "Invalid number of nodes: $NUM_NODES"
        echo "Usage: $0 [2|3]"
        exit 1
        ;;
esac

echo ""
echo "Test nodes launched. Press Ctrl+C to stop."
echo ""

# Wait for user interrupt
wait
