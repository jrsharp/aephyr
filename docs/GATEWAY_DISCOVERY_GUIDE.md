# Æther Service Discovery + Canonical /net/aether — Gateway Integration Guide

**Audience:** the DECT-NR+ modem/gateway firmware (and any other non-deck
Æther node).  Written 2026-07-23 against aephyr `main` and FRST
`release/beta`; the design brief is FRST `docs/aether-discovery-brief.md`.

## What changed in aephyr

Two things landed; both matter to the gateway:

1. **Service discovery** (`CONFIG_AETHER_SVCDISC`, default y):
   the mesh HELLO (0x05) may now carry a service-advertisement payload
   (node name + symbolic service names), and every node keeps a bounded
   addr-keyed table of heard peers.  Spec: `AETHER_PROTOCOL_SPEC.md`
   §4.4.1.  API: `<zephyr/net/aether_svcdisc.h>`.
2. **The /net/aether 9P fs is now IN AEPHYR** —
   `subsys/net/aether/aether_net_fs.c` (`CONFIG_AETHER_NET_FS`).  This is
   FRST's battle-tested `src/dev_aether_net.c`, relocated verbatim (only
   the header path, log name, and a weak `aether_9p_exporting_now`
   default changed) so a single implementation serves deck, gateway, and
   sim.  **Honest scope note:** this relocation was done from the FRST
   side, where no gateway tree is visible — the FRST file was chosen as
   canonical because it is the deck-tested one, NOT because it was
   diffed against the gateway's copy.  No such comparison has happened
   yet.  **If the gateway tree carries its own /net/aether server, that
   diff is YOUR first task**: compare it against `aether_net_fs.c`
   before deleting anything, port any gateway-only behavior into this
   file (upstream it — do not re-fork), and check your copy for the
   semantics in "Reconciling a divergent copy" below.  Anything your
   copy does differently that a deck relies on is a bug on one side or
   the other; surface it rather than silently keeping both.

Both are PHY-blind by design: the TLV rides the mesh-layer HELLO above
HeyMac, and the fs sits above the reliable-datagram engine.  Nothing in
either knows about LoRa or DECT.

## Merging into the gateway

1. Update the aephyr revision.  `CONFIG_AETHER_SVCDISC` defaults on —
   the gateway starts beaconing (nothing to advertise yet = legacy empty
   HELLO) and building a peer table immediately.
2. Switch the gateway's 9P export to `CONFIG_AETHER_NET_FS=y` and delete
   any local /net/aether server source.  The consumer contract
   (header comment in `aether_net_fs.c`):
   - provide FRST's srv framework and allocator — `srv.h` and `mem.h`
     under `<app>/src` (the module CMake adds that include path).  If
     the gateway vendors FRST's `srv.c`/`mem.c`, that is already true;
   - 9p4z module present (`<zephyr/9p/protocol.h>`);
   - `aether_9p_exporting_now()` is a **weak** false default — override
     it only if the gateway grows an aexport-style namespace-export
     toggle that should advertise `9fs`.
3. Call `dev_aether_net_init()` after `aether_mesh_init()` (unchanged
   name, now declared in `<zephyr/net/aether_net_fs.h>`).
4. Set the gateway's beacon name once at bring-up:
   `aether_svcdisc_set_name("gw-<something>")` — safe to call **before**
   mesh init (it parks until init applies it).  Charset is strict:
   `[a-z0-9-]`, first char alphanumeric, ≤ 15 bytes; anything else is
   rejected (and receivers drop violating beacons entirely).

## What the deck sees through you

Deck-side `who`, `achat <name>`, and `ls /net/aether/peers` all operate
**through the namespace**, so when a deck binds the gateway's exported
/net/aether they act on the *gateway's* peer table and the *gateway's*
name resolution — no deck changes, no gateway-specific protocol.  Two
consequences to keep straight:

- **Resolution runs on the gateway.**  `echo connect <name> > ctl`
  resolves name→addr in `parse_peer()` gateway-side.  Unknown name →
  ENOENT; a name claimed by two live addrs → EADDRNOTAVAIL (never
  last-beacon-wins).  Do not "help" by picking one.
- **Identity is yours.**  Services announced through the gateway are
  advertised under the **gateway's** mesh address and CGA identity — the
  mesh sees the gateway as the serving node.  Fine for one deck behind
  one gateway; multi-client gateways will need per-client anames or
  name suffixes (open design point — flag it, don't improvise).

## Service advertisement rules (do not fork these)

- `self/services` is **derived, never authored**: a service is
  advertised iff it is actually served.  The single derivation point is
  `aether_net_readvertise()` — today: 9P export live ⇒ `9fs`, a live
  announced conversation ⇒ `chat`.  If the gateway serves something
  else, extend `readvertise()` (and keep it the only writer) rather
  than calling `aether_svcdisc_set_services()` from new places.
- Beacons carry symbolic names only — **no addresses, no ports**.  A
  service maps to an attach aname on ONE listener, selected at Tattach.
  Wire-aname convention: `9fs` = the whole exported namespace = **empty**
  aname (the 9p4z server walks a non-empty aname as a path from the
  export root); FRST's dialer normalizes this.  Other services keep
  their atom, and the server must serve a tree by that name.
- Advertisement is gossip; correctness lives in the attach (CGA check
  included).  No TTLs, no leases, no sequence numbers — do not add a
  consistency protocol to the beacon.

## DECT-specific adjustments (expected work on your side)

1. **RSSI source.**  HELLO ingest pulls signal strength from
   `aether_lora_get_stats()` under `#ifdef CONFIG_AETHER_LORA`; without
   it peers get the -50 placeholder, so roster signal bars lie.  Wire a
   DECT equivalent (last-RX RSSI is enough) into the HELLO case in
   `aether_mesh_recv()` — keep it a snapshot, not a per-packet plumb.
2. **Peer-table storage.**  `aether_svcdisc_init()` allocates ~3.3 KB
   (32 peers) from shared-multi-heap PSRAM, else `k_malloc`.  On a
   PSRAM-less MCU make sure `HEAP_MEM_POOL_SIZE` covers it, or shrink
   `CONFIG_AETHER_SVCDISC_MAX_PEERS`.
3. **Frame budget.**  Constants (4 services × 15 chars, worst-case 81 B
   payload) were sized for LoRa's 128 B first fragment.  DECT frames are
   larger, but the constants are shared wire format — leave them.
4. **HELLO cadence.**  Discovery freshness follows
   `CONFIG_AETHER_HELLO_INTERVAL` + stable-neighbor backoff.  DECT can
   afford a faster cadence than LoRa; tune the Kconfig, not the code.

## Reconciling a divergent /net/aether copy

If the gateway tree forked the fs before this relocation, diff against
`aether_net_fs.c` and preserve THESE semantics (they are load-bearing
and tested on the deck):

- `clone` open → slot; fid becomes `<N>/ctl`; ctl read → slot number.
- ctl verbs: `connect <addr-or-name>` / `announce` / `hangup`; unknown
  verb → EINVAL.  Slots persist after clone-fid clunk; `hangup` frees
  (and clears `announced` — a leak fixed 2026-07-23, check your copy).
- Announced reads are **source-prefixed** (6-byte addr), announced
  writes are **destination-prefixed**; connected conversations are
  neither.  Broadcast (connect ff:ff:..) is best-effort, source-prefixed
  reads, no fragmentation.
- One `data` read = one datagram; one write = one message (fragmented
  internally over the reliable engine, all-or-nothing).
- `peers/<addr>/` dirs are named by colon-form address (the stable
  key), never by node name; `lastseen` reads as seconds-since.
- Short writes / `ns_write` msize cap and the small-stack rules from
  FRST's NET_AETHER_SPEC.md still apply.

## Smoke test (deck ↔ gateway)

```
gateway:  boots, svcdisc on, name set          # beacons carry name
deck:     bind gateway /net/aether; who        # roster = gateway's table
deck:     achat <peer-name>                    # resolves gateway-side
mesh peer: aexport on                          # deck sees 9fs appear in
deck:     cat /net/aether/peers/<addr>/services  # ... that peer's list
```

Legacy interop: nodes without svcdisc still appear in `peers/` (bare
HELLO = nameless, serviceless entry) and old receivers ignore the new
payload — mixed-version meshes are safe.
