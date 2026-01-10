# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aether_mesh_test)

target_sources(app PRIVATE src/main.c)

# Include just the mesh header parsing/building (extracted for testing)
target_sources(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../subsys/net/aether/aether_mesh.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../subsys/net/l2/heymac/heymac.c
)

target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
)
