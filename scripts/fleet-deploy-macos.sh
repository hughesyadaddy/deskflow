#!/usr/bin/env bash
# Runs ON a Mac (local or via SSH). Pull fleet branch, signed build, install.
#
# No silent success: every step either succeeds or the script exits non-zero.
# The signing identity comes ONLY from `.env` DESKFLOW_CODESIGN_ID; there is no
# "any Apple Development cert" fallback. With `.env` DESKFLOW_KEYCHAIN_PASSWORD
# (mode 600) the seat signs directly in this shell, SSH included; without it,
# steps that need the login keychain are routed through tools/fleet-gui-exec.py,
# which execs them in the console GUI session.
set -euo pipefail

# Non-interactive SSH shells skip login profiles; Homebrew tools must be on PATH.
# Only bootstrap when brew is not already resolvable so an interactive shell's
# (or a test harness's) PATH ordering is left alone.
if ! command -v brew >/dev/null 2>&1; then
  if [[ -x /opt/homebrew/bin/brew ]]; then
    eval "$(/opt/homebrew/bin/brew shellenv)"
  elif [[ -x /usr/local/bin/brew ]]; then
    eval "$(/usr/local/bin/brew shellenv)"
  else
    export PATH="/opt/homebrew/bin:/usr/local/bin:${PATH:-/usr/bin:/bin:/usr/sbin:/sbin}"
  fi
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DESKFLOW_ROOT="${FLEET_DESKFLOW_ROOT:-$ROOT}"
DESKFLOW_ROOT="${DESKFLOW_ROOT/#\~/$HOME}"
MOUSER_ROOT="${FLEET_MOUSER_ROOT:-$HOME/Desktop/Mouser}"
MOUSER_ROOT="${MOUSER_ROOT/#\~/$HOME}"
MOUSER_FORK_URL="${FLEET_MOUSER_FORK_URL:-https://github.com/hughesyadaddy/Mouser.git}"

# Hard structural guard: DESKFLOW_ROOT/MOUSER_ROOT default to the real repo
# checkouts (FLEET_DESKFLOW_ROOT/FLEET_MOUSER_ROOT override them), and this
# script does real `git checkout`/`pull --ff-only`/build/install against
# whichever they resolve to. A 2026-09-16 incident had a downstream script
# (install-macos.sh) overwrite the real /Applications/Deskflow.app because a
# test's path override got silently clobbered; that specific bug is fixed,
# but this check exists so no future bug of the same shape -- here or in a
# caller -- can point real git/build/install operations at a real checkout
# while under a test harness. BATS_TEST_FILENAME is set by bats for every
# test, unconditionally.
# Mouser's settings dir (core/config.py CONFIG_DIR on macOS) and its log; the
# settings-survival proof copies/hashes files in the former and the native-tap
# gate reads the latter. Overridable for tests only.
MOUSER_SETTINGS_DIR="${MOUSER_SETTINGS_DIR:-$HOME/Library/Application Support/Mouser}"
MOUSER_LOG="${MOUSER_LOG:-$HOME/Library/Logs/Mouser/mouser.log}"

if [[ -n "${BATS_TEST_FILENAME:-}" ]]; then
  for _sandbox_check_path in "$DESKFLOW_ROOT" "$MOUSER_ROOT" "$MOUSER_SETTINGS_DIR" "$MOUSER_LOG"; do
    case "$_sandbox_check_path" in
      "$TMPDIR"*|/tmp/*|/private/tmp/*|/private/var/folders/*|"${BATS_TMPDIR:-__unset__}"*|\
      "${BATS_RUN_TMPDIR:-__unset__}"*|"${BATS_TEST_TMPDIR:-__unset__}"*|"${BATS_FILE_TMPDIR:-__unset__}"*)
        ;;
      *)
        echo "FATAL: running under bats (BATS_TEST_FILENAME set) but '$_sandbox_check_path'" \
             "is not inside a tmp sandbox. Refusing to touch it -- this is exactly the bug" \
             "class that overwrote the real /Applications/Deskflow.app on 2026-09-16." \
             "Aborting." >&2
        exit 90
        ;;
    esac
  done
  unset _sandbox_check_path
fi
BRANCH="${FLEET_BRANCH:-main}"
MOUSER_BRANCH="${FLEET_MOUSER_BRANCH:-$BRANCH}"
DEPLOY_DESKFLOW="${FLEET_DEPLOY_DESKFLOW:-1}"
DEPLOY_MOUSER="${FLEET_DEPLOY_MOUSER:-1}"
RECONFIGURE="${FLEET_RECONFIGURE:-0}"
HOST_TAG="$(hostname -s)"

fail() {
  echo "error: [$HOST_TAG] $*" >&2
  exit 1
}

# Run a command in a session that can reach the login keychain.
# With DESKFLOW_KEYCHAIN_PASSWORD in the seat's .env, prepare_keychain_for_ssh
# has already proven codesign works in THIS shell, so commands run directly.
# Otherwise tools/fleet-gui-exec.py decides: direct exec when the keychain is
# reachable, else it drives the console GUI session and propagates the exit code.
KEYCHAIN_SSH_READY=0
gui_exec() {
  local runner="$DESKFLOW_ROOT/tools/fleet-gui-exec.py"
  if [[ "$KEYCHAIN_SSH_READY" == "1" || ! -x "$runner" ]]; then
    "$@"
  else
    python3 "$runner" -- "$@"
  fi
}

# Make the login keychain usable for codesign from a non-GUI (SSH) session.
# codesign over SSH fails with errSecInternalComponent even on an UNLOCKED
# keychain: the private key's ACL needs the codesign partition, which in a
# GUI session is granted through a dialog nobody can click over SSH.
# `set-key-partition-list` writes that ACL once; `unlock-keychain` covers
# seats whose keychain auto-locks. Both take the password on argv, so it is
# briefly visible in `ps` on the seat itself -- the seat owner already holds
# it. The password is never printed, logged or exported past this function.
prepare_keychain_for_ssh() {
  local pw="${DESKFLOW_KEYCHAIN_PASSWORD:-}"
  [[ -n "$pw" ]] || return 0
  local envfile="$DESKFLOW_ROOT/.env"
  local mode
  mode="$(stat -f %Lp "$envfile")"
  [[ "$mode" == "600" ]] || fail ".env holds DESKFLOW_KEYCHAIN_PASSWORD but is mode $mode; run: chmod 600 $envfile"
  local kc="${DESKFLOW_KEYCHAIN:-$HOME/Library/Keychains/login.keychain-db}"
  local id
  id="$(resolve_codesign_id)"
  echo "== [$HOST_TAG] preparing $kc for codesign over SSH =="
  security unlock-keychain -p "$pw" "$kc" >/dev/null 2>&1 \
    || fail "security unlock-keychain failed for $kc (wrong DESKFLOW_KEYCHAIN_PASSWORD?)"
  security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$pw" "$kc" >/dev/null 2>&1 \
    || fail "security set-key-partition-list failed for $kc"
  local probe
  probe="$(mktemp "${TMPDIR:-/tmp}/deskflow-sign-probe.XXXXXX")"
  cp /bin/ls "$probe"
  if ! codesign --force --sign "$id" "$probe" >/dev/null 2>&1; then
    rm -f "$probe"
    fail "codesign probe with $id failed after keychain preparation; signing over SSH is not usable on this seat"
  fi
  rm -f "$probe"
  # The key ACL is now persistent and the keychain is unlocked; nothing
  # downstream needs the password, so children must not inherit it.
  unset DESKFLOW_KEYCHAIN_PASSWORD
  KEYCHAIN_SSH_READY=1
  echo "== [$HOST_TAG] codesign verified from this session; not routing through the GUI =="
}

DOTENV_LOADED=0
load_dotenv() {
  cd "$DESKFLOW_ROOT"
  # Once only: re-sourcing would re-export DESKFLOW_KEYCHAIN_PASSWORD after
  # prepare_keychain_for_ssh deliberately unset it.
  [[ "$DOTENV_LOADED" == "1" ]] && return 0
  DOTENV_LOADED=1
  if [[ -f .env ]]; then
    # .env is this seat's persistent config; an explicit environment
    # variable set on invocation (as tests/CI callers do) must win, not get
    # silently clobbered -- preserve and restore anything .env also declares
    # that was already set. See scripts/install-macos.sh for the same fix.
    # Plain indexed array only: /usr/bin/env bash on macOS is 3.2, no
    # `declare -A`.
    _env_overrides=()
    while IFS='=' read -r _env_key _; do
      [[ -z "$_env_key" || "$_env_key" == \#* ]] && continue
      if [[ -n "${!_env_key+x}" ]]; then
        _env_overrides+=("$_env_key=${!_env_key}")
      fi
    done < <(grep -E '^[A-Za-z_][A-Za-z0-9_]*=' .env)
    set -a
    # shellcheck disable=SC1091
    source .env
    set +a
    if [[ ${#_env_overrides[@]} -gt 0 ]]; then
      for _env_kv in "${_env_overrides[@]}"; do
        export "$_env_kv"
      done
    fi
    unset _env_overrides _env_key _env_kv
  fi
}

# The ONLY source of the signing identity. Empty or ad-hoc ("-") is fatal:
# an ad-hoc signature changes every build, which resets the app's TCC grants.
resolve_codesign_id() {
  local codesign_id="${DESKFLOW_CODESIGN_ID:-}"
  if [[ -z "$codesign_id" || "$codesign_id" == "-" ]]; then
    fail "DESKFLOW_CODESIGN_ID is unset or '-' (ad-hoc) in $DESKFLOW_ROOT/.env." \
      "Set it to the certificate hash from: security find-identity -v -p codesigning"
  fi
  printf '%s\n' "$codesign_id"
}

cmake_cache_value() {
  # $1 = cache file, $2 = variable name. Prints nothing when absent (awk exits 0 either way).
  [[ -f "$1" ]] || return 0
  awk -v k="$2" 'match($0, "^" k "(:[A-Z]+)?=") { print substr($0, RLENGTH + 1); exit }' "$1"
}

git_pull_deskflow() {
  # FLEET_SKIP_GIT_PULL=1: the controller already synced this checkout.
  [[ "${FLEET_SKIP_GIT_PULL:-0}" == "1" ]] && return 0
  cd "$DESKFLOW_ROOT"
  local ref="${FLEET_DESKFLOW_REF:-}"
  if [[ "$ref" == "HEAD" ]]; then
    echo "== [$HOST_TAG] deskflow: building checked-out HEAD =="
  elif [[ -n "$ref" ]]; then
    echo "== [$HOST_TAG] deskflow checkout --detach $ref =="
    # Fetch only what we deploy: a wholesale fetch fails outright on a
    # case-insensitive filesystem when the remote holds refs that differ
    # only by case (seen 2026-09-25 on the Mouser fork), and a deploy has
    # no business updating every remote ref anyway.
    git fetch origin "$ref" || git fetch origin "$BRANCH"
    git checkout --detach "$ref"
  else
    echo "== [$HOST_TAG] deskflow pull ($BRANCH) =="
    git fetch origin "$BRANCH"
    git checkout "$BRANCH"
    git pull --ff-only origin "$BRANCH"
  fi
  git log -1 --oneline
}

configure_deskflow() {
  load_dotenv
  local codesign_id
  codesign_id="$(resolve_codesign_id)"

  local cache="build/CMakeCache.txt"
  local reason=""
  if [[ "$RECONFIGURE" == "1" ]]; then
    reason="FLEET_RECONFIGURE=1"
  elif [[ ! -f "$cache" ]]; then
    reason="no $cache"
  else
    local cached_id cached_strict
    cached_id="$(cmake_cache_value "$cache" APPLE_CODESIGN_DEV)"
    cached_strict="$(cmake_cache_value "$cache" FLEET_STRICT_SIGNING)"
    if [[ "$cached_id" != "$codesign_id" ]]; then
      reason="APPLE_CODESIGN_DEV cache mismatch ('${cached_id:-<unset>}' != '$codesign_id')"
    elif [[ "$cached_strict" != "ON" ]]; then
      reason="FLEET_STRICT_SIGNING cache is '${cached_strict:-<unset>}', need ON"
    fi
  fi

  if [[ -z "$reason" ]]; then
    echo "== [$HOST_TAG] cmake cache matches (identity + strict signing); skipping configure =="
    return 0
  fi

  echo "== [$HOST_TAG] cmake configure (signed, strict): $reason =="
  gui_exec cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$(brew --prefix qt);$(brew --prefix openssl@3)" \
    -DFLEET_STRICT_SIGNING=ON \
    -DAPPLE_CODESIGN_DEV="$codesign_id" \
    -DSKIP_BUILD_TESTS=ON \
    -DBUILD_TESTS=OFF
}

build_install_deskflow() {
  cd "$DESKFLOW_ROOT"
  echo "== [$HOST_TAG] deskflow build + install =="
  gui_exec cmake --build build --target deskflow-core Deskflow deskflow-vhid-bridge deskflow-prio -j"$(sysctl -n hw.ncpu)"
  # install-macos.sh restarts Deskflow through scripts/deskflow-ctl (launchd).
  # It never touches Mouser; MOUSER_RESTART is set only in deploy_mouser.
  gui_exec bash scripts/install-macos.sh
  # Match install-macos.sh's own DESKFLOW_INSTALL_APP resolution -- a
  # hardcoded /Applications/Deskflow.app here ignores a real override and,
  # under a test harness, would probe the real path regardless of how
  # correctly the rest of this script is sandboxed.
  local install_app="${DESKFLOW_INSTALL_APP:-/Applications/Deskflow.app}"
  if ! codesign --verify --deep --strict "$install_app"; then
    fail "codesign --verify --deep --strict $install_app failed — refusing to call this deploy a success"
  fi
  echo "== [$HOST_TAG] codesign verify OK =="
  verify_login_bridge_plist "$install_app"
}

# The ONE fatal single-launcher gate, after everything is installed (agents,
# bridge check, Mouser): install-macos.sh's own assert-single is report-only
# so a seat with pending human steps (a BTM login item, root-owned retired
# files) is still fully deployed before this exits non-zero.
final_single_launcher_gate() {
  local ctl="$DESKFLOW_ROOT/scripts/deskflow-ctl"
  local install_app="${DESKFLOW_INSTALL_APP:-/Applications/Deskflow.app}"
  [[ -x "$ctl" ]] || fail "deskflow-ctl missing at $ctl; cannot assert a single launcher"
  echo "== [$HOST_TAG] retired files (deskflow-ctl retire) =="
  local retire_out
  retire_out="$(DESKFLOW_INSTALL_APP="$install_app" "$ctl" retire 2>&1)" || true # fleet:allow exit 2 = root steps, listed below
  echo "$retire_out"
  echo "== [$HOST_TAG] single launcher (deskflow-ctl assert-single, fatal) =="
  local assert_out
  if assert_out="$(DESKFLOW_INSTALL_APP="$install_app" "$ctl" assert-single 2>&1)"; then
    echo "$assert_out"
    return 0
  fi
  echo "$assert_out"
  local steps
  steps="$(DESKFLOW_INSTALL_APP="$install_app" "$ctl" login-items print-steps 2>&1 || true)" # fleet:allow best-effort detail for the block below
  cat <<EOF

################################################################################
# HUMAN STEP REQUIRED on $HOST_TAG -- the seat IS deployed (agents, bridge,
# Mouser), but more than one launcher survives. Fix each line, then re-run:
#   $ctl assert-single
################################################################################
$(echo "$assert_out" | sed -n '2,$p' | sed 's/^  /  - /')

  exact commands / UI paths:
$(echo "$retire_out" | grep -E '^\s*sudo ' | sed 's/^ */    /')
$(echo "$steps" | grep -vE 'nothing to remove' | sed 's/^ */    /')
    (retired root files: $ctl retire; login items: $ctl login-items print-steps)
################################################################################
EOF
  fail "deskflow-ctl assert-single failed on $HOST_TAG after a full deploy -- see HUMAN STEP REQUIRED above"
}

# The LoginWindow bridge plist lives in /Library/LaunchAgents (root) and this
# script never escalates, so it can only be rendered and compared here; a
# stale one is reported as a root step, never fixed silently.
verify_login_bridge_plist() {
  local install_app="$1" renderer="$DESKFLOW_ROOT/scripts/install-login-bridge-macos.sh"
  local installed="${DESKFLOW_LOGIN_BRIDGE_PLIST:-/Library/LaunchAgents/org.deskflow.vhid-bridge.plist}"
  local rendered render_err
  rendered="$(mktemp "${TMPDIR:-/tmp}/vhid-bridge.XXXXXX.plist")"
  render_err="$(mktemp "${TMPDIR:-/tmp}/vhid-bridge.XXXXXX.err")"
  if ! DESKFLOW_INSTALL_APP="$install_app" bash "$renderer" --dry-run >"$rendered" 2>"$render_err"; then
    local reason
    reason="$(grep -m1 '^error:' "$render_err" || tail -1 "$render_err")"
    rm -f "$rendered" "$render_err"
    # A seat with no peers has no bridge to configure; anything else (missing
    # bridge binary, unreadable config) is a broken install.
    if [[ "$reason" == *"no coordination peers"* ]]; then
      echo "== [$HOST_TAG] bridge plist: not rendered ($reason); login bridge unchanged =="
      return 0
    fi
    fail "install-login-bridge-macos.sh --dry-run failed: ${reason:-no output}"
  fi
  rm -f "$render_err"
  if ! plutil -lint "$rendered" >/dev/null; then
    rm -f "$rendered"
    fail "install-login-bridge-macos.sh --dry-run produced a plist that does not lint"
  fi
  if [[ -f "$installed" ]] && cmp -s "$rendered" "$installed"; then
    echo "== [$HOST_TAG] bridge plist up to date: $installed =="
  else
    echo "== [$HOST_TAG] bridge plist stale — run root step: sudo env DESKFLOW_INSTALL_APP=$install_app bash $renderer =="
  fi
  rm -f "$rendered"
}

# --- Mouser settings-survival proof ----------------------------------------
# Three checkpoints over $MOUSER_SETTINGS_DIR/{config.json,last_device.json}:
# before the install, right after it (must be byte-identical: the installer
# never touches settings) and after Mouser has run for FLEET_SETTINGS_SETTLE_S
# (default 30) seconds. The last one may differ ONLY by an allowed migration:
# config.json .version strictly increased and every pre-deploy key/value
# (minus version) still present. last_device.json is Mouser's HID warm-path
# cache (core/hid_gesture.py), rewritten when a device reconnects, so after
# the run it is reported but not judged. Mouser's own save_config copies the
# NEW file to config.json.bak (core/config.py), so the
# config.json.pre-deploy-<ts> copy taken here is the authoritative restore
# point (newest 5 kept). The verdict is printed as FLEET_SETTINGS=ok|changed|FAIL
# for scripts/fleet-deploy.sh's settings column.
SETTINGS_SETTLE_S="${FLEET_SETTINGS_SETTLE_S:-30}"
MOUSER_SETTINGS_FILES="config.json last_device.json"

mouser_settings_snapshot() { # -> one "<file>=<sha256|absent>" line per file
  local f h
  for f in $MOUSER_SETTINGS_FILES; do
    if [[ -f "$MOUSER_SETTINGS_DIR/$f" ]]; then
      h="$(shasum -a 256 "$MOUSER_SETTINGS_DIR/$f" | awk '{print $1}')"
    else
      h="absent"
    fi
    printf '%s=%s\n' "$f" "$h"
  done
}

mouser_settings_diff() { # pre post -> "<file> <pre12> -> <post12>" lines (empty when identical)
  local pre="$1" post="$2" f a b
  for f in $MOUSER_SETTINGS_FILES; do
    a="$(sed -n "s/^$f=//p" <<<"$pre")"
    b="$(sed -n "s/^$f=//p" <<<"$post")"
    [[ "$a" == "$b" ]] || printf '%s %s -> %s\n' "$f" "${a:0:12}" "${b:0:12}"
  done
}

mouser_backup_config() { # copy config.json -> config.json.pre-deploy-<ts>, keep the newest 5
  local cfg="$MOUSER_SETTINGS_DIR/config.json" dest
  [[ -f "$cfg" ]] || { echo "none"; return 0; }
  dest="$cfg.pre-deploy-$(date +%Y%m%d-%H%M%S)"
  cp -p "$cfg" "$dest"
  # newest first by name (timestamp-sortable); drop everything past the 5th
  ls -1 "$MOUSER_SETTINGS_DIR"/config.json.pre-deploy-* 2>/dev/null | sort -r | tail -n +6 | while IFS= read -r old; do
    rm -f "$old"
  done
  echo "$dest"
}

# jq: .version strictly increased AND del(.version) of the pre-deploy config is
# a (recursive) subset of the post-run config. Any parse problem -> not allowed.
#
# By design this rule refuses key REMOVALS and RENAMES (a pre-deploy key that
# is gone or moved is a lost setting) and refuses same-version key ADDITIONS
# (a rewrite without a migration is the app clobbering settings). A future
# Mouser migration that legitimately removes or renames a key must bump this
# deploy rule deliberately (e.g. an allowlist of removed keys per version)
# in the same PR -- do not loosen the subset check to make a deploy pass.
MOUSER_MIGRATION_JQ='
  def subset($x; $y):
    if ($x|type) == "object" then
      (($y|type) == "object") and all($x|keys_unsorted[]; . as $k | ($y|has($k)) and subset($x[$k]; $y[$k]))
    elif ($x|type) == "array" then
      (($y|type) == "array") and (($x|length) == ($y|length)) and all(range($x|length); . as $i | subset($x[$i]; $y[$i]))
    else $x == $y end;
  ($a[0].version | numbers) as $pv | ($b[0].version | numbers) as $qv
  | ($qv > $pv) and subset($a[0] | del(.version); $b[0])'

mouser_config_migration_allowed() { # pre.json post.json -> 0 when allowed
  [[ -f "$1" && -f "$2" ]] || return 1
  jq -e -n --slurpfile a "$1" --slurpfile b "$2" "$MOUSER_MIGRATION_JQ" >/dev/null 2>&1
}

# After Mouser has run: prints ok | changed, or fails with a diff summary.
mouser_settings_verdict() { # pre_snapshot post_snapshot pre_config_copy
  local pre="$1" post="$2" pre_copy="$3" diff cfg_diff
  diff="$(mouser_settings_diff "$pre" "$post")"
  cfg_diff="$(grep '^config.json ' <<<"$diff" || true)" # fleet:allow grep no-match is the identical case
  if [[ -z "$cfg_diff" ]]; then
    [[ -z "$diff" ]] || echo "== [$HOST_TAG] Mouser cache rewritten while running (not judged): $(tr '\n' ';' <<<"$diff") ==" >&2
    echo ok
    return 0
  fi
  if [[ "$pre_copy" != "none" ]] && mouser_config_migration_allowed "$pre_copy" "$MOUSER_SETTINGS_DIR/config.json"; then
    echo "== [$HOST_TAG] Mouser config.json migrated (version increased, pre-deploy keys preserved): $cfg_diff ==" >&2
    echo changed
    return 0
  fi
  echo "FLEET_SETTINGS=FAIL" >&2
  fail "Mouser settings changed after ${SETTINGS_SETTLE_S}s of running and it is not an allowed migration" \
    "(version must strictly increase and every pre-deploy key survive): $(tr '\n' ';' <<<"$diff")" \
    "-- restore from $pre_copy"
}

# --- native-tap deploy gate (M4) -------------------------------------------
# Mouser must come up on the native CGEventTap: within FLEET_NATIVE_TAP_TIMEOUT_S
# (60) s of the install its log, past the byte offset recorded before the
# install, must show 'CGEventTap created (native tap:' and must NOT show
# 'CGEventTap enabled on its own run loop' (the PyObjC trampoline path that
# leaks; see the fleet memory program).
NATIVE_TAP_TIMEOUT_S="${FLEET_NATIVE_TAP_TIMEOUT_S:-60}"
NATIVE_TAP_OK='CGEventTap created (native tap:'
NATIVE_TAP_BAD='CGEventTap enabled on its own run loop'

mouser_log_offset() { # -> byte size of the log (0 when absent)
  stat -f %z "$MOUSER_LOG" 2>/dev/null || echo 0
}

wait_native_tap() { # offset -> 0 when the native tap line appeared past offset
  local off="$1" deadline=$((SECONDS + NATIVE_TAP_TIMEOUT_S)) size tail_text
  while :; do
    size="$(mouser_log_offset)"
    # rotated/truncated since the snapshot: read from the start
    (( size < off )) && off=0
    tail_text="$(tail -c +$((off + 1)) "$MOUSER_LOG" 2>/dev/null || true)" # fleet:allow log may not exist yet
    if grep -qF "$NATIVE_TAP_BAD" <<<"$tail_text"; then
      fail "Mouser came up on the PyObjC tap ('$NATIVE_TAP_BAD' in $MOUSER_LOG) -- the native tap dylib is missing or failed to load"
    fi
    if grep -qF "$NATIVE_TAP_OK" <<<"$tail_text"; then
      echo "== [$HOST_TAG] Mouser native tap: $(grep -F "$NATIVE_TAP_OK" <<<"$tail_text" | tail -n 1) =="
      return 0
    fi
    if (( SECONDS >= deadline )); then
      fail "Mouser did not log '$NATIVE_TAP_OK' within ${NATIVE_TAP_TIMEOUT_S}s of the install (log: $MOUSER_LOG, offset $off)"
    fi
    sleep 1
  done
}

deploy_mouser() {
  [[ "$DEPLOY_MOUSER" == "1" ]] || return 0
  [[ -d "$MOUSER_ROOT" ]] || fail "Mouser checkout missing at $MOUSER_ROOT (set FLEET_DEPLOY_MOUSER=0 to skip Mouser)"
  cd "$MOUSER_ROOT"
  [[ -d .git ]] || fail "$MOUSER_ROOT is not a git checkout"

  # FLEET_SKIP_GIT_PULL=1: the controller (fleet-deploy.sh/.ps1) already synced
  # Mouser — branch fast-forward, --ref detach or --rollback — so nothing here
  # may move the checkout.
  if [[ "${FLEET_SKIP_GIT_PULL:-0}" != "1" ]]; then
    local ref="${FLEET_MOUSER_REF:-}"
    if ! git remote get-url fork >/dev/null 2>&1; then
      git remote add fork "$MOUSER_FORK_URL"
    fi
    if [[ "$ref" == "HEAD" ]]; then
      echo "== [$HOST_TAG] Mouser: building checked-out HEAD =="
    elif [[ -n "$ref" ]]; then
      echo "== [$HOST_TAG] Mouser checkout --detach $ref =="
      git fetch fork "$ref" || git fetch fork "$MOUSER_BRANCH"
      git checkout --detach "$ref"
    else
      echo "== [$HOST_TAG] Mouser pull (fork/$MOUSER_BRANCH) =="
      git fetch fork "$MOUSER_BRANCH"
      git checkout "$MOUSER_BRANCH"
      git pull --ff-only fork "$MOUSER_BRANCH"
    fi
    git log -1 --oneline
  fi

  # Checkpoint 1: settings before anything is touched, the restore copy, and
  # the log offset the native-tap gate reads from.
  local pre_snap pre_copy log_off post_snap verdict
  pre_snap="$(mouser_settings_snapshot)"
  pre_copy="$(mouser_backup_config)"
  log_off="$(mouser_log_offset)"
  echo "== [$HOST_TAG] Mouser settings snapshot (pre-deploy): $(tr '\n' ' ' <<<"$pre_snap")backup=$pre_copy =="

  # MOUSER_RESTART=1 is scoped to the Mouser step only: the Mouser installer
  # owns Mouser's restart. The Deskflow step above never touches Mouser.
  if [[ "$KEYCHAIN_SSH_READY" == "1" ]]; then
    echo "== [$HOST_TAG] Mouser build + install (this session, MOUSER_RESTART=1) =="
    MOUSER_RESTART=1 python3 scripts/build_and_install.py
  else
    echo "== [$HOST_TAG] Mouser build + install (GUI session, MOUSER_RESTART=1) =="
    MOUSER_RESTART=1 python3 scripts/build_macos_gui_session.py
  fi

  # Checkpoint 2: the installer must not have touched settings at all.
  post_snap="$(mouser_settings_snapshot)"
  if [[ "$post_snap" != "$pre_snap" ]]; then
    echo "FLEET_SETTINGS=FAIL"
    fail "Mouser settings changed by the install: $(mouser_settings_diff "$pre_snap" "$post_snap" | tr '\n' ';') -- restore from $pre_copy"
  fi
  echo "== [$HOST_TAG] Mouser settings identical after install =="

  # Native-tap gate: the freshly installed Mouser must come up on the native tap.
  wait_native_tap "$log_off"

  # Checkpoint 3: after Mouser has run. Only an allowed migration may differ.
  echo "== [$HOST_TAG] Mouser settings: waiting ${SETTINGS_SETTLE_S}s of Mouser running =="
  sleep "$SETTINGS_SETTLE_S"
  # (info lines go to stderr; only the verdict word is captured)
  verdict="$(mouser_settings_verdict "$pre_snap" "$(mouser_settings_snapshot)" "$pre_copy")" || exit 1
  echo "FLEET_SETTINGS=$verdict"
}

main() {
  echo "=== fleet-deploy-macos on $HOST_TAG ==="
  load_dotenv
  prepare_keychain_for_ssh
  if [[ "$DEPLOY_DESKFLOW" == "1" ]]; then
    git_pull_deskflow
    configure_deskflow
    build_install_deskflow
  fi
  deploy_mouser
  final_single_launcher_gate
  echo "=== done: $HOST_TAG ==="
}

main "$@"
