/*
 * Copyright (c) 2026 Æther Authors
 *
 * SPDX-License-Identifier: MIT
 *
 * Unit tests for the HONR16 address math (<zephyr/net/honr.h>). The header is
 * pure and dependency-free, so these run on any platform (qemu_x86 / native_sim)
 * with no DECT hardware. Includes a property test that exhaustively simulates
 * forwarding between every pair of nodes in a small tree and asserts delivery
 * with no loops and a bounded hop count.
 */

#include <zephyr/ztest.h>
#include <zephyr/net/honr.h>

/* --- Primitive checks ----------------------------------------------------- */

ZTEST(honr, test_rank)
{
	zassert_equal(honr_rank(0x0000), 0, "root rank");
	zassert_equal(honr_rank(0xC000), 1, "depth-1 rank");
	zassert_equal(honr_rank(0xC500), 2, "depth-2 rank");
	zassert_equal(honr_rank(0xC59A), 4, "full-depth rank");
}

ZTEST(honr, test_parent)
{
	zassert_equal(honr_parent(0x0000), 0x0000, "root has no parent");
	zassert_equal(honr_parent(0xC000), 0x0000, "parent of depth-1 is root");
	zassert_equal(honr_parent(0xC59A), 0xC590, "parent clears last nibble");
	zassert_equal(honr_parent(0xC500), 0xC000, "parent of C500");
}

ZTEST(honr, test_nca)
{
	zassert_equal(honr_nca(0xC59A, 0xC232), 0xC000, "nca shares C");
	zassert_equal(honr_nca(0x1000, 0x2000), 0x0000, "disjoint -> root");
	zassert_equal(honr_nca(0x1230, 0x1240), 0x1200, "shared 1,2 prefix");
	zassert_equal(honr_nca(0xC200, 0xC59A), 0xC000, "nca of C200,C59A");
}

ZTEST(honr, test_ancestor)
{
	zassert_true(honr_is_ancestor(0x0000, 0xC59A), "root is ancestor of all");
	zassert_true(honr_is_ancestor(0xC000, 0xC232), "C000 ancestor of C232");
	zassert_true(honr_is_ancestor(0xC230, 0xC232), "near ancestor");
	zassert_false(honr_is_ancestor(0xC200, 0xC59A), "C200 not ancestor of C59A");
	zassert_false(honr_is_ancestor(0xD000, 0xC232), "different branch");
}

/* The worked example from HeyMac: C59A -> C232 via NCA C000. */
ZTEST(honr, test_example_route)
{
	static const uint16_t expect[] = {0xC59A, 0xC590, 0xC500, 0xC000,
					  0xC200, 0xC230, 0xC232};
	uint16_t cur = 0xC59A;

	for (int i = 1; i < (int)ARRAY_SIZE(expect); i++) {
		cur = honr_next_hop(cur, 0xC232);
		zassert_equal(cur, expect[i], "hop %d expected %04x got %04x",
			      i, expect[i], cur);
	}
}

ZTEST(honr, test_relay_self_selection)
{
	/* Along the example route, exactly the next hop relays. */
	zassert_true(honr_should_relay(0xC590, 0xC59A, 0xC232), "C590 is next hop");
	zassert_false(honr_should_relay(0xC500, 0xC59A, 0xC232), "C500 is not");
	zassert_false(honr_should_relay(0xD000, 0xC59A, 0xC232), "off-path does not relay");
	/* The destination delivers rather than relays. */
	zassert_false(honr_should_relay(0xC232, 0xC230, 0xC232), "dst does not relay");
}

/* --- Property test: simulate forwarding over a whole tree ------------------ */

/* Walk next-hops from src to dst; return hop count, or -1 on loop/stall. */
static int simulate_route(uint16_t src, uint16_t dst)
{
	uint16_t cur = src;
	int hops = 0;

	while (cur != dst) {
		uint16_t next = honr_next_hop(cur, dst);

		if (next == cur) {
			return -1; /* stalled */
		}
		cur = next;
		if (++hops > 2 * (int)HONR_NIBBLES + 1) {
			return -1; /* loop guard */
		}
	}
	return hops;
}

ZTEST(honr, test_all_pairs_route)
{
	/* Enumerate a tree: root + children using nibbles 1..3, depth up to 3. */
	uint16_t nodes[64];
	int n = 0;

	nodes[n++] = HONR_ROOT;
	for (uint16_t a = 1; a <= 3; a++) {
		uint16_t na = honr_set_nibble(HONR_ROOT, 0, a);

		nodes[n++] = na;
		for (uint16_t b = 1; b <= 3; b++) {
			uint16_t nb = honr_set_nibble(na, 1, b);

			nodes[n++] = nb;
			for (uint16_t c = 1; c <= 3; c++) {
				nodes[n++] = honr_set_nibble(nb, 2, c);
			}
		}
	}

	for (int i = 0; i < n; i++) {
		for (int j = 0; j < n; j++) {
			if (i == j) {
				continue;
			}

			int hops = simulate_route(nodes[i], nodes[j]);
			int nca_rank = honr_rank(honr_nca(nodes[i], nodes[j]));
			int expect = (honr_rank(nodes[i]) - nca_rank) +
				     (honr_rank(nodes[j]) - nca_rank);

			zassert_true(hops >= 0, "%04x->%04x failed to route",
				     nodes[i], nodes[j]);
			zassert_equal(hops, expect,
				      "%04x->%04x took %d hops, expected %d",
				      nodes[i], nodes[j], hops, expect);
		}
	}
}

ZTEST_SUITE(honr, NULL, NULL, NULL, NULL, NULL);
