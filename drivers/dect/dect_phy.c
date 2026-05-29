/*
 * Copyright (c) 2026 Æther Authors
 *
 * SPDX-License-Identifier: MIT
 *
 * DECT NR+ PHY wrapper for the Æther DECT net-device driver. The
 * init/configure/activate handshake and the transmit/receive operation
 * structure follow the nRF Connect SDK "DECT NR+ PHY hello" sample,
 * generalized into reusable radio primitives with listen-before-talk.
 */

#include "dect_phy.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/hwinfo.h>
#include <nrf_modem_dect_phy.h>
#include <modem/nrf_modem_lib.h>

LOG_MODULE_REGISTER(dect_phy, CONFIG_AETHER_DECT_LOG_LEVEL);

BUILD_ASSERT(CONFIG_AETHER_DECT_CARRIER,
	     "DECT NR+ carrier must be set per local regulations "
	     "(CONFIG_AETHER_DECT_CARRIER)");

#define DECT_PHY_TX_HANDLE   0
#define DECT_PHY_RX_HANDLE   1
#define DECT_PHY_RSSI_HANDLE 2

/* PHY header type 1. Due to endianness the field order differs from the spec. */
struct phy_ctrl_field_common {
	uint32_t packet_length : 4;
	uint32_t packet_length_type : 1;
	uint32_t header_format : 3;
	uint32_t short_network_id : 8;
	uint32_t transmitter_id_hi : 8;
	uint32_t transmitter_id_lo : 8;
	uint32_t df_mcs : 3;
	uint32_t reserved : 1;
	uint32_t transmit_power : 4;
	uint32_t pad : 24;
};

/* Synchronizes blocking calls with their modem completion event. */
static K_SEM_DEFINE(operation_sem, 0, 1);

static uint16_t phy_device_id;
static uint64_t modem_time;
static bool init_failed;
static dect_phy_rx_cb_t rx_cb;

/* Runtime-tunable radio settings (defaults from Kconfig). */
static uint16_t cfg_carrier = CONFIG_AETHER_DECT_CARRIER;
static int8_t cfg_tx_power = CONFIG_AETHER_DECT_TX_POWER;

/* Result captured from the most recent operation-complete event, and the peak
 * RSSI captured from the most recent RSSI measurement. The radio is serialized
 * (one operation at a time), so these are written and read in lockstep with
 * operation_sem.
 */
static enum nrf_modem_dect_phy_err last_op_err;
static int8_t last_rssi_peak;

/* Transmitter ID from the most recent control-channel header, paired with the
 * data-channel payload that follows it; both accessed only in modem context.
 */
static uint16_t last_pcc_tx_id;

/* Operational statistics. Counters are incremented from the transmit path and
 * the modem event context; on a single-radio half-duplex link these do not
 * race, so plain accesses are sufficient for diagnostics.
 */
static struct dect_phy_stats stats;

static const struct nrf_modem_dect_phy_config_params config_params = {
	.band_group_index =
		((CONFIG_AETHER_DECT_CARRIER >= 525 && CONFIG_AETHER_DECT_CARRIER <= 551)) ? 1 : 0,
	.harq_rx_process_count = 4,
	.harq_rx_expiry_time_us = 5000000,
};

static void on_init(const struct nrf_modem_dect_phy_init_event *evt)
{
	if (evt->err) {
		LOG_ERR("Init failed, err %d", evt->err);
		init_failed = true;
		return;
	}
	k_sem_give(&operation_sem);
}

static void on_configure(const struct nrf_modem_dect_phy_configure_event *evt)
{
	if (evt->err) {
		LOG_ERR("Configure failed, err %d", evt->err);
		init_failed = true;
		return;
	}
	k_sem_give(&operation_sem);
}

static void on_activate(const struct nrf_modem_dect_phy_activate_event *evt)
{
	if (evt->err) {
		LOG_ERR("Activate failed, err %d", evt->err);
		init_failed = true;
		return;
	}
	k_sem_give(&operation_sem);
}

static void on_op_complete(const struct nrf_modem_dect_phy_op_complete_event *evt)
{
	last_op_err = evt->err;
	LOG_DBG("op_complete handle %u status %d", evt->handle, evt->err);
	k_sem_give(&operation_sem);
}

static void on_pcc(const struct nrf_modem_dect_phy_pcc_event *evt)
{
	last_pcc_tx_id = (uint16_t)(evt->hdr.hdr_type_1.transmitter_id_hi << 8 |
				    evt->hdr.hdr_type_1.transmitter_id_lo);
}

static void on_pdc(const struct nrf_modem_dect_phy_pdc_event *evt)
{
	/* RSSI-2 is in dBm with 0.5 dBm resolution (Q14.1); SNR is Q13.2. */
	int16_t rssi_dbm = evt->rssi_2 / 2;
	int8_t snr_db = (int8_t)(evt->snr / 4);

	stats.rx_frames++;
	stats.last_rssi_dbm = rssi_dbm;
	stats.last_snr_db = snr_db;

	if (rx_cb != NULL && evt->data != NULL && evt->len > 0) {
		rx_cb((const uint8_t *)evt->data, evt->len, rssi_dbm, snr_db, last_pcc_tx_id);
	}
}

static void on_rssi(const struct nrf_modem_dect_phy_rssi_event *evt)
{
	/* int8_t floor; RSSI measurements are reported as int8_t dBm. */
	int8_t peak = INT8_MIN;

	for (uint16_t i = 0; i < evt->meas_len; i++) {
		int8_t m = evt->meas[i];

		/* Skip not-measured symbols; treat saturation (positive) as peak. */
		if (m == NRF_MODEM_DECT_PHY_RSSI_NOT_MEASURED) {
			continue;
		}
		if (m > peak) {
			peak = m;
		}
	}

	last_rssi_peak = peak;
}

static void dect_phy_event_handler(const struct nrf_modem_dect_phy_event *evt)
{
	modem_time = evt->time;

	switch (evt->id) {
	case NRF_MODEM_DECT_PHY_EVT_INIT:
		on_init(&evt->init);
		break;
	case NRF_MODEM_DECT_PHY_EVT_CONFIGURE:
		on_configure(&evt->configure);
		break;
	case NRF_MODEM_DECT_PHY_EVT_ACTIVATE:
		on_activate(&evt->activate);
		break;
	case NRF_MODEM_DECT_PHY_EVT_COMPLETED:
		on_op_complete(&evt->op_complete);
		break;
	case NRF_MODEM_DECT_PHY_EVT_CANCELED:
		k_sem_give(&operation_sem);
		break;
	case NRF_MODEM_DECT_PHY_EVT_PCC:
		on_pcc(&evt->pcc);
		break;
	case NRF_MODEM_DECT_PHY_EVT_PDC:
		on_pdc(&evt->pdc);
		break;
	case NRF_MODEM_DECT_PHY_EVT_RSSI:
		on_rssi(&evt->rssi);
		break;
	default:
		break;
	}
}

void dect_phy_set_rx_cb(dect_phy_rx_cb_t cb)
{
	rx_cb = cb;
}

void dect_phy_set_carrier(uint16_t carrier)
{
	cfg_carrier = carrier;
}

void dect_phy_set_tx_power(int8_t power)
{
	cfg_tx_power = power;
}

int dect_phy_init(uint16_t *device_id)
{
	int err;

	err = nrf_modem_lib_init();
	if (err) {
		LOG_ERR("nrf_modem_lib_init failed, err %d", err);
		return err;
	}

	err = nrf_modem_dect_phy_event_handler_set(dect_phy_event_handler);
	if (err) {
		LOG_ERR("event_handler_set failed, err %d", err);
		return err;
	}

	err = nrf_modem_dect_phy_init();
	if (err) {
		LOG_ERR("dect_phy_init failed, err %d", err);
		return err;
	}
	k_sem_take(&operation_sem, K_FOREVER);
	if (init_failed) {
		return -EIO;
	}

	err = nrf_modem_dect_phy_configure(&config_params);
	if (err) {
		LOG_ERR("dect_phy_configure failed, err %d", err);
		return err;
	}
	k_sem_take(&operation_sem, K_FOREVER);
	if (init_failed) {
		return -EIO;
	}

	err = nrf_modem_dect_phy_activate(NRF_MODEM_DECT_PHY_RADIO_MODE_LOW_LATENCY);
	if (err) {
		LOG_ERR("dect_phy_activate failed, err %d", err);
		return err;
	}
	k_sem_take(&operation_sem, K_FOREVER);
	if (init_failed) {
		return -EIO;
	}

	hwinfo_get_device_id((void *)&phy_device_id, sizeof(phy_device_id));
	if (device_id != NULL) {
		*device_id = phy_device_id;
	}

	LOG_INF("DECT NR+ PHY ready, device ID %u, carrier %u", phy_device_id, cfg_carrier);
	return 0;
}

int dect_phy_tx(const uint8_t *data, size_t len)
{
	int err;

	struct phy_ctrl_field_common header = {
		.header_format = 0x0,
		.packet_length_type = 0x0,
		.packet_length = CONFIG_AETHER_DECT_PACKET_LENGTH,
		.short_network_id = (CONFIG_AETHER_DECT_NETWORK_ID & 0xff),
		.transmitter_id_hi = (phy_device_id >> 8),
		.transmitter_id_lo = (phy_device_id & 0xff),
		.transmit_power = cfg_tx_power,
		.reserved = 0,
		.df_mcs = CONFIG_AETHER_DECT_MCS,
	};

	struct nrf_modem_dect_phy_tx_params tx = {
		.start_time = 0,
		.handle = DECT_PHY_TX_HANDLE,
		.network_id = CONFIG_AETHER_DECT_NETWORK_ID,
		.phy_type = 0,
#ifdef CONFIG_AETHER_DECT_LBT_ENABLED
		.lbt_rssi_threshold_max = CONFIG_AETHER_DECT_LBT_THRESHOLD_DBM,
		.lbt_period = NRF_MODEM_DECT_LBT_PERIOD_MAX,
#else
		.lbt_rssi_threshold_max = 0,
		.lbt_period = 0,
#endif
		.carrier = cfg_carrier,
		.phy_header = (union nrf_modem_dect_phy_hdr *)&header,
		.data = (void *)data,
		.data_size = len,
	};

	err = nrf_modem_dect_phy_tx(&tx);
	if (err) {
		LOG_WRN("dect_phy_tx submit failed, err %d", err);
		return err;
	}

	k_sem_take(&operation_sem, K_FOREVER);

	if (last_op_err == NRF_MODEM_DECT_PHY_ERR_LBT_CHANNEL_BUSY) {
		/* CSMA: channel was busy, frame not sent. Map to the busy code the
		 * upper layers already back off and retry on.
		 */
		stats.tx_busy++;
		return -EBUSY;
	}
	if (last_op_err != NRF_MODEM_DECT_PHY_SUCCESS) {
		LOG_WRN("TX completed with err %d", last_op_err);
		stats.tx_err++;
		return -EIO;
	}

	stats.tx_ok++;
	return 0;
}

int dect_phy_rx_window(uint32_t duration_ms)
{
	int err;

	struct nrf_modem_dect_phy_rx_params rx = {
		.start_time = 0,
		.handle = DECT_PHY_RX_HANDLE,
		.network_id = CONFIG_AETHER_DECT_NETWORK_ID,
		.mode = NRF_MODEM_DECT_PHY_RX_MODE_CONTINUOUS,
		.rssi_interval = NRF_MODEM_DECT_PHY_RSSI_INTERVAL_OFF,
		.link_id = NRF_MODEM_DECT_PHY_LINK_UNSPECIFIED,
		.rssi_level = -60,
		.carrier = cfg_carrier,
		.duration = (uint64_t)duration_ms *
			    NRF_MODEM_DECT_MODEM_TIME_TICK_RATE_KHZ,
		.filter.short_network_id = CONFIG_AETHER_DECT_NETWORK_ID & 0xff,
		.filter.is_short_network_id_used = 1,
		.filter.receiver_identity = 0,
	};

	err = nrf_modem_dect_phy_rx(&rx);
	if (err) {
		LOG_WRN("dect_phy_rx submit failed, err %d", err);
		return err;
	}

	k_sem_take(&operation_sem, K_FOREVER);
	return 0;
}

int dect_phy_channel_rssi(int8_t *rssi_dbm_out)
{
	int err;

	struct nrf_modem_dect_phy_rssi_params params = {
		.start_time = 0,
		.handle = DECT_PHY_RSSI_HANDLE,
		.carrier = cfg_carrier,
		.duration = 24, /* subslots (~1 ms) */
		.reporting_interval = NRF_MODEM_DECT_PHY_RSSI_INTERVAL_12_SLOTS,
	};

	last_rssi_peak = INT8_MIN;

	err = nrf_modem_dect_phy_rssi(&params);
	if (err) {
		LOG_WRN("dect_phy_rssi submit failed, err %d", err);
		return err;
	}

	k_sem_take(&operation_sem, K_FOREVER);

	if (rssi_dbm_out != NULL) {
		*rssi_dbm_out = last_rssi_peak;
	}
	return 0;
}

void dect_phy_get_stats(struct dect_phy_stats *out)
{
	if (out != NULL) {
		*out = stats;
	}
}

void dect_phy_get_info(uint16_t *device_id, uint16_t *carrier, int8_t *tx_power)
{
	if (device_id != NULL) {
		*device_id = phy_device_id;
	}
	if (carrier != NULL) {
		*carrier = cfg_carrier;
	}
	if (tx_power != NULL) {
		*tx_power = cfg_tx_power;
	}
}
