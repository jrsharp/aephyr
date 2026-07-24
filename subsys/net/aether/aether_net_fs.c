/*
 * /net/aether — Plan 9-style network interface to the Æther mesh.
 *
 * A protocol directory under /net (alongside /net/tcp, /net/wifi), reached
 * by longest-prefix server lookup.  Mirrors the 9front /net/<proto> shape:
 *
 *   /net/aether/
 *       clone                 open -> slot N; the fid becomes <N>/ctl; read -> "N"
 *       status                summary (conversations, neighbours)
 *       addr                  our 6-byte mesh address
 *       self/
 *           name              this node's mesh name (write to set)
 *           services          advertised service names (derived from what
 *                             is actually served — read-only)
 *       peers/
 *           <addr>/           discovery peer table, keyed by address
 *               name          display label from the peer's last beacon
 *               services      learned from beacons
 *               lastseen      seconds since last beacon
 *               rssi          last-heard signal, advisory only
 *       <N>/
 *           ctl               write: "connect XX:XX[:..]" | "hangup"
 *           data              write: send to the connected peer (reliable);
 *                             read: received datagrams (RX path: TODO)
 *           local             our address
 *           remote            peer address
 *           status            conversation state
 *
 * Reliable conversations ride the §4.6 DELIV_RELIABLE engine via
 * aether_mesh_reliable_send().
 *
 * CANONICAL HOME.  This file moved here from FRST (src/dev_aether_net.c,
 * where it was battle-tested) so every Æther node — deck, DECT/BLE
 * gateway, sim — serves ONE /net/aether implementation.  Consumer
 * contract (CONFIG_AETHER_NET_FS):
 *   - the app provides FRST's srv framework ("srv.h": srv_register,
 *     srv_make_qid, srv_dir_add_entry, struct srv_ops) and "mem.h"
 *     (mem_alloc/mem_free, __psram) on <app>/src — the module CMake adds
 *     that include path;
 *   - the 9p4z module supplies <zephyr/9p/protocol.h>;
 *   - aether_9p_exporting_now() may be overridden (weak default: false)
 *     by an app with an opt-in 9P namespace export (FRST's aexport).
 *
 * Copyright (c) 2024-2026 Jon Sharp
 * Copyright (c) 2026 Æther Authors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "srv.h"
#include "mem.h"
#include <zephyr/net/aether_net_fs.h>

LOG_MODULE_REGISTER(aether_net_fs, CONFIG_LOG_DEFAULT_LEVEL);

#if defined(CONFIG_AETHER_MESH)

#include <zephyr/9p/protocol.h>
#include <zephyr/net/aether_mesh.h>
#include <zephyr/net/aether_reliable.h>
#ifdef CONFIG_AETHER_SVCDISC
#include <zephyr/net/aether_svcdisc.h>
#endif

/* The mesh context (neighbour table) -- read by /net/aether/neighbours, same
 * accessor /dev/aether/peers uses. */
extern struct aether_mesh_ctx *g_mesh_ctx;

#ifdef CONFIG_NINEP_SERVER
/* Whether the app's opt-in 9P namespace export is live (drives the `9fs`
 * advertisement).  FRST's main.c overrides this with its aexport state;
 * a gateway with no such toggle inherits the default. */
__weak bool aether_9p_exporting_now(void)
{
	return false;
}
#endif

#define AETHER_NET_MAX_CONNS 4
#define AETHER_NET_RX_RING   4   /* messages buffered per conversation */
#define AETHER_NET_MAX_MSG   512  /* reassembled message ceiling          */
/* 1-byte fragment header on each engine payload: */
#define AETHER_NET_FRAG_MORE  0x80 /* more fragments follow                */
#define AETHER_NET_FRAG_START 0x40 /* first fragment of a message          */

enum anf_type {
	ANF_ROOT,      /* /net/aether            */
	ANF_CLONE,     /* /net/aether/clone      */
	ANF_STATUS,    /* /net/aether/status     */
	ANF_STATS,     /* /net/aether/stats      */
	ANF_PHY,       /* /net/aether/phy        */
	ANF_ADDR,      /* /net/aether/addr       */
	ANF_NEIGHBOURS,/* /net/aether/neighbours */
	ANF_CONN_DIR,  /* /net/aether/<N>        */
	ANF_CTL,       /* /net/aether/<N>/ctl    */
	ANF_DATA,      /* /net/aether/<N>/data   */
	ANF_LOCAL,     /* /net/aether/<N>/local  */
	ANF_REMOTE,    /* /net/aether/<N>/remote */
	ANF_CSTATUS,   /* /net/aether/<N>/status */
#ifdef CONFIG_AETHER_SVCDISC
	ANF_SELF_DIR,      /* /net/aether/self                  */
	ANF_SELF_NAME,     /* /net/aether/self/name             */
	ANF_SELF_SVCS,     /* /net/aether/self/services         */
	ANF_PEERS_DIR,     /* /net/aether/peers                 */
	ANF_PEER_DIR,      /* /net/aether/peers/<addr>          */
	ANF_PEER_NAME,     /* /net/aether/peers/<addr>/name     */
	ANF_PEER_SVCS,     /* /net/aether/peers/<addr>/services */
	ANF_PEER_LASTSEEN, /* /net/aether/peers/<addr>/lastseen */
	ANF_PEER_RSSI,     /* /net/aether/peers/<addr>/rssi     */
#endif
};

struct anf_data {
	enum anf_type type;
	int slot;         /* -1 if not applicable */
	uint8_t addr[6];  /* peer files: which table entry (the stable key) */
};

struct arx_msg {
	uint16_t len;
	uint8_t  src[6];
	uint8_t  data[AETHER_NET_MAX_MSG];
};

/* A conversation.  `connected` binds it to one peer (the client side, via
 * ctl connect); `announced` receives reliable datagrams from any peer (the
 * server side, via ctl announce — reads are source-prefixed). */
struct aconn {
	bool    in_use;
	bool    connected;
	bool    announced;
	bool    bcast;      /* connected to the broadcast addr: best-effort party line */
	uint8_t peer[6];

	/* RX ring of reassembled messages (filled by the reliable-deliver
	 * callback, drained by data reads). */
	struct arx_msg rx[AETHER_NET_RX_RING];
	uint8_t        rx_head;   /* oldest */
	uint8_t        rx_count;
	struct k_sem   rx_sem;    /* counts buffered messages */

	/* Fragment reassembly (keyed on source; one in-progress message). */
	uint8_t  ra_buf[AETHER_NET_MAX_MSG];
	uint16_t ra_len;
	uint8_t  ra_src[6];
	bool     ra_active;

	/* Optional in-kernel consumer: when set, completed messages are
	 * delivered here (on the mesh workq) instead of the rx ring — see
	 * aether_net_set_krecv. */
	aether_net_krecv_t krecv;
	void              *krecv_user;
};

/* Large (RX rings + reassembly) — keep in PSRAM on the deck. */
static struct aconn aconns[AETHER_NET_MAX_CONNS] __psram;
static struct k_mutex aconns_lock;

/* ---- small read/dir helpers (mirrors dev_net.c) ---- */

static bool anf_is_dir(enum anf_type t)
{
	return t == ANF_ROOT || t == ANF_CONN_DIR
#ifdef CONFIG_AETHER_SVCDISC
	       || t == ANF_SELF_DIR || t == ANF_PEERS_DIR || t == ANF_PEER_DIR
#endif
	       ;
}

static struct ninep_qid anf_qid(enum anf_type t, int slot)
{
	uint8_t qt = anf_is_dir(t) ? NINEP_QTDIR : NINEP_QTFILE;
	uint64_t path = ((uint64_t)t << 32) | (uint32_t)(slot + 1);
	return srv_make_qid(qt, 0, path);
}

#ifdef CONFIG_AETHER_SVCDISC
/* Peer files: qid path carries the 48-bit addr above the type, disjoint
 * from the slot scheme (whose high 16 bits are always zero). */
static struct ninep_qid anf_peer_qid(enum anf_type t, const uint8_t addr[6])
{
	uint64_t a48 = 0;
	for (int i = 0; i < 6; i++) {
		a48 = (a48 << 8) | addr[i];
	}
	uint8_t qt = anf_is_dir(t) ? NINEP_QTDIR : NINEP_QTFILE;
	return srv_make_qid(qt, 0, ((uint64_t)t << 48) ^ a48);
}
#endif

static struct anf_data *new_anf(enum anf_type t, int slot)
{
	struct anf_data *d = mem_alloc(sizeof(*d));
	if (d) {
		memset(d, 0, sizeof(*d));
		d->type = t;
		d->slot = slot;
	}
	return d;
}

static int emit_string(const char *s, uint64_t off, uint8_t *buf, uint32_t cnt)
{
	size_t len = strlen(s);
	if (off >= len) {
		return 0;
	}
	size_t n = len - off;
	if (n > cnt) {
		n = cnt;
	}
	memcpy(buf, s + off, n);
	return (int)n;
}

static int emit_fmt(uint64_t off, uint8_t *buf, uint32_t cnt, const char *fmt, ...)
{
	char tmp[160];
	va_list ap;

	va_start(ap, fmt);
	int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (n < 0) {
		return -EIO;
	}
	return emit_string(tmp, off, buf, cnt);
}

struct dirent_spec {
	const char *name;
	enum anf_type type;
	uint32_t mode;
	int slot;
};

static int emit_dir(const struct dirent_spec *ents, int n, uint64_t off,
		    uint8_t *buf, uint32_t cnt)
{
	size_t stream_pos = 0, buf_pos = 0;
	uint8_t tmp[96];

	for (int i = 0; i < n; i++) {
		size_t tpos = 0;
		struct ninep_qid qid = anf_qid(ents[i].type, ents[i].slot);
		if (srv_dir_add_entry(tmp, sizeof(tmp), &tpos, ents[i].name,
				      &qid, ents[i].mode, 0) < 0) {
			break;
		}
		if (stream_pos >= off) {
			if (buf_pos + tpos > cnt) {
				break;
			}
			memcpy(buf + buf_pos, tmp, tpos);
			buf_pos += tpos;
		}
		stream_pos += tpos;
	}
	return (int)buf_pos;
}

static void parse_verb(const char *line, int len, char *verb, int vsz,
		       char *rest, int rsz)
{
	while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
			   line[len - 1] == ' ')) {
		len--;
	}
	int i = 0;
	while (i < len && line[i] != ' ' && i < vsz - 1) {
		verb[i] = line[i];
		i++;
	}
	verb[i] = '\0';
	while (i < len && line[i] == ' ') {
		i++;
	}
	int r = 0;
	while (i < len && r < rsz - 1) {
		rest[r++] = line[i++];
	}
	rest[r] = '\0';
}

/* "XX:XX:XX:XX:XX:XX" or short "XX:XX" (→ 00:00:00:00:XX:XX). */
static int parse_mac(const char *s, uint8_t out[6])
{
	unsigned int m[6];
	int n = sscanf(s, "%02x:%02x:%02x:%02x:%02x:%02x",
		       &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]);
	if (n == 6) {
		for (int i = 0; i < 6; i++) {
			out[i] = (uint8_t)m[i];
		}
		return 0;
	}
	if (n == 2) {
		memset(out, 0, 4);
		out[4] = (uint8_t)m[0];
		out[5] = (uint8_t)m[1];
		return 0;
	}
	return -EINVAL;
}

/* Address or discovered node name.  Name→addr binds here, at dial time:
 * unknown name → -ENOENT; two live addrs claiming it → -EADDRNOTAVAIL
 * with a log naming the ambiguity (an error the caller must see — never
 * a last-beacon-wins pick; dial by address instead). */
static int parse_peer(const char *s, uint8_t out[6])
{
	if (parse_mac(s, out) == 0) {
		return 0;
	}
#ifdef CONFIG_AETHER_SVCDISC
	struct aether_peer p;
	int hits = aether_peer_get_by_name(s, &p);

	if (hits == 1) {
		memcpy(out, p.addr, 6);
		return 0;
	}
	if (hits > 1) {
		LOG_WRN("name '%s' claimed by %d nodes - dial by address",
			s, hits);
		return -EADDRNOTAVAIL;
	}
	return -ENOENT;
#else
	return -EINVAL;
#endif
}

static int emit_mac(const uint8_t a[6], uint64_t off, uint8_t *buf, uint32_t cnt)
{
	if (!a) {
		return emit_string("\n", off, buf, cnt);
	}
	return emit_fmt(off, buf, cnt, "%02x:%02x:%02x:%02x:%02x:%02x\n",
			a[0], a[1], a[2], a[3], a[4], a[5]);
}

/* ---- conversation slots ---- */

static int alloc_aconn(void)
{
	k_mutex_lock(&aconns_lock, K_FOREVER);
	for (int i = 0; i < AETHER_NET_MAX_CONNS; i++) {
		if (!aconns[i].in_use) {
			aconns[i].in_use = true;
			aconns[i].connected = false;
			aconns[i].announced = false;
			aconns[i].bcast = false;
			memset(aconns[i].peer, 0, 6);
			aconns[i].rx_head = 0;
			aconns[i].rx_count = 0;
			aconns[i].ra_active = false;
			aconns[i].ra_len = 0;
			aconns[i].krecv = NULL;
			aconns[i].krecv_user = NULL;
			k_sem_init(&aconns[i].rx_sem, 0, AETHER_NET_RX_RING);
			k_mutex_unlock(&aconns_lock);
			return i;
		}
	}
	k_mutex_unlock(&aconns_lock);
	return -ENOSPC;
}

/* Recompute the advertised service list from what is actually served —
 * the single derivation point, so advertised-but-unserved is not
 * expressible: the opt-in aether 9P export advertises `9fs`; a live
 * announced conversation (a datagram server, e.g. `achat -l`)
 * advertises `chat`. */
void aether_net_readvertise(void)
{
#ifdef CONFIG_AETHER_SVCDISC
	struct aether_svc svc[2] = {0};
	uint8_t n = 0;

#ifdef CONFIG_NINEP_SERVER
	extern bool aether_9p_exporting_now(void);
	if (aether_9p_exporting_now()) {
		strcpy(svc[n++].name, "9fs");
	}
#endif
	bool chat = false;
	k_mutex_lock(&aconns_lock, K_FOREVER);
	for (int i = 0; i < AETHER_NET_MAX_CONNS; i++) {
		if (aconns[i].in_use && aconns[i].announced) {
			chat = true;
		}
	}
	k_mutex_unlock(&aconns_lock);
	if (chat) {
		strcpy(svc[n++].name, "chat");
	}
	aether_svcdisc_set_services(svc, n);
#endif
}

static void free_aconn(int s)
{
	if (s < 0 || s >= AETHER_NET_MAX_CONNS) {
		return;
	}
	k_mutex_lock(&aconns_lock, K_FOREVER);
	aconns[s].in_use = false;
	aconns[s].connected = false;
	aconns[s].bcast = false;
	bool was_announced = aconns[s].announced;
	aconns[s].announced = false;
	aconns[s].krecv = NULL;
	aconns[s].krecv_user = NULL;
	k_mutex_unlock(&aconns_lock);
	if (was_announced) {
		aether_net_readvertise();
	}
	/* wake any reader blocked on data so it returns EOF */
	k_sem_give(&aconns[s].rx_sem);
}

int aether_net_set_krecv(int slot, aether_net_krecv_t cb, void *user)
{
	if (slot < 0 || slot >= AETHER_NET_MAX_CONNS) {
		return -EINVAL;
	}
	k_mutex_lock(&aconns_lock, K_FOREVER);
	if (!aconns[slot].in_use) {
		k_mutex_unlock(&aconns_lock);
		return -EINVAL;
	}
	aconns[slot].krecv = cb;
	aconns[slot].krecv_user = user;
	k_mutex_unlock(&aconns_lock);
	return 0;
}

/* Reliable-delivery callback (runs in mesh RX / workqueue context): queue the
 * datagram on the conversation whose connected peer matches src. */
static void anet_rx(const uint8_t src[6], const uint8_t *data, uint16_t len,
		    void *user)
{
	ARG_UNUSED(user);
	if (len > AETHER_RELIABLE_MTU) {
		len = AETHER_RELIABLE_MTU;
	}
	k_mutex_lock(&aconns_lock, K_FOREVER);

	/* Prefer a conversation connected to this peer; otherwise the first
	 * announced (listening) conversation. */
	struct aconn *c = NULL;
	for (int i = 0; i < AETHER_NET_MAX_CONNS; i++) {
		if (aconns[i].in_use && aconns[i].connected &&
		    memcmp(aconns[i].peer, src, 6) == 0) {
			c = &aconns[i];
			break;
		}
	}
	if (!c) {
		for (int i = 0; i < AETHER_NET_MAX_CONNS; i++) {
			if (aconns[i].in_use && aconns[i].announced) {
				c = &aconns[i];
				break;
			}
		}
	}

	if (c && len >= 1) {
		uint8_t hdr = data[0];
		bool start = (hdr & AETHER_NET_FRAG_START) != 0;
		bool more = (hdr & AETHER_NET_FRAG_MORE) != 0;
		const uint8_t *chunk = data + 1;
		uint16_t clen = (uint16_t)(len - 1);

		if (start) {
			/* New message — reset reassembly (discards any partial). */
			c->ra_active = true;
			c->ra_len = 0;
			memcpy(c->ra_src, src, 6);
		} else if (!c->ra_active || memcmp(c->ra_src, src, 6) != 0) {
			/* Mid-message fragment with no start we saw → drop. */
			k_mutex_unlock(&aconns_lock);
			return;
		}

		if (c->ra_len + clen <= AETHER_NET_MAX_MSG) {
			memcpy(c->ra_buf + c->ra_len, chunk, clen);
			c->ra_len += clen;
		} else {
			c->ra_active = false; /* overflow → drop message */
			c->ra_len = 0;
			k_mutex_unlock(&aconns_lock);
			return;
		}

		if (!more) {
			/* Message complete.  An in-kernel consumer (krecv) takes
			 * delivery directly — off-lock, since the callback may
			 * write back into the namespace; ra_buf is stable because
			 * anet_rx is serialized on the mesh workqueue. */
			if (c->krecv) {
				aether_net_krecv_t cb = c->krecv;
				void *u = c->krecv_user;
				uint16_t mlen = c->ra_len;
				c->ra_active = false;
				c->ra_len = 0;
				k_mutex_unlock(&aconns_lock);
				cb(src, c->ra_buf, mlen, u);
				return;
			}
			/* Otherwise buffer for a data-file reader, if there's room. */
			if (c->rx_count < AETHER_NET_RX_RING) {
				int slot = (c->rx_head + c->rx_count) %
					   AETHER_NET_RX_RING;
				c->rx[slot].len = c->ra_len;
				memcpy(c->rx[slot].src, src, 6);
				memcpy(c->rx[slot].data, c->ra_buf, c->ra_len);
				c->rx_count++;
				k_sem_give(&c->rx_sem);
			}
			c->ra_active = false;
			c->ra_len = 0;
		}
	}
	k_mutex_unlock(&aconns_lock);
}

/* RX tap for the party line: fan a best-effort broadcast datagram to every
 * open broadcast conversation's rx ring (source-prefixed on read).  Called
 * from the best-effort mesh recv dispatch for AETHER_NET_DGRAM_PROTO frames.
 * Best-effort: a conversation with a full ring drops the datagram. */
void aether_net_deliver_bcast(const uint8_t src[6], const uint8_t *data,
			      uint16_t len)
{
	if (len > AETHER_NET_MAX_MSG) {
		len = AETHER_NET_MAX_MSG;
	}
	k_mutex_lock(&aconns_lock, K_FOREVER);
	for (int i = 0; i < AETHER_NET_MAX_CONNS; i++) {
		struct aconn *c = &aconns[i];
		if (!c->in_use || !c->bcast ||
		    c->rx_count >= AETHER_NET_RX_RING) {
			continue; /* not a party line, or ring full → drop */
		}
		int slot = (c->rx_head + c->rx_count) % AETHER_NET_RX_RING;
		c->rx[slot].len = len;
		memcpy(c->rx[slot].src, src, 6);
		memcpy(c->rx[slot].data, data, len);
		c->rx_count++;
		k_sem_give(&c->rx_sem);
	}
	k_mutex_unlock(&aconns_lock);
}

static int parse_slot(const char *s)
{
	if (!*s) {
		return -1;
	}
	int n = 0;
	for (const char *p = s; *p; p++) {
		if (*p < '0' || *p > '9') {
			return -1;
		}
		n = n * 10 + (*p - '0');
		if (n >= AETHER_NET_MAX_CONNS) {
			return -1;
		}
	}
	return n;
}

/* ---- 9P ops ---- */

static int anet_attach(struct srv *srv, struct srv_fid *fid, const char *aname)
{
	ARG_UNUSED(srv);
	ARG_UNUSED(aname);
	struct anf_data *d = new_anf(ANF_ROOT, -1);
	if (!d) {
		return -ENOMEM;
	}
	fid->aux = d;
	fid->qid = anf_qid(ANF_ROOT, -1);
	return 0;
}

static int anet_walk(struct srv *srv, struct srv_fid *fid,
		     struct srv_fid *newfid, const char *name)
{
	ARG_UNUSED(srv);
	struct anf_data *old = fid->aux;
	int to_type = -1;
	int to_slot = old->slot;

	if (!name || !*name) {
		struct anf_data *nd = new_anf(old->type, old->slot);
		if (!nd) {
			return -ENOMEM;
		}
		memcpy(nd->addr, old->addr, 6);
		newfid->aux = nd;
		newfid->qid = fid->qid;
		return 0;
	}

	switch (old->type) {
	case ANF_ROOT:
		if (!strcmp(name, "clone")) {
			to_type = ANF_CLONE;
		} else if (!strcmp(name, "status")) {
			to_type = ANF_STATUS;
		} else if (!strcmp(name, "stats")) {
			to_type = ANF_STATS;
		} else if (!strcmp(name, "phy")) {
			to_type = ANF_PHY;
		} else if (!strcmp(name, "addr")) {
			to_type = ANF_ADDR;
		} else if (!strcmp(name, "neighbours")) {
			to_type = ANF_NEIGHBOURS;
#ifdef CONFIG_AETHER_SVCDISC
		} else if (!strcmp(name, "self")) {
			to_type = ANF_SELF_DIR;
		} else if (!strcmp(name, "peers")) {
			to_type = ANF_PEERS_DIR;
#endif
		} else {
			int s = parse_slot(name);
			if (s < 0) {
				return -ENOENT;
			}
			k_mutex_lock(&aconns_lock, K_FOREVER);
			bool live = aconns[s].in_use;
			k_mutex_unlock(&aconns_lock);
			if (!live) {
				return -ENOENT;
			}
			to_type = ANF_CONN_DIR;
			to_slot = s;
		}
		break;

	case ANF_CONN_DIR:
		if (!strcmp(name, "ctl")) {
			to_type = ANF_CTL;
		} else if (!strcmp(name, "data")) {
			to_type = ANF_DATA;
		} else if (!strcmp(name, "local")) {
			to_type = ANF_LOCAL;
		} else if (!strcmp(name, "remote")) {
			to_type = ANF_REMOTE;
		} else if (!strcmp(name, "status")) {
			to_type = ANF_CSTATUS;
		}
		break;

#ifdef CONFIG_AETHER_SVCDISC
	case ANF_SELF_DIR:
		if (!strcmp(name, "name")) {
			to_type = ANF_SELF_NAME;
		} else if (!strcmp(name, "services")) {
			to_type = ANF_SELF_SVCS;
		}
		break;

	case ANF_PEERS_DIR: {
		struct aether_peer p;
		uint8_t addr[6];

		if (parse_mac(name, addr) < 0 ||
		    aether_peer_get_by_addr(addr, &p) < 0) {
			return -ENOENT;
		}
		struct anf_data *nd = new_anf(ANF_PEER_DIR, -1);
		if (!nd) {
			return -ENOMEM;
		}
		memcpy(nd->addr, addr, 6);
		newfid->aux = nd;
		newfid->qid = anf_peer_qid(ANF_PEER_DIR, addr);
		return 0;
	}

	case ANF_PEER_DIR: {
		if (!strcmp(name, "name")) {
			to_type = ANF_PEER_NAME;
		} else if (!strcmp(name, "services")) {
			to_type = ANF_PEER_SVCS;
		} else if (!strcmp(name, "lastseen")) {
			to_type = ANF_PEER_LASTSEEN;
		} else if (!strcmp(name, "rssi")) {
			to_type = ANF_PEER_RSSI;
		}
		if (to_type < 0) {
			return -ENOENT;
		}
		struct anf_data *nd = new_anf(to_type, -1);
		if (!nd) {
			return -ENOMEM;
		}
		memcpy(nd->addr, old->addr, 6);
		newfid->aux = nd;
		newfid->qid = anf_peer_qid(to_type, old->addr);
		return 0;
	}
#endif

	default:
		return -ENOTDIR;
	}

	if (to_type < 0) {
		return -ENOENT;
	}
	struct anf_data *nd = new_anf(to_type, to_slot);
	if (!nd) {
		return -ENOMEM;
	}
	newfid->aux = nd;
	newfid->qid = anf_qid(to_type, to_slot);
	return 0;
}

static int anet_open(struct srv *srv, struct srv_fid *fid, uint8_t mode)
{
	ARG_UNUSED(srv);
	ARG_UNUSED(mode);
	struct anf_data *d = fid->aux;

	if (d->type == ANF_CLONE) {
		int s = alloc_aconn();
		if (s < 0) {
			return s;
		}
		d->type = ANF_CTL; /* this fid is now the new conversation's ctl */
		d->slot = s;
		fid->qid = anf_qid(ANF_CTL, s);
	}
	return 0;
}

static int anet_clunk(struct srv *srv, struct srv_fid *fid)
{
	ARG_UNUSED(srv);
	/* Slots persist after the clone read closes so scripts can use the
	 * Plan 9 idiom: n=`{cat /net/aether/clone}; ... ; echo hangup >ctl.
	 * A conversation is freed by the `hangup` ctl verb (idle GC is a
	 * follow-up backstop against a script that forgets). */
	if (fid->aux) {
		mem_free(fid->aux);
		fid->aux = NULL;
	}
	return 0;
}

static int anet_read(struct srv *srv, struct srv_fid *fid, uint64_t off,
		     uint8_t *buf, uint32_t cnt)
{
	ARG_UNUSED(srv);
	struct anf_data *d = fid->aux;

	switch (d->type) {
	case ANF_ROOT: {
		struct dirent_spec base[] = {
			{"clone",  ANF_CLONE,  0666, -1},
			{"status", ANF_STATUS, 0444, -1},
			{"stats",  ANF_STATS,  0444, -1},
			{"phy",    ANF_PHY,    0666, -1},
			{"addr",   ANF_ADDR,   0444, -1},
			{"neighbours", ANF_NEIGHBOURS, 0444, -1},
#ifdef CONFIG_AETHER_SVCDISC
			{"self",  ANF_SELF_DIR,  NINEP_DMDIR | 0555, -1},
			{"peers", ANF_PEERS_DIR, NINEP_DMDIR | 0555, -1},
#endif
		};
		struct dirent_spec ents[ARRAY_SIZE(base) + AETHER_NET_MAX_CONNS];
		static char names[AETHER_NET_MAX_CONNS][4];
		int n = 0;
		for (; n < (int)ARRAY_SIZE(base); n++) {
			ents[n] = base[n];
		}
		k_mutex_lock(&aconns_lock, K_FOREVER);
		for (int i = 0; i < AETHER_NET_MAX_CONNS; i++) {
			if (!aconns[i].in_use) {
				continue;
			}
			snprintf(names[i], sizeof(names[i]), "%d", i);
			ents[n].name = names[i];
			ents[n].type = ANF_CONN_DIR;
			ents[n].mode = NINEP_DMDIR | 0555;
			ents[n].slot = i;
			n++;
		}
		k_mutex_unlock(&aconns_lock);
		return emit_dir(ents, n, off, buf, cnt);
	}

	case ANF_STATUS: {
		int active = 0;
		k_mutex_lock(&aconns_lock, K_FOREVER);
		for (int i = 0; i < AETHER_NET_MAX_CONNS; i++) {
			active += aconns[i].in_use ? 1 : 0;
		}
		k_mutex_unlock(&aconns_lock);
		struct net_if *iface = aether_mesh_iface();
		int nb = iface ? aether_mesh_get_neighbor_count(iface) : 0;
		return emit_fmt(off, buf, cnt,
				"aether: %d/%d conversations, %d neighbours\n",
				active, AETHER_NET_MAX_CONNS, nb);
	}

	case ANF_STATS: {
		struct aether_lora_stats st;
		aether_lora_get_stats(&st);
		return emit_fmt(off, buf, cnt,
				"rx %u tx %u rssi %d snr %d\n",
				st.rx_count, st.tx_count,
				(int)st.last_rssi, (int)st.last_snr);
	}

	case ANF_PHY: {
		struct aether_lora_phy phy;
		static const uint16_t bw_khz[] = {125, 250, 500};
		aether_lora_get_phy(&phy);
		/* Human-readable + write-symmetric: bw in kHz, cr as the 4/X
		 * denominator (enum 1->5 .. 4->8). */
		return emit_fmt(off, buf, cnt,
				"sf %u bw %u cr %u cad %s pwr %d\n",
				phy.sf,
				phy.bw < ARRAY_SIZE(bw_khz) ? bw_khz[phy.bw] : 0,
				phy.cr + 4, phy.cad ? "on" : "off",
				(int)phy.tx_power);
	}

	case ANF_ADDR:
	case ANF_LOCAL:
		return emit_mac(aether_mesh_local_addr(), off, buf, cnt);

	case ANF_NEIGHBOURS: {
		/* Roster of nodes the PHY can see (mesh neighbour table), one
		 * per line: addr rssi lqi age.  Modem-agnostic: a mounted
		 * gateway's /net/aether/neighbours reads the gateway's peers. */
		char tmp[256];
		int len = 0;
		tmp[0] = '\0';
		if (g_mesh_ctx) {
			for (int i = 0; i < CONFIG_AETHER_MAX_NEIGHBORS; i++) {
				const struct aether_neighbor *nb =
					&g_mesh_ctx->neighbors[i];
				if (!nb->active) {
					continue;
				}
				uint32_t age = (k_uptime_get_32() -
						nb->last_seen) / 1000;
				int n = snprintf(tmp + len, sizeof(tmp) - len,
					"%02x:%02x:%02x:%02x:%02x:%02x rssi %d lqi %u age %us\n",
					nb->addr[0], nb->addr[1], nb->addr[2],
					nb->addr[3], nb->addr[4], nb->addr[5],
					nb->rssi, nb->lqi, age);
				if (n < 0 || len + n >= (int)sizeof(tmp)) {
					break;
				}
				len += n;
			}
		}
		return emit_string(tmp, off, buf, cnt);
	}

	case ANF_CONN_DIR: {
		struct dirent_spec ents[] = {
			{"ctl",    ANF_CTL,     0666, d->slot},
			{"data",   ANF_DATA,    0666, d->slot},
			{"local",  ANF_LOCAL,   0444, d->slot},
			{"remote", ANF_REMOTE,  0444, d->slot},
			{"status", ANF_CSTATUS, 0444, d->slot},
		};
		return emit_dir(ents, 5, off, buf, cnt);
	}

#ifdef CONFIG_AETHER_SVCDISC
	case ANF_SELF_DIR: {
		struct dirent_spec ents[] = {
			{"name",     ANF_SELF_NAME, 0666, -1},
			{"services", ANF_SELF_SVCS, 0444, -1},
		};
		return emit_dir(ents, 2, off, buf, cnt);
	}

	case ANF_PEERS_DIR: {
		/* One dir per known peer, named by address (the stable key —
		 * a name collision can never alias two peers into one dir). */
		size_t stream_pos = 0, buf_pos = 0;
		struct aether_peer p;
		uint8_t tmp[96];
		char nm[18];

		for (size_t i = 0; aether_peer_get_at(i, &p) == 0; i++) {
			size_t tpos = 0;
			struct ninep_qid qid = anf_peer_qid(ANF_PEER_DIR,
							    p.addr);
			snprintf(nm, sizeof(nm),
				 "%02x:%02x:%02x:%02x:%02x:%02x",
				 p.addr[0], p.addr[1], p.addr[2],
				 p.addr[3], p.addr[4], p.addr[5]);
			if (srv_dir_add_entry(tmp, sizeof(tmp), &tpos, nm,
					      &qid, NINEP_DMDIR | 0555,
					      0) < 0) {
				break;
			}
			if (stream_pos >= off) {
				if (buf_pos + tpos > cnt) {
					break;
				}
				memcpy(buf + buf_pos, tmp, tpos);
				buf_pos += tpos;
			}
			stream_pos += tpos;
		}
		return (int)buf_pos;
	}

	case ANF_PEER_DIR: {
		struct dirent_spec pents[] = {
			{"name",     ANF_PEER_NAME,     0444, -1},
			{"services", ANF_PEER_SVCS,     0444, -1},
			{"lastseen", ANF_PEER_LASTSEEN, 0444, -1},
			{"rssi",     ANF_PEER_RSSI,     0444, -1},
		};
		size_t stream_pos = 0, buf_pos = 0;
		uint8_t tmp[96];

		for (int i = 0; i < (int)ARRAY_SIZE(pents); i++) {
			size_t tpos = 0;
			struct ninep_qid qid = anf_peer_qid(pents[i].type,
							    d->addr);
			if (srv_dir_add_entry(tmp, sizeof(tmp), &tpos,
					      pents[i].name, &qid,
					      pents[i].mode, 0) < 0) {
				break;
			}
			if (stream_pos >= off) {
				if (buf_pos + tpos > cnt) {
					break;
				}
				memcpy(buf + buf_pos, tmp, tpos);
				buf_pos += tpos;
			}
			stream_pos += tpos;
		}
		return (int)buf_pos;
	}

	case ANF_SELF_NAME: {
		struct aether_peer self;
		aether_svcdisc_self(&self);
		return emit_fmt(off, buf, cnt, "%s\n", self.name);
	}

	case ANF_SELF_SVCS: {
		struct aether_peer self;
		aether_svcdisc_self(&self);
		char tmp[AETHER_SVC_MAX * (AETHER_SVC_NAME_MAX + 2)];
		int len = 0;
		tmp[0] = '\0';
		for (int i = 0; i < self.nsvc; i++) {
			len += snprintf(tmp + len, sizeof(tmp) - len, "%s\n",
					self.svc[i].name);
		}
		return emit_string(tmp, off, buf, cnt);
	}

	case ANF_PEER_NAME:
	case ANF_PEER_SVCS:
	case ANF_PEER_LASTSEEN:
	case ANF_PEER_RSSI: {
		struct aether_peer p;

		if (aether_peer_get_by_addr(d->addr, &p) < 0) {
			return -ENOENT;   /* aged out under the fid */
		}
		switch (d->type) {
		case ANF_PEER_NAME:
			return emit_fmt(off, buf, cnt, "%s\n", p.name);
		case ANF_PEER_LASTSEEN:
			return emit_fmt(off, buf, cnt, "%u\n",
					(k_uptime_get_32() - p.lastseen) /
						1000U);
		case ANF_PEER_RSSI:
			return emit_fmt(off, buf, cnt, "%d\n", (int)p.rssi);
		default: {
			char tmp[AETHER_SVC_MAX * (AETHER_SVC_NAME_MAX + 2)];
			int len = 0;
			tmp[0] = '\0';
			for (int i = 0; i < p.nsvc; i++) {
				len += snprintf(tmp + len, sizeof(tmp) - len,
						"%s\n", p.svc[i].name);
			}
			return emit_string(tmp, off, buf, cnt);
		}
		}
	}
#endif /* CONFIG_AETHER_SVCDISC */

	case ANF_CTL:
		/* Plan 9 contract: reading ctl yields the slot number. */
		if (d->slot < 0) {
			return -EINVAL;
		}
		return emit_fmt(off, buf, cnt, "%d\n", d->slot);

	case ANF_REMOTE: {
		if (d->slot < 0) {
			return -EINVAL;
		}
		k_mutex_lock(&aconns_lock, K_FOREVER);
		bool conn = aconns[d->slot].connected;
		uint8_t peer[6];
		memcpy(peer, aconns[d->slot].peer, 6);
		k_mutex_unlock(&aconns_lock);
		return emit_mac(conn ? peer : NULL, off, buf, cnt);
	}

	case ANF_CSTATUS: {
		if (d->slot < 0) {
			return -EINVAL;
		}
		k_mutex_lock(&aconns_lock, K_FOREVER);
		bool conn = aconns[d->slot].connected;
		bool ann = aconns[d->slot].announced;
		bool bcast = aconns[d->slot].bcast;
		uint8_t peer[6];
		memcpy(peer, aconns[d->slot].peer, 6);
		k_mutex_unlock(&aconns_lock);
		if (bcast) {
			return emit_string("broadcast best-effort\n", off, buf, cnt);
		}
		if (conn) {
			return emit_fmt(off, buf, cnt,
				"connected aether!%02x:%02x:%02x:%02x:%02x:%02x reliable\n",
				peer[0], peer[1], peer[2], peer[3], peer[4], peer[5]);
		}
		if (ann) {
			return emit_string("announced\n", off, buf, cnt);
		}
		return emit_string("unconnected\n", off, buf, cnt);
	}

	case ANF_DATA: {
		/* One datagram per read (datagram semantics; offset ignored).
		 * Blocks until a datagram arrives or the conversation hangs up. */
		if (d->slot < 0) {
			return -EINVAL;
		}
		struct aconn *c = &aconns[d->slot];
		k_sem_take(&c->rx_sem, K_FOREVER);
		k_mutex_lock(&aconns_lock, K_FOREVER);
		if (c->rx_count == 0) { /* woken by hangup */
			k_mutex_unlock(&aconns_lock);
			return 0;
		}
		struct arx_msg *m = &c->rx[c->rx_head];
		uint32_t n = 0;
		/* Announced (listening) and broadcast (party-line) conversations
		 * prefix each datagram with the 6-byte source address so the
		 * reader knows who sent it. */
		if ((c->announced || c->bcast) && cnt >= 6) {
			memcpy(buf, m->src, 6);
			n = 6;
		}
		uint32_t plen = m->len;
		if (plen > cnt - n) {
			plen = cnt - n; /* truncate payload to caller's buffer */
		}
		memcpy(buf + n, m->data, plen);
		n += plen;
		c->rx_head = (c->rx_head + 1) % AETHER_NET_RX_RING;
		c->rx_count--;
		k_mutex_unlock(&aconns_lock);
		return (int)n;
	}

	default:
		return -EINVAL;
	}
}

static int anet_write(struct srv *srv, struct srv_fid *fid, uint64_t off,
		      const uint8_t *buf, uint32_t cnt)
{
	ARG_UNUSED(srv);
	ARG_UNUSED(off);
	struct anf_data *d = fid->aux;

	switch (d->type) {
	case ANF_CTL: {
		if (d->slot < 0) {
			return -EINVAL;
		}
		char verb[16], rest[64];
		parse_verb((const char *)buf, cnt, verb, sizeof(verb),
			   rest, sizeof(rest));

		if (!strcmp(verb, "connect")) {
			uint8_t peer[6];
			int perr = parse_peer(rest, peer);
			if (perr < 0) {
				return perr;
			}
			/* connect to the all-ones broadcast address = join the
			 * best-effort party line (connectionless): writes go out
			 * best-effort to the mesh, reads receive any peer's
			 * broadcast datagram, source-prefixed. */
			static const uint8_t bcast_addr[6] = {
				0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
			bool is_bc = memcmp(peer, bcast_addr, 6) == 0;
			k_mutex_lock(&aconns_lock, K_FOREVER);
			memcpy(aconns[d->slot].peer, peer, 6);
			aconns[d->slot].connected = true;
			aconns[d->slot].bcast = is_bc;
			k_mutex_unlock(&aconns_lock);
			return cnt;
		}
		if (!strcmp(verb, "announce")) {
			/* Listen for reliable datagrams from any peer; reads
			 * are source-prefixed (headers mode). */
			k_mutex_lock(&aconns_lock, K_FOREVER);
			aconns[d->slot].announced = true;
			k_mutex_unlock(&aconns_lock);
			aether_net_readvertise();
			return cnt;
		}
		if (!strcmp(verb, "hangup")) {
			free_aconn(d->slot);
			return cnt;
		}
		return -EINVAL;
	}

	case ANF_PHY: {
		/* One "key value" per write (ctl idiom): cad {on,off}, sf 7-12,
		 * bw {125,250,500} kHz, cr {5,6,7,8} (=4/X), pwr <dBm>.  Both decks
		 * must match the SF/BW/CR for the link to work. */
		char verb[16], rest[32];
		struct aether_lora_phy phy;
		int ret;

		parse_verb((const char *)buf, cnt, verb, sizeof(verb),
			   rest, sizeof(rest));
		aether_lora_get_phy(&phy);

		if (!strcmp(verb, "cad")) {
			phy.cad = !strcmp(rest, "on");
		} else if (!strcmp(verb, "sf")) {
			phy.sf = (uint8_t)atoi(rest);
		} else if (!strcmp(verb, "bw")) {
			int k = atoi(rest);
			phy.bw = (k >= 500) ? 2 : (k >= 250) ? 1 : 0;
		} else if (!strcmp(verb, "cr")) {
			int c = atoi(rest);
			if (c < 5 || c > 8) {
				return -EINVAL;
			}
			phy.cr = (uint8_t)(c - 4);
		} else if (!strcmp(verb, "pwr")) {
			phy.tx_power = (int8_t)atoi(rest);
		} else {
			return -EINVAL;
		}

		ret = aether_lora_set_phy(&phy);
		return ret < 0 ? ret : (int)cnt;
	}

#ifdef CONFIG_AETHER_SVCDISC
	case ANF_SELF_NAME: {
		/* Beacon name: [a-z0-9-], <= 15 chars; empty clears. */
		char nm[AETHER_SVC_NAME_MAX + 2];
		uint32_t n = MIN(cnt, sizeof(nm) - 1);

		memcpy(nm, buf, n);
		nm[n] = '\0';
		char *nl = strchr(nm, '\n');
		if (nl) {
			*nl = '\0';
		}
		int ret = aether_svcdisc_set_name(nm);
		return ret < 0 ? ret : (int)cnt;
	}
#endif

	case ANF_DATA: {
		if (d->slot < 0) {
			return -EINVAL;
		}
		if (cnt == 0) {
			return 0;
		}
		k_mutex_lock(&aconns_lock, K_FOREVER);
		bool conn = aconns[d->slot].connected;
		bool ann  = aconns[d->slot].announced;
		bool bcast = aconns[d->slot].bcast;
		uint8_t peer[6];
		memcpy(peer, aconns[d->slot].peer, 6);
		k_mutex_unlock(&aconns_lock);

		/* Broadcast party line: one proto-tagged best-effort frame to the
		 * mesh broadcast address — no ARQ, no fragmentation (the lobby
		 * carries small messages; oversize is rejected rather than split,
		 * since best-effort reassembly can't be made reliable anyway).
		 * Peers receive it via the recv dispatch → aether_net_deliver_bcast. */
		if (bcast) {
			if (cnt > CONFIG_AETHER_MAX_PAYLOAD - 1) {
				return -EMSGSIZE;
			}
			struct net_if *iface = aether_mesh_iface();
			if (!iface) {
				return -ENETDOWN;
			}
			static const uint8_t bcast_addr[6] = {
				0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
			uint8_t frame[CONFIG_AETHER_MAX_PAYLOAD];
			frame[0] = AETHER_NET_DGRAM_PROTO;
			memcpy(frame + 1, buf, cnt);
			int ret = aether_mesh_send_reliable(iface, bcast_addr,
							    frame, cnt + 1, 2);
			return ret < 0 ? ret : (int)cnt;
		}

		/* Destination + payload depend on the conversation class:
		 *  - connected: payload is the whole write; peer is the bound peer.
		 *  - announced: write is [6-byte dst][payload] — reply to a
		 *    requester (the /net/udp datagram-server idiom, NET_AETHER_SPEC
		 *    §4/§8; symmetric with the source-prefixed announced read).
		 * This is what carries a 9P server's R-messages back to a mounter. */
		const uint8_t *payload = buf;
		uint32_t plen = cnt;
		if (conn) {
			/* peer already loaded; payload = whole write */
		} else if (ann) {
			if (cnt < 6) {
				return -EINVAL;
			}
			memcpy(peer, buf, 6);
			payload = buf + 6;
			plen = cnt - 6;
		} else {
			return -ENOTCONN;
		}
		if (plen > AETHER_NET_MAX_MSG) {
			return -EMSGSIZE; /* exceeds the reassembly ceiling */
		}
		struct net_if *iface = aether_mesh_iface();
		if (!iface) {
			return -ENETDOWN;
		}

		/* Fragment the payload into engine sends: each carries a 1-byte
		 * header [START?|MORE?] followed by up to MTU-1 payload bytes.
		 * The receiver reassembles (anet_rx). One write == one message. */
		const uint32_t chunk_max = AETHER_RELIABLE_MTU - 1;
		uint8_t frag[AETHER_RELIABLE_MTU];
		uint32_t sent = 0;

		do {
			uint32_t clen = plen - sent;
			if (clen > chunk_max) {
				clen = chunk_max;
			}
			bool more = (sent + clen) < plen;
			frag[0] = (sent == 0 ? AETHER_NET_FRAG_START : 0) |
				  (more ? AETHER_NET_FRAG_MORE : 0);
			memcpy(frag + 1, payload + sent, clen);

			int ret, tries = 0;
			while ((ret = aether_mesh_reliable_send(
					iface, peer, frag, clen + 1)) == -EAGAIN) {
				/* window full — let the engine drain (it acks on
				 * the tick) then retry; ~2 s ceiling. */
				if (++tries > 100) {
					break;
				}
				k_msleep(20);
			}
			if (ret < 0) {
				return ret; /* datagram: all-or-nothing */
			}
			sent += clen;
		} while (sent < plen);

		return (int)cnt; /* all bytes consumed (incl. any dst prefix) */
	}

	default:
		return -EPERM;
	}
}

static int anet_stat(struct srv *srv, struct srv_fid *fid,
		     struct ninep_stat *stat)
{
	ARG_UNUSED(srv);
	struct anf_data *d = fid->aux;
	const char *name = "aether";
	uint32_t mode = NINEP_DMDIR | 0555;

	memset(stat, 0, sizeof(*stat));
	stat->qid = fid->qid;

	switch (d->type) {
	case ANF_ROOT:     name = "aether"; mode = NINEP_DMDIR | 0555; break;
	case ANF_CLONE:    name = "clone";  mode = 0666; break;
	case ANF_STATUS:   name = "status"; mode = 0444; break;
	case ANF_STATS:    name = "stats";  mode = 0444; break;
	case ANF_PHY:      name = "phy";    mode = 0666; break;
	case ANF_ADDR:     name = "addr";   mode = 0444; break;
	case ANF_NEIGHBOURS: name = "neighbours"; mode = 0444; break;
	case ANF_CONN_DIR: {
		static char dbuf[4];
		snprintf(dbuf, sizeof(dbuf), "%d", d->slot);
		name = dbuf;
		mode = NINEP_DMDIR | 0555;
		break;
	}
	case ANF_CTL:      name = "ctl";    mode = 0666; break;
	case ANF_DATA:     name = "data";   mode = 0666; break;
	case ANF_LOCAL:    name = "local";  mode = 0444; break;
	case ANF_REMOTE:   name = "remote"; mode = 0444; break;
	case ANF_CSTATUS:  name = "status"; mode = 0444; break;
#ifdef CONFIG_AETHER_SVCDISC
	case ANF_SELF_DIR:  name = "self";  mode = NINEP_DMDIR | 0555; break;
	case ANF_PEERS_DIR: name = "peers"; mode = NINEP_DMDIR | 0555; break;
	case ANF_SELF_NAME: name = "name";  mode = 0666; break;
	case ANF_SELF_SVCS: name = "services"; mode = 0444; break;
	case ANF_PEER_DIR: {
		static char abuf[18];
		snprintf(abuf, sizeof(abuf), "%02x:%02x:%02x:%02x:%02x:%02x",
			 d->addr[0], d->addr[1], d->addr[2],
			 d->addr[3], d->addr[4], d->addr[5]);
		name = abuf;
		mode = NINEP_DMDIR | 0555;
		break;
	}
	case ANF_PEER_NAME:     name = "name";     mode = 0444; break;
	case ANF_PEER_SVCS:    name = "services"; mode = 0444; break;
	case ANF_PEER_LASTSEEN: name = "lastseen"; mode = 0444; break;
	case ANF_PEER_RSSI:     name = "rssi";     mode = 0444; break;
#endif
	}

	stat->mode = mode;
	stat->name = (char *)name;
	stat->uid = "frst";
	stat->gid = "frst";
	stat->muid = "frst";
	return 0;
}

static const struct srv_ops anet_ops = {
	.attach = anet_attach,
	.walk   = anet_walk,
	.open   = anet_open,
	.read   = anet_read,
	.write  = anet_write,
	.clunk  = anet_clunk,
	.stat   = anet_stat,
};

static struct srv anet_srv = {
	.name   = "aether",
	.prefix = "/net/aether",
	.ops    = &anet_ops,
	.priv   = NULL,
};

int dev_aether_net_init(void)
{
	k_mutex_init(&aconns_lock);
	int ret = srv_register(&anet_srv);
	if (ret) {
		LOG_ERR("failed to register /net/aether: %d", ret);
		return ret;
	}
	/* Receive reliable datagrams from the §4.6 engine. */
	aether_reliable_set_recv_cb(anet_rx, NULL);
	LOG_INF("Æther network server at /net/aether");
	return 0;
}

#else /* !CONFIG_AETHER_MESH */

int dev_aether_net_init(void)
{
	return 0;
}

#endif /* CONFIG_AETHER_MESH */
