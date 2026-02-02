/*
 * Copyright (c) 2025 Æther Authors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(aether_lora, CONFIG_AETHER_LORA_LOG_LEVEL);

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net/heymac.h>
#include <zephyr/sys/ring_buffer.h>
#include <string.h>

/* Æther LoRa driver context */
struct aether_lora_ctx {
	const struct device *lora_dev;
	struct net_if *iface;

	/* Threading */
	struct k_thread rx_thread;
	struct k_thread tx_thread;
	K_KERNEL_STACK_MEMBER(rx_stack, CONFIG_AETHER_RX_STACK_SIZE);
	K_KERNEL_STACK_MEMBER(tx_stack, CONFIG_AETHER_TX_STACK_SIZE);

	/* Message queues */
	struct k_msgq rx_msgq;
	struct k_msgq tx_msgq;
	char rx_msgq_buffer[CONFIG_AETHER_RX_QUEUE_SIZE * sizeof(struct net_pkt *)];
	char tx_msgq_buffer[CONFIG_AETHER_TX_QUEUE_SIZE * sizeof(struct net_pkt *)];

	/* TX completion signaling for synchronous sends */
	struct k_sem tx_done;
	int tx_result;

	/* LoRa configuration */
	struct lora_modem_config modem_config;

	bool running;
};

/* Forward declarations */
static void aether_lora_rx_thread(void *p1, void *p2, void *p3);
static void aether_lora_tx_thread(void *p1, void *p2, void *p3);
static void aether_lora_rx_callback(const struct device *dev, uint8_t *data,
				    uint16_t size, int16_t rssi, int8_t snr);

/* Static context instance - forward declared for RX callback */
static struct aether_lora_ctx aether_lora_ctx_0;

/* LoRa RX thread */
static void aether_lora_rx_thread(void *p1, void *p2, void *p3)
{
	struct aether_lora_ctx *ctx = (struct aether_lora_ctx *)p1;
	struct net_pkt *pkt;
	int ret;

	LOG_DBG("Æther LoRa RX thread started");

	while (ctx->running) {
		/* Wait for received packet */
		ret = k_msgq_get(&ctx->rx_msgq, &pkt, K_FOREVER);
		if (ret < 0) {
			LOG_ERR("Failed to get RX packet: %d", ret);
			continue;
		}

		if (!pkt) {
			continue;
		}

		/* Pass to HeyMac layer */
		heymac_recv(ctx->iface, pkt);
	}

	LOG_DBG("Æther LoRa RX thread stopped");
}

/* LoRa TX thread */
static void aether_lora_tx_thread(void *p1, void *p2, void *p3)
{
	struct aether_lora_ctx *ctx = (struct aether_lora_ctx *)p1;
	struct net_pkt *pkt;
	uint8_t *data;
	size_t len;
	int ret;

	LOG_DBG("Æther LoRa TX thread started");

	while (ctx->running) {
		/* Wait for packet to transmit */
		ret = k_msgq_get(&ctx->tx_msgq, &pkt, K_FOREVER);
		if (ret < 0) {
			LOG_ERR("Failed to get TX packet: %d", ret);
			continue;
		}

		if (!pkt) {
			continue;
		}

		/* Get total packet length */
		len = net_pkt_get_len(pkt);

		if (len == 0 || len > 255) {
			LOG_ERR("Invalid packet length: %zu", len);
			net_pkt_unref(pkt);
			continue;
		}

		/* Linearize packet data - may span multiple fragments.
		 * Static buffer to reduce stack pressure (thread is single-use). */
		static uint8_t tx_buf[256];
		net_pkt_cursor_init(pkt);  /* Reset cursor to start */
		if (net_pkt_read(pkt, tx_buf, len) < 0) {
			LOG_ERR("Failed to read packet data");
			net_pkt_unref(pkt);
			continue;
		}
		data = tx_buf;

		/* Transmit via LoRa.
		 * Stop async RX, wait for radio to be ready, send, then restart RX.
		 * Need substantial delay - radio may be mid-packet when we stop RX. */
		lora_recv_async(ctx->lora_dev, NULL);
		k_msleep(200);  /* Allow radio to fully exit RX mode */

		/* Retry logic:
		 * - EBUSY: modem busy, TX never started - retry immediately
		 * - EAGAIN: timeout, TX state unknown - wait then retry once
		 * RX-side dedup handles any duplicate transmissions. */
		ret = lora_send(ctx->lora_dev, data, len);

		if (ret == 0) {
			LOG_DBG("LoRa packet transmitted, len=%zu", len);
		} else if (ret == -EBUSY) {
			/* Modem busy - wait and retry */
			LOG_WRN("LoRa modem busy, retrying...");
			k_msleep(100);
			ret = lora_send(ctx->lora_dev, data, len);
			if (ret == 0) {
				LOG_DBG("LoRa packet transmitted (retry), len=%zu", len);
			} else {
				LOG_ERR("LoRa TX failed after retry: %d", ret);
			}
		} else {
			/* EAGAIN timeout - TX state unknown.
			 * Wait for any in-flight TX to complete, then retry once.
			 * SF10 packets take ~400-500ms, so wait 1s to be safe.
			 * RX-side dedup filters duplicates if first TX succeeded. */
			LOG_WRN("LoRa TX timeout (%d), wait+retry", ret);
			k_msleep(1000);  /* Wait for potential in-flight TX */
			ret = lora_send(ctx->lora_dev, data, len);
			if (ret == 0) {
				LOG_DBG("LoRa packet transmitted (after timeout), len=%zu", len);
			} else {
				LOG_WRN("LoRa retry also failed: %d (first TX may have succeeded)", ret);
			}
		}

		/* Delay before restarting RX - ensure radio is fully idle */
		k_msleep(50);
		lora_recv_async(ctx->lora_dev, aether_lora_rx_callback);

		net_pkt_unref(pkt);
	}

	LOG_DBG("Æther LoRa TX thread stopped");
}

/* Synchronous send with retries - for reliable messaging at mesh layer.
 * Bypasses the TX queue and sends directly. Blocks until complete. */
int aether_lora_send_sync(const uint8_t *data, size_t len, int max_retries)
{
	struct aether_lora_ctx *ctx = &aether_lora_ctx_0;
	int ret;
	int attempt;

	if (!ctx->running || !data || len == 0 || len > 255) {
		return -EINVAL;
	}

	for (attempt = 0; attempt < max_retries; attempt++) {
		if (attempt > 0) {
			LOG_DBG("Sync TX retry %d/%d", attempt, max_retries - 1);
			k_msleep(200 + (attempt * 100));  /* Backoff */
		}

		/* Stop RX, wait for radio to settle */
		lora_recv_async(ctx->lora_dev, NULL);
		k_msleep(200);

		/* Attempt transmission */
		ret = lora_send(ctx->lora_dev, (uint8_t *)data, len);

		/* Restart RX */
		k_msleep(50);
		lora_recv_async(ctx->lora_dev, aether_lora_rx_callback);

		if (ret == 0) {
			LOG_DBG("Sync TX success, len=%zu, attempts=%d", len, attempt + 1);
			return 0;
		}

		/* On EAGAIN (timeout), packet may have been sent - wait before retry */
		if (ret == -EAGAIN) {
			LOG_WRN("Sync TX timeout, waiting before retry");
			k_msleep(800);  /* Wait for potential in-flight TX */
		}
	}

	LOG_ERR("Sync TX failed after %d attempts: %d", max_retries, ret);
	return ret;
}

/* LoRa RX callback - note: dev is the LoRa radio, not our net device */
static void aether_lora_rx_callback(const struct device *dev, uint8_t *data,
				    uint16_t size, int16_t rssi, int8_t snr)
{
	/* Use static context - dev->data is the LoRa driver's context, not ours */
	struct aether_lora_ctx *ctx = &aether_lora_ctx_0;
	struct heymac_context *heymac_ctx;
	struct net_pkt *pkt;
	int ret;

	ARG_UNUSED(dev);
	ARG_UNUSED(rssi);
	ARG_UNUSED(snr);

	if (!ctx->iface || !data || size == 0) {
		LOG_ERR("Invalid RX callback parameters");
		return;
	}

	/* Get HeyMac context from network interface */
	heymac_ctx = net_if_l2_data(ctx->iface);
	if (!heymac_ctx) {
		LOG_ERR("No HeyMac context");
		return;
	}

	/* Allocate network packet */
	pkt = net_pkt_rx_alloc_with_buffer(ctx->iface, size, AF_UNSPEC, 0, K_NO_WAIT);
	if (!pkt) {
		LOG_ERR("Failed to allocate RX packet");
		heymac_ctx->rx_errors++;
		return;
	}

	/* Copy data to packet */
	ret = net_pkt_write(pkt, data, size);
	if (ret < 0) {
		LOG_ERR("Failed to write RX data: %d", ret);
		heymac_ctx->rx_errors++;
		net_pkt_unref(pkt);
		return;
	}

	/* Reset packet cursor */
	net_pkt_cursor_init(pkt);

	/* Queue for RX thread processing */
	ret = k_msgq_put(&ctx->rx_msgq, &pkt, K_NO_WAIT);
	if (ret < 0) {
		LOG_ERR("RX queue full, dropping packet");
		heymac_ctx->rx_errors++;
		net_pkt_unref(pkt);
		return;
	}

	heymac_ctx->rx_packets++;
	LOG_DBG("LoRa packet received, len=%d, rssi=%d, snr=%d", size, rssi, snr);
}

/* HeyMac radio API implementation */

static int aether_lora_set_channel(const struct device *dev, uint16_t channel)
{
	struct aether_lora_ctx *ctx = dev->data;
	
	/* Convert channel to frequency (simplified) */
	uint32_t frequency = 915000000 + (channel * 200000);  /* 200kHz spacing */
	
	return lora_config(ctx->lora_dev, &ctx->modem_config);
}

static int aether_lora_set_power(const struct device *dev, int8_t power)
{
	struct aether_lora_ctx *ctx = dev->data;
	
	ctx->modem_config.tx_power = power;
	return lora_config(ctx->lora_dev, &ctx->modem_config);
}

static int aether_lora_set_network_id(const struct device *dev, uint16_t net_id)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(net_id);
	
	/* Network ID is handled at HeyMac layer */
	return 0;
}

static int aether_lora_start(const struct device *dev)
{
	struct aether_lora_ctx *ctx = dev->data;
	int ret;

	if (ctx->running) {
		return 0;
	}

	/* Configure LoRa modem */
	ret = lora_config(ctx->lora_dev, &ctx->modem_config);
	if (ret < 0) {
		LOG_ERR("Failed to configure LoRa modem: %d", ret);
		return ret;
	}

	/* Start RX mode */
	ret = lora_recv_async(ctx->lora_dev, aether_lora_rx_callback);
	if (ret < 0) {
		LOG_ERR("Failed to start LoRa RX: %d", ret);
		return ret;
	}

	/* Start threads */
	ctx->running = true;
	
	k_thread_create(&ctx->rx_thread, ctx->rx_stack,
			K_KERNEL_STACK_SIZEOF(ctx->rx_stack),
			aether_lora_rx_thread, ctx, NULL, NULL,
			CONFIG_AETHER_RX_THREAD_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&ctx->rx_thread, "aether_rx");

	k_thread_create(&ctx->tx_thread, ctx->tx_stack,
			K_KERNEL_STACK_SIZEOF(ctx->tx_stack),
			aether_lora_tx_thread, ctx, NULL, NULL,
			CONFIG_AETHER_TX_THREAD_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&ctx->tx_thread, "aether_tx");

	LOG_INF("Æther LoRa driver started");
	return 0;
}

static int aether_lora_stop(const struct device *dev)
{
	struct aether_lora_ctx *ctx = dev->data;

	if (!ctx->running) {
		return 0;
	}

	ctx->running = false;

	/* Stop LoRa */
	lora_recv_async(ctx->lora_dev, NULL);  /* Disable RX callback */

	/* Wait for threads to stop */
	k_thread_join(&ctx->rx_thread, K_SECONDS(1));
	k_thread_join(&ctx->tx_thread, K_SECONDS(1));

	LOG_INF("Æther LoRa driver stopped");
	return 0;
}

static int aether_lora_tx(const struct device *dev, struct net_pkt *pkt)
{
	struct aether_lora_ctx *ctx = dev->data;
	int ret;

	if (!ctx->running) {
		return -ENETDOWN;
	}

	/* Queue packet for transmission */
	ret = k_msgq_put(&ctx->tx_msgq, &pkt, K_NO_WAIT);
	if (ret < 0) {
		LOG_ERR("TX queue full");
		return -ENOBUFS;
	}

	return 0;
}

/* Network interface API */
static void aether_lora_iface_init(struct net_if *iface)
{
	const struct device *dev = net_if_get_device(iface);
	struct aether_lora_ctx *ctx = dev->data;

	ctx->iface = iface;
	
	/* Initialize HeyMac interface */
	heymac_iface_init(iface);
	
	LOG_DBG("Æther LoRa interface initialized");
}

static int aether_lora_iface_send(struct net_if *iface, struct net_pkt *pkt)
{
	const struct device *dev = net_if_get_device(iface);
	
	/* This will be called by the HeyMac L2 layer */
	return aether_lora_tx(dev, pkt);
}

static int aether_lora_iface_enable(struct net_if *iface, bool state)
{
	const struct device *dev = net_if_get_device(iface);
	
	if (state) {
		return aether_lora_start(dev);
	} else {
		return aether_lora_stop(dev);
	}
}

/* Network interface API structure */
static const struct net_if_api aether_lora_iface_api = {
	.init = aether_lora_iface_init,
};

/* HeyMac radio API structure */
static const struct heymac_radio_api aether_lora_radio_api = {
	.iface_api = aether_lora_iface_api,
	.set_channel = aether_lora_set_channel,
	.set_power = aether_lora_set_power,
	.set_network_id = aether_lora_set_network_id,
	.start = aether_lora_start,
	.stop = aether_lora_stop,
	.tx = aether_lora_tx,
};

/* Device initialization */
static int aether_lora_init(const struct device *dev)
{
	struct aether_lora_ctx *ctx = dev->data;
	const struct device *lora_dev;

	/* Get LoRa device */
	LOG_INF("Initializing Æther LoRa driver...");
	lora_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_lora));
	if (!lora_dev) {
		LOG_ERR("LoRa device not found in device tree");
		return -ENODEV;
	}
	
	LOG_INF("Found LoRa device: %s", lora_dev->name);
	
	if (!device_is_ready(lora_dev)) {
		LOG_ERR("LoRa device not ready: %s", lora_dev->name);
		return -ENODEV;
	}

	ctx->lora_dev = lora_dev;
	ctx->running = false;

	/* Initialize default modem configuration */
	ctx->modem_config.frequency = 915000000;  /* 915 MHz */
	ctx->modem_config.bandwidth = BW_125_KHZ;
	ctx->modem_config.datarate = SF_10;  /* Use SF10 as it works reliably */
	ctx->modem_config.preamble_len = 8;
	ctx->modem_config.coding_rate = CR_4_5;
	ctx->modem_config.tx_power = CONFIG_AETHER_LORA_TX_POWER;
	ctx->modem_config.tx = true;  /* Enable TX */

	/* Initialize message queues */
	k_msgq_init(&ctx->rx_msgq, ctx->rx_msgq_buffer, sizeof(struct net_pkt *),
		    CONFIG_AETHER_RX_QUEUE_SIZE);
	k_msgq_init(&ctx->tx_msgq, ctx->tx_msgq_buffer, sizeof(struct net_pkt *),
		    CONFIG_AETHER_TX_QUEUE_SIZE);

	LOG_INF("Æther LoRa driver initialized for device %s", lora_dev->name);
	return 0;
}

/* Network interface registration - use high priority (90) to init after LoRa driver */
NET_DEVICE_INIT(aether_lora_net, "aether_lora_net", aether_lora_init, NULL,
		&aether_lora_ctx_0, NULL, 90,
		&aether_lora_radio_api, HEYMAC_L2, HEYMAC_L2_CTX_TYPE,
		HEYMAC_MAX_PAYLOAD);
