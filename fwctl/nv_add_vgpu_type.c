/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Implements `vgpu-mgmt add-type`: validates vGPU metadata, selects
 * the group for the fwctl device, and uploads one vGPU type blob per RPC.
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fwctl.h"
#include "fwctl_common.h"
#include "metadata.h"
#include "nvrm.h"

struct gmcapi_add_vgpu_type_params_hdr {
	uint8_t discard_vgpu_types;
	uint8_t padding[3];
	uint32_t vgpu_info_count;
};

/* One vGPU type blob follows this prefix in each RPC. */
struct gmcapi_add_vgpu_type_payload_hdr {
	uint8_t gsp_build_version[VGPU_METADATA_GSP_VERSION_SIZE];
	struct gmcapi_add_vgpu_type_params_hdr params;
};

_Static_assert(sizeof(struct gmcapi_add_vgpu_type_params_hdr) == 8,
	       "unexpected add-vGPU-type parameter header layout");
_Static_assert(sizeof(struct gmcapi_add_vgpu_type_payload_hdr) == 136,
	       "unexpected add-vGPU-type payload header layout");

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

static int send_vgpu_type(int fd, const uint8_t *record, size_t record_size,
			  const uint8_t *gsp_build_version, bool discard)
{
	struct fwctl_rpc rpc = {0};
	struct fwctl_rpc_nova_core *req_hdr;
	struct fwctl_rpc_nova_core resp_hdr = {0};
	struct gmcapi_add_vgpu_type_payload_hdr *payload;
	size_t payload_size;
	size_t in_size;
	void *in_buf = NULL;
	int ret;

	if (!record || !gsp_build_version) {
		fprintf(stderr, "invalid vGPU type upload arguments\n");
		return -1;
	}
	if (record_size > UINT32_MAX - sizeof(*payload) - sizeof(*req_hdr)) {
		fprintf(stderr, "vGPU type blob is too large: %zu bytes\n",
			record_size);
		return -1;
	}

	payload_size = sizeof(*payload) + record_size;
	in_size = sizeof(*req_hdr) + payload_size;
	ret = posix_memalign(&in_buf, 16, in_size);
	if (ret) {
		fprintf(stderr, "failed to allocate %zu bytes for RPC input\n",
			in_size);
		return -1;
	}
	memset(in_buf, 0, in_size);

	req_hdr = in_buf;
	req_hdr->command_id = FWCTL_CMD_NOVA_CORE_GMCAPI_ADD_VGPU_TYPE;
	payload = (void *)((uint8_t *)in_buf + sizeof(*req_hdr));
	memcpy(payload->gsp_build_version, gsp_build_version,
	       sizeof(payload->gsp_build_version));
	payload->params.discard_vgpu_types = discard;
	payload->params.vgpu_info_count = 1;
	memcpy((uint8_t *)payload + sizeof(*payload), record, record_size);

	rpc.size = sizeof(rpc);
	rpc.scope = FWCTL_RPC_CONFIGURATION;
	rpc.in_len = (uint32_t)in_size;
	rpc.out_len = sizeof(resp_hdr);
	rpc.in = (uint64_t)(uintptr_t)in_buf;
	rpc.out = (uint64_t)(uintptr_t)&resp_hdr;

	ret = ioctl(fd, FWCTL_RPC, &rpc);
	if (ret)
		perror("ioctl(FWCTL_RPC) ADD_VGPU_TYPE");

	free(in_buf);
	return ret ? -1 : 0;
}

static int send_vgpu_type_group(int fd, const uint8_t *gsp_build_version,
				const uint8_t *records, uint32_t record_size,
				uint32_t record_count)
{
	uint32_t i;

	fprintf(stderr,
		"uploading %u vGPU types one blob per RPC "
		"(payload: %zu bytes)\n",
		record_count,
		sizeof(struct gmcapi_add_vgpu_type_payload_hdr) +
			record_size);

	for (i = 0; i < record_count; i++) {
		const uint8_t *record = records + (size_t)i * record_size;

		if (send_vgpu_type(fd, record, record_size,
				   gsp_build_version, i == 0))
			return -1;

		fprintf(stderr, "  [%u/%u] uploaded\n", i + 1, record_count);
	}

	return 0;
}

int cmd_add_type(const char *prog, int argc, char **argv)
{
	const char *dev_path = NULL;
	const char *meta_path = NULL;
	NvU16 requested_device_id = 0;
	NvU16 vendor_id = 0;
	NvU16 device_id = 0;
	NvU16 subsystem_vendor_id = 0;
	NvU16 subsystem_id = 0;
	int got_pci = 0;
	int opt;
	int fd = -1;
	int meta_fd = -1;
	int find_ret;
	int ret = EXIT_FAILURE;
	struct stat st = {0};
	size_t meta_size = 0;
	void *meta_mem = MAP_FAILED;
	const uint8_t *gsp_build_version;
	const uint8_t *records;
	uint32_t record_size;
	uint32_t record_count;

	while ((opt = getopt(argc, argv, "d:f:p:h")) != -1) {
		switch (opt) {
		case 'd':
			dev_path = optarg;
			break;
		case 'f':
			meta_path = optarg;
			break;
		case 'p':
			if (parse_u16("-p", optarg, &requested_device_id))
				return EXIT_FAILURE;
			got_pci = 1;
			break;
		case 'h':
		default:
			usage_add_type(prog);
			return opt == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
		}
	}

	if (reject_extra_args(prog, argc, argv))
		return EXIT_FAILURE;
	if (!meta_path || !got_pci) {
		fprintf(stderr,
			"error: -f <metadata_file> and -p <device_id> are required\n\n");
		usage_add_type(prog);
		return EXIT_FAILURE;
	}

	if (dev_path)
		fd = open_fwctl_device(dev_path);
	else
		fd = open_nova_core_fwctl();
	if (fd < 0) {
		fprintf(stderr, "no nova-core fwctl device found\n");
		goto out;
	}
	fprintf(stderr, "fwctl device opened\n");

	if (fwctl_get_pci_ids(fd, &vendor_id, &device_id,
			      &subsystem_vendor_id, &subsystem_id))
		goto out;
	if (requested_device_id != device_id) {
		fprintf(stderr,
			"error: -p 0x%04x does not match fwctl device PCI ID 0x%04x\n",
			requested_device_id, device_id);
		goto out;
	}

	meta_fd = open(meta_path, O_RDONLY | O_CLOEXEC);
	if (meta_fd < 0) {
		perror("open metadata file");
		goto out;
	}
	if (fstat(meta_fd, &st)) {
		perror("fstat metadata file");
		goto out;
	}
	if (!S_ISREG(st.st_mode)) {
		fprintf(stderr, "metadata path is not a regular file\n");
		goto out;
	}
	if (st.st_size < (off_t)VGPU_METADATA_HEADER_SIZE ||
	    (uintmax_t)st.st_size > VGPU_METADATA_MAX_FILE_SIZE) {
		fprintf(stderr, "metadata file has unsupported size %jd bytes\n",
			(intmax_t)st.st_size);
		goto out;
	}
	meta_size = (size_t)st.st_size;
	meta_mem = mmap(NULL, meta_size, PROT_READ, MAP_PRIVATE, meta_fd, 0);
	if (meta_mem == MAP_FAILED) {
		perror("mmap metadata file");
		goto out;
	}

	find_ret = metadata_find_vgpu_type_group(
		meta_mem, meta_size, vendor_id, device_id, subsystem_vendor_id,
		subsystem_id, &gsp_build_version, &records, &record_size,
		&record_count);
	if (find_ret < 0) {
		fprintf(stderr, "metadata validation failed\n");
		goto out;
	}
	if (!find_ret) {
		fprintf(stderr,
			"no vGPU type group for PCI IDs %04x:%04x:%04x:%04x\n",
			vendor_id, device_id, subsystem_vendor_id, subsystem_id);
		goto out;
	}

	fprintf(stderr,
		"gsp_build:   %s\n"
		"pci_ids:     %04x:%04x:%04x:%04x\n",
		gsp_build_version, vendor_id, device_id,
		subsystem_vendor_id, subsystem_id);

	if (send_vgpu_type_group(fd, gsp_build_version, records, record_size,
				 record_count)) {
		fprintf(stderr, "failed to upload vGPU types\n");
		goto out;
	}

	fprintf(stderr, "ADD_VGPU_TYPE: OK\n");
	ret = EXIT_SUCCESS;

out:
	if (meta_mem != MAP_FAILED)
		munmap(meta_mem, meta_size);
	if (meta_fd >= 0)
		close(meta_fd);
	if (fd >= 0)
		close(fd);
	return ret;
}
