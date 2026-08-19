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
#include <zephyr/sys/atomic.h>
#include <string.h>

/* Set while a local DFU streams firmware over the inter-chip UART. Gates DECT
 * mesh RX processing (aether_mesh_recv) + HONR announce/election (aether_honr)
 * so the low-priority flash-write proc thread is not starved by the RX thread
 * (PREEMPT 2) or the mesh workq (COOP 2) -- a wired-UART OTA must never be
 * blocked by radio activity. Cleared on DFU complete/error; the node reboots to
 * apply, so the brief mesh silence is harmless (children tolerate it via the
 * HONR hold-down / orphan hysteresis). Set from aether_9p.c's dfu_status. */
static atomic_t mesh_dfu_active;

bool aether_mesh_dfu_active(void)
{
	return atomic_get(&mesh_dfu_active) != 0;
}

/* Safety net: if a DFU stalls in RECEIVING without a clean complete/error (e.g.
 * the relay stops streaming and never closes the fid), the quiesce would leave
 * the node deaf forever. This watchdog force-resumes the mesh after a bound that
 * comfortably exceeds a healthy flash (a full image streams in ~10s). */
static void mesh_dfu_watchdog_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	if (aether_mesh_dfu_active()) {
		atomic_set(&mesh_dfu_active, 0);
		LOG_WRN("DFU quiesce watchdog fired (>90s) -- force-resuming mesh");
	}
}
static K_WORK_DELAYABLE_DEFINE(mesh_dfu_watchdog, mesh_dfu_watchdog_fn);

void aether_mesh_set_dfu_active(bool on)
{
	atomic_set(&mesh_dfu_active, on ? 1 : 0);
	if (on) {
		k_work_reschedule(&mesh_dfu_watchdog, K_SECONDS(90));
	} else {
		k_work_cancel_delayable(&mesh_dfu_watchdog);
	}
}

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

#if defined(CONFIG_AETHER_ROUTING_HONR)
	/* Bring up HONR tree state (root anchors 0x0000; others join). */
	aether_honr_join_init(iface);
#endif

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
	/* Piggyback the flooded node_eui<->HONR binding beacon on the HELLO cadence
	 * so eui addressing resolves to multi-hop peers, not just direct neighbors. */
	aether_mesh_send_binding(ctx->iface);

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

/* True if addr is on ctx->denylist. Caller must hold neighbor_lock. */
static bool addr_is_denied(struct aether_mesh_ctx *ctx, const uint8_t addr[6])
{
	for (int i = 0; i < ctx->denylist_count; i++) {
		if (memcmp(ctx->denylist[i], addr, 6) == 0) {
			return true;
		}
	}
	return false;
}

int aether_mesh_deny_neighbor(struct net_if *iface, const uint8_t addr[6])
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;
	int ret = 0;

	if (!ctx || ctx->iface != iface || !addr) {
		return -EINVAL;
	}

	k_mutex_lock(&ctx->neighbor_lock, K_FOREVER);

	if (!addr_is_denied(ctx, addr)) {
		if (ctx->denylist_count >= CONFIG_AETHER_MAX_DENYLIST) {
			ret = -ENOMEM;
		} else {
			memcpy(ctx->denylist[ctx->denylist_count], addr, 6);
			ctx->denylist_count++;
		}
	}

	/* Evict immediately if already active -- don't wait for the neighbor
	 * timeout to notice the deny. */
	if (ret == 0) {
		for (int i = 0; i < CONFIG_AETHER_MAX_NEIGHBORS; i++) {
			if (ctx->neighbors[i].active &&
			    memcmp(ctx->neighbors[i].addr, addr, 6) == 0) {
				LOG_INF("Denied neighbor %02x:%02x:%02x:%02x:%02x:%02x evicted",
					addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
				ctx->neighbors[i].active = false;
			}
		}
	}

	k_mutex_unlock(&ctx->neighbor_lock);

	/* Also drop any stale legacy AODV-style route learned directly from the
	 * denied address (aether_mesh_add_route() on HELLO receipt, hop_count=1) --
	 * otherwise it lingers up to CONFIG_AETHER_ROUTE_TIMEOUT even though no new
	 * HELLO from that address can arrive (RX-time drop, see aether_mesh_recv()).
	 * Only the direct 1-hop route is invalidated here; a legitimate multi-hop
	 * route learned via a still-allowed next_hop is untouched. */
	if (ret == 0) {
		k_mutex_lock(&ctx->route_lock, K_FOREVER);
		for (int i = 0; i < CONFIG_AETHER_MAX_ROUTES; i++) {
			if (ctx->routes[i].active &&
			    memcmp(ctx->routes[i].dest, addr, 6) == 0 &&
			    memcmp(ctx->routes[i].next_hop, addr, 6) == 0) {
				LOG_INF("Direct route to denied %02x:%02x:%02x:%02x:%02x:%02x invalidated",
					addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
				ctx->routes[i].active = false;
			}
		}
		k_mutex_unlock(&ctx->route_lock);
	}

	return ret;
}

int aether_mesh_allow_neighbor(struct net_if *iface, const uint8_t addr[6])
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;
	int ret = -ENOENT;

	if (!ctx || ctx->iface != iface || !addr) {
		return -EINVAL;
	}

	k_mutex_lock(&ctx->neighbor_lock, K_FOREVER);
	for (int i = 0; i < ctx->denylist_count; i++) {
		if (memcmp(ctx->denylist[i], addr, 6) == 0) {
			/* Swap-remove */
			ctx->denylist_count--;
			memcpy(ctx->denylist[i], ctx->denylist[ctx->denylist_count], 6);
			ret = 0;
			break;
		}
	}
	k_mutex_unlock(&ctx->neighbor_lock);
	return ret;
}

void aether_mesh_denylist_clear(struct net_if *iface)
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;

	if (!ctx || ctx->iface != iface) {
		return;
	}

	k_mutex_lock(&ctx->neighbor_lock, K_FOREVER);
	ctx->denylist_count = 0;
	k_mutex_unlock(&ctx->neighbor_lock);
}

/* Neighbor management */
int aether_mesh_update_neighbor(struct net_if *iface, const uint8_t addr[6],
                                int8_t rssi, int8_t snr)
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;
	int i, free_slot = -1;

	if (!ctx || ctx->iface != iface || !addr) {
		return -EINVAL;
	}

	k_mutex_lock(&ctx->neighbor_lock, K_FOREVER);

	if (addr_is_denied(ctx, addr)) {
		k_mutex_unlock(&ctx->neighbor_lock);
		return -EACCES;
	}

	/* Look for existing neighbor or free slot */
	for (i = 0; i < CONFIG_AETHER_MAX_NEIGHBORS; i++) {
		if (ctx->neighbors[i].active &&
		    memcmp(ctx->neighbors[i].addr, addr, 6) == 0) {
			/* Update existing neighbor (node_id preserved -- set via announce) */
			ctx->neighbors[i].rssi = rssi;
			ctx->neighbors[i].snr = snr;
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
		memset(ctx->neighbors[free_slot].node_eui, 0, 6);  /* learned later from TREE_ANNOUNCE */
		ctx->neighbors[free_slot].rssi = rssi;
		ctx->neighbors[free_slot].snr = snr;
		ctx->neighbors[free_slot].lqi = 0;
		ctx->neighbors[free_slot].last_seen = k_uptime_get_32();
		ctx->neighbors[free_slot].seq_no = 0;
		ctx->neighbors[free_slot].active = true;
		
		LOG_DBG("Added neighbor %02x:%02x:%02x:%02x:%02x:%02x",
			addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
	}

	k_mutex_unlock(&ctx->neighbor_lock);
	return (free_slot >= 0) ? 0 : -ENOMEM;
}

static const uint8_t k_zero_eui[6];

void aether_mesh_set_neighbor_identity(struct net_if *iface, const uint8_t addr[6],
				       const uint8_t node_eui[6])
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;

	if (!ctx || ctx->iface != iface || !addr || !node_eui ||
	    memcmp(node_eui, k_zero_eui, 6) == 0) {
		return;
	}

	k_mutex_lock(&ctx->neighbor_lock, K_FOREVER);
	for (int i = 0; i < CONFIG_AETHER_MAX_NEIGHBORS; i++) {
		if (ctx->neighbors[i].active &&
		    memcmp(ctx->neighbors[i].addr, addr, 6) == 0) {
			memcpy(ctx->neighbors[i].node_eui, node_eui, 6);
			break;
		}
	}
	k_mutex_unlock(&ctx->neighbor_lock);
}

/* Resolve a durable node_eui (the app-facing endpoint identity) to the peer's
/* How long a flooded binding stays usable without a refreshing beacon. Generous
 * (minutes) vs the neighbor table's ~60s so a briefly-quiet multi-hop peer stays
 * resolvable. */
#define AETHER_BINDING_TTL_MS 300000U

/* Record/refresh a node_eui<->HONR binding learned from a BINDING beacon.
 * Caller must hold neighbor_lock. Upserts by node_eui (stable key); on churn the
 * addr is updated. Evicts the stalest slot when full. */
static void binding_upsert(struct aether_mesh_ctx *ctx, const uint8_t node_eui[6],
			   const uint8_t addr[6])
{
	int free_slot = -1, oldest = 0;
	uint32_t now = k_uptime_get_32();

	if (memcmp(node_eui, k_zero_eui, 6) == 0) {
		return;
	}
	for (int i = 0; i < AETHER_MAX_BINDINGS; i++) {
		if (ctx->bindings[i].valid &&
		    memcmp(ctx->bindings[i].node_eui, node_eui, 6) == 0) {
			memcpy(ctx->bindings[i].addr, addr, 6);
			ctx->bindings[i].last_seen = now;
			return;
		}
		if (!ctx->bindings[i].valid && free_slot < 0) {
			free_slot = i;
		}
		if (ctx->bindings[i].last_seen < ctx->bindings[oldest].last_seen) {
			oldest = i;
		}
	}
	int slot = (free_slot >= 0) ? free_slot : oldest;

	memcpy(ctx->bindings[slot].node_eui, node_eui, 6);
	memcpy(ctx->bindings[slot].addr, addr, 6);
	ctx->bindings[slot].last_seen = now;
	ctx->bindings[slot].valid = true;
}

/* Look up a fresh binding by node_eui (dir=0 -> fill addr_out) or by addr
 * (dir=1 -> fill node_eui out). Caller must hold neighbor_lock. Returns 0/-ENOENT. */
static int binding_lookup(struct aether_mesh_ctx *ctx, int dir,
			  const uint8_t key[6], uint8_t out[6])
{
	uint32_t now = k_uptime_get_32();

	for (int i = 0; i < AETHER_MAX_BINDINGS; i++) {
		if (!ctx->bindings[i].valid ||
		    (now - ctx->bindings[i].last_seen) > AETHER_BINDING_TTL_MS) {
			continue;
		}
		const uint8_t *k = dir == 0 ? ctx->bindings[i].node_eui : ctx->bindings[i].addr;
		const uint8_t *v = dir == 0 ? ctx->bindings[i].addr : ctx->bindings[i].node_eui;

		if (memcmp(k, key, 6) == 0) {
			memcpy(out, v, 6);
			return 0;
		}
	}
	return -ENOENT;
}

/* Resolve a durable node_eui (the app-facing endpoint identity) to the peer's
 * CURRENT HONR routing address, for unicast TX. Bindings come from the flooded
 * BINDING beacon (network-wide, multi-hop) with the direct-neighbor table as a
 * fallback; both track HONR churn, so a caller keyed on the stable eui always
 * routes to the live address. Returns 0 and fills addr_out on a hit; -ENOENT if
 * no binding for this eui is known yet; -EINVAL on bad args. */
int aether_mesh_eui_to_addr(struct net_if *iface, const uint8_t node_eui[6],
			    uint8_t addr_out[6])
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;
	int ret = -ENOENT;

	if (!ctx || ctx->iface != iface || !node_eui || !addr_out ||
	    memcmp(node_eui, k_zero_eui, 6) == 0) {
		return -EINVAL;
	}
	k_mutex_lock(&ctx->neighbor_lock, K_FOREVER);
	/* Neighbor table FIRST: it is the freshest source for a directly-heard peer
	 * (refreshed by every HELLO/TREE_ANNOUNCE, ~seconds). The flooded binding
	 * table is staler (beacon cadence) so it is only a FALLBACK for peers we do
	 * NOT hear directly (multi-hop). Order matters: a stale flooded binding must
	 * never shadow a fresh neighbor entry, or a peer that just re-parented would
	 * resolve to its OLD HONR address and the datagram would misroute. */
	for (int i = 0; i < CONFIG_AETHER_MAX_NEIGHBORS; i++) {
		if (ctx->neighbors[i].active &&
		    memcmp(ctx->neighbors[i].node_eui, node_eui, 6) == 0) {
			memcpy(addr_out, ctx->neighbors[i].addr, 6);
			ret = 0;
			break;
		}
	}
	if (ret != 0) {
		ret = binding_lookup(ctx, 0, node_eui, addr_out);
	}
	k_mutex_unlock(&ctx->neighbor_lock);
	return ret;
}

/* Reverse of the above: resolve a peer's HONR routing address (the src stamped
 * on a received unicast frame) back to its durable node_eui, so the RX path can
 * hand the conversation layer the STABLE identity instead of the churning route.
 * Returns 0/-ENOENT/-EINVAL as above. */
int aether_mesh_addr_to_eui(struct net_if *iface, const uint8_t addr[6],
			    uint8_t eui_out[6])
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;
	int ret = -ENOENT;

	if (!ctx || ctx->iface != iface || !addr || !eui_out) {
		return -EINVAL;
	}
	k_mutex_lock(&ctx->neighbor_lock, K_FOREVER);
	/* Neighbor table first (fresh), flooded binding table as fallback -- same
	 * ordering rationale as aether_mesh_eui_to_addr: never let a stale binding
	 * shadow a fresh direct-neighbor entry. */
	for (int i = 0; i < CONFIG_AETHER_MAX_NEIGHBORS; i++) {
		if (ctx->neighbors[i].active &&
		    memcmp(ctx->neighbors[i].addr, addr, 6) == 0 &&
		    memcmp(ctx->neighbors[i].node_eui, k_zero_eui, 6) != 0) {
			memcpy(eui_out, ctx->neighbors[i].node_eui, 6);
			ret = 0;
			break;
		}
	}
	if (ret != 0) {
		ret = binding_lookup(ctx, 1, addr, eui_out);
	}
	k_mutex_unlock(&ctx->neighbor_lock);
	return ret;
}

/* Forward a unicast frame (DATA or ACK) that is not addressed to us, toward
 * hdr->dst. Stateless HONR next-hop selection: only the node that is actually
 * the next hop from this frame's immediate sender relays; everyone else who
 * overheard it drops, so there is no flood. Consumes pkt in every path (queues
 * it or unrefs it). Shared by the DATA relay and the ACK relay so an
 * end-to-end ACK routes back across the same multi-hop path the DATA took --
 * previously ACKs were never forwarded and died one hop from the destination. */
static void mesh_forward_unicast(struct net_if *iface, struct net_pkt *pkt,
				 struct aether_mesh_hdr *hdr)
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;

	LOG_DBG("Forwarding %s toward %02x:%02x:%02x:%02x:%02x:%02x",
		hdr->type == AETHER_MSG_ACK ? "ACK" : "DATA",
		hdr->dst[0], hdr->dst[1], hdr->dst[2],
		hdr->dst[3], hdr->dst[4], hdr->dst[5]);

#if defined(CONFIG_AETHER_ROUTING_HONR)
	struct heymac_context *hm = net_if_l2_data(iface);
	uint8_t resender[6] = {0};

	if (hm) {
		resender[4] = hm->last_rx_src.short_addr[0];
		resender[5] = hm->last_rx_src.short_addr[1];
	}
	if (!aether_honr_should_relay(iface, resender, hdr->dst)) {
		LOG_DBG("HONR: not the next hop, dropping");
		ctx->packets_dropped++;
		net_pkt_unref(pkt);
		return;
	}
	if (hdr->ttl <= 1) {
		LOG_DBG("Packet TTL expired, dropping");
		ctx->packets_dropped++;
		net_pkt_unref(pkt);
		return;
	}
	hdr->ttl--;
	if (pkt->buffer && pkt->buffer->data) {
		pkt->buffer->data[2] = hdr->ttl;   /* TTL at offset 2 */
	}
	{
		uint8_t nh[6];

		if (aether_honr_next_hop(iface, hdr->dst, nh) == 0) {
			struct net_linkaddr *ll = net_pkt_lladdr_dst(pkt);

			ll->len = 2;
			ll->addr[0] = nh[4];
			ll->addr[1] = nh[5];
		}
	}
	net_if_queue_tx(iface, pkt);
	ctx->packets_forwarded++;
	LOG_DBG("HONR forwarded toward %02x:%02x", hdr->dst[4], hdr->dst[5]);
#else
	if (hdr->ttl <= 1) {
		LOG_DBG("Packet TTL expired, dropping");
		ctx->packets_dropped++;
		net_pkt_unref(pkt);
		return;
	}
	hdr->ttl--;

	uint8_t next_hop[6];

	if (aether_mesh_find_route(iface, hdr->dst, next_hop) < 0) {
		LOG_DBG("No route to destination, dropping");
		ctx->packets_dropped++;
		net_pkt_unref(pkt);
		return;
	}
	if (pkt->buffer && pkt->buffer->data) {
		pkt->buffer->data[2] = hdr->ttl;
	}
	net_if_queue_tx(iface, pkt);
	ctx->packets_forwarded++;
	LOG_DBG("Packet forwarded to next hop");
#endif
}

/* Optional flow-control gate: a higher layer (the mesh 9P server) can refuse a
 * unicast DATA-for-us when its intake ring is momentarily full. Returns true to
 * accept (ACK + deliver), false to drop it BEFORE dedup/ACK so the sender's ARQ
 * retransmits and we accept it once there is room -- lossless backpressure. */
static bool (*g_ack_gate)(const uint8_t *payload, size_t len);

void aether_mesh_set_ack_gate(bool (*gate)(const uint8_t *payload, size_t len))
{
	g_ack_gate = gate;
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

	/* A local DFU is streaming firmware over the inter-chip UART. Drop DECT
	 * mesh RX cheaply (before handle_ctrl / recv_cbs) so the RX thread stops
	 * starving the flash-write proc thread -- radio traffic isn't needed while
	 * we're being reflashed, and the DFU data doesn't arrive over the radio. */
	if (aether_mesh_dfu_active()) {
		net_pkt_unref(pkt);
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

	/* Bench-test denylist: drop the whole packet before any route/neighbor
	 * side effect (HELLO's direct-route add included) -- a denied source
	 * must be completely invisible to routing, not just absent from the
	 * neighbor table.
	 *
	 * Gate on the LINK-LAYER immediate sender (resender), NOT hdr.src (the
	 * mesh-layer original sender). hdr.src is unchanged across a relay hop --
	 * a frame the DK relays from a denied node still carries that node's
	 * hdr.src -- so gating on hdr.src would block the relayed copy too and
	 * make "deny" indistinguishable from "globally silence this identity
	 * everywhere in the mesh," breaking exactly the relay case a deny-based
	 * bench topology test wants to prove. The immediate sender genuinely
	 * differs between "heard this node directly" and "heard the DK repeat
	 * it," which is what deny should actually gate: RF adjacency to THIS
	 * hop, not a global ban on the originator's identity. */
	struct heymac_context *hm_deny = net_if_l2_data(iface);
	uint8_t resender[6] = {0};

	if (hm_deny) {
		resender[4] = hm_deny->last_rx_src.short_addr[0];
		resender[5] = hm_deny->last_rx_src.short_addr[1];
	}
	k_mutex_lock(&ctx->neighbor_lock, K_FOREVER);
	bool denied = addr_is_denied(ctx, resender);
	k_mutex_unlock(&ctx->neighbor_lock);
	if (denied) {
		LOG_DBG("Dropping packet heard directly from denied %02x:%02x "
			"(orig src %02x:%02x:%02x:%02x:%02x:%02x)",
			resender[4], resender[5],
			hdr.src[0], hdr.src[1], hdr.src[2],
			hdr.src[3], hdr.src[4], hdr.src[5]);
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

	/* Parent liveness: any valid frame heard directly from our HONR parent keeps
	 * the tree above us alive -- the parent's unicast/ACK/HELLO traffic proves it
	 * is up just as well as its best-effort broadcast TREE_ANNOUNCE, which is easily
	 * lost under load/half-duplex/high-density and was orphaning healthy children. */
	aether_honr_note_rx(resender);

	/* Flow-control gate (before dedup + ACK): if a higher layer refuses this
	 * unicast DATA-for-us (its ring is full), drop it now WITHOUT recording the
	 * seq or ACKing -- the sender retransmits (same seq, still fresh to us) and we
	 * take it once there is room. Lossless backpressure vs a silent overflow drop. */
	if (g_ack_gate && hdr.type == AETHER_MSG_DATA &&
	    memcmp(hdr.dst, ctx->local_addr, 6) == 0) {
		const uint8_t *pl = buf + hdr_len;
		size_t pll = (hdr.length && (size_t)hdr.length <= len - hdr_len)
			     ? hdr.length : (len - hdr_len);

		if (!g_ack_gate(pl, pll)) {
			net_pkt_unref(pkt);
			return;
		}
	}

	/* Duplicate detection (skip hello beacons; ACKs are idempotent and exempt so
	 * a retransmit-driven re-ACK is never dropped as a duplicate). */
	if (hdr.type != AETHER_MSG_HELLO && hdr.type != AETHER_MSG_ACK &&
	    is_duplicate(hdr.src, hdr.seq_no)) {
		/* A duplicate DATA for us means our earlier ACK was lost and the sender
		 * retransmitted -- re-ACK so it stops, then drop (do not re-deliver). */
		if (hdr.type == AETHER_MSG_DATA &&
		    memcmp(hdr.dst, ctx->local_addr, 6) == 0) {
			aether_mesh_send_ack(iface, hdr.src, hdr.seq_no);
		}
		LOG_INF("Dropping duplicate from %02x:%02x:%02x:%02x:%02x:%02x seq=%d",
			hdr.src[0], hdr.src[1], hdr.src[2],
			hdr.src[3], hdr.src[4], hdr.src[5], hdr.seq_no);
		net_pkt_unref(pkt);
		return;
	}

	/* Update neighbor information from source */
	/* Real RSSI/SNR from the PHY: the driver stashed it on the heymac context
	 * for this frame (set in the single-threaded RX path right before recv). */
	{
		struct heymac_context *hm_rx = net_if_l2_data(iface);

		aether_mesh_update_neighbor(iface, hdr.src,
					    hm_rx ? hm_rx->last_rx_rssi : 0,
					    hm_rx ? hm_rx->last_rx_snr : 0);
	}

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

	case AETHER_MSG_ACK:
		/* Stop-and-wait ARQ. If it's for us, wake the waiting sender;
		 * otherwise relay it toward the original sender (multi-hop ACK). */
		if (memcmp(hdr.dst, ctx->local_addr, 6) == 0) {
			aether_mesh_ack_received(hdr.src, hdr.seq_no);
			net_pkt_unref(pkt);
		} else {
			mesh_forward_unicast(iface, pkt, &hdr);
		}
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
		/* Check if packet is for us. Broadcast frames carry our durable identity
		 * (node_eui) as src, not the HONR local_addr, so the own-echo test must
		 * compare against node_eui -- otherwise two nodes sharing a HONR address
		 * (e.g. transient co-roots at 0x0000) would drop each other's broadcasts
		 * as "self". */
		bool dst_is_bcast =
			memcmp(hdr.dst, "\xff\xff\xff\xff\xff\xff", 6) == 0;
		bool src_is_self = dst_is_bcast
			? memcmp(hdr.src, ctx->node_eui, 6) == 0
			: memcmp(hdr.src, ctx->local_addr, 6) == 0;

		if (memcmp(hdr.dst, ctx->local_addr, 6) == 0 || dst_is_bcast) {
			/* Our own broadcast echoed back by a relay: ignore (we already
			 * delivered/displayed it locally when we originated it). */
			if (dst_is_bcast && src_is_self) {
				net_pkt_unref(pkt);
				break;
			}
			/* Unicast datagram for us: ACK it before delivering (stop-and-wait
			 * ARQ). Broadcasts are not ACKed (no single acker). */
			if (!dst_is_bcast) {
				aether_mesh_send_ack(iface, hdr.src, hdr.seq_no);
			}
			/* Broadcast relay: party-line traffic must traverse multiple hops.
			 * Tree-aware (HONR) -- only an *interior* node (>=1 child) relays;
			 * a leaf has nothing downstream, so its retransmit is pure waste.
			 * This rides the HONR tree's up/down structure instead of a blind
			 * flood: the originator transmits once, each interior node forwards
			 * once toward its subtree, and coverage is complete because every
			 * interior node on the path up to (and back down from) the root
			 * relays. In a star this collapses from N+1 transmits to 1. Dedup
			 * (is_duplicate, per (src,seq)) + TTL remain as safety nets for tree
			 * transients (mid-join / re-parent). Without HONR there is no tree,
			 * so fall back to the dedup-bounded flood (every node relays once). */
#if defined(CONFIG_AETHER_ROUTING_HONR)
			bool relay_bcast = (ctx->honr_child_bitmap != 0);
#else
			bool relay_bcast = true;
#endif
			if (dst_is_bcast && hdr.ttl > 1 && relay_bcast) {
				struct net_pkt *fwd = net_pkt_clone(pkt, K_NO_WAIT);

				if (fwd) {
					if (fwd->buffer && fwd->buffer->data) {
						fwd->buffer->data[2] = hdr.ttl - 1; /* TTL at off 2 */
					}
					/* Unset link dst -> driver re-broadcasts (type-1). */
					net_pkt_lladdr_dst(fwd)->len = 0;
					net_if_queue_tx(iface, fwd);
					ctx->packets_forwarded++;
				}
			}
			/* Remove mesh header */
			net_pkt_pull(pkt, hdr_len);

			/* Call application callback if registered */
			if (ctx->recv_cb_count > 0) {
				/* hdr.length is the real datagram length the sender stamped;
				 * net_pkt_get_len() is the fixed DECT PHY slot (payload +
				 * trailing padding). Deliver hdr.length, clamped to the present
				 * length, so callers get the real payload not the padded frame. */
				size_t avail = net_pkt_get_len(pkt);
				size_t payload_len = hdr.length < avail ? hdr.length : avail;
				uint8_t *payload = pkt->buffer ? pkt->buffer->data : NULL;

				if (payload && payload_len > 0) {
					for (uint8_t cbi = 0; cbi < ctx->recv_cb_count; cbi++) {
						ctx->recv_cbs[cbi](iface, hdr.src, payload, payload_len,
								   dst_is_bcast, ctx->recv_cb_users[cbi]);
					}
				}
			}
			net_pkt_unref(pkt);
		} else {
			/* Not for us -- relay toward hdr.dst (shared with ACK relay). */
			mesh_forward_unicast(iface, pkt, &hdr);
		}
		break;

#if defined(CONFIG_AETHER_ROUTING_HONR)
	case AETHER_MSG_TREE_ANNOUNCE:
	case AETHER_MSG_TREE_JOIN_REQ:
	case AETHER_MSG_TREE_JOIN_REP:
		aether_honr_handle_ctrl(iface, hdr.type, hdr.src, buf + hdr_len, hdr.length);
		net_pkt_unref(pkt);
		break;
#endif

	case AETHER_MSG_BINDING:
		/* Flooded node_eui<->HONR binding beacon: payload is the originator's
		 * durable node_eui, hdr.src its current HONR routing address. Record it
		 * (lets us resolve this peer even when it is several hops away -- the
		 * neighbor table only covers direct RF neighbors), then re-flood once
		 * with TTL-1. The (src,seq) dedup above bounds it to one relay per node.
		 * Skip our own beacon echoed back. */
		if (hdr.length >= 6 &&
		    memcmp(buf + hdr_len, ctx->node_eui, 6) != 0) {
			k_mutex_lock(&ctx->neighbor_lock, K_FOREVER);
			binding_upsert(ctx, buf + hdr_len, hdr.src);
			k_mutex_unlock(&ctx->neighbor_lock);

			if (hdr.ttl > 1) {
				struct net_pkt *fwd = net_pkt_clone(pkt, K_NO_WAIT);

				if (fwd) {
					if (fwd->buffer && fwd->buffer->data) {
						fwd->buffer->data[2] = hdr.ttl - 1; /* TTL at off 2 */
					}
					net_pkt_lladdr_dst(fwd)->len = 0; /* re-broadcast (type-1) */
					net_if_queue_tx(iface, fwd);
					ctx->packets_forwarded++;
				}
			}
		}
		net_pkt_unref(pkt);
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

	if (ctx->recv_cb_count >= 4) {
		return -ENOMEM;
	}
	ctx->recv_cbs[ctx->recv_cb_count] = cb;
	ctx->recv_cb_users[ctx->recv_cb_count] = user_data;
	ctx->recv_cb_count++;

	LOG_DBG("Receive callback registered");
	return 0;
}
