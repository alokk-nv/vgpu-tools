/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * On-disk layout for the vGPU metadata blob consumed by `vgpu-mgmt add-type`:
 * fixed header (identifier + CRC32 + version + GSP build) followed by a
 * sequence of typed config blobs.
 */

#ifndef __NVIDIA_VGPU_METADATA_H__
#define __NVIDIA_VGPU_METADATA_H__

#define METADATA_IDR "NVVGPUMT"

enum {
	CONFIG_BLOB_VGPU_TYPE = 0,
	CONFIG_BLOB_MAX,
};

#define GSP_MAX_BUILD_VERSION_LENGTH (0x0000040)

struct metadata_hdr {
	uint64_t identifier; /* "NVVGPUMT" */
	uint32_t crc32;
	uint32_t padding;
	uint64_t vgpu_major;
	uint64_t vgpu_minor;
	uint8_t gsp_build_version[GSP_MAX_BUILD_VERSION_LENGTH];
	uint64_t num_blobs;
	unsigned char data[];
};

struct metadata_blob_hdr {
	uint64_t type;
	uint64_t size;
	uint64_t device_id;
	unsigned char data[]; /* blob payload */
};

#define POLY 0xEDB88320

static uint32_t crc32_le(uint32_t crc, const uint8_t *buf, size_t len) {
	for (size_t i = 0; i < len; i++) {
		crc ^= buf[i];
		for (int j = 0; j < 8; j++)
			crc = (crc >> 1) ^ (crc & 1 ? POLY : 0);
	}
	return crc;
}

#endif
