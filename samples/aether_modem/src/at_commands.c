/*
 * Copyright (c) 2025 Æther Authors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/heymac.h>
#include <zephyr/net/aether_mesh.h>
#include <zephyr/sys/reboot.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "at_commands.h"
#include "message_store.h"

LOG_MODULE_REGISTER(at_commands, LOG_LEVEL_DBG);

/* Command table */
static const struct at_command system_commands[] = {
	{ "RESET",    cmd_reset,    "Reset the interface device" },
	{ "VER",      cmd_version,  "Query firmware version" },
	{ "ECHO",     cmd_echo,     "Enable/disable command echo" },
	{ "VERBOSE",  cmd_verbose,  "Set verbosity level (0-2)" },
	{ "BAUD",     cmd_baud,     "Set baud rate" },
	{ "FLOW",     cmd_flow,     "Set flow control mode" },
	{ "SAVE",     cmd_save,     "Save current settings" },
	{ "HELP",     cmd_help,     "List available commands" },
	{ NULL, NULL, NULL }
};

static const struct at_command network_commands[] = {
	{ "INIT",      cmd_init,      "Initialize LoRa hardware" },
	{ "NETALIAS",  cmd_netalias,  "Set node alias name" },
	{ "NETADDR",   cmd_netaddr,   "Query LoRa network address" },
	{ "NETID",     cmd_netid,     "Set network ID" },
	{ "NETJOIN",   cmd_netjoin,   "Join Aether network" },
	{ "NETLEAVE",  cmd_netleave,  "Leave Aether network" },
	{ "NETSTAT",   cmd_netstat,   "Network status" },
	{ "NEIGH",     cmd_neigh,     "List neighbor nodes" },
	{ "ROUTE",     cmd_route,     "Query routing table" },
	{ "DUTYCYCLE", cmd_dutycycle, "Query remaining duty cycle" },
	{ "SLEEP",     cmd_sleep,     "Enter power saving mode" },
	{ "WAKE",      cmd_wake,      "Wake from sleep mode" },
	{ NULL, NULL, NULL }
};

static const struct at_command message_commands[] = {
	{ "MSGSEND",  cmd_msgsend,  "Begin message send" },
	{ "MSGTO",    cmd_msgto,    "Set default message destination" },
	{ "MSGMODE",  cmd_msgmode,  "Set message mode" },
	{ "MSGRECV",  cmd_msgrecv,  "Check for received messages" },
	{ "MSGREAD",  cmd_msgread,  "Read specific message" },
	{ "MSGLIST",  cmd_msglist,  "List stored messages" },
	{ "MSGDEL",   cmd_msgdel,   "Delete message" },
	{ "MSGACK",   cmd_msgack,   "Configure delivery reports" },
	{ "MSGSENT",  cmd_msgsent,  "Check if message was sent" },
	{ NULL, NULL, NULL }
};

/* Command categories */
static const struct at_command *command_tables[] = {
	[AT_CAT_SYSTEM] = system_commands,
	[AT_CAT_NETWORK] = network_commands,
	[AT_CAT_MESSAGE] = message_commands,
	/* TODO: Add other categories */
};

/* Parser reference */
static struct at_parser *g_parser;

/* Initialize commands */
void at_commands_init(struct at_parser *parser)
{
	g_parser = parser;
	LOG_DBG("AT commands initialized");
}

/* Find and dispatch command */
bool at_dispatch_command(const char *cmd, const char *params, bool query)
{
	/* Search all command tables */
	for (int cat = 0; cat < AT_CAT_MAX; cat++) {
		const struct at_command *table = command_tables[cat];
		if (!table) continue;
		
		for (int i = 0; table[i].name; i++) {
			if (strcmp(cmd, table[i].name) == 0) {
				if (table[i].handler) {
					int ret = table[i].handler(params, query);
					if (ret < 0) {
						return false;
					}
					return true;
				}
			}
		}
	}
	
	return false;
}

/* Helper functions */
int at_parse_hex(const char *str, uint8_t *buf, size_t max_len)
{
	size_t len = 0;
	
	while (*str && len < max_len) {
		char hex[3] = {0};
		hex[0] = *str++;
		if (!*str) break;
		hex[1] = *str++;
		
		buf[len++] = (uint8_t)strtol(hex, NULL, 16);
	}
	
	return len;
}

int at_parse_int(const char *str, int *value)
{
	char *endptr;
	*value = strtol(str, &endptr, 10);
	return (*endptr == '\0') ? 0 : -EINVAL;
}

int at_parse_uint(const char *str, unsigned int *value)
{
	char *endptr;
	*value = strtoul(str, &endptr, 10);
	return (*endptr == '\0') ? 0 : -EINVAL;
}

bool at_parse_bool(const char *str)
{
	return (strcmp(str, "1") == 0 || 
	        strcasecmp(str, "true") == 0 ||
	        strcasecmp(str, "on") == 0);
}

/* System Commands Implementation */

int cmd_reset(const char *params, bool query)
{
	ARG_UNUSED(params);
	ARG_UNUSED(query);
	
	at_send_response(AT_OK);
	k_sleep(K_MSEC(100));  /* Let response go out */
	sys_reboot(SYS_REBOOT_COLD);
	return 0;
}

int cmd_version(const char *params, bool query)
{
	ARG_UNUSED(params);
	
	if (!query) {
		at_send_error(AT_ERR_INVALID_CMD);
		return -EINVAL;
	}
	
	at_send_response_fmt("+VER: Aether Modem v1.0.0");
	at_send_response_fmt("+VER: Zephyr %s", KERNEL_VERSION_STRING);
	at_send_response(AT_OK);
	return 0;
}

int cmd_echo(const char *params, bool query)
{
	if (query) {
		at_send_response_fmt("+ECHO: %d", g_parser->config.echo_enabled ? 1 : 0);
		at_send_response(AT_OK);
		return 0;
	}
	
	if (!params) {
		at_send_error(AT_ERR_INVALID_PARAM);
		return -EINVAL;
	}
	
	g_parser->config.echo_enabled = at_parse_bool(params);
	at_send_response(AT_OK);
	return 0;
}

int cmd_verbose(const char *params, bool query)
{
	if (query) {
		at_send_response_fmt("+VERBOSE: %d", g_parser->config.verbose_level);
		at_send_response(AT_OK);
		return 0;
	}
	
	if (!params) {
		at_send_error(AT_ERR_INVALID_PARAM);
		return -EINVAL;
	}
	
	unsigned int level;
	if (at_parse_uint(params, &level) < 0 || level > 2) {
		at_send_error(AT_ERR_INVALID_PARAM);
		return -EINVAL;
	}
	
	g_parser->config.verbose_level = level;
	at_send_response(AT_OK);
	return 0;
}

int cmd_help(const char *params, bool query)
{
	ARG_UNUSED(query);
	
	if (params && strlen(params) > 0) {
		/* Help for specific command */
		for (int cat = 0; cat < AT_CAT_MAX; cat++) {
			const struct at_command *table = command_tables[cat];
			if (!table) continue;
			
			for (int i = 0; table[i].name; i++) {
				if (strcasecmp(params, table[i].name) == 0) {
					at_send_response_fmt("+HELP: %s - %s", 
					                     table[i].name, table[i].help);
					at_send_response(AT_OK);
					return 0;
				}
			}
		}
		at_send_error(AT_ERR_INVALID_PARAM);
		return -EINVAL;
	}
	
	/* List all commands */
	at_send_response("+HELP: Available commands:");
	for (int cat = 0; cat < AT_CAT_MAX; cat++) {
		const struct at_command *table = command_tables[cat];
		if (!table) continue;
		
		for (int i = 0; table[i].name; i++) {
			at_send_response_fmt("+HELP: AT+%s", table[i].name);
		}
	}
	at_send_response(AT_OK);
	return 0;
}

/* Network Commands Implementation */

int cmd_init(const char *params, bool query)
{
	ARG_UNUSED(params);
	ARG_UNUSED(query);
	
	if (g_parser->initialized) {
		at_send_response(AT_OK);
		return 0;
	}
	
	/* Wait for network interface */
	if (!net_if_is_up(g_parser->iface)) {
		at_send_error(AT_ERR_NET_NOT_INIT);
		return -ENETDOWN;
	}
	
	g_parser->initialized = true;
	at_send_response(AT_OK);
	return 0;
}

int cmd_netalias(const char *params, bool query)
{
	if (query) {
		at_send_response_fmt("+NETALIAS: %s", g_parser->config.node_alias);
		at_send_response(AT_OK);
		return 0;
	}
	
	if (!params || strlen(params) == 0 || strlen(params) > 15) {
		at_send_error(AT_ERR_INVALID_PARAM);
		return -EINVAL;
	}
	
	strncpy(g_parser->config.node_alias, params, 15);
	g_parser->config.node_alias[15] = '\0';
	at_send_response(AT_OK);
	return 0;
}

int cmd_netaddr(const char *params, bool query)
{
	ARG_UNUSED(params);
	
	if (!query) {
		at_send_error(AT_ERR_INVALID_CMD);
		return -EINVAL;
	}
	
	if (!g_parser->initialized) {
		at_send_error(AT_ERR_NET_NOT_INIT);
		return -ENETDOWN;
	}
	
	struct heymac_addr addr;
	if (heymac_get_addr(g_parser->iface, &addr) < 0) {
		at_send_error(AT_ERR_UNKNOWN);
		return -EIO;
	}
	
	if (heymac_addr_is_short(&addr)) {
		at_send_response_fmt("+NETADDR: %02X%02X", 
		                     addr.short_addr[0], addr.short_addr[1]);
	} else {
		at_send_response_fmt("+NETADDR: %02X%02X%02X%02X%02X%02X",
		                     addr.long_addr[0], addr.long_addr[1],
		                     addr.long_addr[2], addr.long_addr[3],
		                     addr.long_addr[4], addr.long_addr[5]);
	}
	at_send_response(AT_OK);
	return 0;
}

int cmd_netjoin(const char *params, bool query)
{
	ARG_UNUSED(params);
	ARG_UNUSED(query);
	
	if (!g_parser->initialized) {
		at_send_error(AT_ERR_NET_NOT_INIT);
		return -ENETDOWN;
	}
	
	if (g_parser->connected) {
		at_send_response(AT_OK);
		return 0;
	}
	
	/* Initialize Æther mesh */
	int ret = aether_mesh_init(g_parser->iface, g_parser->config.network_id);
	if (ret < 0) {
		at_send_error(AT_ERR_NET_NOT_CONNECTED);
		return ret;
	}
	
	g_parser->connected = true;
	at_send_response(AT_OK);
	at_send_notification("NETJOIN", "CONNECTED");
	return 0;
}

int cmd_netleave(const char *params, bool query)
{
	ARG_UNUSED(params);
	ARG_UNUSED(query);
	
	if (!g_parser->connected) {
		at_send_response(AT_OK);
		return 0;
	}
	
	/* TODO: Implement mesh leave */
	g_parser->connected = false;
	at_send_response(AT_OK);
	at_send_notification("NETLEAVE", "DISCONNECTED");
	return 0;
}

int cmd_netstat(const char *params, bool query)
{
	ARG_UNUSED(params);
	ARG_UNUSED(query);
	
	at_send_response_fmt("+NETSTAT: INIT=%d", g_parser->initialized ? 1 : 0);
	at_send_response_fmt("+NETSTAT: CONNECTED=%d", g_parser->connected ? 1 : 0);
	
	if (g_parser->connected) {
		int neighbors = aether_mesh_get_neighbor_count(g_parser->iface);
		int routes = aether_mesh_get_route_count(g_parser->iface);
		at_send_response_fmt("+NETSTAT: NEIGHBORS=%d", neighbors);
		at_send_response_fmt("+NETSTAT: ROUTES=%d", routes);
	}
	
	at_send_response(AT_OK);
	return 0;
}

int cmd_neigh(const char *params, bool query)
{
	ARG_UNUSED(params);
	ARG_UNUSED(query);
	
	if (!g_parser->connected) {
		at_send_error(AT_ERR_NET_NOT_CONNECTED);
		return -ENETDOWN;
	}
	
	/* TODO: Implement neighbor listing */
	at_send_response("+NEIGH: No neighbors");
	at_send_response(AT_OK);
	return 0;
}

/* Stub implementations for remaining commands */
int cmd_baud(const char *params, bool query) { return 0; }
int cmd_flow(const char *params, bool query) { return 0; }
int cmd_save(const char *params, bool query) { return 0; }
int cmd_netid(const char *params, bool query) { return 0; }
int cmd_route(const char *params, bool query) { return 0; }
int cmd_dutycycle(const char *params, bool query) { return 0; }
int cmd_sleep(const char *params, bool query) { return 0; }
int cmd_wake(const char *params, bool query) { return 0; }
int cmd_msgsend(const char *params, bool query) { return 0; }
int cmd_msgto(const char *params, bool query) { return 0; }
int cmd_msgmode(const char *params, bool query) { return 0; }
int cmd_msgrecv(const char *params, bool query) { return 0; }
int cmd_msgread(const char *params, bool query) { return 0; }
int cmd_msglist(const char *params, bool query) { return 0; }
int cmd_msgdel(const char *params, bool query) { return 0; }
int cmd_msgack(const char *params, bool query) { return 0; }
int cmd_msgsent(const char *params, bool query) { return 0; }