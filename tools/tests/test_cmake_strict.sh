#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Configure-only test for the FLEET_STRICT_SIGNING CMake option (macOS).
#
#   ON  + APPLE_CODESIGN_DEV=-   -> configure must fail with the strict message
#   ON  + APPLE_CODESIGN_DEV=""  -> configure must fail with the strict message
#   OFF + APPLE_CODESIGN_DEV=""  -> configure must succeed and warn "ad-hoc signing"
#
# Usage: tools/tests/test_cmake_strict.sh
# Env:   CMAKE_PREFIX_PATH (default /opt/homebrew/opt/qt), CMAKE (default cmake)

set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CMAKE="${CMAKE:-cmake}"
QT_PREFIX="${CMAKE_PREFIX_PATH:-/opt/homebrew/opt/qt}"
STRICT_MSG="FLEET_STRICT_SIGNING=ON but APPLE_CODESIGN_DEV is empty or '-'"
ADHOC_MSG="ad-hoc signing (FLEET_STRICT_SIGNING=OFF)"

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "SKIP: FLEET_STRICT_SIGNING only gates macOS bundles (uname=$(uname -s))"
  exit 0
fi

fail=0
pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1"; fail=1; }

run_configure() {
  # $1 = build dir, rest = extra cmake args. Echoes exit code, log in $1.log
  local bdir="$1"; shift
  rm -rf "$bdir"
  "$CMAKE" -S "$REPO_ROOT" -B "$bdir" \
    -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
    "$@" >"$bdir.log" 2>&1
  echo $?
}

base="/tmp/fleet-strict-$$"
trap 'rm -rf "$base"-* "$base"-*.log' EXIT

# --- Case 1: strict ON, ad-hoc identity "-" must fail with the strict message
b="$base-on-dash"
rc=$(run_configure "$b" -DFLEET_STRICT_SIGNING=ON -DAPPLE_CODESIGN_DEV=-)
if [[ "$rc" -ne 0 ]] && grep -qF "$STRICT_MSG" "$b.log"; then
  pass "strict ON + APPLE_CODESIGN_DEV=- fails configure (exit $rc) with strict message"
else
  fail "strict ON + APPLE_CODESIGN_DEV=- : exit=$rc, strict message present=$(grep -cF "$STRICT_MSG" "$b.log")"
  tail -20 "$b.log"
fi

# --- Case 2: strict ON, empty identity must fail with the strict message
b="$base-on-empty"
rc=$(run_configure "$b" -DFLEET_STRICT_SIGNING=ON -DAPPLE_CODESIGN_DEV=)
if [[ "$rc" -ne 0 ]] && grep -qF "$STRICT_MSG" "$b.log"; then
  pass "strict ON + empty APPLE_CODESIGN_DEV fails configure (exit $rc) with strict message"
else
  fail "strict ON + empty APPLE_CODESIGN_DEV : exit=$rc, strict message present=$(grep -cF "$STRICT_MSG" "$b.log")"
  tail -20 "$b.log"
fi

# --- Case 3: strict OFF, empty identity must configure and warn about ad-hoc
b="$base-off"
rc=$(run_configure "$b" -DFLEET_STRICT_SIGNING=OFF -DAPPLE_CODESIGN_DEV=)
if [[ "$rc" -eq 0 ]]; then
  if grep -qF "$ADHOC_MSG" "$b.log"; then
    pass "strict OFF configures (exit 0) and warns: $ADHOC_MSG"
  else
    fail "strict OFF configured but did not emit the ad-hoc warning"
  fi
  if grep -qF "$STRICT_MSG" "$b.log"; then
    fail "strict OFF must not emit the strict FATAL_ERROR message"
  fi
  # Generated install scripts must check the codesign result.
  if grep -q "RESULT_VARIABLE _final_codesign_rc" "$b/cmake_install.cmake" \
     && grep -q "RESULT_VARIABLE _deployqt_rc" "$b/deploy/cmake_install.cmake"; then
    pass "install scripts capture RESULT_VARIABLE for codesign/macdeployqt"
  else
    fail "install scripts missing RESULT_VARIABLE check for codesign/macdeployqt"
  fi
else
  fail "strict OFF configure failed (exit $rc); unrelated dependency problem? see tail:"
  tail -30 "$b.log"
fi

exit $fail
