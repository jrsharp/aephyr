/*
 * Copyright (c) 2025 Æther Authors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(aether_shell, LOG_LEVEL_INF);

static int cmd_aether_status(const struct shell *shell, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    shell_print(shell, "Æther Mesh Status:");
    shell_print(shell, "  Version: 1.0.0");
    shell_print(shell, "  Stack: HeyMac + Æther");
    shell_print(shell, "  Transport: LoRa");

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

SHELL_STATIC_SUBCMD_SET_CREATE(sub_aether,
    SHELL_CMD(status, NULL, "Show Æther mesh status", cmd_aether_status),
    SHELL_CMD(send, NULL, "Send message", cmd_aether_send),
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(aether, &sub_aether, "Æther mesh commands", NULL);