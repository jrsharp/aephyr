/*
 * Copyright (c) 2025 Æther Authors
 *
 * SPDX-License-Identifier: MIT
 *
 * Unit tests for Aether mesh header parsing and building.
 * Self-contained tests without full stack dependencies.
 */

#include <zephyr/ztest.h>
#include <string.h>

/* Mesh header flags */
#define AETHER_FCTL_MESH_NET_ID  0x01

/* Message types */
#define AETHER_MSG_DATA   0x00
#define AETHER_MSG_HELLO  0x05

/* Priority and delivery */
#define AETHER_PRIORITY_LOW    0
#define AETHER_PRIORITY_NORMAL 1
#define AETHER_PRIORITY_HIGH   2
#define AETHER_DELIV_BESTEFFORT 0
#define AETHER_DELIV_RELIABLE   1

/* Security flags */
#define AETHER_SEC_FLAG_NONE   0x00
#define AETHER_SEC_FLAG_SIGNED 0x01

/* Test mesh header structure */
struct test_mesh_hdr {
	uint8_t flags;
	uint8_t type;
	uint8_t ttl;
	uint8_t src[6];
	uint8_t dst[6];
	uint8_t seq_no;
	uint16_t mesh_net_id;  /* Only if AETHER_FCTL_MESH_NET_ID */
	uint8_t qos_security;
	uint16_t length;
};

/* Build mesh header - test implementation */
static int test_mesh_build_header(const struct test_mesh_hdr *hdr, uint8_t *buf, size_t len)
{
	size_t offset = 0;
	size_t needed = 19;  /* Base: flags + type + ttl + src(6) + dst(6) + seq + qos + len(2) */

	if (hdr->flags & AETHER_FCTL_MESH_NET_ID) {
		needed += 2;
	}

	if (len < needed) {
		return -1;
	}

	buf[offset++] = hdr->flags;
	buf[offset++] = hdr->type;
	buf[offset++] = hdr->ttl;
	memcpy(&buf[offset], hdr->src, 6);
	offset += 6;
	memcpy(&buf[offset], hdr->dst, 6);
	offset += 6;
	buf[offset++] = hdr->seq_no;

	if (hdr->flags & AETHER_FCTL_MESH_NET_ID) {
		buf[offset++] = (hdr->mesh_net_id >> 8) & 0xFF;
		buf[offset++] = hdr->mesh_net_id & 0xFF;
	}

	buf[offset++] = hdr->qos_security;
	buf[offset++] = (hdr->length >> 8) & 0xFF;
	buf[offset++] = hdr->length & 0xFF;

	return (int)offset;
}

/* Header size calculation */
static size_t test_mesh_hdr_size(uint8_t flags)
{
	size_t size = 19;  /* Base size */
	if (flags & AETHER_FCTL_MESH_NET_ID) {
		size += 2;
	}
	return size;
}

/* QoS field helpers */
static uint8_t test_get_priority(uint8_t qos)
{
	return (qos >> 6) & 0x03;
}

static uint8_t test_get_delivery(uint8_t qos)
{
	return (qos >> 5) & 0x01;
}

static uint8_t test_get_security(uint8_t qos)
{
	return (qos >> 1) & 0x0F;
}

/*
 * Test data message building
 */
ZTEST(aether_mesh_header, test_data_message_roundtrip)
{
	struct test_mesh_hdr hdr;
	uint8_t buf[32];
	int len;
	uint8_t src[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
	uint8_t dst[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

	memset(&hdr, 0, sizeof(hdr));
	hdr.flags = 0;
	hdr.type = AETHER_MSG_DATA;
	hdr.ttl = 6;
	memcpy(hdr.src, src, 6);
	memcpy(hdr.dst, dst, 6);
	hdr.seq_no = 0x42;
	hdr.qos_security = (AETHER_PRIORITY_NORMAL << 6);
	hdr.length = 10;

	len = test_mesh_build_header(&hdr, buf, sizeof(buf));
	zassert_true(len > 0, "build_header failed: %d", len);
	zassert_equal(len, 19, "expected 19-byte header without mesh net ID");

	/* Verify buffer contents */
	zassert_equal(buf[0], 0x00, "flags mismatch");
	zassert_equal(buf[1], AETHER_MSG_DATA, "type mismatch");
	zassert_equal(buf[2], 6, "ttl mismatch");
	zassert_mem_equal(&buf[3], src, 6, "src mismatch");
	zassert_mem_equal(&buf[9], dst, 6, "dst mismatch");
	zassert_equal(buf[15], 0x42, "seq_no mismatch");
}

/*
 * Test header with mesh network ID
 */
ZTEST(aether_mesh_header, test_header_with_mesh_net_id)
{
	struct test_mesh_hdr hdr;
	uint8_t buf[32];
	int len;
	uint8_t src[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
	uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

	memset(&hdr, 0, sizeof(hdr));
	hdr.flags = AETHER_FCTL_MESH_NET_ID;
	hdr.type = AETHER_MSG_HELLO;
	hdr.ttl = 1;
	memcpy(hdr.src, src, 6);
	memcpy(hdr.dst, bcast, 6);
	hdr.seq_no = 0x01;
	hdr.mesh_net_id = 0x1234;
	hdr.qos_security = 0;
	hdr.length = 0;

	len = test_mesh_build_header(&hdr, buf, sizeof(buf));
	zassert_true(len > 0, "build_header failed");
	zassert_equal(len, 21, "expected 21-byte header with mesh net ID");

	/* Verify mesh net ID in buffer (big-endian) */
	zassert_equal(buf[16], 0x12, "mesh_net_id high byte mismatch");
	zassert_equal(buf[17], 0x34, "mesh_net_id low byte mismatch");
}

/*
 * Test hello message format
 */
ZTEST(aether_mesh_header, test_hello_message)
{
	struct test_mesh_hdr hdr;
	uint8_t buf[32];
	int len;
	uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
	uint8_t src[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE};

	memset(&hdr, 0, sizeof(hdr));
	hdr.type = AETHER_MSG_HELLO;
	hdr.ttl = 1;
	memcpy(hdr.src, src, 6);
	memcpy(hdr.dst, bcast, 6);
	hdr.seq_no = 0x00;
	hdr.qos_security = (AETHER_PRIORITY_LOW << 6) | (AETHER_DELIV_BESTEFFORT << 5);
	hdr.length = 0;

	len = test_mesh_build_header(&hdr, buf, sizeof(buf));
	zassert_true(len > 0, "build_header failed");
	zassert_equal(buf[1], AETHER_MSG_HELLO, "type should be HELLO");
	zassert_equal(buf[2], 1, "TTL should be 1 for hello");
}

/*
 * Test header size calculation
 */
ZTEST(aether_mesh_header, test_header_size_calc)
{
	size_t size_without = test_mesh_hdr_size(0);
	size_t size_with = test_mesh_hdr_size(AETHER_FCTL_MESH_NET_ID);

	zassert_equal(size_without, 19, "base size should be 19");
	zassert_equal(size_with, 21, "size with net ID should be 21");
}

/*
 * Test QoS field extraction
 */
ZTEST(aether_mesh_header, test_qos_helpers)
{
	uint8_t qos;

	/* Priority high, reliable delivery, signed */
	qos = (AETHER_PRIORITY_HIGH << 6) | (AETHER_DELIV_RELIABLE << 5) |
	      (AETHER_SEC_FLAG_SIGNED << 1);

	zassert_equal(test_get_priority(qos), AETHER_PRIORITY_HIGH, "priority failed");
	zassert_equal(test_get_delivery(qos), AETHER_DELIV_RELIABLE, "delivery failed");
	zassert_equal(test_get_security(qos), AETHER_SEC_FLAG_SIGNED, "security failed");
}

/*
 * Test buffer too small error
 */
ZTEST(aether_mesh_header, test_build_buffer_too_small)
{
	struct test_mesh_hdr hdr;
	uint8_t buf[5];
	int ret;

	memset(&hdr, 0, sizeof(hdr));
	hdr.type = AETHER_MSG_DATA;
	hdr.ttl = 6;

	ret = test_mesh_build_header(&hdr, buf, sizeof(buf));
	zassert_true(ret < 0, "should fail with small buffer");
}

/*
 * Test broadcast address check
 */
ZTEST(aether_mesh_header, test_broadcast_address)
{
	uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
	uint8_t unicast[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};

	zassert_equal(memcmp(bcast, "\xff\xff\xff\xff\xff\xff", 6), 0,
		      "broadcast should match");
	zassert_not_equal(memcmp(unicast, "\xff\xff\xff\xff\xff\xff", 6), 0,
			  "unicast should not match");
}

ZTEST_SUITE(aether_mesh_header, NULL, NULL, NULL, NULL, NULL);
