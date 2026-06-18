/*
 * Copyright (c) 2026 Æther Authors
 *
 * SPDX-License-Identifier: MIT
 *
 * HONR16 structured tree routing for the Æther mesh. Maps the 16-bit HONR
 * address into the low two bytes of the 6-byte mesh address and uses the pure
 * address math in <zephyr/net/honr.h> to make stateless forwarding decisions:
 * up to the nearest common ancestor of resender and destination, then down.
 *
 * Selected by CONFIG_AETHER_ROUTING_HONR. Requires HONR address assignment
 * (tree join, a later step) for the addresses to form a valid tree.
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(aether_honr, CONFIG_AETHER_MESH_LOG_LEVEL);

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net/aether_mesh.h>
#include <zephyr/net/honr.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/random/random.h>
#include <string.h>
#include <errno.h>

extern struct aether_mesh_ctx *g_mesh_ctx;

uint16_t aether_honr_addr16(const uint8_t addr6[6])
{
	return (uint16_t)((addr6[4] << 8) | addr6[5]);
}

int aether_honr_next_hop(struct net_if *iface, const uint8_t dst[6], uint8_t next_hop[6])
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;
	uint16_t self, dst16, nh;

	if (!ctx || ctx->iface != iface || !dst || !next_hop) {
		return -EINVAL;
	}

	self = aether_honr_addr16(ctx->local_addr);
	dst16 = aether_honr_addr16(dst);

	if (self == dst16) {
		return -EALREADY;
	}
	if (!honr_is_valid_node(dst16)) {
		return -EINVAL; /* refuse to route on a malformed destination */
	}

	nh = honr_next_hop(self, dst16);

	/* Carry the 16-bit HONR next hop in the low two address bytes. */
	memset(next_hop, 0, 6);
	next_hop[4] = (uint8_t)(nh >> 8);
	next_hop[5] = (uint8_t)(nh & 0xFF);

	LOG_DBG("HONR next hop %04x -> %04x = %04x", self, dst16, nh);
	return 0;
}

bool aether_honr_should_relay(struct net_if *iface, const uint8_t resender[6],
			      const uint8_t dst[6])
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;
	uint16_t self, res16, dst16;

	if (!ctx || ctx->iface != iface || !resender || !dst) {
		return false;
	}

	if (!ctx->honr_joined) {
		/* Not part of the tree yet: never relay. */
		return false;
	}

	self = aether_honr_addr16(ctx->local_addr);
	res16 = aether_honr_addr16(resender);
	dst16 = aether_honr_addr16(dst);

	/* Refuse to act on malformed addresses received over the air. */
	if (!honr_is_valid_node(dst16) || !honr_is_valid_node(res16)) {
		return false;
	}

	return honr_should_relay(self, res16, dst16);
}

/* ---------------------------------------------------------------------------
 * Tree join (step 3): unaddressed nodes attach to a parent and are handed a
 * HONR child address; addressed nodes periodically announce their address and
 * free-child bitmap. All control frames are single-hop broadcasts.
 * -------------------------------------------------------------------------- */

#define HONR_CHILD_MASK   0x7FFEU /* nibble values 1..14 (bit n = nibble n) */
#define HONR_ANN_LEN      8U      /* addr(2) + free bitmap(2) + node id(4) */
#define HONR_REQ_LEN      8U      /* joiner id(6) + target parent addr(2) */
#define HONR_REP_LEN      8U      /* joiner id(6) + assigned addr(2) */
#define HONR_JOIN_WAIT_MS 2000    /* await a join reply before retrying */
#define HONR_JOIN_COLLECT_MS 2000 /* listen this long for a shallower parent first */
#define HONR_PARENT_MISS_MAX 3U   /* orphan after this many missed parent announces */

static struct k_work_delayable honr_announce_work;
static struct k_work_delayable honr_election_work;
static struct k_work_delayable honr_join_decide_work;
static struct net_if *honr_iface;
static int64_t honr_join_pending_until;

/* Liveness of the parent: a non-root node that stops hearing its parent's
 * announcements has lost the tree above it; it relinquishes and re-elects so a
 * rooted tree re-forms (the lowest-id survivor becomes the new root).
 */
static int64_t honr_parent_last_heard;

/* Shallowest parent heard during the join collection window (lower rank = closer
 * to the root). Committed to by honr_join_decide_handler when the window elapses,
 * so a node attaches near the root instead of to whichever parent it heard first.
 */
static uint16_t honr_cand_parent;
static uint8_t honr_cand_rank;
static bool honr_cand_valid;

/* Stable per-node id used as the root-election tiebreak (lower wins). */
static uint32_t honr_node_id;

uint32_t aether_honr_node_id(void)
{
	return honr_node_id;
}

static uint32_t honr_election_delay_ms(void)
{
	return CONFIG_AETHER_HONR_ELECTION_BASE_MS +
	       (honr_node_id % CONFIG_AETHER_HONR_ELECTION_JITTER_MS);
}

/* Jittered next-announce delay: a deterministic per-node slot offset (so nodes
 * booted in lockstep spread across the interval) plus small random fine jitter
 * (to break ties), mirroring the HELLO beacon's staggering. Without it, two
 * nodes powered up together announce at the SAME instant every interval, never
 * hear each other's TREE_ANNOUNCE, and the dual-root tiebreak never fires -> both
 * stay root. (HELLO populates neighbors but TREE_ANNOUNCE drives convergence.) */
static uint32_t honr_announce_delay_ms(void)
{
	int32_t base = (int32_t)CONFIG_AETHER_HONR_ANNOUNCE_INTERVAL_SECONDS * 1000;
	int32_t slot = base / 4;                                   /* spread ±25% */
	int32_t off = (int32_t)(honr_node_id % (uint32_t)(2 * slot)) - slot;
	int32_t jit = (int32_t)(sys_rand32_get() % 1000) - 500;    /* fine ±0.5s */
	int32_t ms = base + off + jit;

	return ms < 1000 ? 1000U : (uint32_t)ms;
}

/* How long a non-root node tolerates silence from its parent before declaring
 * itself orphaned (a multiple of the announce interval).
 */
static int64_t honr_parent_timeout_ms(void)
{
	return (int64_t)CONFIG_AETHER_HONR_ANNOUNCE_INTERVAL_SECONDS * 1000 *
	       HONR_PARENT_MISS_MAX;
}

static void honr_become_root(struct aether_mesh_ctx *ctx)
{
	ctx->honr_addr = HONR_ROOT;
	ctx->honr_joined = true;
	ctx->honr_child_bitmap = 0;
	ctx->local_addr[4] = 0;
	ctx->local_addr[5] = 0;
}

/* Election timer: if no tree was heard during the listen window, claim root. */
static void honr_election_handler(struct k_work *work)
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;

	ARG_UNUSED(work);

	if (!ctx || ctx->honr_joined) {
		return;
	}

	/* Do NOT self-elect while a join is in flight. After a dual-root yield
	 * (the higher node id relinquishes to join the lower one) we relinquish
	 * AND re-arm this election; if it fires before the join completes we would
	 * re-root, and the winner's JOIN_REP is then ignored (handle_ctrl drops it
	 * when honr_joined) -- the classic dual-root oscillation that leaves BOTH
	 * nodes root with a phantom child on the winner. So while a candidate is
	 * seeded (collect window) or a JOIN_REQ is awaiting its reply, defer:
	 * re-arm and let the join land. Once it does, honr_joined is set and we
	 * never reach here; if it fails the pending window lapses and the next
	 * election self-elects as before.
	 */
	if (honr_cand_valid ||
	    (honr_join_pending_until != 0 && k_uptime_get() < honr_join_pending_until)) {
		k_work_reschedule(&honr_election_work, K_MSEC(honr_election_delay_ms()));
		return;
	}

	honr_become_root(ctx);
	LOG_INF("HONR: no tree heard, self-elected root (id %08x)", honr_node_id);
}

/* Lost a root tiebreak: drop to unjoined and re-arm the election, so we join
 * the winning root (now announcing) or re-elect if it disappears.
 */
static void honr_relinquish_root(void)
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;

	ctx->honr_joined = false;
	ctx->honr_addr = 0;
	ctx->honr_child_bitmap = 0;
	ctx->local_addr[4] = 0;
	ctx->local_addr[5] = 0;
	honr_join_pending_until = 0;
	honr_cand_valid = false;
	k_work_reschedule(&honr_election_work, K_MSEC(honr_election_delay_ms()));
}

static uint16_t honr_free_children(const struct aether_mesh_ctx *ctx)
{
	if (honr_rank(ctx->honr_addr) >= HONR_NIBBLES) {
		return 0; /* at maximum depth: no children possible */
	}
	return (uint16_t)(~ctx->honr_child_bitmap) & HONR_CHILD_MASK;
}

/* Allocate (or reclaim) a child nibble for the joiner identified by eui.
 *
 * A node that briefly orphans (e.g. missed a few parent announces under the
 * half-duplex RX/LBT timing) re-sends a JOIN_REQ while still powered. Without
 * identity tracking the parent handed out a NEW nibble every time and never
 * freed the old one, so a stable node walked the address space (5000 -> 7000
 * -> ...) and eventually exhausted the 14 child slots. Reclaim-by-EUI makes a
 * re-join idempotent: the returning node keeps its existing address.
 */
static int honr_alloc_child(struct aether_mesh_ctx *ctx, const uint8_t eui[6])
{
	/* Reclaim: this EUI already holds a slot -> return its existing nibble. */
	for (unsigned int n = HONR_CHILD_MIN; n <= HONR_CHILD_MAX; n++) {
		if ((ctx->honr_child_bitmap & BIT(n)) &&
		    memcmp(ctx->honr_child_eui[n], eui, 6) == 0) {
			return (int)n;
		}
	}
	/* Otherwise hand out the first free nibble and record the owner. */
	for (unsigned int n = HONR_CHILD_MIN; n <= HONR_CHILD_MAX; n++) {
		if (!(ctx->honr_child_bitmap & BIT(n))) {
			ctx->honr_child_bitmap |= BIT(n);
			memcpy(ctx->honr_child_eui[n], eui, 6);
			return (int)n;
		}
	}
	return -1;
}

static int honr_send_ctrl(struct net_if *iface, uint8_t type, const uint8_t *payload,
			  uint16_t len)
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;
	struct aether_mesh_hdr hdr;
	struct net_pkt *pkt;
	uint8_t hdr_buf[32];
	uint16_t l2_net_id;
	int hdr_len;
	int ret;

	memset(&hdr, 0, sizeof(hdr));
	hdr.type = type;
	hdr.ttl = 1; /* single hop */
	memcpy(hdr.src, ctx->local_addr, 6);
	memset(hdr.dst, 0xFF, 6);
	hdr.seq_no = ctx->seq_no++;
	hdr.qos_security = (AETHER_PRIORITY_LOW << 6) | (AETHER_DELIV_BESTEFFORT << 5);
	hdr.length = len;

	l2_net_id = heymac_get_network_id(iface);
	if (CONFIG_AETHER_CONDITIONAL_NET_ID && ctx->mesh_net_id != l2_net_id) {
		hdr.flags |= AETHER_FCTL_MESH_NET_ID;
		hdr.mesh_net_id = ctx->mesh_net_id;
	}

	hdr_len = aether_mesh_build_header(&hdr, hdr_buf, sizeof(hdr_buf));
	if (hdr_len < 0) {
		return hdr_len;
	}

	pkt = net_pkt_alloc_with_buffer(iface, hdr_len + len, AF_UNSPEC, 0, K_NO_WAIT);
	if (!pkt) {
		return -ENOMEM;
	}

	ret = net_pkt_write(pkt, hdr_buf, hdr_len);
	if (ret == 0 && len > 0) {
		ret = net_pkt_write(pkt, payload, len);
	}
	if (ret < 0) {
		net_pkt_unref(pkt);
		return ret;
	}

	net_if_queue_tx(iface, pkt);
	return 0;
}

static void honr_announce_handler(struct k_work *work)
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;

	ARG_UNUSED(work);

	/* Root-loss detection: a non-root node that has not heard its parent for
	 * several announce intervals has lost the tree above it. Relinquish and
	 * re-elect; with shallowest-parent join the mesh re-forms a rooted tree.
	 */
	if (ctx && ctx->honr_joined && !honr_is_root(ctx->honr_addr) &&
	    (k_uptime_get() - honr_parent_last_heard) > honr_parent_timeout_ms()) {
		LOG_WRN("HONR: parent %04x silent, orphaned - relinquishing to re-elect",
			honr_parent(ctx->honr_addr));
		honr_relinquish_root();
	} else if (ctx && ctx->honr_joined) {
		uint16_t freebits = honr_free_children(ctx);
		uint8_t p[HONR_ANN_LEN] = {
			(uint8_t)(ctx->honr_addr >> 8), (uint8_t)(ctx->honr_addr & 0xFF),
			(uint8_t)(freebits >> 8), (uint8_t)(freebits & 0xFF),
			(uint8_t)(honr_node_id >> 24), (uint8_t)(honr_node_id >> 16),
			(uint8_t)(honr_node_id >> 8), (uint8_t)(honr_node_id & 0xFF),
		};

		(void)honr_send_ctrl(honr_iface, AETHER_MSG_TREE_ANNOUNCE, p, sizeof(p));
	}

	k_work_reschedule(&honr_announce_work, K_MSEC(honr_announce_delay_ms()));
}

/* Commit the join: request an address from the shallowest parent heard during
 * the collection window (closest to the root keeps the tree flat).
 */
static void honr_join_decide_handler(struct k_work *work)
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;
	uint8_t req[HONR_REQ_LEN];

	ARG_UNUSED(work);

	if (!ctx || ctx->honr_joined || !honr_cand_valid) {
		honr_cand_valid = false;
		return;
	}

	memcpy(req, ctx->node_eui, 6); /* joiner id: stable, not the HONR-mutable local_addr */
	req[6] = (uint8_t)(honr_cand_parent >> 8);
	req[7] = (uint8_t)(honr_cand_parent & 0xFF);
	if (honr_send_ctrl(honr_iface, AETHER_MSG_TREE_JOIN_REQ, req, sizeof(req)) == 0) {
		honr_join_pending_until = k_uptime_get() + HONR_JOIN_WAIT_MS;
		LOG_INF("HONR join: requesting address from shallowest parent %04x (rank %u)",
			honr_cand_parent, honr_cand_rank);
	}
	honr_cand_valid = false;
}

void aether_honr_join_init(struct net_if *iface)
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;

	if (!ctx) {
		return;
	}

	uint8_t hwid[8];
	ssize_t hlen;

	/* Capture the stable unique node identity BEFORE the election/forced-root
	 * can overwrite local_addr[4..5] with the HONR address. This is the joiner
	 * id used in the tree-join handshake, so it stays unique even while the
	 * HONR address is 0x0000 (root/unjoined). */
	memcpy(ctx->node_eui, ctx->local_addr, 6);

	honr_iface = iface;
	ctx->honr_child_bitmap = 0;
	honr_join_pending_until = 0;

	/* Stable per-node election id derived from the hardware id. */
	honr_node_id = 0;
	hlen = hwinfo_get_device_id(hwid, sizeof(hwid));
	for (ssize_t i = 0; i < hlen; i++) {
		honr_node_id = honr_node_id * 31U + hwid[i];
	}

	honr_cand_valid = false;
	k_work_init_delayable(&honr_announce_work, honr_announce_handler);
	k_work_init_delayable(&honr_election_work, honr_election_handler);
	k_work_init_delayable(&honr_join_decide_work, honr_join_decide_handler);

	if (IS_ENABLED(CONFIG_AETHER_HONR_ROOT)) {
		/* Optional override: pin this node as root, skipping the election. */
		honr_become_root(ctx);
		LOG_INF("HONR: forced root (address 0000)");
	} else {
		ctx->honr_joined = false;
		/* Same firmware on every node: listen for an existing tree, and if
		 * none is heard, self-elect. Lower node ids claim sooner, so they
		 * tend to win uncontested.
		 */
		k_work_schedule(&honr_election_work, K_MSEC(honr_election_delay_ms()));
		LOG_INF("HONR: id %08x, root election in %u ms unless a tree is heard",
			honr_node_id, honr_election_delay_ms());
	}

	/* First announce is jittered too, so co-booted nodes don't start in phase. */
	k_work_schedule(&honr_announce_work, K_MSEC(honr_announce_delay_ms()));
}

void aether_honr_handle_ctrl(struct net_if *iface, uint8_t type, const uint8_t src[6],
			     const uint8_t *payload, uint16_t len)
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;

	ARG_UNUSED(src);

	if (!ctx) {
		return;
	}

	switch (type) {
	case AETHER_MSG_TREE_ANNOUNCE: {
		uint16_t parent_addr, parent_free;
		uint32_t peer_id;
		uint8_t parent_rank;
		uint8_t req[HONR_REQ_LEN];

		if (len < HONR_ANN_LEN) {
			break;
		}
		parent_addr = (uint16_t)((payload[0] << 8) | payload[1]);
		parent_free = (uint16_t)((payload[2] << 8) | payload[3]);
		peer_id = ((uint32_t)payload[4] << 24) | ((uint32_t)payload[5] << 16) |
			  ((uint32_t)payload[6] << 8) | payload[7];

		/* Dual-root resolution: if we are root and hear another root
		 * (address 0x0000), the lower node id keeps it and the higher one
		 * relinquishes and rejoins under the winner.
		 */
		if (ctx->honr_joined && honr_is_root(ctx->honr_addr) &&
		    parent_addr == HONR_ROOT) {
			if (peer_id >= honr_node_id) {
				break; /* we hold the lower (winning) id: keep root */
			}
			LOG_WRN("HONR: yielding root to lower id %08x, joining it", peer_id);
			honr_relinquish_root();
			/* Fall through (no break): let THIS announce seed the join
			 * candidate now. Relinquishing and then waiting would re-elect
			 * before the winner's next announce arrives, since the election
			 * delay is shorter than the announce interval -- the classic
			 * dual-root oscillation. Seeding the join here lets it complete
			 * (~JOIN_COLLECT_MS) before the re-armed election fires.
			 */
		}

		/* Refresh parent liveness from its announcement (even if it is now
		 * full or we are not joining), so a healthy parent never trips the
		 * root-loss detector.
		 */
		if (ctx->honr_joined && !honr_is_root(ctx->honr_addr) &&
		    parent_addr == honr_parent(ctx->honr_addr)) {
			honr_parent_last_heard = k_uptime_get();
		}

		/* A parent with no room or a malformed address is no use either to
		 * join or to re-parent under.
		 */
		if (parent_free == 0 || !honr_is_valid_node(parent_addr)) {
			break;
		}
		parent_rank = honr_rank(parent_addr);

		if (ctx->honr_joined) {
			/* Re-parent up: a childless, non-root node that hears a
			 * strictly-shallower parent migrates under it, flattening
			 * the tree toward the root over successive announcements.
			 * Restricted to childless nodes so no subtree is orphaned.
			 */
			bool busy = honr_join_pending_until != 0 &&
				    k_uptime_get() < honr_join_pending_until;

			if (!honr_is_root(ctx->honr_addr) && ctx->honr_child_bitmap == 0 &&
			    !busy && (uint8_t)(parent_rank + 1U) < honr_rank(ctx->honr_addr)) {
				LOG_INF("HONR: re-parenting under %04x (rank %u), was %04x (rank %u)",
					parent_addr, parent_rank, ctx->honr_addr,
					honr_rank(ctx->honr_addr));
				ctx->honr_joined = false; /* node_eui carries the joiner id */
				honr_cand_valid = false;
				memcpy(req, ctx->node_eui, 6);
				req[6] = (uint8_t)(parent_addr >> 8);
				req[7] = (uint8_t)(parent_addr & 0xFF);
				if (honr_send_ctrl(iface, AETHER_MSG_TREE_JOIN_REQ, req,
						   sizeof(req)) == 0) {
					honr_join_pending_until =
						k_uptime_get() + HONR_JOIN_WAIT_MS;
				}
			}
			break;
		}

		/* Unjoined and still awaiting a reply from a previous request. */
		if (honr_join_pending_until != 0 && k_uptime_get() < honr_join_pending_until) {
			break;
		}

		/* Otherwise collect the shallowest parent over a short window and
		 * commit in honr_join_decide_handler, so we attach near the root
		 * rather than to whichever parent we happened to hear first.
		 */
		if (!honr_cand_valid || parent_rank < honr_cand_rank) {
			honr_cand_parent = parent_addr;
			honr_cand_rank = parent_rank;
			if (!honr_cand_valid) {
				honr_cand_valid = true;
				k_work_reschedule(&honr_join_decide_work,
						  K_MSEC(HONR_JOIN_COLLECT_MS));
			}
		}
		break;
	}

	case AETHER_MSG_TREE_JOIN_REQ: {
		uint16_t target;
		int nibble;
		uint16_t child;
		uint8_t rep[HONR_REP_LEN];

		if (!ctx->honr_joined || len < HONR_REQ_LEN) {
			break;
		}
		target = (uint16_t)((payload[6] << 8) | payload[7]);
		if (target != ctx->honr_addr) {
			break; /* request is for a different parent */
		}
		if (honr_rank(ctx->honr_addr) >= HONR_NIBBLES) {
			break; /* no depth left */
		}
		nibble = honr_alloc_child(ctx, payload); /* payload[0..5] = joiner EUI */
		if (nibble < 0) {
			break; /* full */
		}
		child = honr_set_nibble(ctx->honr_addr, honr_rank(ctx->honr_addr), (uint8_t)nibble);

		memcpy(rep, payload, 6); /* echo joiner id */
		rep[6] = (uint8_t)(child >> 8);
		rep[7] = (uint8_t)(child & 0xFF);
		(void)honr_send_ctrl(iface, AETHER_MSG_TREE_JOIN_REP, rep, sizeof(rep));
		LOG_INF("HONR: assigned %04x to new child", child);
		break;
	}

	case AETHER_MSG_TREE_JOIN_REP: {
		uint16_t assigned;

		if (ctx->honr_joined || len < HONR_REP_LEN) {
			break;
		}
		if (memcmp(payload, ctx->node_eui, 6) != 0) {
			break; /* not our reply (matched against our stable joiner id) */
		}
		assigned = (uint16_t)((payload[6] << 8) | payload[7]);
		if (!honr_is_valid_node(assigned) || honr_is_root(assigned)) {
			LOG_WRN("HONR: rejecting invalid assigned address %04x", assigned);
			break;
		}
		ctx->honr_addr = assigned;
		ctx->local_addr[4] = payload[6];
		ctx->local_addr[5] = payload[7];
		ctx->honr_joined = true;
		ctx->honr_child_bitmap = 0;
		honr_join_pending_until = 0;
		honr_parent_last_heard = k_uptime_get(); /* start the liveness clock */
		LOG_INF("HONR joined: address %04x (rank %u)", assigned, honr_rank(assigned));
		break;
	}

	default:
		break;
	}
}
