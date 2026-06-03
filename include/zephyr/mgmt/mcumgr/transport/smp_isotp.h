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

/**
 * @}
 */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_MGMT_MCUMGR_TRANSPORT_SMP_ISOTP_H_ */
