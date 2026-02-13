/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Direct-BAR0 FSP RPC PRC knob implementation. See prc_knob.h.
 *
 * Port of (from gpu-admin-tools):
 *   - gpu/fsp_mctp.py            MCTP header bit layout
 *   - gpu/mnoc.py + fsp_mnoc_rpc FspMnocRpc mailbox transport
 *   - nvidia_gpu_tools.py        FspRpc.send_cmd + prc_knob_{read,write}
 *
 * Transport: MNOC port 0 (FSP mailbox at 0x8f1e00). EMEM is not supported.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "prc_knob.h"

/* MNOC mailbox layout (FSP MNOC base, GpuMnoc layout in gpu/mnoc.py). */
#define FSP_MNOC_BASE             0x8f1e00
#define MNOC_INFO_SEND_MBOX(p)    (FSP_MNOC_BASE + 0x104 + (p) * 12)
#define MNOC_RDATA_SEND_MBOX(p)   (MNOC_INFO_SEND_MBOX(p) + 4)
#define MNOC_INFO_RECV_MBOX(p)    (FSP_MNOC_BASE + 0x184 + (p) * 12)
#define MNOC_WDATA_RECV_MBOX(p)   (MNOC_INFO_RECV_MBOX(p) + 4)
#define MNOC_MBOX_RECV_READY      (1u << 24)
#define MNOC_MBOX_MSG_READY       (1u << 24)
#define MNOC_MBOX_HAS_CREDITS     (1u << 26)
#define MNOC_MBOX_ERROR           (1u << 25)
#define MNOC_MBOX_NEW_MESSAGE     (1u << 20)
#define MNOC_MBOX_SIZE_MASK       0xfffff

/* PRC / FSP RPC framing constants. */
#define NVDM_TYPE_PRC             0x13
#define NVDM_TYPE_RESPONSE        0x15
#define PRC_SUBMSG_KNOB_READ      0xc
#define PRC_SUBMSG_KNOB_WRITE     0xd

/* PRC RPC error codes (subset, from gpu/error.py:FspRpcError). */
#define FSP_RPC_ERR_INVALID_KNOB  0x1e3

/* MNOC port and max packet size (matches fsp_mnoc_rpc.py for port 0). */
#define MNOC_PORT_PRC             0
#define MNOC_MAX_PACKET_BYTES     4160

/* BAR0 mapping size. FSP registers all live below ~0x900000; map 16 MiB. */
#define BAR0_MAP_SIZE             (16u * 1024u * 1024u)

#define DEFAULT_TIMEOUT_SEC       5.0
#define POLL_INTERVAL_US          1000

struct prc_knob_ctx {
	int bar0_fd;
	volatile uint32_t *bar0;
	size_t bar0_size;

	int port;
	uint32_t max_packet_bytes;

	/* MCTP packet sequence (wraps mod 4). */
	uint8_t seq;

	/*
	 * Saved /sys/bus/pci/devices/<bdf>/power/control value to restore on
	 * close. NULL if we didn't touch it. Mirrors gpu-admin-tools'
	 * NvidiaDevice.__init__ + atexit(restore_power).
	 */
	char *bdf;
	char *prev_power_control;
};

/* ---- BAR0 access ------------------------------------------------------- */

static uint32_t mmio_read(struct prc_knob_ctx *ctx, uint32_t offset)
{
	return ctx->bar0[offset / 4];
}

static void mmio_write(struct prc_knob_ctx *ctx, uint32_t offset, uint32_t value)
{
	ctx->bar0[offset / 4] = value;
}

/* ---- Time helpers ------------------------------------------------------ */

static double now_seconds(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void sleep_us(unsigned us)
{
	struct timespec ts = { 0, (long)us * 1000 };
	nanosleep(&ts, NULL);
}

/* ---- MCTP framing -----------------------------------------------------
 *
 * MctpHeader (one 32-bit dword, little-endian bit layout from LSB):
 *   [3:0]   version (=0)
 *   [7:4]   rsvd
 *   [15:8]  deid
 *   [23:16] seid
 *   [26:24] tag
 *   [27]    to
 *   [29:28] seq
 *   [30]    eom
 *   [31]    som
 *
 * MctpMessageHeader (one dword):
 *   [6:0]   type (=0x7e)
 *   [7]     ic
 *   [23:8]  vendor_id (=0x10de)
 *   [31:24] nvdm_type
 */

static uint32_t mctp_header_pack(uint8_t seid, uint8_t seq, uint8_t som, uint8_t eom)
{
	uint32_t v = 0;
	v |= ((uint32_t)seid & 0xff) << 16;
	v |= ((uint32_t)(seq & 0x3)) << 28;
	v |= ((uint32_t)(eom & 0x1)) << 30;
	v |= ((uint32_t)(som & 0x1)) << 31;
	return v;
}

static uint32_t mctp_msg_header_pack(uint8_t nvdm_type)
{
	uint32_t v = 0;
	v |= 0x7e & 0x7f;             /* type = vendor-defined PCI */
	v |= ((uint32_t)0x10de) << 8; /* NVIDIA vendor ID */
	v |= ((uint32_t)nvdm_type) << 24;
	return v;
}

static uint8_t mctp_msg_header_nvdm_type(uint32_t v)
{
	return (v >> 24) & 0xff;
}

/* ---- MNOC transport ---------------------------------------------------- */

static int mnoc_poll_bit(struct prc_knob_ctx *ctx, uint32_t reg, uint32_t bit, double timeout)
{
	double start = now_seconds();
	while ((mmio_read(ctx, reg) & bit) == 0) {
		if (now_seconds() - start > timeout) {
			fprintf(stderr, "MNOC: timed out polling reg %#x bit %#x\n", reg, bit);
			return -ETIMEDOUT;
		}
		sleep_us(POLL_INTERVAL_US);
	}
	return 0;
}

static int mnoc_send(struct prc_knob_ctx *ctx, const uint32_t *data, size_t ndwords)
{
	uint32_t info;
	size_t sent_bytes = 0;
	size_t i;
	int rc;

	rc = mnoc_poll_bit(ctx, MNOC_INFO_RECV_MBOX(ctx->port),
			   MNOC_MBOX_RECV_READY, DEFAULT_TIMEOUT_SEC);
	if (rc)
		return rc;

	info = (uint32_t)(ndwords * 4) | MNOC_MBOX_NEW_MESSAGE;
	mmio_write(ctx, MNOC_INFO_RECV_MBOX(ctx->port), info);

	for (i = 0; i < ndwords; i++) {
		if (sent_bytes % 64 == 0) {
			rc = mnoc_poll_bit(ctx, MNOC_INFO_RECV_MBOX(ctx->port),
					   MNOC_MBOX_HAS_CREDITS, 1.0);
			if (rc)
				return rc;
		}
		mmio_write(ctx, MNOC_WDATA_RECV_MBOX(ctx->port), data[i]);
		sent_bytes += 4;
	}

	info = mmio_read(ctx, MNOC_INFO_RECV_MBOX(ctx->port));
	if (info & MNOC_MBOX_ERROR) {
		fprintf(stderr, "MNOC: receive-mbox error %#x\n", info);
		return -EIO;
	}
	return 0;
}

static int mnoc_receive(struct prc_knob_ctx *ctx, uint32_t *out, size_t out_cap,
			size_t *out_ndwords, double timeout)
{
	uint32_t info, msg_size, sent;
	size_t ndwords, i;
	int rc;

	rc = mnoc_poll_bit(ctx, MNOC_INFO_SEND_MBOX(ctx->port),
			   MNOC_MBOX_MSG_READY, timeout);
	if (rc)
		return rc;

	info = mmio_read(ctx, MNOC_INFO_SEND_MBOX(ctx->port));
	msg_size = info & MNOC_MBOX_SIZE_MASK;
	/* Round up: FSP can report a byte count that isn't a multiple of 4
	 * (e.g. 22 bytes for a 6-dword body), matching Python mnoc.py's
	 * `while received < msg_size: read; received += 4` loop. */
	ndwords = (msg_size + 3) / 4;
	if (getenv("PRC_KNOB_DEBUG"))
		fprintf(stderr, "MNOC: info_send_mbox=%#x msg_size=%u ndwords=%zu\n",
			info, msg_size, ndwords);
	if (ndwords > out_cap) {
		fprintf(stderr, "MNOC: response %zu dwords exceeds buffer %zu\n",
			ndwords, out_cap);
		return -EMSGSIZE;
	}

	for (i = 0, sent = 0; sent < msg_size; sent += 4, i++)
		out[i] = mmio_read(ctx, MNOC_RDATA_SEND_MBOX(ctx->port));

	info = mmio_read(ctx, MNOC_INFO_SEND_MBOX(ctx->port));
	if (info & MNOC_MBOX_ERROR) {
		fprintf(stderr, "MNOC: send-mbox error %#x\n", info);
		return -EIO;
	}

	*out_ndwords = ndwords;
	return 0;
}

/* ---- Diagnostic name helpers ------------------------------------------- */

static const char *prc_knob_name(uint16_t id)
{
	switch (id) {
	case PRC_KNOB_ID_VGPU: return "VGPU";
	default:               return NULL;
	}
}

static const char *fsp_status_name(uint32_t status)
{
	switch (status) {
	case FSP_RPC_ERR_INVALID_KNOB: return "invalid knob";
	default:                       return NULL;
	}
}

/* ---- FSP RPC send_cmd -------------------------------------------------- */

#define FSP_MAX_DWORDS 1056   /* >= 4160 / 4 (largest MNOC packet) */

static int fsp_send_cmd(struct prc_knob_ctx *ctx, uint8_t nvdm_type,
			const uint32_t *payload, size_t payload_dwords,
			uint32_t *resp, size_t resp_cap, size_t *resp_dwords)
{
	uint32_t pkt[FSP_MAX_DWORDS];
	const size_t max_pkt_dwords = ctx->max_packet_bytes / 4;
	const uint32_t mctp_dwords = 1;  /* one dword each */
	uint32_t msg_hdr;
	size_t total_dwords;
	size_t first_pkt_dwords;
	size_t remaining_start;
	size_t i;
	int rc;
	uint8_t som = 1, eom = 1;

	ctx->seq = 0;

	total_dwords = payload_dwords + mctp_dwords + mctp_dwords;
	if (total_dwords > max_pkt_dwords)
		eom = 0;

	msg_hdr = mctp_msg_header_pack(nvdm_type);

	/* First packet: [mctp_hdr, msg_hdr, payload...]. */
	first_pkt_dwords = total_dwords;
	if (first_pkt_dwords > max_pkt_dwords)
		first_pkt_dwords = max_pkt_dwords;

	if (first_pkt_dwords > FSP_MAX_DWORDS)
		return -EMSGSIZE;

	pkt[0] = mctp_header_pack(0, ctx->seq, som, eom);
	pkt[1] = msg_hdr;
	for (i = 0; i < first_pkt_dwords - 2; i++)
		pkt[2 + i] = payload[i];

	rc = mnoc_send(ctx, pkt, first_pkt_dwords);
	if (rc)
		return rc;

	remaining_start = first_pkt_dwords - 2;
	while (remaining_start < payload_dwords) {
		size_t chunk;
		size_t left = payload_dwords - remaining_start;

		ctx->seq = (ctx->seq + 1) & 0x3;
		som = 0;
		eom = (left + mctp_dwords <= max_pkt_dwords) ? 1 : 0;

		chunk = left;
		if (chunk + mctp_dwords > max_pkt_dwords)
			chunk = max_pkt_dwords - mctp_dwords;

		pkt[0] = mctp_header_pack(0, ctx->seq, som, eom);
		for (i = 0; i < chunk; i++)
			pkt[1 + i] = payload[remaining_start + i];

		rc = mnoc_send(ctx, pkt, chunk + mctp_dwords);
		if (rc)
			return rc;

		remaining_start += chunk;
	}

	/* Receive: response is always single-packet for the small PRC ops here. */
	{
		uint32_t resp_buf[FSP_MAX_DWORDS];
		size_t resp_n = 0;
		uint8_t resp_nvdm;
		uint32_t status;

		rc = mnoc_receive(ctx, resp_buf, FSP_MAX_DWORDS, &resp_n,
				  DEFAULT_TIMEOUT_SEC);
		if (rc)
			return rc;

		if (getenv("PRC_KNOB_DEBUG")) {
			size_t k;
			fprintf(stderr, "FSP RPC: response %zu dwords:", resp_n);
			for (k = 0; k < resp_n && k < 12; k++)
				fprintf(stderr, " %08x", resp_buf[k]);
			fprintf(stderr, "\n");
		}

		if (resp_n < 5) {
			fprintf(stderr, "FSP RPC: response too short (%zu dwords)\n", resp_n);
			return -EPROTO;
		}

		resp_nvdm = mctp_msg_header_nvdm_type(resp_buf[1]);
		if (resp_nvdm != NVDM_TYPE_RESPONSE) {
			fprintf(stderr, "FSP RPC: bad response nvdm_type %#x\n", resp_nvdm);
			return -EPROTO;
		}
		if (resp_buf[3] != nvdm_type) {
			fprintf(stderr, "FSP RPC: response echoes type %#x, expected %#x\n",
				resp_buf[3], nvdm_type);
			return -EPROTO;
		}
		status = resp_buf[4];
		if (status != 0) {
			const char *name = fsp_status_name(status);
			if (status == FSP_RPC_ERR_INVALID_KNOB)
				return 1;
			if (name)
				fprintf(stderr, "FSP RPC: status %#x (%s)\n",
					status, name);
			else
				fprintf(stderr, "FSP RPC: status %#x\n", status);
			return -EIO;
		}

		/* Body starts at index 5 in the response (matches Python mdata[5:]). */
		if (resp_n - 5 > resp_cap)
			return -EMSGSIZE;
		for (i = 0; i < resp_n - 5; i++)
			resp[i] = resp_buf[5 + i];
		*resp_dwords = resp_n - 5;
	}
	return 0;
}

/* ---- PRC knob read / write -------------------------------------------- */

int prc_knob_read(struct prc_knob_ctx *ctx, uint16_t knob_id, uint16_t *out_value)
{
	uint32_t payload[1];
	uint32_t resp[2];
	size_t resp_n = 0;
	int rc;

	/* sub-msg 0xc, count=2 in byte 1, knob_id in upper 16 bits. */
	payload[0] = PRC_SUBMSG_KNOB_READ | (0x2u << 8) | ((uint32_t)knob_id << 16);

	rc = fsp_send_cmd(ctx, NVDM_TYPE_PRC, payload, 1, resp, 2, &resp_n);
	if (rc < 0) {
		const char *name = prc_knob_name(knob_id);
		fprintf(stderr, "PRC knob read failed: id %u%s%s%s\n",
			knob_id,
			name ? " (" : "", name ? name : "", name ? ")" : "");
		return rc;
	}
	if (rc)
		return rc;

	if (resp_n != 1) {
		fprintf(stderr, "PRC knob read: wrong response size %zu\n", resp_n);
		return -EPROTO;
	}

	*out_value = (uint16_t)(resp[0] & 0xffff);
	return 0;
}

int prc_knob_write(struct prc_knob_ctx *ctx, uint16_t knob_id, uint16_t value)
{
	uint32_t payload[2];
	uint32_t resp[1];
	size_t resp_n = 0;
	int rc;

	payload[0] = PRC_SUBMSG_KNOB_WRITE | (0x2u << 8) | ((uint32_t)knob_id << 16);
	payload[1] = (uint32_t)value;

	rc = fsp_send_cmd(ctx, NVDM_TYPE_PRC, payload, 2, resp, 1, &resp_n);
	if (rc < 0) {
		const char *name = prc_knob_name(knob_id);
		fprintf(stderr, "PRC knob write failed: id %u%s%s%s value %#x\n",
			knob_id,
			name ? " (" : "", name ? name : "", name ? ")" : "",
			value);
		return rc;
	}
	if (rc)
		return rc;

	if (resp_n != 0) {
		fprintf(stderr, "PRC knob write: unexpected response size %zu\n", resp_n);
		return -EPROTO;
	}
	return 0;
}

int prc_knob_check_and_write(struct prc_knob_ctx *ctx, uint16_t knob_id, uint16_t value)
{
	uint16_t cur;
	int rc = prc_knob_read(ctx, knob_id, &cur);
	if (rc)
		return rc;
	if (cur == value)
		return 0;
	return prc_knob_write(ctx, knob_id, value);
}

/* ---- Open / close ------------------------------------------------------ */

/*
 * Pin the device in D0 by writing "on" to /sys/.../power/control, so the
 * kernel doesn't runtime-suspend the GPU between our BAR0 accesses. Returns
 * a malloc'd string with the previous value (caller restores it on close),
 * or NULL if no change was made / the file isn't present.
 */
static char *power_control_pin_on(const char *bdf)
{
	char path[256];
	char buf[32];
	FILE *f;
	size_t n;
	char *prev;

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/power/control", bdf);
	f = fopen(path, "r");
	if (!f)
		return NULL;
	if (!fgets(buf, sizeof(buf), f)) {
		fclose(f);
		return NULL;
	}
	fclose(f);

	n = strlen(buf);
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
		buf[--n] = '\0';

	if (!strcmp(buf, "on"))
		return NULL;

	f = fopen(path, "w");
	if (!f) {
		fprintf(stderr, "open %s for write: %s\n", path, strerror(errno));
		return NULL;
	}
	if (fputs("on", f) < 0) {
		fprintf(stderr, "write %s: %s\n", path, strerror(errno));
		fclose(f);
		return NULL;
	}
	fclose(f);

	fprintf(stderr,
		"%s: forced power/control from '%s' to 'on' (will restore on exit)\n",
		bdf, buf);

	prev = strdup(buf);
	return prev;
}

static void power_control_restore(const char *bdf, const char *prev)
{
	char path[256];
	FILE *f;

	if (!prev)
		return;

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/power/control", bdf);
	f = fopen(path, "w");
	if (!f) {
		fprintf(stderr, "open %s for write: %s\n", path, strerror(errno));
		return;
	}
	if (fputs(prev, f) < 0)
		fprintf(stderr, "write %s: %s\n", path, strerror(errno));
	fclose(f);
	fprintf(stderr, "%s: restored power/control to '%s'\n", bdf, prev);
}

static int open_bar0(const char *bdf, int *out_fd, volatile uint32_t **out_map,
		     size_t *out_size)
{
	char path[256];
	int fd;
	struct stat st;
	size_t map_size = BAR0_MAP_SIZE;
	void *map;

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/resource0", bdf);
	fd = open(path, O_RDWR | O_SYNC);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		return -1;
	}

	if (fstat(fd, &st) == 0 && st.st_size > 0 && (size_t)st.st_size < map_size)
		map_size = (size_t)st.st_size;

	map = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		fprintf(stderr, "mmap %s: %s\n", path, strerror(errno));
		close(fd);
		return -1;
	}

	*out_fd = fd;
	*out_map = (volatile uint32_t *)map;
	*out_size = map_size;
	return 0;
}

struct prc_knob_ctx *prc_knob_open(const char *bdf)
{
	struct prc_knob_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return NULL;

	ctx->bdf = strdup(bdf);
	ctx->prev_power_control = power_control_pin_on(bdf);

	if (open_bar0(bdf, &ctx->bar0_fd, &ctx->bar0, &ctx->bar0_size) < 0) {
		power_control_restore(ctx->bdf, ctx->prev_power_control);
		free(ctx->prev_power_control);
		free(ctx->bdf);
		free(ctx);
		return NULL;
	}

	ctx->port = MNOC_PORT_PRC;
	ctx->max_packet_bytes = MNOC_MAX_PACKET_BYTES;

	return ctx;
}

void prc_knob_close(struct prc_knob_ctx *ctx)
{
	if (!ctx)
		return;
	if (ctx->bar0)
		munmap((void *)ctx->bar0, ctx->bar0_size);
	if (ctx->bar0_fd >= 0)
		close(ctx->bar0_fd);
	power_control_restore(ctx->bdf, ctx->prev_power_control);
	free(ctx->prev_power_control);
	free(ctx->bdf);
	free(ctx);
}
