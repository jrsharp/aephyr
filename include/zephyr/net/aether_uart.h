/*
 * SPDX-License-Identifier: MIT
 *
 * Aether UART L2 Transport
 *
 * Provides a simple UART-based L2 layer for Aether mesh networking,
 * useful for testing and serial interconnects between nodes.
 */

#ifndef ZEPHYR_INCLUDE_NET_AETHER_UART_H_
#define ZEPHYR_INCLUDE_NET_AETHER_UART_H_

#include <zephyr/net/net_l2.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Aether UART L2 layer
 *
 * This L2 layer provides UART transport for Aether mesh networking.
 * Frames are length-delimited with a 2-byte big-endian length prefix.
 */
#define AETHER_UART_L2 AETHER_UART
#define AETHER_UART_CTX_TYPE void*
NET_L2_DECLARE_PUBLIC(AETHER_UART);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_NET_AETHER_UART_H_ */
