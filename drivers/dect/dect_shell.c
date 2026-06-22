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

static int cmd_dect_bands(const struct shell *sh, size_t argc, char **argv)
{
	struct dect_phy_band_info bands[8];
	uint8_t n;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	n = dect_phy_get_bands(bands, ARRAY_SIZE(bands));
	if (n == 0) {
		shell_print(sh, "No band info reported by the modem.");
		return 0;
	}

	shell_print(sh, "DECT NR+ PHY bands (modem-reported):");
	for (uint8_t i = 0; i < n; i++) {
		shell_print(sh,
			    "  band %2u  group %u (%s)  carriers %u-%u  power class %u  rx_gain %d dB",
			    bands[i].band_number, bands[i].band_group_index,
			    bands[i].band_group_index ? "~1 GHz" : "~2 GHz",
			    bands[i].min_carrier, bands[i].max_carrier,
			    bands[i].power_class, bands[i].rx_gain);
	}
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

static int cmd_dect_txpower(const struct shell *sh, size_t argc, char **argv)
{
	uint16_t device_id, carrier;
	int8_t tx_power;
	int idx;

	/* No arg: report the current index. */
	if (argc < 2) {
		dect_phy_get_info(&device_id, &carrier, &tx_power);
		shell_print(sh, "tx power index %d", tx_power);
		return 0;
	}

	idx = atoi(argv[1]);
	if (idx < 0 || idx > 15) {
		shell_error(sh, "tx power index must be 0-15 (ETSI TS 103 636-4 "
			    "power-table index; lower = less power)");
		return -EINVAL;
	}

	/* Takes effect on the next transmission (the index is written into each
	 * frame's PHY control header). Runtime knob for field range tuning;
	 * not persisted -- reverts to CONFIG_AETHER_DECT_TX_POWER on reboot. */
	dect_phy_set_tx_power((int8_t)idx);
	shell_print(sh, "tx power index -> %d (applies to next TX; not persisted)", idx);
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
	SHELL_CMD(bands, NULL, "Show modem-reported bands (carrier ranges, power class)",
		  cmd_dect_bands),
	SHELL_CMD(stats, NULL, "Show DECT NR+ PHY tx/rx statistics", cmd_dect_stats),
	SHELL_CMD_ARG(txpower, NULL, "Get/set TX power index 0-15 (runtime; field range tuning)",
		      cmd_dect_txpower, 1, 1),
	SHELL_CMD_ARG(cad, NULL, "Probe channel activity [count=10]", cmd_dect_cad, 1, 1),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(dect, &sub_dect, "DECT NR+ PHY commands", NULL);

#endif /* CONFIG_SHELL */
