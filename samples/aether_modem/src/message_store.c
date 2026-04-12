/*
 * Copyright (c) 2025 Æther Authors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "message_store.h"

LOG_MODULE_REGISTER(msg_store, LOG_LEVEL_DBG);

/* Global message store */
static struct msg_store g_store;

/* Initialize message store */
void msg_store_init(void)
{
	memset(&g_store, 0, sizeof(g_store));
	k_mutex_init(&g_store.lock);
	g_store.next_id = 1;
	
	/* Initialize semaphores */
	for (int i = 0; i < MSG_STORE_MAX_MESSAGES; i++) {
		k_sem_init(&g_store.messages[i].tx_sem, 0, 1);
	}
	
	LOG_INF("Message store initialized");
}

/* Find free slot */
static int find_free_slot(void)
{
	for (int i = 0; i < MSG_STORE_MAX_MESSAGES; i++) {
		if (g_store.messages[i].id == 0) {
			return i;
		}
	}
	return -1;
}

/* Store received message */
int msg_store_rx(const char *addr, const uint8_t *data, size_t len)
{
	int slot;
	struct msg_entry *msg;
	
	if (!addr || !data || len == 0 || len > MSG_STORE_MAX_SIZE) {
		return -EINVAL;
	}
	
	k_mutex_lock(&g_store.lock, K_FOREVER);
	
	slot = find_free_slot();
	if (slot < 0) {
		/* Store full - find oldest read message to replace */
		uint32_t oldest_time = UINT32_MAX;
		int oldest_slot = -1;
		
		for (int i = 0; i < MSG_STORE_MAX_MESSAGES; i++) {
			if (g_store.messages[i].direction == 0 &&
			    (g_store.messages[i].flags & MSG_FLAG_READ) &&
			    g_store.messages[i].timestamp < oldest_time) {
				oldest_time = g_store.messages[i].timestamp;
				oldest_slot = i;
			}
		}
		
		if (oldest_slot >= 0) {
			slot = oldest_slot;
		} else {
			k_mutex_unlock(&g_store.lock);
			g_store.dropped_count++;
			LOG_WRN("Message store full, dropping RX message");
			return -ENOMEM;
		}
	}
	
	msg = &g_store.messages[slot];
	memset(msg, 0, sizeof(*msg));
	
	msg->id = g_store.next_id++;
	msg->flags = 0;
	msg->direction = 0;  /* RX */
	strncpy(msg->addr, addr, MSG_STORE_MAX_ADDR_LEN - 1);
	memcpy(msg->data, data, len);
	msg->len = len;
	msg->timestamp = k_uptime_get_32();
	
	g_store.rx_count++;
	
	k_mutex_unlock(&g_store.lock);
	
	LOG_DBG("Stored RX message %d from %s, len=%zu", msg->id, addr, len);
	return msg->id;
}

/* Queue message for transmission */
int msg_store_tx(const char *addr, const uint8_t *data, size_t len, bool broadcast)
{
	int slot;
	struct msg_entry *msg;
	
	if ((!addr && !broadcast) || !data || len == 0 || len > MSG_STORE_MAX_SIZE) {
		return -EINVAL;
	}
	
	k_mutex_lock(&g_store.lock, K_FOREVER);
	
	slot = find_free_slot();
	if (slot < 0) {
		k_mutex_unlock(&g_store.lock);
		LOG_WRN("Message store full, cannot queue TX");
		return -ENOMEM;
	}
	
	msg = &g_store.messages[slot];
	memset(msg, 0, sizeof(*msg));
	
	msg->id = g_store.next_id++;
	msg->flags = MSG_FLAG_QUEUED;
	if (broadcast) {
		msg->flags |= MSG_FLAG_BROADCAST;
		strcpy(msg->addr, "BROADCAST");
	} else {
		strncpy(msg->addr, addr, MSG_STORE_MAX_ADDR_LEN - 1);
	}
	msg->direction = 1;  /* TX */
	memcpy(msg->data, data, len);
	msg->len = len;
	msg->timestamp = k_uptime_get_32();
	msg->retry_count = 0;
	
	g_store.tx_count++;
	
	k_mutex_unlock(&g_store.lock);
	
	LOG_DBG("Queued TX message %d to %s, len=%zu", msg->id, msg->addr, len);
	return msg->id;
}

/* Get message by ID */
struct msg_entry *msg_store_get(uint16_t id)
{
	struct msg_entry *msg = NULL;
	
	k_mutex_lock(&g_store.lock, K_FOREVER);
	
	for (int i = 0; i < MSG_STORE_MAX_MESSAGES; i++) {
		if (g_store.messages[i].id == id) {
			msg = &g_store.messages[i];
			break;
		}
	}
	
	k_mutex_unlock(&g_store.lock);
	return msg;
}

/* Delete message */
int msg_store_delete(uint16_t id)
{
	int ret = -ENOENT;
	
	k_mutex_lock(&g_store.lock, K_FOREVER);
	
	for (int i = 0; i < MSG_STORE_MAX_MESSAGES; i++) {
		if (g_store.messages[i].id == id) {
			g_store.messages[i].id = 0;  /* Mark as free */
			ret = 0;
			LOG_DBG("Deleted message %d", id);
			break;
		}
	}
	
	k_mutex_unlock(&g_store.lock);
	return ret;
}

/* Get message count */
int msg_store_count(bool rx_only)
{
	int count = 0;
	
	k_mutex_lock(&g_store.lock, K_FOREVER);
	
	for (int i = 0; i < MSG_STORE_MAX_MESSAGES; i++) {
		if (g_store.messages[i].id != 0) {
			if (!rx_only || g_store.messages[i].direction == 0) {
				count++;
			}
		}
	}
	
	k_mutex_unlock(&g_store.lock);
	return count;
}

/* Get next unread message */
struct msg_entry *msg_store_get_unread(void)
{
	struct msg_entry *msg = NULL;
	
	k_mutex_lock(&g_store.lock, K_FOREVER);
	
	for (int i = 0; i < MSG_STORE_MAX_MESSAGES; i++) {
		if (g_store.messages[i].id != 0 &&
		    g_store.messages[i].direction == 0 &&  /* RX */
		    !(g_store.messages[i].flags & MSG_FLAG_READ)) {
			msg = &g_store.messages[i];
			break;
		}
	}
	
	k_mutex_unlock(&g_store.lock);
	return msg;
}

/* Mark message as read */
int msg_store_mark_read(uint16_t id)
{
	int ret = -ENOENT;
	
	k_mutex_lock(&g_store.lock, K_FOREVER);
	
	for (int i = 0; i < MSG_STORE_MAX_MESSAGES; i++) {
		if (g_store.messages[i].id == id) {
			g_store.messages[i].flags |= MSG_FLAG_READ;
			ret = 0;
			break;
		}
	}
	
	k_mutex_unlock(&g_store.lock);
	return ret;
}

/* Mark TX message as sent */
int msg_store_mark_sent(uint16_t id, bool success)
{
	int ret = -ENOENT;
	
	k_mutex_lock(&g_store.lock, K_FOREVER);
	
	for (int i = 0; i < MSG_STORE_MAX_MESSAGES; i++) {
		if (g_store.messages[i].id == id) {
			g_store.messages[i].flags &= ~MSG_FLAG_QUEUED;
			if (success) {
				g_store.messages[i].flags |= MSG_FLAG_SENT;
			} else {
				g_store.messages[i].flags |= MSG_FLAG_FAILED;
			}
			/* Signal TX completion */
			k_sem_give(&g_store.messages[i].tx_sem);
			ret = 0;
			break;
		}
	}
	
	k_mutex_unlock(&g_store.lock);
	return ret;
}

/* Get next queued TX message */
struct msg_entry *msg_store_get_tx_queued(void)
{
	struct msg_entry *msg = NULL;
	
	k_mutex_lock(&g_store.lock, K_FOREVER);
	
	for (int i = 0; i < MSG_STORE_MAX_MESSAGES; i++) {
		if (g_store.messages[i].id != 0 &&
		    g_store.messages[i].direction == 1 &&  /* TX */
		    (g_store.messages[i].flags & MSG_FLAG_QUEUED)) {
			msg = &g_store.messages[i];
			break;
		}
	}
	
	k_mutex_unlock(&g_store.lock);
	return msg;
}

/* Clean up old messages */
int msg_store_cleanup(uint32_t max_age_ms)
{
	int count = 0;
	uint32_t now = k_uptime_get_32();
	
	k_mutex_lock(&g_store.lock, K_FOREVER);
	
	for (int i = 0; i < MSG_STORE_MAX_MESSAGES; i++) {
		if (g_store.messages[i].id != 0 &&
		    (now - g_store.messages[i].timestamp) > max_age_ms) {
			g_store.messages[i].id = 0;  /* Mark as free */
			count++;
		}
	}
	
	k_mutex_unlock(&g_store.lock);
	
	if (count > 0) {
		LOG_DBG("Cleaned up %d old messages", count);
	}
	
	return count;
}

/* Get statistics */
void msg_store_get_stats(uint32_t *rx_count, uint32_t *tx_count, uint32_t *dropped_count)
{
	k_mutex_lock(&g_store.lock, K_FOREVER);
	
	if (rx_count) *rx_count = g_store.rx_count;
	if (tx_count) *tx_count = g_store.tx_count;
	if (dropped_count) *dropped_count = g_store.dropped_count;
	
	k_mutex_unlock(&g_store.lock);
}