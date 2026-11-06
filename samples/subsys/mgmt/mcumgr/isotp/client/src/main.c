/*
 * Copyright (c) 2026, inovex GmbH
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * MCUmgr SMP client over ISO-TP (CAN) - "main controller".
 *
 * This application turns the device into a main controller that performs
 * firmware updates on other nodes over the ISO-TP (CAN) transport, driven
 * interactively from the console with the "smpc" shell commands (see
 * smp_client_shell.c). Firmware images and files are sourced from the local
 * littlefs file system mounted at /lfs.
 *
 * The ISO-TP transport and the SMP client are brought up by the MCUmgr handler
 * initialisation and the shell; main() only reports startup status.
 */

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/transport/smp_isotp.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#define LFS_MOUNT_POINT "/lfs"

int main(void)
{
	struct fs_statvfs sbuf;
	int rc;

	LOG_INF("MCUmgr ISO-TP (CAN) SMP client (main controller) started");
	LOG_INF("Default peer: TX 0x%x / RX 0x%x; retarget with 'smpc isotp target <rx> <tx>'",
		CONFIG_MCUMGR_TRANSPORT_ISOTP_TX_ID, CONFIG_MCUMGR_TRANSPORT_ISOTP_RX_ID);

	/* The file system is auto-mounted via the devicetree fstab entry; report
	 * whether it is available so missing storage is obvious at boot.
	 */
	rc = fs_statvfs(LFS_MOUNT_POINT, &sbuf);
	if (rc == 0) {
		LOG_INF("Image source %s mounted: %lu KiB free", LFS_MOUNT_POINT,
			(unsigned long)(sbuf.f_bfree * sbuf.f_frsize) / 1024);
	} else {
		LOG_WRN("%s not mounted (%d): stage firmware there before uploading",
			LFS_MOUNT_POINT, rc);
	}

	LOG_INF("Ready. Type 'smpc' for SMP client commands, 'fs' to manage %s",
		LFS_MOUNT_POINT);

	return 0;
}
