/*
 * Copyright (c) 2025 Æther Authors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(aether_mesh, CONFIG_AETHER_MESH_LOG_LEVEL);

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
/* Fallback address/network ID functions for non-HeyMac transports */
static int aether_get_addr_fallback(struct net_if *iface, uint8_t addr[6])
{
	ARG_UNUSED(iface);
	/* Generate random locally-administered address */
	for (int i = 0; i < 6; i++) {
		addr[i] = sys_rand32_get() & 0xFF;
	}
	addr[0] |= 0x02;  /* Locally administered */
	addr[0] &= ~0x01; /* Unicast */
	return 0;
}

static uint16_t aether_get_network_id_fallback(struct net_if *iface)
{
	ARG_UNUSED(iface);
	return 0x0001;  /* Default network ID */
}
#endif /* !CONFIG_NET_L2_HEYMAC */

/* Global mesh context - one per interface */
struct aether_mesh_ctx *g_mesh_ctx;

/* Work queue stack for mesh operations - needs room for send_hello() nested calls */
#define MESH_WORKQ_STACK_SIZE 1536
K_THREAD_STACK_DEFINE(mesh_workq_stack, MESH_WORKQ_STACK_SIZE);

/* Duplicate detection - ring buffer of recently seen (source, seq_no) pairs */
#define DEDUP_CACHE_SIZE 16
struct dedup_entry {
	uint8_t src[6];
	uint8_t seq_no;
	uint32_t timestamp;
};
static struct dedup_entry dedup_cache[DEDUP_CACHE_SIZE];
static uint8_t dedup_idx;

/* Check if packet is duplicate; returns true if duplicate, false if new */
static bool is_duplicate(const uint8_t src[6], uint8_t seq_no)
{
	uint32_t now = k_uptime_get_32();

	/* Check cache for matching entry (within 10 second window) */
	for (int i = 0; i < DEDUP_CACHE_SIZE; i++) {
		if (dedup_cache[i].timestamp != 0 &&
		    (now - dedup_cache[i].timestamp) < 10000 &&
		    dedup_cache[i].seq_no == seq_no &&
		    memcmp(dedup_cache[i].src, src, 6) == 0) {
			return true;  /* Duplicate */
		}
	}

	/* Not found - add to cache */
	memcpy(dedup_cache[dedup_idx].src, src, 6);
	dedup_cache[dedup_idx].seq_no = seq_no;
	dedup_cache[dedup_idx].timestamp = now;
	dedup_idx = (dedup_idx + 1) % DEDUP_CACHE_SIZE;

	return false;  /* New packet */
}

/* Forward declarations */
static void aether_hello_work_handler(struct k_work *work);
static void aether_route_maintenance_handler(struct k_work *work);
static int aether_mesh_parse_header(const uint8_t *buf, size_t len, 
                                    struct aether_mesh_hdr *hdr);
int aether_mesh_build_header(const struct aether_mesh_hdr *hdr,
                             uint8_t *buf, size_t len);

/* Mesh header parsing */
static int aether_mesh_parse_header(const uint8_t *buf, size_t len,
                                    struct aether_mesh_hdr *hdr)
{
	const uint8_t *ptr = buf;
	size_t pos = 0;

	if (!buf || !hdr || len < 16) {
		return -EINVAL;
	}

	memset(hdr, 0, sizeof(*hdr));

	/* Parse basic header fields */
	hdr->flags = ptr[pos++];
	hdr->type = ptr[pos++];
	hdr->ttl = ptr[pos++];
	
	/* Copy addresses */
	memcpy(hdr->src, &ptr[pos], 6);
	pos += 6;
	memcpy(hdr->dst, &ptr[pos], 6);
	pos += 6;
	
	hdr->seq_no = ptr[pos++];

	/* Parse conditional mesh network ID */
	if (hdr->flags & AETHER_FCTL_MESH_NET_ID) {
		if (pos + 2 > len) {
			return -EMSGSIZE;
		}
		hdr->mesh_net_id = sys_get_be16(&ptr[pos]);
		pos += 2;
	}

	/* Parse QoS/Security and length */
	if (pos + 3 > len) {
		return -EMSGSIZE;
	}
	hdr->qos_security = ptr[pos++];
	hdr->length = sys_get_be16(&ptr[pos]);
	pos += 2;

	return pos;  /* Return header size */
}

int aether_mesh_build_header(const struct aether_mesh_hdr *hdr,
                             uint8_t *buf, size_t len)
{
	uint8_t *ptr = buf;
	size_t pos = 0;
	size_t expected_size = aether_mesh_hdr_size(hdr->flags);

	if (!hdr || !buf || len < expected_size) {
		return -EINVAL;
	}

	/* Write basic header fields */
	ptr[pos++] = hdr->flags;
	ptr[pos++] = hdr->type;
	ptr[pos++] = hdr->ttl;
	
	/* Copy addresses */
	memcpy(&ptr[pos], hdr->src, 6);
	pos += 6;
	memcpy(&ptr[pos], hdr->dst, 6);
	pos += 6;
	
	ptr[pos++] = hdr->seq_no;

	/* Write conditional mesh network ID */
	if (hdr->flags & AETHER_FCTL_MESH_NET_ID) {
		sys_put_be16(hdr->mesh_net_id, &ptr[pos]);
		pos += 2;
	}

	/* Write QoS/Security and length */
	ptr[pos++] = hdr->qos_security;
	sys_put_be16(hdr->length, &ptr[pos]);
	pos += 2;

	return pos;  /* Return header size */
}

/* Mesh context management */
int aether_mesh_init(struct net_if *iface, uint16_t mesh_net_id)
{
	struct aether_mesh_ctx *ctx;
	int ret;

	if (!iface) {
		return -EINVAL;
	}

	/* Allocate mesh context */
	ctx = k_malloc(sizeof(*ctx));
	if (!ctx) {
		return -ENOMEM;
	}

	memset(ctx, 0, sizeof(*ctx));
	ctx->iface = iface;
	ctx->mesh_net_id = mesh_net_id;
	ctx->seq_no = sys_rand32_get() & 0xFF;

	/* Get local address from L2 layer */
#ifdef CONFIG_NET_L2_HEYMAC
	{
		struct heymac_addr local_addr;
		ret = heymac_get_addr(iface, &local_addr);
		if (ret < 0) {
			LOG_ERR("Failed to get HeyMac address: %d", ret);
			k_free(ctx);
			return ret;
		}
		/* Convert address to mesh format (always use 6-byte addresses) */
		if (heymac_addr_is_short(&local_addr)) {
			/* Expand short address to 6 bytes */
			memset(ctx->local_addr, 0, 4);
			memcpy(&ctx->local_addr[4], local_addr.short_addr, 2);
		} else {
			memcpy(ctx->local_addr, local_addr.long_addr, 6);
		}
	}
#else
	/* Use fallback for non-HeyMac transports (e.g., UART) */
	ret = aether_get_addr_fallback(iface, ctx->local_addr);
	if (ret < 0) {
		LOG_ERR("Failed to get local address: %d", ret);
		k_free(ctx);
		return ret;
	}
#endif

	/* Initialize mutexes */
	k_mutex_init(&ctx->neighbor_lock);
	k_mutex_init(&ctx->route_lock);

	/* Initialize work queue and work items */
	k_work_queue_init(&ctx->mesh_workq);
	k_work_queue_start(&ctx->mesh_workq, mesh_workq_stack,
			   K_THREAD_STACK_SIZEOF(mesh_workq_stack),
			   K_PRIO_COOP(2), NULL);

	k_work_init_delayable(&ctx->hello_work, aether_hello_work_handler);
	k_work_init_delayable(&ctx->route_maintenance_work, aether_route_maintenance_handler);

	/* Start periodic hello messages */
	k_work_schedule_for_queue(&ctx->mesh_workq, &ctx->hello_work,
				  K_SECONDS(CONFIG_AETHER_HELLO_INTERVAL));

	/* Start route maintenance */
	k_work_schedule_for_queue(&ctx->mesh_workq, &ctx->route_maintenance_work,
				  K_SECONDS(60));

	g_mesh_ctx = ctx;

	LOG_INF("Æther mesh initialized, net_id=0x%04x", mesh_net_id);
	return 0;
}

/* Hello message handling with adaptive intervals */
static void aether_hello_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct aether_mesh_ctx *ctx = CONTAINER_OF(dwork, struct aether_mesh_ctx, hello_work);
	uint32_t now = k_uptime_get_32();
	int stable_neighbors = 0;

	aether_mesh_send_hello(ctx->iface);

	/* Count stable neighbors (active, seen recently, good signal) */
	k_mutex_lock(&ctx->neighbor_lock, K_FOREVER);
	for (int i = 0; i < CONFIG_AETHER_MAX_NEIGHBORS; i++) {
		if (ctx->neighbors[i].active &&
		    (now - ctx->neighbors[i].last_seen) < 60000 &&  /* Seen in last 60s */
		    ctx->neighbors[i].rssi > -90) {  /* Reasonable signal */
			stable_neighbors++;
		}
	}
	k_mutex_unlock(&ctx->neighbor_lock);

	/* Adaptive interval: longer if we have stable neighbors */
	int32_t base_interval_ms;
	if (stable_neighbors > 0) {
		/* Have stable neighbors - use longer interval (60s) */
		base_interval_ms = 60000;
	} else {
		/* No stable neighbors - discovery mode (30s) */
		base_interval_ms = CONFIG_AETHER_HELLO_INTERVAL * 1000;
	}

	/* Staggered beacon scheduling: deterministic slot offset from MAC address
	 * (FNV-1a hash) spreads nodes across the interval, plus small random
	 * jitter to handle hash collisions. */
	uint32_t hash = 2166136261u;
	for (int i = 0; i < 6; i++) {
		hash ^= ctx->local_addr[i];
		hash *= 16777619u;
	}
	int32_t slot_range = base_interval_ms / 4;  /* ±25% of interval */
	int32_t slot_offset = (int32_t)(hash % (2 * slot_range)) - slot_range;
	int32_t jitter = (int32_t)(sys_rand32_get() % 2000) - 1000;  /* ±1s fine jitter */
	int32_t interval_ms = base_interval_ms + slot_offset + jitter;
	if (interval_ms < 5000) {
		interval_ms = 5000;
	}

	LOG_DBG("Next hello in %dms (stable_neighbors=%d, slot_offset=%d)",
		interval_ms, stable_neighbors, slot_offset);
	k_work_schedule_for_queue(&ctx->mesh_workq, &ctx->hello_work,
				  K_MSEC(interval_ms));
}

int aether_mesh_send_hello(struct net_if *iface)
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;
	struct aether_mesh_hdr hdr;
	struct net_pkt *pkt;
	uint8_t hdr_buf[32];
	int hdr_len;
	int ret;

	if (!ctx || ctx->iface != iface) {
		return -EINVAL;
	}

	/* Build hello header */
	memset(&hdr, 0, sizeof(hdr));
	hdr.type = AETHER_MSG_HELLO;
	hdr.ttl = 1;  /* Hello messages are single-hop */
	memcpy(hdr.src, ctx->local_addr, 6);
	memset(hdr.dst, 0xFF, 6);  /* Broadcast */
	hdr.seq_no = ctx->seq_no++;
	hdr.qos_security = (AETHER_PRIORITY_LOW << 6) | (AETHER_DELIV_BESTEFFORT << 5);
	hdr.length = 0;  /* No payload */

	/* Optimize network ID */
#ifdef CONFIG_NET_L2_HEYMAC
	uint16_t l2_net_id = heymac_get_network_id(iface);
#else
	uint16_t l2_net_id = aether_get_network_id_fallback(iface);
#endif
	if (CONFIG_AETHER_CONDITIONAL_NET_ID && ctx->mesh_net_id != l2_net_id) {
		hdr.flags |= AETHER_FCTL_MESH_NET_ID;
		hdr.mesh_net_id = ctx->mesh_net_id;
	}

	/* Build header */
	hdr_len = aether_mesh_build_header(&hdr, hdr_buf, sizeof(hdr_buf));
	if (hdr_len < 0) {
		LOG_ERR("Failed to build hello header: %d", hdr_len);
		return hdr_len;
	}

	/* Allocate packet */
	pkt = net_pkt_alloc_with_buffer(iface, hdr_len, AF_UNSPEC, 0, K_NO_WAIT);
	if (!pkt) {
		LOG_ERR("Failed to allocate hello packet");
		return -ENOMEM;
	}

	/* Write header to packet */
	ret = net_pkt_write(pkt, hdr_buf, hdr_len);
	if (ret < 0) {
		LOG_ERR("Failed to write hello header: %d", ret);
		net_pkt_unref(pkt);
		return ret;
	}

	/* Send via HeyMac layer */
	net_if_queue_tx(ctx->iface, pkt);
	/* net_if_queue_tx takes ownership of the packet */

	ctx->hello_sent++;
	LOG_DBG("Hello message sent");
	return 0;
}

/* Route maintenance */
static void aether_route_maintenance_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct aether_mesh_ctx *ctx = CONTAINER_OF(dwork, struct aether_mesh_ctx, 
						   route_maintenance_work);
	uint32_t now = k_uptime_get_32();
	int i;

	/* Expire old neighbors */
	k_mutex_lock(&ctx->neighbor_lock, K_FOREVER);
	for (i = 0; i < CONFIG_AETHER_MAX_NEIGHBORS; i++) {
		if (ctx->neighbors[i].active &&
		    (now - ctx->neighbors[i].last_seen) > 
		    (CONFIG_AETHER_NEIGHBOR_TIMEOUT * 1000)) {
			LOG_DBG("Neighbor %02x:%02x:%02x:%02x:%02x:%02x expired",
				ctx->neighbors[i].addr[0], ctx->neighbors[i].addr[1],
				ctx->neighbors[i].addr[2], ctx->neighbors[i].addr[3],
				ctx->neighbors[i].addr[4], ctx->neighbors[i].addr[5]);
			ctx->neighbors[i].active = false;
		}
	}
	k_mutex_unlock(&ctx->neighbor_lock);

	/* Expire old routes */
	k_mutex_lock(&ctx->route_lock, K_FOREVER);
	for (i = 0; i < CONFIG_AETHER_MAX_ROUTES; i++) {
		if (ctx->routes[i].active &&
		    now > ctx->routes[i].expires) {
			LOG_DBG("Route to %02x:%02x:%02x:%02x:%02x:%02x expired",
				ctx->routes[i].dest[0], ctx->routes[i].dest[1],
				ctx->routes[i].dest[2], ctx->routes[i].dest[3],
				ctx->routes[i].dest[4], ctx->routes[i].dest[5]);
			ctx->routes[i].active = false;
		}
	}
	k_mutex_unlock(&ctx->route_lock);

	/* Schedule next maintenance */
	k_work_schedule_for_queue(&ctx->mesh_workq, &ctx->route_maintenance_work,
				  K_SECONDS(60));
}

/* Neighbor management */
int aether_mesh_update_neighbor(struct net_if *iface, const uint8_t addr[6],
                                int8_t rssi, uint8_t lqi)
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;
	int i, free_slot = -1;

	if (!ctx || ctx->iface != iface || !addr) {
		return -EINVAL;
	}

	k_mutex_lock(&ctx->neighbor_lock, K_FOREVER);

	/* Look for existing neighbor or free slot */
	for (i = 0; i < CONFIG_AETHER_MAX_NEIGHBORS; i++) {
		if (ctx->neighbors[i].active &&
		    memcmp(ctx->neighbors[i].addr, addr, 6) == 0) {
			/* Update existing neighbor */
			ctx->neighbors[i].rssi = rssi;
			ctx->neighbors[i].lqi = lqi;
			ctx->neighbors[i].last_seen = k_uptime_get_32();
			k_mutex_unlock(&ctx->neighbor_lock);
			return 0;
		}
		if (!ctx->neighbors[i].active && free_slot == -1) {
			free_slot = i;
		}
	}

	/* Add new neighbor */
	if (free_slot >= 0) {
		memcpy(ctx->neighbors[free_slot].addr, addr, 6);
		ctx->neighbors[free_slot].rssi = rssi;
		ctx->neighbors[free_slot].lqi = lqi;
		ctx->neighbors[free_slot].last_seen = k_uptime_get_32();
		ctx->neighbors[free_slot].seq_no = 0;
		ctx->neighbors[free_slot].active = true;
		
		LOG_DBG("Added neighbor %02x:%02x:%02x:%02x:%02x:%02x",
			addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
	}

	k_mutex_unlock(&ctx->neighbor_lock);
	return (free_slot >= 0) ? 0 : -ENOMEM;
}

/* Packet reception */
void aether_mesh_recv(struct net_if *iface, struct net_pkt *pkt)
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;
	struct aether_mesh_hdr hdr;
	uint8_t *buf;
	size_t len;
	int hdr_len;

	if (!ctx || ctx->iface != iface || !pkt) {
		LOG_ERR("Invalid mesh reception parameters");
		if (pkt) {
			net_pkt_unref(pkt);
		}
		return;
	}

	len = net_pkt_get_len(pkt);
	if (len < 16) {
		LOG_ERR("Mesh packet too short: %zu bytes", len);
		net_pkt_unref(pkt);
		return;
	}

	/* Use the first buffer's data directly */
	if (!pkt->buffer) {
		LOG_ERR("No buffer in packet");
		net_pkt_unref(pkt);
		return;
	}
	buf = pkt->buffer->data;

	/* Parse mesh header */
	hdr_len = aether_mesh_parse_header(buf, len, &hdr);
	if (hdr_len < 0) {
		LOG_ERR("Failed to parse mesh header: %d", hdr_len);
		net_pkt_unref(pkt);
		return;
	}

	/* Check TTL */
	if (hdr.ttl == 0) {
		LOG_DBG("Dropping packet with TTL=0");
		net_pkt_unref(pkt);
		return;
	}

	/* Validate mesh network ID if present */
	if ((hdr.flags & AETHER_FCTL_MESH_NET_ID) &&
	    hdr.mesh_net_id != ctx->mesh_net_id) {
		LOG_DBG("Mesh network ID mismatch: got 0x%04x, expected 0x%04x",
			hdr.mesh_net_id, ctx->mesh_net_id);
		net_pkt_unref(pkt);
		return;
	}

	/* Duplicate detection for data messages (skip for hello beacons) */
	if (hdr.type != AETHER_MSG_HELLO && is_duplicate(hdr.src, hdr.seq_no)) {
		LOG_INF("Dropping duplicate from %02x:%02x:%02x:%02x:%02x:%02x seq=%d",
			hdr.src[0], hdr.src[1], hdr.src[2],
			hdr.src[3], hdr.src[4], hdr.src[5], hdr.seq_no);
		net_pkt_unref(pkt);
		return;
	}

	/* Update neighbor information from source */
	aether_mesh_update_neighbor(iface, hdr.src, -50, 255);  /* TODO: get real RSSI/LQI */

	/* Handle different message types */
	switch (hdr.type) {
	case AETHER_MSG_HELLO:
		LOG_DBG("Received hello from %02x:%02x:%02x:%02x:%02x:%02x",
			hdr.src[0], hdr.src[1], hdr.src[2], 
			hdr.src[3], hdr.src[4], hdr.src[5]);
		ctx->hello_received++;
		/* Add direct route to hello sender */
		aether_mesh_add_route(iface, hdr.src, hdr.src, 1);
		net_pkt_unref(pkt);
		break;
	
	case AETHER_MSG_RREQ:
		LOG_DBG("Received RREQ from %02x:%02x:%02x:%02x:%02x:%02x",
			hdr.src[0], hdr.src[1], hdr.src[2], 
			hdr.src[3], hdr.src[4], hdr.src[5]);
		/* TODO: Process RREQ - check if we have route to target */
		/* TODO: If we are the target, send RREP */
		/* TODO: If we have route, send RREP */
		/* TODO: Otherwise, rebroadcast RREQ with incremented hop count */
		net_pkt_unref(pkt);
		break;
	
	case AETHER_MSG_RREP:
		LOG_DBG("Received RREP from %02x:%02x:%02x:%02x:%02x:%02x",
			hdr.src[0], hdr.src[1], hdr.src[2], 
			hdr.src[3], hdr.src[4], hdr.src[5]);
		/* TODO: Process RREP - update routing table */
		/* TODO: Forward RREP toward originator if not for us */
		net_pkt_unref(pkt);
		break;
	
	case AETHER_MSG_RERR:
		LOG_DBG("Received RERR from %02x:%02x:%02x:%02x:%02x:%02x",
			hdr.src[0], hdr.src[1], hdr.src[2], 
			hdr.src[3], hdr.src[4], hdr.src[5]);
		/* TODO: Process RERR - remove broken routes */
		/* TODO: Propagate RERR to affected nodes */
		net_pkt_unref(pkt);
		break;

	case AETHER_MSG_DATA:
		LOG_INF("mesh DATA from %02x:%02x:%02x:%02x:%02x:%02x "
			"to %02x:%02x:%02x:%02x:%02x:%02x "
			"(local=%02x:%02x:%02x:%02x:%02x:%02x)",
			hdr.src[0], hdr.src[1], hdr.src[2],
			hdr.src[3], hdr.src[4], hdr.src[5],
			hdr.dst[0], hdr.dst[1], hdr.dst[2],
			hdr.dst[3], hdr.dst[4], hdr.dst[5],
			ctx->local_addr[0], ctx->local_addr[1],
			ctx->local_addr[2], ctx->local_addr[3],
			ctx->local_addr[4], ctx->local_addr[5]);
		/* Check if packet is for us */
		if (memcmp(hdr.dst, ctx->local_addr, 6) == 0 ||
		    memcmp(hdr.dst, "\xff\xff\xff\xff\xff\xff", 6) == 0) {
			/* Remove mesh header */
			net_pkt_pull(pkt, hdr_len);

			/* Call application callback if registered */
			if (ctx->recv_cb) {
				size_t payload_len = net_pkt_get_len(pkt);
				uint8_t *payload = pkt->buffer ? pkt->buffer->data : NULL;

				if (payload && payload_len > 0) {
					ctx->recv_cb(iface, hdr.src, payload,
						     payload_len, ctx->recv_cb_user_data);
				}
			}
			net_pkt_unref(pkt);
		} else {
			/* Forward packet */
			LOG_DBG("Forwarding packet to %02x:%02x:%02x:%02x:%02x:%02x",
				hdr.dst[0], hdr.dst[1], hdr.dst[2],
				hdr.dst[3], hdr.dst[4], hdr.dst[5]);
			
			/* Decrement TTL and check if packet should be dropped */
			if (hdr.ttl <= 1) {
				LOG_DBG("Packet TTL expired, dropping");
				ctx->packets_dropped++;
				net_pkt_unref(pkt);
				break;
			}
			hdr.ttl--;
			
			/* Find route to destination */
			uint8_t next_hop[6];
			int ret = aether_mesh_find_route(iface, hdr.dst, next_hop);
			if (ret < 0) {
				LOG_DBG("No route to destination, dropping");
				ctx->packets_dropped++;
				net_pkt_unref(pkt);
				break;
			}
			
			/* Update mesh header with decremented TTL */
			if (pkt->buffer && pkt->buffer->data) {
				pkt->buffer->data[2] = hdr.ttl;  /* TTL is at offset 2 in mesh header */
			}
			
			/* Forward packet via L2 layer */
			net_if_queue_tx(iface, pkt);
			ctx->packets_forwarded++;
			LOG_DBG("Packet forwarded to next hop");
		}
		break;

	default:
		LOG_DBG("Received unknown mesh message type: %d", hdr.type);
		net_pkt_unref(pkt);
		break;
	}
}

/* API functions */
int aether_mesh_get_neighbor_count(struct net_if *iface)
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;
	int count = 0;
	int i;

	if (!ctx || ctx->iface != iface) {
		return -EINVAL;
	}

	k_mutex_lock(&ctx->neighbor_lock, K_FOREVER);
	for (i = 0; i < CONFIG_AETHER_MAX_NEIGHBORS; i++) {
		if (ctx->neighbors[i].active) {
			count++;
		}
	}
	k_mutex_unlock(&ctx->neighbor_lock);

	return count;
}

int aether_mesh_get_route_count(struct net_if *iface)
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;
	int count = 0;
	int i;

	if (!ctx || ctx->iface != iface) {
		return -EINVAL;
	}

	k_mutex_lock(&ctx->route_lock, K_FOREVER);
	for (i = 0; i < CONFIG_AETHER_MAX_ROUTES; i++) {
		if (ctx->routes[i].active) {
			count++;
		}
	}
	k_mutex_unlock(&ctx->route_lock);

	return count;
}

int aether_mesh_register_recv_callback(struct net_if *iface,
				       aether_mesh_recv_cb_t cb,
				       void *user_data)
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;

	if (!ctx || ctx->iface != iface) {
		return -EINVAL;
	}

	ctx->recv_cb = cb;
	ctx->recv_cb_user_data = user_data;

	LOG_DBG("Receive callback registered");
	return 0;
}
