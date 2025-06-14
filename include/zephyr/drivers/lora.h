/*
 * Copyright (c) 2025 Æther Authors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_LORA_H_
#define ZEPHYR_INCLUDE_DRIVERS_LORA_H_

#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief LoRa modem configuration structure
 */
struct lora_modem_config {
	uint32_t frequency;		/* Frequency in Hz */
	uint8_t bandwidth;		/* Bandwidth (0-9) */
	uint8_t datarate;		/* Spreading factor (6-12) */
	uint8_t coderate;		/* Coding rate (1-4) */
	int8_t tx_power;		/* TX power in dBm */
	uint16_t preamble;		/* Preamble length */
	bool public_network;		/* Public network mode */
	bool crc;			/* CRC enable */
};

/**
 * @brief LoRa receive callback function type
 * 
 * @param dev LoRa device
 * @param data Received data buffer
 * @param size Size of received data
 * @param rssi RSSI value in dBm
 * @param snr SNR value in dB
 */
typedef void (*lora_recv_cb_t)(const struct device *dev, uint8_t *data, 
			       uint16_t size, int16_t rssi, int8_t snr);

/**
 * @brief LoRa driver API structure
 */
__subsystem struct lora_driver_api {
	int (*config)(const struct device *dev, struct lora_modem_config *config);
	int (*send)(const struct device *dev, uint8_t *data, uint32_t data_len);
	int (*recv_async)(const struct device *dev, lora_recv_cb_t cb);
	int (*test)(const struct device *dev);
};

/**
 * @brief Configure LoRa modem
 * 
 * @param dev LoRa device
 * @param config Modem configuration
 * @return 0 on success, negative errno on failure
 */
static inline int lora_config(const struct device *dev, 
			      struct lora_modem_config *config)
{
	const struct lora_driver_api *api = 
		(const struct lora_driver_api *)dev->api;

	if (!api->config) {
		return -ENOSYS;
	}

	return api->config(dev, config);
}

/**
 * @brief Send data via LoRa
 * 
 * @param dev LoRa device
 * @param data Data buffer to send
 * @param data_len Length of data
 * @return 0 on success, negative errno on failure
 */
static inline int lora_send(const struct device *dev, uint8_t *data, 
			    uint32_t data_len)
{
	const struct lora_driver_api *api = 
		(const struct lora_driver_api *)dev->api;

	if (!api->send) {
		return -ENOSYS;
	}

	return api->send(dev, data, data_len);
}

/**
 * @brief Setup asynchronous receive callback
 * 
 * @param dev LoRa device
 * @param cb Callback function for received packets
 * @return 0 on success, negative errno on failure
 */
static inline int lora_recv_async(const struct device *dev, lora_recv_cb_t cb)
{
	const struct lora_driver_api *api = 
		(const struct lora_driver_api *)dev->api;

	if (!api->recv_async) {
		return -ENOSYS;
	}

	return api->recv_async(dev, cb);
}

/**
 * @brief Test LoRa device
 * 
 * @param dev LoRa device
 * @return 0 on success, negative errno on failure
 */
static inline int lora_test(const struct device *dev)
{
	const struct lora_driver_api *api = 
		(const struct lora_driver_api *)dev->api;

	if (!api->test) {
		return -ENOSYS;
	}

	return api->test(dev);
}

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_LORA_H_ */