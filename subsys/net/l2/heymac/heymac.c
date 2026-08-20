/*
 * Copyright (c) 2025 Æther Authors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(heymac, CONFIG_NET_L2_HEYMAC_LOG_LEVEL);

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net/net_core.h>
#include <zephyr/net/heymac.h>
#include <zephyr/random/random.h>
#include <string.h>

/* Forward declaration for HSM integration */
extern void heymac_state_machine_run(struct heymac_context *ctx, enum heymac_event event);

/* HeyMac frame parsing and building functions */

int heymac_parse_header(const uint8_t *buf, size_t len, struct heymac_hdr *hdr)
{
	const uint8_t *ptr = buf;
	size_t pos = 0;

	if (!buf || !hdr || len < 2) {
		return -EINVAL;
	}

	memset(hdr, 0, sizeof(*hdr));

	/* Parse version/type and frame control */
	hdr->version_type = ptr[pos++];
	hdr->frame_ctrl = ptr[pos++];

	/* Parse network ID if present */
	if (hdr->frame_ctrl & HEYMAC_FCTL_N) {
		if (pos + 2 > len) {
			return -EMSGSIZE;
		}
		hdr->net_id = sys_get_be16(&ptr[pos]);
		pos += 2;
	}

	/* Parse destination address if present */
	if (hdr->frame_ctrl & HEYMAC_FCTL_D) {
		size_t addr_len = (hdr->frame_ctrl & HEYMAC_FCTL_L) ? 
				  HEYMAC_ADDR_LONG_LEN : HEYMAC_ADDR_SHORT_LEN;
		
		if (pos + addr_len > len) {
			return -EMSGSIZE;
		}
		
		hdr->dst_addr.type = (hdr->frame_ctrl & HEYMAC_FCTL_L) ? 1 : 0;
		memcpy(hdr->dst_addr.long_addr, &ptr[pos], addr_len);
		pos += addr_len;
	}

	/* Parse source address if present */
	if (hdr->frame_ctrl & HEYMAC_FCTL_S) {
		size_t addr_len = (hdr->frame_ctrl & HEYMAC_FCTL_L) ? 
				  HEYMAC_ADDR_LONG_LEN : HEYMAC_ADDR_SHORT_LEN;
		
		if (pos + addr_len > len) {
			return -EMSGSIZE;
		}
		
		hdr->src_addr.type = (hdr->frame_ctrl & HEYMAC_FCTL_L) ? 1 : 0;
		memcpy(hdr->src_addr.long_addr, &ptr[pos], addr_len);
		pos += addr_len;
	}

	/* Parse sequence number */
	if (pos >= len) {
		return -EMSGSIZE;
	}
	hdr->seq_no = ptr[pos++];

	return pos;  /* Return header size */
}

int heymac_build_header(const struct heymac_hdr *hdr, uint8_t *buf, size_t len)
{
	uint8_t *ptr = buf;
	size_t pos = 0;

	if (!hdr || !buf) {
		return -EINVAL;
	}

	/* Check minimum buffer size */
	if (len < 3) {  /* version_type + frame_ctrl + seq_no minimum */
		return -EMSGSIZE;
	}

	/* Write version/type and frame control */
	ptr[pos++] = hdr->version_type;
	ptr[pos++] = hdr->frame_ctrl;

	/* Write network ID if present */
	if (hdr->frame_ctrl & HEYMAC_FCTL_N) {
		if (pos + 2 > len) {
			return -EMSGSIZE;
		}
		sys_put_be16(hdr->net_id, &ptr[pos]);
		pos += 2;
	}

	/* Write destination address if present */
	if (hdr->frame_ctrl & HEYMAC_FCTL_D) {
		size_t addr_len = heymac_addr_len(&hdr->dst_addr);
		
		if (pos + addr_len > len) {
			return -EMSGSIZE;
		}
		
		memcpy(&ptr[pos], hdr->dst_addr.long_addr, addr_len);
		pos += addr_len;
	}

	/* Write source address if present */
	if (hdr->frame_ctrl & HEYMAC_FCTL_S) {
		size_t addr_len = heymac_addr_len(&hdr->src_addr);
		
		if (pos + addr_len > len) {
			return -EMSGSIZE;
		}
		
		memcpy(&ptr[pos], hdr->src_addr.long_addr, addr_len);
		pos += addr_len;
	}

	/* Write sequence number */
	if (pos >= len) {
		return -EMSGSIZE;
	}
	ptr[pos++] = hdr->seq_no;

	return pos;  /* Return header size */
}

/* Work handlers for Zephyr integration */
static void heymac_beacon_work_handler(struct k_work *work);
static void heymac_timeout_work_handler(struct k_work *work);

/* HeyMac context management */

int heymac_init(struct net_if *iface)
{
	struct heymac_context *ctx;

	if (!iface) {
		return -EINVAL;
	}

	/* Get L2 context allocated by Zephyr network stack */
	ctx = net_if_l2_data(iface);
	if (!ctx) {
		return -EINVAL;
	}

	ctx->iface = iface;
	ctx->net_id = CONFIG_HEYMAC_DEFAULT_NET_ID;
	ctx->tx_seq_no = sys_rand32_get() & 0xFF;
	ctx->state = HEYMAC_STATE_INITIAL;
	
	/* Mint the canonical 6-byte node identity ONCE -- the durable address the
	 * whole stack treats as the Æther address. Random at boot, locally-
	 * administered + unicast (so it is never the all-ones broadcast). CGA slot:
	 * load a persisted Cryptographically Generated Address here instead when
	 * one exists. */
	for (int i = 0; i < 6; i++) {
		ctx->identity[i] = sys_rand32_get() & 0xFF;
	}
	ctx->identity[0] = (uint8_t)((ctx->identity[0] | 0x02) & 0xFE); /* local, unicast */

	/* The link address is a *view* of that one identity at the PHY's native
	 * width -- a narrow PHY (DECT NR+ = 16-bit) carries the low 2 bytes; a
	 * full-width PHY (LoRa) carries all 6. Single source of truth, sliced as
	 * the wire needs. */
#if CONFIG_HEYMAC_USE_SHORT_ADDR
	ctx->local_addr.type = 0;  /* Short (2-byte) link address: low bytes of id */
	ctx->local_addr.short_addr[0] = ctx->identity[4];
	ctx->local_addr.short_addr[1] = ctx->identity[5];
#else
	ctx->local_addr.type = 1;  /* Long (6-byte) link address == the identity */
	memcpy(ctx->local_addr.long_addr, ctx->identity, HEYMAC_ADDR_LONG_LEN);
#endif

	/* Initialize Zephyr synchronization primitives */
	k_mutex_init(&ctx->tx_lock);
	k_mutex_init(&ctx->state_mutex);

	/* Initialize work items for HSM events */
	k_work_init_delayable(&ctx->beacon_work, heymac_beacon_work_handler);
	k_work_init_delayable(&ctx->timeout_work, heymac_timeout_work_handler);

	LOG_INF("HeyMac initialized on interface %p, net_id=0x%04x", 
		iface, ctx->net_id);

	/* Start HSM by transitioning to INITIALIZING state */
	heymac_state_machine_run(ctx, HEYMAC_EVENT_INIT);

	return 0;
}

int heymac_set_addr(struct net_if *iface, const struct heymac_addr *addr)
{
	struct heymac_context *ctx;

	if (!iface || !addr) {
		return -EINVAL;
	}

	ctx = net_if_l2_data(iface);
	if (!ctx) {
		return -EINVAL;
	}

	memcpy(&ctx->local_addr, addr, sizeof(*addr));

	LOG_DBG("Address updated");
	return 0;
}

int heymac_get_addr(struct net_if *iface, struct heymac_addr *addr)
{
	struct heymac_context *ctx;

	if (!iface || !addr) {
		return -EINVAL;
	}

	ctx = net_if_l2_data(iface);
	if (!ctx) {
		return -EINVAL;
	}

	memcpy(addr, &ctx->local_addr, sizeof(*addr));
	return 0;
}

int heymac_get_identity(struct net_if *iface, uint8_t out[6])
{
	struct heymac_context *ctx;

	if (!iface || !out) {
		return -EINVAL;
	}

	ctx = net_if_l2_data(iface);
	if (!ctx) {
		return -EINVAL;
	}

	memcpy(out, ctx->identity, 6);
	return 0;
}

int heymac_set_network_id(struct net_if *iface, uint16_t net_id)
{
	struct heymac_context *ctx;
	const struct heymac_radio_api *api;

	if (!iface) {
		return -EINVAL;
	}

	ctx = net_if_l2_data(iface);
	if (!ctx) {
		return -EINVAL;
	}

	ctx->net_id = net_id;

	/* Update radio if it supports network ID setting */
	api = net_if_get_device(iface)->api;
	if (api && api->set_network_id) {
		return api->set_network_id(net_if_get_device(iface), net_id);
	}

	return 0;
}

int heymac_get_network_id(struct net_if *iface)
{
	struct heymac_context *ctx;

	if (!iface) {
		return -EINVAL;
	}

	ctx = net_if_l2_data(iface);
	if (!ctx) {
		return -EINVAL;
	}

	return ctx->net_id;
}

void heymac_recv(struct net_if *iface, struct net_pkt *pkt)
{
	struct heymac_context *ctx;
	struct heymac_hdr hdr;
	int hdr_len;
	uint8_t *buf;
	size_t len;

	if (!iface || !pkt) {
		LOG_ERR("Invalid parameters");
		net_pkt_unref(pkt);
		return;
	}

	ctx = net_if_l2_data(iface);
	if (!ctx) {
		LOG_ERR("No HeyMac context");
		net_pkt_unref(pkt);
		return;
	}

	/* Get packet data */
	len = net_pkt_get_len(pkt);
	if (len < 3) {
		LOG_ERR("Packet too short: %zu bytes", len);
		ctx->rx_errors++;
		net_pkt_unref(pkt);
		return;
	}

	/* Use the first buffer's data directly */
	if (!pkt->buffer) {
		LOG_ERR("No buffer in packet");
		ctx->rx_errors++;
		net_pkt_unref(pkt);
		return;
	}
	buf = pkt->buffer->data;

	/* Parse HeyMac header */
	hdr_len = heymac_parse_header(buf, len, &hdr);
	if (hdr_len < 0) {
		LOG_ERR("Failed to parse HeyMac header: %d", hdr_len);
		ctx->rx_errors++;
		net_pkt_unref(pkt);
		return;
	}

	/* Validate network ID if present */
	if ((hdr.frame_ctrl & HEYMAC_FCTL_N) && hdr.net_id != ctx->net_id) {
		LOG_INF("HeyMac net_id mismatch: got 0x%04x, expected 0x%04x",
			hdr.net_id, ctx->net_id);
		net_pkt_unref(pkt);
		return;
	}

	/* Remove HeyMac header from packet */
	net_pkt_pull(pkt, hdr_len);

	ctx->rx_packets++;

	/* DBG, not INF: per-FRAME (see the note in aether_dect.c's RX log). */
	LOG_DBG("HeyMac RX: type=%d, len=%zu",
		hdr.version_type & 0x0F, len - hdr_len);

	/* Remember the immediate sender (resender) for stateless HONR routing. */
	memcpy(&ctx->last_rx_src, &hdr.src_addr, sizeof(ctx->last_rx_src));

#ifdef CONFIG_AETHER_MESH
	/* Pass to Æther mesh layer if available */
	if ((hdr.version_type & 0x0F) == HEYMAC_FRM_TYPE_DATA) {
		extern void aether_mesh_recv(struct net_if *iface, struct net_pkt *pkt);
		aether_mesh_recv(iface, pkt);
		return;
	}
#endif

	/* Handle different frame types and trigger HSM events */
	switch (hdr.version_type & 0x0F) {
	case HEYMAC_FRM_TYPE_BEACON:
		LOG_DBG("Beacon frame received");
		ctx->rx_beacons++;
		k_mutex_lock(&ctx->state_mutex, K_FOREVER);
		heymac_state_machine_run(ctx, HEYMAC_EVENT_NEIGHBOR_FOUND);
		k_mutex_unlock(&ctx->state_mutex);
		net_pkt_unref(pkt);
		break;
	case HEYMAC_FRM_TYPE_DATA:
		/* For now, just pass up to application layer */
		net_recv_data(iface, pkt);
		break;
	default:
		LOG_DBG("Unknown frame type: %d", hdr.version_type & 0x0F);
		net_pkt_unref(pkt);
		break;
	}
}

/* Work handler implementations for HSM integration */

static void heymac_beacon_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct heymac_context *ctx = CONTAINER_OF(dwork, struct heymac_context, beacon_work);
	
	k_mutex_lock(&ctx->state_mutex, K_FOREVER);
	
	if (ctx->state == HEYMAC_STATE_BEACONING) {
		LOG_DBG("Sending periodic beacon");
		heymac_state_machine_run(ctx, HEYMAC_EVENT_BEACON_TIMEOUT);
		/* Reschedule next beacon */
		k_work_reschedule(&ctx->beacon_work, K_MSEC(CONFIG_HEYMAC_BEACON_INTERVAL_MS));
	}
	
	k_mutex_unlock(&ctx->state_mutex);
}

static void heymac_timeout_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct heymac_context *ctx = CONTAINER_OF(dwork, struct heymac_context, timeout_work);
	
	k_mutex_lock(&ctx->state_mutex, K_FOREVER);
	
	LOG_DBG("Timeout in state %d", ctx->state);
	
	switch (ctx->state) {
	case HEYMAC_STATE_LURKING:
		heymac_state_machine_run(ctx, HEYMAC_EVENT_BEACON_TIMEOUT);
		break;
	case HEYMAC_STATE_LINKING:
		heymac_state_machine_run(ctx, HEYMAC_EVENT_LINK_TIMEOUT);
		break;
	default:
		LOG_WRN("Unexpected timeout in state %d", ctx->state);
		break;
	}
	
	k_mutex_unlock(&ctx->state_mutex);
}

/* Helper functions for HSM state transitions */

int heymac_send_beacon(struct heymac_context *ctx)
{
	struct net_pkt *pkt;
	struct heymac_hdr hdr;
	uint8_t hdr_buf[32];
	int hdr_len;
	
	if (!ctx || !ctx->iface) {
		return -EINVAL;
	}
	
	LOG_DBG("Sending beacon");
	
	/* Build beacon header */
	memset(&hdr, 0, sizeof(hdr));
	hdr.version_type = HEYMAC_FRM_TYPE_BEACON;
	hdr.frame_ctrl = HEYMAC_FCTL_S;  /* Source address present */
	
	/* Include network ID if different from default */
	if (ctx->net_id != CONFIG_HEYMAC_DEFAULT_NET_ID) {
		hdr.frame_ctrl |= HEYMAC_FCTL_N;
		hdr.net_id = ctx->net_id;
	}
	
	/* Set source address */
	memcpy(&hdr.src_addr, &ctx->local_addr, sizeof(hdr.src_addr));
	if (ctx->local_addr.type == 1) {
		hdr.frame_ctrl |= HEYMAC_FCTL_L;  /* Long address */
	}
	
	hdr.seq_no = ctx->tx_seq_no++;
	
	/* Build header */
	hdr_len = heymac_build_header(&hdr, hdr_buf, sizeof(hdr_buf));
	if (hdr_len < 0) {
		LOG_ERR("Failed to build beacon header: %d", hdr_len);
		return hdr_len;
	}
	
	/* Allocate packet */
	pkt = net_pkt_alloc_with_buffer(ctx->iface, hdr_len, AF_UNSPEC, 0, K_MSEC(100));
	if (!pkt) {
		LOG_ERR("Failed to allocate beacon packet");
		return -ENOMEM;
	}
	
	/* Add header to packet */
	if (net_pkt_write(pkt, hdr_buf, hdr_len) < 0) {
		LOG_ERR("Failed to write beacon header");
		net_pkt_unref(pkt);
		return -ENOMEM;
	}
	
	/* Send via network interface */
	int ret = net_send_data(pkt);
	if (ret < 0) {
		LOG_ERR("Failed to send beacon: %d", ret);
		net_pkt_unref(pkt);
		return ret;
	}
	
	ctx->tx_beacons++;
	return 0;
}

void heymac_schedule_timeout(struct heymac_context *ctx, uint32_t timeout_ms)
{
	k_work_reschedule(&ctx->timeout_work, K_MSEC(timeout_ms));
}

void heymac_schedule_beacon(struct heymac_context *ctx, uint32_t interval_ms)
{
	k_work_reschedule(&ctx->beacon_work, K_MSEC(interval_ms));
}

void heymac_cancel_timers(struct heymac_context *ctx)
{
	k_work_cancel_delayable(&ctx->beacon_work);
	k_work_cancel_delayable(&ctx->timeout_work);
}