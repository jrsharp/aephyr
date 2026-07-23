/*
 * Unit tests for Æther service discovery: beacon TLV codec (bounds,
 * charset, trailing-byte rejection) and the known-peer table (addr
 * keying, name ambiguity, aging, guarded eviction).
 *
 * Copyright (c) 2026 Æther Authors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/ztest.h>
#include <string.h>

#include <zephyr/net/aether_svcdisc.h>

static const uint8_t ADDR_A[6] = { 0, 0, 0, 0, 0xaa, 0x01 };
static const uint8_t ADDR_B[6] = { 0, 0, 0, 0, 0xbb, 0x02 };

static void table_reset(void *fixture)
{
	ARG_UNUSED(fixture);
	zassert_ok(aether_svcdisc_init());
	/* Age everything out: huge "now", zero allowed age. */
	aether_peer_age(0x40000000, 0);
	aether_svcdisc_set_name(NULL);
	aether_svcdisc_set_services(NULL, 0);
}

static void observe(const uint8_t addr[6], const char *name,
		    const char *svc0, uint32_t now)
{
	struct aether_peer p;

	memset(&p, 0, sizeof(p));
	memcpy(p.addr, addr, 6);
	if (name) {
		strncpy(p.name, name, AETHER_SVC_NAME_MAX);
	}
	if (svc0) {
		strncpy(p.svc[0].name, svc0, AETHER_SVC_NAME_MAX);
		p.nsvc = 1;
	}
	p.lastseen = now;
	aether_peer_observe(&p);
}

/* ---- name validation ---- */

ZTEST(svcdisc, test_name_charset)
{
	zassert_true(aether_svc_name_ok("9fs"));
	zassert_true(aether_svc_name_ok("chat"));
	zassert_true(aether_svc_name_ok("a"));
	zassert_true(aether_svc_name_ok("node-4"));
	zassert_true(aether_svc_name_ok("abcdefghijklmno"));   /* 15 */

	zassert_false(aether_svc_name_ok(NULL));
	zassert_false(aether_svc_name_ok(""));
	zassert_false(aether_svc_name_ok("abcdefghijklmnop"));  /* 16 */
	zassert_false(aether_svc_name_ok("-lead"));
	zassert_false(aether_svc_name_ok("Foo"));               /* uppercase */
	zassert_false(aether_svc_name_ok("a/b"));
	zassert_false(aether_svc_name_ok("."));
	zassert_false(aether_svc_name_ok(".."));
	zassert_false(aether_svc_name_ok("a b"));
	zassert_false(aether_svc_name_ok("a\x01"));
}

/* ---- codec ---- */

ZTEST(svcdisc, test_encode_decode_roundtrip)
{
	struct aether_svc svc[3] = {
		{ .name = "9fs" }, { .name = "chat" }, { .name = "oracle" },
	};
	struct aether_peer peer;
	uint8_t buf[AETHER_SVCADV_MAX];
	int len;

	len = aether_svcadv_encode(buf, sizeof(buf), "deck", svc, 3);
	zassert_true(len > 0);
	zassert_equal(len, 1 + 4 + 1 + (1 + 3) + (1 + 4) + (1 + 6));

	memset(&peer, 0, sizeof(peer));
	zassert_ok(aether_svcadv_decode(buf, len, &peer));
	zassert_str_equal(peer.name, "deck");
	zassert_equal(peer.nsvc, 3);
	zassert_str_equal(peer.svc[0].name, "9fs");
	zassert_str_equal(peer.svc[1].name, "chat");
	zassert_str_equal(peer.svc[2].name, "oracle");
}

ZTEST(svcdisc, test_encode_unnamed_and_empty)
{
	struct aether_svc svc[1] = { { .name = "term" } };
	struct aether_peer peer;
	uint8_t buf[AETHER_SVCADV_MAX];
	int len;

	/* Unnamed node advertising one service. */
	len = aether_svcadv_encode(buf, sizeof(buf), NULL, svc, 1);
	zassert_equal(len, 1 + 1 + 1 + 4);
	memset(&peer, 0xff, sizeof(peer));
	zassert_ok(aether_svcadv_decode(buf, len, &peer));
	zassert_equal(peer.name[0], '\0');
	zassert_equal(peer.nsvc, 1);

	/* Named node with no services. */
	len = aether_svcadv_encode(buf, sizeof(buf), "tag", NULL, 0);
	zassert_equal(len, 1 + 3 + 1);
	zassert_ok(aether_svcadv_decode(buf, len, &peer));
	zassert_str_equal(peer.name, "tag");
	zassert_equal(peer.nsvc, 0);
}

ZTEST(svcdisc, test_encode_rejects)
{
	struct aether_svc bad = { .name = "UPPER" };
	struct aether_svc ok = { .name = "9fs" };
	uint8_t buf[AETHER_SVCADV_MAX];
	uint8_t tiny[4];

	zassert_equal(aether_svcadv_encode(buf, sizeof(buf), "Bad", &ok, 1),
		      -EINVAL);
	zassert_equal(aether_svcadv_encode(buf, sizeof(buf), "n", &bad, 1),
		      -EINVAL);
	zassert_equal(aether_svcadv_encode(buf, sizeof(buf), "n", &ok,
					   AETHER_SVC_MAX + 1), -EINVAL);
	zassert_equal(aether_svcadv_encode(tiny, sizeof(tiny), "longname",
					   &ok, 1), -EMSGSIZE);
}

ZTEST(svcdisc, test_decode_rejects_malformed)
{
	struct aether_peer peer;
	uint8_t buf[AETHER_SVCADV_MAX];
	int len;

	/* Too short to hold namelen + nsvc. */
	zassert_equal(aether_svcadv_decode((uint8_t *)"", 0, &peer), -EBADMSG);
	zassert_equal(aether_svcadv_decode((uint8_t *)"\x00", 1, &peer),
		      -EBADMSG);

	/* namelen overruns the buffer. */
	buf[0] = 10; buf[1] = 'a';
	zassert_equal(aether_svcadv_decode(buf, 2, &peer), -EBADMSG);

	/* namelen over the cap. */
	buf[0] = AETHER_SVC_NAME_MAX + 1;
	memset(&buf[1], 'a', AETHER_SVC_NAME_MAX + 1);
	buf[AETHER_SVC_NAME_MAX + 2] = 0;
	zassert_equal(aether_svcadv_decode(buf, AETHER_SVC_NAME_MAX + 3, &peer),
		      -EBADMSG);

	/* nsvc over the cap. */
	buf[0] = 0; buf[1] = AETHER_SVC_MAX + 1;
	zassert_equal(aether_svcadv_decode(buf, 2, &peer), -EBADMSG);

	/* Service length runs past the end. */
	buf[0] = 0; buf[1] = 1; buf[2] = 5; buf[3] = 'a';
	zassert_equal(aether_svcadv_decode(buf, 4, &peer), -EBADMSG);

	/* Zero-length service name. */
	buf[0] = 0; buf[1] = 1; buf[2] = 0;
	zassert_equal(aether_svcadv_decode(buf, 3, &peer), -EBADMSG);

	/* Bad charset in a node name ("../x" flavor). */
	buf[0] = 2; buf[1] = '.'; buf[2] = '.'; buf[3] = 0;
	zassert_equal(aether_svcadv_decode(buf, 4, &peer), -EBADMSG);

	/* Bad charset in a service name. */
	buf[0] = 1; buf[1] = 'n'; buf[2] = 1; buf[3] = 3;
	buf[4] = 'a'; buf[5] = '/'; buf[6] = 'b';
	zassert_equal(aether_svcadv_decode(buf, 7, &peer), -EBADMSG);

	/* Trailing bytes after a valid advertisement: reject, don't repair. */
	struct aether_svc svc = { .name = "9fs" };

	len = aether_svcadv_encode(buf, sizeof(buf), "deck", &svc, 1);
	zassert_true(len > 0);
	buf[len] = 0x00;
	zassert_equal(aether_svcadv_decode(buf, len + 1, &peer), -EBADMSG);
}

/* ---- peer table ---- */

ZTEST(svcdisc, test_observe_and_lookup)
{
	struct aether_peer out;

	observe(ADDR_A, "alpha", "9fs", 1000);
	observe(ADDR_B, "beta", "chat", 2000);

	zassert_ok(aether_peer_get_by_addr(ADDR_A, &out));
	zassert_str_equal(out.name, "alpha");
	zassert_str_equal(out.svc[0].name, "9fs");
	zassert_equal(out.lastseen, 1000);

	zassert_equal(aether_peer_get_by_name("beta", &out), 1);
	zassert_mem_equal(out.addr, ADDR_B, 6);

	zassert_equal(aether_peer_get_by_name("nobody", &out), 0);

	/* Compacted iteration sees exactly two. */
	zassert_ok(aether_peer_get_at(0, &out));
	zassert_ok(aether_peer_get_at(1, &out));
	zassert_equal(aether_peer_get_at(2, &out), -ENOENT);
}

ZTEST(svcdisc, test_refresh_keyed_by_addr)
{
	struct aether_peer out;

	observe(ADDR_A, "alpha", "9fs", 1000);
	/* Same addr, new name + services: refreshes in place. */
	observe(ADDR_A, "renamed", "chat", 5000);

	zassert_ok(aether_peer_get_by_addr(ADDR_A, &out));
	zassert_str_equal(out.name, "renamed");
	zassert_equal(out.lastseen, 5000);
	zassert_equal(aether_peer_get_at(1, &out), -ENOENT);

	/* Old name no longer resolves. */
	zassert_equal(aether_peer_get_by_name("alpha", &out), 0);
}

ZTEST(svcdisc, test_name_collision_is_ambiguous)
{
	struct aether_peer out;

	/* Two live addrs claim "foo": both present (keyed by addr) ... */
	observe(ADDR_A, "foo", "9fs", 1000);
	observe(ADDR_B, "foo", "chat", 1001);
	zassert_ok(aether_peer_get_by_addr(ADDR_A, &out));
	zassert_ok(aether_peer_get_by_addr(ADDR_B, &out));

	/* ... and by-name reports the ambiguity, never picks. */
	zassert_equal(aether_peer_get_by_name("foo", &out), 2);
}

ZTEST(svcdisc, test_aging)
{
	struct aether_peer out;

	observe(ADDR_A, "old", NULL, 1000);
	observe(ADDR_B, "young", NULL, 90000);

	aether_peer_age(100000, 60000);

	zassert_equal(aether_peer_get_by_addr(ADDR_A, &out), -ENOENT);
	zassert_ok(aether_peer_get_by_addr(ADDR_B, &out));
}

ZTEST(svcdisc, test_eviction_guard)
{
	struct aether_peer out;
	uint8_t addr[6] = { 0, 0, 0, 0, 0, 0 };
	uint32_t now = 1000000;

	/* Fill the table; entry i last heard (i+1)*500 ms ago — even the
	 * oldest (16 s) is inside the 30 s guard window. */
	for (int i = 0; i < AETHER_SVCDISC_MAX_PEERS; i++) {
		addr[5] = i;
		observe(addr, NULL, NULL, now - (i + 1) * 500);
	}

	/* All entries recent: a newcomer is dropped, not evicted in. */
	observe(ADDR_A, "newcomer", NULL, now);
	zassert_equal(aether_peer_get_by_addr(ADDR_A, &out), -ENOENT);

	/* Make the oldest entry stale (beyond the guard), retry: the
	 * newcomer replaces exactly the oldest. */
	addr[5] = AETHER_SVCDISC_MAX_PEERS - 1;
	zassert_ok(aether_peer_get_by_addr(addr, &out));
	observe(addr, NULL, NULL, now - AETHER_SVCDISC_EVICT_GUARD_MS - 1000);
	observe(ADDR_A, "newcomer", NULL, now);
	zassert_ok(aether_peer_get_by_addr(ADDR_A, &out));
	zassert_equal(aether_peer_get_by_addr(addr, &out), -ENOENT);
}

/* ---- self / beacon integration ---- */

ZTEST(svcdisc, test_self_adv_roundtrip)
{
	struct aether_svc svc[2] = { { .name = "9fs" }, { .name = "chat" } };
	struct aether_peer out;
	uint8_t buf[AETHER_SVCADV_MAX];
	int len;

	/* Nothing configured: nothing to advertise. */
	zassert_equal(aether_svcdisc_build_adv(buf, sizeof(buf)), 0);

	zassert_ok(aether_svcdisc_set_name("deck"));
	zassert_ok(aether_svcdisc_set_services(svc, 2));
	zassert_equal(aether_svcdisc_set_name("Bad Name"), -EINVAL);

	len = aether_svcdisc_build_adv(buf, sizeof(buf));
	zassert_true(len > 0);

	/* Receive our own advertisement as if from a peer. */
	aether_svcdisc_ingest(ADDR_B, -80, buf, len, 7777);
	zassert_ok(aether_peer_get_by_addr(ADDR_B, &out));
	zassert_str_equal(out.name, "deck");
	zassert_equal(out.nsvc, 2);
	zassert_str_equal(out.svc[1].name, "chat");
	zassert_equal(out.rssi, -80);
	zassert_equal(out.lastseen, 7777);
}

ZTEST(svcdisc, test_ingest_legacy_and_malformed)
{
	struct aether_peer out;
	uint8_t junk[3] = { 9, 'a', 'b' };   /* namelen overruns */

	/* Legacy bare HELLO (no payload): peer observed, nameless. */
	aether_svcdisc_ingest(ADDR_A, -60, NULL, 0, 4242);
	zassert_ok(aether_peer_get_by_addr(ADDR_A, &out));
	zassert_equal(out.name[0], '\0');
	zassert_equal(out.nsvc, 0);
	zassert_equal(out.lastseen, 4242);

	/* Malformed advertisement: whole observation dropped. */
	aether_svcdisc_ingest(ADDR_B, -60, junk, sizeof(junk), 4243);
	zassert_equal(aether_peer_get_by_addr(ADDR_B, &out), -ENOENT);
}

ZTEST_SUITE(svcdisc, NULL, NULL, table_reset, NULL, NULL);
