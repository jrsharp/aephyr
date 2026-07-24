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
#include <string.h>
#include <stdio.h>

LOG_MODULE_REGISTER(dect_phy, CONFIG_AETHER_DECT_LOG_LEVEL);

BUILD_ASSERT(CONFIG_AETHER_DECT_CARRIER,
	     "DECT NR+ PHY carrier must be set per local regulations "
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

/* The Type 1 header is written through the overlay above into the modem's PHY
 * header union, so it must fit; the addressed Type 2 form must stay 10 bytes
 * (it carries the receiver identity). These break the build loudly if the
 * modem header layout ever changes.
 */
BUILD_ASSERT(sizeof(union nrf_modem_dect_phy_hdr) >= sizeof(struct phy_ctrl_field_common),
	     "Type 1 header overlay does not fit the modem PHY header union");
BUILD_ASSERT(sizeof(((union nrf_modem_dect_phy_hdr *)0)->type_2) == 10,
	     "unexpected DECT NR+ PHY Type 2 header size");

/* DECT NR+ transport block sizes (in bits) indexed [MCS][packet_length], where
 * packet_length is the 4-bit PHY-header field counted in subslots. From ETSI
 * TS 103 636-3 section 5.3 (mirrors the NCS dect_shell dect_common_utils table).
 * -1 = unsupported combination. We size each frame to the minimal packet_length
 * whose block holds it, rather than a fixed CONFIG_AETHER_DECT_PACKET_LENGTH:
 * a hello stays small, a full 9P/data frame scales its airtime up on its own.
 * (LoRa had one packet size; DECT bills airtime per subslot, so right-size it.) */
static const int16_t dect_tbs_bits[5][16] = {
	/* MCS-0 */ { 0, 136, 264, 400, 536, 664, 792, 920, 1064, 1192, 1320, 1448, 1576, 1704, 1864, 1992 },
	/* MCS-1 */ { 32, 296, 552, 824, 1096, 1352, 1608, 1864, 2104, 2360, 2616, 2872, 3128, 3384, 3704, 3960 },
	/* MCS-2 */ { 56, 456, 856, 1256, 1640, 2024, 2360, 2744, 3192, 3576, 3960, 4320, 4768, 5152, 5536, -1 },
	/* MCS-3 */ { 88, 616, 1128, 1672, 2168, 2680, 3192, 3704, 4256, 4768, 5280, -1, -1, -1, -1, -1 },
	/* MCS-4 */ { 144, 936, 1736, 2488, 3256, 4024, 4832, 5600, -1, -1, -1, -1, -1, -1, -1, -1 },
};

/* Smallest subslot packet_length (0..15) whose transport block holds `len`
 * bytes at `mcs`. Clamps to the largest block the MCS supports on overflow --
 * the mesh caps payload at CONFIG_AETHER_MAX_PAYLOAD so that should not trip. */
static uint8_t dect_phy_packet_length_for(size_t len, uint8_t mcs)
{
	uint8_t last = 0;

	if (mcs >= 5) {
		mcs = 0;
	}
	for (uint8_t pl = 0; pl < 16; pl++) {
		int16_t bits = dect_tbs_bits[mcs][pl];

		if (bits < 0) {
			break;
		}
		last = pl;
		if ((size_t)(bits / 8) >= len) {
			return pl;
		}
	}
	return last;
}

/* Synchronizes blocking calls (init/configure/activate, TX, RSSI) with their
 * modem completion event. Receive is asynchronous (see rx_done_sem) so it does
 * not contend on this.
 */
static K_SEM_DEFINE(operation_sem, 0, 1);

/* Receive runs continuously and is only interrupted to transmit. rx_armed
 * tracks whether an RX operation is live in the modem; rx_done_sem is released
 * when that operation completes (its duration elapsed) or is canceled, so the
 * transmit path can wait for the radio to free up before keying up. rx_armed
 * is touched from both thread and modem-event context, hence volatile.
 */
static K_SEM_DEFINE(rx_done_sem, 0, 1);
static volatile bool rx_armed;

static uint16_t phy_device_id;
static uint64_t modem_time;
static bool init_failed;
static dect_phy_rx_cb_t rx_cb;

/* Runtime-tunable radio settings (defaults from Kconfig). */
static uint16_t cfg_carrier = CONFIG_AETHER_DECT_CARRIER;
static int8_t cfg_tx_power = CONFIG_AETHER_DECT_TX_POWER;

/* This node's 16-bit PHY identity. When set, it drives the transmitter ID, the
 * RX hardware filter, and selection of the addressed Type 2 PHY header.
 */
static uint16_t node_id;
static bool node_id_set;

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
/* last_temp_c starts at the modem's "not measured" sentinel so a pre-measurement
 * zero is never mistaken for a real 0 deg C reading. */
static struct dect_phy_stats stats = { .last_temp_c = NRF_MODEM_DECT_PHY_TEMP_NOT_MEASURED };

static const struct nrf_modem_dect_phy_config_params config_params = {
	/* Band group disambiguates the carrier: 0 = ~2 GHz (bands 1/2/9), 1 =
	 * ~1 GHz (bands 4/22). It must be explicit, because band 22's carriers
	 * (1691-1711) overlap band 9 (1703-1711) and band 2 (1680-1700), so the
	 * carrier number alone does not determine the frequency.
	 */
	.band_group_index = CONFIG_AETHER_DECT_BAND_GROUP,
	.harq_rx_process_count = 4,
	.harq_rx_expiry_time_us = 5000000,
};

/* Snapshot of the modem-reported supported bands, captured during init (while
 * the PHY is deactivated, as band_get requires) so the exact carrier range and
 * power class of each band can be confirmed on-device via the `dect bands`
 * shell command before transmitting.
 */
#define DECT_PHY_MAX_BANDS 8
static struct dect_phy_band_info cached_bands[DECT_PHY_MAX_BANDS];
static uint8_t cached_band_count;
static uint32_t cached_supported_bands;

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
	LOG_DBG("op_complete handle %u status %d", evt->handle, evt->err);

	/* Every completed operation reports the modem's temperature and supply
	 * voltage; keep the latest valid reading for telemetry (battery/thermal
	 * health in the field). Sentinels mean "not measured this time". */
	if (evt->temp != NRF_MODEM_DECT_PHY_TEMP_NOT_MEASURED) {
		stats.last_temp_c = evt->temp;
	}
	if (evt->voltage != NRF_MODEM_DECT_PHY_VOLTAGE_NOT_MEASURED) {
		stats.last_voltage_mv = evt->voltage;
	}

	if (evt->handle == DECT_PHY_RX_HANDLE) {
		/* The continuous receive operation ended (duration elapsed or
		 * canceled to make way for a transmit). Free the radio.
		 */
		rx_armed = false;
		k_sem_give(&rx_done_sem);
		return;
	}

	/* Synchronous operations (TX, RSSI): hand the result to the waiter. */
	last_op_err = evt->err;
	k_sem_give(&operation_sem);
}

static void on_pcc(const struct nrf_modem_dect_phy_pcc_event *evt)
{
	stats.rx_pcc++;
	last_pcc_tx_id = (uint16_t)(evt->hdr.hdr_type_1.transmitter_id_hi << 8 |
				    evt->hdr.hdr_type_1.transmitter_id_lo);

}

static void on_pdc(const struct nrf_modem_dect_phy_pdc_event *evt)
{
	/* RSSI-2 is in dBm with 0.5 dBm resolution (Q14.1); SNR is Q13.2. */
	int16_t rssi_dbm = evt->rssi_2 / 2;
	int8_t snr_db = (int8_t)(evt->snr / 4);

	stats.rx_frames++;
	stats.rx_bytes += evt->len;
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

static void on_bands(const struct nrf_modem_dect_phy_band_get_event *evt)
{
	if (evt->err) {
		LOG_WRN("band_get failed, err %d", evt->err);
		k_sem_give(&operation_sem);
		return;
	}

	cached_supported_bands = evt->supported_bands;
	cached_band_count = MIN(evt->band_count, DECT_PHY_MAX_BANDS);
	for (uint8_t i = 0; i < cached_band_count; i++) {
		cached_bands[i].band_number = evt->band[i].band_number;
		cached_bands[i].band_group_index = evt->band[i].band_group_index;
		cached_bands[i].power_class = evt->band[i].power_class;
		cached_bands[i].rx_gain = evt->band[i].rx_gain;
		cached_bands[i].min_carrier = evt->band[i].min_carrier;
		cached_bands[i].max_carrier = evt->band[i].max_carrier;
	}
	k_sem_give(&operation_sem);
}

static void log_supported_bands(void)
{
	LOG_INF("DECT NR+ PHY bands (supported bitmap 0x%08x):",
		(unsigned int)cached_supported_bands);
	for (uint8_t i = 0; i < cached_band_count; i++) {
		LOG_INF("  band %u: group %u, carriers %u-%u, power class %u",
			cached_bands[i].band_number, cached_bands[i].band_group_index,
			cached_bands[i].min_carrier, cached_bands[i].max_carrier,
			cached_bands[i].power_class);
	}
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
		/* Only the continuous receive operation is ever canceled. */
		rx_armed = false;
		k_sem_give(&rx_done_sem);
		break;
	case NRF_MODEM_DECT_PHY_EVT_PCC:
		on_pcc(&evt->pcc);
		break;
	case NRF_MODEM_DECT_PHY_EVT_PCC_ERROR:
		/* A PCC arrived but failed CRC. Kept as a debug-visible signal --
		 * these events were silently ignored before, which cost a full day
		 * of blind PHY debugging (see the Type-2 format-000 saga). */
		LOG_DBG("PCC CRC failure (rssi2=%d)", evt->pcc_crc_err.rssi_2 / 2);
		break;
	case NRF_MODEM_DECT_PHY_EVT_PDC:
		on_pdc(&evt->pdc);
		break;
	case NRF_MODEM_DECT_PHY_EVT_PDC_ERROR:
		LOG_DBG("PDC CRC failure (rssi2=%d)", evt->pdc_crc_err.rssi_2 / 2);
		break;
	case NRF_MODEM_DECT_PHY_EVT_RSSI:
		on_rssi(&evt->rssi);
		break;
	case NRF_MODEM_DECT_PHY_EVT_BANDS:
		on_bands(&evt->band_get);
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

void dect_phy_set_node_id(uint16_t id)
{
	node_id = id;
	node_id_set = true;
}

/* Transmitter ID to advertise: the node identity once set, else the hardware ID. */
static uint16_t tx_id(void)
{
	return node_id_set ? node_id : phy_device_id;
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

	/* Snapshot the supported bands while deactivated (band_get requires it),
	 * so the configured carrier range and power class can be confirmed via the
	 * `dect bands` shell command before transmitting. Non-fatal if it fails.
	 */
	err = nrf_modem_dect_phy_band_get();
	if (err) {
		LOG_WRN("band_get submit failed, err %d", err);
	} else {
		k_sem_take(&operation_sem, K_FOREVER);
		log_supported_bands();
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

int dect_phy_tx(uint16_t receiver_id, const uint8_t *data, size_t len)
{
	union nrf_modem_dect_phy_hdr phdr;
	int err;

	/* Use an addressed Type 2 header only for unicast (a specific receiver);
	 * broadcast frames (hellos, tree announcements) go out as a Type 1
	 * broadcast header so every node receives them regardless of identity.
	 */
	bool addressed = node_id_set && (receiver_id != DECT_PHY_BROADCAST_ID);

	memset(&phdr, 0, sizeof(phdr));

	/* Size the PHY transport block to this frame (subslots), not a fixed value. */
	uint8_t pkt_len = dect_phy_packet_length_for(len, CONFIG_AETHER_DECT_MCS);

	if (addressed) {
		/* Addressed Type 2 header: lets receivers filter in hardware.
		 * Header format 001 = "no HARQ feedback requested" (DECT-2020 Part 4
		 * Table 6.2.1-2a) -- reliability is the mesh layer's stop-and-wait
		 * ARQ, not PHY HARQ. Format 000 requests HARQ feedback the receiver
		 * must schedule, and format-000 frames were never delivered to our
		 * receivers in practice (PCC never surfaced, no CRC error); Nordic's
		 * dect_shell uses format 001 for its own non-HARQ addressed frames. */
		phdr.hdr_type_2.packet_length = pkt_len;
		phdr.hdr_type_2.packet_length_type = 0;
		phdr.hdr_type_2.header_format = 1;
		phdr.hdr_type_2.short_network_id = (CONFIG_AETHER_DECT_NETWORK_ID & 0xff);
		phdr.hdr_type_2.transmitter_id_hi = (tx_id() >> 8);
		phdr.hdr_type_2.transmitter_id_lo = (tx_id() & 0xff);
		phdr.hdr_type_2.df_mcs = CONFIG_AETHER_DECT_MCS;
		phdr.hdr_type_2.transmit_power = cfg_tx_power;
		phdr.hdr_type_2.receiver_id_hi = (receiver_id >> 8);
		phdr.hdr_type_2.receiver_id_lo = (receiver_id & 0xff);
		/* Format 001: HARQ fields (process/NDI/RV) are reserved bits --
		 * leave them zeroed (memset above). */
	} else {
		/* Broadcast Type 1 header (no receiver identity). */
		struct phy_ctrl_field_common *h1 = (struct phy_ctrl_field_common *)&phdr;

		h1->header_format = 0x0;
		h1->packet_length_type = 0x0;
		h1->packet_length = pkt_len;
		h1->short_network_id = (CONFIG_AETHER_DECT_NETWORK_ID & 0xff);
		h1->transmitter_id_hi = (tx_id() >> 8);
		h1->transmitter_id_lo = (tx_id() & 0xff);
		h1->transmit_power = cfg_tx_power;
		h1->df_mcs = CONFIG_AETHER_DECT_MCS;
	}

	struct nrf_modem_dect_phy_tx_params tx = {
		.start_time = 0,
		.handle = DECT_PHY_TX_HANDLE,
		.network_id = CONFIG_AETHER_DECT_NETWORK_ID,
		.phy_type = addressed ? 1 : 0, /* 0 = Type 1, 1 = Type 2 */
#ifdef CONFIG_AETHER_DECT_LBT_ENABLED
		.lbt_rssi_threshold_max = CONFIG_AETHER_DECT_LBT_THRESHOLD_DBM,
		.lbt_period = NRF_MODEM_DECT_LBT_PERIOD_MAX,
#else
		.lbt_rssi_threshold_max = 0,
		.lbt_period = 0,
#endif
		.carrier = cfg_carrier,
		.phy_header = &phdr,
		.data = (void *)data,
		.data_size = len,
	};

	/* Half-duplex radio: free it from continuous receive before keying up,
	 * then resume listening once the transmission resolves.
	 */
	bool resume = rx_armed;
	int result;

	if (resume) {
		dect_phy_rx_stop();
	}

	err = nrf_modem_dect_phy_tx(&tx);
	if (err) {
		LOG_WRN("dect_phy_tx submit failed, err %d", err);
		if (resume) {
			dect_phy_rx_ensure();
		}
		return err;
	}

	k_sem_take(&operation_sem, K_FOREVER);

	if (last_op_err == NRF_MODEM_DECT_PHY_ERR_LBT_CHANNEL_BUSY) {
		/* CSMA: channel was busy, frame not sent. Map to the busy code the
		 * upper layers already back off and retry on.
		 */
		stats.tx_busy++;
		result = -EBUSY;
	} else if (last_op_err != NRF_MODEM_DECT_PHY_SUCCESS) {
		LOG_WRN("TX completed with err %d", last_op_err);
		stats.tx_err++;
		result = -EIO;
	} else {
		stats.tx_ok++;
		stats.tx_bytes += len;
		result = 0;
	}

	if (resume) {
		dect_phy_rx_ensure();
	}
	return result;
}

/* Submit a continuous receive operation. Returns without blocking: decoded
 * frames flow to the receive callback, and the operation completes (then the
 * radio thread re-arms it via dect_phy_rx_ensure) when its duration elapses or
 * it is canceled to transmit. Each re-arm re-reads the carrier and node-id
 * filter, so a HONR join takes effect on the next cycle.
 */
static int rx_submit(void)
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
		.duration = (uint64_t)CONFIG_AETHER_DECT_RX_WINDOW_MS *
			    NRF_MODEM_DECT_MODEM_TIME_TICK_RATE_KHZ,
		.filter.short_network_id = CONFIG_AETHER_DECT_NETWORK_ID & 0xff,
		.filter.is_short_network_id_used = 1,
		/* Hardware-filter to this node's identity once set (0 = root /
		 * unset = promiscuous). Broadcast frames match regardless.
		 */
		.filter.receiver_identity = node_id_set ? node_id : 0,
	};

	err = nrf_modem_dect_phy_rx(&rx);
	if (err) {
		LOG_WRN("dect_phy_rx submit failed, err %d", err);
		return err;
	}

	rx_armed = true;
	return 0;
}

int dect_phy_rx_ensure(void)
{
	if (rx_armed) {
		return 0;
	}
	return rx_submit();
}

int dect_phy_rx_stop(void)
{
	if (!rx_armed) {
		return 0;
	}

	/* Cancel the live receive and wait for its completion event so the radio
	 * is idle before we transmit. The wait is bounded: if the operation
	 * already ended on its own, the cancel is a no-op with nothing to wait on.
	 */
	k_sem_reset(&rx_done_sem);
	(void)nrf_modem_dect_phy_cancel(DECT_PHY_RX_HANDLE);
	(void)k_sem_take(&rx_done_sem, K_MSEC(200));
	rx_armed = false;
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

	/* Free the radio from continuous receive for the measurement, then resume. */
	bool resume = rx_armed;

	if (resume) {
		dect_phy_rx_stop();
	}

	err = nrf_modem_dect_phy_rssi(&params);
	if (err) {
		LOG_WRN("dect_phy_rssi submit failed, err %d", err);
		if (resume) {
			dect_phy_rx_ensure();
		}
		return err;
	}

	k_sem_take(&operation_sem, K_FOREVER);

	if (resume) {
		dect_phy_rx_ensure();
	}

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

void dect_phy_format_carrier(uint16_t carrier, char *buf, size_t len)
{
	if (buf == NULL || len == 0) {
		return;
	}

	/* ETSI channel raster, integer kHz (no floating point in the modem app):
	 * Fc[kHz] = 450144 + carrier * 864. e.g. carrier 538 -> 914976 kHz. */
	uint32_t fc_khz = 450144u + (uint32_t)carrier * 864u;
	int band = -1;

	for (uint8_t i = 0; i < cached_band_count; i++) {
		if (carrier >= cached_bands[i].min_carrier &&
		    carrier <= cached_bands[i].max_carrier) {
			band = cached_bands[i].band_number;
			break;
		}
	}

	if (band >= 0) {
		snprintf(buf, len, "band %d, %u.%03u MHz", band,
			 fc_khz / 1000u, fc_khz % 1000u);
	} else {
		snprintf(buf, len, "carrier %u, %u.%03u MHz", carrier,
			 fc_khz / 1000u, fc_khz % 1000u);
	}
}

uint8_t dect_phy_get_bands(struct dect_phy_band_info *out, uint8_t max)
{
	uint8_t n = MIN(cached_band_count, max);

	if (out != NULL && n > 0) {
		memcpy(out, cached_bands, (size_t)n * sizeof(*out));
	}
	return n;
}
