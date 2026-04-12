/*
 * Copyright (c) 2025 Æther Authors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/aether_mesh.h>
#include <zephyr/net/heymac.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(aether_shell, LOG_LEVEL_INF);

/* Global mesh context from aether_mesh.c */
extern struct aether_mesh_ctx *g_mesh_ctx;

static int cmd_aether_status(const struct shell *shell, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    struct aether_mesh_ctx *ctx = g_mesh_ctx;

    if (!ctx || !ctx->iface) {
        shell_print(shell, "Æther mesh not initialized");
        return 0;
    }

    /* Local address */
    shell_print(shell, "Æther Mesh Status:");
    shell_print(shell, "  addr   %02x:%02x:%02x:%02x:%02x:%02x",
                ctx->local_addr[0], ctx->local_addr[1],
                ctx->local_addr[2], ctx->local_addr[3],
                ctx->local_addr[4], ctx->local_addr[5]);
    shell_print(shell, "  net_id 0x%04x", ctx->mesh_net_id);

    /* HeyMac layer info */
    struct heymac_context *hctx = net_if_l2_data(ctx->iface);
    if (hctx) {
        if (heymac_addr_is_short(&hctx->local_addr)) {
            shell_print(shell, "  heymac %02x:%02x (short)",
                        hctx->local_addr.short_addr[0],
                        hctx->local_addr.short_addr[1]);
        } else {
            shell_print(shell, "  heymac %02x:%02x:%02x:%02x:%02x:%02x (long)",
                        hctx->local_addr.long_addr[0],
                        hctx->local_addr.long_addr[1],
                        hctx->local_addr.long_addr[2],
                        hctx->local_addr.long_addr[3],
                        hctx->local_addr.long_addr[4],
                        hctx->local_addr.long_addr[5]);
        }
        shell_print(shell, "  rx/tx  %u/%u pkts, %u/%u err",
                    hctx->rx_packets, hctx->tx_packets,
                    hctx->rx_errors, hctx->tx_errors);
    }

    /* Neighbors */
    int neighbors = aether_mesh_get_neighbor_count(ctx->iface);
    shell_print(shell, "  neighbors %d", neighbors);
    for (int i = 0; i < CONFIG_AETHER_MAX_NEIGHBORS; i++) {
        if (ctx->neighbors[i].active) {
            uint32_t age = (k_uptime_get_32() -
                           ctx->neighbors[i].last_seen) / 1000;
            shell_print(shell, "    %02x:%02x:%02x:%02x:%02x:%02x  "
                        "rssi %d  %us ago",
                        ctx->neighbors[i].addr[0],
                        ctx->neighbors[i].addr[1],
                        ctx->neighbors[i].addr[2],
                        ctx->neighbors[i].addr[3],
                        ctx->neighbors[i].addr[4],
                        ctx->neighbors[i].addr[5],
                        ctx->neighbors[i].rssi,
                        age);
        }
    }

    /* Routes */
    int routes = aether_mesh_get_route_count(ctx->iface);
    shell_print(shell, "  routes %d", routes);
    for (int i = 0; i < CONFIG_AETHER_MAX_ROUTES; i++) {
        if (ctx->routes[i].active) {
            shell_print(shell, "    %02x:%02x:%02x:%02x:%02x:%02x  "
                        "via %02x:%02x  hops %d",
                        ctx->routes[i].dest[0],
                        ctx->routes[i].dest[1],
                        ctx->routes[i].dest[2],
                        ctx->routes[i].dest[3],
                        ctx->routes[i].dest[4],
                        ctx->routes[i].dest[5],
                        ctx->routes[i].next_hop[4],
                        ctx->routes[i].next_hop[5],
                        ctx->routes[i].hop_count);
        }
    }

    /* Mesh stats */
    shell_print(shell, "  hello  sent %u, recv %u",
                ctx->hello_sent, ctx->hello_received);
    shell_print(shell, "  fwd %u, drop %u",
                ctx->packets_forwarded, ctx->packets_dropped);

    return 0;
}

static int cmd_aether_send(const struct shell *shell, size_t argc, char **argv)
{
    if (argc < 2) {
        shell_error(shell, "Usage: aether send <message>");
        return -EINVAL;
    }

    shell_print(shell, "Sending message: %s", argv[1]);
    LOG_INF("Shell command: send message '%s'", argv[1]);

    return 0;
}

#ifdef CONFIG_AETHER_LORA_CAD_ENABLED
static int cmd_aether_cad(const struct shell *shell, size_t argc, char **argv)
{
    int count = 10;
    int clear, busy, errors;

    if (argc >= 2) {
        count = atoi(argv[1]);
        if (count < 1 || count > 100) {
            shell_error(shell, "Count must be 1-100");
            return -EINVAL;
        }
    }

    shell_print(shell, "Running %d CAD checks...", count);

    int ret = aether_lora_cad_test(count, &clear, &busy, &errors);
    if (ret < 0) {
        shell_error(shell, "CAD test failed: %d", ret);
        return ret;
    }

    shell_print(shell, "CAD results: %d clear, %d busy, %d errors "
                "(of %d total)", clear, busy, errors, count);

    if (busy > 0 && clear == 0) {
        shell_print(shell, "  -> 100%% busy: likely false positives "
                    "or interference");
    } else if (busy == 0) {
        shell_print(shell, "  -> Channel appears clear");
    }

    return 0;
}
#endif /* CONFIG_AETHER_LORA_CAD_ENABLED */

SHELL_STATIC_SUBCMD_SET_CREATE(sub_aether,
    SHELL_CMD(status, NULL, "Show Æther mesh status", cmd_aether_status),
    SHELL_CMD(send, NULL, "Send message", cmd_aether_send),
#ifdef CONFIG_AETHER_LORA_CAD_ENABLED
    SHELL_CMD(cad, NULL, "Run CAD test [count=10]", cmd_aether_cad),
#endif
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(aether, &sub_aether, "Æther mesh commands", NULL);
