/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * NVKV decoder: walks an NvU64 stream encoded per the NVKV format defined in
 * nvkv.h, dispatching each key/value (IMM32, SEQ*, ARRAY*) to a caller-
 * supplied handler. Used by vgpu-mgmt to parse NVKV-encoded RPC responses
 * such as the GMCAPI query-vGPU-properties output.
 */

#include "nvkv.h"

NV_STATUS nvkvDecode(NVKVKeyHandler keyHandler, const NvU64 *pKV, const NvU32 maxKVCount, void *const ctx)
{
    NvU64 kvIndex = 0;

    // It's ok/safe to call nvkvDecode with NULL if count is zero.
    NV_ASSERT_OR_RETURN(maxKVCount == 0 || pKV != NULL, NV_ERR_INVALID_ARGUMENT);
    NV_ASSERT_OR_RETURN(keyHandler != NULL, NV_ERR_INVALID_ARGUMENT);

    while (kvIndex < maxKVCount)
    {
        NvU64 kvData = pKV[kvIndex++];  // Always consume first value.

        /* Extract fields common to all opcode types */
        NvU64 key = REF_VAL64(NVKV_KEY, kvData);
        NvU64 opcode = REF_VAL64(NVKV_OPCODE, kvData);
        NvU64 index = REF_VAL64(NVKV_IDX12, kvData);

        switch (opcode)
        {
            case NVKV_OPCODE_IMM32:
            {
                NvU32 value = REF_VAL64(NVKV_IMM32_VALUE, kvData);
                NVKVValue currentValue;
                currentValue.valueType = NVKV_VALUE_TYPE_U32;
                currentValue.valueCount = 1;
                currentValue.valueData.pU32 = &value;
                if (keyHandler(index, key, &currentValue, ctx) == NV_FALSE)
                {
                    return NV_OK;
                }
                break;
            }

            case NVKV_OPCODE_SEQ32:
            {
                NvU64 valueCount = REF_VAL64(NVKV_COUNT32, kvData);
                NvU64 count64 = NV_ALIGN_UP64(valueCount*sizeof(NvU32), sizeof(NvU64)) / sizeof(NvU64);
                NvU64 limit = kvIndex+count64;
                NvU32 valueIndex = 0;

                if (limit > maxKVCount)
                {
                    return NV_ERR_OUT_OF_RANGE;
                }

                const NvU32 *pValue32 = (const NvU32 *)&pKV[kvIndex];
                while (valueIndex < valueCount)
                {
                    NVKVValue currentValue;
                    currentValue.valueType = NVKV_VALUE_TYPE_U32;
                    currentValue.valueCount = 1;
                    currentValue.valueData.pU32 = &pValue32[valueIndex];
                    if (keyHandler(index, key, &currentValue, ctx) == NV_FALSE)
                    {
                        return NV_OK;
                    }
                    valueIndex++;
                    key++;
                }
                kvIndex += count64;
                break;
            }

            case NVKV_OPCODE_SEQ64:
            {
                NvU64 valueCount = REF_VAL64(NVKV_COUNT32, kvData);
                NvU64 limit = kvIndex+valueCount;
                NvU32 valueIndex = 0;

                if (limit > maxKVCount)
                {
                    return NV_ERR_OUT_OF_RANGE;
                }

                const NvU64 *pValue64 = (const NvU64 *)&pKV[kvIndex];
                while (valueIndex < valueCount)
                {
                    NVKVValue currentValue;
                    currentValue.valueType = NVKV_VALUE_TYPE_U64;
                    currentValue.valueCount = 1;
                    currentValue.valueData.pU64 = &pValue64[valueIndex];
                    if (keyHandler(index, key, &currentValue, ctx) == NV_FALSE)
                    {
                        return NV_OK;
                    }
                    valueIndex++;
                    key++;
                }
                kvIndex += valueCount;
                break;
            }

            case NVKV_OPCODE_ARRAY8:
            {
                NvU64 valueCount = REF_VAL64(NVKV_COUNT32, kvData);
                NvU64 count64 = NV_ALIGN_UP64(valueCount*sizeof(NvU8), sizeof(NvU64)) / sizeof(NvU64);
                NvU64 limit = kvIndex+count64;

                if (limit > maxKVCount)
                {
                    return NV_ERR_OUT_OF_RANGE;
                }

                // If count is 0 we pass a NULL pointer since the callee should not access memory.
                NVKVValue currentValue;
                currentValue.valueType = NVKV_VALUE_TYPE_U8;
                currentValue.valueCount = (NvU32)valueCount;
                currentValue.valueData.pU8 = valueCount ? (const NvU8 *)&pKV[kvIndex] : NULL;
                if (keyHandler(index, key, &currentValue, ctx) == NV_FALSE)
                {
                    return NV_OK;
                }
                kvIndex += count64;
                break;
            }

            case NVKV_OPCODE_ARRAY32:
            {
                NvU64 valueCount = REF_VAL64(NVKV_COUNT32, kvData);
                NvU64 count64 = NV_ALIGN_UP64(valueCount*sizeof(NvU32), sizeof(NvU64)) / sizeof(NvU64);
                NvU64 limit = kvIndex+count64;

                if (limit > maxKVCount)
                {
                    return NV_ERR_OUT_OF_RANGE;
                }

                // If count is 0 we pass a NULL pointer since the callee should not access memory.
                NVKVValue currentValue;
                currentValue.valueType = NVKV_VALUE_TYPE_U32;
                currentValue.valueCount = (NvU32)valueCount;
                currentValue.valueData.pU32 = valueCount ? (const NvU32 *)&pKV[kvIndex] : NULL;
                if (keyHandler(index, key, &currentValue, ctx) == NV_FALSE)
                {
                    return NV_OK;
                }
                kvIndex += count64;
                break;
            }

            case NVKV_OPCODE_ARRAY64:
            {
                NvU64 valueCount = REF_VAL64(NVKV_COUNT32, kvData);
                NvU64 limit = kvIndex+valueCount;

                if (limit > maxKVCount)
                {
                    return NV_ERR_OUT_OF_RANGE;
                }

                // If count is 0 we pass a NULL pointer since the callee should not access memory.
                NVKVValue currentValue;
                currentValue.valueType = NVKV_VALUE_TYPE_U64;
                currentValue.valueCount = (NvU32)valueCount;
                currentValue.valueData.pU64 = valueCount ? (const NvU64 *)&pKV[kvIndex] : NULL;
                if (keyHandler(index, key, &currentValue, ctx) == NV_FALSE)
                {
                    return NV_OK;
                }
                kvIndex += valueCount;
                break;
            }

            /* If we land here then either the opcode was invalid */
            default:
            {
                NV_PRINTF(LEVEL_ERROR,"nvkvDecode() invalid instruction: %016llx\n",(unsigned long long)kvData);
                return NV_ERR_INVALID_COMMAND;
            }
        }
    }

    return NV_OK;
}
