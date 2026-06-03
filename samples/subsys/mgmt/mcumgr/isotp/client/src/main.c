/*
 * Copyright (c) 2026, inovex GmbH
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * MCUmgr SMP client over ISO-TP (CAN).
 *
 * This application uses the MCUmgr SMP client together with the OS management
 * client to periodically send an "echo" request to the companion "server"
 * sample over the ISO-TP (CAN) transport, and logs the outcome.
 *
 * The ISO-TP transport itself is brought up automatically by the MCUmgr handler
 * initialisation; because CONFIG_SMP_CLIENT is enabled it registers itself as a
 * client transport of type SMP_ISOTP_TRANSPORT, which is what we look up below.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt_defines.h>
#include <zephyr/mgmt/mcumgr/smp/smp_client.h>
#include <zephyr/mgmt/mcumgr/grp/os_mgmt/os_mgmt_client.h>
#include <zephyr/mgmt/mcumgr/transport/smp.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#define ECHO_STRING   "Hello ISO-TP"
#define ECHO_INTERVAL K_SECONDS(2)

static struct smp_client_object smp_client;
static struct os_mgmt_client os_client;

int main(void)
{
	int rc;
	unsigned int count = 0;

	LOG_INF("MCUmgr ISO-TP (CAN) SMP client started");
	LOG_INF("Sending to server RX CAN id 0x%x, listening on 0x%x",
		CONFIG_MCUMGR_TRANSPORT_ISOTP_TX_ID, CONFIG_MCUMGR_TRANSPORT_ISOTP_RX_ID);

	/* Discover the ISO-TP transport (registered as SMP_ISOTP_TRANSPORT). */
	rc = smp_client_object_init(&smp_client, SMP_ISOTP_TRANSPORT);
	if (rc != MGMT_ERR_EOK) {
		LOG_ERR("Failed to init SMP client object: %d", rc);
		return 0;
	}

	os_mgmt_client_init(&os_client, &smp_client);

	while (1) {
		LOG_INF("[%u] Sending echo: \"%s\"", count, ECHO_STRING);

		rc = os_mgmt_client_echo(&os_client, ECHO_STRING, strlen(ECHO_STRING));
		if (rc == MGMT_ERR_EOK) {
			LOG_INF("[%u] Echo round-trip OK", count);
		} else {
			LOG_WRN("[%u] Echo failed: %d (is the server running?)", count, rc);
		}

		++count;
		k_sleep(ECHO_INTERVAL);
	}

	return 0;
}
