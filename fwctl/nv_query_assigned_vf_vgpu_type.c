/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Query the vGPU type currently assigned to a VF via the fwctl nova-core interface.
 *
 * GMCAPI QUERY_ASSIGNED_VF_VGPU_TYPE:
 *   in:  fwctl_rpc_nova_core_request_hdr + GmcapiQueryAssignedVfVgpuTypeInParams
 *   out: fwctl_rpc_nova_core_resp_hdr + GmcapiQueryAssignedVfVgpuTypeOutParams
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/ioctl.h>

#include "fwctl.h"
#include "nvrm.h"
#include "nv_vgpu.h"
#include "fwctl_common.h"

static void usage_query_vf(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"  -d <path>     fwctl device path (default: auto-detect)\n"
		"  -b <bdf>      VF address in PCI BDF notation [dddd:]bb:dd.f\n"
		"                (e.g. 0000:01:00.0, 01:00.0)\n"
		"  -h            show this help\n",
		prog);
}

int cmd_query_vf(const char *prog, int argc, char **argv)
{
	const char *dev_path = NULL;
	int opt, fd = -1, ret = EXIT_FAILURE;
	struct fwctl_rpc rpc = {0};
	struct fwctl_rpc_nova_core *req_hdr;
	struct fwctl_rpc_nova_core *resp_hdr;
	struct GmcapiQueryAssignedVfVgpuTypeInParams *in_params;
	struct GmcapiQueryAssignedVfVgpuTypeOutParams *out_params;
	uint32_t in_size = sizeof(*req_hdr) + sizeof(*in_params);
	uint32_t out_size = sizeof(*resp_hdr) + sizeof(*out_params);
	void *in_buf = NULL;
	void *out_buf = NULL;
	NvU64 dbdf = 0;
	int got_dbdf = 0;

	while ((opt = getopt(argc, argv, "d:b:h")) != -1) {
		switch (opt) {
		case 'd':
			dev_path = optarg;
			break;
		case 'b':
			if (parse_dbdf(optarg, &dbdf)) {
				fprintf(stderr, "error: invalid -b value '%s'\n", optarg);
				return EXIT_FAILURE;
			}
			got_dbdf = 1;
			break;
		case 'h':
		default:
			usage_query_vf(prog);
			return opt == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
		}
	}

	if (reject_extra_args(prog, argc, argv))
		return EXIT_FAILURE;

	if (!got_dbdf) {
		fprintf(stderr, "error: -b <bdf> is required\n\n");
		usage_query_vf(prog);
		return EXIT_FAILURE;
	}

	if (check_nvidia_vendor_dbdf(dbdf) < 0)
		return EXIT_FAILURE;

	if (check_is_vf_dbdf(dbdf) < 0)
		return EXIT_FAILURE;

	fprintf(stderr,
		"BDF: %04x:%02x:%02x.%x\n",
		(unsigned)(dbdf >> 32),
		(unsigned)((dbdf >> 8) & 0xFF),
		(unsigned)((dbdf >> 3) & 0x1F),
		(unsigned)(dbdf & 0x7));

	if (dev_path)
		fd = open_fwctl_device(dev_path);
	else
		fd = open_nova_core_fwctl();

	if (fd < 0) {
		fprintf(stderr, "no nova-core fwctl device found\n");
		return EXIT_FAILURE;
	}

	fprintf(stderr, "fwctl device opened\n");

	/* Refuse to dispatch the query RPC to the wrong GSP -- catch the
	 * case where -d points at one PF's fwctl but -b is a VF under a
	 * different PF. */
	if (fwctl_check_vf_matches(fd, dbdf) < 0) {
		ret = EXIT_FAILURE;
		goto out;
	}

	ret = posix_memalign(&in_buf, 16, in_size);
	if (ret) {
		fprintf(stderr, "posix_memalign(in) failed: %d\n", ret);
		ret = EXIT_FAILURE;
		goto out;
	}
	memset(in_buf, 0, in_size);

	ret = posix_memalign(&out_buf, 16, out_size);
	if (ret) {
		fprintf(stderr, "posix_memalign(out) failed: %d\n", ret);
		ret = EXIT_FAILURE;
		goto out;
	}
	memset(out_buf, 0, out_size);

	req_hdr = in_buf;
	req_hdr->command_id = FWCTL_CMD_NOVA_CORE_GMCAPI_QUERY_ASSIGNED_VF_VGPU_TYPE;

	in_params = (struct GmcapiQueryAssignedVfVgpuTypeInParams *)
		((uint8_t *)in_buf + sizeof(*req_hdr));
	in_params->dbdf = dbdf;

	rpc.size = sizeof(rpc);
	rpc.scope = FWCTL_RPC_CONFIGURATION;
	rpc.in_len = in_size;
	rpc.out_len = out_size;
	rpc.in = (uint64_t)(uintptr_t)in_buf;
	rpc.out = (uint64_t)(uintptr_t)out_buf;

	if (ioctl(fd, FWCTL_RPC, &rpc)) {
		perror("ioctl(FWCTL_RPC) QUERY_ASSIGNED_VF_VGPU_TYPE");
		ret = EXIT_FAILURE;
		goto out;
	}

	resp_hdr = out_buf;

	out_params = (struct GmcapiQueryAssignedVfVgpuTypeOutParams *)
		((uint8_t *)out_buf + sizeof(*resp_hdr));

	fprintf(stdout, "ASSIGNED VGPU TYPE ID: %u (0x%x)\n",
		out_params->vgpuTypeId, out_params->vgpuTypeId);

	if (out_params->vgpuTypeId == 0)
		fprintf(stdout, "  (VF is unassigned)\n");

	fprintf(stdout, "QUERY_ASSIGNED_VF_VGPU_TYPE: OK\n");
	ret = EXIT_SUCCESS;

out:
	free(in_buf);
	free(out_buf);
	if (fd >= 0)
		close(fd);
	return ret;
}
