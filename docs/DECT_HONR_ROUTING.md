# HONR16 tree routing on DECT NR+ (design)

Status: **design draft.** The core address math is implemented and unit-testable
in `include/zephyr/net/honr.h`; the join protocol, route integration, and DECT
hardware-address hookup described here are not yet wired.

## Why

Æther today assigns **random** short addresses (`heymac.c`) and routes
**reactively** (AODV-style RREQ/RREP + a next-hop table, `aether_route.c`).
HONR16 — Hierarchically Ordered Numbers for Routing, from Dean Hall's HeyMac —
replaces both with a structured scheme where the **address encodes the node's
place in the tree**, so:

- **Joining is local:** a node attaches to one parent and is handed a child
  address. No global topology discovery, no RREQ flood.
- **Routing is stateless:** forward up to the nearest common ancestor (NCA) of
  resender and destination, then down — each node computes the single next hop
  from `(resender, dst)` with no tables.

This pairs unusually well with DECT NR+ (see "DECT hardware addressing" below),
which is the main reason to do it on this radio rather than LoRa.

## Address format

16 bits = four 4-bit nibbles, MSB first, one per tree level:

| nibble value | meaning              |
| ------------ | -------------------- |
| `0`          | no child / end of path |
| `1`..`14`    | child index (14 children max) |
| `15` (`0xF`) | child-broadcast (reserved) |

Root = `0x0000`. Max depth = 4 levels, max fan-out = 14. Validity: every nibble
right of the left-most zero must also be zero (a left-justified path).

Primitives (`honr.h`, all pure/inline, testable on `native_sim`):

| function | meaning |
| --- | --- |
| `honr_rank(a)` | depth = index of left-most zero nibble (0 = root) |
| `honr_parent(a)` | clear least-significant non-zero nibble |
| `honr_nca(a, b)` | shared left-to-right nibble prefix |
| `honr_is_ancestor(anc, node)` | `nca(anc,node) == anc` |
| `honr_next_hop(resender, dst)` | next node toward `dst`; relay iff it equals self |
| `honr_should_relay(self, resender, dst)` | convenience wrapper |

Worked example (`0xC59A → 0xC232`, NCA `0xC000`):

```
C59A -> C590 -> C500 -> C000 -> C200 -> C230 -> C232
        \___ up to NCA ___/     \___ down to dst ___/
```

`honr_next_hop` reproduces each step: while the resender is not an ancestor of
`dst` it returns `honr_parent(resender)` (ascend); once it is, it sets the
nibble at `rank(resender)` to the destination's nibble at that level (descend).

## Join / address assignment

Reuse the existing HeyMac discovery HSM (`LURKING → BEACONING → LINKING`) as the
substrate; HONR rides on top:

1. **Beacon advertises** the sender's HONR address and a bitmap of free child
   nibbles (`1..14`).
2. A joining node **LURKS**, hears candidate parents, and picks one (e.g. best
   RSSI / lowest rank).
3. It requests attachment; the parent **allocates the next free child nibble**,
   forming the child address = `honr_set_nibble(parent, rank(parent), nibble)`,
   and (per HeyMac) reports the new node's long+short address toward the root.
4. The child adopts the address, sets it as its DECT identity (below), and
   begins beaconing its own free-child bitmap.

Address management is fully local to each parent ("every node manages the
address space of its children without consulting the root").

## Route integration

A new `aether_route` path selected by Kconfig:

```
choice AETHER_ROUTING
	default AETHER_ROUTING_AODV
config AETHER_ROUTING_AODV   # existing reactive RREQ/RREP (unchanged)
config AETHER_ROUTING_HONR   # stateless HONR tree routing
endchoice
```

Under `AETHER_ROUTING_HONR`, the forward decision becomes table-free:

```c
/* on receiving a frame with header {resender, dst}: */
if (dst == self || dst == HONR_BCAST /* or child-broadcast for my subtree */) {
        deliver();
}
if (honr_should_relay(self, resender, dst)) {
        hdr.resender = self;          /* become the new resender */
        radio_tx(frame);              /* single next hop; no flooding */
}
```

The frame carries `resender` (last hop) and `dst`; the source address is not
needed to forward. Keeping AODV as the default means the proven LoRa path is
untouched and HONR is opt-in per build.

## DECT hardware addressing (the payoff)

A HONR address is 16 bits — exactly the width of DECT NR+'s PHY
`transmitter_id` and the RX `receiver_identity` filter. That lets the **modem
filter by tree address in hardware**:

- Each node arms RX with `filter.receiver_identity = own_honr_addr`. The modem
  accepts only frames addressed to it (or the broadcast ID) — the CPU never
  wakes for other traffic. (Today `dect_phy.c` sets `receiver_identity = 0`,
  i.e. promiscuous, and uses the hardware ID as `transmitter_id`.)
- On TX, set `transmitter_id = own_honr_addr` and the frame's receiver field to
  `honr_next_hop(self, dst)` for unicast tree forwarding, or the broadcast ID
  for party-line/flood. Forwarding becomes hardware-addressed unicast hops up
  and down the tree, rather than promiscuous receive + software filtering.
- HONR's `0xF` child-broadcast nibble maps onto DECT's broadcast receiver ID.

This is the LoRa-impossible win: LoRa has no PHY-level addressing, so it must
receive everything and filter in software. It also mirrors DECT's native FT/PT
(parent/child) structure.

**Resolved (step 4):** the **Type 2** PHY control header carries the receiver
identity (`receiver_id_hi/lo`) alongside the transmitter ID; `tx_params.phy_type`
selects it (0 = Type 1 broadcast, 1 = Type 2 addressed). In HONR mode the driver
builds Type 2 frames, with the receiver id set to the next hop (unicast) or the
broadcast id (`0xFFFF`), and arms the RX filter to the node's HONR address. The
per-hop next hop is conveyed from the mesh layer to the driver via
`net_pkt_lladdr_dst` (2 bytes), which is stored by value in the packet and so
survives the transmit queue.

## Trade-offs

- **Bounds:** 4 levels deep × 14 children. Fine for small structured networks
  (e.g. three Thingy:91 X nodes); size depth × fan-out up front.
- **Mobility/robustness:** tree routing gives minimal state but suboptimal paths
  (always via a common ancestor) and is fragile to parent loss (a moved/lost
  parent forces subtree re-addressing). AODV is more resilient under churn — a
  hybrid (HONR default, reactive fallback for broken/cross-branch links) is the
  natural evolution.

## Implementation steps

1. (done) `honr.h` core math, verified against HeyMac's worked example.
2. (done) `AETHER_ROUTING` Kconfig choice (AODV default / HONR); stateless HONR
   forward path in `aether_honr.c`, wired into `aether_mesh_send` (no route
   discovery) and the `aether_mesh_recv` forward decision (relay only if this
   node is the HONR next hop for the frame's resender → destination). The
   resender is the immediate L2 sender, stashed by HeyMac as
   `heymac_context.last_rx_src`. Both modes build for nrf9151/ns.
3. (done) Parent-attach join: addressed nodes periodically broadcast a
   TREE_ANNOUNCE (HONR address + free-child bitmap); an unaddressed node picks a
   parent and sends TREE_JOIN_REQ (its id + chosen parent); the parent allocates
   the next free child nibble and replies TREE_JOIN_REP with the child address.
   Root is `CONFIG_AETHER_HONR_ROOT` (address 0x0000). Implemented in
   `aether_honr.c`; `aether info` shows the assigned HONR address and join state.
   (Single mesh instance; no slot/clock sync yet.)
4. (done) DECT identity hookup: addressed Type 2 PHY header with the HONR
   address as transmitter id and the next hop as receiver id; RX hardware filter
   armed to the node's HONR address (0/promiscuous while unjoined or root). Next
   hop conveyed mesh→driver via `net_pkt_lladdr_dst`. Builds in both modes.
   Hardware-untested.
5. Bring-up on three nodes: root + two children, verify up/down delivery and
   hardware-filtered hops via `aether status` / `dect stats`. (Needs the DECT
   NR+ PHY modem firmware and boards.)
