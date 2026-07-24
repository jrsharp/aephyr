/*
 * /net/aether — Plan 9-style network interface to the Æther mesh.
 *
 * Canonical home of the fs formerly at FRST src/dev_aether_net.h; see
 * aether_net_fs.c for the consumer contract (CONFIG_AETHER_NET_FS).
 *
 * Copyright (c) 2024-2026 Jon Sharp
 * Copyright (c) 2026 Æther Authors
 * SPDX-License-Identifier: MIT
 */

#ifndef ZEPHYR_INCLUDE_NET_AETHER_NET_FS_H_
#define ZEPHYR_INCLUDE_NET_AETHER_NET_FS_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * App-layer protocol marker (first payload byte) for /net/aether best-effort
 * broadcast datagrams — the connectionless "party line" delivery class, ridden
 * by the chat lobby.  Distinct from chat's legacy 0x00 and chess's 0x04; the
 * best-effort recv dispatch routes this byte to aether_net_deliver_bcast().
 */
#define AETHER_NET_DGRAM_PROTO 0x05

/**
 * Register the /net/aether 9P server.
 *
 * Exposes the Æther mesh as a Plan 9 network protocol directory
 * (clone / <N>/{ctl,data,local,remote,status}), with reliable conversations
 * carried by the §4.6 DELIV_RELIABLE engine.  Requires the mesh to be up
 * (aether_mesh_init already called).
 *
 * @return 0 on success, negative errno otherwise.
 */
int dev_aether_net_init(void);

/**
 * Kernel-side per-conversation receive callback.
 *
 * When set on a conversation, anet_rx delivers each reassembled message
 * directly to this callback (on the mesh workqueue) instead of buffering it in
 * the data-file RX ring.  This lets an in-kernel consumer — the 9P client
 * transport — ride the existing mesh RX path with no blocking reader thread
 * (and no per-dial stack).  Runs on the mesh workqueue (~1536 B stack): the
 * callback MUST be shallow (e.g. signal a waiter); it must not run the deep
 * 9P-server reply path.  @src is the datagram source, @msg/@len the message.
 */
typedef void (*aether_net_krecv_t)(const uint8_t src[6], const uint8_t *msg,
				   uint16_t len, void *user);

/**
 * Set (or clear, with cb == NULL) the kernel recv callback for conversation
 * @slot.  @return 0, or -EINVAL for an invalid/unallocated slot.
 */
int aether_net_set_krecv(int slot, aether_net_krecv_t cb, void *user);

/**
 * Deliver a best-effort broadcast datagram into every open broadcast
 * conversation's RX ring (source-prefixed on read, like the announced path).
 * Called from the best-effort mesh recv dispatch for AETHER_NET_DGRAM_PROTO
 * frames — the RX tap for the party-line lobby.  Best-effort: silently drops
 * the datagram for any conversation whose ring is full.
 */
void aether_net_deliver_bcast(const uint8_t src[6], const uint8_t *data,
			      uint16_t len);

/**
 * Recompute the beacon's advertised service list from what is actually
 * served (the single derivation point — advertised-but-unserved must not
 * be expressible): aether 9P export on ⇒ `9fs`; a live announced
 * conversation ⇒ `chat`.  Call after anything that changes either.
 * No-op unless CONFIG_AETHER_SVCDISC.
 */
void aether_net_readvertise(void);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_NET_AETHER_NET_FS_H_ */
