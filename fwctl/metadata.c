/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "metadata.h"

#define METADATA_CRC32_POLYNOMIAL 0xedb88320U

static uint16_t metadata_get_le16(const uint8_t *data)
{
	return (uint16_t)data[0] |
	       ((uint16_t)data[1] << 8);
}

static uint32_t metadata_get_le32(const uint8_t *data)
{
	return (uint32_t)data[0] |
	       ((uint32_t)data[1] << 8) |
	       ((uint32_t)data[2] << 16) |
	       ((uint32_t)data[3] << 24);
}

static uint64_t metadata_get_le64(const uint8_t *data)
{
	return (uint64_t)metadata_get_le32(data) |
	       ((uint64_t)metadata_get_le32(data + 4) << 32);
}

uint32_t metadata_compute_crc32(const uint8_t *data, size_t size)
{
	uint32_t crc = 0xffffffffU;
	size_t i;

	for (i = 0; i < size; i++) {
		uint8_t byte = data[i];
		unsigned int bit;

		if (i >= VGPU_METADATA_CRC32_OFFSET &&
		    i < VGPU_METADATA_CRC32_OFFSET + sizeof(uint32_t))
			byte = 0;

		crc ^= byte;
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^
			      ((crc & 1U) ? METADATA_CRC32_POLYNOMIAL : 0U);
	}

	return crc ^ 0xffffffffU;
}

static int metadata_validate_gsp_build_version(const uint8_t *version)
{
	const uint8_t *terminator;
	const uint8_t *cursor;

	terminator = memchr(version, '\0', VGPU_METADATA_GSP_VERSION_SIZE);
	if (!terminator || terminator == version) {
		fprintf(stderr, "metadata has an invalid GSP build version\n");
		return -1;
	}

	for (cursor = terminator + 1;
	     cursor < version + VGPU_METADATA_GSP_VERSION_SIZE; cursor++) {
		if (*cursor) {
			fprintf(stderr,
				"metadata GSP build version has nonzero trailing bytes\n");
			return -1;
		}
	}

	return 0;
}

int metadata_find_vgpu_type_group(const void *data, size_t size,
				  uint16_t vendor_id, uint16_t device_id,
				  uint16_t subsystem_vendor_id,
				  uint16_t subsystem_id,
				  const uint8_t **gsp_build_version,
				  const uint8_t **records,
				  uint32_t *record_size,
				  uint32_t *record_count)
{
	const uint8_t *file_data = data;
	const uint8_t *match_records = NULL;
	uint16_t format_major;
	uint16_t format_minor;
	uint32_t header_size;
	uint64_t total_size;
	uint32_t group_count;
	uint32_t stored_crc;
	uint32_t computed_crc;
	uint32_t match_record_size = 0;
	uint32_t match_record_count = 0;
	size_t offset;
	uint32_t i;

	if (!data || !gsp_build_version || !records || !record_size ||
	    !record_count) {
		fprintf(stderr, "invalid metadata parser arguments\n");
		return -1;
	}
	*gsp_build_version = NULL;
	*records = NULL;
	*record_size = 0;
	*record_count = 0;

	if (size < VGPU_METADATA_HEADER_SIZE) {
		fprintf(stderr, "metadata is too small for the file header\n");
		return -1;
	}
	if (size > VGPU_METADATA_MAX_FILE_SIZE) {
		fprintf(stderr, "metadata exceeds the maximum supported file size\n");
		return -1;
	}
	if (memcmp(file_data + VGPU_METADATA_MAGIC_OFFSET,
		   VGPU_METADATA_MAGIC, VGPU_METADATA_MAGIC_SIZE)) {
		fprintf(stderr,
			"unsupported metadata magic (expected VGPUMETA)\n");
		return -1;
	}

	format_major = metadata_get_le16(
		file_data + VGPU_METADATA_MAJOR_OFFSET);
	format_minor = metadata_get_le16(
		file_data + VGPU_METADATA_MINOR_OFFSET);
	header_size = metadata_get_le32(
		file_data + VGPU_METADATA_HEADER_SIZE_OFFSET);
	total_size = metadata_get_le64(
		file_data + VGPU_METADATA_TOTAL_SIZE_OFFSET);
	group_count = metadata_get_le32(
		file_data + VGPU_METADATA_GROUP_COUNT_OFFSET);
	stored_crc = metadata_get_le32(
		file_data + VGPU_METADATA_CRC32_OFFSET);

	if (format_major != VGPU_METADATA_FORMAT_MAJOR ||
	    format_minor != VGPU_METADATA_FORMAT_MINOR) {
		fprintf(stderr, "unsupported metadata format version %u.%u\n",
			format_major, format_minor);
		return -1;
	}
	if (header_size != VGPU_METADATA_HEADER_SIZE) {
		fprintf(stderr, "metadata header size is %u, expected %u\n",
			header_size, VGPU_METADATA_HEADER_SIZE);
		return -1;
	}
	if (total_size != size) {
		fprintf(stderr,
			"metadata declared size is %" PRIu64
			", actual file size is %zu\n",
			total_size, size);
		return -1;
	}
	if (!group_count || group_count > VGPU_METADATA_MAX_GROUPS) {
		fprintf(stderr, "metadata has invalid group count %u\n",
			group_count);
		return -1;
	}

	computed_crc = metadata_compute_crc32(file_data, size);
	if (stored_crc != computed_crc) {
		fprintf(stderr,
			"metadata CRC32 mismatch: stored=0x%08x computed=0x%08x\n",
			stored_crc, computed_crc);
		return -1;
	}
	if (metadata_validate_gsp_build_version(
		    file_data + VGPU_METADATA_GSP_VERSION_OFFSET))
		return -1;

	offset = header_size;
	for (i = 0; i < group_count; i++) {
		const uint8_t *group;
		size_t remaining;
		uint32_t group_type;
		uint16_t group_version;
		uint16_t group_header_size;
		uint64_t group_size;

		if (offset > size) {
			fprintf(stderr, "group[%u]: offset is past end of file\n", i);
			return -1;
		}
		remaining = size - offset;
		if (remaining < VGPU_METADATA_GROUP_HEADER_SIZE) {
			fprintf(stderr, "group[%u]: truncated common header\n", i);
			return -1;
		}

		group = file_data + offset;
		group_type = metadata_get_le32(
			group + VGPU_METADATA_GROUP_TYPE_OFFSET);
		group_version = metadata_get_le16(
			group + VGPU_METADATA_GROUP_VERSION_OFFSET);
		group_header_size = metadata_get_le16(
			group + VGPU_METADATA_GROUP_HEADER_SIZE_OFFSET);
		group_size = metadata_get_le64(
			group + VGPU_METADATA_GROUP_SIZE_OFFSET);

		if (group_type == VGPU_METADATA_GROUP_TYPE_INVALID) {
			fprintf(stderr, "group[%u]: invalid group type 0\n", i);
			return -1;
		}
		if (group_header_size < VGPU_METADATA_GROUP_HEADER_SIZE ||
		    group_size < group_header_size || group_size > remaining) {
			fprintf(stderr, "group[%u]: invalid header or group size\n", i);
			return -1;
		}

		if (group_type == VGPU_METADATA_GROUP_TYPE_VGPU_TYPE) {
			uint16_t group_vendor_id;
			uint16_t group_device_id;
			uint16_t group_subsystem_vendor_id;
			uint16_t group_subsystem_id;
			uint32_t record_size;
			uint32_t record_count;
			uint64_t expected_group_size;

			if (group_version != VGPU_METADATA_VGPU_TYPE_VERSION) {
				fprintf(stderr,
					"group[%u]: unsupported vGPU type group version %u\n",
					i, group_version);
				return -1;
			}
			if (group_header_size != VGPU_METADATA_VGPU_HEADER_SIZE) {
				fprintf(stderr,
					"group[%u]: vGPU type header size is %u, expected %u\n",
					i, group_header_size,
					VGPU_METADATA_VGPU_HEADER_SIZE);
				return -1;
			}

			group_vendor_id = metadata_get_le16(
				group + VGPU_METADATA_VGPU_VENDOR_ID_OFFSET);
			group_device_id = metadata_get_le16(
				group + VGPU_METADATA_VGPU_DEVICE_ID_OFFSET);
			group_subsystem_vendor_id = metadata_get_le16(
				group +
				VGPU_METADATA_VGPU_SUBSYSTEM_VENDOR_ID_OFFSET);
			group_subsystem_id = metadata_get_le16(
				group + VGPU_METADATA_VGPU_SUBSYSTEM_ID_OFFSET);
			record_size = metadata_get_le32(
				group + VGPU_METADATA_VGPU_RECORD_SIZE_OFFSET);
			record_count = metadata_get_le32(
				group + VGPU_METADATA_VGPU_RECORD_COUNT_OFFSET);

			if (group_vendor_id !=
				    VGPU_METADATA_NVIDIA_PCI_VENDOR_ID ||
			    group_subsystem_vendor_id !=
				    VGPU_METADATA_NVIDIA_PCI_VENDOR_ID) {
				fprintf(stderr,
					"group[%u]: unsupported PCI vendor IDs 0x%04x/0x%04x\n",
					i, group_vendor_id,
					group_subsystem_vendor_id);
				return -1;
			}
			if (!record_size) {
				fprintf(stderr,
					"group[%u]: invalid vGPU type record size 0\n",
					i);
				return -1;
			}
			if (!record_count ||
			    record_count > VGPU_METADATA_MAX_VGPU_TYPES) {
				fprintf(stderr,
					"group[%u]: invalid vGPU type record count %u\n",
					i, record_count);
				return -1;
			}

			expected_group_size = VGPU_METADATA_VGPU_HEADER_SIZE +
				(uint64_t)record_size * record_count;
			if (expected_group_size != group_size) {
				fprintf(stderr,
					"group[%u]: size does not equal header plus compact records\n",
					i);
				return -1;
			}

			if (group_vendor_id == vendor_id &&
			    group_device_id == device_id &&
			    group_subsystem_vendor_id == subsystem_vendor_id &&
			    group_subsystem_id == subsystem_id) {
				if (match_records) {
					fprintf(stderr,
						"multiple vGPU type groups match PCI IDs %04x:%04x:%04x:%04x\n",
						vendor_id, device_id,
						subsystem_vendor_id, subsystem_id);
					return -1;
				}
				match_records = group + group_header_size;
				match_record_size = record_size;
				match_record_count = record_count;
			}
		}

		offset += (size_t)group_size;
	}

	if (offset != size) {
		fprintf(stderr, "metadata contains %zu trailing bytes\n",
			size - offset);
		return -1;
	}
	if (!match_records)
		return 0;

	*gsp_build_version = file_data + VGPU_METADATA_GSP_VERSION_OFFSET;
	*records = match_records;
	*record_size = match_record_size;
	*record_count = match_record_count;
	return 1;
}
