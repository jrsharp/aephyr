# SPDX-License-Identifier: MIT

if(CONFIG_NET_L2_HEYMAC OR CONFIG_AETHER_MESH OR CONFIG_AETHER_LORA OR CONFIG_AETHER_DECT OR CONFIG_AETHER_UART)
  zephyr_include_directories(include)
  add_subdirectory(subsys)

  if(CONFIG_AETHER_LORA OR CONFIG_AETHER_DECT)
    add_subdirectory(drivers)
  endif()

  # Expose the DECT PHY driver header so the application (e.g. dect_metrics.c)
  # can read live modem telemetry (dect_phy_get_stats: LBT, tx_ok, temp, voltage).
  if(CONFIG_AETHER_DECT)
    zephyr_include_directories(drivers/dect)
  endif()
endif()