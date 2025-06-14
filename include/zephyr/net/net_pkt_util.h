/*
 * Copyright (c) 2025 Æther Authors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_NET_NET_PKT_UTIL_H_
#define ZEPHYR_INCLUDE_NET_NET_PKT_UTIL_H_

#include <zephyr/net/net_pkt.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get pointer to packet data
 * 
 * @param pkt Network packet
 * @return Pointer to first data byte, or NULL if no data
 */
static inline uint8_t *net_pkt_get_data(struct net_pkt *pkt)
{
	if (!pkt || !pkt->frags) {
		return NULL;
	}
	return pkt->frags->data;
}

/**
 * @brief Get total packet data length
 * 
 * @param pkt Network packet
 * @return Total length of all fragments
 */
static inline size_t net_pkt_get_len(struct net_pkt *pkt)
{
	if (!pkt) {
		return 0;
	}
	return net_pkt_get_len(pkt);
}

/**
 * @brief Push data to front of packet
 * 
 * @param pkt Network packet
 * @param len Number of bytes to push
 * @return Pointer to pushed data area, or NULL on failure
 */
static inline uint8_t *net_pkt_push(struct net_pkt *pkt, size_t len)
{
	if (!pkt || !pkt->frags) {
		return NULL;
	}
	
	struct net_buf *frag = pkt->frags;
	
	if (frag->data - frag->__buf < len) {
		/* Not enough headroom */
		return NULL;
	}
	
	frag->data -= len;
	frag->len += len;
	
	return frag->data;
}

/**
 * @brief Pull data from front of packet
 * 
 * @param pkt Network packet  
 * @param len Number of bytes to pull
 * @return Pointer to new data start, or NULL on failure
 */
static inline uint8_t *net_pkt_pull(struct net_pkt *pkt, size_t len)
{
	if (!pkt || !pkt->frags) {
		return NULL;
	}
	
	struct net_buf *frag = pkt->frags;
	
	if (frag->len < len) {
		/* Not enough data */
		return NULL;
	}
	
	frag->data += len;
	frag->len -= len;
	
	return frag->data;
}

/**
 * @brief Write data to packet at current position
 * 
 * @param pkt Network packet
 * @param data Data to write
 * @param len Length of data
 * @return 0 on success, negative errno on failure
 */
static inline int net_pkt_write(struct net_pkt *pkt, const void *data, size_t len)
{
	if (!pkt || !pkt->frags || !data) {
		return -EINVAL;
	}
	
	struct net_buf *frag = pkt->frags;
	
	if (net_buf_tailroom(frag) < len) {
		/* Not enough space */
		return -ENOMEM;
	}
	
	memcpy(net_buf_add(frag, len), data, len);
	return 0;
}

/**
 * @brief Read data from packet at current position
 * 
 * @param pkt Network packet
 * @param data Buffer to read into
 * @param len Length of data to read
 * @return 0 on success, negative errno on failure
 */
static inline int net_pkt_read(struct net_pkt *pkt, void *data, size_t len)
{
	if (!pkt || !pkt->frags || !data) {
		return -EINVAL;
	}
	
	struct net_buf *frag = pkt->frags;
	
	if (frag->len < len) {
		/* Not enough data */
		return -ENOMEM;
	}
	
	memcpy(data, frag->data, len);
	net_buf_pull(frag, len);
	return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_NET_NET_PKT_UTIL_H_ */