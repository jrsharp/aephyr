/*
 * Copyright (c) 2025 Æther Authors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MESSAGE_STORE_H_
#define MESSAGE_STORE_H_

#include <zephyr/kernel.h>
#include <stdint.h>
#include <stdbool.h>

/* Message store configuration */
#define MSG_STORE_MAX_MESSAGES    10
#define MSG_STORE_MAX_SIZE        128
#define MSG_STORE_MAX_ADDR_LEN    16

/* Message flags */
#define MSG_FLAG_READ        0x01
#define MSG_FLAG_SENT        0x02
#define MSG_FLAG_ACKED       0x04
#define MSG_FLAG_FAILED      0x08
#define MSG_FLAG_BROADCAST   0x10
#define MSG_FLAG_QUEUED      0x20

/* Message entry */
struct msg_entry {
	uint16_t id;                    /* Message ID */
	uint8_t flags;                  /* Message flags */
	uint8_t direction;              /* 0=RX, 1=TX */
	char addr[MSG_STORE_MAX_ADDR_LEN]; /* Source/destination address */
	uint8_t data[MSG_STORE_MAX_SIZE];  /* Message data */
	uint16_t len;                   /* Data length */
	uint32_t timestamp;             /* Reception/send time */
	uint8_t retry_count;            /* TX retry count */
	struct k_sem tx_sem;            /* TX completion semaphore */
};

/* Message store context */
struct msg_store {
	struct msg_entry messages[MSG_STORE_MAX_MESSAGES];
	uint16_t next_id;
	struct k_mutex lock;
	
	/* Statistics */
	uint32_t rx_count;
	uint32_t tx_count;
	uint32_t dropped_count;
};

/* Message store API */

/**
 * Initialize message store
 */
void msg_store_init(void);

/**
 * Store received message
 *
 * @param addr Source address
 * @param data Message data
 * @param len Data length
 * @return Message ID on success, negative error code
 */
int msg_store_rx(const char *addr, const uint8_t *data, size_t len);

/**
 * Queue message for transmission
 *
 * @param addr Destination address
 * @param data Message data
 * @param len Data length
 * @param broadcast True for broadcast message
 * @return Message ID on success, negative error code
 */
int msg_store_tx(const char *addr, const uint8_t *data, size_t len, bool broadcast);

/**
 * Get message by ID
 *
 * @param id Message ID
 * @return Message entry or NULL if not found
 */
struct msg_entry *msg_store_get(uint16_t id);

/**
 * Delete message
 *
 * @param id Message ID
 * @return 0 on success, negative error code
 */
int msg_store_delete(uint16_t id);

/**
 * Get number of stored messages
 *
 * @param rx_only Count only received messages
 * @return Number of messages
 */
int msg_store_count(bool rx_only);

/**
 * Get next unread received message
 *
 * @return Message entry or NULL if no unread messages
 */
struct msg_entry *msg_store_get_unread(void);

/**
 * Mark message as read
 *
 * @param id Message ID
 * @return 0 on success, negative error code
 */
int msg_store_mark_read(uint16_t id);

/**
 * Mark TX message as sent
 *
 * @param id Message ID
 * @param success True if sent successfully
 * @return 0 on success, negative error code
 */
int msg_store_mark_sent(uint16_t id, bool success);

/**
 * Get next queued TX message
 *
 * @return Message entry or NULL if no queued messages
 */
struct msg_entry *msg_store_get_tx_queued(void);

/**
 * Clean up old messages
 *
 * @param max_age_ms Maximum age in milliseconds
 * @return Number of messages deleted
 */
int msg_store_cleanup(uint32_t max_age_ms);

/**
 * Get store statistics
 *
 * @param rx_count Pointer to RX count (can be NULL)
 * @param tx_count Pointer to TX count (can be NULL)
 * @param dropped_count Pointer to dropped count (can be NULL)
 */
void msg_store_get_stats(uint32_t *rx_count, uint32_t *tx_count, uint32_t *dropped_count);

#endif /* MESSAGE_STORE_H_ */