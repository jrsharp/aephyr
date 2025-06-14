/*
 * Copyright (c) 2025 Æther Authors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/heymac.h>
#include <zephyr/net/aether_mesh.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

LOG_MODULE_REGISTER(aether_basic, LOG_LEVEL_INF);

static struct net_if *aether_iface;

static void print_status(void)
{
	struct heymac_addr addr;
	int net_id;
	int neighbor_count, route_count;

	if (!aether_iface) {
		LOG_ERR("Æther interface not available");
		return;
	}

	/* Get HeyMac status */
	if (heymac_get_addr(aether_iface, &addr) == 0) {
		if (heymac_addr_is_short(&addr)) {
			LOG_INF("Local address: %02x:%02x (short)",
				addr.short_addr[0], addr.short_addr[1]);
		} else {
			LOG_INF("Local address: %02x:%02x:%02x:%02x:%02x:%02x (long)",
				addr.long_addr[0], addr.long_addr[1], addr.long_addr[2],
				addr.long_addr[3], addr.long_addr[4], addr.long_addr[5]);
		}
	}

	net_id = heymac_get_network_id(aether_iface);
	if (net_id >= 0) {
		LOG_INF("Network ID: 0x%04x", net_id);
	}

	/* Get mesh status */
	neighbor_count = aether_mesh_get_neighbor_count(aether_iface);
	route_count = aether_mesh_get_route_count(aether_iface);
	
	LOG_INF("Mesh status: %d neighbors, %d routes", neighbor_count, route_count);
}

static void periodic_hello(void)
{
	if (aether_iface) {
		aether_mesh_send_hello(aether_iface);
		LOG_DBG("Periodic hello sent");
	}
}

int main(void)
{
	int ret;
	uint32_t hello_count = 0;

	LOG_INF("Æther Basic Node Starting");

	/* Wait for network interface to be available */
	aether_iface = net_if_get_default();
	if (!aether_iface) {
		LOG_ERR("No network interface available");
		return -ENODEV;
	}

	/* Wait for interface to be up */
	while (!net_if_is_up(aether_iface)) {
		LOG_INF("Waiting for network interface...");
		k_sleep(K_MSEC(500));
	}

	LOG_INF("Network interface is up");

	/* Initialize Æther mesh */
	ret = aether_mesh_init(aether_iface, 0x915E);  /* Use default network ID */
	if (ret < 0) {
		LOG_ERR("Failed to initialize Æther mesh: %d", ret);
		return ret;
	}

	LOG_INF("Æther mesh network ready");

	/* Print initial status */
	print_status();

	/* Main application loop */
	while (1) {
		/* Print status every 10 hello intervals */
		if ((hello_count % 10) == 0) {
			print_status();
		}

		/* Send periodic hello */
		periodic_hello();
		hello_count++;

		/* Sleep until next hello interval */
		k_sleep(K_SECONDS(CONFIG_AETHER_HELLO_INTERVAL));
	}

	return 0;
}

/* Shell commands for testing */
#ifdef CONFIG_SHELL

static int cmd_aether_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!aether_iface) {
		shell_error(sh, "Æther interface not available");
		return -ENODEV;
	}

	/* Print detailed status */
	struct heymac_addr addr;
	int net_id;
	int neighbor_count, route_count;

	shell_print(sh, "=== Æther Node Status ===");

	if (heymac_get_addr(aether_iface, &addr) == 0) {
		if (heymac_addr_is_short(&addr)) {
			shell_print(sh, "Local address: %02x:%02x (short)",
				    addr.short_addr[0], addr.short_addr[1]);
		} else {
			shell_print(sh, "Local address: %02x:%02x:%02x:%02x:%02x:%02x (long)",
				    addr.long_addr[0], addr.long_addr[1], addr.long_addr[2],
				    addr.long_addr[3], addr.long_addr[4], addr.long_addr[5]);
		}
	}

	net_id = heymac_get_network_id(aether_iface);
	if (net_id >= 0) {
		shell_print(sh, "Network ID: 0x%04x", net_id);
	}

	neighbor_count = aether_mesh_get_neighbor_count(aether_iface);
	route_count = aether_mesh_get_route_count(aether_iface);
	
	shell_print(sh, "Neighbors: %d", neighbor_count);
	shell_print(sh, "Routes: %d", route_count);

	return 0;
}

static int cmd_aether_hello(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!aether_iface) {
		shell_error(sh, "Æther interface not available");
		return -ENODEV;
	}

	int ret = aether_mesh_send_hello(aether_iface);
	if (ret < 0) {
		shell_error(sh, "Failed to send hello: %d", ret);
		return ret;
	}

	shell_print(sh, "Hello message sent");
	return 0;
}

static int cmd_aether_send(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t dst[6];
	const char *message;
	int ret;

	if (argc != 3) {
		shell_error(sh, "Usage: aether send <dst_addr> <message>");
		shell_print(sh, "Example: aether send 01:02:03:04:05:06 \"Hello World\"");
		return -EINVAL;
	}

	if (!aether_iface) {
		shell_error(sh, "Æther interface not available");
		return -ENODEV;
	}

	/* Parse destination address */
	ret = sscanf(argv[1], "%02x:%02x:%02x:%02x:%02x:%02x",
		     (unsigned int *)&dst[0], (unsigned int *)&dst[1], 
		     (unsigned int *)&dst[2], (unsigned int *)&dst[3],
		     (unsigned int *)&dst[4], (unsigned int *)&dst[5]);
	if (ret != 6) {
		shell_error(sh, "Invalid address format. Use: XX:XX:XX:XX:XX:XX");
		return -EINVAL;
	}

	message = argv[2];
	if (strlen(message) > CONFIG_AETHER_MAX_PAYLOAD) {
		shell_error(sh, "Message too long (max %d bytes)", CONFIG_AETHER_MAX_PAYLOAD);
		return -EMSGSIZE;
	}

	/* Send mesh message */
	ret = aether_mesh_send(aether_iface, dst, (const uint8_t *)message, 
			       strlen(message), AETHER_PRIORITY_NORMAL);
	if (ret < 0) {
		shell_error(sh, "Failed to send message: %d", ret);
		return ret;
	}

	shell_print(sh, "Message sent to %02x:%02x:%02x:%02x:%02x:%02x",
		    dst[0], dst[1], dst[2], dst[3], dst[4], dst[5]);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(aether_cmds,
	SHELL_CMD_ARG(status, NULL, "Show Æther node status", cmd_aether_status, 1, 0),
	SHELL_CMD_ARG(hello, NULL, "Send hello message", cmd_aether_hello, 1, 0),
	SHELL_CMD_ARG(send, NULL, "Send mesh message", cmd_aether_send, 3, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(aether, &aether_cmds, "Æther mesh commands", NULL);

#endif /* CONFIG_SHELL */