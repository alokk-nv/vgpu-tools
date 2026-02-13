/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Deassign a vGPU type from a VF via the fwctl nova-core interface.
 *
 * GMCAPI DEASSIGN_VGPU_TYPE:
 *   in:  fwctl_rpc_nova_core_request_hdr + GmcapiDeassignVgpuTypeInParams
 *   out: fwctl_rpc_nova_core_resp_hdr (no payload)
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

static void usage_deassign(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"  -d <path>     fwctl device path (default: auto-detect)\n"
		"  -b <bdf>      VF address in PCI BDF notation [dddd:]bb:dd.f\n"
		"                (e.g. 0000:01:00.0, 01:00.0)\n"
		"  -h            show this help\n",
		prog);
}

int cmd_deassign(const char *prog, int argc, char **argv)
{
	const char *dev_path = NULL;
	int opt, fd = -1, ret = EXIT_FAILURE;
	struct fwctl_rpc rpc = {0};
	struct fwctl_rpc_nova_core *req_hdr;
	struct fwctl_rpc_nova_core resp_hdr = {0};
	struct GmcapiDeassignVgpuTypeInParams *params;
	uint32_t in_size = sizeof(*req_hdr) + sizeof(*params);
	void *in_buf = NULL;
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
			usage_deassign(prog);
			return opt == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
		}
	}

	if (reject_extra_args(prog, argc, argv))
		return EXIT_FAILURE;

	if (!got_dbdf) {
		fprintf(stderr, "error: -b <bdf> is required\n\n");
		usage_deassign(prog);
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

	/* Refuse to dispatch the deassign RPC to the wrong GSP -- catch
	 * the case where -d points at one PF's fwctl but -b is a VF under
	 * a different PF. */
	if (fwctl_check_vf_matches(fd, dbdf) < 0) {
		ret = EXIT_FAILURE;
		goto out;
	}

	/* Refuse to issue a no-op deassign. Querying the current type
	 * up-front lets us return a clear error when the VF isn't bound
	 * to anything, instead of relying on the underlying RPC to fail
	 * with a generic state error. */
	{
		NvU32 current_type = 0;

		if (vgpu_query_assigned_type(fd, dbdf, &current_type) < 0) {
			ret = EXIT_FAILURE;
			goto out;
		}
		if (current_type == 0) {
			fprintf(stderr,
				"error: VF is not currently assigned to any vGPU type; "
				"nothing to deassign\n");
			ret = EXIT_FAILURE;
			goto out;
		}
	}

	ret = posix_memalign(&in_buf, 16, in_size);
	if (ret) {
		fprintf(stderr, "posix_memalign failed: %d\n", ret);
		ret = EXIT_FAILURE;
		goto out;
	}
	memset(in_buf, 0, in_size);

	req_hdr = in_buf;
	req_hdr->command_id = FWCTL_CMD_NOVA_CORE_GMCAPI_DEASSIGN_VGPU_TYPE;

	params = (struct GmcapiDeassignVgpuTypeInParams *)((uint8_t *)in_buf + sizeof(*req_hdr));
	params->dbdf = dbdf;

	rpc.size = sizeof(rpc);
	rpc.scope = FWCTL_RPC_CONFIGURATION;
	rpc.in_len = in_size;
	rpc.out_len = sizeof(resp_hdr);
	rpc.in = (uint64_t)(uintptr_t)in_buf;
	rpc.out = (uint64_t)(uintptr_t)&resp_hdr;

	if (ioctl(fd, FWCTL_RPC, &rpc)) {
		perror("ioctl(FWCTL_RPC) DEASSIGN_VGPU_TYPE");
		ret = EXIT_FAILURE;
		goto out;
	}

	fprintf(stderr, "DEASSIGN_VGPU_TYPE: OK\n");
	ret = EXIT_SUCCESS;

out:
	free(in_buf);
	if (fd >= 0)
		close(fd);
	return ret;
}
