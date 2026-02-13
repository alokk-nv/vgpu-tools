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

static int validate_metadata(struct metadata_hdr *hdr, size_t file_size)
{
	char idr[sizeof(METADATA_IDR)] = {0};
	uint8_t *p;
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

	return 0;
}

static int send_vgpu_type(int fd, const void *payload, size_t payload_len)
{
	struct fwctl_rpc rpc = {0};
	struct fwctl_rpc_nova_core *req_hdr;
	struct fwctl_rpc_nova_core resp_hdr;
	uint32_t hdr_size = sizeof(*req_hdr);
	uint32_t in_size = hdr_size + payload_len;
	void *in_buf;
	int ret;

	ret = posix_memalign(&in_buf, 16, in_size);
	if (ret) {
		fprintf(stderr, "failed to allocate %u bytes for RPC in buffer\n",
			in_size);
		return -ENOMEM;
	}

	memset(in_buf, 0, in_size);

	req_hdr = in_buf;
	req_hdr->command_id = FWCTL_CMD_NOVA_CORE_GMCAPI_ADD_VGPU_TYPE;
	memcpy((uint8_t *)in_buf + hdr_size, payload, payload_len);

	memset(&resp_hdr, 0, sizeof(resp_hdr));

	rpc.size = sizeof(rpc);
	rpc.scope = FWCTL_RPC_CONFIGURATION;
	rpc.in_len = in_size;
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

/* Bounds + minimum-size check for a single blob header at $bhdr.
 * Called by both passes of send_all_vgpu_types so the second pass
 * doesn't trust the layout validated by the first -- if a future
 * refactor changes which pass does what, both still self-validate. */
static int validate_blob_hdr(const struct metadata_blob_hdr *bhdr,
			     const void *end, uint64_t idx)
{
	const void *p = bhdr;

	if (p + sizeof(*bhdr) > end || p + bhdr->size > end) {
		fprintf(stderr, "blob %lu extends past end of file\n", idx);
		return -1;
	}
	if (bhdr->size < sizeof(*bhdr)) {
		fprintf(stderr, "blob %lu size %lu smaller than header\n",
			idx, bhdr->size);
		return -1;
	}
	return 0;
}

static int send_all_vgpu_types(int fd, void *file_base, size_t file_size, uint64_t device_id)
{
	struct metadata_hdr *mhdr = file_base;
	void *end = (uint8_t *)file_base + file_size;
	void *cursor;
	uint64_t i;
	int found = 0, idx = 0;

	fprintf(stderr, "device_id:    0x%04lx\n", device_id);

	/* First pass: validate, count matching blob. */
	cursor = mhdr->data;
	for (i = 0; i < mhdr->num_blobs; i++) {
		struct metadata_blob_hdr *bhdr = cursor;

		if (validate_blob_hdr(bhdr, end, i))
			return -1;

		if (bhdr->type == CONFIG_BLOB_VGPU_TYPE && bhdr->device_id == device_id) {
			found++;
		}

		cursor = (uint8_t *)cursor + bhdr->size;
	}

	if (!found) {
		fprintf(stderr, "no vGPU type blob found for device 0x%04lx\n", device_id);
		return -1;
	}

	fprintf(stderr,
		"\nuploading %d vGPU types via multi-round GMC\n", found);

	/* Second pass: send each matching blob. */
	cursor = mhdr->data;
	for (i = 0; i < mhdr->num_blobs; i++) {
		struct metadata_blob_hdr *bhdr = cursor;

		if (validate_blob_hdr(bhdr, end, i))
			return -1;

		if (bhdr->type == CONFIG_BLOB_VGPU_TYPE && bhdr->device_id == device_id) {
			idx++;
			if (send_vgpu_type(fd, bhdr->data, bhdr->size - sizeof(*bhdr)))
				return -1;
		}

		cursor = (uint8_t *)cursor + bhdr->size;
	}

	fprintf(stderr,
		"\n%d vGPU Types Uploaded. ", idx);
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

	fprintf(stderr, "metadata :    vgpu %lu.%lu\ngsp_build:    %s\n",
		mhdr->vgpu_major, mhdr->vgpu_minor,
		mhdr->gsp_build_version);

	if (validate_metadata(mhdr, st.st_size)) {
		fprintf(stderr, "metadata validation failed\n");
		goto out;
	}

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
