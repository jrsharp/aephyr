/*
 * Copyright (c) 2026 Æther Authors
 *
 * SPDX-License-Identifier: MIT
 *
 * Host multi-node mesh simulator for HONR16 over a radio topology. Unlike the
 * tree-only check in honr_sim.c, this models an RF medium: a who-hears-whom
 * matrix constrains both the join (a node can only attach to a parent it
 * directly hears) and routing (every forwarding hop must be a real radio link).
 *
 * It runs the discrete-event join protocol (single-hop TREE_ANNOUNCE / JOIN_REQ
 * / JOIN_REP) and then the stateless HONR forward decision, both mirroring the
 * firmware (aether_honr.c) and sharing the real address math (honr.h). For each
 * topology it asserts:
 *   - every joined non-root node attached over a bidirectional radio link;
 *   - every all-pairs route delivers, and every hop is a radio neighbor.
 *
 * Build & run (from the repo root):
 *   cc -I include/zephyr/net -Wall -Wextra tests/host/honr_mesh_sim.c -o /tmp/mesh_sim
 *   /tmp/mesh_sim
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "honr.h"

#define MAX_NODES 49

struct snode {
	uint16_t addr;
	uint16_t child_bitmap;
	int parent;   /* node index of HONR parent, -1 for root/unjoined */
	int joined;
};

static struct snode node[MAX_NODES];
static int hears[MAX_NODES][MAX_NODES]; /* hears[i][j] = i receives j's TX */
static int n;

static int link_ok(int a, int b) /* bidirectional radio link */
{
	return hears[a][b] && hears[b][a];
}

static int alloc_child(struct snode *p) /* mirrors aether_honr.c */
{
	if (honr_rank(p->addr) >= HONR_NIBBLES) {
		return -1;
	}
	for (unsigned int c = HONR_CHILD_MIN; c <= HONR_CHILD_MAX; c++) {
		if (!(p->child_bitmap & (1u << c))) {
			p->child_bitmap |= (1u << c);
			return (int)c;
		}
	}
	return -1;
}

static int addr_to_node(uint16_t a)
{
	for (int i = 0; i < n; i++) {
		if (node[i].joined && node[i].addr == a) {
			return i;
		}
	}
	return -1;
}

/* Run the join protocol round by round until it converges. Node 0 is root. */
static void run_join(void)
{
	for (int i = 0; i < n; i++) {
		node[i].joined = 0;
		node[i].child_bitmap = 0;
		node[i].parent = -1;
		node[i].addr = 0;
	}
	node[0].joined = 1;
	node[0].addr = HONR_ROOT;

	for (int round = 0; round < n + 2; round++) {
		int progressed = 0;

		for (int u = 0; u < n; u++) {
			if (node[u].joined) {
				continue;
			}
			/* Choose the lowest-rank joined neighbour with capacity. */
			int best = -1;

			for (int v = 0; v < n; v++) {
				if (v == u || !node[v].joined || !link_ok(u, v)) {
					continue;
				}
				if (honr_rank(node[v].addr) >= HONR_NIBBLES) {
					continue;
				}
				if (best < 0 ||
				    honr_rank(node[v].addr) < honr_rank(node[best].addr)) {
					best = v;
				}
			}
			if (best < 0) {
				continue;
			}
			int nib = alloc_child(&node[best]);

			if (nib < 0) {
				continue;
			}
			node[u].addr = honr_set_nibble(node[best].addr,
						       honr_rank(node[best].addr), (uint8_t)nib);
			node[u].parent = best;
			node[u].joined = 1;
			progressed = 1;
		}
		if (!progressed) {
			break;
		}
	}
}

/* Forward from src node to dst node by HONR; assert every hop is a radio link.
 * Returns hop count, or negative on failure (-1 stall/loop, -2 non-radio hop).
 */
static int route(int src, int dst)
{
	int cur = src;
	int hops = 0;

	while (cur != dst) {
		uint16_t nh = honr_next_hop(node[cur].addr, node[dst].addr);
		int nx = addr_to_node(nh);

		if (nx < 0) {
			return -1; /* next-hop address has no node */
		}
		if (!link_ok(cur, nx)) {
			return -2; /* HONR next hop is not a physical neighbour */
		}
		cur = nx;
		if (++hops > 2 * (int)HONR_NIBBLES + 1) {
			return -1;
		}
	}
	return hops;
}

/* Returns the number of violations; prints a summary line when verbose. */
static int check_topology(const char *name, int verbose)
{
	int joined = 0, bad_tree = 0, pairs = 0, undeliv = 0, nonradio = 0;

	run_join();

	for (int i = 0; i < n; i++) {
		if (!node[i].joined) {
			continue;
		}
		joined++;
		if (i != 0 && !link_ok(i, node[i].parent)) {
			bad_tree++;
		}
	}

	for (int a = 0; a < n; a++) {
		for (int b = 0; b < n; b++) {
			if (a == b || !node[a].joined || !node[b].joined) {
				continue;
			}
			pairs++;
			int h = route(a, b);

			if (h == -2) {
				nonradio++;
			} else if (h < 0) {
				undeliv++;
			}
		}
	}

	if (verbose) {
		printf("  %-14s nodes=%2d joined=%2d pairs=%4d  "
		       "bad_tree=%d undeliv=%d nonradio=%d\n",
		       name, n, joined, pairs, bad_tree, undeliv, nonradio);
	}
	return bad_tree + undeliv + nonradio;
}

/* ---- topology builders ---- */

static void clear_links(void)
{
	memset(hears, 0, sizeof(hears));
}

static void add_link(int a, int b)
{
	hears[a][b] = 1;
	hears[b][a] = 1;
}

static void build_line(int count) /* 0-1-2-...; depth-limited to 4 from root */
{
	n = count;
	clear_links();
	for (int i = 1; i < n; i++) {
		add_link(i - 1, i);
	}
}

static void build_star(int count) /* root hears everyone */
{
	n = count;
	clear_links();
	for (int i = 1; i < n; i++) {
		add_link(0, i);
	}
}

static void build_grid(int side) /* side x side, 4-neighbour */
{
	n = side * side;
	clear_links();
	for (int r = 0; r < side; r++) {
		for (int c = 0; c < side; c++) {
			int id = r * side + c;

			if (c + 1 < side) {
				add_link(id, id + 1);
			}
			if (r + 1 < side) {
				add_link(id, id + side);
			}
		}
	}
}

static void build_random_connected(int count, unsigned int seed)
{
	n = count;
	clear_links();
	srand(seed);
	/* Random spanning tree keeps the graph connected. */
	for (int i = 1; i < n; i++) {
		add_link(i, rand() % i);
	}
	/* A few extra edges for redundancy. */
	for (int e = 0; e < n; e++) {
		add_link(rand() % n, rand() % n);
	}
}

int main(void)
{
	int fails = 0;

	printf("HONR multi-node mesh simulation (join + route over radio topology)\n");

	build_line(5);
	fails += check_topology("line-5", 1);
	build_star(15);
	fails += check_topology("star-15", 1);
	build_grid(5);
	fails += check_topology("grid-5x5", 1);

	int rand_fail = 0;

	for (unsigned int s = 1; s <= 100; s++) {
		build_random_connected(20, s);
		rand_fail += check_topology("random-20", 0);
	}
	printf("  %-14s runs=100 failures=%d\n", "random-20", rand_fail);
	fails += rand_fail;

	printf("%s\n", fails ? "FAILED" : "PASS: tree built over radio links; all hops are radio neighbours");
	return fails ? 1 : 0;
}
