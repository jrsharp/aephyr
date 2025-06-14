/*
 * Copyright (c) 2025 Æther Authors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AT_COMMANDS_H_
#define AT_COMMANDS_H_

#include <stdbool.h>
#include "at_parser.h"

/* Command handler function type */
typedef int (*at_cmd_handler_t)(const char *params, bool query);

/* Command entry */
struct at_command {
	const char *name;
	at_cmd_handler_t handler;
	const char *help;
};

/* Command categories for organization */
enum at_cmd_category {
	AT_CAT_SYSTEM,
	AT_CAT_NETWORK,
	AT_CAT_MESSAGE,
	AT_CAT_9P,
	AT_CAT_STORE_FWD,
	AT_CAT_SERVICE,
	AT_CAT_SECURITY,
	AT_CAT_MAX
};

/* Initialize command handlers */
void at_commands_init(struct at_parser *parser);

/* Dispatch command to handler */
bool at_dispatch_command(const char *cmd, const char *params, bool query);

/* Helper functions for command handlers */
int at_parse_hex(const char *str, uint8_t *buf, size_t max_len);
int at_parse_int(const char *str, int *value);
int at_parse_uint(const char *str, unsigned int *value);
bool at_parse_bool(const char *str);

/* UART helpers */
void at_uart_send(const char *str);
void at_uart_send_raw(const char *str);

/* Global parser access */
struct at_parser *at_get_parser(void);
int at_setup_data_mode(size_t expected_len, 
                       void (*handler)(const uint8_t *, size_t, void *),
                       void *user_data);

/* System commands */
int cmd_reset(const char *params, bool query);
int cmd_version(const char *params, bool query);
int cmd_echo(const char *params, bool query);
int cmd_verbose(const char *params, bool query);
int cmd_baud(const char *params, bool query);
int cmd_flow(const char *params, bool query);
int cmd_save(const char *params, bool query);
int cmd_help(const char *params, bool query);

/* Network commands */
int cmd_init(const char *params, bool query);
int cmd_netalias(const char *params, bool query);
int cmd_netaddr(const char *params, bool query);
int cmd_netid(const char *params, bool query);
int cmd_netjoin(const char *params, bool query);
int cmd_netleave(const char *params, bool query);
int cmd_netstat(const char *params, bool query);
int cmd_neigh(const char *params, bool query);
int cmd_route(const char *params, bool query);
int cmd_dutycycle(const char *params, bool query);
int cmd_sleep(const char *params, bool query);
int cmd_wake(const char *params, bool query);

/* Message commands */
int cmd_msgsend(const char *params, bool query);
int cmd_msgto(const char *params, bool query);
int cmd_msgmode(const char *params, bool query);
int cmd_msgrecv(const char *params, bool query);
int cmd_msgread(const char *params, bool query);
int cmd_msglist(const char *params, bool query);
int cmd_msgdel(const char *params, bool query);
int cmd_msgack(const char *params, bool query);
int cmd_msgsent(const char *params, bool query);

/* 9P/Styx commands */
int cmd_9p_attach(const char *params, bool query);
int cmd_9p_walk(const char *params, bool query);
int cmd_9p_open(const char *params, bool query);
int cmd_9p_read(const char *params, bool query);
int cmd_9p_write(const char *params, bool query);
int cmd_9p_stat(const char *params, bool query);
int cmd_9p_clunk(const char *params, bool query);
int cmd_9p_ls(const char *params, bool query);
int cmd_9p_cat(const char *params, bool query);
int cmd_9p_mkdir(const char *params, bool query);
int cmd_9p_touch(const char *params, bool query);

/* Store-and-forward commands */
int cmd_sf_queue(const char *params, bool query);
int cmd_sf_status(const char *params, bool query);
int cmd_sf_list(const char *params, bool query);
int cmd_sf_cache(const char *params, bool query);
int cmd_sf_purge(const char *params, bool query);
int cmd_sf_prefs(const char *params, bool query);

/* Service commands */
int cmd_svc_list(const char *params, bool query);
int cmd_svc_conn(const char *params, bool query);
int cmd_svc_disc(const char *params, bool query);
int cmd_bbs_ls(const char *params, bool query);
int cmd_bbs_read(const char *params, bool query);
int cmd_bbs_post(const char *params, bool query);
int cmd_chat_join(const char *params, bool query);
int cmd_chat_send(const char *params, bool query);
int cmd_finger(const char *params, bool query);

/* Security commands */
int cmd_auth(const char *params, bool query);
int cmd_authkey(const char *params, bool query);
int cmd_encrypt(const char *params, bool query);

#endif /* AT_COMMANDS_H_ */