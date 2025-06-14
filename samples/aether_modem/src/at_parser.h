/*
 * Copyright (c) 2025 Æther Authors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AT_PARSER_H_
#define AT_PARSER_H_

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <stdbool.h>

/* AT command response codes */
#define AT_OK           "OK"
#define AT_ERROR        "ERROR"
#define AT_PROMPT       ">"

/* AT error codes */
enum at_error {
	AT_ERR_UNKNOWN = 0,
	AT_ERR_INVALID_CMD = 1,
	AT_ERR_INVALID_PARAM = 2,
	AT_ERR_NET_NOT_INIT = 3,
	AT_ERR_NET_NOT_CONNECTED = 4,
	AT_ERR_TX_FAILED = 5,
	AT_ERR_MSG_TOO_LONG = 6,
	AT_ERR_NO_MSG = 7,
	AT_ERR_INVALID_FID = 8,
	AT_ERR_FILE_NOT_FOUND = 9,
	AT_ERR_PERMISSION_DENIED = 10,
	AT_ERR_NO_RESOURCES = 11,
	AT_ERR_DUTY_CYCLE = 12,
	AT_ERR_SERVICE_UNAVAIL = 13,
	AT_ERR_AUTH_FAILED = 14,
};

/* AT parser configuration */
struct at_config {
	bool echo_enabled;
	uint8_t verbose_level;  /* 0=minimal, 1=normal, 2=verbose */
	uint32_t baud_rate;
	uint8_t flow_control;   /* 0=none, 1=XON/XOFF, 2=RTS/CTS */
	uint16_t network_id;
	char node_alias[16];
};

/* AT parser state */
enum at_state {
	AT_STATE_IDLE,
	AT_STATE_WAIT_DATA,     /* Waiting for data after '>' prompt */
	AT_STATE_EXECUTING,     /* Command executing */
};

/* Data input context */
struct at_data_ctx {
	void (*handler)(const uint8_t *data, size_t len, void *user_data);
	void *user_data;
	size_t expected_len;
	size_t received_len;
	uint8_t *buffer;
};

/* AT parser context */
struct at_parser {
	struct net_if *iface;
	struct at_config config;
	enum at_state state;
	struct at_data_ctx data_ctx;
	struct k_mutex lock;
	
	/* Modem state */
	bool initialized;
	bool connected;
	char default_dest[16];
	uint8_t msg_mode;       /* 0=text, 1=hex, 2=binary */
	
	/* Statistics */
	uint32_t commands_processed;
	uint32_t errors;
};

/* AT parser API */

/**
 * Initialize AT command parser
 */
void at_parser_init(struct net_if *iface);

/**
 * Process AT command
 */
void at_parser_process_command(const char *cmd);

/**
 * Process data input (after '>' prompt)
 */
void at_parser_process_data(const uint8_t *data, size_t len);

/**
 * Send response to host
 */
void at_send_response(const char *response);

/**
 * Send formatted response
 */
void at_send_response_fmt(const char *fmt, ...);

/**
 * Send error response
 */
void at_send_error(enum at_error err);

/**
 * Send data prompt
 */
void at_send_prompt(void);

/**
 * Send unsolicited notification
 */
void at_send_notification(const char *event, const char *data);

/**
 * Get echo mode
 */
bool at_get_echo_mode(void);

/**
 * Get parser state
 */
enum at_state at_get_state(void);

/**
 * Check if modem is initialized
 */
bool at_is_initialized(void);

/**
 * Check if connected to network
 */
bool at_is_connected(void);

#endif /* AT_PARSER_H_ */