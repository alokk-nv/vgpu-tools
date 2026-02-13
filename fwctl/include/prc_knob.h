/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Direct-BAR0 FSP RPC PRC knob interface, ported from gpu-admin-tools
 * (gpu/fsp_mctp.py, gpu/mnoc.py + gpu/fsp_mnoc_rpc.py, and the
 * FspRpc/prc_knob_{read,write} helpers in nvidia_gpu_tools.py).
 *
 * Transport: MNOC port 0 (Blackwell+ FSP path). EMEM is not supported.
 *
 * No fwctl ioctl is used: the tool opens /sys/bus/pci/devices/<bdf>/resource0,
 * mmaps BAR0, and drives the FSP MNOC mailbox directly. The caller is
 * responsible for detaching any kernel driver bound to the device before
 * invoking these helpers (mirrors the Python tool's contract).
 */

#ifndef PRC_KNOB_H
#define PRC_KNOB_H

#include <stdint.h>

/*
 * PRC knob IDs (subset of gpu/prc.py:PrcKnob). Only the VGPU knob is needed
 * for query/set-vgpu-mode but the enum is left open for additions.
 */
enum prc_knob_id {
	PRC_KNOB_ID_VGPU = 41,
};

struct prc_knob_ctx;

/*
 * Open BAR0 for the GPU at the given BDF (canonical "dddd:bb:dd.f" form) and
 * initialize the FSP MNOC RPC transport. The caller must have already
 * validated that the BDF refers to an NVIDIA device (e.g. via
 * check_nvidia_vendor_dbdf()). Returns NULL on failure (errno is left set
 * when possible). The caller releases the context with prc_knob_close().
 */
struct prc_knob_ctx *prc_knob_open(const char *bdf);
void prc_knob_close(struct prc_knob_ctx *ctx);

/*
 * Read / write a PRC knob via FSP RPC sub-message 0xc / 0xd.
 *
 * Return values:
 *   0   success; *out_value is set for read.
 *   1   FSP reported "invalid knob" (the knob is not supported on this FW).
 *  <0   transport / framing failure (negative errno-ish).
 */
int prc_knob_read(struct prc_knob_ctx *ctx, uint16_t knob_id, uint16_t *out_value);
int prc_knob_write(struct prc_knob_ctx *ctx, uint16_t knob_id, uint16_t value);

/* Read-modify-write: only issues the write if the current value differs. */
int prc_knob_check_and_write(struct prc_knob_ctx *ctx, uint16_t knob_id, uint16_t value);

#endif /* PRC_KNOB_H */
