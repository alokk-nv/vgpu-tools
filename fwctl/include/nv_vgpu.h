/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Userspace mirror of GMCAPI vGPU wire types and shared helpers used by the
 * fwctl userspace tools. The struct layouts here must match the kernel-side
 * definitions in drivers/resman/interface/gmcapi/gmcapi_vgpu.h.
 */

#ifndef FWCTL_NV_VGPU_H
#define FWCTL_NV_VGPU_H

#include "nvrm.h"

/* Input of GMCAPI ASSIGN_VGPU_TYPE. */
struct GmcapiAssignVgpuTypeInParams {
	NvU64 dbdf;
	NvU32 vgpuTypeId;
	NvU32 swizzId;
	NvU16 placementId;
};

/* Input of GMCAPI DEASSIGN_VGPU_TYPE. */
struct GmcapiDeassignVgpuTypeInParams {
	NvU64 dbdf;
};

/* Input of GMCAPI QUERY_ASSIGNED_VF_VGPU_TYPE. VF identified by dbdf. */
struct GmcapiQueryAssignedVfVgpuTypeInParams {
	NvU64 dbdf;
};

/* Output of GMCAPI QUERY_ASSIGNED_VF_VGPU_TYPE. */
struct GmcapiQueryAssignedVfVgpuTypeOutParams {
	NvU32 vgpuTypeId;
	NvU32 swizzId;     /* Reserved for future MIG support. */
	NvU16 placementId; /* Reserved for future placement support. */
};

/* Input of GMCAPI QUERY_VGPU_PROPERTIES. */
struct GmcapiQueryVgpuPropertiesInParams {
	NvU32 vgpuTypeId;
};

/*
 * NVKV keys returned by GMCAPI QUERY_VGPU_PROPERTIES. Mirrors definitions in
 * drivers/resman/interface/gmcapi/gmcapi_vgpu.h.
 */
#define NVGMC_MGMT_GMCAPI_VGPU_STRING_BUFFER_SIZE     64

/*
 * Decoded form of the GMCAPI QUERY_VGPU_PROPERTIES NVKV stream — one slot per
 * key emitted by gmcapiNvkvEncodeGetVgpuType in vgpu_mgr.c.
 */
typedef struct nvidia_get_vgpu_properties
{
	NvU32 vgpuTypeId;
	NvU8  vgpu_type_name[NVGMC_MGMT_GMCAPI_VGPU_STRING_BUFFER_SIZE];
	NvU8  vgpuClass[NVGMC_MGMT_GMCAPI_VGPU_STRING_BUFFER_SIZE];
	NvU64 bar1_length;
	NvU32 max_instance;
	NvU32 ecc_supported;
	NvU64 profileSize;
	NvU32 maxFps;
	NvU32 numHeads;
	NvU32 maxResolutionX;
	NvU32 maxResolutionY;
	NvU32 devId;
	NvU32 subSystemId;
	NvU64 fbLength;
	NvU64 gspHeapSize;
	NvU64 fbReservation;
} nvidia_get_vgpu_properties;

#define NVGMC_MGMT_GMCAPI_VGPU_TYPE_NAME              0x3100
#define NVGMC_MGMT_GMCAPI_VGPU_CLASS                  0x3101
#define NVGMC_MGMT_GMCAPI_VGPU_TYPE_ID                0x3102
#define NVGMC_MGMT_GMCAPI_VGPU_BAR1_LENGTH            0x3103
#define NVGMC_MGMT_GMCAPI_VGPU_MAX_INSTANCE           0x3104
#define NVGMC_MGMT_GMCAPI_VGPU_ECC                    0x3105
#define NVGMC_MGMT_GMCAPI_VGPU_PROFILE_SIZE           0x3106
#define NVGMC_MGMT_GMCAPI_VGPU_MAX_FPS                0x3107
#define NVGMC_MGMT_GMCAPI_VGPU_NUM_HEADS              0x3108
#define NVGMC_MGMT_GMCAPI_VGPU_MAX_RES_X              0x3109
#define NVGMC_MGMT_GMCAPI_VGPU_MAX_RES_Y              0x310A
#define NVGMC_MGMT_GMCAPI_VGPU_DEV_ID                 0x310B
#define NVGMC_MGMT_GMCAPI_VGPU_SUBSYSTEM_ID           0x310C
#define NVGMC_MGMT_GMCAPI_VGPU_FB_LENGTH              0x310D
#define NVGMC_MGMT_GMCAPI_VGPU_GSP_HEAP_SIZE          0x310E
#define NVGMC_MGMT_GMCAPI_VGPU_FB_RESERVATION         0x310F

#endif /* FWCTL_NV_VGPU_H */
