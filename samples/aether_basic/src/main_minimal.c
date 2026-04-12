/*
 * Copyright (c) 2025 Æther Authors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(lora_test, LOG_LEVEL_INF);

int main(void)
{
	const struct device *lora_dev;
	struct lora_modem_config config;
	int ret;

	LOG_INF("LoRa Test Starting");

	/* Get LoRa device */
	lora_dev = DEVICE_DT_GET(DT_ALIAS(lora0));
	if (!device_is_ready(lora_dev)) {
		LOG_ERR("LoRa device not ready");
		return -ENODEV;
	}

	LOG_INF("LoRa device ready");

	/* Configure LoRa modem */
	config.frequency = 915000000;
	config.bandwidth = BW_125_KHZ;
	config.datarate = SF_10;  /* SF10 works reliably, SF7 was failing */
	config.coding_rate = CR_4_5;
	config.preamble_len = 8;
	config.tx_power = 5;  /* Safe low power that works */
	config.tx = true;

	ret = lora_config(lora_dev, &config);
	if (ret < 0) {
		LOG_ERR("Failed to configure LoRa: %d", ret);
		return ret;
	}

	LOG_INF("LoRa configured:");
	LOG_INF("  Frequency: %u Hz", config.frequency);
	LOG_INF("  Bandwidth: %d", config.bandwidth);
	LOG_INF("  Spreading Factor: %d", config.datarate);
	LOG_INF("  Coding Rate: %d", config.coding_rate);
	LOG_INF("  TX Power: %d dBm", config.tx_power);
	
	/* Let's test if the antenna switch is actually working */
	LOG_INF("Testing antenna switching...");
	
	/* Try to receive first to test RX antenna switching */
	config.tx = false;
	ret = lora_config(lora_dev, &config);
	if (ret < 0) {
		LOG_ERR("Failed to configure for RX: %d", ret);
	} else {
		LOG_INF("RX mode configuration successful");
		k_sleep(K_MSEC(100));  /* Brief delay */
	}
	
	/* Switch back to TX mode */
	config.tx = true;
	ret = lora_config(lora_dev, &config);
	if (ret < 0) {
		LOG_ERR("Failed to switch back to TX: %d", ret);
	} else {
		LOG_INF("TX mode configuration successful");
	}

	/* PA reset sequence - helps clear protection states */
	LOG_INF("Performing PA reset sequence...");
	config.tx_power = 0;
	ret = lora_config(lora_dev, &config);
	if (ret < 0) {
		LOG_WRN("PA reset step 1 failed: %d", ret);
	}
	k_sleep(K_MSEC(100));
	
	config.tx_power = 5;  /* Back to operating power */
	ret = lora_config(lora_dev, &config);
	if (ret < 0) {
		LOG_ERR("Failed to configure LoRa after PA reset: %d", ret);
		return ret;
	}
	LOG_INF("PA reset sequence complete");

	/* Main loop - send test packets */
	uint8_t tx_data[] = "Hello LoRa!";
	int counter = 0;
	int consecutive_failures = 0;

	while (1) {
		LOG_INF("Attempting to send packet %d", counter);
		
		/* Send packet */
		ret = lora_send(lora_dev, tx_data, sizeof(tx_data));
		if (ret < 0) {
			consecutive_failures++;
			LOG_ERR("Failed to send packet %d: %d (consecutive failures: %d)", 
				counter, ret, consecutive_failures);
			
			/* If we have many failures, try different approaches */
			if (consecutive_failures == 3) {
				LOG_INF("TX might be damaged. Testing RX mode...");
				config.tx = false;  /* Switch to RX to test if receive works */
				ret = lora_config(lora_dev, &config);
				if (ret < 0) {
					LOG_ERR("Failed to configure RX mode: %d", ret);
				} else {
					LOG_INF("RX mode configured. If this works, PA may be damaged.");
					/* Wait a bit in RX mode */
					k_sleep(K_SECONDS(2));
					/* Switch back to TX */
					config.tx = true;
					config.tx_power = -10;  /* Try -10 dBm */
					lora_config(lora_dev, &config);
					LOG_INF("Switched back to TX at %d dBm", config.tx_power);
				}
			} else if (consecutive_failures == 6) {
				LOG_INF("Still failing. Trying minimum TX power...");
				config.tx_power = 0;  /* Try 0 dBm */
				ret = lora_config(lora_dev, &config);
				if (ret < 0) {
					LOG_ERR("Failed to reconfigure with min power: %d", ret);
				} else {
					LOG_INF("Reconfigured with TX power: %d dBm", config.tx_power);
				}
			} else if (consecutive_failures == 9) {
				LOG_INF("Still failing. Trying SF7 (was previously problematic)...");
				config.datarate = SF_7;  /* Try SF7 to see if it works now */
				config.tx_power = 5;
				ret = lora_config(lora_dev, &config);
				if (ret < 0) {
					LOG_ERR("Failed to reconfigure with SF7: %d", ret);
				} else {
					LOG_INF("Reconfigured with SF7 and TX power: %d dBm", config.tx_power);
				}
			} else {
				/* Try basic reconfigure */
				LOG_INF("Reconfiguring LoRa...");
				ret = lora_config(lora_dev, &config);
				if (ret < 0) {
					LOG_ERR("Failed to reconfigure: %d", ret);
				}
			}
		} else {
			LOG_INF("Successfully sent packet %d", counter);
			counter++;
			consecutive_failures = 0;  /* Reset failure counter on success */
		}

		/* Wait before next transmission */
		k_sleep(K_SECONDS(5));
	}

	return 0;
}