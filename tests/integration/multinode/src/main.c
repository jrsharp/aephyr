/*
 * Copyright (c) 2025 Æther Authors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Aether multinode integration test
 *
 * This test runs on multiple QEMU instances connected via virtual UARTs.
 * Each node:
 *   1. Initializes the Aether mesh over UART
 *   2. Sends periodic hello/test messages
 *   3. Reports received messages
 *
 * Use the run-multinode.sh script to launch connected instances.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/shell/shell.h>
#include <zephyr/random/random.h>
#include <string.h>

LOG_MODULE_REGISTER(multinode_test, LOG_LEVEL_INF);

/* Test configuration */
#define TEST_MSG_INTERVAL_MS  5000
#define TEST_MSG_MAX_LEN      64

/* Node identification (set via shell or auto-generated) */
static uint8_t node_addr[6];
static char node_name[16] = "node";
static bool mesh_ready = false;

/* Message statistics */
static uint32_t msgs_sent = 0;
static uint32_t msgs_received = 0;

/* Peer address for testing (broadcast by default) */
static uint8_t peer_addr[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/*
 * Aether mesh receive callback
 */
static void mesh_recv_callback(struct net_if *iface, const uint8_t src[6],
			       const uint8_t *data, size_t len, void *user_data)
{
	ARG_UNUSED(iface);
	ARG_UNUSED(user_data);

	msgs_received++;

	LOG_INF("RX from %02x:%02x:%02x:%02x:%02x:%02x: %.*s",
		src[0], src[1], src[2], src[3], src[4], src[5],
		(int)len, data);

	printk("\n[%s] << %.*s (from %02x:%02x)\n",
	       node_name, (int)len, data, src[4], src[5]);
}

/*
 * Send a test message
 */
static int send_test_message(const char *msg)
{
	struct net_if *iface;
	int ret;

	if (!mesh_ready) {
		LOG_WRN("Mesh not ready");
		return -EAGAIN;
	}

	iface = net_if_get_first_by_type(&NET_L2_GET_NAME(AETHER_UART));
	if (!iface) {
		LOG_ERR("No Aether UART interface found");
		return -ENODEV;
	}

	/* Import send function */
	extern int aether_mesh_send(struct net_if *iface, const uint8_t dst[6],
				    const uint8_t *data, size_t len, uint8_t priority);

	ret = aether_mesh_send(iface, peer_addr, (const uint8_t *)msg,
			       strlen(msg), 1 /* normal priority */);
	if (ret < 0) {
		LOG_ERR("Failed to send: %d", ret);
		return ret;
	}

	msgs_sent++;
	LOG_INF("TX to %02x:%02x:%02x:%02x:%02x:%02x: %s",
		peer_addr[0], peer_addr[1], peer_addr[2],
		peer_addr[3], peer_addr[4], peer_addr[5], msg);

	return 0;
}

/*
 * Initialize the mesh
 */
static int init_mesh(void)
{
	struct net_if *iface;
	int ret;

	/* Find the UART interface */
	iface = net_if_get_first_by_type(&NET_L2_GET_NAME(AETHER_UART));
	if (!iface) {
		LOG_ERR("No Aether UART interface");
		return -ENODEV;
	}

	/* Generate random node address */
	for (int i = 0; i < 6; i++) {
		node_addr[i] = sys_rand32_get() & 0xFF;
	}
	node_addr[0] |= 0x02;  /* Locally administered */

	/* Create node name from last 2 bytes of address */
	snprintf(node_name, sizeof(node_name), "node-%02x%02x",
		 node_addr[4], node_addr[5]);

	LOG_INF("Node address: %02x:%02x:%02x:%02x:%02x:%02x",
		node_addr[0], node_addr[1], node_addr[2],
		node_addr[3], node_addr[4], node_addr[5]);

	/* Initialize Aether mesh */
	extern int aether_mesh_init(struct net_if *iface, uint16_t mesh_net_id);
	ret = aether_mesh_init(iface, 0x0001);
	if (ret < 0) {
		LOG_ERR("Failed to init mesh: %d", ret);
		return ret;
	}

	/* Register receive callback */
	extern int aether_mesh_register_recv_callback(struct net_if *iface,
		void (*cb)(struct net_if *, const uint8_t *, const uint8_t *, size_t, void *),
		void *user_data);
	ret = aether_mesh_register_recv_callback(iface, mesh_recv_callback, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to register callback: %d", ret);
		return ret;
	}

	/* Enable the interface */
	net_if_up(iface);

	mesh_ready = true;
	LOG_INF("Mesh initialized on %s", node_name);

	return 0;
}

/*
 * Shell commands for interactive testing
 */
static int cmd_send(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(sh, "Usage: send <message>");
		return -EINVAL;
	}

	int ret = send_test_message(argv[1]);
	if (ret < 0) {
		shell_error(sh, "Send failed: %d", ret);
	} else {
		shell_print(sh, "Sent: %s", argv[1]);
	}
	return ret;
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "Node: %s", node_name);
	shell_print(sh, "Address: %02x:%02x:%02x:%02x:%02x:%02x",
		    node_addr[0], node_addr[1], node_addr[2],
		    node_addr[3], node_addr[4], node_addr[5]);
	shell_print(sh, "Mesh ready: %s", mesh_ready ? "yes" : "no");
	shell_print(sh, "Messages sent: %u", msgs_sent);
	shell_print(sh, "Messages received: %u", msgs_received);

	return 0;
}

static int cmd_peer(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(sh, "Peer: %02x:%02x:%02x:%02x:%02x:%02x",
			    peer_addr[0], peer_addr[1], peer_addr[2],
			    peer_addr[3], peer_addr[4], peer_addr[5]);
		shell_print(sh, "Usage: peer <addr> (e.g., peer ff:ff:ff:ff:ff:ff)");
		return 0;
	}

	/* Parse address (simplified - expects xx:xx:xx:xx:xx:xx) */
	if (sscanf(argv[1], "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
		   &peer_addr[0], &peer_addr[1], &peer_addr[2],
		   &peer_addr[3], &peer_addr[4], &peer_addr[5]) != 6) {
		shell_error(sh, "Invalid address format");
		return -EINVAL;
	}

	shell_print(sh, "Peer set to: %02x:%02x:%02x:%02x:%02x:%02x",
		    peer_addr[0], peer_addr[1], peer_addr[2],
		    peer_addr[3], peer_addr[4], peer_addr[5]);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(mesh_cmds,
	SHELL_CMD(send, NULL, "Send a message", cmd_send),
	SHELL_CMD(status, NULL, "Show node status", cmd_status),
	SHELL_CMD(peer, NULL, "Set/show peer address", cmd_peer),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(mesh, &mesh_cmds, "Aether mesh commands", NULL);

/*
 * Main entry point
 */
int main(void)
{
	int ret;
	int msg_count = 0;
	char msg_buf[TEST_MSG_MAX_LEN];

	LOG_INF("Aether multinode test starting...");

	/* Wait for system to settle */
	k_sleep(K_SECONDS(1));

	/* Initialize mesh */
	ret = init_mesh();
	if (ret < 0) {
		LOG_ERR("Failed to initialize: %d", ret);
		return ret;
	}

	printk("\n========================================\n");
	printk("Aether Multinode Test - %s\n", node_name);
	printk("Type 'mesh send <msg>' to send messages\n");
	printk("Type 'mesh status' to see statistics\n");
	printk("========================================\n\n");

	/* Periodic test message loop */
	while (1) {
		k_sleep(K_MSEC(TEST_MSG_INTERVAL_MS));

		if (mesh_ready) {
			snprintf(msg_buf, sizeof(msg_buf),
				 "Hello from %s #%d", node_name, ++msg_count);
			send_test_message(msg_buf);
		}
	}

	return 0;
}
