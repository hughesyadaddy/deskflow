#!/usr/bin/env bash
# Fleet deploy controller — symmetric: run it from ANY seat.
# Every host in FLEET_HOSTS is targeted exactly once; the seat you are on is
# deployed locally, every other seat over SSH. Clients deploy first, the
# server (FLEET_ROLE_<id>=server, default hackintosh) last.
#
# Setup:
#   cp scripts/fleet.env.example scripts/fleet.env
#   bash scripts/fleet-setup-ssh.sh          # passwordless SSH
#   printf 'FLEET_SEAT_PASSWORD_hackintosh=…\n' >> .env && chmod 600 .env   # unattended Macs (optional)
#   bash scripts/fleet-deploy.sh
#
# Usage:
#   fleet-deploy [--dry-run] [--json PATH|-] [--self-test] [--ref REF]
#                [--rollback] [--host ID] [--app deskflow|mouser]
#                [--deskflow-only] [--mouser-only] [--reconfigure] [--pull-only]
#
#   --dry-run         plan only; with --json prints {hosts:[{id,target,order,role}]}
#   --json PATH|-     write the JSON report to PATH (or stdout with -)
#   --self-test       rebuild+install current HEAD everywhere, then
#                     tools/fleet-health --check all --host all --json;
#                     exit 0 only if every host is ok
#   --ref REF         deploy REF instead of FLEET_BRANCH (HEAD = build what is
#                     checked out, no pull)
#   --rollback        check out the commit recorded in tools/state/last-good.json
#                     per host/app and redeploy those hosts
#   --host ID         limit to one host id from FLEET_HOSTS
#   --app X           deskflow | mouser (same as --deskflow-only / --mouser-only)
#   --reconfigure     force cmake configure on Macs
#   --pull-only       git sync only, no build
#
# The local seat is the FLEET_HOSTS entry matching `hostname -s`
# (case-insensitive) or FLEET_LOCAL_ID; FLEET_SSH_<id>=local is ignored.
# Contract with the per-OS scripts: FLEET_SKIP_GIT_PULL=1 is always exported
# (this controller performs the git sync for BOTH deskflow and Mouser —
# FLEET_MOUSER_BRANCH, default FLEET_BRANCH, from remote `fork`),
# FLEET_DESKFLOW_REF / FLEET_MOUSER_REF carry the exact commit for --ref /
# --rollback. tools/fleet-health is always given --env "$ENV_FILE"
# (FLEET_ENV_FILE).
#
# Seat passwords: this repo's git-ignored `.env` (mode 600) may hold
# FLEET_SEAT_PASSWORD_<id>=<login password> per macOS seat. A remote Mac gets
# it on the SSH session's STDIN (never argv, never fleet.env, never a log);
# the local seat gets it as two in-process exports. The seat script uses it
# as DESKFLOW_KEYCHAIN_PASSWORD (codesign over SSH) and DESKFLOW_SUDO_PASSWORD
# (root steps). Windows seats get nothing. Without a password the seat falls
# back to its own .env / the GUI-session route and prints its root steps.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_FILE="${FLEET_ENV_FILE:-${ROOT}/scripts/fleet.env}"
STATE_DIR="${ROOT}/tools/state"
LAST_GOOD="${STATE_DIR}/last-good.json"
LOCK_FILE="${STATE_DIR}/deploy.lock"
LOCK_DIR="${STATE_DIR}/deploy.lock.d"
HEALTH="${ROOT}/tools/fleet-health"

DRY_RUN=0
JSON_OUT=""
SELF_TEST=0
REF=""
ROLLBACK=0
FILTER_HOST=""
PULL_ONLY=0
OPT_DEPLOY_DESKFLOW=""
OPT_DEPLOY_MOUSER=""
OPT_RECONFIGURE=""

die() { echo "fleet-deploy: error: $*" >&2; exit 1; }
warn() { echo "fleet-deploy: warning: $*" >&2; }
lower() { printf '%s' "$1" | tr '[:upper:]' '[:lower:]'; }
now_ts() { date -u +%Y-%m-%dT%H:%M:%SZ; }

set_app() {
  case "$1" in
    deskflow) OPT_DEPLOY_MOUSER=0 ;;
    mouser) OPT_DEPLOY_DESKFLOW=0 ;;
    *) die "--app must be deskflow or mouser (got '$1')" ;;
  esac
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dry-run) DRY_RUN=1; shift ;;
    --json) [[ $# -ge 2 ]] || die "--json needs PATH or -"; JSON_OUT="$2"; shift 2 ;;
    --self-test) SELF_TEST=1; shift ;;
    --ref) [[ $# -ge 2 ]] || die "--ref needs REF"; REF="$2"; shift 2 ;;
    --rollback) ROLLBACK=1; shift ;;
    --host) [[ $# -ge 2 ]] || die "--host needs ID"; FILTER_HOST="$(lower "$2")"; shift 2 ;;
    --app) [[ $# -ge 2 ]] || die "--app needs deskflow|mouser"; set_app "$2"; shift 2 ;;
    --deskflow-only) set_app deskflow; shift ;;
    --mouser-only) set_app mouser; shift ;;
    --reconfigure) OPT_RECONFIGURE=1; shift ;;
    --pull-only) PULL_ONLY=1; shift ;;
    -h|--help) sed -n '2,/^set -euo pipefail/p' "$0" | sed '$d'; exit 0 ;;
    *) die "unknown option: $1" ;;
  esac
done

[[ "$SELF_TEST" == 1 && "$ROLLBACK" == 1 ]] && die "--self-test and --rollback are exclusive"
[[ "$SELF_TEST" == 1 && -z "$REF" ]] && REF="HEAD"
[[ "$ROLLBACK" == 1 && -n "$REF" ]] && die "--rollback picks its own commits; drop --ref"

command -v jq >/dev/null 2>&1 || die "jq is required (brew install jq)"

if [[ ! -f "$ENV_FILE" ]]; then
  echo "Missing $ENV_FILE — copy from fleet.env.example:" >&2
  echo "  cp scripts/fleet.env.example scripts/fleet.env" >&2
  exit 1
fi
# shellcheck disable=SC1090
source "$ENV_FILE"

FLEET_BRANCH="${FLEET_BRANCH:-main}"
FLEET_HOSTS="${FLEET_HOSTS:-hackintosh macbookpro tiny11}"
DEPLOY_DESKFLOW="${OPT_DEPLOY_DESKFLOW:-${FLEET_DEPLOY_DESKFLOW:-1}}"
DEPLOY_MOUSER="${OPT_DEPLOY_MOUSER:-${FLEET_DEPLOY_MOUSER:-1}}"
RECONFIGURE="${OPT_RECONFIGURE:-${FLEET_RECONFIGURE:-0}}"

# ---------------------------------------------------------------------------
# Seat credentials: ROOT/.env (git-ignored; the same file the local seat's
# deploy script sources). FLEET_SEAT_PASSWORD_<id> lines are read by a plain
# KEY=VALUE parser -- nothing in .env is executed here and nothing is
# exported -- and the file must be mode 600 as soon as it holds any
# *PASSWORD* value. `bash -x` must never echo a password: every line that
# expands one runs between xtrace_off and xtrace_restore (the trace of
# `set +x` itself is discarded by the redirect).
# ---------------------------------------------------------------------------
DOTENV="${ROOT}/.env"
SEAT_PW=""
XTRACE_ON=0
xtrace_off() { if [[ $- == *x* ]]; then XTRACE_ON=1; else XTRACE_ON=0; fi; { set +x; } 2>/dev/null; }
xtrace_restore() { if (( XTRACE_ON )); then set -x; fi; }

# Names (never values) of *PASSWORD* keys with a non-empty value in a
# KEY=VALUE file, one per line. Comments, `export` prefixes and quoted-empty
# values are handled like `source` would.
dotenv_password_keys() { # file
  awk '
    /^[[:space:]]*(export[[:space:]]+)?[A-Za-z_][A-Za-z0-9_]*PASSWORD[A-Za-z0-9_]*[[:space:]]*=/ {
      key = $0; sub(/=.*$/, "", key); sub(/^[[:space:]]*(export[[:space:]]+)?/, "", key); sub(/[[:space:]]*$/, "", key)
      val = substr($0, index($0, "=") + 1); gsub(/^[[:space:]]+|[[:space:]]+$/, "", val)
      if (val == "\"\"" || val == "'"'"''"'"'") val = ""
      if (val != "") print key
    }' "$1"
}

check_dotenv_mode() {
  [[ -f "$DOTENV" ]] || return 0
  local keys mode
  keys="$(dotenv_password_keys "$DOTENV" | tr '\n' ' ')"
  [[ -n "$keys" ]] || return 0
  mode="$(stat -f %Lp "$DOTENV" 2>/dev/null || stat -c %a "$DOTENV")"
  [[ "$mode" == "600" ]] || die "$DOTENV holds ${keys% } but is mode $mode; run: chmod 600 $DOTENV"
}
check_dotenv_mode

# Sets SEAT_PW to the FLEET_SEAT_PASSWORD_<id> value from ROOT/.env (empty
# when unset). Last assignment wins; matching quotes are stripped; an
# unquoted value ends at a ` #` comment, as under `source`.
load_seat_password() { # id
  local want="FLEET_SEAT_PASSWORD_$(lower "$1")" line key val
  SEAT_PW=""
  [[ -f "$DOTENV" ]] || return 0
  xtrace_off
  while IFS= read -r line || [[ -n "$line" ]]; do
    line="${line#"${line%%[![:space:]]*}"}"
    line="${line#export }"
    key="${line%%=*}"
    key="${key%"${key##*[![:space:]]}"}"
    [[ "$key" == "$want" ]] || continue
    val="${line#*=}"
    val="${val#"${val%%[![:space:]]*}"}"
    if [[ ${#val} -ge 2 && ( "$val" == \"*\" || "$val" == \'*\' ) ]]; then
      val="${val:1:${#val}-2}"
    else
      val="${val%%[[:space:]]#*}"
      val="${val%"${val##*[![:space:]]}"}"
    fi
    SEAT_PW="$val"
  done < "$DOTENV"
  xtrace_restore
}

# ---------------------------------------------------------------------------
# Local identity: hostname (or FLEET_LOCAL_ID) must match a FLEET_HOSTS entry.
# ---------------------------------------------------------------------------
resolve_local_id() {
  local hn want id
  hn="$(hostname -s 2>/dev/null)"
  hn="$(lower "$hn")"
  want="$(lower "${FLEET_LOCAL_ID:-$hn}")"
  for id in $FLEET_HOSTS; do
    if [[ "$(lower "$id")" == "$want" ]]; then
      printf '%s' "$id"
      return 0
    fi
  done
  die "no FLEET_HOSTS entry matches this seat (hostname '$hn'; set FLEET_LOCAL_ID to override)"
}
LOCAL_ID="$(resolve_local_id)"

# ---------------------------------------------------------------------------
# Host plan. Parallel indexed arrays (bash 3.2 on macOS has no declare -A).
# ---------------------------------------------------------------------------
P_ID=(); P_TARGET=(); P_OS=(); P_ROLE=(); P_DESKFLOW=(); P_MOUSER=(); P_USER=(); P_SSH=()

host_os() {
  local id="$1" var="FLEET_OS_${1}" v
  v="${!var:-}"
  if [[ -n "$v" ]]; then printf '%s' "$(lower "$v")"
  elif [[ "$(lower "$id")" == "tiny11" ]]; then printf 'windows'
  else printf 'macos'; fi
}

host_role() {
  local var="FLEET_ROLE_${1}"
  printf '%s' "$(lower "${!var:-client}")"
}

build_plan() {
  local id os role ssh_var ssh user_var user seen=" " server_id="" clients=() n=0
  # Server = explicit FLEET_ROLE_<id>=server, else hackintosh when present.
  for id in $FLEET_HOSTS; do
    [[ "$(host_role "$id")" == "server" ]] && { server_id="$id"; break; }
  done
  if [[ -z "$server_id" ]]; then
    for id in $FLEET_HOSTS; do
      [[ "$(lower "$id")" == "hackintosh" ]] && { server_id="$id"; break; }
    done
  fi
  for id in $FLEET_HOSTS; do
    case "$seen" in *" $(lower "$id") "*) warn "duplicate host '$id' in FLEET_HOSTS ignored"; continue ;; esac
    seen="${seen}$(lower "$id") "
    [[ "$id" == "$server_id" ]] && continue
    clients+=("$id")
  done
  local ordered=(${clients[@]+"${clients[@]}"})
  [[ -n "$server_id" ]] && ordered+=("$server_id")

  for id in ${ordered[@]+"${ordered[@]}"}; do
    os="$(host_os "$id")"
    role="client"; [[ "$id" == "$server_id" ]] && role="server"
    ssh_var="FLEET_SSH_${id}"; ssh="${!ssh_var:-}"
    [[ -z "$ssh" || "$ssh" == "local" ]] && ssh="$id"   # "local" never defines locality
    user_var="FLEET_SSH_USER_${id}"; user="${!user_var:-}"
    if [[ -z "$user" ]]; then
      if [[ "$os" == "windows" ]]; then user="alexh"; else user="${USER:-$(id -un)}"; fi
    fi
    n=$((n + 1))
    P_ID+=("$id"); P_OS+=("$os"); P_ROLE+=("$role"); P_USER+=("$user"); P_SSH+=("$ssh")
    if [[ "$id" == "$LOCAL_ID" ]]; then P_TARGET+=("local"); else P_TARGET+=("${user}@${ssh}"); fi
    if [[ "$os" == "windows" ]]; then
      P_DESKFLOW+=("${FLEET_DESKFLOW_PATH_windows:-C:/Users/alexh/Desktop/deskflow}")
      P_MOUSER+=("${FLEET_MOUSER_PATH_windows:-C:/Users/alexh/Desktop/Mouser}")
    else
      P_DESKFLOW+=("${FLEET_DESKFLOW_PATH_macos:-~/Desktop/deskflow}")
      P_MOUSER+=("${FLEET_MOUSER_PATH_macos:-~/Desktop/Mouser}")
    fi
  done
  [[ ${#P_ID[@]} -gt 0 ]] || die "FLEET_HOSTS is empty"
}
build_plan

if [[ -n "$FILTER_HOST" ]]; then
  found=0
  for id in "${P_ID[@]}"; do [[ "$(lower "$id")" == "$FILTER_HOST" ]] && found=1; done
  [[ "$found" == 1 ]] || die "--host '$FILTER_HOST' is not in FLEET_HOSTS ($FLEET_HOSTS)"
fi

selected() { # index -> 0 if host is in scope
  [[ -z "$FILTER_HOST" || "$(lower "${P_ID[$1]}")" == "$FILTER_HOST" ]]
}

emit_json() { # $1 = json text
  if [[ -z "$JSON_OUT" ]]; then return 0
  elif [[ "$JSON_OUT" == "-" ]]; then printf '%s\n' "$1"
  else mkdir -p "$(dirname "$JSON_OUT")"; printf '%s\n' "$1" > "$JSON_OUT"; fi
}

plan_json() {
  local i rows=""
  for i in "${!P_ID[@]}"; do
    selected "$i" || continue
    rows+="$(jq -cn --arg id "${P_ID[$i]}" --arg t "${P_TARGET[$i]}" --argjson o "$((i + 1))" \
      --arg r "${P_ROLE[$i]}" --arg os "${P_OS[$i]}" \
      '{id:$id,target:$t,order:$o,role:$r,os:$os}')"$'\n'
  done
  printf '%s' "$rows" | jq -cs '{hosts:.}'
}

if [[ "$DRY_RUN" == 1 ]]; then
  if [[ -n "$JSON_OUT" ]]; then
    emit_json "$(plan_json)"
  else
    echo "plan (local=${LOCAL_ID}, branch=${FLEET_BRANCH}${REF:+, ref=$REF}):"
    for i in "${!P_ID[@]}"; do
      selected "$i" || continue
      printf '  %d. %-12s %-8s %-7s %s\n' "$((i + 1))" "${P_ID[$i]}" "${P_ROLE[$i]}" "${P_OS[$i]}" "${P_TARGET[$i]}"
    done
  fi
  exit 0
fi

# ---------------------------------------------------------------------------
# Per-repo lock: flock(1) when present, otherwise an atomic mkdir on
# tools/state/deploy.lock.d (macOS ships no flock; PowerShell uses New-Item on
# the same directory). A dead-pid mkdir lock is reclaimed; anything else refuses.
# ---------------------------------------------------------------------------
LOCK_KIND=""
# shellcheck disable=SC2329  # invoked via trap
release_lock() {
  if [[ "$LOCK_KIND" == "mkdir" ]]; then rm -rf "$LOCK_DIR"; fi
}
acquire_lock() {
  mkdir -p "$STATE_DIR"
  if command -v flock >/dev/null 2>&1; then
    exec 9>"$LOCK_FILE"
    if ! flock -n 9; then
      die "deploy lock held ($LOCK_FILE) — another fleet-deploy is running; refusing"
    fi
    LOCK_KIND="flock"
    return 0
  fi
  if ! mkdir "$LOCK_DIR" 2>/dev/null; then
    local pid=""
    [[ -f "$LOCK_DIR/pid" ]] && pid="$(cat "$LOCK_DIR/pid")"
    if [[ -n "$pid" ]] && ! kill -0 "$pid" 2>/dev/null; then
      warn "reclaiming stale deploy lock left by pid $pid"
      rm -rf "$LOCK_DIR"
      mkdir "$LOCK_DIR" 2>/dev/null || die "deploy lock held ($LOCK_DIR); refusing"
    else
      die "deploy lock held ($LOCK_DIR${pid:+, pid $pid}) — another fleet-deploy is running; refusing"
    fi
  fi
  printf '%s\n' "$$" > "$LOCK_DIR/pid"
  LOCK_KIND="mkdir"
  trap release_lock EXIT
}
acquire_lock

# ---------------------------------------------------------------------------
# last-good.json: {host: {app: {commit, ts}}}
# ---------------------------------------------------------------------------
last_good_commit() { # host app
  [[ -f "$LAST_GOOD" ]] || return 1
  local c
  c="$(jq -r --arg h "$1" --arg a "$2" '.[$h][$a].commit // empty' "$LAST_GOOD")"
  [[ -n "$c" ]] || return 1
  printf '%s' "$c"
}
record_last_good() { # host app commit
  local tmp
  mkdir -p "$STATE_DIR"
  [[ -f "$LAST_GOOD" ]] || printf '{}\n' > "$LAST_GOOD"
  tmp="$(mktemp "${STATE_DIR}/last-good.XXXXXX")"
  jq --arg h "$1" --arg a "$2" --arg c "$3" --arg t "$(now_ts)" \
    '.[$h] = ((.[$h] // {}) + {($a): {commit:$c, ts:$t}})' "$LAST_GOOD" > "$tmp"
  mv "$tmp" "$LAST_GOOD"
}

# ---------------------------------------------------------------------------
# Remote command builders
# ---------------------------------------------------------------------------
# Paths reach the remote shell inside double quotes; a leading "~" becomes
# "$HOME" so it expands there (a single-quoted "~" never does).
sh_path() { printf '%s' "${1/#\~/\$HOME}"; }
sh_git_sync() { # path ref -> POSIX sh fragment
  local path ref="$2"; path="$(sh_path "$1")"
  if [[ -z "$ref" ]]; then
    # Fetch only the deployed branch: a wholesale `git fetch` fails on a
    # case-insensitive filesystem when the remote holds refs differing only
    # by case (2026-09-25: Mouser fork test-MxM4 vs test-mxm4).
    printf 'cd "%s" && git fetch origin "%s" && git checkout "%s" && git pull --ff-only origin "%s"' \
      "$path" "$FLEET_BRANCH" "$FLEET_BRANCH" "$FLEET_BRANCH"
  elif [[ "$ref" == "HEAD" ]]; then
    printf 'cd "%s"' "$path"
  else
    printf 'cd "%s" && { git fetch origin "%s" || git fetch origin "%s"; } && git checkout --detach "%s"' "$path" "$ref" "$FLEET_BRANCH" "$ref"
  fi
}
sh_mouser_sync() { # path ref -> fragment (empty only for HEAD)
  # The per-OS scripts always run with FLEET_SKIP_GIT_PULL=1, so this
  # controller is the ONLY place Mouser is synced: empty ref = fast-forward
  # FLEET_MOUSER_BRANCH (default FLEET_BRANCH) from `fork`, other refs detach.
  local path ref="$2" branch="${FLEET_MOUSER_BRANCH:-$FLEET_BRANCH}"; path="$(sh_path "$1")"
  [[ "$DEPLOY_MOUSER" == 1 ]] || return 0
  [[ "$ref" != "HEAD" ]] || return 0
  local add_remote
  add_remote="git -C \"${path}\" remote get-url fork >/dev/null 2>&1 || git -C \"${path}\" remote add fork \"${FLEET_MOUSER_FORK_URL:-https://github.com/hughesyadaddy/Mouser.git}\""
  if [[ -z "$ref" ]]; then
    printf ' && if [ -d "%s/.git" ]; then %s && git -C "%s" fetch fork "%s" && git -C "%s" checkout "%s" && git -C "%s" pull --ff-only fork "%s"; fi' \
      "$path" "$add_remote" "$path" "$branch" "$path" "$branch" "$path" "$branch"
  else
    printf ' && if [ -d "%s/.git" ]; then %s && { git -C "%s" fetch fork "%s" || git -C "%s" fetch fork "%s"; } && git -C "%s" checkout --detach "%s"; fi' \
      "$path" "$add_remote" "$path" "$ref" "$path" "$branch" "$path" "$ref"
  fi
}
sh_exports() { # deskflow_path mouser_path dref mref
  printf 'export FLEET_BRANCH="%s" FLEET_MOUSER_BRANCH="%s" FLEET_DEPLOY_DESKFLOW="%s" FLEET_DEPLOY_MOUSER="%s" FLEET_RECONFIGURE="%s" FLEET_DESKFLOW_ROOT="%s" FLEET_MOUSER_ROOT="%s" FLEET_SKIP_GIT_PULL=1 FLEET_DESKFLOW_REF="%s" FLEET_MOUSER_REF="%s"' \
    "$FLEET_BRANCH" "${FLEET_MOUSER_BRANCH:-$FLEET_BRANCH}" "$DEPLOY_DESKFLOW" "$DEPLOY_MOUSER" "$RECONFIGURE" "$(sh_path "$1")" "$(sh_path "$2")" "$3" "$4"
}
ps_git_sync() { # path ref -> PowerShell fragment
  local path="$1" ref="$2"
  if [[ -z "$ref" ]]; then
    printf "git fetch origin '%s'; if (\$LASTEXITCODE) { exit \$LASTEXITCODE }; git checkout '%s'; if (\$LASTEXITCODE) { exit \$LASTEXITCODE }; git pull --ff-only origin '%s'; if (\$LASTEXITCODE) { exit \$LASTEXITCODE }; " \
      "$FLEET_BRANCH" "$FLEET_BRANCH" "$FLEET_BRANCH"
  elif [[ "$ref" == "HEAD" ]]; then
    printf ''
  else
    printf "git fetch origin '%s'; if (\$LASTEXITCODE) { git fetch origin '%s'; if (\$LASTEXITCODE) { exit \$LASTEXITCODE } }; git checkout --detach '%s'; if (\$LASTEXITCODE) { exit \$LASTEXITCODE }; " "$ref" "$FLEET_BRANCH" "$ref"
  fi
}
ps_mouser_sync() { # path ref -> PowerShell fragment (empty only for HEAD / mouser disabled)
  local path="$1" ref="$2" branch="${FLEET_MOUSER_BRANCH:-$FLEET_BRANCH}"
  local url="${FLEET_MOUSER_FORK_URL:-https://github.com/hughesyadaddy/Mouser.git}" g
  [[ "$DEPLOY_MOUSER" == 1 && "$ref" != "HEAD" ]] || return 0
  g="git -C '${path}'"
  printf "if (Test-Path '%s/.git') { %s remote get-url fork; if (\$LASTEXITCODE) { %s remote add fork '%s'; if (\$LASTEXITCODE) { exit \$LASTEXITCODE } }; %s fetch fork '%s'; if (\$LASTEXITCODE) { exit \$LASTEXITCODE }; " \
    "$path" "$g" "$g" "$url" "$g" "${ref:-$branch}"
  if [[ -z "$ref" ]]; then
    printf "%s checkout '%s'; if (\$LASTEXITCODE) { exit \$LASTEXITCODE }; %s pull --ff-only fork '%s'; if (\$LASTEXITCODE) { exit \$LASTEXITCODE } }; " "$g" "$branch" "$g" "$branch"
  else
    printf "%s checkout --detach '%s'; if (\$LASTEXITCODE) { exit \$LASTEXITCODE } }; " "$g" "$ref"
  fi
}

# Remote side of the password transport (macOS seats only). The first stdin
# line becomes a plain shell variable -- before the prelude's `set -u`, so an
# empty/absent line leaves it empty -- and reaches ONLY the seat script, as
# per-command environment on its invocation (git never sees it).
sh_password_read() {
  printf 'IFS= read -rs FLEET_SEAT_PASSWORD || FLEET_SEAT_PASSWORD=""; '
}
sh_password_env() {
  printf 'DESKFLOW_KEYCHAIN_PASSWORD="$FLEET_SEAT_PASSWORD" DESKFLOW_SUDO_PASSWORD="$FLEET_SEAT_PASSWORD" '
}

# run_ssh target cmd -> exit code (255 = unreachable). With SEAT_PW set (a
# macOS seat with a FLEET_SEAT_PASSWORD_<id> line) the password rides on the
# session's stdin; the remote command consumes exactly that one line
# (sh_password_read). It is never an argument of ssh or of the remote
# command, and xtrace is off while it is expanded. printf may lose the race
# against an ssh that exits at once (unreachable host): a SIGPIPE there must
# not turn ssh's own exit code into 141 under pipefail.
run_ssh() {
  local rc=0
  if [[ -n "$SEAT_PW" ]]; then
    xtrace_off
    { printf '%s\n' "$SEAT_PW" || true; } 2>/dev/null | ssh -o BatchMode=yes "$1" "$2" || rc=$? # fleet:allow printf SIGPIPE when ssh exits first; ssh's status is what counts
    xtrace_restore
  else
    ssh -o BatchMode=yes "$1" "$2" || rc=$?
  fi
  return "$rc"
}

# ---------------------------------------------------------------------------
# Per-host deploy. Sets HOST_RC / HOST_RESULT.
# ---------------------------------------------------------------------------
HOST_RC=0
HOST_RESULT=""
# Settings-survival verdict printed by the seat scripts (fleet-deploy-macos.sh
# deploy_mouser / fleet-deploy-windows.ps1 Deploy-Mouser) as a
# `FLEET_SETTINGS=ok|changed|FAIL` line: ok = Mouser's config hashes were
# identical at all three checkpoints, changed = only an allowed migration
# (.version strictly increased, pre-deploy keys all preserved), FAIL = the
# deploy lost or rewrote settings (the seat script also exits non-zero).
HOST_SETTINGS="-"

# Run a seat command, mirroring its output to the terminal and to $1 so the
# machine-readable markers can be read back. pipefail keeps the command's
# exit code (tee always exits 0).
run_logged() { # logfile cmd... -> exit code of cmd
  local log="$1"; shift
  local rc=0
  "$@" 2>&1 | tee -a "$log" || rc=$?
  return "$rc"
}

settings_from_log() { # logfile -> ok|changed|FAIL|-
  local v
  v="$(grep -o 'FLEET_SETTINGS=[A-Za-z]*' "$1" 2>/dev/null | tail -n 1 | cut -d= -f2)"
  printf '%s' "${v:--}"
}

deploy_host() { # index dref mref
  local i="$1" dref="$2" mref="$3"
  local id="${P_ID[$i]}" target="${P_TARGET[$i]}" os="${P_OS[$i]}"
  local dpath="${P_DESKFLOW[$i]}" mpath="${P_MOUSER[$i]}" rc=0 cmd log
  HOST_RC=0; HOST_RESULT="ok"; HOST_SETTINGS="-"
  # Only a macOS deploy (not a pull, never Windows) is handed a password.
  SEAT_PW=""
  log="$(mktemp "${TMPDIR:-/tmp}/fleet-deploy-${id}.XXXXXX")"

  if [[ "$target" == "local" ]]; then
    [[ "$os" == "macos" ]] || die "local seat '$id' is $os — run scripts/fleet-deploy.ps1 there"
    dpath="${dpath/#\~/$HOME}"; mpath="${mpath/#\~/$HOME}"
    if [[ "$PULL_ONLY" == 1 ]]; then
      echo ">>> pull: $id (local)"
      cmd="$(sh_git_sync "$dpath" "$dref")$(sh_mouser_sync "$mpath" "$mref") && git log -1 --oneline"
    else
      echo ">>> LOCAL deploy: $id"
      cmd="$(sh_exports "$dpath" "$mpath" "$dref" "$mref"); $(sh_git_sync "$dpath" "$dref")$(sh_mouser_sync "$mpath" "$mref") && bash '${ROOT}/scripts/fleet-deploy-macos.sh'"
      load_seat_password "$id"
    fi
    if [[ -n "$SEAT_PW" ]]; then
      # In-process exports for the local seat script; gone again right after.
      xtrace_off
      export DESKFLOW_KEYCHAIN_PASSWORD="$SEAT_PW" DESKFLOW_SUDO_PASSWORD="$SEAT_PW"
      xtrace_restore
      echo "    seat password: FLEET_SEAT_PASSWORD_$(lower "$id") from $DOTENV (in-process; unattended signing + root steps)"
    fi
    run_logged "$log" bash -euo pipefail -c "$cmd" || rc=$?
    unset DESKFLOW_KEYCHAIN_PASSWORD DESKFLOW_SUDO_PASSWORD
    SEAT_PW=""
    if [[ "$rc" != 0 ]]; then HOST_RC="$rc"; HOST_RESULT="fail($rc)"; fi
    HOST_SETTINGS="$(settings_from_log "$log")"; rm -f "$log"
    return 0
  fi

  if [[ "$os" == "windows" ]]; then
    echo ">>> SSH deploy: $id ($target)"
    if [[ "$PULL_ONLY" == 1 ]]; then
      cmd="cd /d \"${dpath}\" && git fetch origin ${FLEET_BRANCH} && git checkout ${FLEET_BRANCH} && git pull --ff-only origin ${FLEET_BRANCH} && git log -1 --oneline"
    else
      local ps
      ps="\$ErrorActionPreference='Stop'; "
      ps+="\$env:FLEET_BRANCH='${FLEET_BRANCH}'; \$env:FLEET_MOUSER_BRANCH='${FLEET_MOUSER_BRANCH:-$FLEET_BRANCH}'; \$env:FLEET_DEPLOY_DESKFLOW='${DEPLOY_DESKFLOW}'; \$env:FLEET_DEPLOY_MOUSER='${DEPLOY_MOUSER}'; "
      ps+="\$env:FLEET_DESKFLOW_ROOT='${dpath}'; \$env:FLEET_MOUSER_ROOT='${mpath}'; \$env:FLEET_SKIP_GIT_PULL='1'; "
      ps+="\$env:FLEET_DESKFLOW_REF='${dref}'; \$env:FLEET_MOUSER_REF='${mref}'; "
      ps+="Set-Location '${dpath}'; $(ps_git_sync "$dpath" "$dref")$(ps_mouser_sync "$mpath" "$mref")"
      ps+="& '${dpath}/scripts/fleet-deploy-windows.ps1'; exit \$LASTEXITCODE"
      cmd="powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"$ps\""
    fi
  else
    echo ">>> SSH deploy: $id ($target)"
    # Non-interactive SSH shells skip login profiles; put Homebrew on PATH
    # only when brew is not already resolvable (same rule as the seat script).
    local prelude
    prelude="set -euo pipefail; if ! command -v brew >/dev/null 2>&1; then if [ -x /opt/homebrew/bin/brew ]; then eval \"\$(/opt/homebrew/bin/brew shellenv)\"; elif [ -x /usr/local/bin/brew ]; then eval \"\$(/usr/local/bin/brew shellenv)\"; else export PATH=\"/opt/homebrew/bin:/usr/local/bin:\${PATH}\"; fi; fi; "
    if [[ "$PULL_ONLY" == 1 ]]; then
      cmd="${prelude}$(sh_git_sync "$dpath" "$dref")$(sh_mouser_sync "$mpath" "$mref") && git log -1 --oneline"
    else
      load_seat_password "$id"
      local seat_run="bash scripts/fleet-deploy-macos.sh"
      if [[ -n "$SEAT_PW" ]]; then
        prelude="$(sh_password_read)${prelude}"
        seat_run="$(sh_password_env)${seat_run}"
        echo "    seat password: FLEET_SEAT_PASSWORD_$(lower "$id") from $DOTENV (on the SSH session's stdin; unattended signing + root steps)"
      fi
      cmd="${prelude}$(sh_exports "$dpath" "$mpath" "$dref" "$mref"); $(sh_git_sync "$dpath" "$dref")$(sh_mouser_sync "$mpath" "$mref") && ${seat_run}"
    fi
  fi

  run_logged "$log" run_ssh "$target" "$cmd" || rc=$?
  SEAT_PW=""
  if [[ "$rc" == 255 ]]; then HOST_RC=255; HOST_RESULT="unreachable(ssh 255)"
  elif [[ "$rc" != 0 ]]; then HOST_RC="$rc"; HOST_RESULT="fail($rc)"; fi
  HOST_SETTINGS="$(settings_from_log "$log")"; rm -f "$log"
  return 0
}

head_commit() { # index path -> sha or "unknown"
  local i="$1" path="$2" c="" target
  target="${P_TARGET[$i]}"
  if [[ "$target" == "local" ]]; then
    path="${path/#\~/$HOME}"
    c="$(git -C "$path" rev-parse HEAD 2>/dev/null)" || c=""
  else
    c="$(ssh -o BatchMode=yes "$target" "git -C \"$(sh_path "$path")\" rev-parse HEAD" 2>/dev/null)" || c=""
  fi
  c="$(printf '%s' "$c" | tr -d '\r' | tail -n 1)"
  printf '%s' "${c:-unknown}"
}

health_host() { # id -> exit code of tools/fleet-health --host id (0 when absent)
  [[ -x "$HEALTH" ]] || return 0
  "$HEALTH" --host "$1" --env "$ENV_FILE"
}

# Fold fleet-health's real JSON ({ok, results:[{host,check,status,detail}]})
# into one row per host: signedBy = detail of `sign` (Windows: `authenticode`),
# apple/adhoc/hardened = the `apple=N adhoc=N hardened=true|false` tail that
# tools/fleet-health check_sign appends to its detail (Windows: "-"),
# tcc/mesh = status of those checks (any FAIL leg fails mesh), ok = no FAIL for
# that host. The legacy {hosts:[{id,signedBy,tcc,mesh,ok}]} shape is still accepted.
HEALTH_FOLD_JQ='
  def signcounts($d):
    (($d // "") | capture("apple=(?<apple>[0-9]+) adhoc=(?<adhoc>[0-9]+) hardened=(?<hardened>true|false)")?) // {};
  def fold($h):
    [.results[] | select(.host == $h)] as $rs
    | if ($rs | length) == 0 then {}
      else (($rs | map(select(.check == "sign" and .status != "SKIP")) | .[0].detail) // null) as $sd
      | {
        signedBy: (($rs | map(select((.check == "sign" or .check == "authenticode") and .status != "SKIP")) | .[0].detail) // "-"),
        tcc: (($rs | map(select(.check == "tcc")) | .[0].status) // "-"),
        mesh: (($rs | map(select(.check == "mesh")) | if length == 0 then null elif any(.status == "FAIL") then "FAIL" else .[0].status end) // "-"),
        ok: ($rs | all(.status != "FAIL"))
      } + signcounts($sd) end;
  (if (.results? // null) != null then fold($h)
   elif (.hosts? // null) != null then ((.hosts | map(select((.id // .host) == $h)) | .[0]) // {})
   else (.[$h] // {}) end)
  | [(.signedBy // .signed_by // "-"), (.apple // "-"), (.adhoc // "-"), (.hardened // "-"),
     (.tcc // "-"), (.mesh // "-"), (if has("ok") then (.ok|tostring) else "-" end)]
  | map(tostring) | join("\t")'

# ---------------------------------------------------------------------------
# Main loop
# ---------------------------------------------------------------------------
R_ID=(); R_APP=(); R_COMMIT=(); R_RESULT=(); R_SIGNED=(); R_TCC=(); R_MESH=(); R_TARGET=()
R_APPLE=(); R_ADHOC=(); R_HARD=(); R_SETTINGS=()
ALL_OK=1

# $6 = settings verdict of the seat run (ok | changed | FAIL | -), see deploy_host.
add_row() { R_ID+=("$1"); R_TARGET+=("$2"); R_APP+=("$3"); R_COMMIT+=("$4"); R_RESULT+=("$5"); R_SIGNED+=("-"); R_TCC+=("-"); R_MESH+=("-"); R_APPLE+=("-"); R_ADHOC+=("-"); R_HARD+=("-"); R_SETTINGS+=("${6:--}"); }

apps_enabled() {
  local a=()
  [[ "$DEPLOY_DESKFLOW" == 1 ]] && a+=(deskflow)
  [[ "$DEPLOY_MOUSER" == 1 ]] && a+=(mouser)
  [[ ${#a[@]} -gt 0 ]] || die "nothing to deploy: both FLEET_DEPLOY_DESKFLOW and FLEET_DEPLOY_MOUSER are 0"
  printf '%s\n' "${a[@]}"
}
APPS="$(apps_enabled)"

if [[ "$ROLLBACK" == 1 ]]; then
  [[ -f "$LAST_GOOD" ]] || die "--rollback: $LAST_GOOD does not exist (no successful deploy recorded yet)"
fi

for i in "${!P_ID[@]}"; do
  selected "$i" || continue
  id="${P_ID[$i]}"
  dref="$REF"; mref="$REF"
  if [[ "$ROLLBACK" == 1 ]]; then
    for app in $APPS; do
      c="$(last_good_commit "$id" "$app")" || die "--rollback: no last-good commit for $id/$app in $LAST_GOOD"
      if [[ "$app" == deskflow ]]; then dref="$c"; else mref="$c"; fi
    done
    echo ">>> rollback $id: deskflow=${dref:--} mouser=${mref:--}"
  fi

  deploy_host "$i" "$dref" "$mref"
  result="$HOST_RESULT"
  settings="$HOST_SETTINGS"
  # A seat reporting FAIL for settings survival never counts as a good deploy,
  # even if its script somehow exited 0.
  if [[ "$settings" == "FAIL" && "$result" == "ok" ]]; then result="settings-fail"; fi
  if [[ "$HOST_RC" == 0 && "$PULL_ONLY" == 0 && "$result" == "ok" ]]; then
    if health_host "$id"; then
      for app in $APPS; do
        if [[ "$app" == deskflow ]]; then c="$(head_commit "$i" "${P_DESKFLOW[$i]}")"; else c="$(head_commit "$i" "${P_MOUSER[$i]}")"; fi
        [[ "$c" != unknown ]] && record_last_good "$id" "$app" "$c"
        add_row "$id" "${P_TARGET[$i]}" "$app" "$c" "$result" "$( [[ "$app" == mouser ]] && printf '%s' "$settings" || printf -- '-' )"
      done
      continue
    fi
    result="unhealthy"
  fi
  [[ "$result" == "ok" ]] || ALL_OK=0
  for app in $APPS; do
    c="-"
    [[ "$HOST_RC" == 0 ]] && { if [[ "$app" == deskflow ]]; then c="$(head_commit "$i" "${P_DESKFLOW[$i]}")"; else c="$(head_commit "$i" "${P_MOUSER[$i]}")"; fi; }
    add_row "$id" "${P_TARGET[$i]}" "$app" "$c" "$result" "$( [[ "$app" == mouser ]] && printf '%s' "$settings" || printf -- '-' )"
  done
done

# ---------------------------------------------------------------------------
# Self-test: fleet-wide health, folded into the rows.
# ---------------------------------------------------------------------------
if [[ "$SELF_TEST" == 1 ]]; then
  if [[ -x "$HEALTH" ]]; then
    hj=""
    if hj="$("$HEALTH" --check all --host all --json --env "$ENV_FILE")"; then :; else ALL_OK=0; warn "fleet-health --check all reported failures"; fi
    if [[ -n "$hj" ]] && printf '%s' "$hj" | jq -e . >/dev/null 2>&1; then
      for r in "${!R_ID[@]}"; do
        line="$(printf '%s' "$hj" | jq -r --arg h "${R_ID[$r]}" "$HEALTH_FOLD_JQ")"
        IFS=$'\t' read -r s ap ad hd t m ok <<<"$line"
        R_SIGNED[r]="$s"; R_APPLE[r]="$ap"; R_ADHOC[r]="$ad"; R_HARD[r]="$hd"; R_TCC[r]="$t"; R_MESH[r]="$m"
        # An ad-hoc Mach-O or an unhardened first-party binary on a Mac seat is
        # a failed deploy regardless of how fleet-health scored the host.
        if [[ "$ad" != "-" && "$ad" -gt 0 ]] || [[ "$hd" == "false" ]]; then ok=false; fi
        if [[ "$ok" == "false" ]]; then ALL_OK=0; [[ "${R_RESULT[$r]}" == ok ]] && R_RESULT[r]="unhealthy"; fi
      done
    fi
  else
    warn "tools/fleet-health not found — self-test cannot verify health"
    ALL_OK=0
  fi
fi

# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------
echo
printf '%-12s | %-8s | %-12s | %-24s | %-5s | %-5s | %-5s | %-6s | %-6s | %-8s | %s\n' host app commit signed-by apple adhoc hard tcc mesh settings result
printf '%-12s-+-%-8s-+-%-12s-+-%-24s-+-%-5s-+-%-5s-+-%-5s-+-%-6s-+-%-6s-+-%-8s-+-%s\n' ------------ -------- ------------ ------------------------ ----- ----- ----- ------ ------ -------- ------
for r in "${!R_ID[@]}"; do
  printf '%-12s | %-8s | %-12.12s | %-24.24s | %-5.5s | %-5.5s | %-5.5s | %-6.6s | %-6.6s | %-8.8s | %s\n' \
    "${R_ID[$r]}" "${R_APP[$r]}" "${R_COMMIT[$r]}" "${R_SIGNED[$r]}" "${R_APPLE[$r]}" "${R_ADHOC[$r]}" "${R_HARD[$r]}" "${R_TCC[$r]}" "${R_MESH[$r]}" "${R_SETTINGS[$r]}" "${R_RESULT[$r]}"
done

if [[ -n "$JSON_OUT" ]]; then
  rows=""
  for r in "${!R_ID[@]}"; do
    rows+="$(jq -cn --arg id "${R_ID[$r]}" --arg t "${R_TARGET[$r]}" --arg a "${R_APP[$r]}" --arg c "${R_COMMIT[$r]}" \
      --arg s "${R_SIGNED[$r]}" --arg tcc "${R_TCC[$r]}" --arg m "${R_MESH[$r]}" --arg res "${R_RESULT[$r]}" \
      --arg ap "${R_APPLE[$r]}" --arg ad "${R_ADHOC[$r]}" --arg hd "${R_HARD[$r]}" --arg st "${R_SETTINGS[$r]}" \
      '{id:$id,target:$t,app:$a,commit:$c,signedBy:$s,apple:$ap,adhoc:$ad,hardened:$hd,tcc:$tcc,mesh:$m,settings:$st,result:$res}')"$'\n'
  done
  emit_json "$(printf '%s' "$rows" | jq -cs --argjson ok "$([[ "$ALL_OK" == 1 ]] && echo true || echo false)" '{ok:$ok,hosts:.}')"
fi

if [[ "$ALL_OK" == 1 ]]; then
  echo "=== Fleet deploy complete ==="
  exit 0
fi
echo "=== Fleet deploy FAILED on at least one host ===" >&2
exit 1
