/*
 * Copyright (c) 2025 Æther Authors
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ZEPHYR_INCLUDE_NET_HEYMAC_H_
#define ZEPHYR_INCLUDE_NET_HEYMAC_H_

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/device.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* HeyMac Frame Control Bits */
#define HEYMAC_FCTL_L   0x80  /* Long addressing */
#define HEYMAC_FCTL_N   0x40  /* Network ID present */
#define HEYMAC_FCTL_D   0x20  /* Destination present */
#define HEYMAC_FCTL_S   0x10  /* Source present */
#define HEYMAC_FCTL_I   0x08  /* Information Elements */
#define HEYMAC_FCTL_M   0x04  /* MAC payload present */
#define HEYMAC_FCTL_P   0x02  /* Pending frame */
#define HEYMAC_FCTL_X   0x01  /* Extended header */

/* HeyMac Frame Types */
#define HEYMAC_FRM_TYPE_BEACON  0x00
#define HEYMAC_FRM_TYPE_DATA    0x01
#define HEYMAC_FRM_TYPE_ACK     0x02
#define HEYMAC_FRM_TYPE_MACCMD  0x03

/* HeyMac Constants.
 * Frame/payload maxima are sized for the DECT NR+ PHY transport block (sized
 * per-frame from the TBS tables), not the SX1262's 255-byte LoRa packet limit.
 * 512 covers the largest block we transmit at MCS-1 (495 B). */
#define HEYMAC_VERSION         0x01
#define HEYMAC_MAX_FRAME_SIZE  512
#define HEYMAC_MAX_PAYLOAD     480
#define HEYMAC_ADDR_SHORT_LEN  2
#define HEYMAC_ADDR_LONG_LEN   6

/* Default configuration values */
#ifndef CONFIG_HEYMAC_DEFAULT_NET_ID
#define CONFIG_HEYMAC_DEFAULT_NET_ID    0x915E
#endif

#ifndef CONFIG_HEYMAC_BEACON_INTERVAL_MS
#define CONFIG_HEYMAC_BEACON_INTERVAL_MS    1000
#endif

#ifndef CONFIG_HEYMAC_USE_SHORT_ADDR
#define CONFIG_HEYMAC_USE_SHORT_ADDR    0
#endif

/* HeyMac address structure */
struct heymac_addr {
	uint8_t type;     /* 0=short, 1=long */
	union {
		uint8_t short_addr[HEYMAC_ADDR_SHORT_LEN];
		uint8_t long_addr[HEYMAC_ADDR_LONG_LEN];
	};
};

/* HeyMac frame header */
struct heymac_hdr {
	uint8_t version_type;  /* Version (upper 4) | Type (lower 4) */
	uint8_t frame_ctrl;    /* Frame control flags */
	uint16_t net_id;       /* Network ID (conditional) */
	struct heymac_addr dst_addr;  /* Destination address (conditional) */
	struct heymac_addr src_addr;  /* Source address (conditional) */
	uint8_t seq_no;        /* Sequence number */
};

/* HeyMac L2 API - similar to ieee802154_radio_api */
struct heymac_radio_api {
	/* Standard net_if_api must be first */
	struct net_if_api iface_api;

	/* HeyMac specific radio operations */
	int (*set_channel)(const struct device *dev, uint16_t channel);
	int (*set_power)(const struct device *dev, int8_t power);
	int (*set_network_id)(const struct device *dev, uint16_t net_id);
	int (*start)(const struct device *dev);
	int (*stop)(const struct device *dev);
	int (*tx)(const struct device *dev, struct net_pkt *pkt);
};

/* HeyMac states for HSM */
enum heymac_state {
    HEYMAC_STATE_INITIAL,
    HEYMAC_STATE_INITIALIZING,
    HEYMAC_STATE_LURKING,
    HEYMAC_STATE_BEACONING,
    HEYMAC_STATE_LINKING,
};

/* HeyMac events */
enum heymac_event {
    HEYMAC_EVENT_INIT,
    HEYMAC_EVENT_INIT_COMPLETE,
    HEYMAC_EVENT_BEACON_TIMEOUT,
    HEYMAC_EVENT_FRAME_RECEIVED,
    HEYMAC_EVENT_NEIGHBOR_FOUND,
    HEYMAC_EVENT_NEIGHBOR_LOST,
    HEYMAC_EVENT_LINK_TIMEOUT,
};

/* HeyMac context for network interface */
struct heymac_context {
	struct net_if *iface;
	enum heymac_state state;
	struct k_mutex tx_lock;
	struct k_mutex state_mutex;
	struct k_work_delayable beacon_work;
	struct k_work_delayable timeout_work;
	
	uint16_t net_id;
	struct heymac_addr local_addr;
	struct heymac_addr last_rx_src;  /* immediate sender of last RX frame (HONR resender) */
	int8_t last_rx_rssi;             /* real RSSI (dBm) of last RX frame, from the PHY */
	int8_t last_rx_snr;              /* real SNR (dB) of last RX frame, from the PHY */
	uint8_t tx_seq_no;
	
	/* Statistics */
	uint32_t rx_packets;
	uint32_t tx_packets;
	uint32_t rx_beacons;
	uint32_t tx_beacons;
	uint32_t rx_errors;
	uint32_t tx_errors;
};

/* HeyMac API functions */

/**
 * @brief Initialize HeyMac layer for network interface
 *
 * @param iface Network interface
 * @return 0 on success, negative error code otherwise
 */
int heymac_init(struct net_if *iface);

/**
 * @brief Set local address
 *
 * @param iface Network interface
 * @param addr New address
 * @return 0 on success, negative error code otherwise
 */
int heymac_set_addr(struct net_if *iface, const struct heymac_addr *addr);

/**
 * @brief Get local address
 *
 * @param iface Network interface
 * @param addr Address buffer
 * @return 0 on success, negative error code otherwise
 */
int heymac_get_addr(struct net_if *iface, struct heymac_addr *addr);

/**
 * @brief Set network ID
 *
 * @param iface Network interface  
 * @param net_id Network ID
 * @return 0 on success, negative error code otherwise
 */
int heymac_set_network_id(struct net_if *iface, uint16_t net_id);

/**
 * @brief Get network ID
 *
 * @param iface Network interface
 * @return Network ID, or negative error code
 */
int heymac_get_network_id(struct net_if *iface);

/**
 * @brief Parse HeyMac header from buffer
 *
 * @param buf Input buffer
 * @param len Buffer length
 * @param hdr Header structure to fill
 * @return Header size on success, negative error code otherwise
 */
int heymac_parse_header(const uint8_t *buf, size_t len, 
                        struct heymac_hdr *hdr);

/**
 * @brief Build HeyMac header
 *
 * @param hdr Header structure
 * @param buf Output buffer
 * @param len Buffer length
 * @return Header size on success, negative error code otherwise
 */
int heymac_build_header(const struct heymac_hdr *hdr, 
                        uint8_t *buf, size_t len);

/**
 * @brief Handle received packet (called from radio driver)
 *
 * @param iface Network interface
 * @param pkt Received packet
 */
void heymac_recv(struct net_if *iface, struct net_pkt *pkt);

/**
 * @brief State machine execution
 *
 * @param ctx HeyMac context
 * @param event Event to process
 */
void heymac_state_machine_run(struct heymac_context *ctx, enum heymac_event event);

/**
 * @brief Send beacon frame
 *
 * @param ctx HeyMac context
 * @return 0 on success, negative error code otherwise
 */
int heymac_send_beacon(struct heymac_context *ctx);

/**
 * @brief Schedule timeout work
 *
 * @param ctx HeyMac context
 * @param timeout_ms Timeout in milliseconds
 */
void heymac_schedule_timeout(struct heymac_context *ctx, uint32_t timeout_ms);

/**
 * @brief Schedule beacon work
 *
 * @param ctx HeyMac context
 * @param interval_ms Interval in milliseconds
 */
void heymac_schedule_beacon(struct heymac_context *ctx, uint32_t interval_ms);

/**
 * @brief Cancel scheduled timers
 *
 * @param ctx HeyMac context
 */
void heymac_cancel_timers(struct heymac_context *ctx);

/**
 * @brief Initialize HeyMac interface
 *
 * @param iface Network interface
 */
void heymac_iface_init(struct net_if *iface);

/* Helper functions */
static inline bool heymac_addr_is_short(const struct heymac_addr *addr)
{
	return addr->type == 0;
}

static inline bool heymac_addr_is_long(const struct heymac_addr *addr)
{
	return addr->type == 1;
}

static inline size_t heymac_addr_len(const struct heymac_addr *addr)
{
	return heymac_addr_is_short(addr) ? HEYMAC_ADDR_SHORT_LEN : HEYMAC_ADDR_LONG_LEN;
}

/* Layer 2 definitions for Zephyr network stack */
#define HEYMAC_L2 HEYMAC
NET_L2_DECLARE_PUBLIC(HEYMAC_L2);

#ifdef CONFIG_NET_L2_HEYMAC
#define HEYMAC_L2_CTX_TYPE struct heymac_context
#endif

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_NET_HEYMAC_H_ */