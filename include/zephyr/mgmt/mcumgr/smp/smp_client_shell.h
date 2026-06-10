/*
 * Copyright (c) 2026, inovex GmbH
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef H_SMP_CLIENT_SHELL_
#define H_SMP_CLIENT_SHELL_

/**
 * @brief MCUmgr SMP client shell backend API
 *
 * API for transport backends of the "smpc" shell
 * (CONFIG_MCUMGR_SMP_CLIENT_SHELL). A backend contributes its subcommands to
 * the "smpc" set via SHELL_SUBCMD_ADD((smpc), ...) from its own translation
 * unit and uses these functions to take over the shell's SMP client, e.g.
 * when its "target" command selects a peer.
 *
 * @defgroup mcumgr_smp_client_shell SMP client shell
 * @ingroup mcumgr
 * @{
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bind the shell's SMP client to a transport at runtime.
 *
 * The transport must have registered itself with the SMP client transport
 * registry (smp_client_transport_register()). Switching re-initializes the
 * management clients and clears any transport data previously set with
 * smp_client_shell_set_transport_data(). Re-selecting the already-active
 * transport is a no-op.
 *
 * @param smpt_type	Transport (enum smp_transport_type).
 *
 * @return 0 on success
 * @return -ENOENT if no such transport is registered for client use
 * @return -EIO if binding the client failed
 */
int smp_client_shell_set_transport(int smpt_type);

/**
 * @brief Attach per-transport client data to the shell's SMP client.
 *
 * The pointer is handed to the active transport's ud_init hook for every
 * request (see smp_client_object_set_data()); for the UDP transport it is the
 * destination address (struct net_sockaddr). The caller keeps ownership and
 * must keep the object alive; it is dropped on the next transport switch.
 *
 * @param priv	Transport-specific data, or NULL to clear.
 */
void smp_client_shell_set_transport_data(void *priv);

#ifdef __cplusplus
}
#endif

/**
 * @}
 */

#endif /* H_SMP_CLIENT_SHELL_ */
