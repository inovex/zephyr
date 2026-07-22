/*
 * Copyright (c) 2026, inovex GmbH
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_MGMT_MCUMGR_TRANSPORT_SMP_ISOTP_H_
#define ZEPHYR_INCLUDE_MGMT_MCUMGR_TRANSPORT_SMP_ISOTP_H_

#include <zephyr/mgmt/mcumgr/transport/smp.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief MCUmgr SMP transport over ISO-TP (CAN)
 * @defgroup mcumgr_transport_isotp SMP ISO-TP (CAN) transport
 * @ingroup mcumgr_transport_smp
 * @{
 */

/**
 * @brief The ISO-TP (CAN) SMP transport object.
 *
 * Exposed so applications and tests can reference the transport, e.g. to drive
 * it directly or to register additional callbacks. The transport is set up
 * automatically during MCUmgr handler initialisation; applications do not need
 * to initialise it themselves.
 */
extern struct smp_transport smp_isotp_transport;

#if defined(CONFIG_MCUMGR_TRANSPORT_ISOTP_RUNTIME_PEER) || defined(__DOXYGEN__)
/**
 * @brief Retarget the ISO-TP transport to a different peer at runtime.
 *
 * Changes the data CAN identifiers used by the transport so a single node (e.g.
 * a main controller) can address several peers in turn over one CAN bus. The
 * flow-control identifiers are left unchanged; they may be shared across peers
 * as long as only one peer is addressed at a time.
 *
 * @p rx_id becomes the identifier the transport receives SMP data on (must
 * match the peer's TX data identifier) and @p tx_id the identifier it transmits
 * SMP data on (must match the peer's RX data identifier). The change to the RX
 * identifier requires the receive filter to be reinstalled, which the receive
 * thread performs within @kconfig{CONFIG_MCUMGR_TRANSPORT_ISOTP_RX_POLL_MS}.
 *
 * Do not call this while a transfer to the current peer is in progress; the
 * intended use is to switch peers between complete SMP request/response
 * exchanges.
 *
 * @note Requires @kconfig{CONFIG_MCUMGR_TRANSPORT_ISOTP_RUNTIME_PEER}.
 *
 * @param rx_id New RX (receive) data CAN identifier.
 * @param tx_id New TX (transmit) data CAN identifier.
 *
 * @retval 0 on success.
 * @retval -EINVAL if a data id collides with a flow-control identifier.
 * @retval -ETIMEDOUT if the receive thread did not rebind in time.
 */
int smp_isotp_set_peer(uint32_t rx_id, uint32_t tx_id);
#endif

/**
 * @}
 */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_MGMT_MCUMGR_TRANSPORT_SMP_ISOTP_H_ */
