# SPDX-License-Identifier: MIT

zephyr_sources(aether_mesh.c)
zephyr_sources(aether_route.c)
zephyr_sources(aether_reliable.c)

zephyr_sources_ifdef(CONFIG_AETHER_SHELL aether_shell.c)
zephyr_sources_ifdef(CONFIG_AETHER_SVCDISC aether_svcdisc.c)

if(CONFIG_AETHER_NET_FS)
  zephyr_sources(aether_net_fs.c)
  # The fs implements the consuming app's srv/mem contract (srv.h, mem.h
  # under <app>/src) — see the header comment in aether_net_fs.c.
  zephyr_include_directories(${APPLICATION_SOURCE_DIR}/src)
endif()