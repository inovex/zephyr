/*
 * Copyright (c) 2026, inovex GmbH
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * MCUmgr SMP server over ISO-TP (CAN).
 *
 * This application does almost nothing in main(): the ISO-TP transport is
 * brought up automatically by the MCUmgr handler initialisation, binds to the
 * configured RX CAN identifier and waits for incoming SMP requests. The OS
 * management group answers commands such as "echo" and "info".
 *
 * It is meant to be driven by the companion "client" sample (or any SMP/MCUmgr
 * tool that speaks ISO-TP), see the README for how to wire them together.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

int main(void)
{
	LOG_INF("MCUmgr ISO-TP (CAN) SMP server started");
	LOG_INF("Listening for SMP requests on RX CAN id 0x%x, replying on 0x%x",
		CONFIG_MCUMGR_TRANSPORT_ISOTP_RX_ID, CONFIG_MCUMGR_TRANSPORT_ISOTP_TX_ID);

#ifdef CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION
	/* Only defined when built with sysbuild + MCUboot (signed image). */
	LOG_INF("App version: %s", CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION);
#endif

	return 0;
}
