# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aether_basic)

if(CONF_FILE MATCHES "prj_minimal.conf")
    target_sources(app PRIVATE src/main_minimal.c)
elseif(CONF_FILE MATCHES "prj_heymac.conf")
    target_sources(app PRIVATE src/main_heymac.c)
else()
    target_sources(app PRIVATE src/main.c)
endif()