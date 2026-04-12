/*
 * Copyright (c) 2025 Æther Authors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "at_parser.h"
#include "at_commands.h"

LOG_MODULE_REGISTER(at_parser, LOG_LEVEL_DBG);

/* Global parser instance */
static struct at_parser g_parser;

/* Error messages */
static const char *at_error_messages[] = {
	"Unknown error",
	"Invalid command",
	"Invalid parameter",
	"Network not initialized",
	"Network not connected",
	"Transmission failed",
	"Message too long",
	"No message available",
	"Invalid FID",
	"File not found",
	"Permission denied",
	"Insufficient resources",
	"Duty cycle limit reached",
	"Service unavailable",
	"Authentication failed",
};

/* Forward declarations */
static void parse_command_line(const char *cmd);
static int parse_parameters(const char *params, char *argv[], int max_args);

/* Initialize parser */
void at_parser_init(struct net_if *iface)
{
	memset(&g_parser, 0, sizeof(g_parser));
	
	g_parser.iface = iface;
	g_parser.state = AT_STATE_IDLE;
	
	/* Default configuration */
	g_parser.config.echo_enabled = true;
	g_parser.config.verbose_level = 1;
	g_parser.config.baud_rate = 9600;
	g_parser.config.flow_control = 0;  /* None */
	g_parser.config.network_id = 0x915E;
	strcpy(g_parser.config.node_alias, "MODEM");
	
	k_mutex_init(&g_parser.lock);
	
	/* Initialize command handlers */
	at_commands_init(&g_parser);
	
	LOG_INF("AT parser initialized");
}

/* Process AT command */
void at_parser_process_command(const char *cmd)
{
	k_mutex_lock(&g_parser.lock, K_FOREVER);
	
	g_parser.commands_processed++;
	
	/* Skip empty commands */
	if (!cmd || strlen(cmd) == 0) {
		k_mutex_unlock(&g_parser.lock);
		return;
	}
	
	/* Check if we're waiting for data */
	if (g_parser.state == AT_STATE_WAIT_DATA) {
		/* This shouldn't happen - data should come through process_data */
		at_send_error(AT_ERR_INVALID_CMD);
		g_parser.state = AT_STATE_IDLE;
		k_mutex_unlock(&g_parser.lock);
		return;
	}
	
	/* Process the command */
	parse_command_line(cmd);
	
	k_mutex_unlock(&g_parser.lock);
}

/* Parse command line */
static void parse_command_line(const char *cmd)
{
	char cmd_buf[256];
	char *cmd_name;
	char *params;
	
	/* Make a copy for parsing */
	strncpy(cmd_buf, cmd, sizeof(cmd_buf) - 1);
	cmd_buf[sizeof(cmd_buf) - 1] = '\0';
	
	/* Convert to uppercase for command matching */
	for (int i = 0; cmd_buf[i] && cmd_buf[i] != '=' && cmd_buf[i] != '?'; i++) {
		cmd_buf[i] = toupper(cmd_buf[i]);
	}
	
	/* Check for basic "AT" command */
	if (strcmp(cmd_buf, "AT") == 0) {
		at_send_response(AT_OK);
		return;
	}
	
	/* Check for "AT+" prefix */
	if (strncmp(cmd_buf, "AT+", 3) != 0) {
		at_send_error(AT_ERR_INVALID_CMD);
		return;
	}
	
	/* Extract command name and parameters */
	cmd_name = cmd_buf + 3;  /* Skip "AT+" */
	
	/* Find parameter separator */
	params = strchr(cmd_name, '=');
	if (params) {
		*params = '\0';
		params++;
	} else {
		/* Check for query suffix */
		char *query = strchr(cmd_name, '?');
		if (query) {
			*query = '\0';
			/* Query commands have no parameters */
			params = NULL;
		}
	}
	
	/* Dispatch to command handler */
	if (!at_dispatch_command(cmd_name, params, strchr(cmd, '?') != NULL)) {
		at_send_error(AT_ERR_INVALID_CMD);
	}
}

/* Parse comma-separated parameters */
static int parse_parameters(const char *params, char *argv[], int max_args)
{
	int argc = 0;
	char *p;
	char *param_copy;
	
	if (!params || strlen(params) == 0) {
		return 0;
	}
	
	/* Make a copy for strtok */
	param_copy = k_malloc(strlen(params) + 1);
	if (!param_copy) {
		return -ENOMEM;
	}
	strcpy(param_copy, params);
	
	/* Parse parameters */
	p = strtok(param_copy, ",");
	while (p && argc < max_args) {
		/* Skip leading/trailing spaces */
		while (*p == ' ') p++;
		char *end = p + strlen(p) - 1;
		while (end > p && *end == ' ') *end-- = '\0';
		
		/* Allocate and copy parameter */
		argv[argc] = k_malloc(strlen(p) + 1);
		if (argv[argc]) {
			strcpy(argv[argc], p);
			argc++;
		}
		
		p = strtok(NULL, ",");
	}
	
	k_free(param_copy);
	return argc;
}

/* Process data input */
void at_parser_process_data(const uint8_t *data, size_t len)
{
	k_mutex_lock(&g_parser.lock, K_FOREVER);
	
	if (g_parser.state != AT_STATE_WAIT_DATA) {
		/* Not expecting data */
		k_mutex_unlock(&g_parser.lock);
		return;
	}
	
	/* Copy data to buffer */
	size_t copy_len = MIN(len, g_parser.data_ctx.expected_len - g_parser.data_ctx.received_len);
	if (g_parser.data_ctx.buffer) {
		memcpy(g_parser.data_ctx.buffer + g_parser.data_ctx.received_len, data, copy_len);
	}
	g_parser.data_ctx.received_len += copy_len;
	
	/* Check if we have all data */
	if (g_parser.data_ctx.received_len >= g_parser.data_ctx.expected_len) {
		/* Call handler */
		if (g_parser.data_ctx.handler) {
			g_parser.data_ctx.handler(g_parser.data_ctx.buffer, 
			                         g_parser.data_ctx.received_len,
			                         g_parser.data_ctx.user_data);
		}
		
		/* Clean up */
		if (g_parser.data_ctx.buffer) {
			k_free(g_parser.data_ctx.buffer);
			g_parser.data_ctx.buffer = NULL;
		}
		
		g_parser.state = AT_STATE_IDLE;
	}
	
	k_mutex_unlock(&g_parser.lock);
}

/* Set up data input mode */
int at_setup_data_mode(size_t expected_len, 
                       void (*handler)(const uint8_t *, size_t, void *),
                       void *user_data)
{
	if (g_parser.state != AT_STATE_IDLE) {
		return -EBUSY;
	}
	
	/* Allocate buffer */
	g_parser.data_ctx.buffer = k_malloc(expected_len);
	if (!g_parser.data_ctx.buffer) {
		return -ENOMEM;
	}
	
	g_parser.data_ctx.handler = handler;
	g_parser.data_ctx.user_data = user_data;
	g_parser.data_ctx.expected_len = expected_len;
	g_parser.data_ctx.received_len = 0;
	
	g_parser.state = AT_STATE_WAIT_DATA;
	
	/* Send prompt */
	at_send_prompt();
	
	return 0;
}

/* Send response */
void at_send_response(const char *response)
{
	extern void at_uart_send(const char *str);
	at_uart_send(response);
}

/* Send formatted response */
void at_send_response_fmt(const char *fmt, ...)
{
	char buf[256];
	va_list args;
	
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	
	at_send_response(buf);
}

/* Send error response */
void at_send_error(enum at_error err)
{
	if (g_parser.config.verbose_level > 0) {
		at_send_response_fmt("%s %d: %s", AT_ERROR, err, 
		                     at_error_messages[MIN(err, ARRAY_SIZE(at_error_messages) - 1)]);
	} else {
		at_send_response_fmt("%s %d", AT_ERROR, err);
	}
	
	g_parser.errors++;
}

/* Send data prompt */
void at_send_prompt(void)
{
	extern void at_uart_send_raw(const char *str);
	at_uart_send_raw("> ");
}

/* Send unsolicited notification */
void at_send_notification(const char *event, const char *data)
{
	at_send_response_fmt("+%s: %s", event, data);
}

/* Getters */
bool at_get_echo_mode(void)
{
	return g_parser.config.echo_enabled;
}

enum at_state at_get_state(void)
{
	return g_parser.state;
}

bool at_is_initialized(void)
{
	return g_parser.initialized;
}

bool at_is_connected(void)
{
	return g_parser.connected;
}

/* Get parser instance */
struct at_parser *at_get_parser(void)
{
	return &g_parser;
}