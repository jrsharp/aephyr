# SPDX-License-Identifier: Apache-2.0

if(CONFIG_NET_L2_HEYMAC OR CONFIG_AETHER_MESH OR CONFIG_AETHER_LORA)
  zephyr_include_directories(include)
  add_subdirectory(subsys)
  
  if(CONFIG_AETHER_LORA)
    add_subdirectory(drivers)
  endif()
endif()