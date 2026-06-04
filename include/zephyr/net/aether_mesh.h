/*
 * Copyright (c) 2025 Æther Authors
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ZEPHYR_INCLUDE_NET_AETHER_MESH_H_
#define ZEPHYR_INCLUDE_NET_AETHER_MESH_H_

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net/heymac.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Æther Mesh Frame Control Flags */
#define AETHER_FCTL_MESH_NET_ID  0x01  /* Mesh network ID present */
#define AETHER_FCTL_EXTENDED     0x02  /* Extended format (reserved) */
#define AETHER_FCTL_COMPRESSED   0x04  /* Compressed addresses (reserved) */
#define AETHER_FCTL_RESERVED     0xF8  /* Reserved for future use */

/* Protocol IDs */
#define AETHER_PROTO_DATA   0x01
#define AETHER_PROTO_CTRL   0x02
#define AETHER_PROTO_STYX   0x03
#define AETHER_PROTO_SEC    0x04

/* Priority Levels */
#define AETHER_PRIORITY_LOW    0
#define AETHER_PRIORITY_NORMAL 1
#define AETHER_PRIORITY_HIGH   2

/* Delivery Guarantees */
#define AETHER_DELIV_BESTEFFORT 0
#define AETHER_DELIV_RELIABLE   1

/* qos_security layout: Priority(2) | Delivery(1) | Security(4) | Reserved(1).
 * The reserved low bit carries the reliable-datagram flow-start marker
 * (§4.6): set on the first segment of a fresh per-peer sequence space. */
#define AETHER_QOS_FLOW_START       0x01
#define AETHER_QOS_IS_RELIABLE(q)   (((q) >> 5) & 0x01)
#define AETHER_QOS_IS_FLOW_START(q) ((q) & AETHER_QOS_FLOW_START)

/* Security Flags */
#define AETHER_SEC_FLAG_NONE      0x00
#define AETHER_SEC_FLAG_SIGNED    0x01
#define AETHER_SEC_FLAG_ENCRYPTED 0x02
#define AETHER_SEC_FLAG_CGA       0x04

/* Message Types */
#define AETHER_MSG_DATA          0x00
#define AETHER_MSG_ACK           0x01
#define AETHER_MSG_RREQ          0x02  /* Route request */
#define AETHER_MSG_RREP          0x03  /* Route reply */
#define AETHER_MSG_RERR          0x04  /* Route error */
#define AETHER_MSG_HELLO         0x05  /* Neighbor discovery */
#define AETHER_MSG_TREE_ANNOUNCE 0x06
#define AETHER_MSG_TREE_JOIN_REQ 0x07
#define AETHER_MSG_TREE_JOIN_REP 0x08
#define AETHER_MSG_NET_ANNOUNCE  0x09
#define AETHER_MSG_TIME_SYNC     0x0A
#define AETHER_MSG_KEY_EXCHANGE  0x0B
#define AETHER_MSG_MULTICAST     0x0C
#define AETHER_MSG_EMERGENCY     0x0D

/* Limits */
#define AETHER_MAX_NODES   128
#define AETHER_MAX_HOPS    15
#ifndef CONFIG_AETHER_MAX_PAYLOAD
#define AETHER_MAX_PAYLOAD 200
#else
#define AETHER_MAX_PAYLOAD CONFIG_AETHER_MAX_PAYLOAD
#endif

/* Æther mesh header structure */
struct aether_mesh_hdr {
	uint8_t flags;           /* Frame control flags */
	uint8_t type;           /* Message type */
	uint8_t ttl;            /* Time to live / hop count */
	uint8_t src[6];         /* Source address */
	uint8_t dst[6];         /* Destination address */
	uint8_t seq_no;         /* Sequence number */
	uint16_t mesh_net_id;   /* Conditional: only when flags & MESH_NET_ID */
	uint8_t qos_security;   /* Priority(2) | Delivery(1) | Security(4) | Reserved(1) */
	uint16_t length;        /* Payload length */
};

/* Neighbor entry */
struct aether_neighbor {
	uint8_t addr[6];        /* Neighbor address */
	int8_t rssi;           /* Signal strength */
	uint8_t lqi;           /* Link quality */
	uint32_t last_seen;    /* Timestamp of last contact */
	uint16_t seq_no;       /* Last sequence number */
	bool active;           /* Neighbor is active */
};

/* Route table entry */
struct aether_route {
	uint8_t dest[6];       /* Destination address */
	uint8_t next_hop[6];   /* Next hop address */
	uint8_t hop_count;     /* Number of hops */
	uint32_t last_used;    /* Timestamp of last use */
	uint32_t expires;      /* Route expiration time */
	bool active;           /* Route is active */
};

/**
 * @brief Callback for received mesh data
 *
 * @param iface Network interface
 * @param src Source address (6 bytes)
 * @param data Received data
 * @param len Data length
 * @param user_data User-provided context
 */
typedef void (*aether_mesh_recv_cb_t)(struct net_if *iface,
				      const uint8_t src[6],
				      const uint8_t *data, size_t len,
				      void *user_data);

/* Æther mesh context */
struct aether_mesh_ctx {
	struct net_if *iface;
	uint16_t mesh_net_id;
	uint8_t local_addr[6];
	uint8_t seq_no;

	/* Application receive callback */
	aether_mesh_recv_cb_t recv_cb;
	void *recv_cb_user_data;

	/* Neighbor table */
	struct aether_neighbor neighbors[CONFIG_AETHER_MAX_NEIGHBORS];
	struct k_mutex neighbor_lock;

	/* Routing table */
	struct aether_route routes[CONFIG_AETHER_MAX_ROUTES];
	struct k_mutex route_lock;

	/* Work queues and threads */
	struct k_work_q mesh_workq;
	struct k_work_delayable hello_work;
	struct k_work_delayable route_maintenance_work;

	/* Statistics */
	uint32_t hello_sent;
	uint32_t hello_received;
	uint32_t route_requests;
	uint32_t route_replies;
	uint32_t packets_forwarded;
	uint32_t packets_dropped;
};

/* Æther mesh API functions */

/**
 * @brief Initialize Æther mesh layer
 *
 * @param iface Network interface (must have HeyMac L2)
 * @param mesh_net_id Mesh network ID
 * @return 0 on success, negative error code otherwise
 */
int aether_mesh_init(struct net_if *iface, uint16_t mesh_net_id);

/**
 * @brief Send data through mesh network
 *
 * @param iface Network interface
 * @param dst Destination address
 * @param data Payload data
 * @param len Payload length
 * @param priority Message priority
 * @return 0 on success, negative error code otherwise
 */
int aether_mesh_send(struct net_if *iface, const uint8_t dst[6],
                     const uint8_t *data, size_t len, uint8_t priority);

/**
 * @brief Send hello message for neighbor discovery
 *
 * @param iface Network interface
 * @return 0 on success, negative error code otherwise
 */
int aether_mesh_send_hello(struct net_if *iface);

/**
 * @brief Get neighbor count
 *
 * @param iface Network interface
 * @return Number of active neighbors
 */
int aether_mesh_get_neighbor_count(struct net_if *iface);

/**
 * @brief Get route count
 *
 * @param iface Network interface
 * @return Number of active routes
 */
int aether_mesh_get_route_count(struct net_if *iface);

/**
 * @brief Find route to destination
 *
 * @param iface Network interface
 * @param dst Destination address
 * @param next_hop Next hop address buffer (output)
 * @return 0 if route found, negative error code otherwise
 */
int aether_mesh_find_route(struct net_if *iface, const uint8_t dst[6], 
                           uint8_t next_hop[6]);

/**
 * @brief Add or update neighbor
 *
 * @param iface Network interface
 * @param addr Neighbor address
 * @param rssi Signal strength
 * @param lqi Link quality
 * @return 0 on success, negative error code otherwise
 */
int aether_mesh_update_neighbor(struct net_if *iface, const uint8_t addr[6],
                                int8_t rssi, uint8_t lqi);

/**
 * @brief Add or update route
 *
 * @param iface Network interface
 * @param dest Destination address
 * @param next_hop Next hop address
 * @param hop_count Number of hops
 * @return 0 on success, negative error code otherwise
 */
int aether_mesh_add_route(struct net_if *iface, const uint8_t dest[6],
                          const uint8_t next_hop[6], uint8_t hop_count);

/**
 * @brief Handle received mesh packet
 *
 * @param iface Network interface
 * @param pkt Received packet
 */
void aether_mesh_recv(struct net_if *iface, struct net_pkt *pkt);

/**
 * @brief Register callback for received mesh data
 *
 * @param iface Network interface
 * @param cb Callback function
 * @param user_data User context passed to callback
 * @return 0 on success, negative error code otherwise
 */
int aether_mesh_register_recv_callback(struct net_if *iface,
				       aether_mesh_recv_cb_t cb,
				       void *user_data);

/**
 * @brief Build Aether mesh header
 *
 * @param hdr Header structure
 * @param buf Output buffer
 * @param len Buffer length
 * @return Header size on success, negative error code otherwise
 */
int aether_mesh_build_header(const struct aether_mesh_hdr *hdr,
                             uint8_t *buf, size_t len);

/**
 * @brief Calculate mesh header size based on flags
 *
 * @param flags Mesh header flags
 * @return Header size in bytes
 */
static inline size_t aether_mesh_hdr_size(uint8_t flags)
{
	size_t size = 16;  /* Base header size */
	if (flags & AETHER_FCTL_MESH_NET_ID) {
		size += 2;  /* Add mesh network ID */
	}
	return size;
}

/**
 * @brief Extract priority from QoS field
 *
 * @param qos_security QoS/Security byte
 * @return Priority level (0-2)
 */
static inline uint8_t aether_mesh_get_priority(uint8_t qos_security)
{
	return (qos_security >> 6) & 0x03;
}

/**
 * @brief Extract delivery guarantee from QoS field
 *
 * @param qos_security QoS/Security byte
 * @return Delivery guarantee (0=best effort, 1=reliable)
 */
static inline uint8_t aether_mesh_get_delivery(uint8_t qos_security)
{
	return (qos_security >> 5) & 0x01;
}

/**
 * @brief Extract security flags from QoS field
 *
 * @param qos_security QoS/Security byte
 * @return Security flags (4 bits)
 */
static inline uint8_t aether_mesh_get_security(uint8_t qos_security)
{
	return (qos_security >> 1) & 0x0F;
}

/**
 * @brief Send data reliably with mesh-layer retries
 *
 * Blocks until message is sent successfully or all retries exhausted.
 * Uses synchronous TX path with configurable retry count.
 *
 * @param iface Network interface
 * @param dst Destination address (6 bytes, use ff:ff:ff:ff:ff:ff for broadcast)
 * @param data Data to send
 * @param len Data length
 * @param max_retries Maximum number of TX attempts
 * @return 0 on success, negative error code otherwise
 */
int aether_mesh_send_reliable(struct net_if *iface, const uint8_t dst[6],
			      const uint8_t *data, size_t len, int max_retries);

/**
 * @brief Low-level synchronous LoRa send with retries (driver level)
 *
 * @param data Raw data to transmit
 * @param len Data length
 * @param max_retries Maximum retry attempts
 * @return 0 on success, negative error code otherwise
 */
int aether_lora_send_sync(const uint8_t *data, size_t len, int max_retries);

/**
 * @brief Pause TX thread to keep radio in RX mode
 *
 * Called by 9P transport before waiting for a response. Prevents
 * hello beacons from stealing the radio during critical RX windows.
 */
void aether_lora_tx_pause(void);

/**
 * @brief Resume TX thread after response received or timeout
 */
void aether_lora_tx_resume(void);

/**
 * @brief Run CAD diagnostic test
 *
 * Pauses TX, stops RX, runs count CAD checks, restarts RX, resumes TX.
 * Results returned via out parameters.
 *
 * @param count Number of CAD checks to run
 * @param out_clear Number of "channel clear" results (may be NULL)
 * @param out_busy Number of "activity detected" results (may be NULL)
 * @param out_err Number of errors/timeouts (may be NULL)
 * @return 0 on success, negative if driver not running
 */
int aether_lora_cad_test(int count, int *out_clear, int *out_busy, int *out_err);

/* ---- Reliable-datagram glue (§4.6).  Defined in aether_route.c, which
 * drives the pure aether_reliable engine and provides its net_pkt I/O. ---- */

/** The mesh network interface (NULL until aether_mesh_init). Lets callers
 *  that don't already hold the iface (e.g. the /net/aether server) reach it. */
struct net_if *aether_mesh_iface(void);

/** Our 6-byte mesh address (NULL until aether_mesh_init). */
const uint8_t *aether_mesh_local_addr(void);

/** Send a unicast reliable datagram via the §4.6 ARQ engine (ordered,
 *  acknowledged, retransmitted). The path /net/aether reliable conversations
 *  use. Returns 0, -EAGAIN if the peer window is full, or an error. */
int aether_mesh_reliable_send(struct net_if *iface, const uint8_t dst[6],
			      const uint8_t *data, size_t len);

/** Callback for an in-order reliable datagram delivered by the §4.6 engine.
 *  Distinct from the best-effort recv callback (aether_mesh_register_recv_
 *  callback): reliable deliveries route here when a handler is registered. */
typedef void (*aether_reliable_recv_cb_t)(const uint8_t src[6],
					  const uint8_t *data, uint16_t len,
					  void *user);

/** Register the reliable-delivery handler (e.g. the /net/aether server).
 *  Pass cb=NULL to clear and fall back to the best-effort recv callback. */
void aether_reliable_set_recv_cb(aether_reliable_recv_cb_t cb, void *user);

/** Initialize the reliable engine + its periodic tick.  Call once after the
 *  mesh context is up (g_mesh_ctx set). */
void aether_reliable_glue_init(void);

/** Feed a received reliable DATA segment (already stripped of the mesh
 *  header) into the engine.  flow_start is the decoded qos flow-start bit. */
void aether_reliable_glue_rx_data(const uint8_t src[6], uint8_t seq,
				  bool flow_start, const uint8_t *payload,
				  uint16_t len);

/** Feed a received ACK (type 0x01) payload into the engine. */
void aether_reliable_glue_rx_ack(const uint8_t src[6],
				 const uint8_t *ack_bytes, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_NET_AETHER_MESH_H_ */