/*
 * Copyright (c) 2025 Æther Authors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(sx126x_lora, CONFIG_LORA_LOG_LEVEL);

#include <zephyr/device.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>

/* SX126x commands */
#define SX126X_CMD_SET_SLEEP			0x84
#define SX126X_CMD_SET_STANDBY			0x80
#define SX126X_CMD_SET_FS			0xC1
#define SX126X_CMD_SET_TX			0x83
#define SX126X_CMD_SET_RX			0x82
#define SX126X_CMD_SET_RF_FREQUENCY		0x86
#define SX126X_CMD_SET_PA_CONFIG		0x95
#define SX126X_CMD_SET_TX_PARAMS		0x8E
#define SX126X_CMD_SET_MODULATION_PARAMS	0x8B
#define SX126X_CMD_SET_PACKET_PARAMS		0x8C
#define SX126X_CMD_WRITE_BUFFER			0x0E
#define SX126X_CMD_READ_BUFFER			0x1E
#define SX126X_CMD_GET_STATUS			0xC0
#define SX126X_CMD_GET_RX_BUFFER_STATUS		0x13
#define SX126X_CMD_GET_PACKET_STATUS		0x14
#define SX126X_CMD_CLEAR_IRQ_STATUS		0x02
#define SX126X_CMD_SET_DIO_IRQ_PARAMS		0x08

/* SX126x register addresses */
#define SX126X_REG_WHITENING_INITIAL_MSB	0x06B8
#define SX126X_REG_WHITENING_INITIAL_LSB	0x06B9
#define SX126X_REG_CRC_INITIAL_MSB		0x06BC
#define SX126X_REG_CRC_INITIAL_LSB		0x06BD
#define SX126X_REG_CRC_POLYNOMIAL_MSB		0x06BE
#define SX126X_REG_CRC_POLYNOMIAL_LSB		0x06BF
#define SX126X_REG_SYNC_WORD_0			0x06C0
#define SX126X_REG_SYNC_WORD_1			0x06C1
#define SX126X_REG_SYNC_WORD_2			0x06C2
#define SX126X_REG_SYNC_WORD_3			0x06C3
#define SX126X_REG_SYNC_WORD_4			0x06C4
#define SX126X_REG_SYNC_WORD_5			0x06C5
#define SX126X_REG_SYNC_WORD_6			0x06C6
#define SX126X_REG_SYNC_WORD_7			0x06C7

/* IRQ masks */
#define SX126X_IRQ_TX_DONE			BIT(0)
#define SX126X_IRQ_RX_DONE			BIT(1)
#define SX126X_IRQ_PREAMBLE_DETECTED		BIT(2)
#define SX126X_IRQ_SYNC_WORD_VALID		BIT(3)
#define SX126X_IRQ_HEADER_VALID			BIT(4)
#define SX126X_IRQ_HEADER_ERR			BIT(5)
#define SX126X_IRQ_CRC_ERR			BIT(6)
#define SX126X_IRQ_CAD_DONE			BIT(7)
#define SX126X_IRQ_CAD_DETECTED			BIT(8)
#define SX126X_IRQ_TIMEOUT			BIT(9)

struct sx126x_data {
	const struct device *spi_dev;
	struct spi_config spi_cfg;
	struct gpio_dt_spec reset_gpio;
	struct gpio_dt_spec busy_gpio;
	struct gpio_dt_spec dio1_gpio;
	struct gpio_callback dio1_cb;
	lora_recv_cb_t recv_cb;
	struct k_work_delayable rx_work;
	bool rx_continuous;
};

struct sx126x_config {
	struct spi_dt_spec spi;
	struct gpio_dt_spec reset_gpio;
	struct gpio_dt_spec busy_gpio;
	struct gpio_dt_spec dio1_gpio;
};

static int sx126x_write_command(const struct device *dev, uint8_t cmd,
				const uint8_t *data, size_t len)
{
	struct sx126x_data *drv_data = dev->data;
	uint8_t tx_buf[len + 1];
	
	tx_buf[0] = cmd;
	if (data && len > 0) {
		memcpy(&tx_buf[1], data, len);
	}
	
	const struct spi_buf tx_spi_buf = {
		.buf = tx_buf,
		.len = len + 1,
	};
	const struct spi_buf_set tx_spi_buf_set = {
		.buffers = &tx_spi_buf,
		.count = 1,
	};
	
	return spi_write(drv_data->spi_dev, &drv_data->spi_cfg, &tx_spi_buf_set);
}

static int sx126x_read_command(const struct device *dev, uint8_t cmd,
			       uint8_t *data, size_t len)
{
	struct sx126x_data *drv_data = dev->data;
	uint8_t tx_buf[len + 2];  /* cmd + status + data */
	uint8_t rx_buf[len + 2];
	
	memset(tx_buf, 0, sizeof(tx_buf));
	tx_buf[0] = cmd;
	
	const struct spi_buf tx_spi_buf = {
		.buf = tx_buf,
		.len = len + 2,
	};
	const struct spi_buf rx_spi_buf = {
		.buf = rx_buf,
		.len = len + 2,
	};
	const struct spi_buf_set tx_spi_buf_set = {
		.buffers = &tx_spi_buf,
		.count = 1,
	};
	const struct spi_buf_set rx_spi_buf_set = {
		.buffers = &rx_spi_buf,
		.count = 1,
	};
	
	int ret = spi_transceive(drv_data->spi_dev, &drv_data->spi_cfg,
				 &tx_spi_buf_set, &rx_spi_buf_set);
	if (ret == 0 && data && len > 0) {
		memcpy(data, &rx_buf[2], len);  /* Skip cmd echo and status */
	}
	
	return ret;
}

static int sx126x_wait_on_busy(const struct device *dev)
{
	struct sx126x_data *drv_data = dev->data;
	int timeout = 1000; /* 1 second timeout */
	
	while (gpio_pin_get_dt(&drv_data->busy_gpio) && timeout-- > 0) {
		k_sleep(K_MSEC(1));
	}
	
	return timeout > 0 ? 0 : -ETIMEDOUT;
}

static int sx126x_reset(const struct device *dev)
{
	struct sx126x_data *drv_data = dev->data;
	
	gpio_pin_set_dt(&drv_data->reset_gpio, 1);
	k_sleep(K_MSEC(1));
	gpio_pin_set_dt(&drv_data->reset_gpio, 0);
	k_sleep(K_MSEC(10));
	
	return sx126x_wait_on_busy(dev);
}

static int sx126x_set_standby(const struct device *dev)
{
	uint8_t standby_config = 0x00; /* STDBY_RC */
	
	int ret = sx126x_write_command(dev, SX126X_CMD_SET_STANDBY, 
				       &standby_config, 1);
	if (ret == 0) {
		ret = sx126x_wait_on_busy(dev);
	}
	
	return ret;
}

static int sx126x_config(const struct device *dev, struct lora_modem_config *config)
{
	int ret;
	uint8_t buf[8];
	
	LOG_DBG("Configuring SX126x: freq=%u, sf=%u, bw=%u", 
		config->frequency, config->datarate, config->bandwidth);
	
	/* Set standby mode */
	ret = sx126x_set_standby(dev);
	if (ret < 0) {
		LOG_ERR("Failed to set standby mode: %d", ret);
		return ret;
	}
	
	/* Set frequency */
	uint32_t freq_raw = (uint64_t)config->frequency * (1ULL << 25) / 32000000ULL;
	sys_put_be32(freq_raw, buf);
	ret = sx126x_write_command(dev, SX126X_CMD_SET_RF_FREQUENCY, buf, 4);
	if (ret < 0) {
		LOG_ERR("Failed to set frequency: %d", ret);
		return ret;
	}
	
	/* Set modulation parameters (LoRa) */
	buf[0] = config->datarate;           /* Spreading factor */
	buf[1] = config->bandwidth;          /* Bandwidth */
	buf[2] = config->coderate;           /* Coding rate */
	buf[3] = 0;                          /* Low data rate optimize */
	ret = sx126x_write_command(dev, SX126X_CMD_SET_MODULATION_PARAMS, buf, 4);
	if (ret < 0) {
		LOG_ERR("Failed to set modulation params: %d", ret);
		return ret;
	}
	
	/* Set packet parameters */
	buf[0] = (config->preamble >> 8) & 0xFF;  /* Preamble MSB */
	buf[1] = config->preamble & 0xFF;         /* Preamble LSB */
	buf[2] = 0x00;                            /* Header type: explicit */
	buf[3] = 0xFF;                            /* Payload length */
	buf[4] = config->crc ? 0x01 : 0x00;       /* CRC enable */
	buf[5] = 0x00;                            /* IQ setup */
	ret = sx126x_write_command(dev, SX126X_CMD_SET_PACKET_PARAMS, buf, 6);
	if (ret < 0) {
		LOG_ERR("Failed to set packet params: %d", ret);
		return ret;
	}
	
	/* Set TX power */
	buf[0] = config->tx_power;           /* Power */
	buf[1] = 0x02;                       /* PA ramp time */
	ret = sx126x_write_command(dev, SX126X_CMD_SET_TX_PARAMS, buf, 2);
	if (ret < 0) {
		LOG_ERR("Failed to set TX params: %d", ret);
		return ret;
	}
	
	LOG_INF("SX126x configured successfully");
	return 0;
}

static int sx126x_send(const struct device *dev, uint8_t *data, uint32_t data_len)
{
	int ret;
	uint8_t buf[4];
	
	if (data_len > 255) {
		return -EINVAL;
	}
	
	LOG_DBG("Sending %u bytes", data_len);
	
	/* Set standby mode */
	ret = sx126x_set_standby(dev);
	if (ret < 0) {
		return ret;
	}
	
	/* Write data to buffer */
	buf[0] = 0x00;  /* Offset */
	ret = sx126x_write_command(dev, SX126X_CMD_WRITE_BUFFER, buf, 1);
	if (ret < 0) {
		LOG_ERR("Failed to write buffer command: %d", ret);
		return ret;
	}
	
	/* Write actual data (simplified - real implementation needs proper buffer write) */
	/* TODO: Implement proper buffer write with SPI transaction */
	
	/* Set TX mode */
	buf[0] = 0x00;  /* Timeout MSB */
	buf[1] = 0x00;  /* Timeout middle */
	buf[2] = 0x00;  /* Timeout LSB */
	ret = sx126x_write_command(dev, SX126X_CMD_SET_TX, buf, 3);
	if (ret < 0) {
		LOG_ERR("Failed to set TX mode: %d", ret);
		return ret;
	}
	
	LOG_DBG("Packet sent");
	return 0;
}

static int sx126x_recv_async(const struct device *dev, lora_recv_cb_t cb)
{
	struct sx126x_data *drv_data = dev->data;
	uint8_t buf[8];
	int ret;
	
	drv_data->recv_cb = cb;
	
	/* Set DIO1 IRQ configuration */
	sys_put_be16(SX126X_IRQ_RX_DONE | SX126X_IRQ_TIMEOUT | SX126X_IRQ_CRC_ERR, &buf[0]);
	sys_put_be16(SX126X_IRQ_RX_DONE | SX126X_IRQ_TIMEOUT | SX126X_IRQ_CRC_ERR, &buf[2]);
	sys_put_be16(0, &buf[4]);  /* DIO2 mask */
	sys_put_be16(0, &buf[6]);  /* DIO3 mask */
	
	ret = sx126x_write_command(dev, SX126X_CMD_SET_DIO_IRQ_PARAMS, buf, 8);
	if (ret < 0) {
		LOG_ERR("Failed to set IRQ params: %d", ret);
		return ret;
	}
	
	/* Set continuous RX mode */
	buf[0] = 0xFF;  /* Timeout MSB (continuous) */
	buf[1] = 0xFF;  /* Timeout middle */
	buf[2] = 0xFF;  /* Timeout LSB */
	ret = sx126x_write_command(dev, SX126X_CMD_SET_RX, buf, 3);
	if (ret < 0) {
		LOG_ERR("Failed to set RX mode: %d", ret);
		return ret;
	}
	
	drv_data->rx_continuous = true;
	LOG_INF("Async RX enabled");
	return 0;
}

static int sx126x_test(const struct device *dev)
{
	uint8_t status;
	int ret = sx126x_read_command(dev, SX126X_CMD_GET_STATUS, &status, 1);
	
	if (ret == 0) {
		LOG_INF("SX126x status: 0x%02x", status);
	}
	
	return ret;
}

static void sx126x_dio1_callback(const struct device *dev, struct gpio_callback *cb,
				 uint32_t pins)
{
	struct sx126x_data *drv_data = CONTAINER_OF(cb, struct sx126x_data, dio1_cb);
	
	/* Schedule work to handle IRQ */
	k_work_reschedule(&drv_data->rx_work, K_NO_WAIT);
}

static void sx126x_rx_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct sx126x_data *drv_data = CONTAINER_OF(dwork, struct sx126x_data, rx_work);
	const struct device *dev = drv_data->spi_dev; /* TODO: Get proper device reference */
	
	/* TODO: Implement proper RX handling */
	/* - Read IRQ status */
	/* - If RX_DONE, read buffer and call recv_cb */
	/* - Clear IRQ status */
	/* - Restart RX if continuous mode */
	
	if (drv_data->recv_cb) {
		uint8_t dummy_data[] = "test";
		drv_data->recv_cb(dev, dummy_data, sizeof(dummy_data), -50, 10);
	}
}

static const struct lora_driver_api sx126x_api = {
	.config = sx126x_config,
	.send = sx126x_send,
	.recv_async = sx126x_recv_async,
	.test = sx126x_test,
};

static int sx126x_init(const struct device *dev)
{
	struct sx126x_data *drv_data = dev->data;
	const struct sx126x_config *config = dev->config;
	int ret;
	
	/* Initialize SPI */
	if (!spi_is_ready_dt(&config->spi)) {
		LOG_ERR("SPI device not ready");
		return -ENODEV;
	}
	
	drv_data->spi_dev = config->spi.bus;
	drv_data->spi_cfg = config->spi.config;
	
	/* Initialize GPIOs */
	if (!gpio_is_ready_dt(&config->reset_gpio)) {
		LOG_ERR("Reset GPIO not ready");
		return -ENODEV;
	}
	drv_data->reset_gpio = config->reset_gpio;
	
	ret = gpio_pin_configure_dt(&drv_data->reset_gpio, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("Failed to configure reset GPIO: %d", ret);
		return ret;
	}
	
	if (!gpio_is_ready_dt(&config->busy_gpio)) {
		LOG_ERR("Busy GPIO not ready");
		return -ENODEV;
	}
	drv_data->busy_gpio = config->busy_gpio;
	
	ret = gpio_pin_configure_dt(&drv_data->busy_gpio, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("Failed to configure busy GPIO: %d", ret);
		return ret;
	}
	
	if (!gpio_is_ready_dt(&config->dio1_gpio)) {
		LOG_ERR("DIO1 GPIO not ready");
		return -ENODEV;
	}
	drv_data->dio1_gpio = config->dio1_gpio;
	
	ret = gpio_pin_configure_dt(&drv_data->dio1_gpio, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("Failed to configure DIO1 GPIO: %d", ret);
		return ret;
	}
	
	/* Setup DIO1 interrupt */
	gpio_init_callback(&drv_data->dio1_cb, sx126x_dio1_callback,
			   BIT(drv_data->dio1_gpio.pin));
	ret = gpio_add_callback(drv_data->dio1_gpio.port, &drv_data->dio1_cb);
	if (ret < 0) {
		LOG_ERR("Failed to add GPIO callback: %d", ret);
		return ret;
	}
	
	ret = gpio_pin_interrupt_configure_dt(&drv_data->dio1_gpio, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret < 0) {
		LOG_ERR("Failed to configure GPIO interrupt: %d", ret);
		return ret;
	}
	
	/* Initialize work */
	k_work_init_delayable(&drv_data->rx_work, sx126x_rx_work_handler);
	
	/* Reset and initialize chip */
	ret = sx126x_reset(dev);
	if (ret < 0) {
		LOG_ERR("Failed to reset SX126x: %d", ret);
		return ret;
	}
	
	LOG_INF("SX126x LoRa driver initialized");
	return 0;
}

#define SX126X_DEFINE(inst)							\
	static struct sx126x_data sx126x_data_##inst;				\
	static const struct sx126x_config sx126x_config_##inst = {		\
		.spi = SPI_DT_SPEC_INST_GET(inst, SPI_WORD_SET(8) |		\
					    SPI_TRANSFER_MSB, 0),		\
		.reset_gpio = GPIO_DT_SPEC_INST_GET(inst, reset_gpios),		\
		.busy_gpio = GPIO_DT_SPEC_INST_GET(inst, busy_gpios),		\
		.dio1_gpio = GPIO_DT_SPEC_INST_GET(inst, dio1_gpios),		\
	};									\
	DEVICE_DT_INST_DEFINE(inst, sx126x_init, NULL,				\
			      &sx126x_data_##inst, &sx126x_config_##inst,	\
			      POST_KERNEL, CONFIG_LORA_INIT_PRIORITY,		\
			      &sx126x_api);

DT_INST_FOREACH_STATUS_OKAY(SX126X_DEFINE)