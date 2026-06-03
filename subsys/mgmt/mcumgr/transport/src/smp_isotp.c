/*
 * Copyright (c) 2026, inovex GmbH
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/** @file
 * @brief ISO-TP (CAN, ISO 15765-2) transport for the MCUmgr SMP protocol.
 *
 * ISO-TP performs its own segmentation, reassembly and flow control, so a
 * single ISO-TP message carries one complete SMP packet. The MCUmgr reassembly
 * layer is therefore not used. An SMP packet is limited by the net_buf size and
 * by the 4095-byte ISO-TP message limit.
 *
 * The transport uses ISO 15765-2 normal addressing: one CAN identifier per
 * direction. Each peer sends its data and its flow control on the same
 * identifier; the ISO-TP layer dispatches the frames by PCI type.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/util.h>
#include <zephyr/net_buf.h>
#include <zephyr/drivers/can.h>
#include <zephyr/canbus/isotp.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/mgmt/mcumgr/transport/smp.h>
#include <zephyr/mgmt/mcumgr/mgmt/handlers.h>

#include <mgmt/mcumgr/transport/smp_internal.h>

LOG_MODULE_REGISTER(smp_isotp, CONFIG_MCUMGR_TRANSPORT_ISOTP_LOG_LEVEL);

/* CAN ID flags shared by RX and TX addresses. */
#if defined(CONFIG_MCUMGR_TRANSPORT_ISOTP_EXTENDED_ID)
#define SMP_ISOTP_ID_FLAGS ISOTP_MSG_IDE
#define SMP_ISOTP_ID_MASK  CAN_EXT_ID_MASK
#else
#define SMP_ISOTP_ID_FLAGS 0
#define SMP_ISOTP_ID_MASK  CAN_STD_ID_MASK
#endif

/* True if the identifier fits the configured format (11 or 29 bits). */
#define SMP_ISOTP_ID_VALID(id) (((id) & ~SMP_ISOTP_ID_MASK) == 0U)

#if defined(CONFIG_MCUMGR_TRANSPORT_ISOTP_CAN_FD)
#define SMP_ISOTP_FD_FLAGS (ISOTP_MSG_FDF | ISOTP_MSG_BRS)
#define SMP_ISOTP_TX_DL    CONFIG_MCUMGR_TRANSPORT_ISOTP_TX_DL
#else
#define SMP_ISOTP_FD_FLAGS 0
#define SMP_ISOTP_TX_DL    0
#endif

static const struct device *const can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));

BUILD_ASSERT(CONFIG_MCUMGR_TRANSPORT_ISOTP_RX_ID != CONFIG_MCUMGR_TRANSPORT_ISOTP_TX_ID,
	     "ISO-TP RX and TX CAN identifiers must differ");
BUILD_ASSERT(SMP_ISOTP_ID_VALID(CONFIG_MCUMGR_TRANSPORT_ISOTP_RX_ID) &&
		     SMP_ISOTP_ID_VALID(CONFIG_MCUMGR_TRANSPORT_ISOTP_TX_ID),
	     "ISO-TP CAN identifiers do not fit the configured identifier format");

static const struct isotp_msg_id smp_isotp_rx_addr = {
	.ext_id = CONFIG_MCUMGR_TRANSPORT_ISOTP_RX_ID,
	.flags = SMP_ISOTP_ID_FLAGS | SMP_ISOTP_FD_FLAGS,
};

static const struct isotp_msg_id smp_isotp_tx_addr = {
	.ext_id = CONFIG_MCUMGR_TRANSPORT_ISOTP_TX_ID,
	.dl = SMP_ISOTP_TX_DL,
	.flags = SMP_ISOTP_ID_FLAGS | SMP_ISOTP_FD_FLAGS,
};

/* The flow-control identifiers are the data identifiers (normal addressing):
 * the peer paces the transmission of this node on the identifier that this
 * node receives on, and the opposite. The ISO-TP layer forwards flow control
 * that arrives on a bound receive address to the send context, so no second
 * filter is installed for the same identifier.
 */
static const struct isotp_msg_id smp_isotp_fc_tx_addr = {
	.ext_id = CONFIG_MCUMGR_TRANSPORT_ISOTP_TX_ID,
	.flags = SMP_ISOTP_ID_FLAGS | SMP_ISOTP_FD_FLAGS,
};

static const struct isotp_msg_id smp_isotp_fc_rx_addr = {
	.ext_id = CONFIG_MCUMGR_TRANSPORT_ISOTP_RX_ID,
	.flags = SMP_ISOTP_ID_FLAGS | SMP_ISOTP_FD_FLAGS,
};

static const struct isotp_fc_opts smp_isotp_fc_opts = {
	.bs = CONFIG_MCUMGR_TRANSPORT_ISOTP_FC_BS,
	.stmin = CONFIG_MCUMGR_TRANSPORT_ISOTP_FC_STMIN,
};

static struct isotp_recv_ctx smp_isotp_recv_ctx;
static struct isotp_send_ctx smp_isotp_send_ctx;

/* Serialises transmissions and carries the completion result from the ISO-TP
 * work queue back to the SMP work queue thread running the output function.
 */
static struct k_sem smp_isotp_tx_sem;
static int smp_isotp_tx_result;

/* smp_isotp_tx_busy is true from isotp_send() until the completion callback.
 * If the output function stops waiting before that, it hands the buffer over
 * in smp_isotp_tx_orphan, and the callback frees it. Both are guarded by
 * smp_isotp_tx_lock.
 */
static struct k_spinlock smp_isotp_tx_lock;
static bool smp_isotp_tx_busy;
static struct net_buf *smp_isotp_tx_orphan;

static int smp_isotp_tx_pkt(struct net_buf *nb);

struct smp_transport smp_isotp_transport = {
	.functions.output = smp_isotp_tx_pkt,
};

#ifdef CONFIG_SMP_CLIENT
static struct smp_client_transport_entry smp_isotp_client_transport = {
	.smpt = &smp_isotp_transport,
	.smpt_type = SMP_ISOTP_TRANSPORT,
};
#endif

static struct k_thread smp_isotp_thread;
static K_KERNEL_STACK_DEFINE(smp_isotp_stack, CONFIG_MCUMGR_TRANSPORT_ISOTP_THREAD_STACK_SIZE);

static void smp_isotp_tx_complete(int error_nr, void *arg)
{
	struct net_buf *orphan;
	k_spinlock_key_t key;

	ARG_UNUSED(arg);

	key = k_spin_lock(&smp_isotp_tx_lock);
	smp_isotp_tx_result = error_nr;
	smp_isotp_tx_busy = false;
	orphan = smp_isotp_tx_orphan;
	smp_isotp_tx_orphan = NULL;
	k_spin_unlock(&smp_isotp_tx_lock, key);

	if (orphan != NULL) {
		/* The output function timed out and returned; the buffer is ours. */
		smp_packet_free(orphan);
		return;
	}

	k_sem_give(&smp_isotp_tx_sem);
}

static int smp_isotp_tx_pkt(struct net_buf *nb)
{
	k_spinlock_key_t key;
	bool busy;
	int rc;

	if (nb == NULL) {
		return MGMT_ERR_EINVAL;
	}

	/* The net_buf data must remain valid until the ISO-TP send completes, so
	 * the completion callback signals a semaphore that this thread waits on.
	 * A transfer that timed out earlier can still be in progress: the ISO-TP
	 * layer has no cancel operation, and the send context is not free yet.
	 */
	key = k_spin_lock(&smp_isotp_tx_lock);
	busy = smp_isotp_tx_busy;
	smp_isotp_tx_busy = true;
	k_spin_unlock(&smp_isotp_tx_lock, key);

	if (busy) {
		LOG_ERR("ISO-TP send still in progress");
		smp_packet_free(nb);
		return MGMT_ERR_EBUSY;
	}

	k_sem_reset(&smp_isotp_tx_sem);
	smp_isotp_tx_result = ISOTP_N_OK;

	rc = isotp_send(&smp_isotp_send_ctx, can_dev, nb->data, nb->len,
			&smp_isotp_tx_addr, &smp_isotp_fc_rx_addr,
			smp_isotp_tx_complete, NULL);
	if (rc != ISOTP_N_OK) {
		LOG_ERR("Failed to start ISO-TP send: %d", rc);
		key = k_spin_lock(&smp_isotp_tx_lock);
		smp_isotp_tx_busy = false;
		k_spin_unlock(&smp_isotp_tx_lock, key);
		smp_packet_free(nb);
		return MGMT_ERR_EUNKNOWN;
	}

	if (k_sem_take(&smp_isotp_tx_sem,
		       K_MSEC(CONFIG_MCUMGR_TRANSPORT_ISOTP_TX_TIMEOUT_MS)) != 0) {
		key = k_spin_lock(&smp_isotp_tx_lock);
		busy = smp_isotp_tx_busy;
		if (busy) {
			/* The ISO-TP layer still reads the buffer. It ends the
			 * transfer with its own timers, and the completion
			 * callback then frees the buffer.
			 */
			smp_isotp_tx_orphan = nb;
		}
		k_spin_unlock(&smp_isotp_tx_lock, key);

		if (busy) {
			LOG_ERR("ISO-TP send timed out");
			return MGMT_ERR_ETIMEOUT;
		}
		/* The transfer ended after the timeout, before the lock. */
	}

	if (smp_isotp_tx_result != ISOTP_N_OK) {
		LOG_ERR("ISO-TP send failed: %d", smp_isotp_tx_result);
		smp_packet_free(nb);
		return MGMT_ERR_EUNKNOWN;
	}

	smp_packet_free(nb);

	return MGMT_ERR_EOK;
}

/* Receive one complete ISO-TP message into a freshly allocated SMP net_buf.
 * Returns the buffer on success (caller owns it), or NULL on error/overflow.
 */
static struct net_buf *smp_isotp_recv_msg(void)
{
	struct net_buf *nb;
	struct net_buf *frag;
	int rem_len;

	nb = smp_packet_alloc();
	if (nb == NULL) {
		LOG_ERR("Failed to allocate SMP packet");
		return NULL;
	}

	do {
		rem_len = isotp_recv_net(&smp_isotp_recv_ctx, &frag, K_FOREVER);
		if (rem_len < 0) {
			LOG_ERR("ISO-TP receive error: %d", rem_len);
			smp_packet_free(nb);
			return NULL;
		}

		while (frag != NULL) {
			if (frag->len > net_buf_tailroom(nb)) {
				LOG_ERR("ISO-TP message exceeds buffer size, dropping");
				net_buf_unref(frag);
				smp_packet_free(nb);
				return NULL;
			}

			net_buf_add_mem(nb, frag->data, frag->len);
			frag = net_buf_frag_del(NULL, frag);
		}
	} while (rem_len > 0);

	return nb;
}

static void smp_isotp_rx_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	int rc;
	struct net_buf *nb;

	rc = isotp_bind(&smp_isotp_recv_ctx, can_dev, &smp_isotp_rx_addr, &smp_isotp_fc_tx_addr,
			&smp_isotp_fc_opts, K_FOREVER);
	if (rc != ISOTP_N_OK) {
		LOG_ERR("Failed to bind ISO-TP RX address: %d", rc);
		return;
	}

	while (1) {
		nb = smp_isotp_recv_msg();
		if (nb != NULL) {
			smp_rx_req(&smp_isotp_transport, nb);
		}
	}
}

static void smp_isotp_start(void)
{
	int rc;

	if (!device_is_ready(can_dev)) {
		LOG_ERR("CAN device %s not ready", can_dev->name);
		return;
	}

#ifdef CONFIG_MCUMGR_TRANSPORT_ISOTP_AUTO_START
	can_mode_t mode = CAN_MODE_NORMAL;

#ifdef CONFIG_MCUMGR_TRANSPORT_ISOTP_CAN_FD
	mode |= CAN_MODE_FD;
#endif

	rc = can_set_mode(can_dev, mode);
	if (rc != 0) {
		LOG_ERR("Failed to set CAN mode: %d", rc);
		return;
	}

	rc = can_start(can_dev);
	if (rc != 0 && rc != -EALREADY) {
		LOG_ERR("Failed to start CAN device: %d", rc);
		return;
	}
#endif /* CONFIG_MCUMGR_TRANSPORT_ISOTP_AUTO_START */

	k_sem_init(&smp_isotp_tx_sem, 0, 1);

	rc = smp_transport_init(&smp_isotp_transport);
	if (rc != 0) {
		LOG_ERR("Failed to init ISO-TP MCUmgr SMP transport: %d", rc);
		return;
	}

#ifdef CONFIG_SMP_CLIENT
	smp_client_transport_register(&smp_isotp_client_transport);
#endif

	k_thread_create(&smp_isotp_thread, smp_isotp_stack,
			K_KERNEL_STACK_SIZEOF(smp_isotp_stack), smp_isotp_rx_thread,
			NULL, NULL, NULL, CONFIG_MCUMGR_TRANSPORT_ISOTP_THREAD_PRIORITY, 0,
			K_NO_WAIT);
	k_thread_name_set(&smp_isotp_thread, "smp_isotp_rx");
}

MCUMGR_HANDLER_DEFINE(smp_isotp, smp_isotp_start);
