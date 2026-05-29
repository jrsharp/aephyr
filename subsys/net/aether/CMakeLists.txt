# SPDX-License-Identifier: MIT

zephyr_sources(aether_mesh.c)
zephyr_sources(aether_route.c)

zephyr_sources_ifdef(CONFIG_AETHER_ROUTING_HONR aether_honr.c)
zephyr_sources_ifdef(CONFIG_AETHER_SHELL aether_shell.c)