/*
 * Copyright (c) 2025 Æther Authors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(heymac_test, LOG_LEVEL_INF);

/* HeyMac Protocol Constants */
#define HEYMAC_NET_ID_915       0x915E  /* Network ID for 915MHz */
#define HEYMAC_FRAME_TYPE_DATA  0x80    /* Data frame type */
#define HEYMAC_ADDR_SHORT       0x40    /* Short address flag */

/* HeyMac Frame Structure (simplified) */
struct heymac_frame {
    uint16_t net_id;      /* Network ID (big endian) */
    uint8_t frame_type;   /* Frame type and flags */
    uint16_t dst_addr;    /* Destination address (big endian) */
    uint16_t src_addr;    /* Source address (big endian) */
    uint8_t payload[];    /* Payload data */
} __packed;

int main(void)
{
    const struct device *lora_dev;
    struct lora_modem_config config;
    int ret;
    int counter = 0;

    LOG_INF("HeyMac Test Starting");

    /* Get LoRa device */
    lora_dev = DEVICE_DT_GET(DT_ALIAS(lora0));
    if (!device_is_ready(lora_dev)) {
        LOG_ERR("LoRa device not ready");
        return -ENODEV;
    }

    LOG_INF("LoRa device ready");

    /* Configure LoRa modem with working settings */
    config.frequency = 915000000;
    config.bandwidth = BW_125_KHZ;
    config.datarate = SF_10;  /* SF10 works reliably */
    config.coding_rate = CR_4_5;
    config.preamble_len = 8;
    config.tx_power = 5;  /* Safe power level */
    config.tx = true;

    ret = lora_config(lora_dev, &config);
    if (ret < 0) {
        LOG_ERR("Failed to configure LoRa: %d", ret);
        return ret;
    }

    LOG_INF("LoRa configured:");
    LOG_INF("  Frequency: %u Hz", config.frequency);
    LOG_INF("  Bandwidth: %d", config.bandwidth);
    LOG_INF("  Spreading Factor: %d", config.datarate);
    LOG_INF("  Coding Rate: %d", config.coding_rate);
    LOG_INF("  TX Power: %d dBm", config.tx_power);

    /* Main loop - send HeyMac frames */
    while (1) {
        uint8_t frame_buffer[64];
        struct heymac_frame *frame = (struct heymac_frame *)frame_buffer;
        const char *message = "Hello HeyMac!";
        size_t payload_len = strlen(message);
        size_t frame_len = sizeof(struct heymac_frame) + payload_len;

        /* Construct HeyMac frame */
        frame->net_id = htons(HEYMAC_NET_ID_915);
        frame->frame_type = HEYMAC_FRAME_TYPE_DATA | HEYMAC_ADDR_SHORT;
        frame->dst_addr = htons(0xFFFF);  /* Broadcast */
        frame->src_addr = htons(0x0001);  /* Our address */
        memcpy(frame->payload, message, payload_len);

        LOG_INF("Sending HeyMac frame %d:", counter);
        LOG_INF("  Net ID: 0x%04X", ntohs(frame->net_id));
        LOG_INF("  Frame Type: 0x%02X", frame->frame_type);
        LOG_INF("  Dst Addr: 0x%04X", ntohs(frame->dst_addr));
        LOG_INF("  Src Addr: 0x%04X", ntohs(frame->src_addr));
        LOG_INF("  Payload: %s", message);
        LOG_INF("  Frame Length: %zu bytes", frame_len);

        /* Send frame */
        ret = lora_send(lora_dev, frame_buffer, frame_len);
        if (ret < 0) {
            LOG_ERR("Failed to send frame %d: %d", counter, ret);
        } else {
            LOG_INF("Successfully sent HeyMac frame %d", counter);
            counter++;
        }

        /* Wait before next transmission */
        k_sleep(K_SECONDS(10));
    }

    return 0;
}