/*
 * Copyright (c) 2026, inovex GmbH
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/** @file
 * @brief ISO-TP (CAN) transport backend for the MCUmgr SMP client shell.
 *
 * Contributes the transport-specific subcommands under "smpc isotp":
 *
 *   smpc isotp target <rx_id> <tx_id>   retarget the peer's data CAN identifiers
 *
 * "target" also makes ISO-TP the shell's active SMP transport (equivalent to
 * "smpc transport isotp" first), so addressing a CAN node is a single command
 * even when several transports are enabled.
 */

#include <errno.h>
#include <stdint.h>

#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_string_conv.h>
#include <zephyr/mgmt/mcumgr/smp/smp_client_shell.h>
#include <zephyr/mgmt/mcumgr/transport/smp.h>
#include <zephyr/mgmt/mcumgr/transport/smp_isotp.h>

static int cmd_isotp_target(const struct shell *sh, size_t argc, char **argv)
{
	int err = 0;
	uint32_t rx_id, tx_id;
	int rc;

	rx_id = (uint32_t)shell_strtoul(argv[1], 0, &err);
	if (err == 0) {
		tx_id = (uint32_t)shell_strtoul(argv[2], 0, &err);
	}
	if (err != 0) {
		shell_error(sh, "invalid CAN identifier");
		return -EINVAL;
	}

	rc = smp_client_shell_set_transport(SMP_ISOTP_TRANSPORT);
	if (rc != 0) {
		shell_error(sh, "selecting isotp transport failed: %d", rc);
		return rc;
	}

	rc = smp_isotp_set_peer(rx_id, tx_id);
	if (rc != 0) {
		shell_error(sh, "target failed: %d%s", rc,
			    rc == -EINVAL ? " (id collides with a flow-control id)" : "");
		return rc;
	}

	shell_print(sh, "peer set: rx=0x%x tx=0x%x", rx_id, tx_id);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_smpc_isotp,
	SHELL_CMD_ARG(target, NULL, "Select peer data CAN ids: target <rx_id> <tx_id>",
		      cmd_isotp_target, 3, 0),
	SHELL_SUBCMD_SET_END
);

/* Contribute "isotp" under the generic "smpc" root command. */
SHELL_SUBCMD_ADD((smpc), isotp, &sub_smpc_isotp,
		 "ISO-TP (CAN) transport-specific commands", NULL, 1, 0);
