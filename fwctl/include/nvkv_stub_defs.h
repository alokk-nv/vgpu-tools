/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Stub definitions for standalone nvkv build. Defines NV_ALIGN_UP64, REF_VAL64,
 * and NV_PRINTF when not provided by the full driver headers. All macros are
 * guarded with #ifndef so they do not override the real definitions when
 * building within the driver tree.
 */
#ifndef _NVKV_STUB_DEFS_H_
#define _NVKV_STUB_DEFS_H_
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define portMemCopy(p1, s1, p2, s2) memcpy(p1, p2, ((s1) > (s2)) ? (s2) : (s1))
#define portMemSet memset
#define portMemAllocNonPaged malloc
#define portMemFree free
#define portStringLengthSafe(s, n) strnlen((s), (n))
#define portStringCopy(dst, dst_size, src, src_count)                            \
    do {                                                                         \
        NvU64 _n = (src_count) < (dst_size) ? (src_count) : (NvU64)(dst_size) - 1; \
        memcpy((dst), (src), (size_t)_n);                                        \
        ((char *)(dst))[_n] = '\0';                                              \
    } while (0)

typedef uint32_t NvV32;
typedef uint8_t  NvU8;
typedef uint16_t NvU16;
typedef uint32_t NvU32;
typedef uint64_t NvU64;
typedef void*    NvP64;
typedef NvU8     NvBool;
typedef NvU32    NvHandle;
typedef NvU64    NvLength;
typedef NvU64    RmPhysAddr;
typedef NvU32    NV_STATUS;

#ifndef NV_U64_MAX
#define NV_U64_MAX 0xFFFFFFFFFFFFFFFFULL
#endif

/*
 * Bitfield extraction for REF_VAL64. For a field defined as HIGH:LOW (e.g. 15:0),
 * the C ternary (bitval!=0)?HIGH:LOW selects HIGH when bitval=1 and LOW when bitval=0.
 */
#ifndef DRF_ISBIT
#define DRF_ISBIT(bitval, drf) ((bitval) != 0 ? drf)
#endif

#ifndef DRF_SHIFT64
#define DRF_SHIFT64(drf) ((DRF_ISBIT(0, drf)) % 64U)
#endif

#ifndef DRF_MASK64
#define DRF_MASK64(drf) (NV_U64_MAX >> (63U - ((DRF_ISBIT(1, drf)) % 64U) + ((DRF_ISBIT(0, drf)) % 64U)))
#endif

#ifndef REF_VAL64
#define REF_VAL64(drf, v) (((NvU64)(v) >> DRF_SHIFT64(drf)) & DRF_MASK64(drf))
#endif

#ifndef REF_DEF64
#define REF_DEF64(drf, d) (((drf##d) & DRF_MASK64(drf)) << DRF_SHIFT64(drf))
#endif

#ifndef REF_NUM64
#define REF_NUM64(drf, n) ((((NvU64)(n) & DRF_MASK64(drf)) << DRF_SHIFT64(drf)))
#endif

#ifndef NV_ALIGN_UP64
#define NV_ALIGN_UP64(v, gran) (((v) + ((gran) - 1)) & ~(((NvU64)(gran) - 1)))
#endif

#ifndef NV_PRINTF
#define NV_PRINTF(level, format, ...) fprintf(stderr, format, ##__VA_ARGS__)
#endif

#ifndef NV_TRUE
#define NV_TRUE  1
#endif
#ifndef NV_FALSE
#define NV_FALSE 0
#endif

#ifndef NV_OK
#define NV_OK 0x00000000
#endif
#ifndef NV_ERR_OUT_OF_RANGE
#define NV_ERR_OUT_OF_RANGE 0x0000005B
#endif
#ifndef NV_ERR_INVALID_COMMAND
#define NV_ERR_INVALID_COMMAND 0x00000024
#endif
#ifndef NV_ERR_INVALID_ARGUMENT
#define NV_ERR_INVALID_ARGUMENT 0x0000001F
#endif
#ifndef NV_ERR_INSUFFICIENT_RESOURCES
#define NV_ERR_INSUFFICIENT_RESOURCES 0x0000001A
#endif
#ifndef NV_ERR_INVALID_DATA
#define NV_ERR_INVALID_DATA 0x00000025
#endif

#ifndef NV_ASSERT_OR_RETURN
#define NV_ASSERT_OR_RETURN(expr, retval) \
    do { if (!(expr)) return (retval); } while (0)
#endif

#ifndef NV_CHECK_OR_RETURN
#define NV_CHECK_OR_RETURN(level, expr, retval) \
    do { if (!(expr)) return (retval); } while (0)
#endif

#ifndef NV_ASSERT
#define NV_ASSERT(expr) \
    do { if (!(expr)) { fprintf(stderr, "Assertion failed: %s\n", #expr); abort(); } } while (0)
#endif

#ifndef MEM_WR64
#define MEM_WR64(addr, data) (*(NvU64 *)(addr) = (NvU64)(data))
#endif

#endif /* _NVKV_STUB_DEFS_H_ */
