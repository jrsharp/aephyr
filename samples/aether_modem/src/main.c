/*
 * Copyright (c) 2025 Æther Authors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/heymac.h>
#include <zephyr/net/aether_mesh.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(aether_modem, LOG_LEVEL_INF);

/* AT command interface headers */
#include "at_parser.h"
#include "at_commands.h"
#include "message_store.h"

/* UART device for AT commands */
static const struct device *uart_dev;
static struct net_if *aether_iface;

/* AT command buffer */
#define AT_CMD_BUF_SIZE 256
static char at_cmd_buf[AT_CMD_BUF_SIZE];
static size_t at_cmd_pos = 0;

/* UART interrupt handler */
static void uart_isr(const struct device *dev, void *user_data)
{
	uint8_t c;

	if (!uart_irq_update(dev)) {
		return;
	}

	while (uart_irq_rx_ready(dev)) {
		if (uart_fifo_read(dev, &c, 1) != 1) {
			continue;
		}

		/* Handle AT command input */
		if (c == '\r' || c == '\n') {
			if (at_cmd_pos > 0) {
				at_cmd_buf[at_cmd_pos] = '\0';
				/* Process AT command */
				at_parser_process_command(at_cmd_buf);
				at_cmd_pos = 0;
			}
		} else if (at_cmd_pos < AT_CMD_BUF_SIZE - 1) {
			at_cmd_buf[at_cmd_pos++] = c;
			/* Echo character if enabled */
			if (at_get_echo_mode()) {
				uart_poll_out(dev, c);
			}
		}
	}
}

/* UART helper functions */
void at_uart_send(const char *str)
{
	const char *p = str;
	
	while (*p) {
		uart_poll_out(uart_dev, *p++);
	}
	uart_poll_out(uart_dev, '\r');
	uart_poll_out(uart_dev, '\n');
}

void at_uart_send_raw(const char *str)
{
	const char *p = str;
	
	while (*p) {
		uart_poll_out(uart_dev, *p++);
	}
}

/* Main entry point */
int main(void)
{
	int ret;

	LOG_INF("Æther Modem Starting");

	/* Get UART device */
	uart_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	if (!device_is_ready(uart_dev)) {
		LOG_ERR("UART device not ready");
		return -ENODEV;
	}

	/* Configure UART */
	const struct uart_config uart_cfg = {
		.baudrate = 9600,
		.parity = UART_CFG_PARITY_NONE,
		.stop_bits = UART_CFG_STOP_BITS_1,
		.data_bits = UART_CFG_DATA_BITS_8,
		.flow_ctrl = UART_CFG_FLOW_CTRL_NONE
	};
	
	ret = uart_configure(uart_dev, &uart_cfg);
	if (ret < 0) {
		LOG_ERR("Failed to configure UART: %d", ret);
		return ret;
	}

	/* Setup UART interrupt */
	uart_irq_callback_user_data_set(uart_dev, uart_isr, NULL);
	uart_irq_rx_enable(uart_dev);

#ifdef CONFIG_USB_DEVICE_STACK
	/* Initialize USB */
	ret = usb_enable(NULL);
	if (ret < 0) {
		LOG_ERR("Failed to enable USB: %d", ret);
		return ret;
	}
	
	/* Wait for USB to be ready */
	k_sleep(K_SECONDS(1));
#endif

	/* Get network interface */
	aether_iface = net_if_get_default();
	if (!aether_iface) {
		LOG_ERR("No network interface available");
		return -ENODEV;
	}

	/* Initialize AT command processor */
	at_parser_init(aether_iface);
	
	/* Initialize message store */
	msg_store_init();

	/* Send ready message */
	at_uart_send("Aether modem ready");
	at_uart_send("OK");

	LOG_INF("AT command interface ready");

	/* Main loop - everything handled in interrupts/threads */
	while (1) {
		k_sleep(K_FOREVER);
	}

	return 0;
}

