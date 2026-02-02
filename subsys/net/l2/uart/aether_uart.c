/*
 * Copyright (c) 2025 Æther Authors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Aether UART L2 Transport
 *
 * Simple length-delimited framing for point-to-point UART links.
 * No collision avoidance needed - UART is full-duplex and reliable.
 * Frames Aether mesh packets directly (no HeyMac overhead).
 *
 * Frame format:
 *   [LEN_HI][LEN_LO][AETHER_MESH_PACKET...]
 *
 * Where LEN is the length of the Aether mesh packet (big-endian).
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net/net_l2.h>
#include <zephyr/net/aether_uart.h>

LOG_MODULE_REGISTER(aether_uart, CONFIG_AETHER_UART_LOG_LEVEL);

#define AETHER_UART_MAX_FRAME  256
#define AETHER_UART_RX_TIMEOUT K_MSEC(100)

/* UART L2 context */
struct aether_uart_ctx {
	const struct device *uart_dev;
	struct net_if *iface;

	/* RX state machine */
	enum {
		RX_STATE_LEN_HI,
		RX_STATE_LEN_LO,
		RX_STATE_DATA,
	} rx_state;
	uint16_t rx_len;
	uint16_t rx_pos;
	uint8_t rx_buf[AETHER_UART_MAX_FRAME];

	/* Threading */
	struct k_thread rx_thread;
	K_KERNEL_STACK_MEMBER(rx_stack, CONFIG_AETHER_UART_RX_STACK_SIZE);

	/* TX synchronization */
	struct k_mutex tx_lock;

	/* Peer address (the node on the other end of this UART) */
	uint8_t peer_addr[6];
	bool peer_known;

	/* Statistics */
	uint32_t rx_packets;
	uint32_t tx_packets;
	uint32_t rx_errors;
	uint32_t tx_errors;

	bool running;
};

/* Forward declarations */
static void aether_uart_rx_thread(void *p1, void *p2, void *p3);

/* Process a received frame */
static void aether_uart_process_frame(struct aether_uart_ctx *ctx)
{
	struct net_pkt *pkt;
	int ret;

	if (ctx->rx_len < 16) {  /* Minimum Aether mesh header */
		LOG_WRN("Frame too short: %u bytes", ctx->rx_len);
		ctx->rx_errors++;
		return;
	}

	/* Allocate network packet */
	pkt = net_pkt_rx_alloc_with_buffer(ctx->iface, ctx->rx_len,
					   AF_UNSPEC, 0, K_NO_WAIT);
	if (!pkt) {
		LOG_ERR("Failed to allocate RX packet");
		ctx->rx_errors++;
		return;
	}

	/* Copy frame data */
	ret = net_pkt_write(pkt, ctx->rx_buf, ctx->rx_len);
	if (ret < 0) {
		LOG_ERR("Failed to write packet data");
		ctx->rx_errors++;
		net_pkt_unref(pkt);
		return;
	}

	net_pkt_cursor_init(pkt);
	ctx->rx_packets++;

	LOG_DBG("Received frame: %u bytes", ctx->rx_len);

	/* Pass directly to Aether mesh layer */
#ifdef CONFIG_AETHER_MESH
	extern void aether_mesh_recv(struct net_if *iface, struct net_pkt *pkt);
	aether_mesh_recv(ctx->iface, pkt);
#else
	/* No mesh layer - pass up to network stack */
	net_recv_data(ctx->iface, pkt);
#endif
}

/* RX thread - reads from UART and reassembles frames */
static void aether_uart_rx_thread(void *p1, void *p2, void *p3)
{
	struct aether_uart_ctx *ctx = p1;
	uint8_t byte;
	int ret;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	LOG_DBG("Aether UART RX thread started");

	ctx->rx_state = RX_STATE_LEN_HI;
	ctx->rx_pos = 0;

	while (ctx->running) {
		/* Poll for data (blocking with timeout) */
		ret = uart_poll_in(ctx->uart_dev, &byte);
		if (ret < 0) {
			/* No data available, sleep briefly */
			k_sleep(K_MSEC(1));
			continue;
		}

		switch (ctx->rx_state) {
		case RX_STATE_LEN_HI:
			ctx->rx_len = (uint16_t)byte << 8;
			ctx->rx_state = RX_STATE_LEN_LO;
			break;

		case RX_STATE_LEN_LO:
			ctx->rx_len |= byte;
			if (ctx->rx_len == 0 || ctx->rx_len > AETHER_UART_MAX_FRAME) {
				LOG_WRN("Invalid frame length: %u", ctx->rx_len);
				ctx->rx_errors++;
				ctx->rx_state = RX_STATE_LEN_HI;
			} else {
				ctx->rx_pos = 0;
				ctx->rx_state = RX_STATE_DATA;
			}
			break;

		case RX_STATE_DATA:
			ctx->rx_buf[ctx->rx_pos++] = byte;
			if (ctx->rx_pos >= ctx->rx_len) {
				/* Frame complete */
				aether_uart_process_frame(ctx);
				ctx->rx_state = RX_STATE_LEN_HI;
			}
			break;
		}
	}

	LOG_DBG("Aether UART RX thread stopped");
}

/* L2 send function */
static int aether_uart_send(struct net_if *iface, struct net_pkt *pkt)
{
	const struct device *dev = net_if_get_device(iface);
	struct aether_uart_ctx *ctx = dev->data;
	uint8_t len_buf[2];
	size_t len;
	struct net_buf *buf;
	int ret = 0;

	if (!ctx->running) {
		return -ENETDOWN;
	}

	len = net_pkt_get_len(pkt);
	if (len == 0 || len > AETHER_UART_MAX_FRAME) {
		LOG_ERR("Invalid packet length: %zu", len);
		return -EINVAL;
	}

	k_mutex_lock(&ctx->tx_lock, K_FOREVER);

	/* Send length header (big-endian) */
	len_buf[0] = (len >> 8) & 0xFF;
	len_buf[1] = len & 0xFF;

	for (int i = 0; i < 2; i++) {
		uart_poll_out(ctx->uart_dev, len_buf[i]);
	}

	/* Send packet data */
	for (buf = pkt->buffer; buf; buf = buf->frags) {
		for (int i = 0; i < buf->len; i++) {
			uart_poll_out(ctx->uart_dev, buf->data[i]);
		}
	}

	ctx->tx_packets++;
	LOG_DBG("Transmitted frame: %zu bytes", len);

	k_mutex_unlock(&ctx->tx_lock);

	/* L2 send should not free the packet - caller does */
	return ret;
}

/* L2 receive - called by network stack (we bypass this, going direct to mesh) */
static enum net_verdict aether_uart_recv(struct net_if *iface, struct net_pkt *pkt)
{
	ARG_UNUSED(iface);
	ARG_UNUSED(pkt);
	return NET_CONTINUE;
}

/* L2 enable */
static int aether_uart_enable(struct net_if *iface, bool state)
{
	const struct device *dev = net_if_get_device(iface);
	struct aether_uart_ctx *ctx = dev->data;

	if (state) {
		if (ctx->running) {
			return 0;
		}

		ctx->running = true;

		/* Start RX thread */
		k_thread_create(&ctx->rx_thread, ctx->rx_stack,
				K_KERNEL_STACK_SIZEOF(ctx->rx_stack),
				aether_uart_rx_thread, ctx, NULL, NULL,
				CONFIG_AETHER_UART_RX_THREAD_PRIO, 0, K_NO_WAIT);
		k_thread_name_set(&ctx->rx_thread, "aether_uart_rx");

		LOG_INF("Aether UART L2 enabled");
	} else {
		if (!ctx->running) {
			return 0;
		}

		ctx->running = false;
		k_thread_join(&ctx->rx_thread, K_SECONDS(1));

		LOG_INF("Aether UART L2 disabled");
	}

	return 0;
}

/* L2 flags */
static enum net_l2_flags aether_uart_flags(struct net_if *iface)
{
	ARG_UNUSED(iface);
	return NET_L2_POINT_TO_POINT;
}

/* L2 registration */
NET_L2_INIT(AETHER_UART, aether_uart_recv, aether_uart_send,
	    aether_uart_enable, aether_uart_flags);

/* Network interface initialization */
static void aether_uart_iface_init(struct net_if *iface)
{
	const struct device *dev = net_if_get_device(iface);
	struct aether_uart_ctx *ctx = dev->data;

	ctx->iface = iface;
	net_if_set_mtu(iface, AETHER_UART_MAX_FRAME);

	LOG_DBG("Aether UART interface initialized");
}

/* Device initialization */
static int aether_uart_init(const struct device *dev)
{
	struct aether_uart_ctx *ctx = dev->data;
	const struct device *uart_dev;

	LOG_INF("Initializing Aether UART L2...");

	/* Get UART device from devicetree */
	uart_dev = DEVICE_DT_GET(DT_CHOSEN(aether_uart));
	if (!device_is_ready(uart_dev)) {
		LOG_ERR("UART device not ready");
		return -ENODEV;
	}

	ctx->uart_dev = uart_dev;
	ctx->running = false;
	ctx->peer_known = false;

	k_mutex_init(&ctx->tx_lock);

	LOG_INF("Aether UART L2 initialized on %s", uart_dev->name);
	return 0;
}

/* Device and interface API */
static const struct net_if_api aether_uart_iface_api = {
	.init = aether_uart_iface_init,
};

/* Device instance */
static struct aether_uart_ctx aether_uart_ctx_0;

/* Network device - uses uart1 for mesh communication */
NET_DEVICE_INIT(aether_uart_net, "aether_uart",
		aether_uart_init, NULL,
		&aether_uart_ctx_0, NULL,
		CONFIG_KERNEL_INIT_PRIORITY_DEVICE,
		&aether_uart_iface_api,
		AETHER_UART, AETHER_UART_CTX_TYPE, AETHER_UART_MAX_FRAME);
