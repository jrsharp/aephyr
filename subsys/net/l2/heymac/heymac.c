/*
 * Copyright (c) 2025 Æther Authors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(heymac, CONFIG_NET_L2_HEYMAC_LOG_LEVEL);

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net/heymac.h>
#include <zephyr/random/random.h>
#include <string.h>

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

/* HeyMac context management */

int heymac_init(struct net_if *iface)
{
	struct heymac_context *ctx;

	if (!iface) {
		return -EINVAL;
	}

	ctx = net_if_get_device(iface)->data;
	if (!ctx) {
		return -EINVAL;
	}

	ctx->iface = iface;
	ctx->net_id = CONFIG_HEYMAC_DEFAULT_NET_ID;
	ctx->tx_seq_no = sys_rand32_get() & 0xFF;
	
	/* Initialize local address */
#if CONFIG_HEYMAC_USE_SHORT_ADDR
	ctx->local_addr.type = 0;  /* Short address */
	ctx->local_addr.short_addr[0] = sys_rand32_get() & 0xFF;
	ctx->local_addr.short_addr[1] = sys_rand32_get() & 0xFF;
#else
	ctx->local_addr.type = 1;  /* Long address */
	/* Generate random MAC-like address */
	for (int i = 0; i < HEYMAC_ADDR_LONG_LEN; i++) {
		ctx->local_addr.long_addr[i] = sys_rand32_get() & 0xFF;
	}
	/* Set locally administered bit */
	ctx->local_addr.long_addr[0] |= 0x02;
#endif

	k_mutex_init(&ctx->tx_lock);

	LOG_INF("HeyMac initialized on interface %p, net_id=0x%04x", 
		iface, ctx->net_id);

	return 0;
}

int heymac_set_addr(struct net_if *iface, const struct heymac_addr *addr)
{
	struct heymac_context *ctx;

	if (!iface || !addr) {
		return -EINVAL;
	}

	ctx = net_if_get_device(iface)->data;
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

	ctx = net_if_get_device(iface)->data;
	if (!ctx) {
		return -EINVAL;
	}

	memcpy(addr, &ctx->local_addr, sizeof(*addr));
	return 0;
}

int heymac_set_network_id(struct net_if *iface, uint16_t net_id)
{
	struct heymac_context *ctx;
	const struct heymac_radio_api *api;

	if (!iface) {
		return -EINVAL;
	}

	ctx = net_if_get_device(iface)->data;
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

	ctx = net_if_get_device(iface)->data;
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

	ctx = net_if_get_device(iface)->data;
	if (!ctx) {
		LOG_ERR("No HeyMac context");
		net_pkt_unref(pkt);
		return;
	}

	/* Get packet data */
	buf = net_pkt_get_data(pkt);
	len = net_pkt_get_len(pkt);

	if (!buf || len < 3) {
		LOG_ERR("Packet too short: %zu bytes", len);
		ctx->rx_errors++;
		net_pkt_unref(pkt);
		return;
	}

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
		LOG_DBG("Network ID mismatch: got 0x%04x, expected 0x%04x",
			hdr.net_id, ctx->net_id);
		net_pkt_unref(pkt);
		return;
	}

	/* Remove HeyMac header from packet */
	net_pkt_pull(pkt, hdr_len);

	ctx->rx_packets++;

	LOG_DBG("Received HeyMac frame, type=%d, len=%zu",
		hdr.version_type & 0x0F, len - hdr_len);

#ifdef CONFIG_AETHER_MESH
	/* Pass to Æther mesh layer if available */
	if ((hdr.version_type & 0x0F) == HEYMAC_FRM_TYPE_DATA) {
		extern void aether_mesh_recv(struct net_if *iface, struct net_pkt *pkt);
		aether_mesh_recv(iface, pkt);
		return;
	}
#endif

	/* For now, just pass up to application layer */
	net_recv_data(iface, pkt);
}