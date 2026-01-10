/*
 * Copyright (c) 2025 Æther Authors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(heymac_l2, CONFIG_NET_L2_HEYMAC_LOG_LEVEL);

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net/net_l2.h>
#include <zephyr/net/heymac.h>
#include <zephyr/random/random.h>
#include <string.h>

/* HeyMac L2 implementation following Zephyr conventions */

static enum net_verdict heymac_l2_recv(struct net_if *iface, struct net_pkt *pkt)
{
	/* This function is called when a packet is received from the radio */
	heymac_recv(iface, pkt);
	return NET_CONTINUE;
}

static int heymac_l2_send(struct net_if *iface, struct net_pkt *pkt)
{
	struct heymac_context *ctx;
	const struct heymac_radio_api *radio_api;
	int ret;

	if (!iface || !pkt) {
		return -EINVAL;
	}

	ctx = net_if_l2_data(iface);
	if (!ctx) {
		return -EINVAL;
	}

	radio_api = net_if_get_device(iface)->api;
	if (!radio_api || !radio_api->tx) {
		return -ENOTSUP;
	}

	k_mutex_lock(&ctx->tx_lock, K_FOREVER);

	/* Send via radio - let the driver handle HeyMac framing */
	ret = radio_api->tx(net_if_get_device(iface), pkt);
	if (ret < 0) {
		LOG_ERR("Radio TX failed: %d", ret);
		ctx->tx_errors++;
	} else {
		ctx->tx_packets++;
		LOG_DBG("Transmitted frame, len=%zu", net_pkt_get_len(pkt));
	}

	k_mutex_unlock(&ctx->tx_lock);
	return ret;
}

static int heymac_l2_enable(struct net_if *iface, bool state)
{
	const struct heymac_radio_api *radio_api;
	int ret = 0;

	if (!iface) {
		return -EINVAL;
	}

	radio_api = net_if_get_device(iface)->api;
	if (!radio_api) {
		return -ENOTSUP;
	}

	if (state) {
		/* Initialize HeyMac context */
		ret = heymac_init(iface);
		if (ret < 0) {
			LOG_ERR("Failed to initialize HeyMac: %d", ret);
			return ret;
		}

		/* Start radio */
		if (radio_api->start) {
			ret = radio_api->start(net_if_get_device(iface));
			if (ret < 0) {
				LOG_ERR("Failed to start radio: %d", ret);
				return ret;
			}
		}

		LOG_INF("HeyMac L2 enabled on interface %p", iface);
	} else {
		/* Stop radio */
		if (radio_api->stop) {
			ret = radio_api->stop(net_if_get_device(iface));
			if (ret < 0) {
				LOG_ERR("Failed to stop radio: %d", ret);
			}
		}

		LOG_INF("HeyMac L2 disabled on interface %p", iface);
	}

	return ret;
}

static enum net_l2_flags heymac_l2_flags(struct net_if *iface)
{
	ARG_UNUSED(iface);
	
	return NET_L2_MULTICAST;  /* Support broadcast/multicast */
}

NET_L2_INIT(HEYMAC, heymac_l2_recv, heymac_l2_send, heymac_l2_enable, heymac_l2_flags);

/* Network interface initialization for HeyMac devices */
void heymac_iface_init(struct net_if *iface)
{
	const struct device *dev = net_if_get_device(iface);

	/* Set MTU based on HeyMac maximum payload */
	net_if_set_mtu(iface, HEYMAC_MAX_PAYLOAD);

	LOG_DBG("HeyMac interface %p initialized for device %s", 
		iface, dev->name);
}