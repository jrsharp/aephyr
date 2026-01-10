# Aether Multinode Integration Test

Test Aether mesh routing across multiple QEMU instances connected via virtual UARTs.

## Overview

This test creates a topology where multiple Zephyr instances communicate over
simulated UART links, allowing you to test:

- Aether mesh packet routing
- Neighbor discovery
- Multi-hop message forwarding
- Protocol interoperability

## Topology

```
2-node:  [Node A] <--UART--> [Node B]

3-node:  [Node A] <--UART--> [Node B] <--UART--> [Node C]
         (A and C cannot directly communicate - must route through B)
```

## Quick Start

```bash
# From this directory
./run-multinode.sh 2

# Or specify number of nodes
./run-multinode.sh 3
```

## Manual Setup

If the script doesn't work on your system, run manually:

### Terminal 1 (Node A - server):
```bash
west build -p auto -b qemu_x86 .

qemu-system-i386 \
    -cpu qemu32,+nx,+pae \
    -machine q35 \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -no-reboot -nographic \
    -kernel build/zephyr/zephyr.elf \
    -serial mon:stdio \
    -serial unix:/tmp/aether-ab.sock,server,nowait
```

### Terminal 2 (Node B - client):
```bash
qemu-system-i386 \
    -cpu qemu32,+nx,+pae \
    -machine q35 \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -no-reboot -nographic \
    -kernel build/zephyr/zephyr.elf \
    -serial mon:stdio \
    -serial unix:/tmp/aether-ab.sock
```

## Shell Commands

Once running, use the shell to interact:

```
mesh status          # Show node info and statistics
mesh send "hello"    # Send a message to peer
mesh peer xx:xx:xx:xx:xx:xx   # Set destination address
```

## Expected Output

You should see:
1. Each node announces its address on startup
2. Periodic "Hello from node-xxxx" messages appear on both nodes
3. Messages received show source address

Example:
```
[node-a1b2] << Hello from node-c3d4 #1 (from c3:d4)
```

## Troubleshooting

- **No output on uart1**: Check that the devicetree overlay is applied
- **Connection refused**: Start the server node (first terminal) before the client
- **No messages received**: Verify both nodes initialized successfully
