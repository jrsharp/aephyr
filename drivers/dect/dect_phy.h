/*
 * Copyright (c) 2026 Æther Authors
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AETHER_DRIVERS_DECT_PHY_H_
#define AETHER_DRIVERS_DECT_PHY_H_

#include <stdint.h>
#include <stddef.h>

/**
 * @file
 * @brief Thin wrapper around the DECT NR+ PHY modem API.
 *
 * Provides the radio primitives the Æther DECT net-device driver needs:
 * init/configure/activate, a blocking listen-before-talk transmit, a blocking
 * receive window that delivers decoded frames to a callback, and a one-shot
 * channel RSSI measurement for CAD. The modem is half-duplex and single-radio;
 * the driver serializes transmit and receive-window operations.
 */

/**
 * @brief Callback for each successfully decoded DECT NR+ data frame.
 *
 * Invoked from the modem event-handler context. Keep it short and non-blocking
 * (queue the frame for a thread).
 *
 * @param data     Received payload (valid only during the call).
 * @param len      Payload length in bytes.
 * @param rssi_dbm Link RSSI in dBm.
 * @param snr_db   Link SNR in dB.
 * @param tx_id    PHY transmitter ID of the immediate sender (last hop).
 */
typedef void (*dect_phy_rx_cb_t)(const uint8_t *data, size_t len, int16_t rssi_dbm,
				 int8_t snr_db, uint16_t tx_id);

/**
 * @brief Initialize the modem and bring up the DECT NR+ PHY.
 *
 * @param device_id Output: PHY device ID derived from the hardware ID.
 * @retval 0 on success, negative errno otherwise.
 */
int dect_phy_init(uint16_t *device_id);

/** DECT NR+ broadcast receiver identity (accepted by every RX filter). */
#define DECT_PHY_BROADCAST_ID 0xFFFFU

/** @brief Register the receive callback (NULL to disable). */
void dect_phy_set_rx_cb(dect_phy_rx_cb_t cb);

/**
 * @brief Set this node's 16-bit PHY identity.
 *
 * Once set, the identity is used as the transmitter ID and as the RX hardware
 * filter (the modem accepts only frames whose receiver identity matches this
 * value or the broadcast ID), and transmissions use the addressed Type 2 PHY
 * header. If never set, transmissions use the broadcast Type 1 header and the
 * RX filter is disabled (promiscuous). An identity of 0 (the HONR root) also
 * leaves the RX filter disabled.
 */
void dect_phy_set_node_id(uint16_t node_id);

/** @brief Set the carrier (channel) used by subsequent operations. */
void dect_phy_set_carrier(uint16_t carrier);

/** @brief Set the transmit power index used by subsequent transmissions. */
void dect_phy_set_tx_power(int8_t power);

/**
 * @brief Transmit one frame with listen-before-talk, blocking until complete.
 *
 * @param receiver_id Destination PHY identity for the addressed Type 2 header,
 *                    or DECT_PHY_BROADCAST_ID for broadcast. Ignored (broadcast
 *                    Type 1 header) when no node identity has been set.
 * @param data        Payload to send.
 * @param len         Payload length in bytes.
 * @retval 0       on success.
 * @retval -EBUSY  LBT found the channel busy (transmission was not sent).
 * @retval <0      other modem error.
 */
int dect_phy_tx(uint16_t receiver_id, const uint8_t *data, size_t len);

/**
 * @brief Ensure the radio is listening, arming a continuous receive if it is not.
 *
 * Receive runs continuously in the background and is only interrupted to
 * transmit (see @ref dect_phy_tx, which stops and resumes it automatically).
 * The radio thread calls this periodically to re-arm receive after the
 * operation's duration elapses. Decoded frames are delivered to the registered
 * receive callback. Idempotent: a no-op while a receive is already live.
 *
 * @retval 0 on success, negative errno otherwise.
 */
int dect_phy_rx_ensure(void);

/**
 * @brief Stop the continuous receive operation and wait for the radio to idle.
 *
 * Cancels any live receive so the radio is free to transmit or measure. Bounded
 * wait; safe to call when no receive is active.
 *
 * @retval 0 on success, negative errno otherwise.
 */
int dect_phy_rx_stop(void);

/**
 * @brief Measure peak channel RSSI over a short window (for CAD).
 *
 * @param rssi_dbm_out Output: peak measured RSSI in dBm.
 * @retval 0 on success, negative errno otherwise.
 */
int dect_phy_channel_rssi(int8_t *rssi_dbm_out);

/** @brief DECT NR+ PHY operational statistics. */
struct dect_phy_stats {
	uint32_t tx_ok;       /**< Frames transmitted successfully. */
	uint32_t tx_busy;     /**< Transmits skipped because LBT found the channel busy. */
	uint32_t tx_err;      /**< Transmits that failed for other reasons. */
	uint32_t rx_pcc;      /**< Control headers (PCC) detected off-air. */
	uint32_t rx_frames;   /**< Data frames (PDC) received. */
	int16_t last_rssi_dbm; /**< RSSI of the most recently received frame, dBm. */
	int8_t last_snr_db;    /**< SNR of the most recently received frame, dB. */
};

/** @brief Snapshot the PHY statistics. */
void dect_phy_get_stats(struct dect_phy_stats *out);

/**
 * @brief Read current PHY identity and radio settings.
 *
 * Any output pointer may be NULL.
 */
void dect_phy_get_info(uint16_t *device_id, uint16_t *carrier, int8_t *tx_power);

#endif /* AETHER_DRIVERS_DECT_PHY_H_ */
