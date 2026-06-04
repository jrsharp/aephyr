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
#include <zephyr/net/aether_reliable.h>
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

/* Static frame buffer for the legacy synchronous reliable send (modem-level
 * retries). Sends are serialized so a single buffer is safe. */
#ifdef CONFIG_ESP_SPIRAM
static uint8_t frame_buf[CONFIG_AETHER_MAX_PAYLOAD + 32]
	__attribute__((section(".ext_ram.bss")));
#else
static uint8_t frame_buf[CONFIG_AETHER_MAX_PAYLOAD + 32];
#endif

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
		/* Unicast - find route to destination */
		ret = aether_mesh_find_route(iface, dst, next_hop);
		if (ret < 0) {
			/* No route found - initiate route discovery */
			LOG_DBG("No route to destination, sending RREQ");
			return aether_mesh_send_rreq(iface, dst);
		}
	}

	/* Build mesh header */
	memset(&hdr, 0, sizeof(hdr));
	hdr.type = AETHER_MSG_DATA;
	hdr.ttl = CONFIG_AETHER_DEFAULT_TTL;
	memcpy(hdr.src, ctx->local_addr, 6);
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

	/* Send packet */
	net_if_queue_tx(iface, pkt);
	/* net_if_queue_tx takes ownership of the packet */

	LOG_DBG("Mesh data sent to %02x:%02x:%02x:%02x:%02x:%02x, len=%zu",
		dst[0], dst[1], dst[2], dst[3], dst[4], dst[5], len);
	return 0;
}

/* Reliable mesh send with synchronous TX and retries */
/* ===================================================================
 * Reliable-datagram glue (AETHER_PROTOCOL_SPEC §4.6)
 *
 * Drives the pure aether_reliable engine: the engine owns the ARQ state
 * machine and calls back here to put bytes on the wire; this side builds the
 * mesh frames, ticks the engine from the mesh workqueue, and feeds it RX.
 * The engine state is large (per-peer TX+RX buffers) — kept in PSRAM on the
 * deck so it doesn't pressure DRAM.
 * =================================================================== */

#ifdef CONFIG_ESP_SPIRAM
static struct aether_reliable_ctx g_arq __attribute__((section(".ext_ram.bss")));
#else
static struct aether_reliable_ctx g_arq;
#endif
static struct k_work_delayable arq_tick_work;
#define ARQ_TICK_MS 100

/* Build and queue one mesh frame with explicit type/qos/seq (the engine,
 * not ctx->seq_no, owns the reliable sequence). Mirrors aether_mesh_send's
 * frame construction. */
static int arq_tx_frame(const uint8_t dst[6], uint8_t type, uint8_t qos,
			uint8_t seq, const uint8_t *data, uint16_t len)
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;
	struct aether_mesh_hdr hdr;
	struct net_pkt *pkt;
	uint8_t hdr_buf[32];
	uint8_t next_hop[6];
	int hdr_len, ret;

	if (!ctx) {
		return -EINVAL;
	}

	static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
	bool is_bcast = (memcmp(dst, bcast, 6) == 0);
	if (!is_bcast) {
		ret = aether_mesh_find_route(ctx->iface, dst, next_hop);
		if (ret < 0) {
			/* No route yet — kick discovery; the engine's RTO will
			 * retransmit once a route exists. */
			aether_mesh_send_rreq(ctx->iface, dst);
			return -ENETUNREACH;
		}
	}

	memset(&hdr, 0, sizeof(hdr));
	hdr.type = type;
	hdr.ttl = CONFIG_AETHER_DEFAULT_TTL;
	memcpy(hdr.src, ctx->local_addr, 6);
	memcpy(hdr.dst, dst, 6);
	hdr.seq_no = seq;
	hdr.qos_security = qos;
	hdr.length = len;

#ifdef CONFIG_NET_L2_HEYMAC
	uint16_t l2_net_id = heymac_get_network_id(ctx->iface);
#else
	uint16_t l2_net_id = aether_route_get_network_id_fallback(ctx->iface);
#endif
	if (CONFIG_AETHER_CONDITIONAL_NET_ID && ctx->mesh_net_id != l2_net_id) {
		hdr.flags |= AETHER_FCTL_MESH_NET_ID;
		hdr.mesh_net_id = ctx->mesh_net_id;
	}

	hdr_len = aether_mesh_build_header(&hdr, hdr_buf, sizeof(hdr_buf));
	if (hdr_len < 0) {
		return hdr_len;
	}

	pkt = net_pkt_alloc_with_buffer(ctx->iface, hdr_len + len, AF_UNSPEC, 0,
					K_NO_WAIT);
	if (!pkt) {
		return -ENOMEM;
	}
	ret = net_pkt_write(pkt, hdr_buf, hdr_len);
	if (ret == 0 && len) {
		ret = net_pkt_write(pkt, data, len);
	}
	if (ret < 0) {
		net_pkt_unref(pkt);
		return ret;
	}
	net_if_queue_tx(ctx->iface, pkt);
	return 0;
}

/* ---- engine I/O callbacks ---- */

static int arq_cb_tx_data(void *user, const uint8_t dst[6], uint8_t seq,
			  bool flow_start, const uint8_t *data, uint16_t len)
{
	ARG_UNUSED(user);
	uint8_t qos = (AETHER_PRIORITY_NORMAL << 6) | (AETHER_DELIV_RELIABLE << 5) |
		      (flow_start ? AETHER_QOS_FLOW_START : 0);
	return arq_tx_frame(dst, AETHER_MSG_DATA, qos, seq, data, len);
}

static int arq_cb_tx_ack(void *user, const uint8_t dst[6],
			 const struct aether_ack *ack)
{
	uint8_t b[AETHER_ACK_LEN];

	ARG_UNUSED(user);
	aether_ack_encode(ack, b);
	return arq_tx_frame(dst, AETHER_MSG_ACK, 0, 0, b, AETHER_ACK_LEN);
}

static aether_reliable_recv_cb_t g_rel_recv_cb;
static void *g_rel_recv_user;

void aether_reliable_set_recv_cb(aether_reliable_recv_cb_t cb, void *user)
{
	g_rel_recv_cb = cb;
	g_rel_recv_user = user;
}

static void arq_cb_deliver(void *user, const uint8_t src[6],
			   const uint8_t *data, uint16_t len)
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;

	ARG_UNUSED(user);
	/* Reliable deliveries go to the registered reliable handler (the
	 * /net/aether server); fall back to the best-effort recv callback so
	 * nothing is silently dropped if no handler is set. */
	if (g_rel_recv_cb) {
		g_rel_recv_cb(src, data, len, g_rel_recv_user);
		return;
	}
	if (ctx && ctx->recv_cb) {
		ctx->recv_cb(ctx->iface, src, data, len, ctx->recv_cb_user_data);
	}
}

static void arq_cb_failed(void *user, const uint8_t dst[6], uint8_t seq)
{
	ARG_UNUSED(user);
	LOG_WRN("reliable delivery failed to %02x:%02x seq=%u",
		dst[4], dst[5], seq);
}

static void arq_tick_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	aether_reliable_tick(&g_arq, k_uptime_get_32());
	if (g_mesh_ctx) {
		k_work_schedule_for_queue(&g_mesh_ctx->mesh_workq,
					  &arq_tick_work, K_MSEC(ARQ_TICK_MS));
	}
}

void aether_reliable_glue_init(void)
{
	struct aether_reliable_cfg cfg = {
		.window = CONFIG_AETHER_RELIABLE_WINDOW,
		.rto_base_ms = CONFIG_AETHER_RELIABLE_RTO_MS,
		.ack_delay_ms = CONFIG_AETHER_RELIABLE_ACK_DELAY_MS,
		.max_retries = CONFIG_AETHER_RELIABLE_MAX_RETRIES,
	};
	struct aether_reliable_io io = {
		.tx_data = arq_cb_tx_data,
		.tx_ack = arq_cb_tx_ack,
		.deliver = arq_cb_deliver,
		.failed = arq_cb_failed,
		.user = NULL,
	};

	aether_reliable_init(&g_arq, &cfg, &io);
	k_work_init_delayable(&arq_tick_work, arq_tick_handler);
	if (g_mesh_ctx) {
		k_work_schedule_for_queue(&g_mesh_ctx->mesh_workq,
					  &arq_tick_work, K_MSEC(ARQ_TICK_MS));
	}
	LOG_INF("Æther reliable datagram engine ready (win=%d rto=%dms)",
		CONFIG_AETHER_RELIABLE_WINDOW, CONFIG_AETHER_RELIABLE_RTO_MS);
}

void aether_reliable_glue_rx_data(const uint8_t src[6], uint8_t seq,
				  bool flow_start, const uint8_t *payload,
				  uint16_t len)
{
	aether_reliable_on_data(&g_arq, src, seq, flow_start, payload, len,
				k_uptime_get_32());
}

void aether_reliable_glue_rx_ack(const uint8_t src[6],
				 const uint8_t *ack_bytes, uint16_t len)
{
	struct aether_ack a;

	if (len < AETHER_ACK_LEN) {
		return;
	}
	aether_ack_decode(ack_bytes, &a);
	aether_reliable_on_ack(&g_arq, src, &a, k_uptime_get_32());
}

/* Reliable datagram send via the §4.6 ARQ engine (unicast, end-to-end
 * ordered + acknowledged). This is the path /net/aether's reliable
 * conversations use. Distinct from the legacy aether_mesh_send_reliable()
 * below, which is a synchronous best-effort send with LoRa-modem retries and
 * is still used for broadcast traffic (chat) and the existing 9P transport. */
int aether_mesh_reliable_send(struct net_if *iface, const uint8_t dst[6],
			      const uint8_t *data, size_t len)
{
	if (!g_mesh_ctx || g_mesh_ctx->iface != iface || !dst || !data) {
		return -EINVAL;
	}
	if (len > AETHER_RELIABLE_MTU) {
		return -E2BIG;
	}
	return aether_reliable_send(&g_arq, dst, data, (uint16_t)len,
				    k_uptime_get_32());
}

/* Legacy synchronous reliable send: builds the frame inline and blocks on
 * aether_lora_send_sync() (modem-level retries). Best-effort delivery class;
 * works for broadcast. Retained for chat, chess, and the current 9P-over-LoRa
 * transport until they migrate to the engine. */
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

	memset(&mesh_hdr, 0, sizeof(mesh_hdr));
	mesh_hdr.type = AETHER_MSG_DATA;
	mesh_hdr.ttl = CONFIG_AETHER_DEFAULT_TTL;
	memcpy(mesh_hdr.src, ctx->local_addr, 6);
	memcpy(mesh_hdr.dst, dst, 6);
	mesh_hdr.seq_no = ctx->seq_no++;
	mesh_hdr.qos_security = (AETHER_PRIORITY_NORMAL << 6) |
				(AETHER_DELIV_BESTEFFORT << 5);
	mesh_hdr.length = len;

	uint16_t heymac_net_id = heymac_get_network_id(iface);
	if (CONFIG_AETHER_CONDITIONAL_NET_ID && ctx->mesh_net_id != heymac_net_id) {
		mesh_hdr.flags |= AETHER_FCTL_MESH_NET_ID;
		mesh_hdr.mesh_net_id = ctx->mesh_net_id;
	}

	mesh_hdr_len = aether_mesh_build_header(&mesh_hdr, mesh_hdr_buf,
						sizeof(mesh_hdr_buf));
	if (mesh_hdr_len < 0) {
		LOG_ERR("Failed to build mesh header: %d", mesh_hdr_len);
		return mesh_hdr_len;
	}

	struct heymac_context *heymac_ctx = net_if_l2_data(iface);
	if (!heymac_ctx) {
		return -EINVAL;
	}

	/* Frame format: [HeyMac header][Mesh header][Payload] */
	frame_len = 0;
	frame_buf[frame_len++] = HEYMAC_FRM_TYPE_DATA;
	frame_buf[frame_len++] = HEYMAC_FCTL_S | HEYMAC_FCTL_M | HEYMAC_FCTL_N;
	frame_buf[frame_len++] = (heymac_ctx->net_id >> 8) & 0xFF;
	frame_buf[frame_len++] = heymac_ctx->net_id & 0xFF;
	frame_buf[frame_len++] = heymac_ctx->local_addr.short_addr[0];
	frame_buf[frame_len++] = heymac_ctx->local_addr.short_addr[1];
	frame_buf[frame_len++] = heymac_ctx->tx_seq_no++;

	if (frame_len + mesh_hdr_len + len > sizeof(frame_buf)) {
		LOG_ERR("Frame too large: %d",
			frame_len + mesh_hdr_len + (int)len);
		return -ENOMEM;
	}
	memcpy(&frame_buf[frame_len], mesh_hdr_buf, mesh_hdr_len);
	frame_len += mesh_hdr_len;
	memcpy(&frame_buf[frame_len], data, len);
	frame_len += len;

	ret = aether_lora_send_sync(frame_buf, frame_len, max_retries);
	if (ret == 0) {
		LOG_DBG("Reliable send success to %02x:%02x, len=%zu",
			dst[4], dst[5], len);
	} else {
		LOG_WRN("Reliable send failed to %02x:%02x: %d",
			dst[4], dst[5], ret);
	}
	return ret;
}
