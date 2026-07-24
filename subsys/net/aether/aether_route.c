/*
 * Copyright (c) 2025 Æther Authors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(aether_route, CONFIG_AETHER_MESH_LOG_LEVEL);

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_pkt.h>
#ifdef CONFIG_NET_L2_HEYMAC
#include <zephyr/net/heymac.h>
#endif
#include <zephyr/net/aether_mesh.h>
#include <zephyr/random/random.h>
#include <string.h>

#ifndef CONFIG_NET_L2_HEYMAC
/* Fallback for non-HeyMac transports */
static inline uint16_t aether_route_get_network_id_fallback(struct net_if *iface)
{
	ARG_UNUSED(iface);
	return 0x0001;  /* Default network ID */
}
#endif

/* External reference to global mesh context */
extern struct aether_mesh_ctx *g_mesh_ctx;

/* All mesh TX now goes through mesh_tx_frame() on the net_pkt pipeline (no more
 * hand-built raw HeyMac frames in a file-scope buffer, and no aether_lora_send_
 * sync() direct path) -- that unification is what lets reliable DATA and ACKs
 * route across multiple hops. The old frame_buf/ack_buf statics are gone. */

/* --- Stop-and-wait ACK for unicast datagram reliability ------------------
 * The PHY is best-effort (no link ack), so a lost frame is just lost. Here we
 * add a minimal stop-and-wait ARQ: the sender waits for the receiver's ACK and
 * retransmits on timeout (only on actual loss -- no blind redundancy, so no
 * channel congestion when the first copy lands). The ACK is an end-to-end
 * signal from the final destination back to the source, routed hop-by-hop the
 * same way the DATA is (see mesh_tx_frame() + the relay's ACK-forward path).
 *
 * A POOL of waiters (not a single slot) so multiple reliable transactions can
 * be outstanding at once -- e.g. concurrent /net/aether conversations each
 * carrying a 9P session, or a node relaying one flow while originating another.
 * A waiter is matched to its ACK by (dst, seq); the per-node monotonic seq
 * counter keeps that pair unique across conversations to the same peer within
 * the dedup window. */
#define AETHER_ACK_TIMEOUT_MS 250          /* per-attempt wait for the ACK */

/* Binary-exponential backoff between ACK-timeout RETRANSMITS (CSMA/CA). Without
 * it, two nodes that collide time out together and retransmit in lock-step,
 * colliding again every retry -- the dominant loss under simultaneous two-way
 * traffic. Each retry waits a uniform-random time in a contention window that
 * doubles per attempt (capped), so contending senders de-sync. */
#define AETHER_ACK_BACKOFF_MS     40       /* base contention window (doubles per retry) */
#define AETHER_ACK_BACKOFF_MAX_MS 300      /* window cap */
#define AETHER_ACK_BACKOFF_FLOOR_MS 5      /* min wait, so two picks are never both 0 */

/* Transient DECT channel-busy (LBT -EBUSY) is not a send failure -- back off and
 * retry the TX so the caller (chat, 9P-over-mesh) never sees it. Separate from the
 * ACK-retry budget below (that handles a lost frame, this handles a busy channel).
 * First slice of the reformation-tolerant transport: absorb transient RF. */
#define AETHER_TX_BUSY_RETRIES   6         /* extra channel-busy spins per TX */
#define AETHER_TX_BUSY_BACKOFF_MS 15       /* base backoff between busy spins (jittered) */

struct arq_waiter {
	struct k_sem sem;
	uint8_t dst[6];      /* peer we are awaiting an ACK from */
	uint8_t seq;         /* seq_no of the DATA we sent */
	bool in_use;
};
static struct arq_waiter arq_waiters[CONFIG_AETHER_MAX_ARQ_WAITERS];
static K_MUTEX_DEFINE(arq_mutex);          /* guards the pool + the seq counter */
static bool arq_ready;

/* k_sem in a static array needs runtime init; do it once, lazily, under the
 * mutex (avoids adding a new boot hook in this file). */
static void arq_init_once(void)
{
	k_mutex_lock(&arq_mutex, K_FOREVER);
	if (!arq_ready) {
		for (int i = 0; i < ARRAY_SIZE(arq_waiters); i++) {
			k_sem_init(&arq_waiters[i].sem, 0, 1);
		}
		arq_ready = true;
	}
	k_mutex_unlock(&arq_mutex);
}

/* Allocate the next monotonic per-node sequence number. Guarded because
 * reliable sends can now run from more than one thread (multiple conversations
 * / work queues), unlike the old single-serialized-sender assumption. */
static uint8_t alloc_seq(struct aether_mesh_ctx *ctx)
{
	uint8_t s;

	k_mutex_lock(&arq_mutex, K_FOREVER);
	s = ctx->seq_no++;
	k_mutex_unlock(&arq_mutex);
	return s;
}

static struct arq_waiter *arq_alloc(const uint8_t dst[6], uint8_t seq)
{
	struct arq_waiter *w = NULL;

	k_mutex_lock(&arq_mutex, K_FOREVER);
	for (int i = 0; i < ARRAY_SIZE(arq_waiters); i++) {
		if (!arq_waiters[i].in_use) {
			w = &arq_waiters[i];
			w->in_use = true;
			memcpy(w->dst, dst, 6);
			w->seq = seq;
			k_sem_reset(&w->sem);
			break;
		}
	}
	k_mutex_unlock(&arq_mutex);
	return w;
}

static void arq_free(struct arq_waiter *w)
{
	k_mutex_lock(&arq_mutex, K_FOREVER);
	w->in_use = false;
	k_mutex_unlock(&arq_mutex);
}

/* Route discovery and management functions */

int aether_mesh_find_route(struct net_if *iface, const uint8_t dst[6], 
                           uint8_t next_hop[6])
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;
	uint32_t now = k_uptime_get_32();
	int i;

	if (!ctx || ctx->iface != iface || !dst || !next_hop) {
		return -EINVAL;
	}

	k_mutex_lock(&ctx->route_lock, K_FOREVER);

	/* Look for active route to destination */
	for (i = 0; i < CONFIG_AETHER_MAX_ROUTES; i++) {
		if (ctx->routes[i].active &&
		    memcmp(ctx->routes[i].dest, dst, 6) == 0 &&
		    now < ctx->routes[i].expires) {
			
			/* Found valid route */
			memcpy(next_hop, ctx->routes[i].next_hop, 6);
			ctx->routes[i].last_used = now;
			
			k_mutex_unlock(&ctx->route_lock);
			LOG_DBG("Route found to %02x:%02x:%02x:%02x:%02x:%02x via %02x:%02x:%02x:%02x:%02x:%02x",
				dst[0], dst[1], dst[2], dst[3], dst[4], dst[5],
				next_hop[0], next_hop[1], next_hop[2], next_hop[3], next_hop[4], next_hop[5]);
			return 0;
		}
	}

	k_mutex_unlock(&ctx->route_lock);

	/* Check if destination is a direct neighbor */
	k_mutex_lock(&ctx->neighbor_lock, K_FOREVER);
	for (i = 0; i < CONFIG_AETHER_MAX_NEIGHBORS; i++) {
		if (ctx->neighbors[i].active &&
		    memcmp(ctx->neighbors[i].addr, dst, 6) == 0) {
			
			/* Direct neighbor - add route */
			memcpy(next_hop, dst, 6);
			k_mutex_unlock(&ctx->neighbor_lock);
			
			/* Add to route table */
			aether_mesh_add_route(iface, dst, dst, 1);
			
			LOG_DBG("Direct route to neighbor %02x:%02x:%02x:%02x:%02x:%02x",
				dst[0], dst[1], dst[2], dst[3], dst[4], dst[5]);
			return 0;
		}
	}
	k_mutex_unlock(&ctx->neighbor_lock);

	LOG_DBG("No route found to %02x:%02x:%02x:%02x:%02x:%02x",
		dst[0], dst[1], dst[2], dst[3], dst[4], dst[5]);
	return -ENOENT;
}

int aether_mesh_add_route(struct net_if *iface, const uint8_t dest[6],
                          const uint8_t next_hop[6], uint8_t hop_count)
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;
	uint32_t now = k_uptime_get_32();
	int i, free_slot = -1, oldest_slot = -1;
	uint32_t oldest_time = UINT32_MAX;

	if (!ctx || ctx->iface != iface || !dest || !next_hop) {
		return -EINVAL;
	}

	k_mutex_lock(&ctx->route_lock, K_FOREVER);

	/* Look for existing route or find free/oldest slot */
	for (i = 0; i < CONFIG_AETHER_MAX_ROUTES; i++) {
		if (ctx->routes[i].active &&
		    memcmp(ctx->routes[i].dest, dest, 6) == 0) {
			
			/* Update existing route if this is better */
			if (hop_count < ctx->routes[i].hop_count) {
				memcpy(ctx->routes[i].next_hop, next_hop, 6);
				ctx->routes[i].hop_count = hop_count;
				ctx->routes[i].last_used = now;
				ctx->routes[i].expires = now + (CONFIG_AETHER_ROUTE_TIMEOUT * 1000);
				
				LOG_DBG("Updated route to %02x:%02x:%02x:%02x:%02x:%02x, hops=%d",
					dest[0], dest[1], dest[2], dest[3], dest[4], dest[5],
					hop_count);
			}
			k_mutex_unlock(&ctx->route_lock);
			return 0;
		}

		if (!ctx->routes[i].active && free_slot == -1) {
			free_slot = i;
		} else if (ctx->routes[i].active && 
			   ctx->routes[i].last_used < oldest_time) {
			oldest_time = ctx->routes[i].last_used;
			oldest_slot = i;
		}
	}

	/* Use free slot or replace oldest */
	int slot = (free_slot >= 0) ? free_slot : oldest_slot;
	if (slot >= 0) {
		memcpy(ctx->routes[slot].dest, dest, 6);
		memcpy(ctx->routes[slot].next_hop, next_hop, 6);
		ctx->routes[slot].hop_count = hop_count;
		ctx->routes[slot].last_used = now;
		ctx->routes[slot].expires = now + (CONFIG_AETHER_ROUTE_TIMEOUT * 1000);
		ctx->routes[slot].active = true;
		
		LOG_DBG("Added route to %02x:%02x:%02x:%02x:%02x:%02x via %02x:%02x:%02x:%02x:%02x:%02x, hops=%d",
			dest[0], dest[1], dest[2], dest[3], dest[4], dest[5],
			next_hop[0], next_hop[1], next_hop[2], next_hop[3], next_hop[4], next_hop[5],
			hop_count);
	}

	k_mutex_unlock(&ctx->route_lock);
	return (slot >= 0) ? 0 : -ENOMEM;
}

int aether_mesh_remove_route(struct net_if *iface, const uint8_t dest[6])
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;
	int i;

	if (!ctx || ctx->iface != iface || !dest) {
		return -EINVAL;
	}

	k_mutex_lock(&ctx->route_lock, K_FOREVER);

	for (i = 0; i < CONFIG_AETHER_MAX_ROUTES; i++) {
		if (ctx->routes[i].active &&
		    memcmp(ctx->routes[i].dest, dest, 6) == 0) {
			
			ctx->routes[i].active = false;
			
			LOG_DBG("Removed route to %02x:%02x:%02x:%02x:%02x:%02x",
				dest[0], dest[1], dest[2], dest[3], dest[4], dest[5]);
			k_mutex_unlock(&ctx->route_lock);
			return 0;
		}
	}

	k_mutex_unlock(&ctx->route_lock);
	return -ENOENT;
}

/* Route request/reply handling */
#if !defined(CONFIG_AETHER_ROUTING_HONR)
static int aether_mesh_send_rreq(struct net_if *iface, const uint8_t dest[6])
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;
	struct aether_mesh_hdr hdr;
	struct net_pkt *pkt;
	uint8_t hdr_buf[32];
	uint8_t payload[6];  /* Just destination address for RREQ */
	int hdr_len;
	int ret;

	if (!ctx || ctx->iface != iface || !dest) {
		return -EINVAL;
	}

	/* Build RREQ header */
	memset(&hdr, 0, sizeof(hdr));
	hdr.type = AETHER_MSG_RREQ;
	hdr.ttl = CONFIG_AETHER_DEFAULT_TTL;
	memcpy(hdr.src, ctx->local_addr, 6);
	memset(hdr.dst, 0xFF, 6);  /* Broadcast */
	hdr.seq_no = ctx->seq_no++;
	hdr.qos_security = (AETHER_PRIORITY_NORMAL << 6) | (AETHER_DELIV_BESTEFFORT << 5);
	hdr.length = sizeof(payload);

	/* Optimize network ID */
#ifdef CONFIG_NET_L2_HEYMAC
	uint16_t l2_net_id = heymac_get_network_id(iface);
#else
	uint16_t l2_net_id = aether_route_get_network_id_fallback(iface);
#endif
	if (CONFIG_AETHER_CONDITIONAL_NET_ID && ctx->mesh_net_id != l2_net_id) {
		hdr.flags |= AETHER_FCTL_MESH_NET_ID;
		hdr.mesh_net_id = ctx->mesh_net_id;
	}

	/* Build header */
	hdr_len = aether_mesh_build_header(&hdr, hdr_buf, sizeof(hdr_buf));
	if (hdr_len < 0) {
		LOG_ERR("Failed to build RREQ header: %d", hdr_len);
		return hdr_len;
	}

	/* Prepare payload */
	memcpy(payload, dest, 6);

	/* Allocate packet */
	pkt = net_pkt_alloc_with_buffer(iface, hdr_len + sizeof(payload), 
					AF_UNSPEC, 0, K_NO_WAIT);
	if (!pkt) {
		LOG_ERR("Failed to allocate RREQ packet");
		return -ENOMEM;
	}

	/* Write header and payload */
	ret = net_pkt_write(pkt, hdr_buf, hdr_len);
	if (ret < 0) {
		LOG_ERR("Failed to write RREQ header: %d", ret);
		net_pkt_unref(pkt);
		return ret;
	}

	ret = net_pkt_write(pkt, payload, sizeof(payload));
	if (ret < 0) {
		LOG_ERR("Failed to write RREQ payload: %d", ret);
		net_pkt_unref(pkt);
		return ret;
	}

	/* Send packet */
	net_if_queue_tx(iface, pkt);
	/* net_if_queue_tx takes ownership of the packet */

	ctx->route_requests++;
	LOG_DBG("RREQ sent for %02x:%02x:%02x:%02x:%02x:%02x",
		dest[0], dest[1], dest[2], dest[3], dest[4], dest[5]);
	return 0;
}
#endif /* !CONFIG_AETHER_ROUTING_HONR */

/* Shared mesh TX core: build a mesh frame (header + optional payload) into a
 * net_pkt, address the link-layer next hop (HONR), and queue it. The single
 * place any mesh frame -- best-effort DATA, reliable DATA, or an ACK -- goes
 * onto the wire, so all three route across multiple hops identically. Unifying
 * this is what fixes the long-standing split where the reliable/ACK paths hand-
 * built raw single-hop frames and never set a next hop.
 *
 * `src` is the caller's choice: node_eui for broadcast (stable identity), the
 * HONR local_addr for unicast DATA + ACKs (doubles as the routing/reply addr).
 * `seq` is pre-allocated by the caller (so a reliable sender can match the ACK).
 * Broadcast (dst == ff:ff:ff:ff:ff:ff) is left link-unaddressed so the driver
 * sends a type-1 broadcast; the mesh reflood in the RX path carries it onward. */
static int mesh_tx_frame(struct net_if *iface, uint8_t type,
			 const uint8_t src[6], const uint8_t dst[6],
			 uint8_t seq, uint8_t priority,
			 const uint8_t *data, size_t len)
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;
	struct aether_mesh_hdr hdr;
	struct net_pkt *pkt;
	uint8_t hdr_buf[32];
	int hdr_len, ret;
	static const uint8_t broadcast_addr[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
	bool is_broadcast = (memcmp(dst, broadcast_addr, 6) == 0);

	memset(&hdr, 0, sizeof(hdr));
	hdr.type = type;
	hdr.ttl = CONFIG_AETHER_DEFAULT_TTL;
	memcpy(hdr.src, src, 6);
	memcpy(hdr.dst, dst, 6);
	hdr.seq_no = seq;
	hdr.qos_security = (priority << 6) | (AETHER_DELIV_BESTEFFORT << 5);
	hdr.length = (uint16_t)len;

	/* Optimize network ID */
#ifdef CONFIG_NET_L2_HEYMAC
	uint16_t l2_net_id = heymac_get_network_id(iface);
#else
	uint16_t l2_net_id = aether_route_get_network_id_fallback(iface);
#endif
	if (CONFIG_AETHER_CONDITIONAL_NET_ID && ctx->mesh_net_id != l2_net_id) {
		hdr.flags |= AETHER_FCTL_MESH_NET_ID;
		hdr.mesh_net_id = ctx->mesh_net_id;
	}

	hdr_len = aether_mesh_build_header(&hdr, hdr_buf, sizeof(hdr_buf));
	if (hdr_len < 0) {
		LOG_ERR("Failed to build mesh header: %d", hdr_len);
		return hdr_len;
	}

	pkt = net_pkt_alloc_with_buffer(iface, hdr_len + len, AF_UNSPEC, 0, K_NO_WAIT);
	if (!pkt) {
		LOG_ERR("Failed to allocate mesh packet");
		return -ENOMEM;
	}

	ret = net_pkt_write(pkt, hdr_buf, hdr_len);
	if (ret == 0 && len > 0) {
		ret = net_pkt_write(pkt, data, len);
	}
	if (ret < 0) {
		LOG_ERR("Failed to write mesh frame: %d", ret);
		net_pkt_unref(pkt);
		return ret;
	}

#if defined(CONFIG_AETHER_ROUTING_HONR)
	/* Tag the link-layer destination with the HONR next hop so the radio
	 * addresses (and the modem hardware-filters) the frame. Applies to any
	 * unicast frame -- DATA or ACK -- so both route across multiple hops. */
	if (!is_broadcast) {
		uint8_t nh[6];

		if (aether_honr_next_hop(iface, dst, nh) == 0) {
			struct net_linkaddr *ll = net_pkt_lladdr_dst(pkt);

			ll->len = 2;
			ll->addr[0] = nh[4];
			ll->addr[1] = nh[5];
		}
	}
#endif

	net_if_queue_tx(iface, pkt);   /* takes ownership of the packet */
	return 0;
}

/* Mesh data transmission (best-effort, one shot). */
int aether_mesh_send(struct net_if *iface, const uint8_t dst[6],
                     const uint8_t *data, size_t len, uint8_t priority)
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;

	if (!ctx || ctx->iface != iface || !dst || !data ||
	    len > CONFIG_AETHER_MAX_PAYLOAD) {
		return -EINVAL;
	}

	static const uint8_t broadcast_addr[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
	bool is_broadcast = (memcmp(dst, broadcast_addr, 6) == 0);

#if !defined(CONFIG_AETHER_ROUTING_HONR)
	if (!is_broadcast) {
		/* Non-HONR: need a route (or discovery) before we can send. */
		uint8_t next_hop[6];

		if (aether_mesh_find_route(iface, dst, next_hop) < 0) {
			LOG_DBG("No route to destination, sending RREQ");
			return aether_mesh_send_rreq(iface, dst);
		}
	}
#endif

	/* Broadcast (party-line) frames carry the durable node identity as src so
	 * receivers learn a *stable* sender -- the HONR routing address churns and
	 * the root's is always 0x0000, which breaks chat keying, dedup, and the
	 * own-echo guard. Unicast keeps the HONR src (routing/reply address). */
	const uint8_t *src = is_broadcast ? ctx->node_eui : ctx->local_addr;
	int ret = mesh_tx_frame(iface, AETHER_MSG_DATA, src, dst,
				alloc_seq(ctx), priority, data, len);

	if (ret == 0) {
		LOG_DBG("Mesh data sent to %02x:%02x:%02x:%02x:%02x:%02x, len=%zu",
			dst[0], dst[1], dst[2], dst[3], dst[4], dst[5], len);
	}
	return ret;
}

/* Broadcast this node's durable node_eui<->HONR binding, flooded (DEFAULT_TTL so
 * intermediate nodes re-flood it, dedup-bounded) so multi-hop peers can resolve
 * us by our stable identity. src = our current HONR routing address, payload =
 * our node_eui. Best-effort; the periodic re-send plus the receiver's generous
 * binding aging cover a dropped beacon. */
int aether_mesh_send_binding(struct net_if *iface)
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;
	static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

	if (!ctx || ctx->iface != iface) {
		return -EINVAL;
	}
	return mesh_tx_frame(iface, AETHER_MSG_BINDING, ctx->local_addr, bcast,
			     alloc_seq(ctx), AETHER_PRIORITY_LOW, ctx->node_eui, 6);
}

/* Reliable mesh send: stop-and-wait ARQ over the routed (multi-hop) TX core.
 * Unicast waits for the destination's end-to-end ACK and retransmits on
 * timeout; broadcast has no single acker so it is best-effort one-shot (the
 * caller layers its own delivery check, e.g. the chat transcript verify). */
int aether_mesh_send_reliable(struct net_if *iface, const uint8_t dst[6],
			      const uint8_t *data, size_t len, int max_retries)
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;
	uint8_t seq;
	int ret;

	if (!ctx || ctx->iface != iface || !dst || !data ||
	    len > CONFIG_AETHER_MAX_PAYLOAD) {
		return -EINVAL;
	}

	static const uint8_t broadcast_addr[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
	bool is_broadcast = (memcmp(dst, broadcast_addr, 6) == 0);

	seq = alloc_seq(ctx);

	if (is_broadcast) {
		/* No single acker -- best-effort, one shot. node_eui src (stable). */
		return mesh_tx_frame(iface, AETHER_MSG_DATA, ctx->node_eui, dst,
				     seq, AETHER_PRIORITY_NORMAL, data, len);
	}

	/* Unicast: stop-and-wait ARQ. Claim a waiter (matched to the ACK by
	 * (dst,seq)) BEFORE the first TX, then (re)transmit and wait for the
	 * receiver's end-to-end ACK; the RX path gives the waiter's sem.
	 * Retransmits only on a missed ACK (no congestion when the first
	 * arrives); the receiver dedups a retransmitted DATA by (src, seq_no).
	 * The HONR src doubles as the routing/reply address. */
	arq_init_once();

	struct arq_waiter *w = arq_alloc(dst, seq);

	if (!w) {
		LOG_WRN("ARQ waiter pool exhausted (%d in flight); send to %02x:%02x dropped",
			CONFIG_AETHER_MAX_ARQ_WAITERS, dst[4], dst[5]);
		return -EBUSY;
	}

	ret = -ETIMEDOUT;
	for (int attempt = 0; attempt <= max_retries; attempt++) {
		int txr = -EBUSY;

		/* Absorb a transient busy channel (LBT): spin with jittered backoff a
		 * few times before giving up, WITHOUT consuming an ACK-retry attempt.
		 * -EBUSY means the frame never left, so re-sending the same seq is safe
		 * (the receiver dedups a genuine retransmit by src,seq anyway). */
		for (int b = 0; b < AETHER_TX_BUSY_RETRIES && txr == -EBUSY; b++) {
			if (b > 0) {
				k_msleep(AETHER_TX_BUSY_BACKOFF_MS +
					 (sys_rand32_get() % AETHER_TX_BUSY_BACKOFF_MS));
			}
			txr = mesh_tx_frame(iface, AETHER_MSG_DATA, ctx->local_addr, dst,
					    seq, AETHER_PRIORITY_NORMAL, data, len);
		}

		if (txr < 0) {
			ret = txr;   /* hard error or channel busy past the spin budget */
			break;
		}
		if (k_sem_take(&w->sem, K_MSEC(AETHER_ACK_TIMEOUT_MS)) == 0) {
			ret = 0;
			break;
		}
		/* No ACK: collision or loss. Back off a random time in a contention
		 * window that doubles per attempt before retransmitting, so two senders
		 * that collided don't retransmit in lock-step and collide again. */
		if (attempt < max_retries) {
			uint32_t cw = (uint32_t)AETHER_ACK_BACKOFF_MS << attempt;

			ctx->arq_retx++;   /* a lost frame -> we retransmit (link-quality proxy) */
			if (cw > AETHER_ACK_BACKOFF_MAX_MS) {
				cw = AETHER_ACK_BACKOFF_MAX_MS;
			}
			k_msleep(AETHER_ACK_BACKOFF_FLOOR_MS + (sys_rand32_get() % cw));
		}
	}
	arq_free(w);

	if (ret == 0) {
		LOG_DBG("datagram ACKed by %02x:%02x seq=%u len=%zu",
			dst[4], dst[5], seq, len);
	} else {
		ctx->arq_failed++;   /* gave up after all retries -- the reliability KPI */
		LOG_WRN("datagram to %02x:%02x seq=%u: no ACK/failed (%d) after %d tries",
			dst[4], dst[5], seq, ret, max_retries + 1);
	}

	return ret;
}

/* Build + transmit a bare ACK frame back to `to`, echoing the acked `seq`.
 * Routed through the shared TX core so the ACK is next-hop-addressed and
 * relays back across multiple hops (the relay's ACK-forward path carries it).
 * The ACK's src is our HONR local_addr (the reply/routing address the original
 * sender awaits by (dst,seq)). Best-effort: if the ACK is lost the sender
 * retransmits the DATA and we re-ACK from the dup path. */
void aether_mesh_send_ack(struct net_if *iface, const uint8_t to[6], uint8_t seq)
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;

	if (!ctx || ctx->iface != iface) {
		return;
	}
	(void)mesh_tx_frame(iface, AETHER_MSG_ACK, ctx->local_addr, to,
			    seq, AETHER_PRIORITY_NORMAL, NULL, 0);
}

/* Called from the RX path when an ACK arrives: wake whichever reliable send is
 * awaiting this (from, seq) -- scans the waiter pool so concurrent flows each
 * get their own ACK. Giving an already-taken/freed waiter's sem is harmless. */
void aether_mesh_ack_received(const uint8_t from[6], uint8_t seq)
{
	k_mutex_lock(&arq_mutex, K_FOREVER);
	for (int i = 0; i < ARRAY_SIZE(arq_waiters); i++) {
		if (arq_waiters[i].in_use && arq_waiters[i].seq == seq &&
		    memcmp(arq_waiters[i].dst, from, 6) == 0) {
			k_sem_give(&arq_waiters[i].sem);
		}
	}
	k_mutex_unlock(&arq_mutex);
}
