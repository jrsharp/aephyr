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

/** @brief Register the receive callback (NULL to disable). */
void dect_phy_set_rx_cb(dect_phy_rx_cb_t cb);

/** @brief Set the carrier (channel) used by subsequent operations. */
void dect_phy_set_carrier(uint16_t carrier);

/** @brief Set the transmit power index used by subsequent transmissions. */
void dect_phy_set_tx_power(int8_t power);

/**
 * @brief Transmit one frame with listen-before-talk, blocking until complete.
 *
 * @param data Payload to send.
 * @param len  Payload length in bytes.
 * @retval 0       on success.
 * @retval -EBUSY  LBT found the channel busy (transmission was not sent).
 * @retval <0      other modem error.
 */
int dect_phy_tx(const uint8_t *data, size_t len);

/**
 * @brief Open a continuous receive window, blocking until it elapses.
 *
 * Decoded frames are delivered to the registered receive callback.
 *
 * @param duration_ms Receive window duration in milliseconds.
 * @retval 0 on success, negative errno otherwise.
 */
int dect_phy_rx_window(uint32_t duration_ms);

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
	uint32_t rx_frames;   /**< Data frames received. */
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
