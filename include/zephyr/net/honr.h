/*
 * Copyright (c) 2026 Æther Authors
 *
 * SPDX-License-Identifier: MIT
 *
 * HONR16 - Hierarchically Ordered Numbers for Routing, 16-bit.
 *
 * A structured tree-addressing scheme (from Dean Hall's HeyMac, heymac/net/
 * honr.py) in which the address itself encodes a node's position in the tree,
 * so routing is stateless: a frame travels up to the nearest common ancestor of
 * the resender and destination, then down to the destination, with each node
 * computing the single next hop from (resender, destination) alone - no route
 * tables and no route discovery.
 *
 * A 16-bit address is four 4-bit nibbles, most-significant first, one per tree
 * level. Nibble value 0 means "no child here" (end of path); 1..14 select a
 * child; 15 (0xF) is reserved for child-broadcast. The root is 0x0000. Address
 * validity requires every nibble to the right of the left-most zero to also be
 * zero (a left-justified path).
 *
 *   rank   = depth = index of the left-most zero nibble (0 = root)
 *   parent = zero the node's least-significant non-zero nibble
 *   NCA    = the shared left-to-right nibble prefix of two addresses
 *
 * Example (resender 0xC59A -> destination 0xC232, NCA 0xC000):
 *   C59A -> C590 -> C500 -> C000 -> C200 -> C230 -> C232
 *           (up to the NCA)        (down to the destination)
 */

#ifndef ZEPHYR_INCLUDE_NET_HONR_H_
#define ZEPHYR_INCLUDE_NET_HONR_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HONR_ROOT          0x0000U  /**< Root address. */
#define HONR_NIBBLES       4U       /**< Nibbles (levels) in a 16-bit address. */
#define HONR_CHILD_MIN     0x1U     /**< Lowest child nibble value. */
#define HONR_CHILD_MAX     0xEU     /**< Highest child nibble value (14 children). */
#define HONR_BCAST_NIBBLE  0xFU     /**< Reserved child-broadcast nibble. */

/* Compile-time invariants (portable: keyword, no <assert.h>). */
_Static_assert(HONR_NIBBLES * 4U == 16U, "HONR address must be 16 bits");
_Static_assert(HONR_CHILD_MAX == 0xEU, "14 children per node");
_Static_assert(HONR_BCAST_NIBBLE == 0xFU, "broadcast nibble is 0xF");

/** @brief Nibble @p i (0 = most significant) of a 16-bit address. */
static inline uint8_t honr_nibble(uint16_t addr, unsigned int i)
{
	return (uint8_t)((addr >> (12U - 4U * i)) & 0xFU);
}

/** @brief Return @p addr with nibble @p i (0 = most significant) set to @p v. */
static inline uint16_t honr_set_nibble(uint16_t addr, unsigned int i, uint8_t v)
{
	unsigned int shift = 12U - 4U * i;
	unsigned int cleared = (unsigned int)addr & ~(0xFU << shift);

	return (uint16_t)(cleared | (((unsigned int)(v & 0xFU)) << shift));
}

/** @brief Rank (tree depth): index of the left-most zero nibble. 0 = root. */
static inline uint8_t honr_rank(uint16_t addr)
{
	for (unsigned int i = 0; i < HONR_NIBBLES; i++) {
		if (honr_nibble(addr, i) == 0U) {
			return (uint8_t)i;
		}
	}
	return (uint8_t)HONR_NIBBLES;
}

/** @brief True if @p addr is the root (0x0000). */
static inline bool honr_is_root(uint16_t addr)
{
	return addr == HONR_ROOT;
}

/** @brief Parent of @p addr: clears the least-significant non-zero nibble.
 *  Returns HONR_ROOT for the root.
 */
static inline uint16_t honr_parent(uint16_t addr)
{
	uint8_t rank = honr_rank(addr);

	if (rank == 0U) {
		return HONR_ROOT;
	}
	return honr_set_nibble(addr, (unsigned int)(rank - 1U), 0U);
}

/**
 * @brief True if @p addr is a well-formed HONR *node* address.
 *
 * A node address is a left-justified path of child nibbles (1..14) followed by
 * zeros: every nibble before the first zero must be in 1..14, and every nibble
 * from the first zero onward must be zero. The root (0x0000) is valid; the
 * broadcast nibble (0xF) is not allowed in a node address. Use this to reject
 * malformed or untrusted addresses received over the air before routing on
 * them (the routing math assumes well-formed input).
 */
static inline bool honr_is_valid_node(uint16_t addr)
{
	bool in_zeros = false;

	for (unsigned int i = 0; i < HONR_NIBBLES; i++) {
		uint8_t nb = honr_nibble(addr, i);

		if (in_zeros) {
			if (nb != 0U) {
				return false; /* non-zero after the path ended */
			}
		} else if (nb == 0U) {
			in_zeros = true;
		} else if (nb > HONR_CHILD_MAX) {
			return false; /* 0xF or otherwise out of child range */
		}
	}
	return true;
}

/** @brief Nearest common ancestor: the shared left-to-right nibble prefix. */
static inline uint16_t honr_nca(uint16_t a, uint16_t b)
{
	uint16_t nca = HONR_ROOT;

	for (unsigned int i = 0; i < HONR_NIBBLES; i++) {
		uint8_t na = honr_nibble(a, i);

		if (na == 0U || na != honr_nibble(b, i)) {
			break;
		}
		nca = honr_set_nibble(nca, i, na);
	}
	return nca;
}

/** @brief True if @p anc is an ancestor of (or equal to) @p node. */
static inline bool honr_is_ancestor(uint16_t anc, uint16_t node)
{
	return honr_nca(anc, node) == anc;
}

/**
 * @brief Next-hop address after @p resender toward @p dst.
 *
 * Stateless HONR forwarding: ascend toward the nearest common ancestor while
 * @p resender is not itself an ancestor of @p dst, then descend one level
 * toward @p dst. A node relays iff this equals its own address. Returns @p dst
 * when @p resender already equals @p dst (nothing to forward).
 */
static inline uint16_t honr_next_hop(uint16_t resender, uint16_t dst)
{
	uint8_t rank;

	if (resender == dst) {
		return dst;
	}

	/* Ascending: resender is not yet an ancestor of dst. */
	if (!honr_is_ancestor(resender, dst)) {
		return honr_parent(resender);
	}

	/* Descending: take the child of resender on the path to dst. */
	rank = honr_rank(resender);
	return honr_set_nibble(resender, rank, honr_nibble(dst, rank));
}

/** @brief True if @p self should relay a frame from @p resender toward @p dst. */
static inline bool honr_should_relay(uint16_t self, uint16_t resender, uint16_t dst)
{
	return self != dst && honr_next_hop(resender, dst) == self;
}

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_NET_HONR_H_ */
