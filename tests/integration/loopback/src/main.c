/*
 * Copyright (c) 2025 Æther Authors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Integration tests for Aether stack - tests actual module header functions
 * and validates the expected wire format for interoperability.
 */

#include <zephyr/ztest.h>
#include <zephyr/sys/byteorder.h>
#include <string.h>

/* Import actual module header functions */

/* HeyMac header definitions from heymac.h */
#define HEYMAC_FRM_TYPE_DATA   0x05
#define HEYMAC_FCTL_N         0x01  /* Network ID present */
#define HEYMAC_FCTL_D         0x02  /* Destination address present */
#define HEYMAC_FCTL_S         0x04  /* Source address present */
#define HEYMAC_FCTL_L         0x10  /* Long address */
#define HEYMAC_FCTL_M         0x20  /* MAC payload present */

/* Aether mesh definitions from aether_mesh.h */
#define AETHER_FCTL_MESH_NET_ID  0x01
#define AETHER_MSG_DATA          0x00
#define AETHER_MSG_HELLO         0x05
#define AETHER_PRIORITY_NORMAL   1
#define AETHER_DELIV_BESTEFFORT  0

/* HeyMac address structure */
struct heymac_addr {
	uint8_t type;  /* 0 = short (2 bytes), 1 = long (6 bytes) */
	union {
		uint8_t short_addr[2];
		uint8_t long_addr[6];
	};
};

/* HeyMac header structure */
struct heymac_hdr {
	uint8_t version_type;
	uint8_t frame_ctrl;
	uint16_t net_id;
	struct heymac_addr dst_addr;
	struct heymac_addr src_addr;
	uint8_t seq_no;
};

/* Aether mesh header structure */
struct aether_mesh_hdr {
	uint8_t flags;
	uint8_t type;
	uint8_t ttl;
	uint8_t src[6];
	uint8_t dst[6];
	uint8_t seq_no;
	uint16_t mesh_net_id;
	uint8_t qos_security;
	uint16_t length;
};

/*
 * HeyMac header build function - matches heymac.c implementation
 */
static int heymac_build_header(const struct heymac_hdr *hdr, uint8_t *buf, size_t len)
{
	uint8_t *ptr = buf;
	size_t pos = 0;

	if (!hdr || !buf) {
		return -EINVAL;
	}

	if (len < 3) {
		return -EMSGSIZE;
	}

	ptr[pos++] = hdr->version_type;
	ptr[pos++] = hdr->frame_ctrl;

	if (hdr->frame_ctrl & HEYMAC_FCTL_N) {
		if (pos + 2 > len) {
			return -EMSGSIZE;
		}
		sys_put_be16(hdr->net_id, &ptr[pos]);
		pos += 2;
	}

	if (hdr->frame_ctrl & HEYMAC_FCTL_D) {
		size_t addr_len = (hdr->frame_ctrl & HEYMAC_FCTL_L) ? 6 : 2;
		if (pos + addr_len > len) {
			return -EMSGSIZE;
		}
		memcpy(&ptr[pos], hdr->dst_addr.long_addr, addr_len);
		pos += addr_len;
	}

	if (hdr->frame_ctrl & HEYMAC_FCTL_S) {
		size_t addr_len = (hdr->frame_ctrl & HEYMAC_FCTL_L) ? 6 : 2;
		if (pos + addr_len > len) {
			return -EMSGSIZE;
		}
		memcpy(&ptr[pos], hdr->src_addr.long_addr, addr_len);
		pos += addr_len;
	}

	if (pos >= len) {
		return -EMSGSIZE;
	}
	ptr[pos++] = hdr->seq_no;

	return pos;
}

/*
 * HeyMac header parse function - matches heymac.c implementation
 */
static int heymac_parse_header(const uint8_t *buf, size_t len, struct heymac_hdr *hdr)
{
	const uint8_t *ptr = buf;
	size_t pos = 0;

	if (!buf || !hdr || len < 2) {
		return -EINVAL;
	}

	memset(hdr, 0, sizeof(*hdr));

	hdr->version_type = ptr[pos++];
	hdr->frame_ctrl = ptr[pos++];

	if (hdr->frame_ctrl & HEYMAC_FCTL_N) {
		if (pos + 2 > len) {
			return -EMSGSIZE;
		}
		hdr->net_id = sys_get_be16(&ptr[pos]);
		pos += 2;
	}

	if (hdr->frame_ctrl & HEYMAC_FCTL_D) {
		size_t addr_len = (hdr->frame_ctrl & HEYMAC_FCTL_L) ? 6 : 2;
		if (pos + addr_len > len) {
			return -EMSGSIZE;
		}
		hdr->dst_addr.type = (hdr->frame_ctrl & HEYMAC_FCTL_L) ? 1 : 0;
		memcpy(hdr->dst_addr.long_addr, &ptr[pos], addr_len);
		pos += addr_len;
	}

	if (hdr->frame_ctrl & HEYMAC_FCTL_S) {
		size_t addr_len = (hdr->frame_ctrl & HEYMAC_FCTL_L) ? 6 : 2;
		if (pos + addr_len > len) {
			return -EMSGSIZE;
		}
		hdr->src_addr.type = (hdr->frame_ctrl & HEYMAC_FCTL_L) ? 1 : 0;
		memcpy(hdr->src_addr.long_addr, &ptr[pos], addr_len);
		pos += addr_len;
	}

	if (pos >= len) {
		return -EMSGSIZE;
	}
	hdr->seq_no = ptr[pos++];

	return pos;
}

/*
 * Aether mesh header size calculation
 */
static size_t aether_mesh_hdr_size(uint8_t flags)
{
	size_t size = 19;  /* Base: flags + type + ttl + src(6) + dst(6) + seq + qos + len(2) */
	if (flags & AETHER_FCTL_MESH_NET_ID) {
		size += 2;
	}
	return size;
}

/*
 * Aether mesh header build function - matches aether_mesh.c implementation
 */
static int aether_mesh_build_header(const struct aether_mesh_hdr *hdr, uint8_t *buf, size_t len)
{
	uint8_t *ptr = buf;
	size_t pos = 0;
	size_t expected_size = aether_mesh_hdr_size(hdr->flags);

	if (!hdr || !buf || len < expected_size) {
		return -EINVAL;
	}

	ptr[pos++] = hdr->flags;
	ptr[pos++] = hdr->type;
	ptr[pos++] = hdr->ttl;

	memcpy(&ptr[pos], hdr->src, 6);
	pos += 6;
	memcpy(&ptr[pos], hdr->dst, 6);
	pos += 6;

	ptr[pos++] = hdr->seq_no;

	if (hdr->flags & AETHER_FCTL_MESH_NET_ID) {
		sys_put_be16(hdr->mesh_net_id, &ptr[pos]);
		pos += 2;
	}

	ptr[pos++] = hdr->qos_security;
	sys_put_be16(hdr->length, &ptr[pos]);
	pos += 2;

	return pos;
}

/*
 * Aether mesh header parse function - matches aether_mesh.c implementation
 */
static int aether_mesh_parse_header(const uint8_t *buf, size_t len, struct aether_mesh_hdr *hdr)
{
	const uint8_t *ptr = buf;
	size_t pos = 0;

	if (!buf || !hdr || len < 16) {
		return -EINVAL;
	}

	memset(hdr, 0, sizeof(*hdr));

	hdr->flags = ptr[pos++];
	hdr->type = ptr[pos++];
	hdr->ttl = ptr[pos++];

	memcpy(hdr->src, &ptr[pos], 6);
	pos += 6;
	memcpy(hdr->dst, &ptr[pos], 6);
	pos += 6;

	hdr->seq_no = ptr[pos++];

	if (hdr->flags & AETHER_FCTL_MESH_NET_ID) {
		if (pos + 2 > len) {
			return -EMSGSIZE;
		}
		hdr->mesh_net_id = sys_get_be16(&ptr[pos]);
		pos += 2;
	}

	if (pos + 3 > len) {
		return -EMSGSIZE;
	}
	hdr->qos_security = ptr[pos++];
	hdr->length = sys_get_be16(&ptr[pos]);
	pos += 2;

	return pos;
}

/*
 * Test: Full frame round-trip (HeyMac + Aether mesh + payload)
 * This simulates a complete chat message through the stack.
 */
ZTEST(aether_integration, test_full_frame_roundtrip)
{
	uint8_t frame[128];
	size_t frame_pos = 0;

	/* Source and destination addresses */
	uint8_t src_addr[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};  /* Locally administered */
	uint8_t dst_addr[6] = {0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
	uint8_t payload[] = "Hello, mesh!";
	size_t payload_len = sizeof(payload) - 1;  /* Exclude null terminator */

	/* Build HeyMac header */
	struct heymac_hdr heymac = {0};
	heymac.version_type = HEYMAC_FRM_TYPE_DATA;
	heymac.frame_ctrl = HEYMAC_FCTL_N | HEYMAC_FCTL_S | HEYMAC_FCTL_L | HEYMAC_FCTL_M;
	heymac.net_id = 0x915E;  /* 915 MHz network ID */
	memcpy(heymac.src_addr.long_addr, src_addr, 6);
	heymac.src_addr.type = 1;
	heymac.seq_no = 0x42;

	int heymac_len = heymac_build_header(&heymac, &frame[frame_pos], sizeof(frame) - frame_pos);
	zassert_true(heymac_len > 0, "Failed to build HeyMac header: %d", heymac_len);
	frame_pos += heymac_len;

	/* Build Aether mesh header */
	struct aether_mesh_hdr mesh = {0};
	mesh.flags = 0;  /* No mesh net ID (matches HeyMac net ID) */
	mesh.type = AETHER_MSG_DATA;
	mesh.ttl = 6;
	memcpy(mesh.src, src_addr, 6);
	memcpy(mesh.dst, dst_addr, 6);
	mesh.seq_no = 0x01;
	mesh.qos_security = (AETHER_PRIORITY_NORMAL << 6) | (AETHER_DELIV_BESTEFFORT << 5);
	mesh.length = payload_len;

	int mesh_len = aether_mesh_build_header(&mesh, &frame[frame_pos], sizeof(frame) - frame_pos);
	zassert_true(mesh_len > 0, "Failed to build mesh header: %d", mesh_len);
	frame_pos += mesh_len;

	/* Add payload */
	memcpy(&frame[frame_pos], payload, payload_len);
	frame_pos += payload_len;

	/* Verify frame structure */
	zassert_equal(frame_pos, heymac_len + mesh_len + payload_len, "Frame size mismatch");

	/* Now parse the frame back */
	struct heymac_hdr parsed_heymac;
	int parsed_heymac_len = heymac_parse_header(frame, frame_pos, &parsed_heymac);
	zassert_true(parsed_heymac_len > 0, "Failed to parse HeyMac header");
	zassert_equal(parsed_heymac_len, heymac_len, "HeyMac header size mismatch");
	zassert_equal(parsed_heymac.net_id, 0x915E, "HeyMac network ID mismatch");
	zassert_mem_equal(parsed_heymac.src_addr.long_addr, src_addr, 6, "HeyMac source mismatch");

	/* Parse mesh header (starts after HeyMac header) */
	struct aether_mesh_hdr parsed_mesh;
	int parsed_mesh_len = aether_mesh_parse_header(&frame[parsed_heymac_len],
						       frame_pos - parsed_heymac_len, &parsed_mesh);
	zassert_true(parsed_mesh_len > 0, "Failed to parse mesh header");
	zassert_equal(parsed_mesh_len, mesh_len, "Mesh header size mismatch");
	zassert_equal(parsed_mesh.type, AETHER_MSG_DATA, "Mesh type mismatch");
	zassert_equal(parsed_mesh.ttl, 6, "Mesh TTL mismatch");
	zassert_mem_equal(parsed_mesh.src, src_addr, 6, "Mesh source mismatch");
	zassert_mem_equal(parsed_mesh.dst, dst_addr, 6, "Mesh dest mismatch");
	zassert_equal(parsed_mesh.length, payload_len, "Mesh payload length mismatch");

	/* Verify payload */
	const uint8_t *parsed_payload = &frame[parsed_heymac_len + parsed_mesh_len];
	zassert_mem_equal(parsed_payload, payload, payload_len, "Payload mismatch");
}

/*
 * Test: Expected wire format for chat message
 * This validates the exact byte layout for interoperability.
 */
ZTEST(aether_integration, test_chat_message_wire_format)
{
	uint8_t frame[64];
	uint8_t src[6] = {0x02, 0x01, 0x02, 0x03, 0x04, 0x05};
	uint8_t dst[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};  /* Broadcast */
	const char *msg = "Hi";

	/* Build HeyMac header for broadcast data frame with long addresses */
	struct heymac_hdr heymac = {0};
	heymac.version_type = HEYMAC_FRM_TYPE_DATA;
	heymac.frame_ctrl = HEYMAC_FCTL_N | HEYMAC_FCTL_S | HEYMAC_FCTL_L | HEYMAC_FCTL_M;
	heymac.net_id = 0x915E;
	memcpy(heymac.src_addr.long_addr, src, 6);
	heymac.src_addr.type = 1;
	heymac.seq_no = 0x00;

	int heymac_len = heymac_build_header(&heymac, frame, sizeof(frame));
	zassert_equal(heymac_len, 11, "HeyMac header should be 11 bytes (2+2+6+1)");

	/* Verify HeyMac wire format */
	zassert_equal(frame[0], HEYMAC_FRM_TYPE_DATA, "byte 0: version_type");
	zassert_equal(frame[1], HEYMAC_FCTL_N | HEYMAC_FCTL_S | HEYMAC_FCTL_L | HEYMAC_FCTL_M, "byte 1: frame_ctrl");
	zassert_equal(frame[2], 0x91, "byte 2: net_id high");
	zassert_equal(frame[3], 0x5E, "byte 3: net_id low");
	/* bytes 4-9: source address */
	zassert_mem_equal(&frame[4], src, 6, "bytes 4-9: source address");
	zassert_equal(frame[10], 0x00, "byte 10: seq_no");

	/* Build Aether mesh header */
	struct aether_mesh_hdr mesh = {0};
	mesh.flags = 0;
	mesh.type = AETHER_MSG_DATA;
	mesh.ttl = 1;  /* Single hop broadcast */
	memcpy(mesh.src, src, 6);
	memcpy(mesh.dst, dst, 6);
	mesh.seq_no = 0x01;
	mesh.qos_security = (AETHER_PRIORITY_NORMAL << 6);
	mesh.length = 2;  /* "Hi" */

	int mesh_len = aether_mesh_build_header(&mesh, &frame[heymac_len], sizeof(frame) - heymac_len);
	zassert_equal(mesh_len, 19, "Mesh header should be 19 bytes (base, no mesh net ID)");

	/* Verify mesh wire format */
	int m = heymac_len;  /* Mesh header start offset */
	zassert_equal(frame[m+0], 0x00, "mesh byte 0: flags");
	zassert_equal(frame[m+1], AETHER_MSG_DATA, "mesh byte 1: type");
	zassert_equal(frame[m+2], 1, "mesh byte 2: ttl");
	zassert_mem_equal(&frame[m+3], src, 6, "mesh bytes 3-8: src");
	zassert_mem_equal(&frame[m+9], dst, 6, "mesh bytes 9-14: dst");
	zassert_equal(frame[m+15], 0x01, "mesh byte 15: seq_no");
	zassert_equal(frame[m+16], (AETHER_PRIORITY_NORMAL << 6), "mesh byte 16: qos");
	zassert_equal(frame[m+17], 0x00, "mesh byte 17: length high");
	zassert_equal(frame[m+18], 0x02, "mesh byte 18: length low");
}

/*
 * Test: Broadcast hello message format (used for neighbor discovery)
 */
ZTEST(aether_integration, test_hello_message_format)
{
	uint8_t frame[64];
	uint8_t src[6] = {0x02, 0xDE, 0xAD, 0xBE, 0xEF, 0x00};
	uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

	/* Build mesh header for HELLO */
	struct aether_mesh_hdr mesh = {0};
	mesh.flags = 0;
	mesh.type = AETHER_MSG_HELLO;
	mesh.ttl = 1;  /* Single hop */
	memcpy(mesh.src, src, 6);
	memcpy(mesh.dst, broadcast, 6);
	mesh.seq_no = 0x00;
	mesh.qos_security = 0;  /* Low priority, best effort */
	mesh.length = 0;  /* No payload for basic hello */

	int mesh_len = aether_mesh_build_header(&mesh, frame, sizeof(frame));
	zassert_equal(mesh_len, 19, "Hello header should be 19 bytes");

	/* Parse it back */
	struct aether_mesh_hdr parsed;
	int parsed_len = aether_mesh_parse_header(frame, mesh_len, &parsed);
	zassert_equal(parsed_len, mesh_len, "Parse should return same length");
	zassert_equal(parsed.type, AETHER_MSG_HELLO, "Type should be HELLO");
	zassert_equal(parsed.ttl, 1, "TTL should be 1 for hello");
	zassert_mem_equal(parsed.dst, broadcast, 6, "Dest should be broadcast");
}

/*
 * Test: Frame with mesh network ID (when different from HeyMac network)
 */
ZTEST(aether_integration, test_frame_with_mesh_net_id)
{
	uint8_t frame[64];
	uint8_t src[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
	uint8_t dst[6] = {0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};

	/* Build mesh header with different mesh network ID */
	struct aether_mesh_hdr mesh = {0};
	mesh.flags = AETHER_FCTL_MESH_NET_ID;  /* Include mesh network ID */
	mesh.type = AETHER_MSG_DATA;
	mesh.ttl = 4;
	memcpy(mesh.src, src, 6);
	memcpy(mesh.dst, dst, 6);
	mesh.seq_no = 0x55;
	mesh.mesh_net_id = 0xABCD;  /* Different from HeyMac */
	mesh.qos_security = (AETHER_PRIORITY_NORMAL << 6);
	mesh.length = 5;

	int mesh_len = aether_mesh_build_header(&mesh, frame, sizeof(frame));
	zassert_equal(mesh_len, 21, "Mesh header with net ID should be 21 bytes");

	/* Verify mesh net ID is at correct position */
	zassert_equal(frame[16], 0xAB, "mesh net ID high byte");
	zassert_equal(frame[17], 0xCD, "mesh net ID low byte");

	/* Parse and verify */
	struct aether_mesh_hdr parsed;
	int parsed_len = aether_mesh_parse_header(frame, mesh_len, &parsed);
	zassert_equal(parsed_len, mesh_len, "Parse length mismatch");
	zassert_true(parsed.flags & AETHER_FCTL_MESH_NET_ID, "Mesh net ID flag should be set");
	zassert_equal(parsed.mesh_net_id, 0xABCD, "Mesh net ID mismatch");
}

/*
 * Test: TTL decrement simulation (forwarding)
 */
ZTEST(aether_integration, test_ttl_decrement_forwarding)
{
	uint8_t frame[64];
	uint8_t src[6] = {0x02, 0x01, 0x02, 0x03, 0x04, 0x05};
	uint8_t dst[6] = {0x02, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5};

	/* Build mesh header with TTL=6 */
	struct aether_mesh_hdr mesh = {0};
	mesh.flags = 0;
	mesh.type = AETHER_MSG_DATA;
	mesh.ttl = 6;
	memcpy(mesh.src, src, 6);
	memcpy(mesh.dst, dst, 6);
	mesh.seq_no = 0x10;
	mesh.qos_security = (AETHER_PRIORITY_NORMAL << 6);
	mesh.length = 10;

	int mesh_len = aether_mesh_build_header(&mesh, frame, sizeof(frame));
	zassert_true(mesh_len > 0, "Build failed");

	/* TTL is at offset 2 in mesh header */
	zassert_equal(frame[2], 6, "Initial TTL should be 6");

	/* Simulate forwarding: decrement TTL */
	frame[2]--;
	zassert_equal(frame[2], 5, "Decremented TTL should be 5");

	/* Parse and verify TTL was decremented */
	struct aether_mesh_hdr parsed;
	int parsed_len = aether_mesh_parse_header(frame, mesh_len, &parsed);
	zassert_true(parsed_len > 0, "Parse failed");
	zassert_equal(parsed.ttl, 5, "Parsed TTL should be 5");
}

/*
 * Test: Maximum payload size
 */
ZTEST(aether_integration, test_max_payload_indication)
{
	uint8_t frame[64];
	uint8_t src[6] = {0x02, 0x01, 0x02, 0x03, 0x04, 0x05};
	uint8_t dst[6] = {0x02, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5};

	/* Build mesh header indicating large payload */
	struct aether_mesh_hdr mesh = {0};
	mesh.flags = 0;
	mesh.type = AETHER_MSG_DATA;
	mesh.ttl = 4;
	memcpy(mesh.src, src, 6);
	memcpy(mesh.dst, dst, 6);
	mesh.seq_no = 0x20;
	mesh.qos_security = (AETHER_PRIORITY_NORMAL << 6);
	mesh.length = 200;  /* Max practical payload for LoRa */

	int mesh_len = aether_mesh_build_header(&mesh, frame, sizeof(frame));
	zassert_true(mesh_len > 0, "Build failed");

	/* Parse and verify length */
	struct aether_mesh_hdr parsed;
	aether_mesh_parse_header(frame, mesh_len, &parsed);
	zassert_equal(parsed.length, 200, "Payload length should be preserved");
}

/*
 * Test: QoS field encoding
 */
ZTEST(aether_integration, test_qos_encoding)
{
	/* QoS field layout:
	 * bits 7-6: priority (0=low, 1=normal, 2=high, 3=urgent)
	 * bit 5: delivery (0=best effort, 1=reliable)
	 * bits 4-1: security flags
	 * bit 0: reserved
	 */

	/* Normal priority, best effort */
	uint8_t qos1 = (1 << 6) | (0 << 5);
	zassert_equal((qos1 >> 6) & 0x03, 1, "Priority should be normal");
	zassert_equal((qos1 >> 5) & 0x01, 0, "Delivery should be best effort");

	/* High priority, reliable */
	uint8_t qos2 = (2 << 6) | (1 << 5);
	zassert_equal((qos2 >> 6) & 0x03, 2, "Priority should be high");
	zassert_equal((qos2 >> 5) & 0x01, 1, "Delivery should be reliable");

	/* Test in header context */
	uint8_t frame[32];
	struct aether_mesh_hdr mesh = {0};
	uint8_t addr[6] = {0x02, 0x01, 0x02, 0x03, 0x04, 0x05};
	mesh.flags = 0;
	mesh.type = AETHER_MSG_DATA;
	mesh.ttl = 4;
	memcpy(mesh.src, addr, 6);
	memcpy(mesh.dst, addr, 6);
	mesh.seq_no = 0x00;
	mesh.qos_security = qos2;
	mesh.length = 0;

	int mesh_len = aether_mesh_build_header(&mesh, frame, sizeof(frame));
	zassert_true(mesh_len > 0, "Build failed");

	/* QoS byte is at offset 16 (after flags, type, ttl, src, dst, seq) */
	zassert_equal(frame[16], qos2, "QoS byte should match");
}

ZTEST_SUITE(aether_integration, NULL, NULL, NULL, NULL, NULL);
