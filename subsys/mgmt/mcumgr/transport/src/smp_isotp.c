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
 * layer is therefore not used and the transport MTU is the net_buf size, capped
 * at the 4095-byte ISO-TP message limit.
 *
 * The transport supports up to two independent channels on the same CAN bus,
 * each a separate SMP endpoint with its own four CAN identifiers and receive
 * thread:
 *  - Channel 0 (always present): registered as an SMP server and, when
 *    CONFIG_SMP_CLIENT is set, as the SMP client transport. With
 *    CONFIG_MCUMGR_TRANSPORT_ISOTP_RUNTIME_PEER it can be retargeted at runtime
 *    (smp_isotp_set_peer()) so a controller can address several nodes in turn.
 *  - Channel 1 (CONFIG_MCUMGR_TRANSPORT_ISOTP_SECOND_CHANNEL): a fixed,
 *    server-only endpoint. This lets a node that uses channel 0 as a retargeting
 *    client still be reached at a stable address for its own updates, because a
 *    single channel cannot be both a fixed server endpoint and a client whose
 *    identifiers move per peer.
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

#include <zephyr/mgmt/mcumgr/transport/smp_isotp.h>

#include <mgmt/mcumgr/transport/smp_internal.h>

LOG_MODULE_REGISTER(smp_isotp, CONFIG_MCUMGR_TRANSPORT_ISOTP_LOG_LEVEL);

/* ISO-TP single message can carry at most 4095 bytes (12-bit length field). */
#define SMP_ISOTP_MAX_MSG_SIZE 4095U

/* CAN ID flags shared by RX and TX addresses. */
#if defined(CONFIG_MCUMGR_TRANSPORT_ISOTP_EXTENDED_ID)
#define SMP_ISOTP_ID_FLAGS ISOTP_MSG_IDE
#else
#define SMP_ISOTP_ID_FLAGS 0
#endif

#if defined(CONFIG_MCUMGR_TRANSPORT_ISOTP_CAN_FD)
#define SMP_ISOTP_FD_FLAGS (ISOTP_MSG_FDF | ISOTP_MSG_BRS)
#define SMP_ISOTP_TX_DL    CONFIG_MCUMGR_TRANSPORT_ISOTP_TX_DL
#else
#define SMP_ISOTP_FD_FLAGS 0
#define SMP_ISOTP_TX_DL    0
#endif

#define SMP_ISOTP_FLAGS (SMP_ISOTP_ID_FLAGS | SMP_ISOTP_FD_FLAGS)

static const struct device *const can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));

/* Zephyr's ISO-TP API uses separate receive (isotp_bind) and send (isotp_send)
 * contexts, each of which installs an exact CAN RX filter: the receive context
 * filters on the incoming data identifier, the send context filters on the
 * identifier it expects flow-control on. If flow control shared a data
 * identifier, both contexts would filter the same CAN ID and every frame would
 * be delivered to both state machines, aborting transfers with
 * ISOTP_N_UNEXP_PDU. The flow-control identifiers are therefore configured
 * separately from, and must be distinct from, the data identifiers. When two
 * peers talk to each other the data and flow-control identifiers are mirrored
 * (each node's TX id is the other's RX id, and likewise for flow control).
 */
BUILD_ASSERT(CONFIG_MCUMGR_TRANSPORT_ISOTP_RX_ID != CONFIG_MCUMGR_TRANSPORT_ISOTP_RX_FC_ID,
	     "ISO-TP RX data and RX flow-control CAN identifiers must differ");
BUILD_ASSERT(CONFIG_MCUMGR_TRANSPORT_ISOTP_TX_ID != CONFIG_MCUMGR_TRANSPORT_ISOTP_TX_FC_ID,
	     "ISO-TP TX data and TX flow-control CAN identifiers must differ");

#if defined(CONFIG_MCUMGR_TRANSPORT_ISOTP_SECOND_CHANNEL)
BUILD_ASSERT(CONFIG_MCUMGR_TRANSPORT_ISOTP_RX_ID2 != CONFIG_MCUMGR_TRANSPORT_ISOTP_RX_FC_ID2,
	     "ISO-TP channel 2 RX data and RX flow-control CAN identifiers must differ");
BUILD_ASSERT(CONFIG_MCUMGR_TRANSPORT_ISOTP_TX_ID2 != CONFIG_MCUMGR_TRANSPORT_ISOTP_TX_FC_ID2,
	     "ISO-TP channel 2 TX data and TX flow-control CAN identifiers must differ");
#endif

/* Shared flow-control behaviour for every channel and reception. */
static const struct isotp_fc_opts smp_isotp_fc_opts = {
	.bs = CONFIG_MCUMGR_TRANSPORT_ISOTP_FC_BS,
	.stmin = CONFIG_MCUMGR_TRANSPORT_ISOTP_FC_STMIN,
};

/* One ISO-TP SMP endpoint: its CAN identifiers, ISO-TP contexts, receive thread
 * and the smp_transport it is registered as.
 */
struct smp_isotp_channel {
	struct smp_transport *transport;
	struct isotp_recv_ctx recv_ctx;
	struct isotp_send_ctx send_ctx;
	/* Data identifiers are mutable so a retargetable channel can change peer
	 * at runtime; flow-control identifiers stay fixed for the channel.
	 */
	struct isotp_msg_id rx_addr;
	struct isotp_msg_id tx_addr;
	struct isotp_msg_id fc_tx_addr; /* FC this node sends while receiving */
	struct isotp_msg_id fc_rx_addr; /* FC this node receives while sending */
	struct k_sem tx_sem;
	int tx_result;
	struct k_thread thread;
	k_thread_stack_t *stack;
	size_t stack_size;
	const char *name;
	bool is_client;    /* also register as the SMP client transport */
	bool retargetable; /* allow smp_isotp_set_peer() */
#if defined(CONFIG_MCUMGR_TRANSPORT_ISOTP_RUNTIME_PEER)
	/* Retarget handshake (see smp_isotp_set_peer()); all guarded by peer_mutex.
	 * Only the receive thread touches the ISO-TP context (bind/unbind/recv);
	 * set_peer just updates the data addresses and bumps req_seq, then polls
	 * for the thread to publish a matching done_seq. A monotonic sequence
	 * (rather than a flag + semaphore) makes a rebind that is superseded by a
	 * newer request, or that fails and is retried, impossible to lose or
	 * acknowledge out of order.
	 */
	struct k_mutex peer_mutex;
	uint32_t req_seq;
	uint32_t done_seq;
#endif
};

#if defined(CONFIG_MCUMGR_TRANSPORT_ISOTP_RUNTIME_PEER)
/* Number of poll intervals smp_isotp_set_peer() waits for the rebind, sized to
 * comfortably cover a stuck in-flight reception (ISO-TP N_Cr timeout) plus a
 * couple of poll cycles.
 */
#define SMP_ISOTP_REBIND_WAIT_TICKS \
	MAX(5, DIV_ROUND_UP(5000, CONFIG_MCUMGR_TRANSPORT_ISOTP_RX_POLL_MS))
#endif

static uint16_t smp_isotp_get_mtu(const struct net_buf *nb);
static int smp_isotp_out_ch0(struct net_buf *nb);
#if defined(CONFIG_MCUMGR_TRANSPORT_ISOTP_SECOND_CHANNEL)
static int smp_isotp_out_ch1(struct net_buf *nb);
#endif

/* Channel 0's transport is exported (see smp_isotp.h) so applications/tests can
 * reference it; channel 1's is private.
 */
struct smp_transport smp_isotp_transport = {
	.functions.output = smp_isotp_out_ch0,
	.functions.get_mtu = smp_isotp_get_mtu,
};

#if defined(CONFIG_MCUMGR_TRANSPORT_ISOTP_SECOND_CHANNEL)
static struct smp_transport smp_isotp_transport_2 = {
	.functions.output = smp_isotp_out_ch1,
	.functions.get_mtu = smp_isotp_get_mtu,
};
#endif

#ifdef CONFIG_SMP_CLIENT
/* Only channel 0 acts as the SMP client transport, so the lookup by type stays
 * unambiguous.
 */
static struct smp_client_transport_entry smp_isotp_client_transport = {
	.smpt = &smp_isotp_transport,
	.smpt_type = SMP_ISOTP_TRANSPORT,
};
#endif

static K_KERNEL_STACK_DEFINE(smp_isotp_stack0, CONFIG_MCUMGR_TRANSPORT_ISOTP_THREAD_STACK_SIZE);
#if defined(CONFIG_MCUMGR_TRANSPORT_ISOTP_SECOND_CHANNEL)
static K_KERNEL_STACK_DEFINE(smp_isotp_stack1, CONFIG_MCUMGR_TRANSPORT_ISOTP_THREAD_STACK_SIZE);
#endif

static struct smp_isotp_channel smp_isotp_channels[] = {
	{
		.transport = &smp_isotp_transport,
		.rx_addr = {.ext_id = CONFIG_MCUMGR_TRANSPORT_ISOTP_RX_ID,
			    .flags = SMP_ISOTP_FLAGS},
		.tx_addr = {.ext_id = CONFIG_MCUMGR_TRANSPORT_ISOTP_TX_ID, .dl = SMP_ISOTP_TX_DL,
			    .flags = SMP_ISOTP_FLAGS},
		.fc_tx_addr = {.ext_id = CONFIG_MCUMGR_TRANSPORT_ISOTP_TX_FC_ID,
			       .flags = SMP_ISOTP_FLAGS},
		.fc_rx_addr = {.ext_id = CONFIG_MCUMGR_TRANSPORT_ISOTP_RX_FC_ID,
			       .flags = SMP_ISOTP_FLAGS},
		.stack = smp_isotp_stack0,
		.stack_size = K_KERNEL_STACK_SIZEOF(smp_isotp_stack0),
		.name = "smp_isotp_rx",
		.is_client = IS_ENABLED(CONFIG_SMP_CLIENT),
		.retargetable = IS_ENABLED(CONFIG_MCUMGR_TRANSPORT_ISOTP_RUNTIME_PEER),
	},
#if defined(CONFIG_MCUMGR_TRANSPORT_ISOTP_SECOND_CHANNEL)
	{
		.transport = &smp_isotp_transport_2,
		.rx_addr = {.ext_id = CONFIG_MCUMGR_TRANSPORT_ISOTP_RX_ID2,
			    .flags = SMP_ISOTP_FLAGS},
		.tx_addr = {.ext_id = CONFIG_MCUMGR_TRANSPORT_ISOTP_TX_ID2, .dl = SMP_ISOTP_TX_DL,
			    .flags = SMP_ISOTP_FLAGS},
		.fc_tx_addr = {.ext_id = CONFIG_MCUMGR_TRANSPORT_ISOTP_TX_FC_ID2,
			       .flags = SMP_ISOTP_FLAGS},
		.fc_rx_addr = {.ext_id = CONFIG_MCUMGR_TRANSPORT_ISOTP_RX_FC_ID2,
			       .flags = SMP_ISOTP_FLAGS},
		.stack = smp_isotp_stack1,
		.stack_size = K_KERNEL_STACK_SIZEOF(smp_isotp_stack1),
		.name = "smp_isotp_rx2",
		.is_client = false,
		.retargetable = false,
	},
#endif
};

static void smp_isotp_tx_complete(int error_nr, void *arg)
{
	struct smp_isotp_channel *ch = arg;

	ch->tx_result = error_nr;
	k_sem_give(&ch->tx_sem);
}

static uint16_t smp_isotp_get_mtu(const struct net_buf *nb)
{
	ARG_UNUSED(nb);

	return (uint16_t)MIN(CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE, SMP_ISOTP_MAX_MSG_SIZE);
}

static int smp_isotp_tx(struct smp_isotp_channel *ch, struct net_buf *nb)
{
	struct isotp_msg_id tx_addr;
	int rc;

	if (nb == NULL) {
		return MGMT_ERR_EINVAL;
	}

	/* The net_buf data must remain valid until the ISO-TP send completes, so
	 * the completion callback signals a semaphore that this thread waits on.
	 */
	k_sem_reset(&ch->tx_sem);
	ch->tx_result = ISOTP_N_OK;

#if defined(CONFIG_MCUMGR_TRANSPORT_ISOTP_RUNTIME_PEER)
	if (ch->retargetable) {
		/* smp_isotp_set_peer() may update tx_addr from another thread; take a
		 * stable copy under the mutex so this send cannot capture a torn
		 * identifier. The in-flight transfer keeps the snapshotted peer; a
		 * retarget applies to the next send.
		 */
		k_mutex_lock(&ch->peer_mutex, K_FOREVER);
		tx_addr = ch->tx_addr;
		k_mutex_unlock(&ch->peer_mutex);
	} else {
		tx_addr = ch->tx_addr;
	}
#else
	tx_addr = ch->tx_addr;
#endif

	rc = isotp_send(&ch->send_ctx, can_dev, nb->data, nb->len, &tx_addr, &ch->fc_rx_addr,
			smp_isotp_tx_complete, ch);
	if (rc != ISOTP_N_OK) {
		LOG_ERR("Failed to start ISO-TP send: %d", rc);
		smp_packet_free(nb);
		return MGMT_ERR_EUNKNOWN;
	}

	if (k_sem_take(&ch->tx_sem, K_MSEC(CONFIG_MCUMGR_TRANSPORT_ISOTP_TX_TIMEOUT_MS)) != 0) {
		LOG_ERR("ISO-TP send timed out");
		smp_packet_free(nb);
		return MGMT_ERR_ETIMEOUT;
	}

	if (ch->tx_result != ISOTP_N_OK) {
		LOG_ERR("ISO-TP send failed: %d", ch->tx_result);
		smp_packet_free(nb);
		return MGMT_ERR_EUNKNOWN;
	}

	smp_packet_free(nb);

	return MGMT_ERR_EOK;
}

static int smp_isotp_out_ch0(struct net_buf *nb)
{
	return smp_isotp_tx(&smp_isotp_channels[0], nb);
}

#if defined(CONFIG_MCUMGR_TRANSPORT_ISOTP_SECOND_CHANNEL)
static int smp_isotp_out_ch1(struct net_buf *nb)
{
	return smp_isotp_tx(&smp_isotp_channels[1], nb);
}
#endif

/* Discard the remaining fragments of a partially consumed ISO-TP message so
 * the next receive starts on a clean message boundary. Blocking on
 * isotp_recv_net() yields the CPU, so this does not busy-spin.
 */
static void smp_isotp_drain_msg(struct smp_isotp_channel *ch, int rem_len)
{
	struct net_buf *frag;

	while (rem_len > 0) {
		rem_len = isotp_recv_net(&ch->recv_ctx, &frag, K_FOREVER);
		if (rem_len < 0) {
			return;
		}
		net_buf_unref(frag);
	}
}

/* Receive one complete ISO-TP message into a freshly allocated SMP net_buf.
 * Returns the buffer on success (caller owns it), or NULL on error/overflow.
 *
 * The first fragment is awaited for at most @p first_timeout; if that elapses
 * with no message in progress, *timed_out is set and NULL is returned so the
 * caller can act on housekeeping (e.g. a pending peer retarget). Once the first
 * fragment has arrived, the remaining fragments are awaited indefinitely so a
 * message in flight is never truncated by the poll interval.
 *
 * The SMP packet is allocated only after data actually starts arriving, so an
 * idle receive thread (there is one per channel) does not tie up a packet
 * buffer from the small transport pool while it waits.
 */
static struct net_buf *smp_isotp_recv_msg(struct smp_isotp_channel *ch, k_timeout_t first_timeout,
					  bool *timed_out)
{
	struct net_buf *nb = NULL;
	struct net_buf *frag;
	int rem_len;
	bool first = true;

	*timed_out = false;

	do {
		rem_len = isotp_recv_net(&ch->recv_ctx, &frag, first ? first_timeout : K_FOREVER);
		if (rem_len < 0) {
			if (first && rem_len == ISOTP_RECV_TIMEOUT) {
				/* Idle poll tick, not an error. */
				*timed_out = true;
			} else {
				LOG_ERR("ISO-TP receive error: %d", rem_len);
			}
			if (nb != NULL) {
				smp_packet_free(nb);
			}
			return NULL;
		}
		first = false;

		if (nb == NULL) {
			nb = smp_packet_alloc();
			if (nb == NULL) {
				/* Pool momentarily empty: drop this message and drain its
				 * remaining fragments so the next receive starts on a clean
				 * message boundary. Returning to isotp_recv_net() yields the
				 * CPU, so the consumer can free buffers (no busy spin).
				 */
				LOG_ERR("Failed to allocate SMP packet, dropping message");
				net_buf_unref(frag);
				smp_isotp_drain_msg(ch, rem_len);
				return NULL;
			}
		}

		while (frag != NULL) {
			if (frag->len > net_buf_tailroom(nb)) {
				/* Too big for the SMP packet buffer: drop it, and drain it
				 * like above so the tail of this message is not mistaken
				 * for the start of the next one.
				 */
				LOG_ERR("ISO-TP message exceeds buffer size, dropping");
				net_buf_unref(frag);
				smp_packet_free(nb);
				smp_isotp_drain_msg(ch, rem_len);
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
	struct smp_isotp_channel *ch = p1;
	int rc;
	struct net_buf *nb;
	bool timed_out;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	rc = isotp_bind(&ch->recv_ctx, can_dev, &ch->rx_addr, &ch->fc_tx_addr, &smp_isotp_fc_opts,
			K_FOREVER);
	if (rc != ISOTP_N_OK) {
		LOG_ERR("Failed to bind ISO-TP RX address: %d", rc);
		return;
	}

	while (1) {
#if defined(CONFIG_MCUMGR_TRANSPORT_ISOTP_RUNTIME_PEER)
		if (ch->retargetable) {
			/* Service a pending retarget before blocking on RX again. Doing the
			 * unbind/rebind here (rather than in smp_isotp_set_peer()) keeps all
			 * receive-context access on this thread. The address snapshot is taken
			 * under the mutex; the (potentially slow) unbind/bind runs unlocked so
			 * the TX path and set_peer are never blocked on it.
			 */
			uint32_t want_seq;
			struct isotp_msg_id rx_addr;

			k_mutex_lock(&ch->peer_mutex, K_FOREVER);
			want_seq = ch->req_seq;
			rx_addr = ch->rx_addr;
			k_mutex_unlock(&ch->peer_mutex);

			if (want_seq != ch->done_seq) {
				isotp_unbind(&ch->recv_ctx);
				rc = isotp_bind(&ch->recv_ctx, can_dev, &rx_addr, &ch->fc_tx_addr,
						&smp_isotp_fc_opts, K_FOREVER);
				if (rc == ISOTP_N_OK) {
					/* Publish completion only on success, so a failed bind
					 * is retried on the next iteration and set_peer keeps
					 * waiting rather than seeing a false acknowledgment.
					 */
					k_mutex_lock(&ch->peer_mutex, K_FOREVER);
					ch->done_seq = want_seq;
					k_mutex_unlock(&ch->peer_mutex);
				} else {
					LOG_ERR("Failed to rebind ISO-TP RX address: %d", rc);
					k_sleep(K_MSEC(CONFIG_MCUMGR_TRANSPORT_ISOTP_RX_POLL_MS));
				}
				continue;
			}

			nb = smp_isotp_recv_msg(
				ch, K_MSEC(CONFIG_MCUMGR_TRANSPORT_ISOTP_RX_POLL_MS), &timed_out);
			if (timed_out) {
				continue;
			}
		} else {
			nb = smp_isotp_recv_msg(ch, K_FOREVER, &timed_out);
		}
#else
		nb = smp_isotp_recv_msg(ch, K_FOREVER, &timed_out);
#endif

		if (nb != NULL) {
			smp_rx_req(ch->transport, nb);
		}
	}
}

#if defined(CONFIG_MCUMGR_TRANSPORT_ISOTP_RUNTIME_PEER)
int smp_isotp_set_peer(uint32_t rx_id, uint32_t tx_id)
{
	struct smp_isotp_channel *ch = &smp_isotp_channels[0];
	uint32_t my_seq;

	if (!ch->retargetable) {
		return -ENOTSUP;
	}

	/* Flow control must stay on its own identifiers, distinct from both data
	 * identifiers in both directions (see file header), else the receive and
	 * send contexts would filter the same CAN id. Reject any such collision.
	 */
	if (rx_id == ch->fc_tx_addr.ext_id || rx_id == ch->fc_rx_addr.ext_id ||
	    tx_id == ch->fc_tx_addr.ext_id || tx_id == ch->fc_rx_addr.ext_id) {
		return -EINVAL;
	}

	k_mutex_lock(&ch->peer_mutex, K_FOREVER);
	ch->rx_addr.ext_id = rx_id;
	ch->tx_addr.ext_id = tx_id;
	my_seq = ++ch->req_seq;
	k_mutex_unlock(&ch->peer_mutex);

	/* The new TX id takes effect on the next isotp_send(); wait until the RX
	 * thread has installed the new RX filter, i.e. published a done sequence at
	 * least as new as ours. A later request superseding ours also satisfies the
	 * wait (the transport ends up bound to the most recent peer either way).
	 */
	for (int i = 0; i < SMP_ISOTP_REBIND_WAIT_TICKS; i++) {
		uint32_t done;

		k_sleep(K_MSEC(CONFIG_MCUMGR_TRANSPORT_ISOTP_RX_POLL_MS));

		k_mutex_lock(&ch->peer_mutex, K_FOREVER);
		done = ch->done_seq;
		k_mutex_unlock(&ch->peer_mutex);

		if ((int32_t)(done - my_seq) >= 0) {
			return 0;
		}
	}

	return -ETIMEDOUT;
}
#endif /* CONFIG_MCUMGR_TRANSPORT_ISOTP_RUNTIME_PEER */

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

	for (size_t i = 0; i < ARRAY_SIZE(smp_isotp_channels); i++) {
		struct smp_isotp_channel *ch = &smp_isotp_channels[i];

		k_sem_init(&ch->tx_sem, 0, 1);
#if defined(CONFIG_MCUMGR_TRANSPORT_ISOTP_RUNTIME_PEER)
		if (ch->retargetable) {
			k_mutex_init(&ch->peer_mutex);
		}
#endif

		rc = smp_transport_init(ch->transport);
		if (rc != 0) {
			LOG_ERR("Failed to init ISO-TP SMP transport (channel %zu): %d", i, rc);
			continue;
		}

#ifdef CONFIG_SMP_CLIENT
		if (ch->is_client) {
			smp_client_transport_register(&smp_isotp_client_transport);
		}
#endif

		k_thread_create(&ch->thread, ch->stack, ch->stack_size, smp_isotp_rx_thread, ch,
				NULL, NULL, CONFIG_MCUMGR_TRANSPORT_ISOTP_THREAD_PRIORITY, 0,
				K_NO_WAIT);
		k_thread_name_set(&ch->thread, ch->name);
	}
}

MCUMGR_HANDLER_DEFINE(smp_isotp, smp_isotp_start);
