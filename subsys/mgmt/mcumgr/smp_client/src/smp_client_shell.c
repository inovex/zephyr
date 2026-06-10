/*
 * Copyright (c) 2026, inovex GmbH
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/** @file
 * @brief Interactive "smpc" shell for the MCUmgr SMP client.
 *
 * Provides transport-agnostic commands that drive the SMP client (image, OS and
 * file management) so a controller can perform firmware updates on remote
 * nodes. Firmware images and files are sourced from a mounted file system.
 *
 * Command tree (transport-agnostic part):
 *   smpc echo <text>                    OS echo round-trip (connectivity check)
 *   smpc image list                     read the target's image state
 *   smpc image upload <file> [slot]     upload a firmware image from a file
 *   smpc image test <hash>              mark an image for test (revert on reboot)
 *   smpc image confirm [hash]           confirm an image (or the running one)
 *   smpc image erase <slot>             erase an image slot
 *   smpc file upload <local> <remote>   copy a local file to the target
 *   smpc file download <remote> <local> copy a file from the target
 *   smpc upgrade <file> [test|confirm]  upload -> mark -> reset, in one step
 *   smpc reset                          reboot the target
 *
 * Transport-specific commands are contributed by a backend that adds
 * subcommands to the "smpc" set and supplies the SMP transport type via
 * smp_client_shell_transport_type() (e.g. smp_isotp_shell.c registers
 * "smpc isotp" for ISO-TP/CAN peer retargeting).
 *
 * The image and OS operations use Zephyr's img_mgmt/os_mgmt client libraries.
 * Zephyr has no fs_mgmt client, so the file commands implement the FS group
 * (group 8, command 0) directly using the SMP client buffer/send primitives.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_string_conv.h>
#include <zephyr/fs/fs.h>
#include <zephyr/net_buf.h>

#include <zcbor_common.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>

#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt_defines.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/mgmt/mcumgr/smp/smp_client.h>
#include <zephyr/mgmt/mcumgr/transport/smp.h>
#include <zephyr/mgmt/mcumgr/grp/img_mgmt/img_mgmt.h>
#include <zephyr/mgmt/mcumgr/grp/img_mgmt/img_mgmt_client.h>
#include <zephyr/mgmt/mcumgr/grp/os_mgmt/os_mgmt_client.h>
#include <zephyr/mgmt/mcumgr/grp/fs_mgmt/fs_mgmt.h>

/* Maximum number of image entries parsed from an "image list" response. */
#define IMG_LIST_MAX 4
/* RAM staging buffer for reading the local file during transfers. */
#define STAGE_BUF_SZ 1024
/* Payload bytes per FS-group frame (kept well under the transport MTU). */
#define FS_CHUNK_SZ  256
/* Maximum remote/local path length accepted. */
#define PATH_MAX_LEN 128
/* SHA-256 image hash length in bytes / hex characters. */
#define HASH_LEN     IMG_MGMT_DATA_SHA_LEN

#define KEY_IS(k, lit) ((k).len == (sizeof(lit) - 1) && memcmp((k).value, (lit), (k).len) == 0)

static struct smp_client_object smp_client;
static struct img_mgmt_client img_client;
static struct os_mgmt_client os_client;
static struct mcumgr_image_data img_list_buf[IMG_LIST_MAX];
static bool clients_ready;

/* Shared by the file commands (shell runs single-threaded, so one buffer is
 * sufficient and the FS-client state is additionally guarded by fs_mutex).
 */
static uint8_t stage_buf[STAGE_BUF_SZ];

/* Default SMP transport type. A transport backend (e.g. smp_isotp_shell.c)
 * overrides this; the weak default makes "no backend selected" fail cleanly.
 */
__weak int smp_client_shell_transport_type(void)
{
	return -1;
}

/* ---- helpers ------------------------------------------------------------- */

static int ensure_clients(const struct shell *sh)
{
	int rc;
	int transport;

	if (clients_ready) {
		return 0;
	}

	transport = smp_client_shell_transport_type();
	if (transport < 0) {
		shell_error(sh, "no SMP client shell transport backend enabled");
		return -ENOTSUP;
	}

	rc = smp_client_object_init(&smp_client, transport);
	if (rc != MGMT_ERR_EOK) {
		shell_error(sh, "SMP client init failed: %d (is the transport up?)", rc);
		return rc;
	}

	img_mgmt_client_init(&img_client, &smp_client, IMG_LIST_MAX, img_list_buf);
	os_mgmt_client_init(&os_client, &smp_client);
	clients_ready = true;

	return 0;
}

static int parse_u32(const struct shell *sh, const char *s, uint32_t *out)
{
	int err = 0;
	unsigned long v = shell_strtoul(s, 0, &err);

	if (err != 0) {
		shell_error(sh, "invalid number: %s", s);
		return -EINVAL;
	}
	*out = (uint32_t)v;
	return 0;
}

static int parse_hash(const struct shell *sh, const char *s, uint8_t out[HASH_LEN])
{
	if (strlen(s) != HASH_LEN * 2) {
		shell_error(sh, "hash must be %d hex characters", HASH_LEN * 2);
		return -EINVAL;
	}

	for (int i = 0; i < HASH_LEN; i++) {
		char byte[3] = { s[2 * i], s[2 * i + 1], '\0' };
		char *end;
		long v = strtol(byte, &end, 16);

		if (*end != '\0') {
			shell_error(sh, "invalid hex digit in hash");
			return -EINVAL;
		}
		out[i] = (uint8_t)v;
	}
	return 0;
}

static void print_hash(const struct shell *sh, const char *prefix, const uint8_t *hash)
{
	char hex[HASH_LEN * 2 + 1];

	for (int i = 0; i < HASH_LEN; i++) {
		(void)snprintf(&hex[2 * i], 3, "%02x", hash[i]);
	}
	shell_print(sh, "%s%s", prefix, hex);
}

/* ---- FS management client (hand-rolled; no fs_mgmt client in Zephyr) ------ */

static struct {
	/* Download copy target (NULL for upload). */
	uint8_t *dst;
	size_t dst_cap;
	size_t data_len;  /* bytes copied into dst */
	/* Decoded response fields. */
	size_t off;       /* server-reported offset */
	bool have_off;
	size_t len;       /* total file size (present at off 0) */
	bool have_len;
	int status;       /* MGMT_ERR_* for the operation */
} fs_op;

static K_SEM_DEFINE(fs_sem, 0, 1);
static K_MUTEX_DEFINE(fs_mutex);

static int fs_res_fn(struct net_buf *nb, void *user_data)
{
	zcbor_state_t zsd[CONFIG_MCUMGR_SMP_CBOR_MAX_DECODING_LEVELS + 2];
	struct zcbor_string key;
	int rc_field = 0;

	if (nb == NULL) {
		fs_op.status = MGMT_ERR_ETIMEOUT;
		goto end;
	}

	zcbor_new_decode_state(zsd, ARRAY_SIZE(zsd), nb->data, nb->len, 1, NULL, 0);

	if (!zcbor_map_start_decode(zsd)) {
		fs_op.status = MGMT_ERR_ECORRUPT;
		goto end;
	}

	while (zcbor_tstr_decode(zsd, &key)) {
		bool ok;

		if (KEY_IS(key, "off")) {
			uint64_t v;

			ok = zcbor_uint64_decode(zsd, &v);
			if (ok) {
				fs_op.off = (size_t)v;
				fs_op.have_off = true;
			}
		} else if (KEY_IS(key, "len")) {
			uint64_t v;

			ok = zcbor_uint64_decode(zsd, &v);
			if (ok) {
				fs_op.len = (size_t)v;
				fs_op.have_len = true;
			}
		} else if (KEY_IS(key, "rc")) {
			int32_t v;

			ok = zcbor_int32_decode(zsd, &v);
			if (ok) {
				rc_field = v;
			}
		} else if (KEY_IS(key, "data")) {
			struct zcbor_string data;

			ok = zcbor_bstr_decode(zsd, &data);
			if (ok && fs_op.dst != NULL) {
				size_t n = MIN(data.len, fs_op.dst_cap);

				memcpy(fs_op.dst, data.value, n);
				fs_op.data_len = n;
			}
		} else {
			ok = zcbor_any_skip(zsd, NULL);
		}

		if (!ok) {
			fs_op.status = MGMT_ERR_ECORRUPT;
			goto end;
		}
	}

	if (rc_field != 0) {
		fs_op.status = rc_field;
	} else if (!fs_op.have_off) {
		fs_op.status = MGMT_ERR_ECORRUPT;
	} else {
		fs_op.status = MGMT_ERR_EOK;
	}

end:
	k_sem_give(user_data);
	return fs_op.status;
}

/* Upload one chunk to a remote file; returns MGMT_ERR_* and the new offset. */
static int fs_upload_chunk(const char *name, size_t off, const uint8_t *data, size_t len,
			   size_t total, size_t *new_off)
{
	struct net_buf *nb;
	zcbor_state_t zse[CONFIG_MCUMGR_SMP_CBOR_MAX_DECODING_LEVELS + 2];
	uint32_t map_count = (off == 0) ? 4 : 3;
	bool ok;
	int rc;

	nb = smp_client_buf_allocation(&smp_client, MGMT_GROUP_ID_FS, FS_MGMT_ID_FILE,
				       MGMT_OP_WRITE, SMP_MCUMGR_VERSION_1);
	if (nb == NULL) {
		return MGMT_ERR_ENOMEM;
	}

	zcbor_new_encode_state(zse, ARRAY_SIZE(zse), nb->data + nb->len, net_buf_tailroom(nb), 0);

	ok = zcbor_map_start_encode(zse, map_count) && zcbor_tstr_put_lit(zse, "off") &&
	     zcbor_uint64_put(zse, off) && zcbor_tstr_put_lit(zse, "data") &&
	     zcbor_bstr_encode_ptr(zse, data, len) && zcbor_tstr_put_lit(zse, "name") &&
	     zcbor_tstr_put_term(zse, name, PATH_MAX_LEN);
	if (ok && off == 0) {
		ok = zcbor_tstr_put_lit(zse, "len") && zcbor_uint64_put(zse, total);
	}
	if (ok) {
		ok = zcbor_map_end_encode(zse, map_count);
	}
	if (!ok) {
		smp_packet_free(nb);
		return MGMT_ERR_ENOMEM;
	}

	nb->len = zse->payload - nb->data;

	memset(&fs_op, 0, sizeof(fs_op));
	fs_op.status = MGMT_ERR_EINVAL;
	k_sem_reset(&fs_sem);

	rc = smp_client_send_cmd(&smp_client, nb, fs_res_fn, &fs_sem,
				 CONFIG_SMP_CMD_DEFAULT_LIFE_TIME);
	if (rc != 0) {
		smp_packet_free(nb);
		return rc;
	}

	k_sem_take(&fs_sem, K_FOREVER);
	if (fs_op.status == MGMT_ERR_EOK) {
		*new_off = fs_op.off;
	}
	return fs_op.status;
}

/* Download one chunk from a remote file. */
static int fs_download_chunk(const char *name, size_t off, uint8_t *dst, size_t cap,
			     size_t *chunk_len, size_t *total, bool *have_total)
{
	struct net_buf *nb;
	zcbor_state_t zse[CONFIG_MCUMGR_SMP_CBOR_MAX_DECODING_LEVELS + 2];
	bool ok;
	int rc;

	nb = smp_client_buf_allocation(&smp_client, MGMT_GROUP_ID_FS, FS_MGMT_ID_FILE,
				       MGMT_OP_READ, SMP_MCUMGR_VERSION_1);
	if (nb == NULL) {
		return MGMT_ERR_ENOMEM;
	}

	zcbor_new_encode_state(zse, ARRAY_SIZE(zse), nb->data + nb->len, net_buf_tailroom(nb), 0);

	ok = zcbor_map_start_encode(zse, 2) && zcbor_tstr_put_lit(zse, "off") &&
	     zcbor_uint64_put(zse, off) && zcbor_tstr_put_lit(zse, "name") &&
	     zcbor_tstr_put_term(zse, name, PATH_MAX_LEN) && zcbor_map_end_encode(zse, 2);
	if (!ok) {
		smp_packet_free(nb);
		return MGMT_ERR_ENOMEM;
	}

	nb->len = zse->payload - nb->data;

	memset(&fs_op, 0, sizeof(fs_op));
	fs_op.status = MGMT_ERR_EINVAL;
	fs_op.dst = dst;
	fs_op.dst_cap = cap;
	k_sem_reset(&fs_sem);

	rc = smp_client_send_cmd(&smp_client, nb, fs_res_fn, &fs_sem,
				 CONFIG_SMP_CMD_DEFAULT_LIFE_TIME);
	if (rc != 0) {
		smp_packet_free(nb);
		return rc;
	}

	k_sem_take(&fs_sem, K_FOREVER);
	if (fs_op.status == MGMT_ERR_EOK) {
		*chunk_len = fs_op.data_len;
		if (fs_op.have_len) {
			*total = fs_op.len;
			*have_total = true;
		}
	}
	return fs_op.status;
}

/* ---- command handlers ---------------------------------------------------- */

static int cmd_echo(const struct shell *sh, size_t argc, char **argv)
{
	int rc;

	if (ensure_clients(sh)) {
		return -ENODEV;
	}

	rc = os_mgmt_client_echo(&os_client, argv[1], strlen(argv[1]));
	if (rc != MGMT_ERR_EOK) {
		shell_error(sh, "echo failed: %d", rc);
		return rc;
	}

	shell_print(sh, "echo OK");
	return 0;
}

static int cmd_reset(const struct shell *sh, size_t argc, char **argv)
{
	int rc;

	if (ensure_clients(sh)) {
		return -ENODEV;
	}

	rc = os_mgmt_client_reset(&os_client);
	if (rc != MGMT_ERR_EOK) {
		shell_error(sh, "reset failed: %d", rc);
		return rc;
	}

	shell_print(sh, "reset request sent");
	return 0;
}

static int cmd_image_list(const struct shell *sh, size_t argc, char **argv)
{
	struct mcumgr_image_state state;
	int rc;

	if (ensure_clients(sh)) {
		return -ENODEV;
	}

	rc = img_mgmt_client_state_read(&img_client, &state);
	if (rc != MGMT_ERR_EOK) {
		shell_error(sh, "image list failed: %d", rc);
		return rc;
	}

	shell_print(sh, "images: %d", state.image_list_length);
	for (int i = 0; i < state.image_list_length; i++) {
		struct mcumgr_image_data *img = &state.image_list[i];

		shell_print(sh, " [%d] image=%u slot=%u version=%s%s%s%s%s%s", i, img->img_num,
			    img->slot_num, img->version,
			    img->flags.active ? " active" : "",
			    img->flags.confirmed ? " confirmed" : "",
			    img->flags.pending ? " pending" : "",
			    img->flags.permanent ? " permanent" : "",
			    img->flags.bootable ? " bootable" : "");
		print_hash(sh, "     hash=", img->hash);
	}
	return 0;
}

/* Upload an image file to the given slot. Returns an MGMT_ERR/errno code. */
static int image_upload_file(const struct shell *sh, const char *path, uint32_t slot)
{
	struct fs_dirent ent;
	struct fs_file_t file;
	struct mcumgr_image_upload res;
	size_t total, sent = 0;
	int rc;

	rc = fs_stat(path, &ent);
	if (rc != 0) {
		shell_error(sh, "cannot stat %s: %d", path, rc);
		return rc;
	}
	total = ent.size;
	if (total == 0) {
		shell_error(sh, "%s is empty", path);
		return -EINVAL;
	}

	fs_file_t_init(&file);
	rc = fs_open(&file, path, FS_O_READ);
	if (rc != 0) {
		shell_error(sh, "cannot open %s: %d", path, rc);
		return rc;
	}

	rc = img_mgmt_client_upload_init(&img_client, total, slot, NULL);
	if (rc != MGMT_ERR_EOK) {
		shell_error(sh, "upload init failed: %d", rc);
		goto out;
	}

	shell_print(sh, "uploading %s (%zu bytes) to slot %u", path, total, slot);

	while (sent < total) {
		ssize_t n = fs_read(&file, stage_buf, sizeof(stage_buf));
		size_t before = sent;

		if (n <= 0) {
			shell_error(sh, "read error at offset %zu: %zd", sent, n);
			rc = (n < 0) ? (int)n : -EIO;
			goto out;
		}

		rc = img_mgmt_client_upload(&img_client, stage_buf, (size_t)n, &res);
		if (rc != MGMT_ERR_EOK) {
			shell_error(sh, "upload failed at offset %zu: %d", sent, rc);
			goto out;
		}

		sent = res.image_upload_offset;
		if (sent != before + (size_t)n) {
			/* Device acknowledged a different offset (resume); realign. */
			if (fs_seek(&file, sent, FS_SEEK_SET) != 0) {
				shell_error(sh, "seek to %zu failed", sent);
				rc = -EIO;
				goto out;
			}
		}

		shell_print(sh, "  %zu/%zu (%zu%%)", sent, total, (sent * 100) / total);
	}

	shell_print(sh, "upload complete");
	rc = MGMT_ERR_EOK;
out:
	fs_close(&file);
	return rc;
}

static int cmd_image_upload(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t slot = 0;

	if (ensure_clients(sh)) {
		return -ENODEV;
	}
	if (argc == 3 && parse_u32(sh, argv[2], &slot)) {
		return -EINVAL;
	}

	return image_upload_file(sh, argv[1], slot);
}

static int cmd_image_test(const struct shell *sh, size_t argc, char **argv)
{
	struct mcumgr_image_state state;
	uint8_t hash[HASH_LEN];
	int rc;

	if (ensure_clients(sh)) {
		return -ENODEV;
	}
	if (parse_hash(sh, argv[1], hash)) {
		return -EINVAL;
	}

	rc = img_mgmt_client_state_write(&img_client, (char *)hash, false, &state);
	if (rc != MGMT_ERR_EOK) {
		shell_error(sh, "test failed: %d", rc);
		return rc;
	}

	shell_print(sh, "image marked for test");
	return 0;
}

static int cmd_image_confirm(const struct shell *sh, size_t argc, char **argv)
{
	struct mcumgr_image_state state;
	uint8_t hash[HASH_LEN];
	char *hash_arg = NULL;
	int rc;

	if (ensure_clients(sh)) {
		return -ENODEV;
	}
	if (argc == 2) {
		if (parse_hash(sh, argv[1], hash)) {
			return -EINVAL;
		}
		hash_arg = (char *)hash;
	}

	rc = img_mgmt_client_state_write(&img_client, hash_arg, true, &state);
	if (rc != MGMT_ERR_EOK) {
		shell_error(sh, "confirm failed: %d", rc);
		return rc;
	}

	shell_print(sh, "image confirmed");
	return 0;
}

static int cmd_image_erase(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t slot;
	int rc;

	if (ensure_clients(sh)) {
		return -ENODEV;
	}
	if (parse_u32(sh, argv[1], &slot)) {
		return -EINVAL;
	}

	rc = img_mgmt_client_erase(&img_client, slot);
	if (rc != MGMT_ERR_EOK) {
		shell_error(sh, "erase failed: %d", rc);
		return rc;
	}

	shell_print(sh, "slot %u erased", slot);
	return 0;
}

static int cmd_file_upload(const struct shell *sh, size_t argc, char **argv)
{
	const char *local = argv[1];
	const char *remote = argv[2];
	struct fs_dirent ent;
	struct fs_file_t file;
	size_t total, off = 0;
	int rc;

	if (ensure_clients(sh)) {
		return -ENODEV;
	}
	if (strlen(remote) >= PATH_MAX_LEN) {
		shell_error(sh, "remote path too long (max %d)", PATH_MAX_LEN - 1);
		return -EINVAL;
	}

	rc = fs_stat(local, &ent);
	if (rc != 0) {
		shell_error(sh, "cannot stat %s: %d", local, rc);
		return rc;
	}
	total = ent.size;
	if (total == 0) {
		shell_error(sh, "%s is empty", local);
		return -EINVAL;
	}

	fs_file_t_init(&file);
	rc = fs_open(&file, local, FS_O_READ);
	if (rc != 0) {
		shell_error(sh, "cannot open %s: %d", local, rc);
		return rc;
	}

	shell_print(sh, "uploading %s -> %s (%zu bytes)", local, remote, total);
	k_mutex_lock(&fs_mutex, K_FOREVER);

	while (off < total) {
		ssize_t n = fs_read(&file, stage_buf, MIN(FS_CHUNK_SZ, sizeof(stage_buf)));
		size_t new_off;

		if (n <= 0) {
			rc = (n < 0) ? (int)n : -EIO;
			shell_error(sh, "read error at %zu: %d", off, rc);
			break;
		}

		rc = fs_upload_chunk(remote, off, stage_buf, (size_t)n, total, &new_off);
		if (rc != MGMT_ERR_EOK) {
			shell_error(sh, "upload failed at %zu: %d", off, rc);
			break;
		}

		off = new_off;
		if (fs_seek(&file, off, FS_SEEK_SET) != 0) {
			shell_error(sh, "seek to %zu failed", off);
			rc = -EIO;
			break;
		}
		shell_print(sh, "  %zu/%zu", off, total);
	}

	k_mutex_unlock(&fs_mutex);
	fs_close(&file);

	if (rc == MGMT_ERR_EOK) {
		shell_print(sh, "file upload complete");
	}
	return rc;
}

static int cmd_file_download(const struct shell *sh, size_t argc, char **argv)
{
	const char *remote = argv[1];
	const char *local = argv[2];
	struct fs_file_t file;
	size_t off = 0, total = 0;
	bool have_total = false;
	int rc;

	if (ensure_clients(sh)) {
		return -ENODEV;
	}
	if (strlen(remote) >= PATH_MAX_LEN) {
		shell_error(sh, "remote path too long (max %d)", PATH_MAX_LEN - 1);
		return -EINVAL;
	}

	fs_file_t_init(&file);
	rc = fs_open(&file, local, FS_O_CREATE | FS_O_WRITE);
	if (rc != 0) {
		shell_error(sh, "cannot open %s: %d", local, rc);
		return rc;
	}
	(void)fs_truncate(&file, 0);

	shell_print(sh, "downloading %s -> %s", remote, local);
	k_mutex_lock(&fs_mutex, K_FOREVER);

	do {
		size_t chunk_len = 0;

		rc = fs_download_chunk(remote, off, stage_buf, MIN(FS_CHUNK_SZ, sizeof(stage_buf)),
				       &chunk_len, &total, &have_total);
		if (rc != MGMT_ERR_EOK) {
			shell_error(sh, "download failed at %zu: %d", off, rc);
			break;
		}
		if (chunk_len == 0) {
			break;
		}

		if (fs_write(&file, stage_buf, chunk_len) != (ssize_t)chunk_len) {
			shell_error(sh, "write error at %zu", off);
			rc = -EIO;
			break;
		}
		off += chunk_len;
		shell_print(sh, "  %zu%s", off, have_total ? "" : " (size unknown)");
	} while (!have_total || off < total);

	k_mutex_unlock(&fs_mutex);
	fs_close(&file);

	if (rc == MGMT_ERR_EOK) {
		shell_print(sh, "file download complete (%zu bytes)", off);
	} else {
		/* Don't leave the freshly-created (possibly empty/partial) file
		 * behind on failure.
		 */
		(void)fs_unlink(local);
	}
	return rc;
}

static int cmd_upgrade(const struct shell *sh, size_t argc, char **argv)
{
	struct mcumgr_image_state state;
	const char *path = argv[1];
	bool confirm = false;
	const uint8_t *hash = NULL;
	int rc;

	if (ensure_clients(sh)) {
		return -ENODEV;
	}
	if (argc == 3) {
		if (strcmp(argv[2], "confirm") == 0) {
			confirm = true;
		} else if (strcmp(argv[2], "test") != 0) {
			shell_error(sh, "mode must be 'test' or 'confirm'");
			return -EINVAL;
		}
	}

	rc = image_upload_file(sh, path, 0);
	if (rc != MGMT_ERR_EOK) {
		return rc;
	}

	/* Find the freshly uploaded (non-active) image and use its device-computed
	 * hash to arm the test/confirm.
	 */
	rc = img_mgmt_client_state_read(&img_client, &state);
	if (rc != MGMT_ERR_EOK) {
		shell_error(sh, "state read after upload failed: %d", rc);
		return rc;
	}
	for (int i = 0; i < state.image_list_length; i++) {
		if (!state.image_list[i].flags.active) {
			hash = state.image_list[i].hash;
			break;
		}
	}
	if (hash == NULL) {
		shell_error(sh, "no inactive image found to mark");
		return -ENOENT;
	}

	rc = img_mgmt_client_state_write(&img_client, (char *)hash, confirm, &state);
	if (rc != MGMT_ERR_EOK) {
		shell_error(sh, "marking image failed: %d", rc);
		return rc;
	}
	shell_print(sh, "image marked for %s; resetting target", confirm ? "confirm" : "test");

	rc = os_mgmt_client_reset(&os_client);
	if (rc != MGMT_ERR_EOK) {
		shell_error(sh, "reset failed: %d", rc);
		return rc;
	}

	shell_print(sh, "upgrade complete");
	return 0;
}

/* ---- command registration ------------------------------------------------ */

SHELL_STATIC_SUBCMD_SET_CREATE(sub_image,
	SHELL_CMD_ARG(list, NULL, "List images on the target", cmd_image_list, 1, 0),
	SHELL_CMD_ARG(upload, NULL, "Upload firmware: upload <file> [slot]", cmd_image_upload, 2, 1),
	SHELL_CMD_ARG(test, NULL, "Mark image for test: test <hash>", cmd_image_test, 2, 0),
	SHELL_CMD_ARG(confirm, NULL, "Confirm image: confirm [hash]", cmd_image_confirm, 1, 1),
	SHELL_CMD_ARG(erase, NULL, "Erase a slot: erase <slot>", cmd_image_erase, 2, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_file,
	SHELL_CMD_ARG(upload, NULL, "Copy local->target: upload <local> <remote>", cmd_file_upload,
		      3, 0),
	SHELL_CMD_ARG(download, NULL, "Copy target->local: download <remote> <local>",
		      cmd_file_download, 3, 0),
	SHELL_SUBCMD_SET_END
);

/* Root "smpc" subcommand set. Transport backends add their own subcommands to
 * this set from their own translation units via SHELL_SUBCMD_ADD((smpc), ...).
 */
SHELL_SUBCMD_SET_CREATE(smp_client_shell_cmds, (smpc));

SHELL_SUBCMD_ADD((smpc), echo, NULL, "OS echo round-trip: echo <text>", cmd_echo, 2, 0);
SHELL_SUBCMD_ADD((smpc), image, &sub_image, "Image management commands", NULL, 1, 0);
SHELL_SUBCMD_ADD((smpc), file, &sub_file, "File management commands", NULL, 1, 0);
SHELL_SUBCMD_ADD((smpc), upgrade, NULL, "Upload+mark+reset: upgrade <file> [test|confirm]",
		 cmd_upgrade, 2, 1);
SHELL_SUBCMD_ADD((smpc), reset, NULL, "Reset the target", cmd_reset, 1, 0);

SHELL_CMD_REGISTER(smpc, &smp_client_shell_cmds, "MCUmgr SMP client control", NULL);
