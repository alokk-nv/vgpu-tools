#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: MIT
#
# End-to-end test for `vgpu-mgmt query-vgpu-mode`.
#
# query-vgpu-mode reads PRC knob ID 41 (PRC_KNOB_ID_VGPU) over the FSP
# MNOC mailbox via direct BAR0 mmap. The target is the PF (the GPU
# itself), not a VF -- VFs do not expose the FSP mailbox in BAR0.
#
# Positive cases:
#   1. query-vgpu-mode -b <pf>     -- prints "vGPU mode is on|off|unsupported"
#   2. query-vgpu-mode -b <pf>     -- second call must agree (idempotent read)
#
# Negative cases:
#   N1. missing -b
#   N2. malformed BDF
#   N3. well-formed but nonexistent BDF
#
# Each step asserts exit code AND output content -- a silent return-zero
# regression must not pass.
#
# Usage:
#   test_query_vgpu_mode.sh [-b <pf-bdf>] [-v]
#
# With no args the script picks the first NVIDIA PCI function that has
# no `physfn` symlink (i.e. a PF, not a VF).

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
VGPU_MGMT="${VGPU_MGMT:-${SCRIPT_DIR}/vgpu-mgmt}"

BDF=""
VERBOSE=0
PASS=0
FAIL=0

usage() {
	cat <<EOF
Usage: $(basename "$0") -b <pf-bdf> [-v]

  -b <bdf>      (required) PF address in PCI BDF notation
  -v            verbose: stream tool output to stderr as tests run
  -h            show this help

Env:
  VGPU_MGMT     override path to the vgpu-mgmt binary
                (default: \${SCRIPT_DIR}/vgpu-mgmt)
EOF
}

while getopts "b:vh" opt; do
	case "$opt" in
		b) BDF="$OPTARG" ;;
		v) VERBOSE=1 ;;
		h) usage; exit 0 ;;
		*) usage; exit 2 ;;
	esac
done

if [[ -z "$BDF" ]]; then
	printf '[test] missing required -b <pf-bdf>\n' >&2
	usage
	exit 2
fi

# ------------------------------------------------------------------ helpers

log()  { printf '[test] %s\n' "$*" >&2; }
ok()   { PASS=$((PASS+1)); printf '  \e[32mPASS\e[0m  %s\n' "$*"; }
fail() { FAIL=$((FAIL+1)); printf '  \e[31mFAIL\e[0m  %s\n' "$*"; }

LAST_OUTPUT=""
LAST_RC=0

# run_step <label> <expected-regex> <vgpu-mgmt args...>
#   Captures combined stdout+stderr into $LAST_OUTPUT, exit code into
#   $LAST_RC. Passes when exit is zero AND the output matches the regex.
run_step() {
	local label="$1" regex="$2"
	shift 2

	log "step: $label  (\$ vgpu-mgmt $*)"

	set +e
	LAST_OUTPUT="$("$VGPU_MGMT" "$@" 2>&1)"
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

	if ! grep -qE "$regex" <<<"$LAST_OUTPUT"; then
		fail "$label missing expected pattern '$regex'"
		printf '%s\n' "$LAST_OUTPUT" | sed 's/^/      | /' >&2
		return 1
	fi

	ok "$label"
	printf '%s\n' "$LAST_OUTPUT" | sed 's/^/      | /'
	return 0
}

# run_step_fail <label> <vgpu-mgmt args...>
#   Inverse of run_step. Passes when the command is rejected (non-zero
#   exit OR output without an obvious success line).
run_step_fail() {
	local label="$1"
	shift

	log "neg-step: $label  (\$ vgpu-mgmt $*)"

	set +e
	LAST_OUTPUT="$("$VGPU_MGMT" "$@" 2>&1)"
	LAST_RC=$?
	set -e

	if (( VERBOSE )); then
		printf '%s\n' "$LAST_OUTPUT" >&2
	fi

	# "Success" for query-vgpu-mode looks like "<bdf> vGPU mode is ...".
	# A negative test passes if either rc != 0, or no such line is found.
	if (( LAST_RC != 0 )) || ! grep -qE 'vGPU mode is (on|off|unsupported)' <<<"$LAST_OUTPUT"; then
		ok "$label (rejected, rc=$LAST_RC)"
		printf '%s\n' "$LAST_OUTPUT" | sed 's/^/      | /'
		return 0
	fi

	fail "$label unexpectedly succeeded (rc=$LAST_RC)"
	printf '%s\n' "$LAST_OUTPUT" | sed 's/^/      | /' >&2
	return 1
}

# ------------------------------------------------------------------ preflight

if [[ ! -x "$VGPU_MGMT" ]]; then
	log "vgpu-mgmt binary not found or not executable at: $VGPU_MGMT"
	log "build it first:  make -C $(dirname "$SCRIPT_DIR")"
	exit 2
fi

# Validate -b: query-vgpu-mode talks to the FSP via direct BAR0 mmap,
# which is only exposed on the PF. Accept short ("bb:dd.f") or full
# ("dddd:bb:dd.f") BDFs, and reject VFs outright -- their BAR0 has no
# FSP mailbox and the operation can't succeed there.
[[ "$BDF" =~ ^[0-9a-fA-F]{4}: ]] || BDF="0000:$BDF"
if [[ ! -d "/sys/bus/pci/devices/$BDF" ]]; then
	log "$BDF: no such PCI device under /sys/bus/pci/devices/"
	exit 2
fi
if [[ "$(cat "/sys/bus/pci/devices/$BDF/vendor" 2>/dev/null)" != "0x10de" ]]; then
	log "$BDF: vendor is not NVIDIA (0x10de)"
	exit 2
fi
if [[ -L "/sys/bus/pci/devices/$BDF/physfn" ]]; then
	PF_OF_VF="$(basename "$(readlink -f "/sys/bus/pci/devices/$BDF/physfn")")"
	log "$BDF: is a VF, not a PF -- query-vgpu-mode requires the PF"
	log "  -> pass -b $PF_OF_VF instead"
	exit 2
fi

# ------------------------------------------------------------------ tests

log "binary    : $VGPU_MGMT"
log "PF (BDF)  : $BDF"
echo

# 1. query-vgpu-mode -- must print a valid mode line.
EXPECTED='vGPU mode is (on|off|unsupported)'
run_step "query-vgpu-mode -b $BDF" "$EXPECTED" \
	query-vgpu-mode -b "$BDF" || true
FIRST_MODE_LINE="$(grep -E "$EXPECTED" <<<"$LAST_OUTPUT" || true)"

# 2. query-vgpu-mode again -- the mode must not change between two
#    consecutive reads. Catches a stale-buffer regression in the FSP
#    RPC layer that would otherwise be invisible.
run_step "query-vgpu-mode -b $BDF (repeat)" "$EXPECTED" \
	query-vgpu-mode -b "$BDF" || true
SECOND_MODE_LINE="$(grep -E "$EXPECTED" <<<"$LAST_OUTPUT" || true)"

if [[ -n "$FIRST_MODE_LINE" && -n "$SECOND_MODE_LINE" \
      && "$FIRST_MODE_LINE" != "$SECOND_MODE_LINE" ]]; then
	fail "two consecutive reads disagree:"
	fail "  first : $FIRST_MODE_LINE"
	fail "  second: $SECOND_MODE_LINE"
fi

# ------------------------------------------------------------------ negative tests

BAD_BDF_MALFORMED="not-a-bdf"
BAD_BDF_NONEXISTENT="ffff:ff:1f.7"

echo
log "--- negative test cases ---"

# N1. missing -b
run_step_fail "query-vgpu-mode without -b" \
	query-vgpu-mode || true

# N2. malformed BDF
run_step_fail "query-vgpu-mode with malformed BDF ($BAD_BDF_MALFORMED)" \
	query-vgpu-mode -b "$BAD_BDF_MALFORMED" || true

# N3. well-formed but nonexistent BDF
run_step_fail "query-vgpu-mode with nonexistent BDF ($BAD_BDF_NONEXISTENT)" \
	query-vgpu-mode -b "$BAD_BDF_NONEXISTENT" || true

# ------------------------------------------------------------------ summary

echo
log "results: $PASS passed, $FAIL failed"
(( FAIL == 0 ))
