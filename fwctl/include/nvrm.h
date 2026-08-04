/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * NVRM-style fixed-width typedefs (NvU8/NvU16/NvU32/NvU64/NvBool) and the
 * fwctl_cmd_nova_core opcode enum used by the userspace fwctl tools to
 * talk to the nova-core GMCAPI RPC surface.
 */

#ifndef NVRM_H
#define NVRM_H

#include <stdint.h>

#ifndef __NVRM_NVTYPES_H__
typedef uint64_t NvU64;
typedef uint32_t NvU32;
typedef uint16_t NvU16;
typedef uint8_t NvBool;
typedef uint8_t NvU8;
#endif

enum fwctl_cmd_nova_core {
	/*
	 * Add a single vGPU type.
	 * Wire: fwctl_rpc_nova_core + GMC add-vGPU-type payload IN
	 *       fwctl_rpc_nova_core OUT.
	 */
	FWCTL_CMD_NOVA_CORE_GMCAPI_ADD_VGPU_TYPE = 0x00020001,
	/*
	 * Query supported vGPU type IDs.
	 * Wire: fwctl_rpc_nova_core (no payload) IN;
	 *       fwctl_rpc_nova_core + NvU32[] type IDs OUT.
	 * Valid count N = outParamSize / sizeof(NvU32).
	 */
	FWCTL_CMD_NOVA_CORE_GMCAPI_QUERY_SUPPORTED_VGPU_TYPES = 0x00020002,
	/*
	 * Query creatable vGPU type IDs.
	 * Wire: fwctl_rpc_nova_core (no payload) IN;
	 *       fwctl_rpc_nova_core + NvU32[] type IDs OUT.
	 * Valid count N = outParamSize / sizeof(NvU32).
	 */
	FWCTL_CMD_NOVA_CORE_GMCAPI_QUERY_CREATABLE_VGPU_TYPES = 0x00020003,
	/*
	 * Assign a vGPU type to a VF.
	 * Wire: fwctl_rpc_nova_core + GmcapiAssignVgpuTypeInParams IN;
	 *       fwctl_rpc_nova_core (no payload) OUT.
	 */
	FWCTL_CMD_NOVA_CORE_GMCAPI_ASSIGN_VGPU_TYPE = 0x00020004,
	/*
	 * Deassign a vGPU type from a VF.
	 * Wire: fwctl_rpc_nova_core + GmcapiDeassignVgpuTypeInParams IN;
	 *       fwctl_rpc_nova_core (no payload) OUT.
	 */
	FWCTL_CMD_NOVA_CORE_GMCAPI_DEASSIGN_VGPU_TYPE = 0x00020005,
	/*
	 * Query the properties of a vGPU type.
	 * Wire: fwctl_rpc_nova_core + GmcapiQueryVgpuPropertiesInParams IN;
	 *       fwctl_rpc_nova_core + NVKV-encoded NvU64 stream OUT.
	 *       Valid byte count = outParamSize; decode keys with NVGMC_MGMT prefix.
	 */
	FWCTL_CMD_NOVA_CORE_GMCAPI_QUERY_VGPU_PROPERTIES = 0x00020006,
	/*
	 * Query the vGPU type currently assigned to a VF.
	 * Wire: fwctl_rpc_nova_core + GmcapiQueryAssignedVfVgpuTypeInParams IN;
	 *       fwctl_rpc_nova_core + GmcapiQueryAssignedVfVgpuTypeOutParams OUT.
	 */
	FWCTL_CMD_NOVA_CORE_GMCAPI_QUERY_ASSIGNED_VF_VGPU_TYPE = 0x00020007,
};

/* Matches include/uapi/fwctl/nova-core.h in the kernel uAPI. */
struct fwctl_rpc_nova_core {
	uint32_t command_id;
	uint32_t reserved;
};

#endif
