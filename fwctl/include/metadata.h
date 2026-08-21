/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Parser interface for vGPU metadata. File and group header integers are
 * serialized in little-endian order; vGPU type blobs remain opaque.
 */

#ifndef VGPU_METADATA_H
#define VGPU_METADATA_H

#include <stddef.h>
#include <stdint.h>

#define VGPU_METADATA_MAGIC                    "VGPUMETA"
#define VGPU_METADATA_MAGIC_SIZE               8U
#define VGPU_METADATA_FORMAT_MAJOR             1U
#define VGPU_METADATA_FORMAT_MINOR             0U
#define VGPU_METADATA_GSP_VERSION_SIZE         128U

#define VGPU_METADATA_MAGIC_OFFSET             0U
#define VGPU_METADATA_MAJOR_OFFSET             8U
#define VGPU_METADATA_MINOR_OFFSET             10U
#define VGPU_METADATA_HEADER_SIZE_OFFSET       12U
#define VGPU_METADATA_TOTAL_SIZE_OFFSET        16U
#define VGPU_METADATA_GROUP_COUNT_OFFSET       24U
#define VGPU_METADATA_CRC32_OFFSET             28U
#define VGPU_METADATA_GSP_VERSION_OFFSET       32U
#define VGPU_METADATA_HEADER_SIZE              160U

#define VGPU_METADATA_GROUP_TYPE_OFFSET        0U
#define VGPU_METADATA_GROUP_VERSION_OFFSET     4U
#define VGPU_METADATA_GROUP_HEADER_SIZE_OFFSET 6U
#define VGPU_METADATA_GROUP_SIZE_OFFSET        8U
#define VGPU_METADATA_GROUP_HEADER_SIZE        16U

#define VGPU_METADATA_GROUP_TYPE_INVALID       0U
#define VGPU_METADATA_GROUP_TYPE_VGPU_TYPE     1U

#define VGPU_METADATA_VGPU_TYPE_VERSION                1U
#define VGPU_METADATA_VGPU_VENDOR_ID_OFFSET            16U
#define VGPU_METADATA_VGPU_DEVICE_ID_OFFSET            18U
#define VGPU_METADATA_VGPU_SUBSYSTEM_VENDOR_ID_OFFSET  20U
#define VGPU_METADATA_VGPU_SUBSYSTEM_ID_OFFSET         22U
#define VGPU_METADATA_VGPU_RECORD_SIZE_OFFSET          24U
#define VGPU_METADATA_VGPU_RECORD_COUNT_OFFSET         28U
#define VGPU_METADATA_VGPU_HEADER_SIZE                 32U

#define VGPU_METADATA_NVIDIA_PCI_VENDOR_ID     0x10deU
#define VGPU_METADATA_MAX_GROUPS               1024U
#define VGPU_METADATA_MAX_FILE_SIZE            (1024U * 1024U * 1024U)
#define VGPU_METADATA_MAX_VGPU_TYPES           128U

/*
 * Validate the complete file and return the vgpu_type group matching the
 * supplied PCI IDs. Validation always reaches EOF before a successful result
 * is returned, so callers can upload only after this call.
 */
int metadata_find_vgpu_type_group(const void *data, size_t size,
				  uint16_t vendor_id, uint16_t device_id,
				  uint16_t subsystem_vendor_id,
				  uint16_t subsystem_id,
				  const uint8_t **gsp_build_version,
				  const uint8_t **records,
				  uint32_t *record_size,
				  uint32_t *record_count);

/* CRC-32/ISO-HDLC over the whole file with the CRC field treated as zero. */
uint32_t metadata_compute_crc32(const uint8_t *data, size_t size);

#endif /* VGPU_METADATA_H */
