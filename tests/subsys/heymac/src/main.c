/*
 * Copyright (c) 2025 Æther Authors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include <zephyr/net/heymac.h>
#include <string.h>

/*
 * Test heymac_build_header and heymac_parse_header round-trip
 * for minimal header (no network ID, short address, source only)
 */
ZTEST(heymac_header, test_minimal_header_roundtrip)
{
	struct heymac_hdr hdr_out, hdr_in;
	uint8_t buf[32];
	int build_len, parse_len;

	memset(&hdr_out, 0, sizeof(hdr_out));
	hdr_out.version_type = HEYMAC_FRM_TYPE_DATA;
	hdr_out.frame_ctrl = HEYMAC_FCTL_S;  /* Source present, short addr */
	hdr_out.src_addr.type = 0;  /* Short address */
	hdr_out.src_addr.short_addr[0] = 0xAB;
	hdr_out.src_addr.short_addr[1] = 0xCD;
	hdr_out.seq_no = 0x42;

	build_len = heymac_build_header(&hdr_out, buf, sizeof(buf));
	zassert_true(build_len > 0, "build_header failed: %d", build_len);

	parse_len = heymac_parse_header(buf, build_len, &hdr_in);
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
	struct heymac_hdr hdr_out, hdr_in;
	uint8_t buf[32];
	int build_len, parse_len;

	memset(&hdr_out, 0, sizeof(hdr_out));
	hdr_out.version_type = HEYMAC_FRM_TYPE_DATA;
	hdr_out.frame_ctrl = HEYMAC_FCTL_S | HEYMAC_FCTL_N;  /* Source + NetID */
	hdr_out.net_id = 0x915E;
	hdr_out.src_addr.type = 0;
	hdr_out.src_addr.short_addr[0] = 0x12;
	hdr_out.src_addr.short_addr[1] = 0x34;
	hdr_out.seq_no = 0x99;

	build_len = heymac_build_header(&hdr_out, buf, sizeof(buf));
	zassert_true(build_len > 0, "build_header failed");

	parse_len = heymac_parse_header(buf, build_len, &hdr_in);
	zassert_equal(parse_len, build_len, "parse length mismatch");
	zassert_equal(hdr_in.net_id, 0x915E, "net_id mismatch");
}

/*
 * Test header with long addresses (6 bytes)
 */
ZTEST(heymac_header, test_header_long_addr)
{
	struct heymac_hdr hdr_out, hdr_in;
	uint8_t buf[32];
	int build_len, parse_len;
	uint8_t src_addr[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
	uint8_t dst_addr[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

	memset(&hdr_out, 0, sizeof(hdr_out));
	hdr_out.version_type = HEYMAC_FRM_TYPE_DATA;
	hdr_out.frame_ctrl = HEYMAC_FCTL_S | HEYMAC_FCTL_D | HEYMAC_FCTL_L;  /* Src + Dst + Long */
	hdr_out.src_addr.type = 1;
	memcpy(hdr_out.src_addr.long_addr, src_addr, 6);
	hdr_out.dst_addr.type = 1;
	memcpy(hdr_out.dst_addr.long_addr, dst_addr, 6);
	hdr_out.seq_no = 0x55;

	build_len = heymac_build_header(&hdr_out, buf, sizeof(buf));
	zassert_true(build_len > 0, "build_header failed");

	parse_len = heymac_parse_header(buf, build_len, &hdr_in);
	zassert_equal(parse_len, build_len, "parse length mismatch");
	zassert_mem_equal(hdr_in.src_addr.long_addr, src_addr, 6, "src_addr mismatch");
	zassert_mem_equal(hdr_in.dst_addr.long_addr, dst_addr, 6, "dst_addr mismatch");
}

/*
 * Test beacon frame type
 */
ZTEST(heymac_header, test_beacon_frame)
{
	struct heymac_hdr hdr_out, hdr_in;
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

	build_len = heymac_build_header(&hdr_out, buf, sizeof(buf));
	zassert_true(build_len > 0, "build_header failed");

	parse_len = heymac_parse_header(buf, build_len, &hdr_in);
	zassert_equal(parse_len, build_len, "parse length mismatch");
	zassert_equal(hdr_in.version_type & 0x0F, HEYMAC_FRM_TYPE_BEACON, "frame type mismatch");
}

/*
 * Test error handling - buffer too small
 */
ZTEST(heymac_header, test_build_buffer_too_small)
{
	struct heymac_hdr hdr;
	uint8_t buf[2];  /* Too small */
	int ret;

	memset(&hdr, 0, sizeof(hdr));
	hdr.version_type = HEYMAC_FRM_TYPE_DATA;
	hdr.frame_ctrl = HEYMAC_FCTL_S;
	hdr.seq_no = 0x01;

	ret = heymac_build_header(&hdr, buf, sizeof(buf));
	zassert_true(ret < 0, "should fail with small buffer");
}

/*
 * Test error handling - parse truncated packet
 */
ZTEST(heymac_header, test_parse_truncated)
{
	struct heymac_hdr hdr;
	uint8_t buf[1] = {0x01};  /* Too short */
	int ret;

	ret = heymac_parse_header(buf, sizeof(buf), &hdr);
	zassert_true(ret < 0, "should fail with truncated packet");
}

/*
 * Test heymac_addr_len helper
 */
ZTEST(heymac_header, test_addr_len)
{
	struct heymac_addr short_addr = { .type = 0 };
	struct heymac_addr long_addr = { .type = 1 };

	zassert_equal(heymac_addr_len(&short_addr), 2, "short addr should be 2 bytes");
	zassert_equal(heymac_addr_len(&long_addr), 6, "long addr should be 6 bytes");
}

ZTEST_SUITE(heymac_header, NULL, NULL, NULL, NULL, NULL);
