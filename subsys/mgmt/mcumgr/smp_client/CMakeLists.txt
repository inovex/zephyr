#
# Copyright (c) 2023 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: Apache-2.0
#
# Protocol API is only exposed to MCUmgr internals.

zephyr_library_named(mcumgr_smp_client)
zephyr_library_sources(src/client.c)

zephyr_library_sources_ifdef(CONFIG_MCUMGR_SMP_CLIENT_SHELL src/smp_client_shell.c)
# The image management client headers pull in MCUboot's <bootutil/image.h>;
# linking the bootutil interface library puts it on the include path.
zephyr_library_link_libraries_ifdef(CONFIG_MCUMGR_SMP_CLIENT_SHELL_IMG MCUBOOT_BOOTUTIL)
