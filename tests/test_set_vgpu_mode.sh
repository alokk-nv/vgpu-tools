#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: MIT
#
# End-to-end test for `vgpu-mgmt set-vgpu-mode`.
#
# set-vgpu-mode writes PRC knob ID 41 (PRC_KNOB_ID_VGPU) over the FSP
# MNOC mailbox via direct BAR0 mmap (read-modify-write -- the firmware
# only issues a write when the new value differs from the current one).
# A reboot is required for the new mode to actually take effect on the
# GPU, but the *configured* knob value is reflected in subsequent reads
# immediately.
#
# Because this test mutates persistent firmware state, the script always
# records the initial mode up-front and restores it on EXIT (covering
# both the success path and any early bail-out between flip and
# restore).
#
# Positive cases:
#   1. record current mode via query-vgpu-mode
#   2. set-vgpu-mode -m <current>   -- no-op write path
#   3. set-vgpu-mode -m <opposite>  -- toggle path
#   4. query-vgpu-mode              -- confirm toggle took effect
#   5. set-vgpu-mode -m <current>   -- restore (also exercised by trap)
#
# Negative cases:
#   N1. missing -b
#   N2. missing -m
#   N3. invalid -m value
#   N4. malformed BDF
#   N5. well-formed but nonexistent BDF
#
# Usage:
#   test_set_vgpu_mode.sh [-b <pf-bdf>] [-v]
#
# With no args the script picks the first NVIDIA PCI function that has
# no `physfn` symlink (i.e. a PF, not a VF). If the GPU reports
# "unsupported" (no FSP VGPU knob in firmware), the toggle portion is
# skipped -- there is nothing meaningful to set.

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
skip() { printf '  \e[33mSKIP\e[0m  %s\n' "$*"; }

LAST_OUTPUT=""
LAST_RC=0

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

	# Success looks like "<bdf> vGPU mode set to <mode>. ..."; rejection
	# is either non-zero exit OR no such line.
	if (( LAST_RC != 0 )) || ! grep -qE 'vGPU mode set to (on|off)' <<<"$LAST_OUTPUT"; then
		ok "$label (rejected, rc=$LAST_RC)"
		printf '%s\n' "$LAST_OUTPUT" | sed 's/^/      | /'
		return 0
	fi

	fail "$label unexpectedly succeeded (rc=$LAST_RC)"
	printf '%s\n' "$LAST_OUTPUT" | sed 's/^/      | /' >&2
	return 1
}

# Pull "on" / "off" / "unsupported" out of a query-vgpu-mode invocation.
# Echoes the mode word on stdout; returns non-zero if the output didn't
# parse.
read_mode() {
	local out
	if ! out="$("$VGPU_MGMT" query-vgpu-mode -b "$BDF" 2>&1)"; then
		return 1
	fi
	awk '/vGPU mode is/ { print $NF; exit }' <<<"$out"
}

# ------------------------------------------------------------------ preflight

if [[ ! -x "$VGPU_MGMT" ]]; then
	log "vgpu-mgmt binary not found or not executable at: $VGPU_MGMT"
	log "build it first:  make -C $(dirname "$SCRIPT_DIR")"
	exit 2
fi

# Validate -b: set-vgpu-mode talks to the FSP via direct BAR0 mmap,
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
	log "$BDF: is a VF, not a PF -- set-vgpu-mode requires the PF"
	log "  -> pass -b $PF_OF_VF instead"
	exit 2
fi

# ------------------------------------------------------------------ cleanup

# Record the original mode up-front so we can always restore. If
# read_mode fails or returns "unsupported", we have nothing to restore
# (FSP firmware has no VGPU knob).
INITIAL_MODE="$(read_mode || true)"
RESTORE_NEEDED=0

cleanup() {
	if (( RESTORE_NEEDED )) && [[ "$INITIAL_MODE" == "on" || "$INITIAL_MODE" == "off" ]]; then
		log "cleanup: restoring vGPU mode to '$INITIAL_MODE'"
		"$VGPU_MGMT" set-vgpu-mode -b "$BDF" -m "$INITIAL_MODE" >/dev/null 2>&1 || true
	fi
}
trap cleanup EXIT

# ------------------------------------------------------------------ tests

log "binary    : $VGPU_MGMT"
log "PF (BDF)  : $BDF"
log "initial   : $INITIAL_MODE"
echo

if [[ "$INITIAL_MODE" != "on" && "$INITIAL_MODE" != "off" ]]; then
	skip "FSP firmware reports vGPU mode '$INITIAL_MODE' -- cannot exercise set path"
	skip "running negative cases only"
else
	# Pick the opposite of the initial mode for the toggle step.
	if [[ "$INITIAL_MODE" == "on" ]]; then
		OPPOSITE="off"
	else
		OPPOSITE="on"
	fi

	# 1. set-vgpu-mode to the *current* mode -- exercises the no-op
	#    short-circuit (read returns the current value; nothing is
	#    written; output matches "already <mode>; no change").
	run_step "set-vgpu-mode -m $INITIAL_MODE (no-op write)" \
		"is already $INITIAL_MODE; no change" \
		set-vgpu-mode -b "$BDF" -m "$INITIAL_MODE" || true

	# 2. set-vgpu-mode to the *opposite* mode -- exercises the real
	#    write path. From here until step 4 we owe a restore.
	RESTORE_NEEDED=1
	run_step "set-vgpu-mode -m $OPPOSITE (toggle)" \
		"vGPU mode set to $OPPOSITE\." \
		set-vgpu-mode -b "$BDF" -m "$OPPOSITE" || true

	# 3. confirm query-vgpu-mode now reports the new value.
	CURRENT_MODE="$(read_mode || true)"
	if [[ "$CURRENT_MODE" == "$OPPOSITE" ]]; then
		ok "query-vgpu-mode reports '$OPPOSITE' after toggle"
		PASS=$((PASS+1))
	else
		fail "expected query-vgpu-mode to report '$OPPOSITE', got '$CURRENT_MODE'"
	fi

	# 4. restore the initial mode and verify. The trap also covers
	#    this, but doing it explicitly lets us assert success.
	run_step "set-vgpu-mode -m $INITIAL_MODE (restore)" \
		"vGPU mode set to $INITIAL_MODE\." \
		set-vgpu-mode -b "$BDF" -m "$INITIAL_MODE" || true
	CURRENT_MODE="$(read_mode || true)"
	if [[ "$CURRENT_MODE" == "$INITIAL_MODE" ]]; then
		ok "query-vgpu-mode reports '$INITIAL_MODE' after restore"
		PASS=$((PASS+1))
		RESTORE_NEEDED=0
	else
		fail "expected query-vgpu-mode to report '$INITIAL_MODE' after restore, got '$CURRENT_MODE'"
	fi
fi

# ------------------------------------------------------------------ negative tests

BAD_BDF_MALFORMED="not-a-bdf"
BAD_BDF_NONEXISTENT="ffff:ff:1f.7"
BAD_MODE="maybe"

echo
log "--- negative test cases ---"

# N1. missing -b
run_step_fail "set-vgpu-mode without -b" \
	set-vgpu-mode -m on || true

# N2. missing -m
run_step_fail "set-vgpu-mode without -m" \
	set-vgpu-mode -b "$BDF" || true

# N3. invalid -m value
run_step_fail "set-vgpu-mode with invalid mode ($BAD_MODE)" \
	set-vgpu-mode -b "$BDF" -m "$BAD_MODE" || true

# N4. malformed BDF
run_step_fail "set-vgpu-mode with malformed BDF ($BAD_BDF_MALFORMED)" \
	set-vgpu-mode -b "$BAD_BDF_MALFORMED" -m on || true

# N5. well-formed but nonexistent BDF
run_step_fail "set-vgpu-mode with nonexistent BDF ($BAD_BDF_NONEXISTENT)" \
	set-vgpu-mode -b "$BAD_BDF_NONEXISTENT" -m on || true

# ------------------------------------------------------------------ summary

echo
log "results: $PASS passed, $FAIL failed"
(( FAIL == 0 ))
