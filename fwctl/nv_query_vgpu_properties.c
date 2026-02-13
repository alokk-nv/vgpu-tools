/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Query a vGPU type's properties via the fwctl nova-core interface.
 *
 * GMCAPI QUERY_VGPU_PROPERTIES:
 *   in:  fwctl_rpc_nova_core_request_hdr + GmcapiQueryVgpuPropertiesInParams
 *   out: fwctl_rpc_nova_core_resp_hdr + NVKV-encoded NvU64 stream
 *
 * The kernel handler (gmcapiQueryVgpuProperties → gmcapiNvkvEncodeGetVgpuType)
 * encodes the VGPU_TYPE record using the NVGMC_MGMT_GMCAPI_VGPU_* keys. We
 * decode the same keys here to surface a human-readable view.
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
#include "nvkv.h"
#include "nv_vgpu.h"
#include "fwctl_common.h"

/*
 * QUERY_VGPU_PROPERTIES (GMCAPI) NVKV response upper bound (matches
 * gmcapiNvkvEncodeGetVgpuType in vgpu_mgr.c).
 */
#define VGPUCONFIG_GMCAPI_QUERY_VGPU_NVKV_MAX_NAME_PAYLOAD_WORDS                          \
    (NV_ALIGN_UP64((NvU64)NVGMC_MGMT_GMCAPI_VGPU_STRING_BUFFER_SIZE * sizeof(NvU8),       \
                   sizeof(NvU64)) / sizeof(NvU64))
#define VGPUCONFIG_GMCAPI_QUERY_VGPU_NVKV_MAX_CLASS_PAYLOAD_WORDS                         \
    (NV_ALIGN_UP64((NvU64)NVGMC_MGMT_GMCAPI_VGPU_STRING_BUFFER_SIZE * sizeof(NvU8),       \
                   sizeof(NvU64)) / sizeof(NvU64))
#define VGPUCONFIG_GMCAPI_QUERY_VGPU_NVKV_MAX_OUT_WORDS                                   \
    (1 + 1 + VGPUCONFIG_GMCAPI_QUERY_VGPU_NVKV_MAX_NAME_PAYLOAD_WORDS +                   \
     1 + 1 + VGPUCONFIG_GMCAPI_QUERY_VGPU_NVKV_MAX_CLASS_PAYLOAD_WORDS +                  \
     9 /* SEQ32_1U fields */ +                                                            \
     5 * 2 /* SEQ64_1U fields */)

static void usage_query_props(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"  -d <path>     fwctl device path (default: auto-detect)\n"
		"  -t <type_id>  vGPU type ID to query\n"
		"  -h            show this help\n",
		prog);
}

static void print_vgpu_type(NvU32 vgpuTypeId, const nvidia_get_vgpu_properties *p)
{
	fprintf(stdout, "vGPU type %u properties:\n", vgpuTypeId);
	fprintf(stdout, "  %-16s = \"%s\"\n",          "vgpuName",       (const char *)p->vgpu_type_name);
	fprintf(stdout, "  %-16s = \"%s\"\n",          "vgpuClass",      (const char *)p->vgpuClass);
	fprintf(stdout, "  %-16s = %u (0x%x)\n",       "vgpuTypeId",     p->vgpuTypeId, p->vgpuTypeId);
	fprintf(stdout, "  %-16s = %u (0x%x)\n",       "maxInstance",    p->max_instance, p->max_instance);
	fprintf(stdout, "  %-16s = %u (0x%x)\n",       "eccSupported",   p->ecc_supported, p->ecc_supported);
	fprintf(stdout, "  %-16s = %u (0x%x)\n",       "frlConfig",      p->maxFps, p->maxFps);
	fprintf(stdout, "  %-16s = %u (0x%x)\n",       "numHeads",       p->numHeads, p->numHeads);
	fprintf(stdout, "  %-16s = %u (0x%x)\n",       "maxResolutionX", p->maxResolutionX, p->maxResolutionX);
	fprintf(stdout, "  %-16s = %u (0x%x)\n",       "maxResolutionY", p->maxResolutionY, p->maxResolutionY);
	fprintf(stdout, "  %-16s = %u (0x%x)\n",       "devId",          p->devId, p->devId);
	fprintf(stdout, "  %-16s = %u (0x%x)\n",       "subsystemId",    p->subSystemId, p->subSystemId);
	fprintf(stdout, "  %-16s = %llu (0x%llx)\n",   "bar1Length",     (unsigned long long)p->bar1_length,   (unsigned long long)p->bar1_length);
	fprintf(stdout, "  %-16s = %llu (0x%llx)\n",   "profileSize",    (unsigned long long)p->profileSize,   (unsigned long long)p->profileSize);
	fprintf(stdout, "  %-16s = %llu (0x%llx)\n",   "fbLength",       (unsigned long long)p->fbLength,      (unsigned long long)p->fbLength);
	fprintf(stdout, "  %-16s = %llu (0x%llx)\n",   "gspHeapSize",    (unsigned long long)p->gspHeapSize,   (unsigned long long)p->gspHeapSize);
	fprintf(stdout, "  %-16s = %llu (0x%llx)\n",   "fbReservation",  (unsigned long long)p->fbReservation, (unsigned long long)p->fbReservation);
}

static NvBool vgpu_props_key_handler(const NvU64 keyIndex, const NvU64 key,
				     const NVKVValue *pValue, void *ctx)
{
	nvidia_get_vgpu_properties *p = (nvidia_get_vgpu_properties *)ctx;

    #define NVKV_PREFIX NVGMC_MGMT

	NVKVDecodeContext dctx = NVKV_DECODE_INIT(keyIndex, key, pValue);
	NVKV_DECODE_SWITCH(&dctx);

		NVKV_CASE_STRING8(&dctx, GMCAPI_VGPU_TYPE_NAME, p->vgpu_type_name);
		NVKV_CASE_STRING8(&dctx, GMCAPI_VGPU_CLASS,     p->vgpuClass);

		NVKV_CASE_VAR(&dctx, U32, GMCAPI_VGPU_TYPE_ID,      p->vgpuTypeId);
		NVKV_CASE_VAR(&dctx, U32, GMCAPI_VGPU_MAX_INSTANCE, p->max_instance);
		NVKV_CASE_VAR(&dctx, U32, GMCAPI_VGPU_ECC,          p->ecc_supported);
		NVKV_CASE_VAR(&dctx, U32, GMCAPI_VGPU_MAX_FPS,      p->maxFps);
		NVKV_CASE_VAR(&dctx, U32, GMCAPI_VGPU_NUM_HEADS,    p->numHeads);
		NVKV_CASE_VAR(&dctx, U32, GMCAPI_VGPU_MAX_RES_X,    p->maxResolutionX);
		NVKV_CASE_VAR(&dctx, U32, GMCAPI_VGPU_MAX_RES_Y,    p->maxResolutionY);
		NVKV_CASE_VAR(&dctx, U32, GMCAPI_VGPU_DEV_ID,       p->devId);
		NVKV_CASE_VAR(&dctx, U32, GMCAPI_VGPU_SUBSYSTEM_ID, p->subSystemId);

		NVKV_CASE_VAR(&dctx, U64, GMCAPI_VGPU_BAR1_LENGTH,    p->bar1_length);
		NVKV_CASE_VAR(&dctx, U64, GMCAPI_VGPU_PROFILE_SIZE,   p->profileSize);
		NVKV_CASE_VAR(&dctx, U64, GMCAPI_VGPU_FB_LENGTH,      p->fbLength);
		NVKV_CASE_VAR(&dctx, U64, GMCAPI_VGPU_GSP_HEAP_SIZE,  p->gspHeapSize);
		NVKV_CASE_VAR(&dctx, U64, GMCAPI_VGPU_FB_RESERVATION, p->fbReservation);

	NVKV_DECODE_SWITCH_END(&dctx);

    #undef NVKV_PREFIX

	NV_PRINTF(LEVEL_ERROR, "Unexpected NVGMC_MGMT key 0x%04x: index: 0x%04x, type: %d, count: %d\n",
		  (NvU32)key, (NvU32)keyIndex, (NvU32)pValue->valueType, (NvU32)pValue->valueCount);
	return NV_TRUE;
}


/*
 * Decode an NVKV-encoded NvU64 stream into the destination record.
 * Returns 0 on success, -1 on a malformed buffer (truncated payload or
 * unknown opcode).
 */
static int decode_nvkv(const NvU64 *kv, NvU32 word_count, nvidia_get_vgpu_properties *out)
{
	return nvkvDecode(vgpu_props_key_handler, kv, word_count, out) == NV_OK
		? 0 : -1;
}

int cmd_query_props(const char *prog, int argc, char **argv)
{
	const char *dev_path = NULL;
	int opt, fd = -1, ret = EXIT_FAILURE;
	struct fwctl_rpc rpc = {0};
	struct fwctl_rpc_nova_core *req_hdr;
	struct fwctl_rpc_nova_core *resp_hdr;
	struct GmcapiQueryVgpuPropertiesInParams *in_params;
	uint32_t in_size = sizeof(*req_hdr) + sizeof(*in_params);
	uint32_t out_payload_size = VGPUCONFIG_GMCAPI_QUERY_VGPU_NVKV_MAX_OUT_WORDS * sizeof(NvU64);
	uint32_t out_size = sizeof(*resp_hdr) + out_payload_size;
	void *in_buf = NULL;
	void *out_buf = NULL;
	NvU32 vgpu_type_id = 0;
	int got_type = 0;

	while ((opt = getopt(argc, argv, "d:t:h")) != -1) {
		switch (opt) {
		case 'd':
			dev_path = optarg;
			break;
		case 't':
			if (parse_u32("-t", optarg, &vgpu_type_id))
				return EXIT_FAILURE;
			got_type = 1;
			break;
		case 'h':
		default:
			usage_query_props(prog);
			return opt == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
		}
	}

	if (reject_extra_args(prog, argc, argv))
		return EXIT_FAILURE;

	if (!got_type) {
		fprintf(stderr, "error: -t <type_id> is required\n\n");
		usage_query_props(prog);
		return EXIT_FAILURE;
	}

	fprintf(stderr, "VGPU TYPE ID: %u (0x%x)\n",
		vgpu_type_id, vgpu_type_id);

	if (dev_path)
		fd = open_fwctl_device(dev_path);
	else
		fd = open_nova_core_fwctl();

	if (fd < 0) {
		fprintf(stderr, "no nova-core fwctl device found\n");
		return EXIT_FAILURE;
	}

	fprintf(stderr, "fwctl device opened\n");

	{
		int sup = vgpu_type_is_supported(fd, vgpu_type_id);
		if (sup <= 0) {
			ret = EXIT_FAILURE;
			goto out;
		}
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
	req_hdr->command_id = FWCTL_CMD_NOVA_CORE_GMCAPI_QUERY_VGPU_PROPERTIES;

	in_params = (struct GmcapiQueryVgpuPropertiesInParams *)
		((uint8_t *)in_buf + sizeof(*req_hdr));

	in_params->vgpuTypeId = vgpu_type_id;

	rpc.size = sizeof(rpc);
	rpc.scope = FWCTL_RPC_CONFIGURATION;
	rpc.in_len = in_size;
	rpc.out_len = out_size;
	rpc.in = (uint64_t)(uintptr_t)in_buf;
	rpc.out = (uint64_t)(uintptr_t)out_buf;

	if (ioctl(fd, FWCTL_RPC, &rpc)) {
		perror("ioctl(FWCTL_RPC) QUERY_VGPU_PROPERTIES");
		ret = EXIT_FAILURE;
		goto out;
	}

	resp_hdr = out_buf;

	{
		const NvU64 *kv = (const NvU64 *)((uint8_t *)out_buf + sizeof(*resp_hdr));
		NvU32 kv_words = VGPUCONFIG_GMCAPI_QUERY_VGPU_NVKV_MAX_OUT_WORDS;
		nvidia_get_vgpu_properties props = { 0 };

		if (decode_nvkv(kv, kv_words, &props) != 0) {
			fprintf(stderr, "warning: nvkv decode hit malformed payload\n");
			ret = EXIT_FAILURE;
			goto out;
		}

		/* A successful decode of a zero-key stream leaves props zero.
		 * Since the supported preflight already passed, this is an
		 * inconsistent GSP state -- bail before printing 15 lines of
		 * zeros that look like real properties. */
		if (props.vgpuTypeId == 0) {
			fprintf(stderr,
				"error: GSP returned no properties for vGPU type %u (0x%x).\n"
				"       The type is in the supported list but the property\n"
				"       stream was empty -- GSP state may be inconsistent.\n",
				vgpu_type_id, vgpu_type_id);
			ret = EXIT_FAILURE;
			goto out;
		}

		print_vgpu_type(vgpu_type_id, &props);
	}

	fprintf(stderr, "QUERY_VGPU_PROPERTIES: OK\n");
	ret = EXIT_SUCCESS;

out:
	free(in_buf);
	free(out_buf);
	if (fd >= 0)
		close(fd);
	return ret;
}
