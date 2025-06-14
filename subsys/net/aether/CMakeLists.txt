# SPDX-License-Identifier: Apache-2.0

zephyr_sources(aether_mesh.c)
zephyr_sources(aether_route.c)

zephyr_sources_ifdef(CONFIG_AETHER_SHELL aether_shell.c)