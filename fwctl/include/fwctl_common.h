/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Shared helpers for the fwctl userspace tools: locating and validating a
 * nova-core fwctl device, and the per-subcommand entry points dispatched
 * from vgpu-mgmt.
 */

#ifndef FWCTL_COMMON_H
#define FWCTL_COMMON_H

#include "nvrm.h"

#define NVIDIA_PCI_VENDOR_ID 0x10de

/* NVA081_MAX_VGPU_TYPES_PER_PGPU -- bound on the NvU32[] payload returned by
 * QUERY_SUPPORTED_VGPU_TYPES / QUERY_CREATABLE_VGPU_TYPES. */
#define QUERY_VGPU_TYPES_MAX 0x40

/*
 * Scan /dev/fwctl for the first nova-core device and return an open RDWR fd.
 * Returns -1 if no nova-core device is found.
 */
int open_nova_core_fwctl(void);

/*
 * Open the given fwctl device path RDWR and verify it reports
 * FWCTL_DEVICE_TYPE_NOVA_CORE. Returns -1 on any failure.
 */
int open_fwctl_device(const char *dev_path);

/*
 * Parse PCI BDF notation [domain:]bus:device.function (e.g. 0000:01:00.0,
 * 01:00.0) into the packed 64-bit dbdf form (bits 63:32 = domain, 15:8 =
 * bus, 7:0 = devfn). Numeric fields are interpreted as hex per lspci
 * convention; domain defaults to 0 when omitted. Returns 0 on success,
 * -1 on error.
 */
int parse_dbdf(const char *s, NvU64 *out);

/*
 * Format the packed dbdf (as produced by parse_dbdf()) into canonical
 * "dddd:bb:dd.f" form and confirm /sys/bus/pci/devices/<bdf>/vendor matches
 * NVIDIA's vendor ID. Returns 0 on success, -1 on any sysfs error or
 * non-NVIDIA vendor.
 */
int check_nvidia_vendor_dbdf(NvU64 dbdf);

/*
 * Confirm the given dbdf is a Physical Function (PF), not a Virtual
 * Function. VFs are identified by the presence of a 'physfn' symlink
 * under /sys/bus/pci/devices/<bdf>/. Required gate for any operation
 * that mmaps BAR0 to reach the FSP mailbox -- only the PF exposes
 * that, and attempting it on a VF reads invalid registers (typically
 * faulting). Returns 0 on success, -1 on a VF or sysfs error.
 */
int check_is_pf_dbdf(NvU64 dbdf);

/*
 * Mirror of check_is_pf_dbdf for subcommands that operate on a VF
 * (assign / deassign / query-vf): rejects PFs. Returns 0 if the
 * given dbdf has a 'physfn' symlink (so it's a VF), -1 with an
 * error message otherwise.
 */
int check_is_vf_dbdf(NvU64 dbdf);

/*
 * Issue a QUERY_ASSIGNED_VF_VGPU_TYPE RPC on an already-open nova-core
 * fwctl fd and return the current type bound to $dbdf in *out_type_id
 * (0 == unassigned). Returns 0 on success, -1 with an error logged on
 * RPC / allocation failure. Used by assign/deassign as a preflight to
 * avoid clobber/no-op regressions.
 */
int vgpu_query_assigned_type(int fd, NvU64 dbdf, NvU32 *out_type_id);

/*
 * Issue QUERY_SUPPORTED_VGPU_TYPES on $fd and return whether $type_id
 * appears in the GSP-reported list. Returns 1 if present, 0 if absent
 * (with an error logged naming the bad type ID), -1 on RPC / allocation
 * failure. Used by query-props / assign as a preflight so an invalid -t
 * fails with a useful message instead of an opaque RPC error.
 */
int vgpu_type_is_supported(int fd, NvU32 type_id);

/*
 * Mirror of vgpu_type_is_supported against QUERY_CREATABLE_VGPU_TYPES --
 * the dynamic, per-PF set of types that currently have a free placement
 * slot. A type can be "supported" (metadata uploaded) yet not creatable
 * (vGPU mode off, all VFs already assigned, placement table doesn't
 * include this VF). Used by assign so EIO from GSP is preempted with a
 * specific hint.
 */
int vgpu_type_is_creatable(int fd, NvU32 type_id);

/*
 * Issue QUERY_SUPPORTED_VGPU_TYPES on $fd and return whether the list
 * is empty (1) or non-empty (0); returns -1 on RPC / allocation failure
 * with an error logged. Used by list-creatable when it sees an empty
 * creatable list, to distinguish "metadata never uploaded" from
 * "metadata uploaded but the placement plan / mode is blocking
 * everything."
 */
int vgpu_supported_list_is_empty(int fd);

/*
 * Read the four PCI IDs of the device underlying an already-open nova-core
 * fwctl fd from sysfs. Returns 0 with all outputs populated, or -1 with an
 * error logged. add-type uses them to select the matching metadata group.
 */
int fwctl_get_pci_ids(int fd, NvU16 *vendor_id, NvU16 *device_id,
			 NvU16 *subsystem_vendor_id, NvU16 *subsystem_id);

/*
 * After a subcommand's getopt() loop, fail if any positional arguments
 * remain. getopt leaves them at argv[optind..argc-1]. Without this check
 * "vgpu-mgmt assign -b 01:00.0 -t 5 garbage extra" succeeds silently.
 * Returns 0 if no extras, -1 with an error logged otherwise. Call
 * before required-arg validation so trailing junk is reported as its
 * specific cause rather than a misleading "missing required".
 */
int reject_extra_args(const char *prog, int argc, char **argv);

/*
 * Strict numeric parse helpers for CLI options. Reject:
 *   - empty / NULL input
 *   - leading '-' (strtoull silently wraps negatives to ULLONG_MAX)
 *   - trailing non-numeric chars ("5abc")
 *   - values that exceed the destination width
 *
 * Base interpretation follows strtoul(0): "0x..." hex, "0..." octal,
 * else decimal. $opt is the option label (e.g. "-t") for the diagnostic.
 * Returns 0 on success, -1 with an error logged otherwise.
 *
 * Replaces bare strtoul(optarg, NULL, 0) sites, which silently parsed
 * "abc" as 0 and truncated > 32-bit input.
 */
int parse_u32(const char *opt, const char *s, NvU32 *out);
int parse_u16(const char *opt, const char *s, NvU16 *out);

/*
 * Verify that $vf_dbdf is a VF under the PF exposed by the open
 * nova-core fwctl fd. Resolves the fwctl fd's underlying PF BDF via
 * /sys/class/fwctl/<name>/device, then the VF's parent PF BDF via
 * /sys/bus/pci/devices/<vf>/physfn, and compares the two. Returns 0
 * on match, -1 on mismatch or any sysfs/procfs failure (with an error
 * logged in both cases). Used by assign/deassign/query-vf to refuse
 * RPCs that would be dispatched to the wrong GSP -- callers must run
 * check_is_vf_dbdf() first so a missing physfn yields its specific
 * message rather than this one.
 */
int fwctl_check_vf_matches(int fd, NvU64 vf_dbdf);

/*
 * Subcommand entry points. Each takes a `prog` string ("vgpu-mgmt <subcmd>")
 * used for usage messages, plus the residual argc/argv (with argv[0] set to
 * the subcommand token so getopt scans from argv[1]).
 */
int cmd_add_type(const char *prog, int argc, char **argv);
int cmd_assign(const char *prog, int argc, char **argv);
int cmd_deassign(const char *prog, int argc, char **argv);
int cmd_list_supported(const char *prog, int argc, char **argv);
int cmd_list_creatable(const char *prog, int argc, char **argv);
int cmd_query_vf(const char *prog, int argc, char **argv);
int cmd_query_props(const char *prog, int argc, char **argv);
int cmd_query_vgpu_mode(const char *prog, int argc, char **argv);
int cmd_set_vgpu_mode(const char *prog, int argc, char **argv);

#endif /* FWCTL_COMMON_H */
