/*
 * Copyright (c) 2026, inovex GmbH
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/** @file
 * @brief Test of the MCUmgr SMP client shell ("smpc").
 *
 * The test registers a loopback SMP transport in the client transport
 * registry: everything the transport outputs is fed straight back into the
 * local SMP processing. A request sent by the SMP client is therefore handled
 * by the SMP server groups of the same image, whose response travels the same
 * way back and is routed to the client again. This gives the shell a real,
 * fully processed request/response round trip without any bus.
 *
 * The shell commands are driven through the dummy shell backend and verified
 * by their console output and return codes.
 */

#include <string.h>

#include <zephyr/ztest.h>
#include <zephyr/net_buf.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>

#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/grp/os_mgmt/os_mgmt_callbacks.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/mgmt/mcumgr/smp/smp_client.h>
#include <zephyr/mgmt/mcumgr/transport/smp.h>

#include <mgmt/mcumgr/transport/smp_internal.h>

static int loopback_output(struct net_buf *nb);
static uint16_t loopback_get_mtu(const struct net_buf *nb);

static struct smp_transport loopback_transport = {
	.functions.output = loopback_output,
	.functions.get_mtu = loopback_get_mtu,
};

static int loopback_output(struct net_buf *nb)
{
	struct net_buf *clone;

	/* Behave like a real transport: the receiver gets its own copy of the
	 * packet. Feeding nb itself into the RX path would let the server
	 * consume the very buffer the SMP client keeps for retries and response
	 * matching.
	 */
	clone = smp_packet_alloc();
	if (clone == NULL) {
		smp_packet_free(nb);
		return MGMT_ERR_ENOMEM;
	}
	net_buf_add_mem(clone, nb->data, nb->len);
	smp_packet_free(nb);

	/* Both requests (from the client) and responses (from the server) come
	 * through here; the SMP processing dispatches them by operation.
	 */
	smp_rx_req(&loopback_transport, clone);

	return 0;
}

static uint16_t loopback_get_mtu(const struct net_buf *nb)
{
	ARG_UNUSED(nb);

	return CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE;
}

static struct smp_client_transport_entry loopback_client_transport = {
	.smpt = &loopback_transport,
	.smpt_type = SMP_USER_DEFINED_TRANSPORT,
};

/* Deny "smpc reset" at the server side so the round trip can be verified
 * without actually rebooting the test binary.
 */
static enum mgmt_cb_return reset_deny_cb(uint32_t event, enum mgmt_cb_return prev_status,
					 int32_t *rc, uint16_t *group, bool *abort_more,
					 void *data, size_t data_size)
{
	ARG_UNUSED(event);
	ARG_UNUSED(prev_status);
	ARG_UNUSED(group);
	ARG_UNUSED(abort_more);
	ARG_UNUSED(data);
	ARG_UNUSED(data_size);

	*rc = MGMT_ERR_EBUSY;

	return MGMT_CB_ERROR_RC;
}

static struct mgmt_callback reset_deny_callback = {
	.callback = reset_deny_cb,
	.event_id = MGMT_EVT_OP_OS_MGMT_RESET,
};

/* Run a shell command and return its result and (NUL terminated) output. */
static int run_cmd(const char *cmd, const char **output)
{
	const struct shell *sh = shell_backend_dummy_get_ptr();
	size_t size;
	int rc;

	shell_backend_dummy_clear_output(sh);

	rc = shell_execute_cmd(sh, cmd);
	*output = shell_backend_dummy_get_output(sh, &size);

	return rc;
}

ZTEST(smp_client_shell, test_transport_selection)
{
	const char *out;
	int rc;

	/* The loopback transport is registered as the "user" type. */
	rc = run_cmd("smpc transport", &out);
	zassert_equal(rc, 0, "transport list failed: %d (%s)", rc, out);
	zassert_not_null(strstr(out, "user"), "expected 'user' in transport list: %s", out);

	rc = run_cmd("smpc transport nonsuch", &out);
	zassert_not_equal(rc, 0, "expected unknown transport to fail");
	zassert_not_null(strstr(out, "unknown transport"), "unexpected output: %s", out);

	/* "serial" is a known name but no such transport is registered. */
	rc = run_cmd("smpc transport serial", &out);
	zassert_not_equal(rc, 0, "expected unregistered transport to fail");
	zassert_not_null(strstr(out, "not registered"), "unexpected output: %s", out);

	rc = run_cmd("smpc transport user", &out);
	zassert_equal(rc, 0, "transport select failed: %d (%s)", rc, out);
	zassert_not_null(strstr(out, "active transport: user"), "unexpected output: %s", out);

	rc = run_cmd("smpc transport", &out);
	zassert_equal(rc, 0, "transport list failed: %d (%s)", rc, out);
	zassert_not_null(strstr(out, "user (active)"), "expected active marker: %s", out);
}

ZTEST(smp_client_shell, test_echo)
{
	const char *out;
	int rc;

	/* With exactly one registered transport the client binds automatically,
	 * so this works whether or not the transport was selected before.
	 */
	rc = run_cmd("smpc echo round-trip-through-the-loopback", &out);
	zassert_equal(rc, 0, "echo failed: %d (%s)", rc, out);
	zassert_not_null(strstr(out, "echo OK"), "unexpected output: %s", out);
}

ZTEST(smp_client_shell, test_reset)
{
	const char *out;
	int rc;

	/* The reset hook rejects the request with MGMT_ERR_EBUSY so the test
	 * binary does not actually reboot; the OS client library reports
	 * success for any response, so the command completing proves the
	 * request/response round trip.
	 */
	rc = run_cmd("smpc reset", &out);
	zassert_equal(rc, 0, "reset failed: %d (%s)", rc, out);
	zassert_not_null(strstr(out, "reset request sent"), "unexpected output: %s", out);
}

static void *smp_client_shell_setup(void)
{
	int rc;

	rc = smp_transport_init(&loopback_transport);
	zassert_equal(rc, 0, "Failed to init loopback transport: %d", rc);

	smp_client_transport_register(&loopback_client_transport);
	mgmt_callback_register(&reset_deny_callback);

	/* Let the dummy shell backend finish initialising. */
	k_msleep(10);

	return NULL;
}

ZTEST_SUITE(smp_client_shell, NULL, smp_client_shell_setup, NULL, NULL, NULL);
