/*
 * Copyright (c) 2026 Æther Authors
 *
 * SPDX-License-Identifier: MIT
 *
 * HONR16 structured tree routing for the Æther mesh. Maps the 16-bit HONR
 * address into the low two bytes of the 6-byte mesh address and uses the pure
 * address math in <zephyr/net/honr.h> to make stateless forwarding decisions:
 * up to the nearest common ancestor of resender and destination, then down.
 *
 * Selected by CONFIG_AETHER_ROUTING_HONR. Requires HONR address assignment
 * (tree join, a later step) for the addresses to form a valid tree.
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(aether_honr, CONFIG_AETHER_MESH_LOG_LEVEL);

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/aether_mesh.h>
#include <zephyr/net/honr.h>
#include <errno.h>

extern struct aether_mesh_ctx *g_mesh_ctx;

uint16_t aether_honr_addr16(const uint8_t addr6[6])
{
	return (uint16_t)((addr6[4] << 8) | addr6[5]);
}

int aether_honr_next_hop(struct net_if *iface, const uint8_t dst[6], uint8_t next_hop[6])
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;
	uint16_t self, dst16, nh;

	if (!ctx || ctx->iface != iface || !dst || !next_hop) {
		return -EINVAL;
	}

	self = aether_honr_addr16(ctx->local_addr);
	dst16 = aether_honr_addr16(dst);

	if (self == dst16) {
		return -EALREADY;
	}

	nh = honr_next_hop(self, dst16);

	/* Carry the 16-bit HONR next hop in the low two address bytes. */
	memset(next_hop, 0, 6);
	next_hop[4] = (uint8_t)(nh >> 8);
	next_hop[5] = (uint8_t)(nh & 0xFF);

	LOG_DBG("HONR next hop %04x -> %04x = %04x", self, dst16, nh);
	return 0;
}

bool aether_honr_should_relay(struct net_if *iface, const uint8_t resender[6],
			      const uint8_t dst[6])
{
	struct aether_mesh_ctx *ctx = g_mesh_ctx;
	uint16_t self, res16, dst16;

	if (!ctx || ctx->iface != iface || !resender || !dst) {
		return false;
	}

	self = aether_honr_addr16(ctx->local_addr);
	res16 = aether_honr_addr16(resender);
	dst16 = aether_honr_addr16(dst);

	return honr_should_relay(self, res16, dst16);
}
