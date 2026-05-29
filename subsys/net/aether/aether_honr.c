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

	return honr_should_relay(self, res16, dst16);
}

/* ---------------------------------------------------------------------------
 * Tree join (step 3): unaddressed nodes attach to a parent and are handed a
 * HONR child address; addressed nodes periodically announce their address and
 * free-child bitmap. All control frames are single-hop broadcasts.
 * -------------------------------------------------------------------------- */

#define HONR_CHILD_MASK   0x7FFEU /* nibble values 1..14 (bit n = nibble n) */
#define HONR_ANN_LEN      4U      /* addr(2) + free bitmap(2) */
#define HONR_REQ_LEN      8U      /* joiner id(6) + target parent addr(2) */
#define HONR_REP_LEN      8U      /* joiner id(6) + assigned addr(2) */
#define HONR_JOIN_WAIT_MS 2000    /* await a join reply before retrying */

static struct k_work_delayable honr_announce_work;
static struct net_if *honr_iface;
static int64_t honr_join_pending_until;

static uint16_t honr_free_children(const struct aether_mesh_ctx *ctx)
{
	if (honr_rank(ctx->honr_addr) >= HONR_NIBBLES) {
		return 0; /* at maximum depth: no children possible */
	}
	return (uint16_t)(~ctx->honr_child_bitmap) & HONR_CHILD_MASK;
}

static int honr_alloc_child(struct aether_mesh_ctx *ctx)
{
	for (unsigned int n = HONR_CHILD_MIN; n <= HONR_CHILD_MAX; n++) {
		if (!(ctx->honr_child_bitmap & BIT(n))) {
			ctx->honr_child_bitmap |= BIT(n);
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

	if (ctx && ctx->honr_joined) {
		uint16_t freebits = honr_free_children(ctx);
		uint8_t p[HONR_ANN_LEN] = {
			(uint8_t)(ctx->honr_addr >> 8), (uint8_t)(ctx->honr_addr & 0xFF),
			(uint8_t)(freebits >> 8), (uint8_t)(freebits & 0xFF),
		};

		(void)honr_send_ctrl(honr_iface, AETHER_MSG_TREE_ANNOUNCE, p, sizeof(p));
	}

	k_work_reschedule(&honr_announce_work,
			  K_SECONDS(CONFIG_AETHER_HONR_ANNOUNCE_INTERVAL_SECONDS));
}

void aether_honr_join_init(struct net_if *iface)
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;

	if (!ctx) {
		return;
	}

	honr_iface = iface;
	ctx->honr_child_bitmap = 0;
	honr_join_pending_until = 0;

	if (IS_ENABLED(CONFIG_AETHER_HONR_ROOT)) {
		ctx->honr_addr = HONR_ROOT;
		ctx->honr_joined = true;
		ctx->local_addr[4] = 0;
		ctx->local_addr[5] = 0;
		LOG_INF("HONR root: address 0000");
	} else {
		ctx->honr_joined = false;
		LOG_INF("HONR node: awaiting tree join");
	}

	k_work_init_delayable(&honr_announce_work, honr_announce_handler);
	k_work_schedule(&honr_announce_work, K_SECONDS(2));
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
		uint8_t req[HONR_REQ_LEN];

		if (ctx->honr_joined || len < HONR_ANN_LEN) {
			break;
		}
		/* Still awaiting a reply from a previous request. */
		if (honr_join_pending_until != 0 && k_uptime_get() < honr_join_pending_until) {
			break;
		}

		parent_addr = (uint16_t)((payload[0] << 8) | payload[1]);
		parent_free = (uint16_t)((payload[2] << 8) | payload[3]);
		if (parent_free == 0) {
			break; /* parent has no room */
		}

		/* Request an address from this parent (id + chosen parent). */
		memcpy(req, ctx->local_addr, 6);
		req[6] = (uint8_t)(parent_addr >> 8);
		req[7] = (uint8_t)(parent_addr & 0xFF);
		if (honr_send_ctrl(iface, AETHER_MSG_TREE_JOIN_REQ, req, sizeof(req)) == 0) {
			honr_join_pending_until = k_uptime_get() + HONR_JOIN_WAIT_MS;
			LOG_INF("HONR join: requesting address from parent %04x", parent_addr);
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
		nibble = honr_alloc_child(ctx);
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
		if (memcmp(payload, ctx->local_addr, 6) != 0) {
			break; /* not our reply */
		}
		assigned = (uint16_t)((payload[6] << 8) | payload[7]);
		ctx->honr_addr = assigned;
		ctx->local_addr[4] = payload[6];
		ctx->local_addr[5] = payload[7];
		ctx->honr_joined = true;
		ctx->honr_child_bitmap = 0;
		honr_join_pending_until = 0;
		LOG_INF("HONR joined: address %04x (rank %u)", assigned, honr_rank(assigned));
		break;
	}

	default:
		break;
	}
}
