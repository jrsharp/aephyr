/*
 * Copyright (c) 2025 Æther Authors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(aether_route, CONFIG_AETHER_MESH_LOG_LEVEL);

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net/aether_mesh.h>
#include <string.h>

/* External reference to global mesh context */
extern struct aether_mesh_ctx *g_mesh_ctx;

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
	uint16_t heymac_net_id = heymac_get_network_id(iface);
	if (CONFIG_AETHER_CONDITIONAL_NET_ID && ctx->mesh_net_id != heymac_net_id) {
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

	/* Find route to destination */
	ret = aether_mesh_find_route(iface, dst, next_hop);
	if (ret < 0) {
		/* No route found - initiate route discovery */
		LOG_DBG("No route to destination, sending RREQ");
		return aether_mesh_send_rreq(iface, dst);
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
	uint16_t heymac_net_id = heymac_get_network_id(iface);
	if (CONFIG_AETHER_CONDITIONAL_NET_ID && ctx->mesh_net_id != heymac_net_id) {
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