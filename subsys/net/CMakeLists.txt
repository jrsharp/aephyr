# SPDX-License-Identifier: MIT

if(CONFIG_NET_L2_HEYMAC OR CONFIG_AETHER_UART)
  add_subdirectory(l2)
endif()

if(CONFIG_AETHER_MESH)
  add_subdirectory(aether)
endif()