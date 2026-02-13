<!--
SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: MIT

User-facing documentation for the vgpu-mgmt CLI: describes the subcommand
catalog, fwctl device discovery, and the wire-level RPC contract the tool
uses to drive nova-core GMCAPI from userspace.
-->

# fwctl nova-core GMCAPI tools — `vgpu-mgmt`

`vgpu-mgmt` is a single userspace CLI that wraps `fwctl_cmd` RPC opcodes
exposed by the Linux `fwctl` subsystem on top of NVIDIA's nova-core driver.
Each opcode is reached via a subcommand (see [Command catalog](#command-catalog)).

```
vgpu-mgmt <subcommand> [options]
```

Every subcommand opens a `/dev/fwctl/fwctlN` character device (or one
specified via `-d`), validates the device is a nova-core endpoint, and
submits a single GMCAPI command via the `FWCTL_RPC` ioctl.

The kernel-side handler routes the request to the GSP-RM firmware over
MCTP/NVDM and returns the raw response.

Subcommand entry points live in their own translation units
(`nv_*.c`, exposing `cmd_*()` functions); device discovery, BDF parsing,
and the shared GSP preflight RPCs live in `fwctl_common.{c,h}`; the
top-level dispatcher lives in `vgpu-mgmt.c`.

## Transport layer (common to every command)

### Device discovery
1. `opendir("/dev/fwctl")`, iterate `fwctlN` entries.

2. `open(path, O_RDWR)` then `ioctl(fd, FWCTL_INFO, &info)` (`fwctl_info` is
   filled with `info.size = sizeof(info)`).

3. Accept the descriptor when
   `info.out_device_type == FWCTL_DEVICE_TYPE_NOVA_CORE` (= 5); otherwise
   close and continue.

A `-d <path>` flag short-circuits the scan; the tool still validates the
device type before issuing an RPC.

### RPC submission

```c
struct fwctl_rpc rpc = {
    .size    = sizeof(rpc),
    .scope   = FWCTL_RPC_CONFIGURATION,    // every nova-core RPC uses this scope
    .in_len  = in_size,
    .out_len = out_size,
    .in      = (uint64_t)(uintptr_t)in_buf,
    .out     = (uint64_t)(uintptr_t)out_buf,
};
ioctl(fd, FWCTL_RPC, &rpc);
```

`in_buf` and `out_buf` are 16-byte-aligned via `posix_memalign` and
zero-initialised. The buffers are laid out as

```
in_buf  : struct fwctl_rpc_nova_core + command-specific IN payload
out_buf : struct fwctl_rpc_nova_core + command-specific OUT payload
```

### Header (`fwctl_rpc_nova_core`)

Matches `include/uapi/fwctl/nova-core.h` in the kernel uAPI; used for both
the IN and OUT buffers.

| Field        | Set to                          |
|--------------|---------------------------------|
| `command_id` | one of `FWCTL_CMD_NOVA_CORE_*`  |
| `reserved`   | 0                               |

The kernel handler wraps the request in the MCTP/NVDM transport layer
(NVDM type `0x26` = GMCAPI, NV vendor `0x10DE`, MCTP vendor-PCI `0x7E`) on
its own; userspace does not need to populate those fields.

A failing `FWCTL_RPC` ioctl (negative return) signals an error from the
kernel or firmware; tools convert that to `EXIT_FAILURE`. On success the
OUT payload follows the 8-byte header.

### Preflight helpers

Several subcommands run cheap GSP RPCs up-front so an obviously-bad input
fails with a specific message instead of an opaque `EIO` from the real
operation. The helpers live in `fwctl_common.c`:

| Helper | Underlying RPC | Used by |
|--------|----------------|---------|
| `vgpu_type_is_supported(fd, type_id)`   | `QUERY_SUPPORTED_VGPU_TYPES` | `assign`, `query-props` |
| `vgpu_type_is_creatable(fd, type_id)`   | `QUERY_CREATABLE_VGPU_TYPES` | `assign` |
| `vgpu_query_assigned_type(fd, dbdf, *)` | `QUERY_ASSIGNED_VF_VGPU_TYPE` | `assign` (refuses to clobber) |

`supported` is static (metadata uploaded). `creatable` is dynamic and,
in practice, narrower than "types with a free VF": once *any* VF on the
PF is bound, GSP locks the PF's placement plan and `creatable` shrinks
to the subset of supported types compatible with that plan. Examples on
a PF with 10 supported types and 4 VFs:

- 0 VFs bound → `creatable` = all 10 supported types.
- 1 VF bound to type `X` → `creatable` typically narrows to `{X}` (or
  the family of types co-resident with `X`); the other 9 supported
  types are now "supported but not currently creatable."
- vGPU mode off, no metadata, or wrong PCI device ID for the uploaded
  metadata → `creatable` empty even with 0 VFs bound.

Running both preflights lets `assign` give specific messages:
"not in the supported list" (wrong type ID / metadata wasn't loaded for
this PCI device) vs. "not in the creatable list" (valid type but not
co-resident with the active placement plan, or mode/metadata missing
entirely).

---

## Command catalog

### 1. `FWCTL_CMD_NOVA_CORE_GMCAPI_ADD_VGPU_TYPE` — opcode 1
**Subcommand:** `vgpu-mgmt add-type`

| Direction | Layout |
|-----------|--------|
| **IN**    | request header + opaque vGPU-type blob (binary metadata) |
| **OUT**   | response header only (no payload) |

The blob is read from a file produced by `vgpu_metadata_tools` (see `../vgpu_metadata_tools/`).
The subcommand `mmap`s the file, validates the metadata header (identifier
+ CRC32), then iterates the blobs and uploads every `CONFIG_BLOB_VGPU_TYPE`
record whose `device_id` matches `-p`.

**CLI:**
```
vgpu-mgmt add-type [-d <path>] -f <metadata-file> -p <pci_device_id>
```

`-f` and `-p` are required.

**Flow:**
1. Open device.
2. `mmap` the metadata file; validate the identifier and CRC32 in the header.
3. For each blob whose `type == CONFIG_BLOB_VGPU_TYPE` and whose
   `device_id` matches `-p`: build an aligned IN buffer
   (`sizeof(*req_hdr) + payload_len`), copy the blob payload, and issue
   `FWCTL_RPC`.
4. Bail on the first ioctl failure.

### 2. `FWCTL_CMD_NOVA_CORE_GMCAPI_QUERY_SUPPORTED_VGPU_TYPES` — opcode 2
**Subcommand:** `vgpu-mgmt list-supported`

| Direction | Layout |
|-----------|--------|
| **IN**    | request header only (no payload) |
| **OUT**   | response header + `NvU32 typeIds[]` |

The OUT array is sized `QUERY_VGPU_TYPES_MAX = 0x40` slots (= `MAX_VGPU_TYPES_PER_PGPU`).
Trailing zero entries indicate the end of the populated portion.
The tool stops printing at the first `typeId == 0`.

**CLI:** `vgpu-mgmt list-supported [-d <path>]`

**Flow:**
1. Open device.
2. Allocate OUT buffer of
   `sizeof(*resp_hdr) + QUERY_VGPU_TYPES_MAX * sizeof(NvU32)`.
3. Issue `FWCTL_RPC` with `in_len = sizeof(req_hdr)`,
   `out_len = sizeof(*resp_hdr) + 0x40*4`.
4. On ioctl success, walk the array printing each type ID until a zero
   sentinel is hit.

### 3. `FWCTL_CMD_NOVA_CORE_GMCAPI_QUERY_CREATABLE_VGPU_TYPES` — opcode 3
**Subcommand:** `vgpu-mgmt list-creatable`

Identical wire shape, OUT buffer sizing, and flow as opcode 2 — only the
filtering performed by the firmware differs. "Supported" returns every
type the GPU can theoretically host; "creatable" returns those that are
*currently compatible with the PF's locked placement plan*. With zero
VFs bound the two lists match; once any VF is bound, the plan locks and
`creatable` narrows to the co-resident family of types. See the
[Preflight helpers](#preflight-helpers) section for examples.

**CLI:** `vgpu-mgmt list-creatable [-d <path>]`

### 4. `FWCTL_CMD_NOVA_CORE_GMCAPI_ASSIGN_VGPU_TYPE` — opcode 4
**Subcommand:** `vgpu-mgmt assign`

| Direction | Layout |
|-----------|--------|
| **IN**    | request header + `struct GmcapiAssignVgpuTypeInParams` |
| **OUT**   | response header only |

`GmcapiAssignVgpuTypeInParams`:
```c
NvU64 dbdf;          // VF identifier (domain<<32 | bus<<8 | device<<3 | function)
NvU32 vgpuTypeId;    // from QUERY_CREATABLE_VGPU_TYPES
NvU32 swizzId;       // reserved; pass 0 (MIG hook)
NvU16 placementId;   // optional placement hint, 16-bit
```

**CLI:**
```
vgpu-mgmt assign [-d <path>] -b <dbdf> -t <type_id> [-s <swizz>] [-p <placement>]
```

`-b` accepts both `dddd:bb:dd.f` and `bb:dd.f` forms (domain defaults to 0)
via `parse_dbdf()` in `fwctl_common.c`.

**Flow:**
1. Parse and validate args (`got_dbdf && got_type` required;
   `placement_id` must fit in 16 bits).
2. Validate `-b` is an NVIDIA VF (`check_nvidia_vendor_dbdf`,
   `check_is_vf_dbdf`).
3. Open device.
4. Preflight chain — bail with a specific error if any fails:
   - `vgpu_type_is_supported(-t)` — rejects unknown type IDs.
   - `vgpu_type_is_creatable(-t)` — rejects supported-but-not-creatable
     types (most commonly: a different vGPU type is already bound to
     a sibling VF and the locked placement plan doesn't permit `-t`;
     also: vGPU mode off, or the metadata uploaded for this PF doesn't
     cover `-t`).
   - `vgpu_query_assigned_type(-b)` — refuses to clobber a VF that
     already has a type bound.
5. Build IN buffer, populate the params struct, zero-fill the rest.
6. Issue `FWCTL_RPC`. On success the firmware now considers the VF
   claimed by that vGPU type until DEASSIGN.

### 5. `FWCTL_CMD_NOVA_CORE_GMCAPI_DEASSIGN_VGPU_TYPE` — opcode 5
**Subcommand:** `vgpu-mgmt deassign`

| Direction | Layout |
|-----------|--------|
| **IN**    | request header + `struct GmcapiDeassignVgpuTypeInParams` (`{ NvU64 dbdf; }`) |
| **OUT**   | response header only |

**CLI:** `vgpu-mgmt deassign [-d <path>] -b <dbdf>`

**Flow:** symmetric to ASSIGN but with no type-id needed — the firmware
already tracks which vGPU type currently owns this VF and releases it.

### 6. `FWCTL_CMD_NOVA_CORE_GMCAPI_QUERY_VGPU_PROPERTIES` — opcode 6
**Subcommand:** `vgpu-mgmt query-props`

| Direction | Layout |
|-----------|--------|
| **IN**    | request header + `struct GmcapiQueryVgpuPropertiesInParams` (`{ NvU32 vgpuTypeId; }`) |
| **OUT**   | response header + NVKV-encoded `NvU64[]` stream |

The OUT payload is a self-describing NVKV (key/value) stream.
Worst-case wire size: 39 NvU64 words = 312 bytes
(see `VGPUCONFIG_GMCAPI_QUERY_VGPU_NVKV_MAX_OUT_WORDS`).

**CLI:** `vgpu-mgmt query-props [-d <path>] -t <type_id>`

**Flow:**
1. Parse `-t`.
2. Open device.
3. Preflight: `vgpu_type_is_supported(-t)` — bail with "not in the
   supported list" if the type is unknown to GSP.
4. Allocate OUT buffer sized for the 39-word NVKV upper bound.
5. Build IN buf with `vgpuTypeId`.
6. Issue `FWCTL_RPC`.
7. On success, hand the NVKV stream to `nvkvDecode()` (from `nvkv.{h,c}`)
   with `vgpu_props_key_handler` as the callback. The handler uses
   `NVKV_CASE_VAR` / `NVKV_CASE_STRING8` to populate `nvidia_get_vgpu_properties`.
8. `print_vgpu_type()` walks the populated record. If the decoded
   `vgpuTypeId` is `0` (GSP returned an empty record despite the
   preflight letting the type through), an extra
   `(no properties returned -- GSP does not recognize this type)`
   line is appended.

### 7. `FWCTL_CMD_NOVA_CORE_GMCAPI_QUERY_ASSIGNED_VF_VGPU_TYPE` — opcode 7
**Subcommand:** `vgpu-mgmt query-vf`

| Direction | Layout |
|-----------|--------|
| **IN**    | request header + `struct GmcapiQueryAssignedVfVgpuTypeInParams` (`{ NvU64 dbdf; }`) |
| **OUT**   | response header + `struct GmcapiQueryAssignedVfVgpuTypeOutParams` |

`GmcapiQueryAssignedVfVgpuTypeOutParams`:
```c
NvU32 vgpuTypeId;    // 0 if no vGPU type is assigned to this VF
NvU32 swizzId;       // reserved
NvU16 placementId;   // reserved
```

**CLI:** `vgpu-mgmt query-vf [-d <path>] -b <dbdf>`

**Flow:**
1. Parse `-b`.
2. Open device.
3. Build IN buf with `dbdf`; build OUT buf sized for the OUT struct.
4. Issue `FWCTL_RPC`.
5. Cast `out_buf + sizeof(*resp_hdr)` to the OUT params and print
   `vgpuTypeId`. A returned `0` is printed as `(VF is unassigned)`
   on the line below.

### 8. `vgpu-mgmt query-vgpu-mode` / `set-vgpu-mode`

These two subcommands are *not* GMCAPI RPCs. They poke the FSP PRC
knob directly via an `mmap` of the PF's BAR0 (so they require a PF
dbdf, not a VF). Wiring:

| Subcommand | Action |
|------------|--------|
| `query-vgpu-mode -b <pf-dbdf>` | reads the current vGPU-mode PRC knob |
| `set-vgpu-mode   -b <pf-dbdf> -m <mode>` | writes a new value to the same knob |

The PF gate (`check_is_pf_dbdf`) is required because only the PF
exposes the FSP mailbox in BAR0; running these against a VF reads
invalid registers (typically faults).

---

## Build / run

```sh
cd vgpu-tools/fwctl
make                                    # builds the single vgpu-mgmt binary
./vgpu-mgmt                             # top-level usage / subcommand list
./vgpu-mgmt list-supported              # auto-detects /dev/fwctl/fwctlN
./vgpu-mgmt query-props -t 1            # decodes NVKV response
./vgpu-mgmt assign -h                   # subcommand-specific help
```

`make clean` removes the `vgpu-mgmt` binary. The Makefile is a one-shot
build: every `.c` is recompiled and linked in a single `gcc` invocation,
so no intermediate `.o` or `.d` files are produced.

## Files

| Area                  | Files |
|-----------------------|-------|
| Transport / wire      | `fwctl.h` (Linux UAPI, ioctls), `nvrm.h` (`fwctl_rpc_nova_core` header, opcodes) |
| GMCAPI types          | `nv_vgpu.h` (params structs, NVKV keys, `nvidia_get_vgpu_properties`) |
| Metadata / RM headers | `include/metadata.h` (vGPU metadata binary layout: `METADATA_IDR`, `metadata_hdr`, `metadata_blob_hdr`, `CONFIG_BLOB_VGPU_TYPE`, `crc32_le`); consumed by `add-type` to walk the metadata file produced by that tool |
| NVKV (libraries fork) | `nvkv.h`, `nvkv.c`, `nvkv_stub_defs.h` (userspace stubs for kernel `portMem*`/`REF_*`/`NV_*` machinery) |
| Dispatcher            | `vgpu-mgmt.c` (top-level `main`, subcommand table) |
| Shared helpers        | `fwctl_common.{c,h}` — device discovery (`open_nova_core_fwctl`, `open_fwctl_device`), BDF parsing (`parse_dbdf`), PF/VF + vendor gates (`check_*_dbdf`), GSP preflights (`vgpu_query_assigned_type`, `vgpu_type_is_supported`, `vgpu_type_is_creatable`), `QUERY_VGPU_TYPES_MAX`, and the `cmd_*` declarations |
| Subcommand sources    | one `nv_*.c` per opcode, each exporting a `cmd_*` entry point |
| FSP PRC subcommands   | `nv_query_vgpu_mode.c`, `nv_set_vgpu_mode.c`, `prc_knob.{c,h}` — direct BAR0 mailbox path, not an RPC |
