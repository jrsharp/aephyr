/*
 * Æther service discovery — beacon TLV codec + known-peer table.
 *
 * Discovery is not a subsystem: the HELLO beacon carries a small
 * advertisement (node name + symbolic service names, never addresses or
 * ports), a bounded static table remembers who was heard, and everything
 * above (namespace, ndb, cs) renders that table.  Liveness is discovered
 * at attach time; `lastseen` is advisory aging only.
 *
 * Copyright (c) 2026 Æther Authors
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ZEPHYR_INCLUDE_NET_AETHER_SVCDISC_H_
#define ZEPHYR_INCLUDE_NET_AETHER_SVCDISC_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
	AETHER_SVC_NAME_MAX = 15,   /* symbolic names stay short */
	AETHER_SVC_MAX      = 4,    /* services advertised per node; keeps the
				     * worst-case advertisement at 81 B within
				     * the 128 B mesh frame budget */
};

#ifdef CONFIG_AETHER_SVCDISC_MAX_PEERS
#define AETHER_SVCDISC_MAX_PEERS CONFIG_AETHER_SVCDISC_MAX_PEERS
#else
#define AETHER_SVCDISC_MAX_PEERS 32
#endif

/* Never evict a table entry heard within this window, so a beacon burst
 * from unknown nodes cannot flush live peers.  If every entry is recent
 * and the table is full, the newcomer is dropped instead. */
#define AETHER_SVCDISC_EVICT_GUARD_MS (30 * 1000)

/* Advertisement wire format (HELLO payload):
 *   [namelen:u8] [name:namelen] [nsvc:u8] ( [svclen:u8] [svc:svclen] )*
 * namelen 0 = unnamed node.  No NULs on the wire; exact length — trailing
 * bytes are malformed.  Worst case 1+15+1+4*(1+15) = 81 bytes. */
#define AETHER_SVCADV_MAX \
	(1 + AETHER_SVC_NAME_MAX + 1 + AETHER_SVC_MAX * (1 + AETHER_SVC_NAME_MAX))

/* One service name, fixed storage, NUL-terminated in RAM. */
struct aether_svc {
	char name[AETHER_SVC_NAME_MAX + 1];
};

/* A known peer.  Keyed by addr; name is a self-asserted display label
 * (may collide — lookups by name surface ambiguity, never pick). */
struct aether_peer {
	uint8_t           addr[6];
	char              name[AETHER_SVC_NAME_MAX + 1];  /* "" if unnamed */
	struct aether_svc svc[AETHER_SVC_MAX];
	uint8_t           nsvc;
	uint32_t          lastseen;   /* uptime ms of last beacon */
	int8_t            rssi;       /* last-heard signal, advisory only */
	uint8_t           used;
};

/* Names become 9P path components and dial-string atoms: only
 * [a-z0-9-], first char alphanumeric, 1..AETHER_SVC_NAME_MAX chars. */
bool aether_svc_name_ok(const char *name);

/* Encode an advertisement into buf.  name may be NULL/"" for unnamed.
 * Returns bytes written, or -EMSGSIZE / -EINVAL (bad name or nsvc). */
int aether_svcadv_encode(uint8_t *buf, size_t cap, const char *name,
			 const struct aether_svc *svc, uint8_t nsvc);

/* Decode a received advertisement into peer (name/svc/nsvc only; addr,
 * rssi, lastseen are the caller's, from the beacon header).  Reject,
 * don't repair: any bound, charset, or trailing-byte violation returns
 * -EBADMSG and the beacon is dropped, never truncated into a valid one. */
int aether_svcadv_decode(const uint8_t *buf, size_t len,
			 struct aether_peer *peer);

/* ---- peer table (static, bounded, aging) ---- */

/* Allocate the table (PSRAM when available, else heap) and reset state.
 * Idempotent.  Called from aether_mesh_init. */
int aether_svcdisc_init(void);

/* Insert or refresh a peer, keyed by addr.  seen->lastseen is "now".
 * O(n) scan; no allocation.  Evicts the oldest entry when full — but
 * never one inside the eviction guard window. */
void aether_peer_observe(const struct aether_peer *seen);

/* Age out peers not heard from within max_age_ms.  Called periodically
 * (rides the mesh route-maintenance tick). */
void aether_peer_age(uint32_t now, uint32_t max_age_ms);

/* Copy-out lookups (never a pointer into the table: writers are the mesh
 * RX path, readers the 9P server and dial resolution — entries are
 * copied under the lock, which is never held across a render or dial).
 * Return 0 and fill *out, or -ENOENT. */
int aether_peer_get_by_addr(const uint8_t addr[6], struct aether_peer *out);

/* By-name lookup: returns the number of live addrs claiming the name.
 * *out is filled only on exactly 1.  More than one is an error the
 * caller must surface (dial refuses; the ndb feeder emits no tuple) —
 * never a last-beacon-wins pick. */
int aether_peer_get_by_name(const char *name, struct aether_peer *out);

/* idx'th used entry (compacted view, for directory listing).  -ENOENT
 * past the end. */
int aether_peer_get_at(size_t idx, struct aether_peer *out);

/* ---- self (what this node advertises) ---- */

/* Set this node's beacon name ("" or NULL to clear).  -EINVAL if not a
 * valid name.  Callable before init (boot settings load precedes mesh
 * bringup): the name parks in a pending buffer and init applies it. */
int aether_svcdisc_set_name(const char *name);

/* Replace the advertised service list.  The list is derived from what
 * is actually served (the srv registry on FRST) — advertised-but-
 * unserved must not be expressible; callers pass registered anames.
 * -EINVAL on a bad name or nsvc > AETHER_SVC_MAX. */
int aether_svcdisc_set_services(const struct aether_svc *svc, uint8_t nsvc);

/* Snapshot own name + services (addr/rssi/lastseen zeroed), for the
 * namespace's self/ files. */
void aether_svcdisc_self(struct aether_peer *out);

/* Encode the current self advertisement for the outgoing HELLO.
 * Returns bytes written (0 when nothing to advertise and unnamed), or
 * a negative encode error. */
int aether_svcdisc_build_adv(uint8_t *buf, size_t cap);

/* Feed a received HELLO into the table.  len 0 = legacy beacon with no
 * advertisement: the peer is still observed (addr/lastseen/rssi), just
 * nameless and serviceless.  A malformed advertisement drops the whole
 * observation. */
void aether_svcdisc_ingest(const uint8_t src[6], int8_t rssi,
			   const uint8_t *payload, size_t len, uint32_t now);

/* Same, from a (possibly fragmented) net_buf chain at offset — the mesh
 * RX path's form.  Linearizes into internal scratch (single writer: RX
 * is serialized on the mesh workq). */
struct net_buf;
void aether_svcdisc_ingest_frags(const uint8_t src[6], int8_t rssi,
				 struct net_buf *frags, size_t offset,
				 size_t len, uint32_t now);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_NET_AETHER_SVCDISC_H_ */
