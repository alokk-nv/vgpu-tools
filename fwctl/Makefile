# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: MIT
#
# Build rules for the vgpu-mgmt userspace CLI: compiles the per-subcommand
# .c files plus shared helpers (fwctl_common, nvkv, prc_knob) into a single
# statically linked binary against the in-tree headers under include/.

CC := gcc
CFLAGS := -O2 -Iinclude
LDFLAGS :=

TARGET := vgpu-mgmt

SRCS := \
	vgpu-mgmt.c \
	fwctl_common.c \
	nv_add_vgpu_type.c \
	nv_assign_vgpu_type.c \
	nv_deassign_vgpu_type.c \
	nv_query_supported_vgpu_types.c \
	nv_query_creatable_vgpu_types.c \
	nv_query_assigned_vf_vgpu_type.c \
	nv_query_vgpu_properties.c \
	nv_query_vgpu_mode.c \
	nv_set_vgpu_mode.c \
	prc_knob.c \
	nvkv.c

all: $(TARGET)

$(TARGET): $(SRCS) $(wildcard include/*.h)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

clean:
	rm -f $(TARGET)
