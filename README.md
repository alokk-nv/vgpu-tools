<!--
SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: MIT

Top-level README for vgpu-tools:
Index of the repository layout, build quick-start, runtime requirements,
and a summary of the vgpu-mgmt subcommands.
Detailed wire-level docs live under fwctl/README.md and tests/README.md.
-->

# vgpu-tools

Userspace tooling for NVIDIA vGPU management on top of the Linux
`fwctl` subsystem and the nova-core driver.

This repository ships a single CLI, `vgpu-mgmt`, plus end-to-end
shell tests that exercise it against a real `nova-core` fwctl
device.

## Layout

| directory | contents |
|---|---|
| [`fwctl/`](fwctl/README.md) | source for the `vgpu-mgmt` CLI — one translation unit per GMCAPI opcode, plus the FSP PRC-knob path for `query-vgpu-mode` / `set-vgpu-mode`. Includes the kernel UAPI headers, the NVKV decoder fork, and the shared transport/discovery helpers. |
| [`tests/`](tests/README.md) | end-to-end test scripts: `test_vgpu_mgmt.sh` (full vGPU-type lifecycle), `test_query_vgpu_mode.sh`, `test_set_vgpu_mode.sh`. |

> **Note:** the `vgpu-mgmt` binary checked in under `tests/` is a copy
> of the one built in `fwctl/` — the tests run against it from the
> tests directory. Rebuild in `fwctl/` and copy it over (or point the
> tests at it via `VGPU_MGMT=../fwctl/vgpu-mgmt`) when source changes.

See each subdirectory's `README.md` for command-by-command wire
formats, flags, and flow.

## Quick start

```sh
# Build the CLI
cd fwctl
make

# Top-level usage / subcommand list
./vgpu-mgmt

# A couple of read-only probes (auto-detects /dev/fwctl/fwctlN)
./vgpu-mgmt list-supported
./vgpu-mgmt query-props -t <type_id>
```

## Requirements

- Linux with the `fwctl` subsystem and the `nova-core` driver bound
  to an NVIDIA PF, exposing `/dev/fwctl/fwctlN`.
- Root (the device node and the sysfs SR-IOV controls used by the
  tests both require it).
- FSP vGPU mode turned **on** for the lifecycle commands — flip it
  with `vgpu-mgmt set-vgpu-mode -b <pf> -m on` followed by a cold
  reboot.
- A vGPU metadata file produced by `vgpu-metadata` for
  `add-type` and for `test_vgpu_mgmt.sh -f`.

## Subcommand summary

| subcommand | purpose |
|---|---|
| `add-type` | upload vGPU-type metadata blobs to GSP-RM |
| `list-supported` | type IDs the GPU can theoretically host |
| `list-creatable` | type IDs currently compatible with the locked placement plan |
| `query-props` | decode NVKV-encoded properties for a type ID |
| `assign` / `deassign` | bind / release a vGPU type on a VF |
| `query-vf` | which type (if any) is bound to a VF |
| `query-vgpu-mode` / `set-vgpu-mode` | read/write the FSP PRC knob (PF only; not an RPC) |

Wire-level details for every subcommand live in
[`fwctl/README.md`](fwctl/README.md).
