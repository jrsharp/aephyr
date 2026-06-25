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

/* Static buffers for reliable send - moved to file scope for PSRAM placement.
 * Sends are serialized so a single buffer is safe.
 * Frame = HeyMac hdr (~7) + Mesh hdr (~20) + payload (up to MAX_PAYLOAD). */
#ifdef CONFIG_ESP_SPIRAM
static uint8_t frame_buf[CONFIG_AETHER_MAX_PAYLOAD + 32]
	__attribute__((section(".ext_ram.bss")));
#else
static uint8_t frame_buf[CONFIG_AETHER_MAX_PAYLOAD + 32];
#endif

/* --- Stop-and-wait ACK for unicast datagram reliability ------------------
 * The PHY is best-effort (no link ack), so a lost frame is just lost. Here we
 * add a minimal stop-and-wait ARQ: the sender waits for the receiver's ACK and
 * retransmits on timeout (only on actual loss -- no blind redundancy, so no
 * channel congestion when the first copy lands). Sends are serialized (the 9P
 * server drives them on one thread), so a single pending-ack slot is enough;
 * the ACK arrives on the RX thread and gives the semaphore. */
#define AETHER_ACK_TIMEOUT_MS 250          /* per-attempt wait for the ACK */
static K_SEM_DEFINE(ack_sem, 0, 1);
static struct {
	uint8_t dst[6];      /* peer we are awaiting an ACK from */
	uint8_t seq;         /* seq_no of the DATA we sent */
	volatile bool waiting;
} ack_wait;
static uint8_t ack_buf[40];                /* separate from frame_buf: ACK TX from RX ctx */

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

/* Mesh data transmission */
int aether_mesh_send(struct net_if *iface, const uint8_t dst[6],
                     const uint8_t *data, size_t len, uint8_t priority)
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;
	struct aether_mesh_hdr hdr;
	struct net_pkt *pkt;
	uint8_t hdr_buf[32];
	uint8_t next_hop[6];
	int hdr_len;
	int ret;

	if (!ctx || ctx->iface != iface || !dst || !data ||
	    len > CONFIG_AETHER_MAX_PAYLOAD) {
		return -EINVAL;
	}

	/* Check if destination is broadcast */
	static const uint8_t broadcast_addr[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
	bool is_broadcast = (memcmp(dst, broadcast_addr, 6) == 0);

	if (!is_broadcast) {
#if defined(CONFIG_AETHER_ROUTING_HONR)
		/* HONR forwarding is stateless and receivers self-select the next
		 * hop, so no route lookup or route discovery is needed here. */
		ARG_UNUSED(next_hop);
#else
		/* Unicast - find route to destination */
		ret = aether_mesh_find_route(iface, dst, next_hop);
		if (ret < 0) {
			/* No route found - initiate route discovery */
			LOG_DBG("No route to destination, sending RREQ");
			return aether_mesh_send_rreq(iface, dst);
		}
#endif
	}

	/* Build mesh header. Broadcast (party-line) frames carry the durable node
	 * identity as src so receivers learn a *stable* sender -- the HONR routing
	 * address churns and the root's is always 0x0000, which breaks chat keying,
	 * dedup, and the own-echo guard. Unicast keeps the HONR src (it doubles as
	 * the routing/reply address until the unicast path is likewise decoupled). */
	memset(&hdr, 0, sizeof(hdr));
	hdr.type = AETHER_MSG_DATA;
	hdr.ttl = CONFIG_AETHER_DEFAULT_TTL;
	memcpy(hdr.src, is_broadcast ? ctx->node_eui : ctx->local_addr, 6);
	memcpy(hdr.dst, dst, 6);
	hdr.seq_no = ctx->seq_no++;
	hdr.qos_security = (priority << 6) | (AETHER_DELIV_BESTEFFORT << 5);
	hdr.length = len;

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
		LOG_ERR("Failed to build mesh header: %d", hdr_len);
		return hdr_len;
	}

	/* Allocate packet */
	pkt = net_pkt_alloc_with_buffer(iface, hdr_len + len, AF_UNSPEC, 0, K_NO_WAIT);
	if (!pkt) {
		LOG_ERR("Failed to allocate mesh packet");
		return -ENOMEM;
	}

	/* Write header and payload */
	ret = net_pkt_write(pkt, hdr_buf, hdr_len);
	if (ret < 0) {
		LOG_ERR("Failed to write mesh header: %d", ret);
		net_pkt_unref(pkt);
		return ret;
	}

	ret = net_pkt_write(pkt, data, len);
	if (ret < 0) {
		LOG_ERR("Failed to write mesh payload: %d", ret);
		net_pkt_unref(pkt);
		return ret;
	}

#if defined(CONFIG_AETHER_ROUTING_HONR)
	/* Tag the link-layer destination with the HONR next hop so the radio
	 * addresses (and the modem hardware-filters) the frame. */
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

	/* Send packet */
	net_if_queue_tx(iface, pkt);
	/* net_if_queue_tx takes ownership of the packet */

	LOG_DBG("Mesh data sent to %02x:%02x:%02x:%02x:%02x:%02x, len=%zu",
		dst[0], dst[1], dst[2], dst[3], dst[4], dst[5], len);
	return 0;
}

/* Reliable mesh send with synchronous TX and retries */
int aether_mesh_send_reliable(struct net_if *iface, const uint8_t dst[6],
			      const uint8_t *data, size_t len, int max_retries)
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;
	struct aether_mesh_hdr mesh_hdr;
	static uint8_t mesh_hdr_buf[32];
	int mesh_hdr_len;
	int frame_len;
	int ret;

	if (!ctx || ctx->iface != iface || !dst || !data ||
	    len > CONFIG_AETHER_MAX_PAYLOAD) {
		return -EINVAL;
	}

	/* Check if destination is broadcast */
	static const uint8_t broadcast_addr[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
	bool is_broadcast = (memcmp(dst, broadcast_addr, 6) == 0);

	if (!is_broadcast) {
		/* For unicast, would need route lookup - for now just direct */
		LOG_DBG("Reliable unicast not fully implemented, sending direct");
	}

	/* Build mesh header */
	memset(&mesh_hdr, 0, sizeof(mesh_hdr));
	mesh_hdr.type = AETHER_MSG_DATA;
	mesh_hdr.ttl = CONFIG_AETHER_DEFAULT_TTL;
	memcpy(mesh_hdr.src, ctx->local_addr, 6);
	memcpy(mesh_hdr.dst, dst, 6);
	mesh_hdr.seq_no = ctx->seq_no++;
	mesh_hdr.qos_security = (AETHER_PRIORITY_NORMAL << 6) | (AETHER_DELIV_BESTEFFORT << 5);
	mesh_hdr.length = len;

	/* Optimize network ID */
	uint16_t heymac_net_id = heymac_get_network_id(iface);
	if (CONFIG_AETHER_CONDITIONAL_NET_ID && ctx->mesh_net_id != heymac_net_id) {
		mesh_hdr.flags |= AETHER_FCTL_MESH_NET_ID;
		mesh_hdr.mesh_net_id = ctx->mesh_net_id;
	}

	/* Build mesh header into buffer */
	mesh_hdr_len = aether_mesh_build_header(&mesh_hdr, mesh_hdr_buf, sizeof(mesh_hdr_buf));
	if (mesh_hdr_len < 0) {
		LOG_ERR("Failed to build mesh header: %d", mesh_hdr_len);
		return mesh_hdr_len;
	}

	/* Build simple HeyMac header (minimal for direct TX) */
	struct heymac_context *heymac_ctx = net_if_l2_data(iface);
	if (!heymac_ctx) {
		return -EINVAL;
	}

	/* Frame format: [HeyMac header][Mesh header][Payload] */
	frame_len = 0;

	/* HeyMac header - simplified version for direct TX */
	frame_buf[frame_len++] = HEYMAC_FRM_TYPE_DATA;  /* Version/Type */
	frame_buf[frame_len++] = HEYMAC_FCTL_S | HEYMAC_FCTL_M | HEYMAC_FCTL_N;  /* Frame control */
	frame_buf[frame_len++] = (heymac_ctx->net_id >> 8) & 0xFF;  /* Net ID high */
	frame_buf[frame_len++] = heymac_ctx->net_id & 0xFF;  /* Net ID low */
	/* Source address (short) */
	frame_buf[frame_len++] = heymac_ctx->local_addr.short_addr[0];
	frame_buf[frame_len++] = heymac_ctx->local_addr.short_addr[1];
	frame_buf[frame_len++] = heymac_ctx->tx_seq_no++;  /* Sequence */

	/* Append mesh header */
	if (frame_len + mesh_hdr_len + len > sizeof(frame_buf)) {
		LOG_ERR("Frame too large: %d", frame_len + mesh_hdr_len + (int)len);
		return -ENOMEM;
	}
	memcpy(&frame_buf[frame_len], mesh_hdr_buf, mesh_hdr_len);
	frame_len += mesh_hdr_len;

	/* Append payload */
	memcpy(&frame_buf[frame_len], data, len);
	frame_len += len;

	if (is_broadcast) {
		/* No single acker for a broadcast -- best-effort, one shot. */
		return aether_lora_send_sync(frame_buf, frame_len, max_retries);
	}

	/* Unicast: stop-and-wait ARQ. Arm the pending-ack BEFORE the first TX, then
	 * (re)transmit and wait for the receiver's ACK; the RX path gives ack_sem.
	 * Retransmits only on a missed ACK (no congestion when the first arrives);
	 * the receiver dedups a retransmitted DATA by (src, seq_no). */
	k_sem_reset(&ack_sem);
	memcpy(ack_wait.dst, dst, 6);
	ack_wait.seq = mesh_hdr.seq_no;
	ack_wait.waiting = true;

	ret = -ETIMEDOUT;
	for (int attempt = 0; attempt <= max_retries; attempt++) {
		(void)aether_lora_send_sync(frame_buf, frame_len, 2);
		if (k_sem_take(&ack_sem, K_MSEC(AETHER_ACK_TIMEOUT_MS)) == 0) {
			ret = 0;
			break;
		}
	}
	ack_wait.waiting = false;

	if (ret == 0) {
		LOG_DBG("datagram ACKed by %02x:%02x seq=%u len=%zu",
			dst[4], dst[5], mesh_hdr.seq_no, len);
	} else {
		LOG_WRN("datagram to %02x:%02x seq=%u: NO ACK after %d tries",
			dst[4], dst[5], mesh_hdr.seq_no, max_retries + 1);
	}

	return ret;
}

/* Build + transmit a bare ACK frame back to `to`, echoing the acked `seq`.
 * Called from the RX path; uses ack_buf (never frame_buf) so it can run
 * concurrently with a send_reliable in its ACK wait. Best-effort: if the ACK
 * is lost the sender retransmits the DATA and we re-ACK from the dup path. */
void aether_mesh_send_ack(struct net_if *iface, const uint8_t to[6], uint8_t seq)
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;
	struct heymac_context *hm = iface ? net_if_l2_data(iface) : NULL;
	struct aether_mesh_hdr h;
	uint8_t mh[32];
	int mhl, fl = 0;

	if (!ctx || !hm) {
		return;
	}
	memset(&h, 0, sizeof(h));
	h.type = AETHER_MSG_ACK;
	h.ttl = CONFIG_AETHER_DEFAULT_TTL;
	memcpy(h.src, ctx->local_addr, 6);
	memcpy(h.dst, to, 6);
	h.seq_no = seq;                 /* echo the DATA's seq so the sender matches */
	h.qos_security = (AETHER_PRIORITY_NORMAL << 6) | (AETHER_DELIV_BESTEFFORT << 5);
	h.length = 0;
	mhl = aether_mesh_build_header(&h, mh, sizeof(mh));
	if (mhl < 0) {
		return;
	}
	ack_buf[fl++] = HEYMAC_FRM_TYPE_DATA;
	ack_buf[fl++] = HEYMAC_FCTL_S | HEYMAC_FCTL_M | HEYMAC_FCTL_N;
	ack_buf[fl++] = (hm->net_id >> 8) & 0xFF;
	ack_buf[fl++] = hm->net_id & 0xFF;
	ack_buf[fl++] = hm->local_addr.short_addr[0];
	ack_buf[fl++] = hm->local_addr.short_addr[1];
	ack_buf[fl++] = hm->tx_seq_no++;
	memcpy(&ack_buf[fl], mh, mhl);
	fl += mhl;
	(void)aether_lora_send_sync(ack_buf, fl, 1);
}

/* Called from the RX path when an ACK arrives: wake send_reliable if it matches
 * the conversation we're awaiting an ACK for. */
void aether_mesh_ack_received(const uint8_t from[6], uint8_t seq)
{
	if (ack_wait.waiting && ack_wait.seq == seq &&
	    memcmp(ack_wait.dst, from, 6) == 0) {
		k_sem_give(&ack_sem);
	}
}
