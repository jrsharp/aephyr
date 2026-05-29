# SPDX-License-Identifier: MIT

zephyr_sources_ifdef(CONFIG_AETHER_DECT
	aether_dect.c
	dect_phy.c
	dect_shell.c
)
