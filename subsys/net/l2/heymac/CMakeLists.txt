# SPDX-License-Identifier: Apache-2.0

zephyr_sources_ifdef(CONFIG_NET_L2_HEYMAC
    heymac.c
    heymac_l2.c
    heymac_hsm.c
)