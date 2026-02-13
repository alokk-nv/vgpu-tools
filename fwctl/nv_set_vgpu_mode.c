/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * vgpu-mgmt set-vgpu-mode subcommand.
 *
 * Mirrors gpu-admin-tools nvidia_gpu_tools.py:GpuDevice.set_vgpu_mode():
 * map "on" -> 0x1, "off" -> 0x0 and PRC-knob-write knob ID 41
 * (PRC_KNOB_ID_VGPU) via check-and-write. A reboot is required for the new
 * mode to take effect, matching the Python tool's message.
 *
 * Transport is fixed to MNOC port 0 (Blackwell+ FSP path).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#include "fwctl_common.h"
#include "nv_vgpu.h"
#include "prc_knob.h"

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s -b <[dddd:]bb:dd.f> -m {on|off}\n"
		"\n"
		"  -b  PCI BDF of the GPU (e.g. 0000:17:00.0, 17:00.0)\n"
		"  -m  vGPU mode to configure: on or off\n"
		"  -h  show this help\n",
		prog);
}

int cmd_set_vgpu_mode(const char *prog, int argc, char **argv)
{
	const char *bdf_arg = NULL;
	const char *mode = NULL;
	char bdf[16];
	NvU64 dbdf = 0;
	struct prc_knob_ctx *ctx = NULL;
	uint16_t value, current;
	int rc;
	int opt;

	while ((opt = getopt(argc, argv, "b:m:h")) != -1) {
		switch (opt) {
		case 'b':
			bdf_arg = optarg;
			break;
		case 'm':
			mode = optarg;
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

	if (!bdf_arg || !mode) {
		fprintf(stderr, "%s: -b <bdf> and -m <mode> are required\n", prog);
		usage(prog);
		return EXIT_FAILURE;
	}

	if (!strcmp(mode, "on"))
		value = 0x1;
	else if (!strcmp(mode, "off"))
		value = 0x0;
	else {
		fprintf(stderr, "invalid -m value '%s' (use on or off)\n", mode);
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

	/* Read first so we can short-circuit no-op writes ("already X")
	 * and surface "knob not supported" before attempting the write. */
	rc = prc_knob_read(ctx, PRC_KNOB_ID_VGPU, &current);
	if (rc < 0) {
		fprintf(stderr, "prc_knob_read(VGPU) failed: %d (%s)\n",
			rc, strerror(-rc));
		prc_knob_close(ctx);
		return EXIT_FAILURE;
	}
	if (rc == 1) {
		fprintf(stderr, "%s: VGPU knob not supported by FSP firmware\n", bdf);
		prc_knob_close(ctx);
		return EXIT_FAILURE;
	}
	if (rc != 0) {
		fprintf(stderr,
			"prc_knob_read(VGPU) returned unexpected status %d\n", rc);
		prc_knob_close(ctx);
		return EXIT_FAILURE;
	}

	if (current == value) {
		fprintf(stdout, "%s vGPU mode is already %s; no change\n",
			bdf, mode);
		prc_knob_close(ctx);
		return EXIT_SUCCESS;
	}

	rc = prc_knob_write(ctx, PRC_KNOB_ID_VGPU, value);
	if (rc < 0) {
		fprintf(stderr, "prc_knob_write(VGPU) failed: %d (%s)\n",
			rc, strerror(-rc));
		prc_knob_close(ctx);
		return EXIT_FAILURE;
	}
	if (rc == 1) {
		/* Shouldn't happen if the read above succeeded with rc==0,
		 * but the FW contract permits it -- treat as error rather
		 * than silently swallow. */
		fprintf(stderr, "%s: VGPU knob became unsupported between read and write\n",
			bdf);
		prc_knob_close(ctx);
		return EXIT_FAILURE;
	}
	if (rc != 0) {
		fprintf(stderr,
			"prc_knob_write(VGPU) returned unexpected status %d\n", rc);
		prc_knob_close(ctx);
		return EXIT_FAILURE;
	}

	fprintf(stdout,
		"%s vGPU mode set to %s. A reboot is required to activate the new mode.\n",
		bdf, mode);

	prc_knob_close(ctx);
	return EXIT_SUCCESS;
}
