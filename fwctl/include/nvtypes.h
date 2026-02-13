/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * NVRM fixed-width integer typedefs (NvU8/NvU16/NvU32/NvU64/NvV32) and
 * alignment helper macros shared by the userspace fwctl tools.
 */
#ifndef __NVRM_NVTYPES_H__
#define __NVRM_NVTYPES_H__

#include <stdint.h>

#define NV_ALIGN_BYTES(a) __attribute__ ((__aligned__(a)))
#define NV_DECLARE_ALIGNED(f, a) f __attribute__ ((__aligned__(a)))

typedef uint32_t NvV32;

typedef uint8_t NvU8;
typedef uint16_t NvU16;
typedef uint32_t NvU32;
typedef uint64_t NvU64;

typedef void* NvP64;

typedef NvU8 NvBool;
typedef NvU32 NvHandle;
typedef NvU64 NvLength;

typedef NvU64 RmPhysAddr;

typedef NvU32 NV_STATUS;

typedef union {} rpc_generic_union;
#endif
