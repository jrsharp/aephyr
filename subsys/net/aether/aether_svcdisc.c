/*
 * Æther service discovery — beacon TLV codec + known-peer table.
 *
 * Copyright (c) 2026 Æther Authors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <string.h>
#include <errno.h>

#include <zephyr/net/aether_svcdisc.h>

#ifdef CONFIG_SHARED_MULTI_HEAP
#include <zephyr/multi_heap/shared_multi_heap.h>
#endif

LOG_MODULE_REGISTER(aether_svcdisc, CONFIG_AETHER_MESH_LOG_LEVEL);

/* All discovery state is plain data in one block, allocated from PSRAM
 * when available (DRAM heap otherwise) — the deck's DRAM is nearly full.
 * The lock is a kernel object and stays here in DRAM.  Copy-in/copy-out
 * under the lock, which is never held across anything slower than a
 * memcpy. */
struct disc_state {
	struct aether_peer peers[AETHER_SVCDISC_MAX_PEERS];
	/* Self advertisement (name/svc/nsvc fields only). */
	struct aether_peer self;
	/* HELLO-payload linearize scratch; single writer (mesh RX is
	 * serialized on the mesh workq). */
	uint8_t rx_scratch[AETHER_SVCADV_MAX];
};

static struct disc_state *st;
static K_MUTEX_DEFINE(disc_lock);

/* Name set before init (boot settings load runs before the radio/mesh
 * bringup thread finishes) parks here and is applied by init.  Small
 * enough to live in DRAM. */
static char pre_name[AETHER_SVC_NAME_MAX + 1];

bool aether_svc_name_ok(const char *name)
{
	size_t len;

	if (!name || !name[0]) {
		return false;
	}
	len = strnlen(name, AETHER_SVC_NAME_MAX + 1);
	if (len > AETHER_SVC_NAME_MAX) {
		return false;
	}
	/* Path-component / dial-atom safe: [a-z0-9-], first alphanumeric. */
	for (size_t i = 0; i < len; i++) {
		char c = name[i];

		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
			continue;
		}
		if (c == '-' && i > 0) {
			continue;
		}
		return false;
	}
	return true;
}

int aether_svcadv_encode(uint8_t *buf, size_t cap, const char *name,
			 const struct aether_svc *svc, uint8_t nsvc)
{
	size_t off = 0;
	size_t namelen = (name != NULL) ? strlen(name) : 0;

	if (nsvc > AETHER_SVC_MAX || (nsvc > 0 && svc == NULL)) {
		return -EINVAL;
	}
	if (namelen > 0 && !aether_svc_name_ok(name)) {
		return -EINVAL;
	}
	if (cap < 1 + namelen + 1) {
		return -EMSGSIZE;
	}

	buf[off++] = (uint8_t)namelen;
	memcpy(&buf[off], name, namelen);
	off += namelen;
	buf[off++] = nsvc;

	for (uint8_t i = 0; i < nsvc; i++) {
		size_t sl;

		if (!aether_svc_name_ok(svc[i].name)) {
			return -EINVAL;
		}
		sl = strlen(svc[i].name);
		if (off + 1 + sl > cap) {
			return -EMSGSIZE;
		}
		buf[off++] = (uint8_t)sl;
		memcpy(&buf[off], svc[i].name, sl);
		off += sl;
	}
	return (int)off;
}

int aether_svcadv_decode(const uint8_t *buf, size_t len,
			 struct aether_peer *peer)
{
	size_t off = 0;
	uint8_t namelen, nsvc;

	if (!buf || !peer || len < 2) {
		return -EBADMSG;
	}

	namelen = buf[off++];
	if (namelen > AETHER_SVC_NAME_MAX || off + namelen + 1 > len) {
		return -EBADMSG;
	}
	memcpy(peer->name, &buf[off], namelen);
	peer->name[namelen] = '\0';
	off += namelen;
	if (namelen > 0 && !aether_svc_name_ok(peer->name)) {
		return -EBADMSG;
	}

	nsvc = buf[off++];
	if (nsvc > AETHER_SVC_MAX) {
		return -EBADMSG;
	}
	for (uint8_t i = 0; i < nsvc; i++) {
		uint8_t sl;

		if (off + 1 > len) {
			return -EBADMSG;
		}
		sl = buf[off++];
		if (sl == 0 || sl > AETHER_SVC_NAME_MAX || off + sl > len) {
			return -EBADMSG;
		}
		memcpy(peer->svc[i].name, &buf[off], sl);
		peer->svc[i].name[sl] = '\0';
		off += sl;
		if (!aether_svc_name_ok(peer->svc[i].name)) {
			return -EBADMSG;
		}
	}
	if (off != len) {
		return -EBADMSG;   /* trailing bytes: reject, don't repair */
	}

	peer->nsvc = nsvc;
	return 0;
}

/* ---- peer table ---- */

int aether_svcdisc_init(void)
{
	if (st != NULL) {
		return 0;
	}
#ifdef CONFIG_SHARED_MULTI_HEAP
	st = shared_multi_heap_alloc(SMH_REG_ATTR_EXTERNAL, sizeof(*st));
#endif
	if (st == NULL) {
		st = k_malloc(sizeof(*st));
	}
	if (st == NULL) {
		LOG_ERR("discovery state alloc failed");
		return -ENOMEM;
	}
	memset(st, 0, sizeof(*st));
	k_mutex_lock(&disc_lock, K_FOREVER);
	memcpy(st->self.name, pre_name, sizeof(st->self.name));
	k_mutex_unlock(&disc_lock);
	return 0;
}

void aether_peer_observe(const struct aether_peer *seen)
{
	struct aether_peer *slot = NULL;
	struct aether_peer *oldest = NULL;
	uint32_t now = seen->lastseen;

	if (st == NULL) {
		return;
	}

	k_mutex_lock(&disc_lock, K_FOREVER);
	for (int i = 0; i < AETHER_SVCDISC_MAX_PEERS; i++) {
		struct aether_peer *p = &st->peers[i];

		if (p->used && memcmp(p->addr, seen->addr, 6) == 0) {
			slot = p;
			break;
		}
		if (!p->used) {
			if (slot == NULL) {
				slot = p;
			}
			continue;
		}
		if (oldest == NULL ||
		    (int32_t)(oldest->lastseen - p->lastseen) > 0) {
			oldest = p;
		}
	}
	if (slot == NULL) {
		/* Full and unknown: evict the oldest, unless it was heard
		 * inside the guard window — then drop the newcomer. */
		if (oldest == NULL ||
		    now - oldest->lastseen < AETHER_SVCDISC_EVICT_GUARD_MS) {
			k_mutex_unlock(&disc_lock);
			return;
		}
		slot = oldest;
	}
	*slot = *seen;
	slot->used = 1;
	k_mutex_unlock(&disc_lock);
}

void aether_peer_age(uint32_t now, uint32_t max_age_ms)
{
	if (st == NULL) {
		return;
	}
	k_mutex_lock(&disc_lock, K_FOREVER);
	for (int i = 0; i < AETHER_SVCDISC_MAX_PEERS; i++) {
		if (st->peers[i].used &&
		    now - st->peers[i].lastseen > max_age_ms) {
			memset(&st->peers[i], 0, sizeof(st->peers[i]));
		}
	}
	k_mutex_unlock(&disc_lock);
}

int aether_peer_get_by_addr(const uint8_t addr[6], struct aether_peer *out)
{
	int ret = -ENOENT;

	if (st == NULL) {
		return -ENOENT;
	}
	k_mutex_lock(&disc_lock, K_FOREVER);
	for (int i = 0; i < AETHER_SVCDISC_MAX_PEERS; i++) {
		if (st->peers[i].used &&
		    memcmp(st->peers[i].addr, addr, 6) == 0) {
			*out = st->peers[i];
			ret = 0;
			break;
		}
	}
	k_mutex_unlock(&disc_lock);
	return ret;
}

int aether_peer_get_by_name(const char *name, struct aether_peer *out)
{
	int hits = 0;

	if (st == NULL || !name || !name[0]) {
		return 0;
	}
	k_mutex_lock(&disc_lock, K_FOREVER);
	for (int i = 0; i < AETHER_SVCDISC_MAX_PEERS; i++) {
		if (st->peers[i].used &&
		    strcmp(st->peers[i].name, name) == 0) {
			if (hits++ == 0) {
				*out = st->peers[i];
			}
		}
	}
	k_mutex_unlock(&disc_lock);
	return hits;
}

int aether_peer_get_at(size_t idx, struct aether_peer *out)
{
	size_t n = 0;
	int ret = -ENOENT;

	if (st == NULL) {
		return -ENOENT;
	}
	k_mutex_lock(&disc_lock, K_FOREVER);
	for (int i = 0; i < AETHER_SVCDISC_MAX_PEERS; i++) {
		if (st->peers[i].used && n++ == idx) {
			*out = st->peers[i];
			ret = 0;
			break;
		}
	}
	k_mutex_unlock(&disc_lock);
	return ret;
}

/* ---- self ---- */

int aether_svcdisc_set_name(const char *name)
{
	if (name && name[0] && !aether_svc_name_ok(name)) {
		return -EINVAL;
	}
	k_mutex_lock(&disc_lock, K_FOREVER);
	/* Before init the name parks in pre_name (applied by init); after,
	 * it goes straight into the live state. */
	char *dst = st ? st->self.name : pre_name;

	if (name == NULL) {
		dst[0] = '\0';
	} else {
		strncpy(dst, name, AETHER_SVC_NAME_MAX);
		dst[AETHER_SVC_NAME_MAX] = '\0';
	}
	k_mutex_unlock(&disc_lock);
	return 0;
}

int aether_svcdisc_set_services(const struct aether_svc *svc, uint8_t nsvc)
{
	if (st == NULL) {
		return -ENODEV;
	}
	if (nsvc > AETHER_SVC_MAX || (nsvc > 0 && svc == NULL)) {
		return -EINVAL;
	}
	for (uint8_t i = 0; i < nsvc; i++) {
		if (!aether_svc_name_ok(svc[i].name)) {
			return -EINVAL;
		}
	}
	k_mutex_lock(&disc_lock, K_FOREVER);
	memset(st->self.svc, 0, sizeof(st->self.svc));
	for (uint8_t i = 0; i < nsvc; i++) {
		st->self.svc[i] = svc[i];
	}
	st->self.nsvc = nsvc;
	k_mutex_unlock(&disc_lock);
	return 0;
}

void aether_svcdisc_self(struct aether_peer *out)
{
	memset(out, 0, sizeof(*out));
	if (st == NULL) {
		return;
	}
	k_mutex_lock(&disc_lock, K_FOREVER);
	memcpy(out->name, st->self.name, sizeof(out->name));
	memcpy(out->svc, st->self.svc, sizeof(out->svc));
	out->nsvc = st->self.nsvc;
	k_mutex_unlock(&disc_lock);
}

int aether_svcdisc_build_adv(uint8_t *buf, size_t cap)
{
	struct aether_peer self;

	aether_svcdisc_self(&self);
	if (self.name[0] == '\0' && self.nsvc == 0) {
		return 0;   /* nothing to say: legacy bare HELLO */
	}
	return aether_svcadv_encode(buf, cap, self.name, self.svc, self.nsvc);
}

void aether_svcdisc_ingest(const uint8_t src[6], int8_t rssi,
			   const uint8_t *payload, size_t len, uint32_t now)
{
	struct aether_peer seen;

	memset(&seen, 0, sizeof(seen));
	if (len > 0 && aether_svcadv_decode(payload, len, &seen) != 0) {
		LOG_DBG("malformed svcadv from %02x%02x dropped",
			src[4], src[5]);
		return;
	}
	memcpy(seen.addr, src, 6);
	seen.rssi = rssi;
	seen.lastseen = now;
	aether_peer_observe(&seen);
}

void aether_svcdisc_ingest_frags(const uint8_t src[6], int8_t rssi,
				 struct net_buf *frags, size_t offset,
				 size_t len, uint32_t now)
{
	if (st == NULL) {
		return;
	}
	if (len > sizeof(st->rx_scratch)) {
		len = 0;   /* oversized advertisement: observe bare peer */
	} else if (len > 0) {
		len = net_buf_linearize(st->rx_scratch, sizeof(st->rx_scratch),
					frags, offset, len);
	}
	aether_svcdisc_ingest(src, rssi, st->rx_scratch, len, now);
}
