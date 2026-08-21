<!--
SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: MIT

End-to-end test suite for the vgpu-mgmt CLI: drives the compiled binary
against a real nova-core fwctl device and asserts on both exit code and
the per-subcommand "<CMD>: OK" marker.
-->

# vgpu-mgmt tests

End-to-end tests for the `vgpu-mgmt` fwctl tool. Each script drives the
compiled binary against a real `nova-core` fwctl device and asserts on
both exit code AND the per-command `<CMD>: OK` marker — a silent
return-zero regression must not pass.

> **Note:** The tests exercise the `vgpu-mgmt` binary built in `../fwctl/`.
> Either copy that binary into the `tests` directory, or point the tests
> at it directly with `VGPU_MGMT=../fwctl/vgpu-mgmt`.

Reference run on a Blackwell Pro 6000 (PF `0x2bb5`, 10 vGPU types
in the metadata blob):

| script | assertions | exit |
|---|---|---|
| `test_vgpu_mgmt.sh` | 33 passed | `0` |
| `test_query_vgpu_mode.sh` | 5 passed | `0` |
| `test_set_vgpu_mode.sh` | 12 passed | `0` |

## Files

| file | what it exercises |
|---|---|
| `test_vgpu_mgmt.sh` | full vGPU type lifecycle: `add-type → list-supported → query-props → list-creatable → assign → query-vf → deassign` (+ negative cases) |
| `test_query_vgpu_mode.sh` | `query-vgpu-mode` (FSP PRC knob read via direct BAR0 mmap) |
| `test_set_vgpu_mode.sh` | `set-vgpu-mode` (FSP PRC knob write, with auto-restore of the original mode on exit) |
| `vgpu-mgmt` | the binary under test (built from the parent directory) |

## Prerequisites

- Root (writes to `sysfs` SR-IOV controls and opens `/dev/fwctl/*`).
- An NVIDIA PF with the FSP vGPU knob already turned **on** (use
  `vgpu-mgmt set-vgpu-mode -b <pf> -m on` followed by a cold reboot
  if it isn't).
- The `nova-core` kernel driver bound to the PF, exposing
  `/dev/fwctl/fwctlN`.
- `vgpu-mgmt` copied in this directory (or pointed at via the
  `VGPU_MGMT` env var).
- A vGPU metadata file on disk, passed to
  `test_vgpu_mgmt.sh` via the required `-f <path>` flag.

## Tool-level validation (`vgpu-mgmt`)

Independently of the test scripts, the `vgpu-mgmt` binary itself
enforces a few preflight checks. These were added to fail fast (and
clearly) instead of letting bad input reach firmware:

| subcommand | requires | additional preflight |
|---|---|---|
| `query-vgpu-mode`, `set-vgpu-mode` | **PF** | — |
| `query-vf` | **VF** | — |
| `assign` | **VF** | refuses VFs that are already bound to a type |
| `deassign` | **VF** | refuses VFs that aren't bound to any type |

Role mismatch (passing a VF where a PF is required, or vice versa)
exits with status `1` and an error like:

```
0000:9b:00.2: is a VF, not a PF; this command requires the PF
  -> try -b 0000:9b:00.0 instead
```

```
0000:9b:00.0: is a PF, not a VF; this command requires a VF
```

`assign` and `deassign` both query the VF's currently-bound type
before issuing the write RPC, so:

```
$ vgpu-mgmt assign -b <bound-vf> -t <type>
error: VF is already assigned to vGPU type 1519 (0x5ef); deassign it first

$ vgpu-mgmt deassign -b <unbound-vf>
error: VF is not currently assigned to any vGPU type; nothing to deassign
```

`test_vgpu_mgmt.sh` accommodates the assign check with a pre-clean:
if its target VF still has a type bound from a previous run, it
deassigns before exercising the assign path.

## Running

All paths default to the **current working directory**, so run from
`fwctl/tests/`:

```sh
cd fwctl/tests
sudo ./test_vgpu_mgmt.sh       -p 0000:9b:00.0 -f vgpu-xxx.bin
sudo ./test_query_vgpu_mode.sh -b 0000:9b:00.0
sudo ./test_set_vgpu_mode.sh   -b 0000:9b:00.0
```

`-b` is **required** for both `test_query_vgpu_mode.sh` and
`test_set_vgpu_mode.sh` (no more PF auto-detection). `-v` on any
script streams the tool's stdout/stderr live as steps run.

## `test_vgpu_mgmt.sh`

The full lifecycle test. Single entry point that brings the GPU up,
exercises every subcommand, and tears state back down.

### Flags

| flag | meaning | default |
|---|---|---|
| `-p <pf-bdf>` | **required**: the PF BDF. Drives every step. | (none) |
| `-f <path>` | **required**: metadata blob path. | (none) |
| `-b <vf-bdf>` | specific VF to use | first VF that hangs off `-p`'s PF |
| `-t <type_id>` | vGPU type to assign | first ID returned by `list-creatable` |
| `-d <path>` | fwctl device path passed through | auto-detected by `vgpu-mgmt` |
| `-v` | verbose tool output | off |

Both `-p` and `-b` accept short (`bb:dd.f`) or full (`dddd:bb:dd.f`)
BDF notation — the domain defaults to `0000` when omitted.

Validation on `-p` rejects:

- non-existent BDFs (`<bdf>: no such PCI device`),
- non-NVIDIA devices (`<bdf>: vendor is not NVIDIA (0x10de)`),
- **VF BDFs** (`<bdf>: is a VF, not a PF — -p requires the PF`). The
  script prints the parent PF in the error so you know what to retry
  with.

Validation on `-b` (when supplied) rejects:

- non-existent BDFs,
- non-NVIDIA devices,
- **PF BDFs** (`<bdf>: is a PF, not a VF — -b requires a VF`) — the
  error suggests omitting `-b` to auto-detect a VF under `-p`'s PF,
- VFs that belong to a different PF (`<bdf>: is a VF, but belongs to
  PF X (not Y)`) — guards against accidentally testing one GPU's VF
  while claiming another GPU's PF.

> **Consistent policy across all three scripts.** A VF passed where a
> PF is expected (`-p` in this script, `-b` in the mode scripts) is
> always rejected with `exit 2` and a hint pointing at the parent PF.
> Silently promoting would mask real misuses — and the FSP/BAR0
> operations under test only work on the PF anyway.

### Env

| var | meaning | default |
|---|---|---|
| `VGPU_MGMT` | binary path | `./vgpu-mgmt` (cwd-relative) |
| `NUM_VFS` | VFs to spawn when SR-IOV is inactive | `4` |

### Flow

1. Validate `-p` (exists, is NVIDIA, is a PF — or derive its PF if a
   VF was passed). If `-b` is also given, confirm it's a child of
   `-p`'s PF.
2. Read FSP vGPU mode via `query-vgpu-mode`. If `off`, abort with a
   hint to run `set-vgpu-mode -m on` followed by `reboot`.
3. Read the PF's PCI device ID from sysfs (passed to `add-type` as
   `-p <hex_id>`).
4. Register the cleanup `EXIT` trap.
5. `ensure_sriov` — write `$NUM_VFS` to `sriov_numvfs` if it's
   currently `0`. Records the PF so cleanup zeroes it on exit. If
   SR-IOV is already up, leaves the configuration alone.
6. Auto-detect the first VF that lives under `-p`'s PF (skipped if
   `-b` was already provided).
7. Run the lifecycle steps + 7 negative cases. The positive count
   varies — `query-props` loops over every supported type ID, so a
   blob with N types produces N+10 positive assertions (27 total for
   the reference blob with 10 types). Two of the negative cases
   grep-check the tool's *preflight* marker — not just for any
   rejection — to guard against silent regressions:
   - **mid-lifecycle double-assign**: while the VF is bound, a
     second `assign` must fail with the `already assigned to vGPU
     type N` marker.
   - **post-lifecycle stray deassign** (N6): after the deassign step
     leaves the VF unbound, another `deassign` must fail with the
     `not currently assigned to any vGPU type` marker.
   Without these grep checks, a regression that removed the
   preflight would fall through to a generic FSP rejection and the
   test would silently keep passing.
8. Cleanup trap: deassign the VF (if assign succeeded mid-way), then
   roll back SR-IOV iff this script was the one that enabled it.

### Side effects

- `sriov_numvfs` is written to and restored only if it was previously
  `0` — pre-existing SR-IOV configurations are untouched.
- An assigned VF is always deassigned, even on failure between the
  `assign` and `deassign` steps.
- Before the assign step runs, a pre-clean checks whether the target
  VF is already bound to a type (e.g. a leftover from a previous
  killed run) and deassigns it first. This is required because
  `vgpu-mgmt assign` refuses to clobber a live binding — without the
  pre-clean the test would fail with `error: VF is already assigned
  to vGPU type N` on the very first step.

### Exit codes

- `0` — all positive + negative assertions passed.
- `1` — at least one step failed.
- `2` — preflight rejected the input (missing `-p`, missing `-f`,
  PF not NVIDIA, VF not under `-p`'s PF, vGPU mode off, can't read
  PCI ID, no VF available after SR-IOV enable, metadata blob not
  readable, binary missing).

## `test_query_vgpu_mode.sh`

Verifies `query-vgpu-mode` returns one of `on`/`off`/`unsupported`,
and that two consecutive reads agree (the operation must be idempotent).

### Flags

| flag | meaning | default |
|---|---|---|
| `-b <pf-bdf>` | **required**: PF BDF | (none) |
| `-v` | verbose | off |

`-b` accepts short (`bb:dd.f`) or full (`dddd:bb:dd.f`) form; the
domain defaults to `0000`. Validation rejects:

- non-existent BDFs (`<bdf>: no such PCI device`),
- non-NVIDIA devices (`<bdf>: vendor is not NVIDIA (0x10de)`),
- **VF BDFs** (`<bdf>: is a VF, not a PF — query-vgpu-mode requires the PF`). The script prints the parent PF in the error so you know what to retry with. This is intentionally strict — `query-vgpu-mode` mmaps BAR0 to reach the FSP mailbox, and only the PF exposes that.

### Coverage

- **Positive** (2): two calls in a row must produce the same mode.
- **Negative** (3): the *tool* must reject `query-vgpu-mode` without
  `-b`, with a malformed BDF, and with a well-formed but nonexistent
  BDF. (The script itself also gates missing `-b` at the user level
  with exit 2.)

5 assertions total. No persistent state is touched — pure read path.

## `test_set_vgpu_mode.sh`

Verifies `set-vgpu-mode` actually flips the FSP PRC knob and that
reads reflect the new configured value immediately. **A reboot is
required for the new mode to take effect on the GPU**, but the knob
value itself is observable right after the write.

### Flags

| flag | meaning | default |
|---|---|---|
| `-b <pf-bdf>` | **required**: PF BDF | (none) |
| `-v` | verbose | off |

`-b` accepts short (`bb:dd.f`) or full (`dddd:bb:dd.f`) form; the
domain defaults to `0000`. Same strict validation as
`test_query_vgpu_mode.sh` — VFs are rejected with a hint pointing at
the parent PF, non-existent and non-NVIDIA BDFs are rejected too.

### Coverage

- **Positive** (7):
  - Record the initial mode via `query-vgpu-mode`.
  - No-op write path: `set-vgpu-mode -m <current>` (firmware should
    short-circuit when new == old).
  - Toggle path: `set-vgpu-mode -m <opposite>`, then `query-vgpu-mode`
    must reflect the toggle.
  - Restore the original mode (also enforced by an `EXIT` trap, so an
    early bail-out between flip and restore still leaves firmware
    state untouched).
- **Negative** (5): the *tool* must reject missing `-b`, missing
  `-m`, invalid `-m` value, malformed BDF, nonexistent BDF. (The
  script itself also gates missing `-b` at the user level with exit
  2.)

12 assertions total. If the GPU reports `unsupported` (no FSP VGPU
knob in firmware), the toggle portion is skipped — there's nothing
meaningful to set.

### Side effects

- The FSP vGPU knob is toggled and restored. If the script is killed
  with `SIGKILL` (which bypasses the `EXIT` trap), firmware state can
  end up flipped. Recover with `set-vgpu-mode -m <intended>` manually.

## Troubleshooting

| symptom | likely cause |
|---|---|
| `vGPU mode : off` then abort | run `set-vgpu-mode -b <pf> -m on`, then **cold-reboot** |
| `could not find a VF under PF <pf>` after SR-IOV enable | check `dmesg` — SR-IOV may have failed to bring the VFs up |
| `<bdf>: no such PCI device` | typo in the BDF, or the device isn't bound to the host |
| `<bdf>: vendor is not NVIDIA (0x10de)` | wrong BDF — that PCI function belongs to something else |
| `<bdf>: is a VF, not a PF — ... requires the PF` | you passed a VF where a PF was expected (`-p` to `test_vgpu_mgmt.sh`, or `-b` to either mode script); use the parent PF the error suggests |
| `<bdf>: is a PF, not a VF — -b requires a VF` | you passed a PF to `test_vgpu_mgmt.sh -b`; either omit `-b` (auto-detect a VF) or pass one of the PF's actual VFs |
| `<bdf>: is a VF, but belongs to PF X (not Y)` | `-b` points at a VF of a different GPU than `-p`; either match them up or omit `-b` |
| `error: VF is already assigned to vGPU type N (0xN); deassign it first` | `vgpu-mgmt assign` refuses to clobber a live binding. Run `vgpu-mgmt deassign -b <vf>` then retry, or let `test_vgpu_mgmt.sh` pre-clean for you. |
| `error: VF is not currently assigned to any vGPU type; nothing to deassign` | `vgpu-mgmt deassign` refuses a no-op. Pass a VF that's actually bound to a type, or skip the deassign — there's nothing to release. |
| `vgpu-mgmt binary not found or not executable at: ./vgpu-mgmt` | not running from `fwctl/tests/`, or binary not built; build with `make -C ..` |
| `vGPU metadata binary not readable at: <path>` | the path passed via `-f` doesn't exist or isn't readable — verify it points at a vGPU metadata blob |
| `missing required -f <metadata-bin>` | `-f <path>` is now required; pass the metadata blob explicitly |
