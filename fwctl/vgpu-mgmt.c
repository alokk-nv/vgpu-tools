/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * vgpu-mgmt: unified CLI for the fwctl-based GMCAPI userspace tools.
 * Dispatches to per-subcommand cmd_* entry points declared in fwctl_common.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fwctl_common.h"

struct subcommand {
	const char *name;
	const char *summary;
	int (*fn)(const char *prog, int argc, char **argv);
};

static const struct subcommand subcommands[] = {
	{ "add-type",       "upload vGPU type metadata blobs to GSP",        cmd_add_type       },
	{ "assign",         "assign a vGPU type to a VF",                     cmd_assign         },
	{ "deassign",       "deassign the vGPU type from a VF",               cmd_deassign       },
	{ "list-supported", "list supported vGPU type IDs",                   cmd_list_supported },
	{ "list-creatable", "list creatable vGPU type IDs",                   cmd_list_creatable },
	{ "query-vf",       "query the vGPU type assigned to a VF",           cmd_query_vf       },
	{ "query-props",    "query a vGPU type's properties",                 cmd_query_props    },
	{ "query-vgpu-mode","query vGPU mode via FSP PRC knob (direct BAR0)", cmd_query_vgpu_mode},
	{ "set-vgpu-mode",  "set vGPU mode via FSP PRC knob (direct BAR0)",   cmd_set_vgpu_mode  },
};

static void usage(const char *prog)
{
	size_t i;

	fprintf(stderr,
		"Usage: %s <subcommand> [options]\n"
		"\n"
		"Subcommands:\n",
		prog);

	for (i = 0; i < sizeof(subcommands) / sizeof(subcommands[0]); i++)
		fprintf(stderr, "  %-16s %s\n",
			subcommands[i].name, subcommands[i].summary);

	fprintf(stderr,
		"\n"
		"Run '%s <subcommand> -h' for subcommand-specific options.\n",
		prog);
}

int main(int argc, char **argv)
{
	const char *prog = (argc > 0 && argv[0]) ? argv[0] : "vgpu-mgmt";
	const char *sub;
	char prog_buf[128];
	size_t i;

	if (argc < 2) {
		usage(prog);
		return EXIT_FAILURE;
	}

	sub = argv[1];

	if (!strcmp(sub, "-h") || !strcmp(sub, "--help") || !strcmp(sub, "help")) {
		usage(prog);
		return EXIT_SUCCESS;
	}

	for (i = 0; i < sizeof(subcommands) / sizeof(subcommands[0]); i++) {
		if (strcmp(sub, subcommands[i].name) != 0)
			continue;

		snprintf(prog_buf, sizeof(prog_buf), "%s %s", prog, sub);
		return subcommands[i].fn(prog_buf, argc - 1, argv + 1);
	}

	fprintf(stderr, "%s: unknown subcommand '%s'\n\n", prog, sub);
	usage(prog);
	return EXIT_FAILURE;
}
