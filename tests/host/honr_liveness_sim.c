/*
 * Copyright (c) 2026 Æther Authors
 *
 * SPDX-License-Identifier: MIT
 *
 * Host simulation of HONR parent-liveness maintenance under a lossy radio, built
 * to answer a specific question: does refreshing parent-liveness on ANY frame from
 * the parent (the fix) make the mesh MORE or LESS stable than refreshing only on the
 * parent's periodic broadcast TREE_ANNOUNCE (the original policy)?
 *
 * This is the dynamic layer the existing honr_mesh_sim.c does NOT model: it does a
 * lossless one-shot join and checks routing, so it never exercises ongoing liveness
 * and never caught the field bug where healthy children orphaned under load.
 *
 * Model (one tick == one announce interval):
 *   - A joined child either RECEIVES its parent's best-effort broadcast announce
 *     (prob 1 - p_loss) or misses it. Announces are unACKed, so loss is real.
 *   - ORIGINAL policy: miss counter resets only when the announce is heard; the child
 *     orphans after MISS_MAX consecutive misses (mirrors HONR_PARENT_MISS_MAX + the
 *     honr_parent_last_heard-only-on-TREE_ANNOUNCE code).
 *   - FIX policy: the miss counter ALSO resets if the child exchanged any other frame
 *     with the parent this tick -- unicast data / ACK / HELLO (prob p_traffic). This
 *     mirrors aether_honr_note_rx() refreshing liveness on any frame from the parent.
 *
 * The crux is the COUPLING to load. On a half-duplex radio, load raises BOTH:
 *   - p_loss  (a busy child transmitting can't hear the parent's broadcast), and
 *   - p_traffic (that same load IS the parent's unicast/ACK traffic to the child).
 * So under load the original policy loses the very announces it depends on, while the
 * fix turns that load into the liveness signal. We sweep load and count orphan events.
 *
 * Build & run (from the repo root):
 *   cc -std=c11 -O1 -Wall -Wextra -Werror tests/host/honr_liveness_sim.c -o /tmp/liveness_sim
 *   /tmp/liveness_sim
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define MISS_MAX   5      /* HONR_PARENT_MISS_MAX */
#define CHILDREN   200    /* independent children maintaining one parent */
#define TICKS      3000   /* announce intervals simulated */

/* Deterministic PRNG (xorshift32) so runs are reproducible without <time.h>. */
static uint32_t rng_state = 0x1234abcdu;
static double randf(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return (double)(rng_state & 0xffffffu) / (double)0x1000000u;
}

/* Simulate all children over TICKS; return total orphan events under one policy.
 * fix=0: only a heard announce resets the miss counter.
 * fix=1: a heard announce OR any parent traffic resets it. */
static long simulate(double p_loss, double p_traffic, int fix)
{
	long orphans = 0;

	for (int c = 0; c < CHILDREN; c++) {
		int miss = 0;

		for (int t = 0; t < TICKS; t++) {
			int heard_announce = (randf() >= p_loss);
			int heard_traffic  = fix && (randf() < p_traffic);

			if (heard_announce || heard_traffic) {
				miss = 0;
			} else {
				miss++;
			}
			if (miss >= MISS_MAX) {
				orphans++;   /* child declares parent dead -> orphan/re-elect churn */
				miss = 0;    /* re-joins and starts over */
			}
		}
	}
	return orphans;
}

int main(void)
{
	struct load { const char *name; double p_loss; double p_traffic; };
	/* Load levels. Half-duplex coupling: heavier load -> more missed announces AND
	 * more parent unicast/ACK traffic. (idle has little of either.) */
	struct load levels[] = {
		{ "idle",          0.05, 0.10 },
		{ "light",         0.20, 0.45 },
		{ "moderate",      0.40, 0.70 },
		{ "heavy (drain)", 0.60, 0.88 },
		{ "saturated",     0.75, 0.95 },
	};
	int nlevels = (int)(sizeof(levels) / sizeof(levels[0]));

	printf("HONR parent-liveness under announce loss (%d children x %d ticks, MISS_MAX=%d)\n",
	       CHILDREN, TICKS, MISS_MAX);
	printf("  %-15s  %8s  %8s  %8s   %10s\n",
	       "load", "p_loss", "p_traf", "orig", "fix");

	int fix_never_worse = 1;
	long total_orig = 0, total_fix = 0;

	for (int i = 0; i < nlevels; i++) {
		rng_state = 0x1234abcdu;                 /* same draws for both policies */
		long orig = simulate(levels[i].p_loss, levels[i].p_traffic, 0);
		rng_state = 0x1234abcdu;
		long fix  = simulate(levels[i].p_loss, levels[i].p_traffic, 1);

		total_orig += orig;
		total_fix  += fix;
		if (fix > orig) {
			fix_never_worse = 0;
		}
		printf("  %-15s  %8.2f  %8.2f  %8ld   %10ld  (%.0f%% fewer)\n",
		       levels[i].name, levels[i].p_loss, levels[i].p_traffic,
		       orig, fix, orig ? 100.0 * (double)(orig - fix) / (double)orig : 0.0);
	}

	printf("\n  TOTAL orphan events:  orig=%ld  fix=%ld\n", total_orig, total_fix);
	if (fix_never_worse && total_fix < total_orig) {
		printf("PASS: the fix orphans strictly less at every load, and far less under load.\n");
		return 0;
	}
	printf("FAIL: the fix did not reduce orphaning as expected.\n");
	return 1;
}
