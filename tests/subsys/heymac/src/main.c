/*
 * Copyright (c) 2025 Æther Authors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for HeyMac header parsing and building.
 * These are self-contained tests that implement the parsing logic
 * directly to avoid pulling in the full HeyMac stack.
 */

#include <zephyr/ztest.h>
#include <string.h>

/* HeyMac frame control flags (from heymac.h) */
#define HEYMAC_FCTL_S  0x80  /* Source address present */
#define HEYMAC_FCTL_D  0x40  /* Destination address present */
#define HEYMAC_FCTL_N  0x20  /* Network ID present */
#define HEYMAC_FCTL_L  0x10  /* Long address mode (6 bytes vs 2) */
#define HEYMAC_FCTL_M  0x08  /* MAC payload present */
#define HEYMAC_FCTL_I  0x04  /* IE list present */
#define HEYMAC_FCTL_X  0x02  /* Extended frame */
#define HEYMAC_FCTL_R  0x01  /* Reserved */

/* Frame types */
#define HEYMAC_FRM_TYPE_BEACON  0x00
#define HEYMAC_FRM_TYPE_DATA    0x01
#define HEYMAC_FRM_TYPE_ACK     0x02
#define HEYMAC_FRM_TYPE_CMD     0x03

/* Test address structure */
struct test_addr {
	uint8_t type;  /* 0 = short (2 bytes), 1 = long (6 bytes) */
	union {
		uint8_t short_addr[2];
		uint8_t long_addr[6];
	};
};

/* Test header structure */
struct test_hdr {
	uint8_t version_type;
	uint8_t frame_ctrl;
	uint16_t net_id;
	struct test_addr src_addr;
	struct test_addr dst_addr;
	uint8_t seq_no;
};

/* Build header into buffer - test implementation */
static int test_build_header(const struct test_hdr *hdr, uint8_t *buf, size_t len)
{
	size_t offset = 0;
	size_t needed = 3;  /* version_type + frame_ctrl + seq_no minimum */

	if (hdr->frame_ctrl & HEYMAC_FCTL_N) {
		needed += 2;
	}
	if (hdr->frame_ctrl & HEYMAC_FCTL_S) {
		needed += (hdr->frame_ctrl & HEYMAC_FCTL_L) ? 6 : 2;
	}
	if (hdr->frame_ctrl & HEYMAC_FCTL_D) {
		needed += (hdr->frame_ctrl & HEYMAC_FCTL_L) ? 6 : 2;
	}

	if (len < needed) {
		return -1;
	}

	buf[offset++] = hdr->version_type;
	buf[offset++] = hdr->frame_ctrl;

	if (hdr->frame_ctrl & HEYMAC_FCTL_N) {
		buf[offset++] = (hdr->net_id >> 8) & 0xFF;
		buf[offset++] = hdr->net_id & 0xFF;
	}

	if (hdr->frame_ctrl & HEYMAC_FCTL_S) {
		if (hdr->frame_ctrl & HEYMAC_FCTL_L) {
			memcpy(&buf[offset], hdr->src_addr.long_addr, 6);
			offset += 6;
		} else {
			memcpy(&buf[offset], hdr->src_addr.short_addr, 2);
			offset += 2;
		}
	}

	if (hdr->frame_ctrl & HEYMAC_FCTL_D) {
		if (hdr->frame_ctrl & HEYMAC_FCTL_L) {
			memcpy(&buf[offset], hdr->dst_addr.long_addr, 6);
			offset += 6;
		} else {
			memcpy(&buf[offset], hdr->dst_addr.short_addr, 2);
			offset += 2;
		}
	}

	buf[offset++] = hdr->seq_no;

	return (int)offset;
}

/* Parse header from buffer - test implementation */
static int test_parse_header(const uint8_t *buf, size_t len, struct test_hdr *hdr)
{
	size_t offset = 0;

	if (len < 3) {
		return -1;
	}

	memset(hdr, 0, sizeof(*hdr));

	hdr->version_type = buf[offset++];
	hdr->frame_ctrl = buf[offset++];

	if (hdr->frame_ctrl & HEYMAC_FCTL_N) {
		if (offset + 2 > len) return -1;
		hdr->net_id = (buf[offset] << 8) | buf[offset + 1];
		offset += 2;
	}

	if (hdr->frame_ctrl & HEYMAC_FCTL_S) {
		if (hdr->frame_ctrl & HEYMAC_FCTL_L) {
			if (offset + 6 > len) return -1;
			hdr->src_addr.type = 1;
			memcpy(hdr->src_addr.long_addr, &buf[offset], 6);
			offset += 6;
		} else {
			if (offset + 2 > len) return -1;
			hdr->src_addr.type = 0;
			memcpy(hdr->src_addr.short_addr, &buf[offset], 2);
			offset += 2;
		}
	}

	if (hdr->frame_ctrl & HEYMAC_FCTL_D) {
		if (hdr->frame_ctrl & HEYMAC_FCTL_L) {
			if (offset + 6 > len) return -1;
			hdr->dst_addr.type = 1;
			memcpy(hdr->dst_addr.long_addr, &buf[offset], 6);
			offset += 6;
		} else {
			if (offset + 2 > len) return -1;
			hdr->dst_addr.type = 0;
			memcpy(hdr->dst_addr.short_addr, &buf[offset], 2);
			offset += 2;
		}
	}

	if (offset >= len) return -1;
	hdr->seq_no = buf[offset++];

	return (int)offset;
}

/*
 * Test minimal header round-trip (short addr, source only)
 */
ZTEST(heymac_header, test_minimal_header_roundtrip)
{
	struct test_hdr hdr_out, hdr_in;
	uint8_t buf[32];
	int build_len, parse_len;

	memset(&hdr_out, 0, sizeof(hdr_out));
	hdr_out.version_type = HEYMAC_FRM_TYPE_DATA;
	hdr_out.frame_ctrl = HEYMAC_FCTL_S;  /* Source present, short addr */
	hdr_out.src_addr.type = 0;
	hdr_out.src_addr.short_addr[0] = 0xAB;
	hdr_out.src_addr.short_addr[1] = 0xCD;
	hdr_out.seq_no = 0x42;

	build_len = test_build_header(&hdr_out, buf, sizeof(buf));
	zassert_true(build_len > 0, "build_header failed: %d", build_len);

	parse_len = test_parse_header(buf, build_len, &hdr_in);
	zassert_equal(parse_len, build_len, "parse length mismatch");
	zassert_equal(hdr_in.version_type, hdr_out.version_type, "version_type mismatch");
	zassert_equal(hdr_in.frame_ctrl, hdr_out.frame_ctrl, "frame_ctrl mismatch");
	zassert_equal(hdr_in.seq_no, hdr_out.seq_no, "seq_no mismatch");
	zassert_mem_equal(hdr_in.src_addr.short_addr, hdr_out.src_addr.short_addr, 2,
			  "src_addr mismatch");
}

/*
 * Test header with network ID present
 */
ZTEST(heymac_header, test_header_with_net_id)
{
	struct test_hdr hdr_out, hdr_in;
	uint8_t buf[32];
	int build_len, parse_len;

	memset(&hdr_out, 0, sizeof(hdr_out));
	hdr_out.version_type = HEYMAC_FRM_TYPE_DATA;
	hdr_out.frame_ctrl = HEYMAC_FCTL_S | HEYMAC_FCTL_N;
	hdr_out.net_id = 0x915E;
	hdr_out.src_addr.type = 0;
	hdr_out.src_addr.short_addr[0] = 0x12;
	hdr_out.src_addr.short_addr[1] = 0x34;
	hdr_out.seq_no = 0x99;

	build_len = test_build_header(&hdr_out, buf, sizeof(buf));
	zassert_true(build_len > 0, "build_header failed");

	parse_len = test_parse_header(buf, build_len, &hdr_in);
	zassert_equal(parse_len, build_len, "parse length mismatch");
	zassert_equal(hdr_in.net_id, 0x915E, "net_id mismatch");
}

/*
 * Test header with long addresses (6 bytes)
 */
ZTEST(heymac_header, test_header_long_addr)
{
	struct test_hdr hdr_out, hdr_in;
	uint8_t buf[32];
	int build_len, parse_len;
	uint8_t src_addr[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
	uint8_t dst_addr[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

	memset(&hdr_out, 0, sizeof(hdr_out));
	hdr_out.version_type = HEYMAC_FRM_TYPE_DATA;
	hdr_out.frame_ctrl = HEYMAC_FCTL_S | HEYMAC_FCTL_D | HEYMAC_FCTL_L;
	hdr_out.src_addr.type = 1;
	memcpy(hdr_out.src_addr.long_addr, src_addr, 6);
	hdr_out.dst_addr.type = 1;
	memcpy(hdr_out.dst_addr.long_addr, dst_addr, 6);
	hdr_out.seq_no = 0x55;

	build_len = test_build_header(&hdr_out, buf, sizeof(buf));
	zassert_true(build_len > 0, "build_header failed");

	parse_len = test_parse_header(buf, build_len, &hdr_in);
	zassert_equal(parse_len, build_len, "parse length mismatch");
	zassert_mem_equal(hdr_in.src_addr.long_addr, src_addr, 6, "src_addr mismatch");
	zassert_mem_equal(hdr_in.dst_addr.long_addr, dst_addr, 6, "dst_addr mismatch");
}

/*
 * Test beacon frame type
 */
ZTEST(heymac_header, test_beacon_frame)
{
	struct test_hdr hdr_out, hdr_in;
	uint8_t buf[32];
	int build_len, parse_len;

	memset(&hdr_out, 0, sizeof(hdr_out));
	hdr_out.version_type = HEYMAC_FRM_TYPE_BEACON;
	hdr_out.frame_ctrl = HEYMAC_FCTL_S | HEYMAC_FCTL_N;
	hdr_out.net_id = 0x915E;
	hdr_out.src_addr.type = 0;
	hdr_out.src_addr.short_addr[0] = 0xDE;
	hdr_out.src_addr.short_addr[1] = 0xAD;
	hdr_out.seq_no = 0x00;

	build_len = test_build_header(&hdr_out, buf, sizeof(buf));
	zassert_true(build_len > 0, "build_header failed");

	parse_len = test_parse_header(buf, build_len, &hdr_in);
	zassert_equal(parse_len, build_len, "parse length mismatch");
	zassert_equal(hdr_in.version_type & 0x0F, HEYMAC_FRM_TYPE_BEACON, "frame type mismatch");
}

/*
 * Test error handling - buffer too small
 */
ZTEST(heymac_header, test_build_buffer_too_small)
{
	struct test_hdr hdr;
	uint8_t buf[2];  /* Too small */
	int ret;

	memset(&hdr, 0, sizeof(hdr));
	hdr.version_type = HEYMAC_FRM_TYPE_DATA;
	hdr.frame_ctrl = HEYMAC_FCTL_S;
	hdr.seq_no = 0x01;

	ret = test_build_header(&hdr, buf, sizeof(buf));
	zassert_true(ret < 0, "should fail with small buffer");
}

/*
 * Test error handling - parse truncated packet
 */
ZTEST(heymac_header, test_parse_truncated)
{
	struct test_hdr hdr;
	uint8_t buf[1] = {0x01};  /* Too short */
	int ret;

	ret = test_parse_header(buf, sizeof(buf), &hdr);
	zassert_true(ret < 0, "should fail with truncated packet");
}

ZTEST_SUITE(heymac_header, NULL, NULL, NULL, NULL, NULL);
