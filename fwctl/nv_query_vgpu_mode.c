/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * vgpu-mgmt query-vgpu-mode subcommand.
 *
 * Mirrors gpu-admin-tools nvidia_gpu_tools.py:GpuDevice.query_vgpu_mode():
 * read PRC knob ID 41 (PRC_KNOB_ID_VGPU) and map 0x1 -> "on", else "off",
 * with FSP "invalid knob" reported as "unsupported".
 *
 * Transport is fixed to MNOC port 0 (Blackwell+ FSP path).
 */

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>

#include "fwctl_common.h"
#include "nv_vgpu.h"
#include "prc_knob.h"

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s -b <[dddd:]bb:dd.f>\n"
		"\n"
		"  -b  PCI BDF of the GPU (e.g. 0000:17:00.0, 17:00.0)\n"
		"  -h  show this help\n",
		prog);
}

int cmd_query_vgpu_mode(const char *prog, int argc, char **argv)
{
	const char *bdf_arg = NULL;
	char bdf[16];
	NvU64 dbdf = 0;
	struct prc_knob_ctx *ctx = NULL;
	uint16_t value = 0;
	int rc;
	int opt;

	while ((opt = getopt(argc, argv, "b:h")) != -1) {
		switch (opt) {
		case 'b':
			bdf_arg = optarg;
			break;
		case 'h':
			usage(prog);
			return EXIT_SUCCESS;
		default:
			usage(prog);
			return EXIT_FAILURE;
		}
	}

	if (reject_extra_args(prog, argc, argv))
		return EXIT_FAILURE;

	if (!bdf_arg) {
		fprintf(stderr, "%s: -b <bdf> is required\n", prog);
		usage(prog);
		return EXIT_FAILURE;
	}

	if (parse_dbdf(bdf_arg, &dbdf)) {
		fprintf(stderr, "error: invalid -b value '%s'\n", bdf_arg);
		return EXIT_FAILURE;
	}

	if (check_nvidia_vendor_dbdf(dbdf) < 0)
		return EXIT_FAILURE;

	if (check_is_pf_dbdf(dbdf) < 0)
		return EXIT_FAILURE;

	snprintf(bdf, sizeof(bdf), "%04x:%02x:%02x.%x",
		 (unsigned)(dbdf >> 32),
		 (unsigned)((dbdf >> 8) & 0xFF),
		 (unsigned)((dbdf >> 3) & 0x1F),
		 (unsigned)(dbdf & 0x7));

	ctx = prc_knob_open(bdf);
	if (!ctx)
		return EXIT_FAILURE;

	rc = prc_knob_read(ctx, PRC_KNOB_ID_VGPU, &value);
	if (rc < 0) {
		fprintf(stderr, "prc_knob_read(VGPU) failed: %d\n", rc);
		prc_knob_close(ctx);
		return EXIT_FAILURE;
	}

	/* Contract: 0 = success (value valid), 1 = unsupported knob.
	 * Anything else is undefined -- don't print a state derived from a
	 * potentially-unwritten value. */
	if (rc == 0) {
		fprintf(stdout, "%s vGPU mode is %s\n",
			bdf, value == 0x1 ? "on" : "off");
	} else if (rc == 1) {
		fprintf(stdout, "%s vGPU mode is unsupported\n", bdf);
	} else {
		fprintf(stderr,
			"prc_knob_read(VGPU) returned unexpected status %d\n", rc);
		prc_knob_close(ctx);
		return EXIT_FAILURE;
	}

	prc_knob_close(ctx);
	return EXIT_SUCCESS;
}
