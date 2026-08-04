/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Implements the `add-type` subcommand of vgpu-mgmt: loads a vGPU metadata
 * binary from disk, validates its identifier and CRC32, and issues the
 * FWCTL_CMD_NOVA_CORE_GMCAPI_ADD_VGPU_TYPE RPC to register the type with
 * the nova-core driver.
 */

#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include "fwctl.h"
#include "fwctl_common.h"

#include "metadata.h"
#include "nvrm.h"

#define GSP_BUILD_VERSION_LEN 128
#define GMCAPI_MAX_VGPU_TYPES 128

/* Header stored ahead of the opaque NVA081_CTRL_VGPU_INFO array in the
 * metadata RMCTRL section. */
struct metadata_add_vgpu_type_hdr {
	uint8_t discard_vgpu_types;
	uint8_t padding[3];
	uint32_t vgpu_info_count;
};

/* Prefix expected by gmcapiAddVgpuType().  One opaque vGPU info record is
 * appended to this header for each GMC round. */
struct gmcapi_add_vgpu_type_hdr {
	uint8_t gsp_build_version[GSP_BUILD_VERSION_LEN];
	struct metadata_add_vgpu_type_hdr params;
};

struct vgpu_type_blob_view {
	const struct vgpu_type_blob_hdr *hdr;
	const uint8_t *info;
	size_t info_size;
	uint32_t info_count;
};

_Static_assert(sizeof(struct metadata_add_vgpu_type_hdr) == 8,
	       "unexpected metadata add-vGPU-type header layout");
_Static_assert(sizeof(struct gmcapi_add_vgpu_type_hdr) == 136,
	       "unexpected GMC add-vGPU-type header layout");
_Static_assert(offsetof(struct metadata_hdr, data) == 232,
	       "unexpected metadata header layout");
_Static_assert(offsetof(struct metadata_blob_hdr, data) == 16,
	       "unexpected metadata blob header layout");
_Static_assert(offsetof(struct vgpu_type_blob_hdr, data) == 56,
	       "unexpected vGPU type blob header layout");

static void usage_add_type(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"  -d <path>     fwctl device path (default: auto-detect)\n"
		"  -f <path>     vGPU metadata binary file\n"
		"  -p <hex_id>   PCI device ID to match (e.g. 0x27b8)\n"
		"  -h            show this help\n",
		prog);
}

static int validate_metadata(const struct metadata_hdr *hdr, size_t file_size)
{
	char idr[sizeof(METADATA_IDR)] = {0};
	const uint8_t *p;
	uint32_t crc;

	memcpy(idr, &hdr->identifier, sizeof(hdr->identifier));

	if (memcmp(idr, METADATA_IDR, sizeof(hdr->identifier))) {
		fprintf(stderr, "invalid metadata identifier: %s (expected %s)\n",
			idr, METADATA_IDR);
		return -1;
	}

	/* CRC32 starts after identifier(8) + crc32(4) + padding(4) = offset 16 */
	p = (uint8_t *)hdr + 16;
	crc = crc32_le(0xffffffff, p, file_size - 16);

	if (crc != hdr->crc32) {
		fprintf(stderr, "CRC32 mismatch: file=0x%08x computed=0x%08x\n",
			hdr->crc32, crc);
		return -1;
	}

	if (!memchr(hdr->gsp_build_version, '\0',
		    sizeof(hdr->gsp_build_version))) {
		fprintf(stderr, "GSP build version is not NUL-terminated\n");
		return -1;
	}

	return 0;
}

static int send_vgpu_type(int fd, const uint8_t *info, size_t info_size,
			  const uint8_t *gsp_build_version, bool discard)
{
	struct fwctl_rpc rpc = {0};
	struct fwctl_rpc_nova_core *req_hdr;
	struct fwctl_rpc_nova_core resp_hdr;
	struct gmcapi_add_vgpu_type_hdr *payload;
	size_t payload_size;
	size_t in_size;
	size_t version_len;
	void *in_buf;
	int ret;

	if (info_size > UINT32_MAX - sizeof(*payload) - sizeof(*req_hdr)) {
		fprintf(stderr, "vGPU info record is too large: %zu bytes\n",
			info_size);
		return -EOVERFLOW;
	}
	payload_size = sizeof(*payload) + info_size;
	in_size = sizeof(*req_hdr) + payload_size;

	ret = posix_memalign(&in_buf, 16, in_size);
	if (ret) {
		fprintf(stderr, "failed to allocate %zu bytes for RPC in buffer\n",
			in_size);
		return -ENOMEM;
	}

	memset(in_buf, 0, in_size);

	req_hdr = in_buf;
	req_hdr->command_id = FWCTL_CMD_NOVA_CORE_GMCAPI_ADD_VGPU_TYPE;
	payload = (void *)((uint8_t *)in_buf + sizeof(*req_hdr));

	version_len = strnlen((const char *)gsp_build_version,
			      GSP_MAX_BUILD_VERSION_LENGTH);
	memcpy(payload->gsp_build_version, gsp_build_version, version_len);
	payload->params.discard_vgpu_types = discard;
	payload->params.vgpu_info_count = 1;
	memcpy((uint8_t *)payload + sizeof(*payload), info, info_size);

	memset(&resp_hdr, 0, sizeof(resp_hdr));

	rpc.size = sizeof(rpc);
	rpc.scope = FWCTL_RPC_CONFIGURATION;
	rpc.in_len = (uint32_t)in_size;
	rpc.out_len = sizeof(resp_hdr);
	rpc.in = (uint64_t)(uintptr_t)in_buf;
	rpc.out = (uint64_t)(uintptr_t)&resp_hdr;

	ret = ioctl(fd, FWCTL_RPC, &rpc);
	if (ret) {
		perror("ioctl(FWCTL_RPC) ADD_VGPU_TYPE");
		free(in_buf);
		return -1;
	}

	free(in_buf);
	return 0;
}

/* Bounds + minimum-size check for a single blob header at @bhdr. */
static int validate_blob_hdr(const struct metadata_blob_hdr *bhdr,
			     const uint8_t *end, uint64_t idx)
{
	const uint8_t *p = (const uint8_t *)bhdr;

	if (p > end || (size_t)(end - p) < sizeof(*bhdr)) {
		fprintf(stderr, "blob %lu extends past end of file\n", idx);
		return -1;
	}
	if (bhdr->size < sizeof(*bhdr)) {
		fprintf(stderr, "blob %lu size %lu smaller than header\n",
			idx, bhdr->size);
		return -1;
	}
	if (bhdr->size > (uint64_t)(end - p)) {
		fprintf(stderr, "blob %lu extends past end of file\n", idx);
		return -1;
	}
	return 0;
}

static int get_vgpu_type_blob_view(const struct metadata_blob_hdr *bhdr,
				   uint64_t idx,
				   struct vgpu_type_blob_view *view)
{
	const uint8_t *blob_end = (const uint8_t *)bhdr + bhdr->size;
	const struct vgpu_type_blob_hdr *vhdr;
	struct metadata_add_vgpu_type_hdr params;
	const uint8_t *rmctrl;
	size_t remaining;
	uint64_t max_count;

	if ((size_t)(blob_end - bhdr->data) < sizeof(*vhdr)) {
		fprintf(stderr, "blob %lu is smaller than its vGPU type header\n",
			idx);
		return -1;
	}

	vhdr = (const void *)bhdr->data;
	remaining = (size_t)(blob_end - vhdr->data);
	if (vhdr->kernel_struct_size > (uint64_t)remaining) {
		fprintf(stderr, "blob %lu kernel struct section is out of bounds\n",
			idx);
		return -1;
	}

	rmctrl = vhdr->data + (size_t)vhdr->kernel_struct_size;
	remaining = (size_t)(blob_end - rmctrl);
	if (vhdr->gsp_rmctrl_size > (uint64_t)remaining) {
		fprintf(stderr, "blob %lu GSP RMCTRL section is out of bounds\n",
			idx);
		return -1;
	}

	if (vhdr->gsp_rmctrl_vgpu_info_offset != sizeof(params) ||
	    vhdr->gsp_rmctrl_size < sizeof(params)) {
		fprintf(stderr,
			"blob %lu has unsupported vGPU info offset %lu\n",
			idx, vhdr->gsp_rmctrl_vgpu_info_offset);
		return -1;
	}
	if (!vhdr->gsp_rmctrl_vgpu_info_size) {
		fprintf(stderr, "blob %lu has an invalid vGPU info size\n", idx);
		return -1;
	}

	memcpy(&params, rmctrl, sizeof(params));
	if (!params.vgpu_info_count ||
	    params.vgpu_info_count > GMCAPI_MAX_VGPU_TYPES) {
		fprintf(stderr, "blob %lu has invalid vGPU info count %u\n",
			idx, params.vgpu_info_count);
		return -1;
	}
	if (vhdr->num_kernel_structs != params.vgpu_info_count) {
		fprintf(stderr,
			"blob %lu kernel/GSP vGPU counts differ (%lu/%u)\n",
			idx, vhdr->num_kernel_structs,
			params.vgpu_info_count);
		return -1;
	}

	max_count = (vhdr->gsp_rmctrl_size -
		     vhdr->gsp_rmctrl_vgpu_info_offset) /
		    vhdr->gsp_rmctrl_vgpu_info_size;
	if (params.vgpu_info_count > max_count) {
		fprintf(stderr, "blob %lu vGPU info array is out of bounds\n", idx);
		return -1;
	}

	view->hdr = vhdr;
	view->info = rmctrl + vhdr->gsp_rmctrl_vgpu_info_offset;
	view->info_size = (size_t)vhdr->gsp_rmctrl_vgpu_info_size;
	view->info_count = params.vgpu_info_count;
	return 0;
}

static int send_vgpu_type_blob(int fd,
			       const struct vgpu_type_blob_view *view,
			       const uint8_t *gsp_build_version,
			       uint64_t *uploaded)
{
	uint32_t i;

	fprintf(stderr,
		"uploading %u vGPU types via multi-round GMC "
		"(per-round payload: %zu bytes)\n",
		view->info_count,
		sizeof(struct gmcapi_add_vgpu_type_hdr) + view->info_size);

	for (i = 0; i < view->info_count; i++) {
		const uint8_t *info = view->info + i * view->info_size;

		if (send_vgpu_type(fd, info, view->info_size,
				   gsp_build_version, *uploaded == 0))
			return -1;

		(*uploaded)++;
		fprintf(stderr, "  [%u/%u] uploaded\n", i + 1,
			view->info_count);
	}

	return 0;
}

static int send_all_vgpu_types(int fd, const void *file_base,
			       size_t file_size, uint64_t device_id)
{
	const struct metadata_hdr *mhdr = file_base;
	const uint8_t *end = (const uint8_t *)file_base + file_size;
	const uint8_t *cursor;
	uint64_t i;
	uint64_t found = 0;
	uint64_t uploaded = 0;

	fprintf(stderr, "device_id:    0x%04lx\n", device_id);

	/* Validate every blob before changing GSP state. */
	cursor = mhdr->data;
	for (i = 0; i < mhdr->num_blobs; i++) {
		const struct metadata_blob_hdr *bhdr = (const void *)cursor;
		struct vgpu_type_blob_view view;

		if (validate_blob_hdr(bhdr, end, i))
			return -1;

		if (bhdr->type == CONFIG_BLOB_VGPU_TYPE) {
			if (get_vgpu_type_blob_view(bhdr, i, &view))
				return -1;
			if (view.hdr->device_id == device_id)
				found++;
		}

		cursor += (size_t)bhdr->size;
	}

	if (!found) {
		fprintf(stderr, "no vGPU type blob found for device 0x%04lx\n", device_id);
		return -1;
	}

	/* Upload each matching blob one vGPU info record per GMC round. */
	cursor = mhdr->data;
	for (i = 0; i < mhdr->num_blobs; i++) {
		const struct metadata_blob_hdr *bhdr = (const void *)cursor;
		struct vgpu_type_blob_view view;

		if (validate_blob_hdr(bhdr, end, i))
			return -1;

		if (bhdr->type == CONFIG_BLOB_VGPU_TYPE) {
			if (get_vgpu_type_blob_view(bhdr, i, &view))
				return -1;
			if (view.hdr->device_id == device_id &&
			    send_vgpu_type_blob(fd, &view,
						mhdr->gsp_build_version,
						&uploaded))
				return -1;
		}

		cursor += (size_t)bhdr->size;
	}

	fprintf(stderr, "\n%lu vGPU Types Uploaded. ", uploaded);
	return 0;
}

int cmd_add_type(const char *prog, int argc, char **argv)
{
	const char *dev_path = NULL;
	const char *meta_path = NULL;
	uint64_t device_id = 0;
	NvU32 actual_device_id = 0;
	int got_pci = 0;
	int opt, fd = -1, meta_fd = -1, ret = EXIT_FAILURE;
	struct stat st;
	void *meta_mem = MAP_FAILED;
	struct metadata_hdr *mhdr;

	while ((opt = getopt(argc, argv, "d:f:p:h")) != -1) {
		switch (opt) {
		case 'd':
			dev_path = optarg;
			break;
		case 'f':
			meta_path = optarg;
			break;
		case 'p': {
			NvU32 dev_id_u32;
			if (parse_u32("-p", optarg, &dev_id_u32))
				return EXIT_FAILURE;
			device_id = dev_id_u32;
			got_pci = 1;
			break;
		}
		case 'h':
		default:
			usage_add_type(prog);
			return opt == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
		}
	}

	if (reject_extra_args(prog, argc, argv))
		return EXIT_FAILURE;

	if (!meta_path || !got_pci) {
		fprintf(stderr, "error: -f <metadata_file> and -p <device_id> are required\n\n");
		usage_add_type(prog);
		return EXIT_FAILURE;
	}

	/* Open fwctl device */
	if (dev_path)
		fd = open_fwctl_device(dev_path);
	else
		fd = open_nova_core_fwctl();

	if (fd < 0) {
		fprintf(stderr, "no nova-core fwctl device found\n");
		return EXIT_FAILURE;
	}

	fprintf(stderr, "fwctl device opened\n");

	/* Reject -p values that don't match the underlying hardware before
	 * we touch the metadata file -- a mismatched device_id would either
	 * silently miss every blob or upload types keyed to the wrong SKU. */
	if (fwctl_get_pci_device_id(fd, &actual_device_id))
		goto out;

	if ((NvU32)device_id != actual_device_id) {
		fprintf(stderr,
			"error: -p 0x%04lx does not match fwctl device PCI ID 0x%04x\n",
			device_id, actual_device_id);
		goto out;
	}

	/* Load and mmap metadata file */
	meta_fd = open(meta_path, O_RDONLY);
	if (meta_fd < 0) {
		perror("open metadata file");
		goto out;
	}

	if (fstat(meta_fd, &st)) {
		perror("fstat metadata file");
		goto out;
	}

	if ((size_t)st.st_size < sizeof(struct metadata_hdr)) {
		fprintf(stderr, "metadata file too small (%ld bytes)\n", st.st_size);
		goto out;
	}

	meta_mem = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, meta_fd, 0);
	if (meta_mem == MAP_FAILED) {
		perror("mmap metadata file");
		goto out;
	}

	/* Validate metadata header */
	mhdr = meta_mem;

	if (validate_metadata(mhdr, st.st_size)) {
		fprintf(stderr, "metadata validation failed\n");
		goto out;
	}

	fprintf(stderr, "metadata :    vgpu %lu.%lu\ngsp_build:    %s\n",
		mhdr->vgpu_major, mhdr->vgpu_minor,
		mhdr->gsp_build_version);

	/* Send all vGPU type blobs for the requested device to GSP via fwctl */
	if (send_all_vgpu_types(fd, meta_mem, st.st_size, device_id)) {
		fprintf(stderr, "failed to upload vGPU types\n");
		goto out;
	}

	fprintf(stderr, "ADD_VGPU_TYPE: OK\n");
	ret = EXIT_SUCCESS;

out:
	if (meta_mem != MAP_FAILED)
		munmap(meta_mem, st.st_size);
	if (meta_fd >= 0)
		close(meta_fd);
	if (fd >= 0)
		close(fd);
	return ret;
}
