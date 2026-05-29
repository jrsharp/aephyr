/*
 * Copyright (c) 2026 Æther Authors
 *
 * SPDX-License-Identifier: MIT
 *
 * Host-side HONR16 protocol simulator. No Zephyr, no hardware: it builds random
 * trees by simulating the step-3 join (each parent allocates its next free child
 * nibble) and then verifies that the step-2 stateless forwarding routes between
 * every pair of nodes with the optimal hop count and no loops. Complements the
 * on-target ztest (tests/subsys/honr) for fast, deterministic, scalable checks.
 *
 * Build & run (from the repo root):
 *   cc -I include/zephyr/net -Wall -Wextra tests/host/honr_sim.c -o /tmp/honr_sim
 *   /tmp/honr_sim
 */

#include <stdio.h>
#include <stdlib.h>
#include "honr.h"

#define MAX_NODES 200

struct node {
	uint16_t addr;
	uint16_t child_bitmap; /* bit n (1..14) set = nibble allocated */
};

static struct node nodes[MAX_NODES];
static int node_count;

/* Mirror of the firmware's child allocation (aether_honr.c). */
static int alloc_child(struct node *p)
{
	if (honr_rank(p->addr) >= HONR_NIBBLES) {
		return -1;
	}
	for (unsigned int n = HONR_CHILD_MIN; n <= HONR_CHILD_MAX; n++) {
		if (!(p->child_bitmap & (1u << n))) {
			p->child_bitmap |= (1u << n);
			return (int)n;
		}
	}
	return -1;
}

/* Build a random tree of up to `target` nodes rooted at 0x0000. */
static void build_random_tree(int target)
{
	node_count = 0;
	nodes[node_count].addr = HONR_ROOT;
	nodes[node_count].child_bitmap = 0;
	node_count++;

	int attempts = 0;

	while (node_count < target && attempts < target * 50) {
		attempts++;
		struct node *p = &nodes[rand() % node_count];
		int nib = alloc_child(p);

		if (nib < 0) {
			continue; /* parent full or at max depth; try another */
		}
		uint16_t child = honr_set_nibble(p->addr, honr_rank(p->addr), (uint8_t)nib);

		nodes[node_count].addr = child;
		nodes[node_count].child_bitmap = 0;
		node_count++;
	}
}

/* Walk next-hops from src to dst; return hop count, or -1 on loop/stall. */
static int sim_route(uint16_t src, uint16_t dst)
{
	uint16_t cur = src;
	int hops = 0;

	while (cur != dst) {
		uint16_t next = honr_next_hop(cur, dst);

		if (next == cur || ++hops > 2 * (int)HONR_NIBBLES + 1) {
			return -1;
		}
		cur = next;
	}
	return hops;
}

/* Verify every ordered pair routes optimally. Returns failure count. */
static int verify_all_pairs(int *max_hops_out)
{
	int fail = 0, max_hops = 0;

	for (int i = 0; i < node_count; i++) {
		for (int j = 0; j < node_count; j++) {
			if (i == j) {
				continue;
			}
			uint16_t a = nodes[i].addr, b = nodes[j].addr;
			int hops = sim_route(a, b);
			int nr = honr_rank(honr_nca(a, b));
			int expect = (honr_rank(a) - nr) + (honr_rank(b) - nr);

			if (hops < 0 || hops != expect) {
				if (fail < 5) {
					printf("  FAIL %04x->%04x hops=%d expect=%d\n",
					       a, b, hops, expect);
				}
				fail++;
			}
			if (hops > max_hops) {
				max_hops = hops;
			}
		}
	}
	if (max_hops_out) {
		*max_hops_out = max_hops;
	}
	return fail;
}

int main(void)
{
	int total_fail = 0;
	long total_pairs = 0;
	int overall_max = 0;

	/* Many deterministic random topologies. */
	for (int seed = 1; seed <= 200; seed++) {
		srand((unsigned int)seed);
		build_random_tree(60);

		int max_hops = 0;
		int fail = verify_all_pairs(&max_hops);

		total_fail += fail;
		total_pairs += (long)node_count * (node_count - 1);
		if (max_hops > overall_max) {
			overall_max = max_hops;
		}
	}

	printf("HONR routing over 200 random trees: %ld pairs, %d failures, max %d hops\n",
	       total_pairs, total_fail, overall_max);
	printf("%s\n", total_fail ? "FAILED" : "PASS: every pair routes optimally, no loops");
	return total_fail ? 1 : 0;
}
