/*
 * Æther reliable-datagram engine (DELIV_RELIABLE) — see
 * docs/AETHER_PROTOCOL_SPEC.md §4.6.
 *
 * Pure ARQ state machine: no net_pkt / net_if / kernel-clock dependency.
 * The current time (ms) is passed in and all I/O is via callbacks, so the
 * engine is host-testable (see tests) and the networking glue lives in the
 * mesh layer.
 *
 * Copyright (c) 2025 Æther Authors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/net/aether_reliable.h>
#include <string.h>
#include <errno.h>

/* Idle peers are reclaimed after this long with no traffic (also the
 * resync window after a sender reset — see the flow-start note in §4.6). */
#define AETHER_RELIABLE_IDLE_MS 60000

/* Forward distance from base to a in mod-256 space. */
static inline uint8_t fwd(uint8_t a, uint8_t base)
{
	return (uint8_t)(a - base);
}

/* ---- wire helpers ---- */

void aether_ack_encode(const struct aether_ack *a, uint8_t out[AETHER_ACK_LEN])
{
	out[0] = a->cum_ack;
	out[1] = a->rwnd;
	out[2] = a->sack;
}

void aether_ack_decode(const uint8_t in[AETHER_ACK_LEN], struct aether_ack *a)
{
	a->cum_ack = in[0];
	a->rwnd = in[1];
	a->sack = in[2];
}

/* ---- peer table ---- */

static void peer_init(struct aether_reliable_peer *p, const uint8_t addr[6],
		      uint32_t now)
{
	memset(p, 0, sizeof(*p));
	memcpy(p->addr, addr, 6);
	p->in_use = true;
	p->last_ms = now;
}

static struct aether_reliable_peer *peer_find(struct aether_reliable_ctx *c,
					      const uint8_t addr[6])
{
	for (int i = 0; i < AETHER_RELIABLE_PEERS; i++) {
		if (c->peers[i].in_use &&
		    memcmp(c->peers[i].addr, addr, 6) == 0) {
			return &c->peers[i];
		}
	}
	return NULL;
}

/* Find or create. When the table is full, evict the least-recently-used
 * peer (soft state — losing it just forces a flow-start resync). */
static struct aether_reliable_peer *peer_get(struct aether_reliable_ctx *c,
					     const uint8_t addr[6], uint32_t now)
{
	struct aether_reliable_peer *p = peer_find(c, addr);
	if (p) {
		return p;
	}
	struct aether_reliable_peer *lru = &c->peers[0];
	for (int i = 0; i < AETHER_RELIABLE_PEERS; i++) {
		if (!c->peers[i].in_use) {
			peer_init(&c->peers[i], addr, now);
			return &c->peers[i];
		}
		if ((int32_t)(c->peers[i].last_ms - lru->last_ms) < 0) {
			lru = &c->peers[i];
		}
	}
	peer_init(lru, addr, now);
	return lru;
}

/* ---- ACK send ---- */

static void send_ack(struct aether_reliable_ctx *c,
		     struct aether_reliable_peer *p)
{
	uint8_t W = c->cfg.window;
	struct aether_ack a;

	a.cum_ack = (uint8_t)(p->rx_expected - 1);
	a.sack = 0;
	for (int i = 0; i < 8 && i < W - 1; i++) {
		uint8_t s = (uint8_t)(p->rx_expected + 1 + i);
		if (p->rx[s % W].in_use) {
			a.sack |= (uint8_t)(1u << i);
		}
	}
	uint8_t used = 0;
	for (int i = 0; i < W; i++) {
		if (p->rx[i].in_use) {
			used++;
		}
	}
	a.rwnd = (uint8_t)(W - used);

	if (c->io.tx_ack) {
		c->io.tx_ack(c->io.user, p->addr, &a);
	}
	p->ack_pending = false;
}

static void schedule_ack(struct aether_reliable_ctx *c,
			 struct aether_reliable_peer *p, uint32_t now,
			 bool immediate)
{
	if (immediate || c->cfg.ack_delay_ms == 0) {
		send_ack(c, p);
		return;
	}
	if (!p->ack_pending) {
		p->ack_pending = true;
		p->ack_due_ms = now + c->cfg.ack_delay_ms;
	}
}

/* ---- public API ---- */

void aether_reliable_init(struct aether_reliable_ctx *c,
			  const struct aether_reliable_cfg *cfg,
			  const struct aether_reliable_io *io)
{
	memset(c, 0, sizeof(*c));
	c->cfg = *cfg;
	if (c->cfg.window == 0 || c->cfg.window > AETHER_RELIABLE_WINDOW) {
		c->cfg.window = AETHER_RELIABLE_WINDOW;
	}
	c->io = *io;
}

int aether_reliable_send(struct aether_reliable_ctx *c, const uint8_t dst[6],
			 const uint8_t *data, uint16_t len, uint32_t now)
{
	if (len > AETHER_RELIABLE_MTU) {
		return -E2BIG;
	}
	struct aether_reliable_peer *p = peer_get(c, dst, now);
	if (!p) {
		return -ENOSPC;
	}

	uint8_t W = c->cfg.window;
	if (fwd(p->tx_next, p->tx_base) >= W) {
		return -EAGAIN; /* window full — caller backpressures */
	}

	uint8_t seq = p->tx_next;
	struct aether_reliable_tx_slot *s = &p->tx[seq % W];

	s->seq = seq;
	s->len = len;
	memcpy(s->data, data, len);
	s->sent_ms = now;
	s->retries = 0;
	s->in_use = true;
	s->flow_start = !p->tx_opened;

	p->tx_opened = true;
	p->tx_next++;
	p->last_ms = now;

	if (c->io.tx_data) {
		c->io.tx_data(c->io.user, dst, seq, s->flow_start, s->data, len);
	}
	return 0;
}

void aether_reliable_on_data(struct aether_reliable_ctx *c,
			     const uint8_t src[6], uint8_t seq, bool flow_start,
			     const uint8_t *data, uint16_t len, uint32_t now)
{
	if (len > AETHER_RELIABLE_MTU) {
		return; /* cannot buffer; drop */
	}
	struct aether_reliable_peer *p = peer_get(c, src, now);
	if (!p) {
		return;
	}
	uint8_t W = c->cfg.window;
	p->last_ms = now;

	/* Flow-start rebases ordering, but only when we have no open flow.
	 * A flow-start to an already-open flow is a retransmit of the opener
	 * (its ACK was lost) — fall through and treat it by sequence (it will
	 * read as a past duplicate and be re-ACKed). */
	if (flow_start && !p->rx_opened) {
		for (int i = 0; i < W; i++) {
			p->rx[i].in_use = false;
		}
		p->rx_expected = seq;
		p->rx_opened = true;
	} else if (!p->rx_opened) {
		/* No baseline yet and not a flow-start: we can't place this
		 * segment. Drop silently; the sender's RTO will retransmit the
		 * flow-start opener and we resync then. */
		return;
	}

	uint8_t d = fwd(seq, p->rx_expected);

	if (d == 0) {
		/* in order */
		if (c->io.deliver) {
			c->io.deliver(c->io.user, src, data, len);
		}
		p->rx_expected++;
		/* drain any contiguous buffered successors */
		while (p->rx[p->rx_expected % W].in_use) {
			struct aether_reliable_rx_slot *r =
				&p->rx[p->rx_expected % W];
			if (c->io.deliver) {
				c->io.deliver(c->io.user, src, r->data, r->len);
			}
			r->in_use = false;
			p->rx_expected++;
		}
		schedule_ack(c, p, now, false);
	} else if (d < W) {
		/* future, within window → buffer out of order */
		struct aether_reliable_rx_slot *r = &p->rx[seq % W];
		if (!r->in_use) {
			r->in_use = true;
			r->len = len;
			memcpy(r->data, data, len);
		}
		schedule_ack(c, p, now, true); /* gap → ACK immediately */
	} else {
		/* past (already-delivered duplicate) or out of window → re-ACK */
		schedule_ack(c, p, now, true);
	}
}

void aether_reliable_on_ack(struct aether_reliable_ctx *c,
			    const uint8_t src[6], const struct aether_ack *ack,
			    uint32_t now)
{
	struct aether_reliable_peer *p = peer_find(c, src);
	if (!p) {
		return;
	}
	uint8_t W = c->cfg.window;
	uint8_t cum = ack->cum_ack;
	p->last_ms = now;

	/* cumulative: free everything in flight up to and including cum_ack */
	while (p->tx_base != p->tx_next && fwd(cum, p->tx_base) < W) {
		p->tx[p->tx_base % W].in_use = false;
		p->tx_base++;
	}

	/* selective: free out-of-order-received slots above the hole */
	for (int i = 0; i < 8; i++) {
		if (!(ack->sack & (1u << i))) {
			continue;
		}
		uint8_t s_seq = (uint8_t)(cum + 2 + i);
		if (fwd(s_seq, p->tx_base) < W) {
			struct aether_reliable_tx_slot *s = &p->tx[s_seq % W];
			if (s->in_use && s->seq == s_seq) {
				s->in_use = false;
			}
		}
	}

	/* fast retransmit the hole when the SACK shows progress beyond it */
	if (ack->sack && p->tx_base != p->tx_next) {
		struct aether_reliable_tx_slot *s = &p->tx[p->tx_base % W];
		if (s->in_use && s->seq == p->tx_base) {
			s->sent_ms = now;
			if (c->io.tx_data) {
				c->io.tx_data(c->io.user, src, s->seq,
					      s->flow_start, s->data, s->len);
			}
		}
	}
}

void aether_reliable_tick(struct aether_reliable_ctx *c, uint32_t now)
{
	uint8_t W = c->cfg.window;

	for (int i = 0; i < AETHER_RELIABLE_PEERS; i++) {
		struct aether_reliable_peer *p = &c->peers[i];
		if (!p->in_use) {
			continue;
		}

		/* due delayed ACK */
		if (p->ack_pending && (int32_t)(now - p->ack_due_ms) >= 0) {
			send_ack(c, p);
		}

		/* RTO retransmission over in-flight slots */
		bool reset = false;
		for (uint8_t s_seq = p->tx_base; s_seq != p->tx_next; s_seq++) {
			struct aether_reliable_tx_slot *s = &p->tx[s_seq % W];
			if (!s->in_use || s->seq != s_seq) {
				continue;
			}
			uint8_t shift = s->retries < 6 ? s->retries : 6;
			uint32_t rto = c->cfg.rto_base_ms << shift;
			if ((int32_t)(now - s->sent_ms) < (int32_t)rto) {
				continue;
			}
			if (s->retries >= c->cfg.max_retries) {
				if (c->io.failed) {
					c->io.failed(c->io.user, p->addr, s->seq);
				}
				/* reliable guarantee broken → drop the peer; the
				 * upper-layer session (e.g. a 9P Tag) times out */
				memset(p, 0, sizeof(*p));
				reset = true;
				break;
			}
			s->retries++;
			s->sent_ms = now;
			if (c->io.tx_data) {
				c->io.tx_data(c->io.user, p->addr, s->seq,
					      s->flow_start, s->data, s->len);
			}
		}
		if (reset) {
			continue;
		}

		/* idle GC: reclaim quiet peers with nothing in flight */
		if (p->tx_base == p->tx_next && !p->ack_pending &&
		    (int32_t)(now - p->last_ms) >= AETHER_RELIABLE_IDLE_MS) {
			memset(p, 0, sizeof(*p));
		}
	}
}
