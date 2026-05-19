#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: MIT
#
# End-to-end test for the vgpu-mgmt fwctl tool.
#
# Drives the compiled binary against a real nova-core fwctl device and
# walks through the full lifecycle of every subcommand under test:
#
#    1. list-supported   -- sanity check device + transport
#    2. query-props -t   -- dump properties for every supported type
#    3. list-creatable   -- pick a type ID we can actually assign
#    4. query-props -t   -- validate NVKV decode for the chosen type
#    5. assign -b -t     -- claim a free VF with that type
#    6. query-vf -b      -- confirm the assignment round-tripped
#    7. list-creatable   -- observe how the creatable set narrows
#    8. deassign -b      -- release the VF
#    9. query-vf -b      -- confirm vgpuTypeId is now 0
#   10. list-creatable   -- confirm the released type is creatable again
#
# Each step asserts on exit code AND on the per-command "OK" marker so a
# silent return-zero regression is caught.
#
# Negative tests follow the positive flow. N7 in particular drives the
# "supported but not currently creatable" state: GSP narrows the creatable
# list to types compatible with the active placement plan once any VF is
# bound, and the assign preflight is expected to reject the dropped types
# with the "not in the creatable list" hint.
#
# Usage:
#   test_vgpu_mgmt.sh [-b <vf-bdf>] [-t <type_id>] [-d <fwctl-dev>] [-v]
#
# With no args the script picks the first VF whose PF has
# /sys/.../nvidia/enable_vgpu_support == 1, and the first type ID
# returned by list-creatable.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
VGPU_MGMT="${VGPU_MGMT:-./vgpu-mgmt}"
METADATA_BIN=""
NUM_VFS="${NUM_VFS:-4}"

PF=""
BDF=""
TYPE_ID=""
DEV_PATH=""
META_PATH=""
PCI_ID=""
VERBOSE=0
PASS=0
FAIL=0

usage() {
	cat <<EOF
Usage: $(basename "$0") -p <pf-bdf> -f <metadata-bin>
                       [-b <vf-bdf>] [-t <type_id>] [-d <fwctl-dev>] [-v]

  -p <pf-bdf>   (required) PF BDF in PCI notation. Drives every step:
                vGPU mode is checked on this PF, vGPU types are uploaded
                here, SR-IOV is enabled here, and -- when -b is omitted
                -- the auto-detected VF must be one of its children.
  -f <path>     (required) vGPU metadata binary to upload via add-type.
  -b <bdf>      VF address in PCI BDF notation (default: first VF of -p)
  -t <type_id>  vGPU type ID to assign (default: first creatable type)
  -d <path>     fwctl device path passed through to vgpu-mgmt
  -v            verbose: stream tool output to stderr as tests run
  -h            show this help

Env:
  VGPU_MGMT     override path to the vgpu-mgmt binary
                (default: ./vgpu-mgmt, resolved from cwd)
  NUM_VFS       VFs to spawn on the -p PF when SR-IOV is inactive
                (default: 4). Restored to 0 on exit if we were the ones
                who enabled it.
EOF
}

while getopts "b:t:d:f:p:vh" opt; do
	case "$opt" in
		b) BDF="$OPTARG" ;;
		t) TYPE_ID="$OPTARG" ;;
		d) DEV_PATH="$OPTARG" ;;
		f) META_PATH="$OPTARG" ;;
		p) PF="$OPTARG" ;;
		v) VERBOSE=1 ;;
		h) usage; exit 0 ;;
		*) usage; exit 2 ;;
	esac
done

METADATA_BIN="$META_PATH"

if [[ -z "$PF" ]]; then
	printf '[test] missing required -p <pf-bdf>\n' >&2
	usage
	exit 2
fi

if [[ -z "$METADATA_BIN" ]]; then
	printf '[test] missing required -f <metadata-bin>\n' >&2
	usage
	exit 2
fi

# Accept short ("bb:dd.f") or full ("dddd:bb:dd.f") BDF notation on -p
# and -b. sysfs paths require the full form, so default the domain to
# 0000 when it's omitted.
normalize_bdf() {
	local bdf="$1"
	[[ "$bdf" =~ ^[0-9a-fA-F]{4}: ]] || bdf="0000:$bdf"
	printf '%s' "$bdf"
}
PF="$(normalize_bdf "$PF")"
[[ -n "$BDF" ]] && BDF="$(normalize_bdf "$BDF")"

# ------------------------------------------------------------------ helpers

log()  { printf '[test] %s\n' "$*" >&2; }
ok()   { PASS=$((PASS+1)); printf '  \e[32mPASS\e[0m  %s\n' "$*"; }
fail() { FAIL=$((FAIL+1)); printf '  \e[31mFAIL\e[0m  %s\n' "$*"; }

# Build the optional `-d <path>` suffix once.
DEV_ARG=()
[[ -n "$DEV_PATH" ]] && DEV_ARG=(-d "$DEV_PATH")

# run_step <label> <expected-marker> <vgpu-mgmt args...>
#   Captures combined stdout+stderr into $LAST_OUTPUT and exit code into
#   $LAST_RC. Verifies the command succeeded and that the expected
#   "<CMD>: OK" marker is present.
LAST_OUTPUT=""
LAST_RC=0
run_step() {
	local label="$1" marker="$2"
	shift 2

	log "step: $label  (\$ vgpu-mgmt $* ${DEV_ARG[*]:-})"

	# Run; never abort on non-zero — we want to record + report.
	set +e
	LAST_OUTPUT="$("$VGPU_MGMT" "$@" "${DEV_ARG[@]}" 2>&1)"
	LAST_RC=$?
	set -e

	if (( VERBOSE )); then
		printf '%s\n' "$LAST_OUTPUT" >&2
	fi

	if (( LAST_RC != 0 )); then
		fail "$label exited $LAST_RC"
		printf '%s\n' "$LAST_OUTPUT" | sed 's/^/      | /' >&2
		return 1
	fi

	if ! grep -qF "$marker" <<<"$LAST_OUTPUT"; then
		fail "$label missing marker '$marker'"
		printf '%s\n' "$LAST_OUTPUT" | sed 's/^/      | /' >&2
		return 1
	fi

	ok "$label"
	printf '%s\n' "$LAST_OUTPUT" | sed 's/^/      | /'
	return 0
}

# run_step_fail <label> <vgpu-mgmt args...>
#   Inverse of run_step: passes when the command is rejected. "Rejected"
#   means either a non-zero exit OR no ": OK" success marker in the
#   output. Useful for negative-path tests where we want to confirm the
#   tool (or GSP) refuses bogus input rather than silently succeeding.
run_step_fail() {
	local label="$1"
	shift

	log "neg-step: $label  (\$ vgpu-mgmt $* ${DEV_ARG[*]:-})"

	set +e
	LAST_OUTPUT="$("$VGPU_MGMT" "$@" "${DEV_ARG[@]}" 2>&1)"
	LAST_RC=$?
	set -e

	if (( VERBOSE )); then
		printf '%s\n' "$LAST_OUTPUT" >&2
	fi

	if (( LAST_RC != 0 )) || ! grep -qE ": OK( |$)" <<<"$LAST_OUTPUT"; then
		ok "$label (rejected, rc=$LAST_RC)"
		printf '%s\n' "$LAST_OUTPUT" | sed 's/^/      | /'
		return 0
	fi

	fail "$label unexpectedly succeeded (rc=$LAST_RC, OK marker present)"
	printf '%s\n' "$LAST_OUTPUT" | sed 's/^/      | /' >&2
	return 1
}

# Auto-detect the first NVIDIA VF (vendor 0x10de) whose `physfn` link
# resolves to the given PF BDF, optionally excluding a specific VF BDF
# (so callers can pick a *different* VF than one already in use).
# Restricted by design -- we never pick a VF that belongs to a
# different GPU.
detect_vf() {
	local pf="$1"
	local exclude="${2:-}"
	local dev vendor link name
	for dev in /sys/bus/pci/devices/*; do
		[[ -L "$dev/physfn" ]] || continue
		[[ -r "$dev/vendor" ]] || continue
		vendor="$(cat "$dev/vendor")"
		[[ "$vendor" == "0x10de" ]] || continue
		link="$(readlink -f "$dev/physfn" 2>/dev/null || true)"
		[[ "$(basename "${link:-}")" == "$pf" ]] || continue
		name="$(basename "$dev")"
		[[ -n "$exclude" && "$name" == "$exclude" ]] && continue
		printf '%s\n' "$name"
		return 0
	done
	return 1
}

# Parse the first "vGPU type ID: N" line out of list-creatable output.
first_creatable_type() {
	"$VGPU_MGMT" list-creatable "${DEV_ARG[@]}" 2>/dev/null \
		| awk '/vGPU type ID:/ { print $5; exit }'
}

# Read the PCI device ID (e.g. "0x27b8") of a given PF directly from
# its sysfs `device` file. add-type needs this to key the uploaded
# metadata against the right hardware family.
pf_device_id() {
	local pf="$1"
	[[ -r "/sys/bus/pci/devices/$pf/device" ]] || return 1
	cat "/sys/bus/pci/devices/$pf/device"
}

# If no VFs exist yet on $1 (a PF BDF), write NUM_VFS to its sriov_numvfs
# so detect_vf has something to pick up. Records the PF in
# $SRIOV_ENABLED_BY_US so cleanup() rolls it back on exit. No-op if
# SR-IOV is already active.
ensure_sriov() {
	local pf="$1" numvfs i

	numvfs="$(cat "/sys/bus/pci/devices/$pf/sriov_numvfs" 2>/dev/null || echo 0)"
	if [[ "$numvfs" != "0" ]]; then
		log "SR-IOV already active on $pf (numvfs=$numvfs); leaving as-is"
		return 0
	fi

	log "enabling SR-IOV on $pf with numvfs=$NUM_VFS"
	if ! echo "$NUM_VFS" > "/sys/bus/pci/devices/$pf/sriov_numvfs" 2>/dev/null; then
		log "failed to write sriov_numvfs=$NUM_VFS to $pf (need root?)"
		return 1
	fi
	SRIOV_ENABLED_BY_US="$pf"

	# Give the kernel a moment to enumerate the new VFs before detect_vf scans.
	for i in 1 2 3 4 5; do
		detect_vf "$pf" >/dev/null 2>&1 && return 0
		sleep 0.5
	done
}

# Read the FSP vGPU mode of a given PF, parsed out of the trailing
# "<bdf> vGPU mode is {on|off|unsupported}" line of query-vgpu-mode.
# Returns the literal token (on/off/unsupported) on stdout, or empty if
# the probe failed.
read_vgpu_mode() {
	local pf="$1" out
	out="$("$VGPU_MGMT" query-vgpu-mode -b "$pf" 2>&1 || true)"
	awk '/vGPU mode is/ { print $NF; exit }' <<<"$out"
}

# ------------------------------------------------------------------ preflight

if [[ ! -x "$VGPU_MGMT" ]]; then
	log "vgpu-mgmt binary not found or not executable at: $VGPU_MGMT"
	log "build it first:  make -C $(dirname "$SCRIPT_DIR")"
	exit 2
fi

if [[ ! -r "$METADATA_BIN" ]]; then
	log "vGPU metadata binary not readable at: $METADATA_BIN"
	log "pass -f <path> pointing at a readable vGPU metadata blob"
	exit 2
fi

# Validate the -p argument: it must exist, be an NVIDIA device, and
# actually be a PF (not a VF). VFs are rejected outright with a hint
# pointing at the parent PF -- same policy as the mode scripts.
if [[ ! -d "/sys/bus/pci/devices/$PF" ]]; then
	log "$PF: no such PCI device under /sys/bus/pci/devices/"
	exit 2
fi
if [[ "$(cat "/sys/bus/pci/devices/$PF/vendor" 2>/dev/null)" != "0x10de" ]]; then
	log "$PF: vendor is not NVIDIA (0x10de)"
	exit 2
fi
if [[ -L "/sys/bus/pci/devices/$PF/physfn" ]]; then
	PF_OF_VF="$(basename "$(readlink -f "/sys/bus/pci/devices/$PF/physfn")")"
	log "$PF: is a VF, not a PF -- -p requires the PF"
	log "  -> pass -p $PF_OF_VF instead"
	exit 2
fi

# If -b was also given, validate it: must exist, be NVIDIA, be a VF
# (i.e. have a physfn link), and belong to -p's PF -- so we don't end
# up testing one GPU's VF while claiming another GPU's PF.
if [[ -n "$BDF" ]]; then
	if [[ ! -d "/sys/bus/pci/devices/$BDF" ]]; then
		log "$BDF: no such PCI device under /sys/bus/pci/devices/"
		exit 2
	fi
	if [[ "$(cat "/sys/bus/pci/devices/$BDF/vendor" 2>/dev/null)" != "0x10de" ]]; then
		log "$BDF: vendor is not NVIDIA (0x10de)"
		exit 2
	fi
	if [[ ! -L "/sys/bus/pci/devices/$BDF/physfn" ]]; then
		log "$BDF: is a PF, not a VF -- -b requires a VF"
		log "  -> omit -b to auto-detect a VF under PF $PF"
		exit 2
	fi
	VF_PHYSFN="$(readlink -f "/sys/bus/pci/devices/$BDF/physfn" 2>/dev/null || true)"
	BDF_PARENT="$(basename "${VF_PHYSFN:-?}")"
	if [[ "$BDF_PARENT" != "$PF" ]]; then
		log "$BDF: is a VF, but belongs to PF $BDF_PARENT (not $PF)"
		exit 2
	fi
fi

# Step 1: refuse to run if the FSP vGPU knob is off on $PF, since none
# of the downstream steps (SR-IOV bring-up, add-type, assign, ...) will
# work without it.
VGPU_MODE="$(read_vgpu_mode "$PF")"
log "vGPU mode : ${VGPU_MODE:-?} (PF $PF)"
if [[ "$VGPU_MODE" == "off" ]]; then
	log "set it on before proceeding:"
	log "  $VGPU_MGMT set-vgpu-mode -b $PF -m on"
	log "  reboot"
	exit 2
fi

# Read $PF's PCI device ID -- add-type needs it to key the uploaded
# metadata against the right hardware family.
PCI_ID="$(pf_device_id "$PF" || true)"
if [[ -z "$PCI_ID" ]]; then
	log "could not read PCI device ID from /sys/bus/pci/devices/$PF/device"
	exit 2
fi

# Register the cleanup trap *before* we mutate host state (SR-IOV /
# assigned-VF), so any failure between here and the end of the run
# still rolls back the host. Both flags are initialized empty/zero;
# cleanup() is a no-op until ensure_sriov or the assign step sets them.
ASSIGN_DONE=0
SRIOV_ENABLED_BY_US=""
cleanup() {
	if (( ASSIGN_DONE )); then
		log "cleanup: deassigning VF $BDF"
		"$VGPU_MGMT" deassign -b "$BDF" "${DEV_ARG[@]}" >/dev/null 2>&1 || true
	fi
	if [[ -n "$SRIOV_ENABLED_BY_US" ]]; then
		log "cleanup: disabling SR-IOV on $SRIOV_ENABLED_BY_US (sriov_numvfs=0)"
		echo 0 > "/sys/bus/pci/devices/$SRIOV_ENABLED_BY_US/sriov_numvfs" 2>/dev/null || true
	fi
}
trap cleanup EXIT

# Step 2: enable SR-IOV on $PF (no-op if already up).
ensure_sriov "$PF" || true

# Step 3: pick the first VF that belongs to $PF -- unless the user
#         already pinned one (via -b, or via -p pointing at a VF), in
#         which case we trust their choice.
if [[ -z "$BDF" ]]; then
	BDF="$(detect_vf "$PF" || true)"
	if [[ -z "$BDF" ]]; then
		log "could not find a VF under PF $PF after enabling SR-IOV."
		log "check dmesg for SR-IOV errors, or pass -b <vf-bdf> explicitly."
		exit 2
	fi
	log "auto-detected VF: $BDF"
fi

# NB: TYPE_ID auto-detection runs `list-creatable`, which only returns
# anything useful AFTER add-type has uploaded the metadata blob. So we
# defer that detection until after step 0 (add-type) below.

# ------------------------------------------------------------------ tests

log "binary    : ${VGPU_MGMT#$SCRIPT_DIR/}"
log "metadata  : ${METADATA_BIN#$SCRIPT_DIR/}"
log "PF (BDF)  : $PF"
log "PCI dev   : $PCI_ID"
log "VF (BDF)  : $BDF"
log "type ID   : ${TYPE_ID:-<auto, resolved after add-type>}"
[[ -n "$DEV_PATH" ]] && log "fwctl dev : $DEV_PATH"
echo

# If the VF is already assigned (leftover from a previous run), release
# it first so the assign step actually exercises the assign path.
set +e
PRE_OUTPUT="$("$VGPU_MGMT" query-vf -b "$BDF" "${DEV_ARG[@]}" 2>&1)"
PRE_RC=$?
set -e
if (( PRE_RC == 0 )); then
	PRE_TYPE="$(awk '/ASSIGNED VGPU TYPE ID:/ { print $5; exit }' <<<"$PRE_OUTPUT")"
	if [[ -n "$PRE_TYPE" && "$PRE_TYPE" != "0" ]]; then
		log "pre-clean: VF $BDF already has type $PRE_TYPE -- deassigning"
		"$VGPU_MGMT" deassign -b "$BDF" "${DEV_ARG[@]}" >/dev/null 2>&1 || true
	fi
fi

# 0. add-type -- upload the vGPU type metadata blob to GSP. Until this
#    succeeds list-supported / list-creatable have nothing to return,
#    so this must come first.
run_step "add-type -f ${METADATA_BIN#$SCRIPT_DIR/} -p $PCI_ID" "ADD_VGPU_TYPE: OK" \
	add-type -f "$METADATA_BIN" -p "$PCI_ID" || true

# Now that types are uploaded, resolve TYPE_ID if the user didn't pin one.
if [[ -z "$TYPE_ID" ]]; then
	TYPE_ID="$(first_creatable_type || true)"
	if [[ -z "$TYPE_ID" ]]; then
		log "list-creatable returned no usable type IDs after add-type;"
		log "pass -t <id> explicitly or check the metadata blob"
		exit 2
	fi
	log "auto-detected creatable type ID: $TYPE_ID"
fi

# 1. list-supported
run_step "list-supported" "QUERY_SUPPORTED_VGPU_TYPES: OK" \
	list-supported || true

# 2. query-props for every supported type ID. Uses the type IDs we just
#    captured in $LAST_OUTPUT from list-supported, so we don't have to
#    re-query the device.
SUPPORTED_IDS="$(awk '/vGPU type ID:/ { print $5 }' <<<"$LAST_OUTPUT")"
if [[ -z "$SUPPORTED_IDS" ]]; then
	fail "list-supported emitted no type IDs; skipping per-type query-props"
else
	while read -r tid; do
		[[ -z "$tid" ]] && continue
		run_step "query-props -t $tid (supported)" "QUERY_VGPU_PROPERTIES: OK" \
			query-props -t "$tid" || true
		if ! grep -qE "vgpuTypeId .*= $tid([^0-9]|$)" <<<"$LAST_OUTPUT"; then
			fail "query-props output for type $tid missing vgpuTypeId=$tid"
		fi
	done <<<"$SUPPORTED_IDS"
fi

# 3. list-creatable (and verify TYPE_ID is actually creatable right now)
run_step "list-creatable" "QUERY_CREATABLE_VGPU_TYPES: OK" \
	list-creatable || true
if ! grep -qE "vGPU type ID: $TYPE_ID([^0-9]|$)" <<<"$LAST_OUTPUT"; then
	fail "list-creatable does not include requested type ID $TYPE_ID"
fi

# 4. query-props on the chosen type
run_step "query-props -t $TYPE_ID" "QUERY_VGPU_PROPERTIES: OK" \
	query-props -t "$TYPE_ID" || true
# Sanity-check that the NVKV decode emitted the type back to us.
if ! grep -qE "vgpuTypeId .*= $TYPE_ID([^0-9]|$)" <<<"$LAST_OUTPUT"; then
	fail "query-props output missing vgpuTypeId=$TYPE_ID"
fi

# 5. assign
if run_step "assign -b $BDF -t $TYPE_ID" "ASSIGN_VGPU_TYPE: OK" \
	assign -b "$BDF" -t "$TYPE_ID"; then
	ASSIGN_DONE=1
fi

# 6. query-vf -- must report the type we just assigned
run_step "query-vf -b $BDF (post-assign)" "QUERY_ASSIGNED_VF_VGPU_TYPE: OK" \
	query-vf -b "$BDF" || true
if ! grep -qE "ASSIGNED VGPU TYPE ID: $TYPE_ID([^0-9]|$)" <<<"$LAST_OUTPUT"; then
	fail "query-vf did not report type $TYPE_ID after assign"
fi

# 7. list-creatable (post-assign) -- shows how the creatable set narrows
#    once a VF on this PF has been claimed.
run_step "list-creatable (post-assign)" "QUERY_CREATABLE_VGPU_TYPES: OK" \
	list-creatable || true

# 7b. double-assign -- the tool's preflight queries the current binding
#     before sending the assign RPC, so re-assigning a VF that already
#     has a type bound must be rejected with a clear error. This guards
#     against silent clobber regressions in either the preflight check
#     itself or the underlying QUERY_ASSIGNED_VF_VGPU_TYPE path.
if run_step_fail "assign while already assigned (-b $BDF -t $TYPE_ID)" \
	assign -b "$BDF" -t "$TYPE_ID"; then
	# Be specific about the rejection reason -- a generic non-zero exit
	# would also pass run_step_fail. We want to see *our* preflight
	# message ("already assigned to vGPU type N"), not an unrelated
	# fwctl/FSP error that happens to fail.
	if ! grep -qF "already assigned to vGPU type $TYPE_ID" <<<"$LAST_OUTPUT"; then
		fail "double-assign rejection didn't include the 'already assigned to vGPU type $TYPE_ID' marker"
	fi
fi

# 8. deassign
if run_step "deassign -b $BDF" "DEASSIGN_VGPU_TYPE: OK" \
	deassign -b "$BDF"; then
	ASSIGN_DONE=0
fi

# 9. query-vf again -- must report 0 (unassigned)
run_step "query-vf -b $BDF (post-deassign)" "QUERY_ASSIGNED_VF_VGPU_TYPE: OK" \
	query-vf -b "$BDF" || true
if ! grep -qE "ASSIGNED VGPU TYPE ID: 0([^0-9]|$)" <<<"$LAST_OUTPUT"; then
	fail "query-vf did not report type 0 after deassign"
fi

# 10. list-creatable (post-deassign) -- the type we just released should be
#     creatable again now that the VF is free.
run_step "list-creatable (post-deassign)" "QUERY_CREATABLE_VGPU_TYPES: OK" \
	list-creatable || true
if ! grep -qE "vGPU type ID: $TYPE_ID([^0-9]|$)" <<<"$LAST_OUTPUT"; then
	fail "list-creatable does not include type $TYPE_ID after deassign"
fi

# ------------------------------------------------------------------ negative tests
#
# Each of these expects the tool to reject the request. We require either
# a non-zero exit or a missing "X: OK" marker -- silent zero-exit success
# on bogus input is itself a regression.
#
# Pick a type ID that's almost certainly unsupported. 0 is reserved
# ("unassigned"), and the supported list returned by GSP is bounded well
# below this value on every product line we ship.
BAD_TYPE_ID=999999
# A syntactically-broken BDF (the tool's parser should reject this
# outright, without ever hitting fwctl).
BAD_BDF_MALFORMED="not-a-bdf"
# Well-formed but extremely unlikely to exist on the box under test.
BAD_BDF_NONEXISTENT="ffff:ff:1f.7"

echo
log "--- negative test cases ---"

# N1. assign with an unsupported / out-of-range type ID
run_step_fail "assign with invalid type ID ($BAD_TYPE_ID)" \
	assign -b "$BDF" -t "$BAD_TYPE_ID" || true

# N2. assign with a malformed BDF string
run_step_fail "assign with malformed BDF ($BAD_BDF_MALFORMED)" \
	assign -b "$BAD_BDF_MALFORMED" -t "$TYPE_ID" || true

# N3. assign with a well-formed but nonexistent BDF
run_step_fail "assign with nonexistent BDF ($BAD_BDF_NONEXISTENT)" \
	assign -b "$BAD_BDF_NONEXISTENT" -t "$TYPE_ID" || true

# N4. query-props on an invalid type ID
run_step_fail "query-props with invalid type ID ($BAD_TYPE_ID)" \
	query-props -t "$BAD_TYPE_ID" || true

# N5. query-vf with a malformed BDF
run_step_fail "query-vf with malformed BDF ($BAD_BDF_MALFORMED)" \
	query-vf -b "$BAD_BDF_MALFORMED" || true

# N6. deassign on a VF that isn't currently assigned. After step 8 the
#     VF is back in the unassigned state, so this exercises the
#     "deassign-without-assign" path on a real VF (avoids conflating
#     BDF-validation errors with state errors). The tool's preflight
#     queries the current binding before sending the deassign RPC --
#     we grep for its specific marker so a regression that removes
#     the preflight (falling back to the FSP rejection path) would
#     fail this test instead of silently passing.
if run_step_fail "deassign without prior assign ($BDF)" \
	deassign -b "$BDF"; then
	if ! grep -qF "not currently assigned to any vGPU type" <<<"$LAST_OUTPUT"; then
		fail "deassign-without-assign rejection didn't include the 'not currently assigned' marker"
	fi
fi

# N7. creatable preflight: a vGPU type is in the supported list but NOT
#     in the (currently narrowed) creatable list. Binding any VF locks
#     the PF's placement plan, after which creatable shrinks to types
#     compatible with that plan; the other supported types become
#     "supported but not currently creatable" -- exactly the case the
#     creatable preflight is meant to catch.
#
#     A regression that dropped the preflight would fall through to a
#     generic EIO from the assign RPC, which would miss the "not in the
#     creatable list" marker that this test grep-asserts on.
#
#     The assign-side preflight order is: already-assigned -> supported
#     -> creatable. So the not-creatable attempt must target a VF that
#     ISN'T $BDF (which we just bound in the setup), or the
#     already-assigned check short-circuits before we ever reach the
#     creatable preflight.
log "--- N7: assigning $BDF to narrow the creatable list ---"
if run_step "assign -b $BDF -t $TYPE_ID (N7 setup)" "ASSIGN_VGPU_TYPE: OK" \
	assign -b "$BDF" -t "$TYPE_ID"; then
	ASSIGN_DONE=1
fi

# Snapshot the now-narrowed creatable list and the full supported list.
if run_step "list-creatable (post-N7-setup)" "QUERY_CREATABLE_VGPU_TYPES: OK" \
	list-creatable; then
	N7_CREATABLE="$(awk '/vGPU type ID:/ { print $5 }' <<<"$LAST_OUTPUT" | sort -u)"
fi
if run_step "list-supported (post-N7-setup)" "QUERY_SUPPORTED_VGPU_TYPES: OK" \
	list-supported; then
	N7_SUPPORTED="$(awk '/vGPU type ID:/ { print $5 }' <<<"$LAST_OUTPUT" | sort -u)"
fi

# Find a type that's supported but not currently creatable.
N7_DROPPED_TYPE="$(comm -23 <(echo "$N7_SUPPORTED") <(echo "$N7_CREATABLE") | head -n 1)"

# Pick a second VF under the same PF (not $BDF) for the rejection attempt.
N7_VF="$(detect_vf "$PF" "$BDF" || true)"

if [[ -z "$N7_DROPPED_TYPE" ]]; then
	fail "N7 setup: no supported-but-not-creatable type found"
	log "  supported: $(echo "$N7_SUPPORTED" | tr '\n' ' ')"
	log "  creatable: $(echo "$N7_CREATABLE" | tr '\n' ' ')"
elif [[ -z "$N7_VF" ]]; then
	fail "N7 setup: need a second VF under PF $PF (besides $BDF) to exercise the creatable preflight; bump NUM_VFS or pass one in"
else
	log "supported-but-not-creatable type for N7: $N7_DROPPED_TYPE"
	log "second VF for N7 rejection attempt: $N7_VF"

	# The actual check. Targets $N7_VF (not $BDF) so the
	# already-assigned preflight doesn't fire first.
	if run_step_fail "assign rejected (not creatable, -b $N7_VF -t $N7_DROPPED_TYPE)" \
		assign -b "$N7_VF" -t "$N7_DROPPED_TYPE"; then
		if ! grep -qF "not in the creatable list" <<<"$LAST_OUTPUT"; then
			fail "not-creatable rejection didn't include the 'not in the creatable list' marker"
		fi
	fi
fi

# Release the N7 setup assign so the summary reflects test outcomes only
# (the EXIT trap is the backstop if anything between here and now bailed).
if (( ASSIGN_DONE )); then
	"$VGPU_MGMT" deassign -b "$BDF" "${DEV_ARG[@]}" >/dev/null 2>&1 || true
	ASSIGN_DONE=0
fi

# N8. add-type with a -p value that doesn't match the fwctl device's
#     PCI ID. The validator reads /sys/class/fwctl/<name>/device/device
#     and refuses to upload metadata for the wrong SKU. A regression
#     that drops the check would either upload to the wrong device or
#     silently produce no matching blobs; we grep for the specific
#     mismatch marker so either failure mode is caught here.
BAD_PCI_ID="0xdead"
if run_step_fail "add-type with mismatched -p ($BAD_PCI_ID)" \
	add-type -f "$METADATA_BIN" -p "$BAD_PCI_ID"; then
	if ! grep -qF "does not match fwctl device PCI ID" <<<"$LAST_OUTPUT"; then
		fail "add-type -p mismatch rejection didn't include the 'does not match fwctl device PCI ID' marker"
	fi
fi

# N9. add-type with an unknown trailing positional argument. getopt
#     leaves anything after the options at argv[optind..], and every
#     subcommand calls reject_extra_args() to refuse silent drops. We
#     hit add-type here (any subcommand works); the marker is shared.
if run_step_fail "add-type with unknown trailing arg" \
	add-type -f "$METADATA_BIN" -p "$PCI_ID" junk_trailing_arg; then
	if ! grep -qF "unexpected positional argument" <<<"$LAST_OUTPUT"; then
		fail "add-type trailing-arg rejection didn't include the 'unexpected positional argument' marker"
	fi
fi

# ------------------------------------------------------------------ summary

echo
log "results: $PASS passed, $FAIL failed"
(( FAIL == 0 ))
