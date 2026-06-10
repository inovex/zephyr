/*
 * Copyright (c) 2026, inovex GmbH
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/** @file
 * @brief UDP transport backend for the MCUmgr SMP client shell.
 *
 * Contributes the transport-specific subcommands under "smpc udp":
 *
 *   smpc udp target <ip> [port]   select the peer to send SMP requests to
 *
 * "target" also makes UDP (IPv4 or IPv6, matching the address) the shell's
 * active SMP transport (equivalent to "smpc transport udp"/"udp6" first), so
 * addressing a node is a single command even when several transports are
 * enabled. The port defaults to CONFIG_MCUMGR_TRANSPORT_UDP_PORT.
 *
 * The destination address is attached to the shell's SMP client as transport
 * data; the UDP transport copies it into each request buffer (ud_init) and
 * uses it as the sendto() destination.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_string_conv.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/mgmt/mcumgr/smp/smp_client_shell.h>
#include <zephyr/mgmt/mcumgr/transport/smp.h>

/* Destination for client requests; stays alive while attached to the client. */
static struct net_sockaddr udp_peer;

static int cmd_udp_target(const struct shell *sh, size_t argc, char **argv)
{
	uint16_t port = CONFIG_MCUMGR_TRANSPORT_UDP_PORT;
	struct net_sockaddr peer;
	int transport;
	int rc;

	if (argc == 3) {
		int err = 0;
		unsigned long v = shell_strtoul(argv[2], 0, &err);

		if (err != 0 || v == 0 || v > UINT16_MAX) {
			shell_error(sh, "invalid port: %s", argv[2]);
			return -EINVAL;
		}
		port = (uint16_t)v;
	}

	memset(&peer, 0, sizeof(peer));

	if (IS_ENABLED(CONFIG_MCUMGR_TRANSPORT_UDP_IPV4) &&
	    net_addr_pton(NET_AF_INET, argv[1], &net_sin(&peer)->sin_addr) == 0) {
		net_sin(&peer)->sin_family = NET_AF_INET;
		net_sin(&peer)->sin_port = net_htons(port);
		transport = SMP_UDP_IPV4_TRANSPORT;
	} else if (IS_ENABLED(CONFIG_MCUMGR_TRANSPORT_UDP_IPV6) &&
		   net_addr_pton(NET_AF_INET6, argv[1], &net_sin6(&peer)->sin6_addr) == 0) {
		net_sin6(&peer)->sin6_family = NET_AF_INET6;
		net_sin6(&peer)->sin6_port = net_htons(port);
		transport = SMP_UDP_IPV6_TRANSPORT;
	} else {
		shell_error(sh, "invalid address: %s", argv[1]);
		return -EINVAL;
	}

	rc = smp_client_shell_set_transport(transport);
	if (rc != 0) {
		shell_error(sh, "selecting udp transport failed: %d%s", rc,
			    rc == -ENOENT ? " (transport not registered; is the network up?)"
					  : "");
		return rc;
	}

	/* Publish only after the switch: switching clears the transport data. */
	udp_peer = peer;
	smp_client_shell_set_transport_data(&udp_peer);

	shell_print(sh, "peer set: %s port %u", argv[1], port);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_smpc_udp,
	SHELL_CMD_ARG(target, NULL, "Select peer address: target <ip> [port]",
		      cmd_udp_target, 2, 1),
	SHELL_SUBCMD_SET_END
);

/* Contribute "udp" under the generic "smpc" root command. */
SHELL_SUBCMD_ADD((smpc), udp, &sub_smpc_udp,
		 "UDP transport-specific commands", NULL, 1, 0);
