/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <getopt.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include "fwctl.h"
#include "nvrm.h"
#include "nv_vgpu.h"
#include "fwctl_common.h"

/*
 * The 64-bit dbdf value matches pGpu->busInfo.nvDomainBusDeviceFunc:
 *   bits 63:32 = domain, 15:8 = bus, 7:0 = devfn (device<<3 | function).
 */
static NvU64 dbdf_encode(uint32_t domain, uint8_t bus, uint8_t device, uint8_t function)
{
	return ((NvU64)domain << 32) |
	       ((NvU64)bus << 8) |
	       (NvU64)(((device & 0x1F) << 3) | (function & 0x7));
}

int reject_extra_args(const char *prog, int argc, char **argv)
{
	int i;

	if (optind >= argc)
		return 0;

	fprintf(stderr, "error: unexpected positional argument(s):");
	for (i = optind; i < argc; i++)
		fprintf(stderr, " '%s'", argv[i]);
	fprintf(stderr, "\nrun '%s -h' for help\n", prog);
	return -1;
}

static int parse_unsigned(const char *opt, const char *s,
			  unsigned long long max, unsigned long long *out)
{
	unsigned long long v;
	char *end;

	if (!s || !*s) {
		fprintf(stderr, "error: %s requires a non-empty value\n", opt);
		return -1;
	}
	/* strtoull silently wraps "-1" to ULLONG_MAX without ERANGE -- reject
	 * negatives up front for a useful message. */
	if (s[0] == '-') {
		fprintf(stderr, "error: %s value '%s' is negative\n", opt, s);
		return -1;
	}

	errno = 0;
	v = strtoull(s, &end, 0);
	if (errno == ERANGE) {
		fprintf(stderr, "error: %s value '%s' is out of range\n", opt, s);
		return -1;
	}
	if (end == s || *end != '\0') {
		fprintf(stderr,
			"error: %s value '%s' is not a valid integer\n", opt, s);
		return -1;
	}
	if (v > max) {
		fprintf(stderr,
			"error: %s value '%s' exceeds maximum 0x%llx\n",
			opt, s, max);
		return -1;
	}
	*out = v;
	return 0;
}

int parse_u32(const char *opt, const char *s, NvU32 *out)
{
	unsigned long long v;

	if (parse_unsigned(opt, s, 0xFFFFFFFFULL, &v))
		return -1;
	*out = (NvU32)v;
	return 0;
}

int parse_u16(const char *opt, const char *s, NvU16 *out)
{
	unsigned long long v;

	if (parse_unsigned(opt, s, 0xFFFFULL, &v))
		return -1;
	*out = (NvU16)v;
	return 0;
}

int parse_dbdf(const char *s, NvU64 *out)
{
	uint32_t domain = 0;
	unsigned int bus = 0, device = 0, function = 0;
	int n;

	if (!s || !*s)
		return -1;

	/* dddd:bb:dd.f */
	if (sscanf(s, "%x:%x:%x.%x%n", &domain, &bus, &device, &function, &n) == 4 &&
	    s[n] == '\0')
		goto done;

	/* bb:dd.f (domain defaults to 0) */
	domain = 0;
	if (sscanf(s, "%x:%x.%x%n", &bus, &device, &function, &n) == 3 &&
	    s[n] == '\0')
		goto done;

	return -1;

done:
	if (bus > 0xFF || device > 0x1F || function > 0x7)
		return -1;
	*out = dbdf_encode(domain, (uint8_t)bus, (uint8_t)device, (uint8_t)function);
	return 0;
}

int open_nova_core_fwctl(void)
{
	struct dirent *ent;
	DIR *dir;
	char path[280];
	int fd;

	dir = opendir("/dev/fwctl");
	if (!dir)
		return -1;

	while ((ent = readdir(dir)) != NULL) {
		struct fwctl_info info = {0};

		if (strncmp(ent->d_name, "fwctl", 5) != 0)
			continue;

		snprintf(path, sizeof(path), "/dev/fwctl/%s", ent->d_name);
		fd = open(path, O_RDWR);
		if (fd < 0)
			continue;

		info.size = sizeof(info);
		if (ioctl(fd, FWCTL_INFO, &info) == 0 &&
		    info.out_device_type == FWCTL_DEVICE_TYPE_NOVA_CORE) {
			fprintf(stderr, "using %s (device_type=%u)\n",
				path, info.out_device_type);
			closedir(dir);
			return fd;
		}

		close(fd);
	}

	closedir(dir);
	return -1;
}

int open_fwctl_device(const char *dev_path)
{
	struct fwctl_info info = {0};
	int fd;

	fd = open(dev_path, O_RDWR);
	if (fd < 0) {
		perror("open fwctl device");
		return -1;
	}

	info.size = sizeof(info);
	if (ioctl(fd, FWCTL_INFO, &info)) {
		perror("ioctl(FWCTL_INFO)");
		close(fd);
		return -1;
	}

	if (info.out_device_type != FWCTL_DEVICE_TYPE_NOVA_CORE) {
		fprintf(stderr, "%s is not a nova-core device (type=%u)\n",
			dev_path, info.out_device_type);
		close(fd);
		return -1;
	}

	fprintf(stderr, "using %s (device_type=%u)\n",
		dev_path, info.out_device_type);
	return fd;
}

static int check_nvidia_vendor(const char *bdf)
{
	char path[256];
	char buf[32];
	FILE *f;
	unsigned long vendor;

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/vendor", bdf);
	f = fopen(path, "r");
	if (!f) {
		if (errno == ENOENT) {
			fprintf(stderr,
				"PCI device %s not found on this system.\n"
				"\n"
				"The BDF you passed (-b %s) does not correspond to any PCI device\n"
				"currently visible to the kernel. Common causes:\n"
				"\n"
				"  * Typo in the BDF -- double-check domain/bus/device/function.\n"
				"  * SR-IOV is not enabled on the parent PF, so this VF does not\n"
				"    exist yet. Check:\n"
				"        cat /sys/bus/pci/devices/<pf-bdf>/sriov_numvfs\n"
				"    If it is 0, enable VFs first:\n"
				"        echo N > /sys/bus/pci/devices/<pf-bdf>/sriov_numvfs\n"
				"    (N must be <= sriov_totalvfs.)\n"
				"\n",
				bdf, bdf);
		} else {
			fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		}
		return -1;
	}
	if (!fgets(buf, sizeof(buf), f)) {
		fprintf(stderr, "read %s: %s\n", path, strerror(errno));
		fclose(f);
		return -1;
	}
	fclose(f);

	vendor = strtoul(buf, NULL, 16);
	if (vendor != NVIDIA_PCI_VENDOR_ID) {
		fprintf(stderr,
			"%s: PCI vendor 0x%04lx is not NVIDIA (0x%04x).\n",
			bdf, vendor, NVIDIA_PCI_VENDOR_ID);
		return -1;
	}
	return 0;
}

int check_nvidia_vendor_dbdf(NvU64 dbdf)
{
	char bdf[16];

	snprintf(bdf, sizeof(bdf), "%04x:%02x:%02x.%x",
		 (unsigned)(dbdf >> 32),
		 (unsigned)((dbdf >> 8) & 0xFF),
		 (unsigned)((dbdf >> 3) & 0x1F),
		 (unsigned)(dbdf & 0x7));

	return check_nvidia_vendor(bdf);
}

/* Returns 1 if /sys/bus/pci/devices/<bdf>/physfn exists as a symlink
 * (i.e. the device is a VF), 0 if it doesn't (PF), regardless of any
 * filesystem errors -- callers don't care about the difference. */
static int dbdf_has_physfn(const char *bdf)
{
	char path[256];
	struct stat st;

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/physfn", bdf);
	if (lstat(path, &st) < 0)
		return 0;
	return S_ISLNK(st.st_mode) ? 1 : 0;
}

static void format_dbdf(NvU64 dbdf, char *out, size_t out_sz)
{
	snprintf(out, out_sz, "%04x:%02x:%02x.%x",
		 (unsigned)(dbdf >> 32),
		 (unsigned)((dbdf >> 8) & 0xFF),
		 (unsigned)((dbdf >> 3) & 0x1F),
		 (unsigned)(dbdf & 0x7));
}

int check_is_pf_dbdf(NvU64 dbdf)
{
	char bdf[16];
	char path[256];
	char target[256];
	ssize_t n;
	const char *pf;

	format_dbdf(dbdf, bdf, sizeof(bdf));

	if (!dbdf_has_physfn(bdf))
		return 0;

	/* It IS a VF. Report the parent PF in the error so the caller
	 * knows what to retry with. */
	fprintf(stderr,
		"%s: is a VF, not a PF; this command requires the PF\n",
		bdf);

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/physfn", bdf);
	n = readlink(path, target, sizeof(target) - 1);
	if (n > 0) {
		target[n] = '\0';
		pf = strrchr(target, '/');
		pf = pf ? pf + 1 : target;
		fprintf(stderr, "  -> try -b %s instead\n", pf);
	}
	return -1;
}

int check_is_vf_dbdf(NvU64 dbdf)
{
	char bdf[16];

	format_dbdf(dbdf, bdf, sizeof(bdf));

	if (dbdf_has_physfn(bdf))
		return 0;

	fprintf(stderr,
		"%s: is a PF, not a VF; this command requires a VF\n",
		bdf);
	return -1;
}

int vgpu_query_assigned_type(int fd, NvU64 dbdf, NvU32 *out_type_id)
{
	struct fwctl_rpc rpc = {0};
	struct fwctl_rpc_nova_core *req_hdr;
	struct fwctl_rpc_nova_core *resp_hdr;
	struct GmcapiQueryAssignedVfVgpuTypeInParams *in_params;
	struct GmcapiQueryAssignedVfVgpuTypeOutParams *out_params;
	uint32_t in_size = sizeof(*req_hdr) + sizeof(*in_params);
	uint32_t out_size = sizeof(*resp_hdr) + sizeof(*out_params);
	void *in_buf = NULL;
	void *out_buf = NULL;
	int ret = -1;
	int err;

	err = posix_memalign(&in_buf, 16, in_size);
	if (err) {
		fprintf(stderr, "posix_memalign(in) failed: %d\n", err);
		goto out;
	}
	memset(in_buf, 0, in_size);

	err = posix_memalign(&out_buf, 16, out_size);
	if (err) {
		fprintf(stderr, "posix_memalign(out) failed: %d\n", err);
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
		perror("ioctl(FWCTL_RPC) QUERY_ASSIGNED_VF_VGPU_TYPE (preflight)");
		goto out;
	}

	resp_hdr = out_buf;
	out_params = (struct GmcapiQueryAssignedVfVgpuTypeOutParams *)
		((uint8_t *)out_buf + sizeof(*resp_hdr));
	*out_type_id = out_params->vgpuTypeId;
	ret = 0;

out:
	free(in_buf);
	free(out_buf);
	return ret;
}

static int vgpu_type_in_query_list(int fd, NvU32 cmd, const char *cmd_name,
				   NvU32 type_id, const char *list_token)
{
	struct fwctl_rpc rpc = {0};
	struct fwctl_rpc_nova_core req_hdr = {0};
	uint32_t out_payload_size = QUERY_VGPU_TYPES_MAX * sizeof(NvU32);
	uint32_t out_size = sizeof(struct fwctl_rpc_nova_core) + out_payload_size;
	void *out_buf = NULL;
	NvU32 *ids;
	uint32_t i;
	int ret = -1;
	int err;

	err = posix_memalign(&out_buf, 16, out_size);
	if (err) {
		fprintf(stderr, "posix_memalign failed: %d\n", err);
		goto out;
	}
	memset(out_buf, 0, out_size);

	req_hdr.command_id = cmd;

	rpc.size = sizeof(rpc);
	rpc.scope = FWCTL_RPC_CONFIGURATION;
	rpc.in_len = sizeof(req_hdr);
	rpc.out_len = out_size;
	rpc.in = (uint64_t)(uintptr_t)&req_hdr;
	rpc.out = (uint64_t)(uintptr_t)out_buf;

	if (ioctl(fd, FWCTL_RPC, &rpc)) {
		fprintf(stderr, "ioctl(FWCTL_RPC) %s (preflight): %s\n",
			cmd_name, strerror(errno));
		goto out;
	}

	ids = (NvU32 *)((uint8_t *)out_buf + sizeof(struct fwctl_rpc_nova_core));
	for (i = 0; i < QUERY_VGPU_TYPES_MAX; i++) {
		if (ids[i] == 0)
			break;
		if (ids[i] == type_id) {
			ret = 1;
			goto out;
		}
	}

	fprintf(stderr,
		"error: vGPU type %u (0x%x) is not in the %s list.\n"
		"       Run 'vgpu-mgmt list-%s' to see valid type IDs.\n",
		type_id, type_id, list_token, list_token);
	ret = 0;

out:
	free(out_buf);
	return ret;
}

int vgpu_type_is_supported(int fd, NvU32 type_id)
{
	return vgpu_type_in_query_list(
		fd, FWCTL_CMD_NOVA_CORE_GMCAPI_QUERY_SUPPORTED_VGPU_TYPES,
		"QUERY_SUPPORTED_VGPU_TYPES",
		type_id, "supported");
}

int vgpu_type_is_creatable(int fd, NvU32 type_id)
{
	return vgpu_type_in_query_list(
		fd, FWCTL_CMD_NOVA_CORE_GMCAPI_QUERY_CREATABLE_VGPU_TYPES,
		"QUERY_CREATABLE_VGPU_TYPES",
		type_id, "creatable");
}

/* Read $link_path, then copy the final '/'-separated component of the
 * resolved target into $out. Returns 0 on success, -1 (with an error
 * logged) on readlink failure, an empty target, or truncation. */
static int readlink_basename(const char *link_path, char *out, size_t out_sz)
{
	char target[256];
	const char *base;
	ssize_t n;

	n = readlink(link_path, target, sizeof(target) - 1);
	if (n <= 0) {
		fprintf(stderr, "readlink %s: %s\n",
			link_path, n < 0 ? strerror(errno) : "empty target");
		return -1;
	}
	target[n] = '\0';

	base = strrchr(target, '/');
	base = base ? base + 1 : target;

	if (snprintf(out, out_sz, "%s", base) >= (int)out_sz) {
		fprintf(stderr, "%s: target basename '%s' too long\n",
			link_path, base);
		return -1;
	}
	return 0;
}

/* Resolve $fd's underlying fwctl character device name (e.g. "fwctl0"),
 * by reading /proc/self/fd/<fd>. Returns 0 on success, -1 with an error
 * logged. */
static int fwctl_get_dev_name(int fd, char *out, size_t out_sz)
{
	char proc_path[64];

	snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd);
	return readlink_basename(proc_path, out, out_sz);
}

int fwctl_get_pci_device_id(int fd, NvU32 *out_device_id)
{
	char name[64];
	char sysfs_path[320];
	char buf[32];
	FILE *f;
	unsigned long device_id;

	if (fwctl_get_dev_name(fd, name, sizeof(name)))
		return -1;

	snprintf(sysfs_path, sizeof(sysfs_path),
		 "/sys/class/fwctl/%s/device/device", name);
	f = fopen(sysfs_path, "r");
	if (!f) {
		fprintf(stderr, "open %s: %s\n", sysfs_path, strerror(errno));
		return -1;
	}
	if (!fgets(buf, sizeof(buf), f)) {
		fprintf(stderr, "read %s: %s\n", sysfs_path, strerror(errno));
		fclose(f);
		return -1;
	}
	fclose(f);

	device_id = strtoul(buf, NULL, 16);
	*out_device_id = (NvU32)device_id;
	return 0;
}

/* Resolve $fd's underlying PF BDF (e.g. "0000:17:00.0") by readlinking
 * /sys/class/fwctl/<name>/device. Returns 0 on success, -1 with an
 * error logged. */
static int fwctl_get_pf_bdf(int fd, char *out_bdf, size_t out_sz)
{
	char name[64];
	char sysfs_link[320];

	if (fwctl_get_dev_name(fd, name, sizeof(name)))
		return -1;

	snprintf(sysfs_link, sizeof(sysfs_link),
		 "/sys/class/fwctl/%s/device", name);
	return readlink_basename(sysfs_link, out_bdf, out_sz);
}

int fwctl_check_vf_matches(int fd, NvU64 vf_dbdf)
{
	char fwctl_pf[16] = {0};
	char vf_pf[16] = {0};
	char vf_bdf[16];
	char physfn_link[320];

	if (fwctl_get_pf_bdf(fd, fwctl_pf, sizeof(fwctl_pf)))
		return -1;

	format_dbdf(vf_dbdf, vf_bdf, sizeof(vf_bdf));
	snprintf(physfn_link, sizeof(physfn_link),
		 "/sys/bus/pci/devices/%s/physfn", vf_bdf);
	if (readlink_basename(physfn_link, vf_pf, sizeof(vf_pf)))
		return -1;

	if (strcmp(fwctl_pf, vf_pf) != 0) {
		fprintf(stderr,
			"error: VF %s belongs to PF %s, but fwctl is bound to PF %s\n",
			vf_bdf, vf_pf, fwctl_pf);
		return -1;
	}
	return 0;
}

int vgpu_supported_list_is_empty(int fd)
{
	struct fwctl_rpc rpc = {0};
	struct fwctl_rpc_nova_core req_hdr = {0};
	uint32_t out_payload_size = QUERY_VGPU_TYPES_MAX * sizeof(NvU32);
	uint32_t out_size = sizeof(struct fwctl_rpc_nova_core) + out_payload_size;
	void *out_buf = NULL;
	NvU32 *ids;
	int ret = -1;
	int err;

	err = posix_memalign(&out_buf, 16, out_size);
	if (err) {
		fprintf(stderr, "posix_memalign failed: %d\n", err);
		goto out;
	}
	memset(out_buf, 0, out_size);

	req_hdr.command_id = FWCTL_CMD_NOVA_CORE_GMCAPI_QUERY_SUPPORTED_VGPU_TYPES;

	rpc.size = sizeof(rpc);
	rpc.scope = FWCTL_RPC_CONFIGURATION;
	rpc.in_len = sizeof(req_hdr);
	rpc.out_len = out_size;
	rpc.in = (uint64_t)(uintptr_t)&req_hdr;
	rpc.out = (uint64_t)(uintptr_t)out_buf;

	if (ioctl(fd, FWCTL_RPC, &rpc)) {
		fprintf(stderr, "ioctl(FWCTL_RPC) QUERY_SUPPORTED_VGPU_TYPES: %s\n",
			strerror(errno));
		goto out;
	}

	ids = (NvU32 *)((uint8_t *)out_buf + sizeof(struct fwctl_rpc_nova_core));
	ret = (ids[0] == 0) ? 1 : 0;

out:
	free(out_buf);
	return ret;
}
