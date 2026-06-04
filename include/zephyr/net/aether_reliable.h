/*
 * Æther reliable-datagram engine (DELIV_RELIABLE) — see
 * docs/AETHER_PROTOCOL_SPEC.md §4.6.
 *
 * This is the pure ARQ state machine: per-peer sequencing, the
 * Acknowledgment (type 0x01) wire block, selective retransmission, and RTO.
 * It has NO dependency on net_pkt / net_if / the kernel clock — the current
 * time is passed in, and all I/O happens through caller-supplied callbacks.
 * That keeps it host-testable (two instances over a lossy loopback) and lets
 * the networking glue (frame build, TX queue, periodic tick) live separately
 * in the mesh layer.
 *
 * Copyright (c) 2025 Æther Authors
 * SPDX-License-Identifier: MIT
 */

#ifndef ZEPHYR_INCLUDE_NET_AETHER_RELIABLE_H_
#define ZEPHYR_INCLUDE_NET_AETHER_RELIABLE_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Acknowledgment payload (§4.6.3) — also appended to a DATA frame flagged
 * AETHER_FCTL_EXTENDED when piggybacked.  Wire layout is exactly these three
 * bytes in order. */
struct aether_ack {
	uint8_t cum_ack; /* highest seq received in order; all <= it delivered */
	uint8_t rwnd;    /* receiver free slots, in packets (flow control)      */
	uint8_t sack;    /* bitmap: bit i set => seq (cum_ack + 2 + i) received  */
};
#define AETHER_ACK_LEN 3

/* Compile-time caps (§4.6.8).  Window MUST stay < 64 so selective repeat is
 * unambiguous in the 1-byte (mod-256) sequence space. */
#ifndef CONFIG_AETHER_RELIABLE_WINDOW
#define AETHER_RELIABLE_WINDOW 8
#else
#define AETHER_RELIABLE_WINDOW CONFIG_AETHER_RELIABLE_WINDOW
#endif

#ifndef CONFIG_AETHER_RELIABLE_PEERS
#define AETHER_RELIABLE_PEERS 4
#else
#define AETHER_RELIABLE_PEERS CONFIG_AETHER_RELIABLE_PEERS
#endif

#ifndef CONFIG_AETHER_RELIABLE_MTU
#define AETHER_RELIABLE_MTU 200
#else
#define AETHER_RELIABLE_MTU CONFIG_AETHER_RELIABLE_MTU
#endif

/* Per-PHY tunables (§4.6.8). */
struct aether_reliable_cfg {
	uint8_t  window;        /* in-flight packets (<= AETHER_RELIABLE_WINDOW) */
	uint32_t rto_base_ms;   /* initial RTO before exponential backoff        */
	uint16_t ack_delay_ms;  /* delayed/coalesced-ACK window (0 = ack now)    */
	uint8_t  max_retries;   /* before a slot is declared a delivery failure  */
};

struct aether_reliable_ctx; /* forward */

/* I/O callbacks.  The engine never touches the wire directly. */
struct aether_reliable_io {
	/* Transmit a reliable DATA segment to dst (best-effort; the engine
	 * handles retransmission).  flow_start marks the first segment of a
	 * fresh per-peer sequence space (the glue sets a header bit); the peer
	 * rebases rx_expected on it, which bootstraps ordering without a
	 * handshake and re-syncs a peer that lost its soft state.  Return 0 on
	 * success. */
	int  (*tx_data)(void *user, const uint8_t dst[6], uint8_t seq,
			bool flow_start, const uint8_t *data, uint16_t len);
	/* Transmit a standalone ACK (mesh type 0x01) to dst. */
	int  (*tx_ack)(void *user, const uint8_t dst[6],
		       const struct aether_ack *ack);
	/* Deliver an in-order, gap-free payload up to the application. */
	void (*deliver)(void *user, const uint8_t src[6],
			const uint8_t *data, uint16_t len);
	/* A slot exhausted max_retries — delivery failed (optional, may be
	 * NULL).  The upper layer (e.g. a 9P Tag) should time out. */
	void (*failed)(void *user, const uint8_t dst[6], uint8_t seq);
	void *user;
};

struct aether_reliable_tx_slot {
	uint8_t  seq;
	uint16_t len;
	uint32_t sent_ms;
	uint8_t  retries;
	bool     in_use;
	bool     flow_start; /* re-send the flow-start bit on retransmit */
	uint8_t  data[AETHER_RELIABLE_MTU];
};

struct aether_reliable_rx_slot {
	uint16_t len;
	bool     in_use;
	uint8_t  data[AETHER_RELIABLE_MTU];
};

struct aether_reliable_peer {
	uint8_t  addr[6];
	bool     in_use;
	uint32_t last_ms;       /* for idle GC */

	/* TX */
	bool     tx_opened;     /* false until the first send (flow-start)   */
	uint8_t  tx_next;       /* next seq to assign                        */
	uint8_t  tx_base;       /* oldest unacked seq (== tx_next if empty)  */
	struct aether_reliable_tx_slot tx[AETHER_RELIABLE_WINDOW];

	/* RX */
	bool     rx_opened;     /* false until a flow-start has rebased us   */
	uint8_t  rx_expected;   /* next in-order seq expected                */
	struct aether_reliable_rx_slot rx[AETHER_RELIABLE_WINDOW];

	/* deferred ACK */
	bool     ack_pending;
	uint32_t ack_due_ms;
};

struct aether_reliable_ctx {
	struct aether_reliable_cfg cfg;
	struct aether_reliable_io  io;
	struct aether_reliable_peer peers[AETHER_RELIABLE_PEERS];
};

/* ---- wire helpers ---- */
void aether_ack_encode(const struct aether_ack *a, uint8_t out[AETHER_ACK_LEN]);
void aether_ack_decode(const uint8_t in[AETHER_ACK_LEN], struct aether_ack *a);

/* ---- engine ---- */

/** Initialize the engine with per-PHY config and I/O callbacks. */
void aether_reliable_init(struct aether_reliable_ctx *c,
			  const struct aether_reliable_cfg *cfg,
			  const struct aether_reliable_io *io);

/**
 * Application send (DELIV_RELIABLE).  Buffers the payload, assigns a seq, and
 * calls io.tx_data once.  Returns 0, -EAGAIN if the peer's window is full
 * (apply backpressure / retry), or -E2BIG / -ENOSPC.
 */
int aether_reliable_send(struct aether_reliable_ctx *c, const uint8_t dst[6],
			 const uint8_t *data, uint16_t len, uint32_t now_ms);

/** A reliable DATA segment arrived (§4.6.6 receiver).  flow_start is the
 *  decoded header bit: when set, the receiver rebases its sequence to seq. */
void aether_reliable_on_data(struct aether_reliable_ctx *c,
			     const uint8_t src[6], uint8_t seq, bool flow_start,
			     const uint8_t *data, uint16_t len, uint32_t now_ms);

/** An ACK arrived — standalone or piggybacked (§4.6.5 sender). */
void aether_reliable_on_ack(struct aether_reliable_ctx *c,
			    const uint8_t src[6], const struct aether_ack *ack,
			    uint32_t now_ms);

/**
 * Periodic tick: fires due delayed-ACKs and RTO retransmissions.  Call from a
 * timer/workqueue at roughly the smallest ack_delay/RTO granularity.
 */
void aether_reliable_tick(struct aether_reliable_ctx *c, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_NET_AETHER_RELIABLE_H_ */
