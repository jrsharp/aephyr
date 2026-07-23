# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aether_svcdisc_test)

# Test the real module source, not a copy.
target_sources(app PRIVATE
    src/main.c
    ../../../subsys/net/aether/aether_svcdisc.c
)

target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
)

# The module normally gets these from the aephyr Kconfig; the test app
# builds it standalone.
target_compile_definitions(app PRIVATE
    CONFIG_AETHER_MESH_LOG_LEVEL=0
)
