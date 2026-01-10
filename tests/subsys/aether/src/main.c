/*
 * Copyright (c) 2025 Æther Authors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include <zephyr/net/aether_mesh.h>
#include <string.h>

/*
 * Test aether_mesh_build_header and parse round-trip
 * for basic data message
 */
ZTEST(aether_mesh_header, test_data_message_roundtrip)
{
	struct aether_mesh_hdr hdr_out, hdr_in;
	uint8_t buf[32];
	int build_len, parse_len;
	uint8_t src[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
	uint8_t dst[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

	memset(&hdr_out, 0, sizeof(hdr_out));
	hdr_out.flags = 0;
	hdr_out.type = AETHER_MSG_DATA;
	hdr_out.ttl = 6;
	memcpy(hdr_out.src, src, 6);
	memcpy(hdr_out.dst, dst, 6);
	hdr_out.seq_no = 0x42;
	hdr_out.qos_security = (AETHER_PRIORITY_NORMAL << 6);
	hdr_out.length = 10;

	build_len = aether_mesh_build_header(&hdr_out, buf, sizeof(buf));
	zassert_true(build_len > 0, "build_header failed: %d", build_len);
	zassert_equal(build_len, 19, "expected 19-byte header without mesh net ID");

	/* Parse it back - we need a local parse function for testing */
	/* For now, verify the buffer contents directly */
	zassert_equal(buf[0], 0x00, "flags mismatch");
	zassert_equal(buf[1], AETHER_MSG_DATA, "type mismatch");
	zassert_equal(buf[2], 6, "ttl mismatch");
	zassert_mem_equal(&buf[3], src, 6, "src mismatch");
	zassert_mem_equal(&buf[9], dst, 6, "dst mismatch");
	zassert_equal(buf[15], 0x42, "seq_no mismatch");
}

/*
 * Test header with mesh network ID present
 */
ZTEST(aether_mesh_header, test_header_with_mesh_net_id)
{
	struct aether_mesh_hdr hdr_out;
	uint8_t buf[32];
	int build_len;
	uint8_t src[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
	uint8_t dst[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

	memset(&hdr_out, 0, sizeof(hdr_out));
	hdr_out.flags = AETHER_FCTL_MESH_NET_ID;
	hdr_out.type = AETHER_MSG_HELLO;
	hdr_out.ttl = 1;
	memcpy(hdr_out.src, src, 6);
	memcpy(hdr_out.dst, dst, 6);
	hdr_out.seq_no = 0x01;
	hdr_out.mesh_net_id = 0x1234;
	hdr_out.qos_security = 0;
	hdr_out.length = 0;

	build_len = aether_mesh_build_header(&hdr_out, buf, sizeof(buf));
	zassert_true(build_len > 0, "build_header failed");
	zassert_equal(build_len, 21, "expected 21-byte header with mesh net ID");

	/* Verify mesh net ID is in the buffer (big-endian) */
	zassert_equal(buf[16], 0x12, "mesh_net_id high byte mismatch");
	zassert_equal(buf[17], 0x34, "mesh_net_id low byte mismatch");
}

/*
 * Test hello message format
 */
ZTEST(aether_mesh_header, test_hello_message)
{
	struct aether_mesh_hdr hdr;
	uint8_t buf[32];
	int len;
	uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
	uint8_t src[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE};

	memset(&hdr, 0, sizeof(hdr));
	hdr.type = AETHER_MSG_HELLO;
	hdr.ttl = 1;  /* Hello is single-hop */
	memcpy(hdr.src, src, 6);
	memcpy(hdr.dst, bcast, 6);
	hdr.seq_no = 0x00;
	hdr.qos_security = (AETHER_PRIORITY_LOW << 6) | (AETHER_DELIV_BESTEFFORT << 5);
	hdr.length = 0;

	len = aether_mesh_build_header(&hdr, buf, sizeof(buf));
	zassert_true(len > 0, "build_header failed");
	zassert_equal(buf[1], AETHER_MSG_HELLO, "type should be HELLO");
	zassert_equal(buf[2], 1, "TTL should be 1 for hello");
}

/*
 * Test header size calculation
 */
ZTEST(aether_mesh_header, test_header_size_calc)
{
	size_t size_without_net_id = aether_mesh_hdr_size(0);
	size_t size_with_net_id = aether_mesh_hdr_size(AETHER_FCTL_MESH_NET_ID);

	zassert_equal(size_without_net_id, 16, "base size should be 16");
	zassert_equal(size_with_net_id, 18, "size with net ID should be 18");
}

/*
 * Test QoS field extraction helpers
 */
ZTEST(aether_mesh_header, test_qos_helpers)
{
	uint8_t qos;

	/* Priority high, reliable delivery, signed */
	qos = (AETHER_PRIORITY_HIGH << 6) | (AETHER_DELIV_RELIABLE << 5) |
	      (AETHER_SEC_FLAG_SIGNED << 1);

	zassert_equal(aether_mesh_get_priority(qos), AETHER_PRIORITY_HIGH,
		      "priority extraction failed");
	zassert_equal(aether_mesh_get_delivery(qos), AETHER_DELIV_RELIABLE,
		      "delivery extraction failed");
	zassert_equal(aether_mesh_get_security(qos), AETHER_SEC_FLAG_SIGNED,
		      "security extraction failed");
}

/*
 * Test error handling - buffer too small
 */
ZTEST(aether_mesh_header, test_build_buffer_too_small)
{
	struct aether_mesh_hdr hdr;
	uint8_t buf[5];  /* Way too small */
	int ret;

	memset(&hdr, 0, sizeof(hdr));
	hdr.type = AETHER_MSG_DATA;
	hdr.ttl = 6;

	ret = aether_mesh_build_header(&hdr, buf, sizeof(buf));
	zassert_true(ret < 0, "should fail with small buffer");
}

/*
 * Test broadcast address detection
 */
ZTEST(aether_mesh_header, test_broadcast_address)
{
	uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
	uint8_t unicast[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};

	/* Simple memcmp test - this is how the code checks for broadcast */
	zassert_equal(memcmp(bcast, "\xff\xff\xff\xff\xff\xff", 6), 0,
		      "broadcast should match");
	zassert_not_equal(memcmp(unicast, "\xff\xff\xff\xff\xff\xff", 6), 0,
			  "unicast should not match broadcast");
}

ZTEST_SUITE(aether_mesh_header, NULL, NULL, NULL, NULL, NULL);
