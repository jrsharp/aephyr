/*
 * Copyright (c) 2026 Æther Authors
 *
 * SPDX-License-Identifier: MIT
 *
 * Æther net-device driver over the DECT NR+ PHY. Mirrors the structure of the LoRa
 * integration driver (aether_lora.c): a HeyMac L2 network device with RX and
 * radio threads feeding the same net_pkt/HeyMac plumbing, but backed by the
 * DECT NR+ PHY (dect_phy.c) instead of an SX126x over SPI.
 *
 * Differences from the LoRa driver, by design:
 *  - Carrier-sense is atomic: listen-before-talk is performed inside the DECT
 *    transmit operation, so there is no separate "stop RX, CAD, send" dance.
 *  - There is no SPI bus, DMA-source staging, or DIO1 settling, so the long
 *    LoRa turnaround delays are gone.
 *  - The radio is half-duplex and operated as windowed receive: the radio
 *    thread transmits queued frames, then opens a bounded RX window. TX latency
 *    is therefore bounded by CONFIG_AETHER_DECT_RX_WINDOW_MS.
 *    TODO: move to interrupt-driven continuous RX with cancel-on-TX for lower
 *    latency once the protocol is validated.
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(aether_dect, CONFIG_AETHER_DECT_LOG_LEVEL);

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net/heymac.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/random/random.h>
#include <string.h>

#include "dect_phy.h"

#if defined(CONFIG_AETHER_ROUTING_HONR)
#include <zephyr/net/aether_mesh.h>
extern struct aether_mesh_ctx *g_mesh_ctx;
#endif

/* RX queue element: the received packet plus the PHY link quality for that
 * frame, so the real RSSI/SNR travels with the packet to the (single-threaded)
 * RX thread, which stashes it on the heymac context just before recv. */
struct aether_dect_rx_item {
	struct net_pkt *pkt;
	int8_t rssi;
	int8_t snr;
};

/* Æther DECT driver context. */
struct aether_dect_ctx {
	struct net_if *iface;

	/* Threading. */
	struct k_thread rx_thread;
	struct k_thread radio_thread;
	K_KERNEL_STACK_MEMBER(rx_stack, CONFIG_AETHER_DECT_RX_STACK_SIZE);
	K_KERNEL_STACK_MEMBER(radio_stack, CONFIG_AETHER_DECT_TX_STACK_SIZE);

	/* Message queues. RX carries rx_item (pkt + link quality); TX is net_pkt ptrs. */
	struct k_msgq rx_msgq;
	struct k_msgq tx_msgq;
	char rx_msgq_buffer[CONFIG_AETHER_DECT_RX_QUEUE_SIZE * sizeof(struct aether_dect_rx_item)];
	char tx_msgq_buffer[CONFIG_AETHER_DECT_TX_QUEUE_SIZE * sizeof(struct net_pkt *)];

	/* Serializes access to the single half-duplex radio between the radio
	 * thread (RX windows + queued TX) and direct synchronous sends.
	 */
	struct k_mutex radio_lock;

	bool running;
};

/* Forward declarations. */
static void aether_dect_rx_thread(void *p1, void *p2, void *p3);
static void aether_dect_radio_thread(void *p1, void *p2, void *p3);
static void aether_dect_rx_callback(const uint8_t *data, size_t len, int16_t rssi_dbm,
				    int8_t snr_db, uint16_t tx_id);

/* Static context instance (referenced from the modem-context RX callback). */
static struct aether_dect_ctx aether_dect_ctx_0;

/* TX pause flag: when set, the radio thread defers queued sends to keep the
 * radio in RX (used by the 9P transport during response waits).
 */
static atomic_t tx_paused;

/* Linearization buffer for outbound frames. The radio thread and the
 * synchronous send path are mutually exclusive under radio_lock, so one
 * buffer is safe.
 */
/* 512 holds the largest DECT NR+ transport block we transmit at MCS-1
 * (16-subslot block = 495 bytes); NOT the LoRa SX1262 256-byte hard limit. */
static uint8_t tx_frame_buf[512];

/* Guard against silently truncating an outbound frame. */
BUILD_ASSERT(sizeof(tx_frame_buf) >= HEYMAC_MAX_FRAME_SIZE,
	     "tx_frame_buf too small for a maximum HeyMac frame");

/* Interface link-layer address (net_if stores the pointer, so it must persist). */
static uint8_t iface_ll_addr[6];

/* Transmit one already-linearized frame with LBT and bounded busy-retry.
 * Returns 0 on success, negative errno otherwise. Takes radio_lock. */
static int aether_dect_radio_tx(struct aether_dect_ctx *ctx, const uint8_t *data, size_t len,
				uint16_t receiver_id, int max_retries)
{
	int ret = -EBUSY;

	for (int attempt = 0; attempt < max_retries; attempt++) {
		if (attempt > 0) {
			/* CSMA backoff before retrying a busy channel. */
			k_msleep(sys_rand32_get() % CONFIG_AETHER_DECT_CAD_BACKOFF_MAX_MS);
		}

		k_mutex_lock(&ctx->radio_lock, K_FOREVER);
		ret = dect_phy_tx(receiver_id, data, len);
		k_mutex_unlock(&ctx->radio_lock);

		if (ret == 0) {
			return 0;
		}
		if (ret != -EBUSY) {
			/* Hard error: do not spin. */
			break;
		}
		LOG_DBG("LBT busy, backoff %d/%d", attempt + 1, max_retries);
	}

	return ret;
}

/* RX thread: hand received packets up to HeyMac. */
static void aether_dect_rx_thread(void *p1, void *p2, void *p3)
{
	struct aether_dect_ctx *ctx = (struct aether_dect_ctx *)p1;
	struct aether_dect_rx_item item;
	int ret;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	LOG_DBG("Æther DECT RX thread started");

	while (ctx->running) {
		ret = k_msgq_get(&ctx->rx_msgq, &item, K_FOREVER);
		if (ret < 0 || !item.pkt) {
			continue;
		}

		/* Stash this frame's real link quality on the heymac context so the
		 * mesh recv (called synchronously below) can attribute it to the
		 * sender's neighbor entry. Single-threaded here -> no race. */
		struct heymac_context *hc = net_if_l2_data(ctx->iface);

		if (hc) {
			hc->last_rx_rssi = item.rssi;
			hc->last_rx_snr = item.snr;
		}

		heymac_recv(ctx->iface, item.pkt);
	}

	LOG_DBG("Æther DECT RX thread stopped");
}

/* Radio thread: keep the radio listening continuously and transmit queued
 * frames as they arrive, briefly interrupting receive for each. The receive
 * operation is asynchronous (driven in the modem); the transmit path stops and
 * resumes it, so the radio listens at all times except during an actual key-up.
 */
static void aether_dect_radio_thread(void *p1, void *p2, void *p3)
{
	struct aether_dect_ctx *ctx = (struct aether_dect_ctx *)p1;
	struct net_pkt *pkt;
	size_t len;
	int ret;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	LOG_DBG("Æther DECT radio thread started");

	while (ctx->running) {
#if defined(CONFIG_AETHER_ROUTING_HONR)
		/* Track the HONR identity so receive hardware-filters to it once
		 * joined (0 while unjoined keeps RX promiscuous for the tree join).
		 * Takes effect on the next receive re-arm.
		 */
		if (g_mesh_ctx) {
			dect_phy_set_node_id(g_mesh_ctx->honr_joined ?
					     g_mesh_ctx->honr_addr : 0);
		}
#endif

		/* Keep the radio listening. A no-op while a receive is already
		 * live; re-arms once the previous operation's duration elapsed.
		 */
		k_mutex_lock(&ctx->radio_lock, K_FOREVER);
		dect_phy_rx_ensure();
		k_mutex_unlock(&ctx->radio_lock);

		/* While TX is paused (e.g. a 9P response wait), stay in receive and
		 * don't dequeue. Sleep so the receive operation keeps running.
		 */
		if (atomic_get(&tx_paused)) {
			k_msleep(CONFIG_AETHER_DECT_RX_WINDOW_MS);
			continue;
		}

		/* Wait for a frame to send. A queued frame wakes us immediately; on
		 * timeout we loop to re-arm receive after its duration elapses. The
		 * radio is receiving the entire time we block here.
		 */
		if (k_msgq_get(&ctx->tx_msgq, &pkt, K_MSEC(CONFIG_AETHER_DECT_RX_WINDOW_MS)) != 0) {
			continue;
		}
		if (!pkt) {
			continue;
		}

		len = net_pkt_get_len(pkt);
		if (len == 0 || len > sizeof(tx_frame_buf)) {
			LOG_ERR("Invalid TX length: %zu", len);
			net_pkt_unref(pkt);
			continue;
		}

		net_pkt_cursor_init(pkt);
		if (net_pkt_read(pkt, tx_frame_buf, len) < 0) {
			LOG_ERR("Failed to read TX packet");
			net_pkt_unref(pkt);
			continue;
		}

		/* A 2-byte link-layer destination (set by the mesh layer for HONR
		 * unicast hops) becomes the PHY receiver id; else the frame is
		 * broadcast. The transmit stops and resumes receive internally.
		 */
		uint16_t rx_id = DECT_PHY_BROADCAST_ID;
		struct net_linkaddr *ll = net_pkt_lladdr_dst(pkt);

		if (ll->len >= 2) {
			rx_id = (uint16_t)((ll->addr[0] << 8) | ll->addr[1]);
		}

		ret = aether_dect_radio_tx(ctx, tx_frame_buf, len, rx_id,
					   CONFIG_AETHER_DECT_CAD_MAX_RETRIES);
		if (ret == 0) {
			LOG_DBG("DECT TX ok, len=%zu", len);
		} else {
			LOG_WRN("DECT TX failed: %d", ret);
		}

		net_pkt_unref(pkt);
	}

	LOG_DBG("Æther DECT radio thread stopped");
}

/* Synchronous send with retries, for reliable messaging at the mesh layer.
 * Bypasses the TX queue and transmits directly. Blocks until complete.
 */
int aether_lora_send_sync(const uint8_t *data, size_t len, int max_retries)
{
	struct aether_dect_ctx *ctx = &aether_dect_ctx_0;

	if (!ctx->running || !data || len == 0 || len > sizeof(tx_frame_buf)) {
		return -EINVAL;
	}

	return aether_dect_radio_tx(ctx, data, len, DECT_PHY_BROADCAST_ID, max_retries);
}

int aether_lora_cad_test(int count, int *out_clear, int *out_busy, int *out_err)
{
	struct aether_dect_ctx *ctx = &aether_dect_ctx_0;
	int clear = 0, busy = 0, errors = 0;

	if (!ctx->running) {
		return -ENETDOWN;
	}

	/* Pause queued TX and take the radio so measurements are uncontended. */
	atomic_set(&tx_paused, 1);
	k_mutex_lock(&ctx->radio_lock, K_FOREVER);

	for (int i = 0; i < count; i++) {
		int8_t rssi;
		int ret = dect_phy_channel_rssi(&rssi);

		if (ret < 0) {
			errors++;
		} else if (rssi > CONFIG_AETHER_DECT_LBT_THRESHOLD_DBM) {
			busy++;
		} else {
			clear++;
		}
	}

	k_mutex_unlock(&ctx->radio_lock);
	atomic_clear(&tx_paused);

	if (out_clear) {
		*out_clear = clear;
	}
	if (out_busy) {
		*out_busy = busy;
	}
	if (out_err) {
		*out_err = errors;
	}

	return 0;
}

void aether_lora_tx_pause(void)
{
	atomic_set(&tx_paused, 1);
	LOG_DBG("TX paused");
}

void aether_lora_tx_resume(void)
{
	atomic_clear(&tx_paused);
	LOG_DBG("TX resumed");
}

/* DECT RX callback - runs in modem event context. Queue frame for RX thread. */
static void aether_dect_rx_callback(const uint8_t *data, size_t len, int16_t rssi_dbm,
				    int8_t snr_db, uint16_t tx_id)
{
	struct aether_dect_ctx *ctx = &aether_dect_ctx_0;
	struct heymac_context *heymac_ctx;
	struct net_pkt *pkt;
	int ret;

	ARG_UNUSED(tx_id);

	if (!ctx->iface || !data || len == 0) {
		return;
	}

	heymac_ctx = net_if_l2_data(ctx->iface);
	if (!heymac_ctx) {
		LOG_ERR("No HeyMac context");
		return;
	}

	pkt = net_pkt_rx_alloc_with_buffer(ctx->iface, len, AF_UNSPEC, 0, K_NO_WAIT);
	if (!pkt) {
		LOG_ERR("Failed to allocate RX packet");
		heymac_ctx->rx_errors++;
		return;
	}

	ret = net_pkt_write(pkt, data, len);
	if (ret < 0) {
		LOG_ERR("Failed to write RX data: %d", ret);
		heymac_ctx->rx_errors++;
		net_pkt_unref(pkt);
		return;
	}

	net_pkt_cursor_init(pkt);

	/* Carry the PHY link quality with the packet. rssi_dbm is int16 from the
	 * modem; real values (~-30..-110 dBm) fit int8 -- clamp defensively. */
	int16_t r = rssi_dbm < -128 ? -128 : (rssi_dbm > 127 ? 127 : rssi_dbm);
	struct aether_dect_rx_item item = { .pkt = pkt, .rssi = (int8_t)r, .snr = snr_db };

	ret = k_msgq_put(&ctx->rx_msgq, &item, K_NO_WAIT);
	if (ret < 0) {
		LOG_ERR("RX queue full, dropping packet");
		heymac_ctx->rx_errors++;
		net_pkt_unref(pkt);
		return;
	}

	heymac_ctx->rx_packets++;
	LOG_INF("DECT RX: len=%zu, rssi=%d, snr=%d", len, rssi_dbm, snr_db);
}

/* HeyMac radio API implementation. */

static int aether_dect_set_channel(const struct device *dev, uint16_t channel)
{
	ARG_UNUSED(dev);

	/* Channel 0 keeps the configured default carrier; otherwise the channel
	 * value is used directly as the DECT absolute carrier number.
	 */
	if (channel != 0) {
		dect_phy_set_carrier(channel);
	}
	return 0;
}

static int aether_dect_set_power(const struct device *dev, int8_t power)
{
	ARG_UNUSED(dev);

	dect_phy_set_tx_power(power);
	return 0;
}

static int aether_dect_set_network_id(const struct device *dev, uint16_t net_id)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(net_id);

	/* Network ID is handled at the HeyMac layer. */
	return 0;
}

static int aether_dect_start(const struct device *dev)
{
	struct aether_dect_ctx *ctx = dev->data;
	uint16_t device_id;
	int ret;

	if (ctx->running) {
		return 0;
	}

	ret = dect_phy_init(&device_id);
	if (ret < 0) {
		LOG_ERR("Failed to bring up DECT NR+ PHY: %d", ret);
		return ret;
	}

	dect_phy_set_rx_cb(aether_dect_rx_callback);
	dect_phy_set_carrier(CONFIG_AETHER_DECT_CARRIER);
	dect_phy_set_tx_power(CONFIG_AETHER_DECT_TX_POWER);

	ctx->running = true;

	k_thread_create(&ctx->rx_thread, ctx->rx_stack,
			K_KERNEL_STACK_SIZEOF(ctx->rx_stack),
			aether_dect_rx_thread, ctx, NULL, NULL,
			CONFIG_AETHER_DECT_RX_THREAD_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&ctx->rx_thread, "aether_dect_rx");

	k_thread_create(&ctx->radio_thread, ctx->radio_stack,
			K_KERNEL_STACK_SIZEOF(ctx->radio_stack),
			aether_dect_radio_thread, ctx, NULL, NULL,
			CONFIG_AETHER_DECT_TX_THREAD_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&ctx->radio_thread, "aether_dect_radio");

	LOG_INF("Æther DECT driver started (device ID %u)", device_id);
	return 0;
}

static int aether_dect_stop(const struct device *dev)
{
	struct aether_dect_ctx *ctx = dev->data;

	if (!ctx->running) {
		return 0;
	}

	ctx->running = false;

	k_thread_join(&ctx->rx_thread, K_SECONDS(2));
	k_thread_join(&ctx->radio_thread, K_SECONDS(2));

	LOG_INF("Æther DECT driver stopped");
	return 0;
}

static int aether_dect_tx(const struct device *dev, struct net_pkt *pkt)
{
	struct aether_dect_ctx *ctx = dev->data;

	if (!ctx->running) {
		return -ENETDOWN;
	}

	if (k_msgq_put(&ctx->tx_msgq, &pkt, K_NO_WAIT) < 0) {
		LOG_ERR("TX queue full");
		return -ENOBUFS;
	}

	return 0;
}

/* Network interface API. */
static void aether_dect_iface_init(struct net_if *iface)
{
	const struct device *dev = net_if_get_device(iface);
	struct aether_dect_ctx *ctx = dev->data;

	ctx->iface = iface;

	/* Bringing up the modem (dect_phy_init) can block; do not auto-start at
	 * boot. The application brings the interface up explicitly.
	 */
	net_if_flag_set(iface, NET_IF_NO_AUTO_START);

	heymac_iface_init(iface);

	/* net_if requires a non-zero-length link-layer address before the
	 * interface may go up (asserts otherwise). Derive a stable one from the
	 * hardware ID; the mesh uses its own addressing, so this is only for the
	 * net stack's bookkeeping.
	 */
	if (hwinfo_get_device_id(iface_ll_addr, sizeof(iface_ll_addr)) <= 0) {
		memset(iface_ll_addr, 0xA5, sizeof(iface_ll_addr));
	}
	net_if_set_link_addr(iface, iface_ll_addr, sizeof(iface_ll_addr), NET_LINK_DUMMY);

	LOG_DBG("Æther DECT interface initialized");
}

static const struct net_if_api aether_dect_iface_api = {
	.init = aether_dect_iface_init,
};

static const struct heymac_radio_api aether_dect_radio_api = {
	.iface_api = aether_dect_iface_api,
	.set_channel = aether_dect_set_channel,
	.set_power = aether_dect_set_power,
	.set_network_id = aether_dect_set_network_id,
	.start = aether_dect_start,
	.stop = aether_dect_stop,
	.tx = aether_dect_tx,
};

static int aether_dect_init(const struct device *dev)
{
	struct aether_dect_ctx *ctx = dev->data;

	ctx->running = false;
	k_mutex_init(&ctx->radio_lock);

	k_msgq_init(&ctx->rx_msgq, ctx->rx_msgq_buffer, sizeof(struct aether_dect_rx_item),
		    CONFIG_AETHER_DECT_RX_QUEUE_SIZE);
	k_msgq_init(&ctx->tx_msgq, ctx->tx_msgq_buffer, sizeof(struct net_pkt *),
		    CONFIG_AETHER_DECT_TX_QUEUE_SIZE);

	LOG_INF("Æther DECT driver initialized");
	return 0;
}

/* Network interface registration. */
NET_DEVICE_INIT(aether_dect_net, "aether_dect_net", aether_dect_init, NULL,
		&aether_dect_ctx_0, NULL, 90,
		&aether_dect_radio_api, HEYMAC_L2, HEYMAC_L2_CTX_TYPE,
		HEYMAC_MAX_PAYLOAD);
