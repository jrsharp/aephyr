/*
 * Copyright (c) 2026 Æther Authors
 *
 * SPDX-License-Identifier: MIT
 *
 * Shell commands for inspecting the DECT NR+ PHY: identity and radio settings,
 * transmit/receive statistics, and a channel-activity (CAD) probe.
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/net/aether_mesh.h>
#include <stdlib.h>

#include "dect_phy.h"

#ifdef CONFIG_SHELL

static int cmd_dect_info(const struct shell *sh, size_t argc, char **argv)
{
	uint16_t device_id, carrier;
	int8_t tx_power;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	dect_phy_get_info(&device_id, &carrier, &tx_power);

	shell_print(sh, "DECT NR+ PHY:");
	shell_print(sh, "  device id  %u (0x%04x)", device_id, device_id);
	shell_print(sh, "  carrier    %u", carrier);
	shell_print(sh, "  network id %d", CONFIG_AETHER_DECT_NETWORK_ID);
	shell_print(sh, "  mcs        %d", CONFIG_AETHER_DECT_MCS);
	shell_print(sh, "  tx power   %d", tx_power);
#ifdef CONFIG_AETHER_DECT_LBT_ENABLED
	shell_print(sh, "  lbt        on, threshold %d dBm",
		    CONFIG_AETHER_DECT_LBT_THRESHOLD_DBM);
#else
	shell_print(sh, "  lbt        off");
#endif
	return 0;
}

static int cmd_dect_stats(const struct shell *sh, size_t argc, char **argv)
{
	struct dect_phy_stats stats;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	dect_phy_get_stats(&stats);

	shell_print(sh, "DECT NR+ PHY stats:");
	shell_print(sh, "  tx   %u ok, %u busy (LBT), %u err",
		    stats.tx_ok, stats.tx_busy, stats.tx_err);
	shell_print(sh, "  rx   %u frames, %u pcc (control headers)",
		    stats.rx_frames, stats.rx_pcc);
	shell_print(sh, "  last rssi %d dBm, snr %d dB",
		    stats.last_rssi_dbm, stats.last_snr_db);
	return 0;
}

static int cmd_dect_cad(const struct shell *sh, size_t argc, char **argv)
{
	int count = 10;
	int clear = 0, busy = 0, errors = 0;
	int ret;

	if (argc >= 2) {
		count = atoi(argv[1]);
		if (count < 1 || count > 100) {
			shell_error(sh, "count must be 1-100");
			return -EINVAL;
		}
	}

	shell_print(sh, "Sampling channel %d times...", count);

	ret = aether_lora_cad_test(count, &clear, &busy, &errors);
	if (ret < 0) {
		shell_error(sh, "channel probe failed: %d", ret);
		return ret;
	}

	shell_print(sh, "  %d clear, %d busy, %d errors (threshold %d dBm)",
		    clear, busy, errors, CONFIG_AETHER_DECT_LBT_THRESHOLD_DBM);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_dect,
	SHELL_CMD(info, NULL, "Show DECT NR+ PHY identity and settings", cmd_dect_info),
	SHELL_CMD(stats, NULL, "Show DECT NR+ PHY tx/rx statistics", cmd_dect_stats),
	SHELL_CMD_ARG(cad, NULL, "Probe channel activity [count=10]", cmd_dect_cad, 1, 1),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(dect, &sub_dect, "DECT NR+ PHY commands", NULL);

#endif /* CONFIG_SHELL */
