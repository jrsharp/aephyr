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
 * @brief Callback for each successfully decoded DECT NR+ PHY data frame.
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

/** DECT NR+ PHY broadcast receiver identity (accepted by every RX filter). */
#define DECT_PHY_BROADCAST_ID 0xFFFFU

/** PHY receiver identity the HONR root (address 0x0000) filters/receives on.
 * The modem treats receiver_identity == 0 as "broadcast-only" (it does NOT
 * deliver addressed Type-2 frames to a node filtering on 0), so the root -- and
 * addressed unicast toward it -- must use a reserved non-zero id instead. 0xFFFE
 * is not the broadcast id (0xFFFF), not the 0 sentinel, and not a valid HONR node
 * address (nibbles 0xF are invalid; HONR_CHILD_MAX is 0xE), so it can never
 * collide with a real node's routing address. The driver maps HONR 0x0000 <->
 * this id at the PHY boundary only; mesh/HeyMac addresses stay 0x0000. */
#define DECT_PHY_ROOT_ID 0xFFFEU

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
	uint32_t tx_bytes;    /**< Payload bytes in successfully transmitted frames. */
	uint32_t rx_bytes;    /**< Payload bytes in decoded received data frames. */
	int16_t last_rssi_dbm; /**< RSSI of the most recently received frame, dBm. */
	int8_t last_snr_db;    /**< SNR of the most recently received frame, dB. */
	int16_t last_temp_c;   /**< Modem temperature at the last operation, deg C
				 *   (NRF_MODEM_DECT_PHY_TEMP_NOT_MEASURED = 999 if none yet). */
	uint16_t last_voltage_mv; /**< Modem supply voltage at the last operation, mV
				    *   (0 = not measured). */
};

/** @brief Snapshot the PHY statistics. */
void dect_phy_get_stats(struct dect_phy_stats *out);

/**
 * @brief Read current PHY identity and radio settings.
 *
 * Any output pointer may be NULL.
 */
void dect_phy_get_info(uint16_t *device_id, uint16_t *carrier, int8_t *tx_power);

/**
 * @brief Format a carrier's band + centre frequency as human-readable text.
 *
 * Looks the band up live from the modem's reported band table and applies the
 * ETSI channel raster (Fc[kHz] = 450144 + carrier*864), e.g. "band 4, 914.976 MHz".
 * Falls back to "carrier N, F MHz" if the carrier is outside every reported band.
 *
 * @param carrier DECT NR+ carrier number.
 * @param buf     Output buffer.
 * @param len     Size of @p buf (>= 24 recommended).
 */
void dect_phy_format_carrier(uint16_t carrier, char *buf, size_t len);

/** @brief A supported RF band as reported by the modem (via band_get). */
struct dect_phy_band_info {
	uint8_t  band_number;      /**< DECT NR+ band number (e.g. 9, 22). */
	uint8_t  band_group_index; /**< 0 = ~2 GHz, 1 = ~1 GHz. */
	uint8_t  power_class;      /**< Transmit power class. */
	int8_t   rx_gain;          /**< Receiver sensitivity capability, dB. */
	uint16_t min_carrier;      /**< Lowest valid carrier (channel) number. */
	uint16_t max_carrier;      /**< Highest valid carrier (channel) number. */
};

/**
 * @brief Copy the modem-reported supported bands (captured during init).
 *
 * Lets a caller confirm the exact carrier range and power class of each band
 * on-device before transmitting.
 *
 * @param out Destination array, or NULL to just query the count.
 * @param max Capacity of @p out.
 * @return Number of band entries available (clamped to @p max when copying).
 */
uint8_t dect_phy_get_bands(struct dect_phy_band_info *out, uint8_t max);

#endif /* AETHER_DRIVERS_DECT_PHY_H_ */
