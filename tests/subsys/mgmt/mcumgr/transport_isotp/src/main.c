/*
 * Copyright (c) 2026, inovex GmbH
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/** @file
 * @brief End-to-end test of the MCUmgr ISO-TP (CAN) SMP transport.
 *
 * The CAN controller selected by the zephyr,canbus chosen node is put into
 * loopback mode and the test acts as the transport's ISO-TP peer on the same
 * controller, using the mirror image of the transport's CAN identifiers (the
 * transport's TX data identifier is the peer's RX data identifier and so on,
 * likewise for the flow-control identifiers).
 *
 * The peer sends SMP os_mgmt echo requests and validates the responses, which
 * exercises the whole chain: ISO-TP reception, SMP packet allocation, command
 * processing and ISO-TP transmission. Scenario-specific tests (see
 * testcase.yaml) cover the second channel, runtime peer retargeting and the
 * SMP client direction.
 */

#include <string.h>

#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/canbus/isotp.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/grp/os_mgmt/os_mgmt.h>
#include <zephyr/mgmt/mcumgr/transport/smp.h>
#include <zephyr/mgmt/mcumgr/transport/smp_isotp.h>

#ifdef CONFIG_SMP_CLIENT
#include <zephyr/mgmt/mcumgr/smp/smp_client.h>
#include <zephyr/mgmt/mcumgr/grp/os_mgmt/os_mgmt_client.h>
#endif

#include <zcbor_common.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>
#include <mgmt/mcumgr/util/zcbor_bulk.h>
#include <mgmt/mcumgr/transport/smp_internal.h>

/* Mirror the identifier flags the transport uses. */
#if defined(CONFIG_MCUMGR_TRANSPORT_ISOTP_EXTENDED_ID)
#define PEER_ID_FLAGS ISOTP_MSG_IDE
#else
#define PEER_ID_FLAGS 0
#endif

#if defined(CONFIG_MCUMGR_TRANSPORT_ISOTP_CAN_FD)
#define PEER_FD_FLAGS (ISOTP_MSG_FDF | ISOTP_MSG_BRS)
#define PEER_TX_DL    CONFIG_MCUMGR_TRANSPORT_ISOTP_TX_DL
#else
#define PEER_FD_FLAGS 0
#define PEER_TX_DL    0
#endif

#define PEER_FLAGS (PEER_ID_FLAGS | PEER_FD_FLAGS)

#define RESPONSE_WAIT      K_SECONDS(3)
#define NO_RESPONSE_WAIT   K_MSEC(500)
#define MSG_BUF_SIZE       512
/* Larger than CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE so the transport must drop it. */
#define OVERSIZED_MSG_SIZE (CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE + 200)

/* Identifiers used by the runtime retargeting test; distinct from every
 * compile-time data and flow-control identifier.
 */
#define ALT_PEER_RX_ID 0xa0
#define ALT_PEER_TX_ID 0x1a0

BUILD_ASSERT(OVERSIZED_MSG_SIZE <= 4095, "oversized test message must still fit in ISO-TP");

static const struct device *const can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));

/* Same flow-control parameters as the transport's defaults. */
static const struct isotp_fc_opts peer_fc_opts = {
	.bs = 8,
	.stmin = 0,
};

/* Peer addressing, the mirror image of the transport's channel 0. */
static const struct isotp_msg_id peer_rx_addr = {
	.ext_id = CONFIG_MCUMGR_TRANSPORT_ISOTP_TX_ID,
	.flags = PEER_FLAGS,
};
static const struct isotp_msg_id peer_fc_tx_addr = {
	.ext_id = CONFIG_MCUMGR_TRANSPORT_ISOTP_RX_FC_ID,
	.flags = PEER_FLAGS,
};
static const struct isotp_msg_id peer_tx_addr = {
	.ext_id = CONFIG_MCUMGR_TRANSPORT_ISOTP_RX_ID,
	.dl = PEER_TX_DL,
	.flags = PEER_FLAGS,
};
static const struct isotp_msg_id peer_fc_rx_addr = {
	.ext_id = CONFIG_MCUMGR_TRANSPORT_ISOTP_TX_FC_ID,
	.flags = PEER_FLAGS,
};

static struct isotp_recv_ctx peer_recv_ctx;
static struct isotp_send_ctx peer_send_ctx;

static uint8_t msg_buf[MSG_BUF_SIZE];

/* Build an os_mgmt echo request (SMP header + CBOR payload) into out. */
static uint16_t build_echo_req(uint8_t *out, size_t cap, const char *str, uint8_t seq)
{
	zcbor_state_t zse[4];
	struct smp_hdr hdr;
	uint8_t *payload = &out[sizeof(hdr)];
	uint16_t payload_len;
	bool ok;

	zcbor_new_encode_state(zse, ARRAY_SIZE(zse), payload, cap - sizeof(hdr), 0);
	ok = zcbor_map_start_encode(zse, 2) &&
	     zcbor_tstr_put_lit(zse, "d") &&
	     zcbor_tstr_encode_ptr(zse, str, strlen(str)) &&
	     zcbor_map_end_encode(zse, 2);
	zassert_true(ok, "Failed to encode echo request");

	payload_len = (uint16_t)(zse->payload_mut - payload);

	hdr = (struct smp_hdr){
		.nh_len = sys_cpu_to_be16(payload_len),
		.nh_flags = 0,
		.nh_op = MGMT_OP_READ,
		.nh_group = sys_cpu_to_be16(MGMT_GROUP_ID_OS),
		.nh_seq = seq,
		.nh_id = OS_MGMT_ID_ECHO,
		.nh_version = 1,
	};
	memcpy(out, &hdr, sizeof(hdr));

	return payload_len + sizeof(hdr);
}

/* Receive one complete ISO-TP message; returns its length or a negative
 * ISOTP_RECV_* / ISOTP_N_* error. Once the first fragment has arrived the
 * remainder is awaited with a generous fixed timeout.
 */
static int peer_recv_msg(struct isotp_recv_ctx *ctx, uint8_t *buf, size_t cap,
			 k_timeout_t first_timeout)
{
	struct net_buf *frag;
	size_t len = 0;
	bool first = true;
	int rem_len;

	do {
		rem_len = isotp_recv_net(ctx, &frag, first ? first_timeout : RESPONSE_WAIT);
		if (rem_len < 0) {
			return rem_len;
		}
		first = false;

		while (frag != NULL) {
			zassert_true(len + frag->len <= cap, "peer receive buffer overflow");
			memcpy(&buf[len], frag->data, frag->len);
			len += frag->len;
			frag = net_buf_frag_del(NULL, frag);
		}
	} while (rem_len > 0);

	return (int)len;
}

/* Send an echo request to the transport and validate the echo response, using
 * the given peer contexts/addresses (so retargeted/second-channel variants can
 * reuse it).
 */
static void echo_roundtrip(struct isotp_recv_ctx *rx_ctx, const struct isotp_msg_id *tx_addr,
			   const struct isotp_msg_id *fc_rx_addr, const char *str, uint8_t seq)
{
	struct smp_hdr hdr;
	struct zcbor_string echo_rsp = { 0 };
	zcbor_state_t zsd[4];
	size_t decoded = 0;
	uint16_t req_len;
	int len;
	int rc;

	struct zcbor_map_decode_key_val echo_decode[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("r", zcbor_tstr_decode, &echo_rsp),
	};

	req_len = build_echo_req(msg_buf, sizeof(msg_buf), str, seq);

	rc = isotp_send(&peer_send_ctx, can_dev, msg_buf, req_len, tx_addr, fc_rx_addr, NULL,
			NULL);
	zassert_equal(rc, ISOTP_N_OK, "ISO-TP send failed: %d", rc);

	len = peer_recv_msg(rx_ctx, msg_buf, sizeof(msg_buf), RESPONSE_WAIT);
	zassert_true(len >= (int)sizeof(hdr), "No or truncated SMP response: %d", len);

	memcpy(&hdr, msg_buf, sizeof(hdr));
	zassert_equal(hdr.nh_op, MGMT_OP_READ_RSP, "SMP header operation mismatch");
	zassert_equal(sys_be16_to_cpu(hdr.nh_group), MGMT_GROUP_ID_OS,
		      "SMP header group mismatch");
	zassert_equal(hdr.nh_seq, seq, "SMP header sequence number mismatch");
	zassert_equal(hdr.nh_id, OS_MGMT_ID_ECHO, "SMP header command ID mismatch");
	zassert_equal(sys_be16_to_cpu(hdr.nh_len), len - sizeof(hdr),
		      "SMP header length mismatch");

	zcbor_new_decode_state(zsd, ARRAY_SIZE(zsd), &msg_buf[sizeof(hdr)], len - sizeof(hdr), 1,
			       NULL, 0);
	rc = zcbor_map_decode_bulk(zsd, echo_decode, ARRAY_SIZE(echo_decode), &decoded);
	zassert_equal(rc, 0, "Failed to decode echo response");
	zassert_equal(decoded, 1, "Expected 1 decoded element");
	zassert_equal(echo_rsp.len, strlen(str), "Echo response length mismatch");
	zassert_mem_equal(echo_rsp.value, str, echo_rsp.len, "Echo response mismatch");
}

ZTEST(transport_isotp, test_echo)
{
	echo_roundtrip(&peer_recv_ctx, &peer_tx_addr, &peer_fc_rx_addr, "Hello over ISO-TP!", 1);
}

ZTEST(transport_isotp, test_echo_multi_frame)
{
	char long_str[261];

	/* Long enough that request and response are segmented into (many)
	 * consecutive frames with intermediate flow control in both directions.
	 */
	for (size_t i = 0; i < sizeof(long_str) - 1; i++) {
		long_str[i] = 'a' + (i % 26);
	}
	long_str[sizeof(long_str) - 1] = '\0';

	echo_roundtrip(&peer_recv_ctx, &peer_tx_addr, &peer_fc_rx_addr, long_str, 2);
}

ZTEST(transport_isotp, test_oversized_message_dropped)
{
	static uint8_t oversized[OVERSIZED_MSG_SIZE];
	int rc;

	/* A message larger than the SMP packet buffer must be dropped in full
	 * (no response) without wedging the receive path.
	 */
	memset(oversized, 0xaa, sizeof(oversized));

	rc = isotp_send(&peer_send_ctx, can_dev, oversized, sizeof(oversized), &peer_tx_addr,
			&peer_fc_rx_addr, NULL, NULL);
	zassert_equal(rc, ISOTP_N_OK, "ISO-TP send failed: %d", rc);

	rc = peer_recv_msg(&peer_recv_ctx, msg_buf, sizeof(msg_buf), NO_RESPONSE_WAIT);
	zassert_equal(rc, ISOTP_RECV_TIMEOUT, "Expected no response, got %d", rc);

	/* The transport must still answer afterwards. */
	echo_roundtrip(&peer_recv_ctx, &peer_tx_addr, &peer_fc_rx_addr, "still alive", 3);
}

#if defined(CONFIG_MCUMGR_TRANSPORT_ISOTP_SECOND_CHANNEL)
ZTEST(transport_isotp, test_second_channel)
{
	static const struct isotp_msg_id peer2_rx_addr = {
		.ext_id = CONFIG_MCUMGR_TRANSPORT_ISOTP_TX_ID2,
		.flags = PEER_FLAGS,
	};
	static const struct isotp_msg_id peer2_fc_tx_addr = {
		.ext_id = CONFIG_MCUMGR_TRANSPORT_ISOTP_RX_FC_ID2,
		.flags = PEER_FLAGS,
	};
	static const struct isotp_msg_id peer2_tx_addr = {
		.ext_id = CONFIG_MCUMGR_TRANSPORT_ISOTP_RX_ID2,
		.dl = PEER_TX_DL,
		.flags = PEER_FLAGS,
	};
	static const struct isotp_msg_id peer2_fc_rx_addr = {
		.ext_id = CONFIG_MCUMGR_TRANSPORT_ISOTP_TX_FC_ID2,
		.flags = PEER_FLAGS,
	};
	static struct isotp_recv_ctx peer2_recv_ctx;
	int rc;

	rc = isotp_bind(&peer2_recv_ctx, can_dev, &peer2_rx_addr, &peer2_fc_tx_addr,
			&peer_fc_opts, K_FOREVER);
	zassert_equal(rc, ISOTP_N_OK, "Failed to bind second channel peer: %d", rc);

	echo_roundtrip(&peer2_recv_ctx, &peer2_tx_addr, &peer2_fc_rx_addr,
		       "second channel echo", 4);

	/* Channel 0 must be unaffected. */
	echo_roundtrip(&peer_recv_ctx, &peer_tx_addr, &peer_fc_rx_addr, "first channel echo", 5);

	isotp_unbind(&peer2_recv_ctx);
}
#endif /* CONFIG_MCUMGR_TRANSPORT_ISOTP_SECOND_CHANNEL */

#if defined(CONFIG_MCUMGR_TRANSPORT_ISOTP_RUNTIME_PEER)
ZTEST(transport_isotp, test_runtime_peer_retarget)
{
	static const struct isotp_msg_id alt_peer_rx_addr = {
		.ext_id = ALT_PEER_TX_ID,
		.flags = PEER_FLAGS,
	};
	static const struct isotp_msg_id alt_peer_tx_addr = {
		.ext_id = ALT_PEER_RX_ID,
		.dl = PEER_TX_DL,
		.flags = PEER_FLAGS,
	};
	static struct isotp_recv_ctx alt_peer_recv_ctx;
	int rc;

	/* Data identifiers must not collide with the flow-control identifiers. */
	rc = smp_isotp_set_peer(CONFIG_MCUMGR_TRANSPORT_ISOTP_TX_FC_ID, ALT_PEER_TX_ID);
	zassert_equal(rc, -EINVAL, "Expected FC collision to be rejected, got %d", rc);

	rc = smp_isotp_set_peer(ALT_PEER_RX_ID, ALT_PEER_TX_ID);
	zassert_equal(rc, 0, "Failed to retarget peer: %d", rc);

	/* The flow-control identifiers stay the same after a retarget. */
	rc = isotp_bind(&alt_peer_recv_ctx, can_dev, &alt_peer_rx_addr, &peer_fc_tx_addr,
			&peer_fc_opts, K_FOREVER);
	zassert_equal(rc, ISOTP_N_OK, "Failed to bind retargeted peer: %d", rc);

	echo_roundtrip(&alt_peer_recv_ctx, &alt_peer_tx_addr, &peer_fc_rx_addr,
		       "echo after retarget", 6);

	/* The previous identifiers must no longer be answered. The probe must fit
	 * a single frame: a multi-frame message would already fail to send, as
	 * nobody flow-controls the old identifiers any more.
	 */
	memset(msg_buf, 0, 6);
	rc = isotp_send(&peer_send_ctx, can_dev, msg_buf, 6, &peer_tx_addr, &peer_fc_rx_addr,
			NULL, NULL);
	zassert_equal(rc, ISOTP_N_OK, "ISO-TP send failed: %d", rc);
	rc = peer_recv_msg(&peer_recv_ctx, msg_buf, sizeof(msg_buf), NO_RESPONSE_WAIT);
	zassert_equal(rc, ISOTP_RECV_TIMEOUT, "Expected no response on old identifiers, got %d",
		      rc);

	isotp_unbind(&alt_peer_recv_ctx);

	/* Restore the compile-time identifiers for the remaining tests. */
	rc = smp_isotp_set_peer(CONFIG_MCUMGR_TRANSPORT_ISOTP_RX_ID,
				CONFIG_MCUMGR_TRANSPORT_ISOTP_TX_ID);
	zassert_equal(rc, 0, "Failed to restore peer: %d", rc);

	echo_roundtrip(&peer_recv_ctx, &peer_tx_addr, &peer_fc_rx_addr, "echo after restore", 8);
}
#endif /* CONFIG_MCUMGR_TRANSPORT_ISOTP_RUNTIME_PEER */

#ifdef CONFIG_SMP_CLIENT
#define CLIENT_ECHO_STR "client echo over ISO-TP"

static struct smp_client_object smp_client;
static struct os_mgmt_client os_client;
static int client_echo_rc;
static K_SEM_DEFINE(client_done_sem, 0, 1);
static K_THREAD_STACK_DEFINE(client_stack, 2048);
static struct k_thread client_thread;

static void client_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	client_echo_rc = os_mgmt_client_echo(&os_client, CLIENT_ECHO_STR,
					     strlen(CLIENT_ECHO_STR));
	k_sem_give(&client_done_sem);
}

ZTEST(transport_isotp, test_client_echo)
{
	struct smp_hdr hdr;
	struct zcbor_string echo_req = { 0 };
	zcbor_state_t zsd[4];
	zcbor_state_t zse[4];
	size_t decoded = 0;
	uint16_t payload_len;
	bool ok;
	int len;
	int rc;

	struct zcbor_map_decode_key_val req_decode[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("d", zcbor_tstr_decode, &echo_req),
	};

	rc = smp_client_object_init(&smp_client, SMP_ISOTP_TRANSPORT);
	zassert_equal(rc, MGMT_ERR_EOK, "SMP client init failed: %d", rc);
	os_mgmt_client_init(&os_client, &smp_client);

	/* os_mgmt_client_echo() blocks until the response arrives, so it runs in
	 * its own thread while this thread plays the peer (server) side.
	 */
	k_thread_create(&client_thread, client_stack, K_THREAD_STACK_SIZEOF(client_stack),
			client_thread_fn, NULL, NULL, NULL, K_PRIO_PREEMPT(1), 0, K_NO_WAIT);

	/* Receive and validate the echo request sent by the client. */
	len = peer_recv_msg(&peer_recv_ctx, msg_buf, sizeof(msg_buf), RESPONSE_WAIT);
	zassert_true(len >= (int)sizeof(hdr), "No or truncated SMP request: %d", len);

	memcpy(&hdr, msg_buf, sizeof(hdr));
	zassert_equal(hdr.nh_op, MGMT_OP_WRITE, "SMP header operation mismatch");
	zassert_equal(sys_be16_to_cpu(hdr.nh_group), MGMT_GROUP_ID_OS,
		      "SMP header group mismatch");
	zassert_equal(hdr.nh_id, OS_MGMT_ID_ECHO, "SMP header command ID mismatch");

	zcbor_new_decode_state(zsd, ARRAY_SIZE(zsd), &msg_buf[sizeof(hdr)], len - sizeof(hdr), 1,
			       NULL, 0);
	rc = zcbor_map_decode_bulk(zsd, req_decode, ARRAY_SIZE(req_decode), &decoded);
	zassert_equal(rc, 0, "Failed to decode echo request");
	zassert_equal(echo_req.len, strlen(CLIENT_ECHO_STR), "Echo request length mismatch");
	zassert_mem_equal(echo_req.value, CLIENT_ECHO_STR, echo_req.len,
			  "Echo request mismatch");

	/* Turn the request into a response in place: bump the operation, replace
	 * the payload with {"r": <echoed string>} and send it back.
	 */
	hdr.nh_op = MGMT_OP_WRITE_RSP;

	zcbor_new_encode_state(zse, ARRAY_SIZE(zse), &msg_buf[sizeof(hdr)],
			       sizeof(msg_buf) - sizeof(hdr), 0);
	ok = zcbor_map_start_encode(zse, 2) &&
	     zcbor_tstr_put_lit(zse, "r") &&
	     zcbor_tstr_put_lit(zse, CLIENT_ECHO_STR) &&
	     zcbor_map_end_encode(zse, 2);
	zassert_true(ok, "Failed to encode echo response");

	payload_len = (uint16_t)(zse->payload_mut - &msg_buf[sizeof(hdr)]);
	hdr.nh_len = sys_cpu_to_be16(payload_len);
	memcpy(msg_buf, &hdr, sizeof(hdr));

	rc = isotp_send(&peer_send_ctx, can_dev, msg_buf, sizeof(hdr) + payload_len,
			&peer_tx_addr, &peer_fc_rx_addr, NULL, NULL);
	zassert_equal(rc, ISOTP_N_OK, "ISO-TP send failed: %d", rc);

	rc = k_sem_take(&client_done_sem, RESPONSE_WAIT);
	zassert_equal(rc, 0, "SMP client did not complete");
	zassert_equal(client_echo_rc, MGMT_ERR_EOK, "SMP client echo failed: %d",
		      client_echo_rc);
}
#endif /* CONFIG_SMP_CLIENT */

static void *transport_isotp_setup(void)
{
	can_mode_t mode = CAN_MODE_LOOPBACK;
	int rc;

	zassert_true(device_is_ready(can_dev), "CAN device not ready");

#ifdef CONFIG_MCUMGR_TRANSPORT_ISOTP_CAN_FD
	mode |= CAN_MODE_FD;
#endif

	/* CONFIG_MCUMGR_TRANSPORT_ISOTP_AUTO_START is disabled, so the CAN
	 * controller is still stopped here and the test owns its mode.
	 */
	rc = can_set_mode(can_dev, mode);
	zassert_equal(rc, 0, "Failed to set CAN loopback mode: %d", rc);

	rc = can_start(can_dev);
	zassert_true(rc == 0 || rc == -EALREADY, "Failed to start CAN device: %d", rc);

	rc = isotp_bind(&peer_recv_ctx, can_dev, &peer_rx_addr, &peer_fc_tx_addr, &peer_fc_opts,
			K_FOREVER);
	zassert_equal(rc, ISOTP_N_OK, "Failed to bind peer RX address: %d", rc);

	/* The transport's receive threads bind their ISO-TP contexts from their
	 * own (preemptible) threads; give them a chance to run before the first
	 * request is sent, or its first frame would be lost.
	 */
	k_sleep(K_MSEC(100));

	return NULL;
}

ZTEST_SUITE(transport_isotp, NULL, transport_isotp_setup, NULL, NULL, NULL);
