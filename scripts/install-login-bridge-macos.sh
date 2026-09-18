#!/usr/bin/env bash
# Install the Deskflow login-window LaunchAgent (org.deskflow.vhid-bridge).
# Reads coordination settings from ~/Library/Deskflow/Deskflow.conf, renders
# the plist, and installs it: directly when already root (ssh + sudo), via an
# admin prompt otherwise. Retires the legacy kvm-autoswitch launchers.
#
# This is the ONLY generator of the bridge plist; the GUI (LoginBridgeManager)
# calls it with --dry-run to compare and without to install.
#
# After install: log out or restart — LoginWindow agents do not hot-load.
set -euo pipefail

# Under `sudo` HOME is root's; the settings belong to the invoking user.
if [[ -n "${SUDO_USER:-}" && -z "${DESKFLOW_SETTINGS:-}" ]]; then
  eval "USER_HOME=~$SUDO_USER"
else
  USER_HOME="$HOME"
fi
CONF="${DESKFLOW_SETTINGS:-$USER_HOME/Library/Deskflow/Deskflow.conf}"
APP="${DESKFLOW_INSTALL_APP:-/Applications/Deskflow.app}"
APP="${APP%/}"
BRIDGE="$APP/Contents/MacOS/deskflow-vhid-bridge"
AGENT_LABEL="org.deskflow.vhid-bridge"
AGENT_PLIST="${DESKFLOW_LOGIN_BRIDGE_PLIST:-/Library/LaunchAgents/${AGENT_LABEL}.plist}"
LEGACY_PLIST="$(dirname "$AGENT_PLIST")/com.kvm.autoswitch.loginwindow.plist"
LEGACY_USER_LABEL="com.kvm.autoswitch"
LOG_PATH="${DESKFLOW_LOGIN_BRIDGE_LOG:-/var/log/deskflow-vhid-bridge.log}"
MACHINE_LOCK_DIR="${DESKFLOW_MACHINE_LOCK_DIR:-/private/var/db/deskflow}"
SCALE="${DESKFLOW_LOGIN_BRIDGE_SCALE:-}"
SCALE_FIXED=0

usage() {
  cat <<'EOF'
Usage: scripts/install-login-bridge-macos.sh [--scale N] [--scale-fixed] [--dry-run]

Installs /Library/LaunchAgents/org.deskflow.vhid-bridge.plist from Deskflow.conf.
Requires Karabiner DriverKit VirtualHIDDevice and deskflow-vhid-bridge in the app bundle.

--dry-run writes the rendered plist to stdout (the summary goes to stderr)
and installs nothing; callers compare it against the installed file.

By default the bridge is started with --calibrate: it measures its own
counts-per-point at startup and corrects every move against the real cursor,
so no --scale is passed. --scale N (or loginBridgeScale in config) is only a
seed unless --scale-fixed is given, which disables calibration entirely.

Server hosts come from [coordination] peers: only the address and LAN
fields of each `name=address|lan|mac|wakeCommand` entry; MAC addresses and
wake commands are never used as hosts.

Environment:
  DESKFLOW_SETTINGS              Path to Deskflow.conf (default: ~/Library/Deskflow/Deskflow.conf)
  DESKFLOW_INSTALL_APP           Bundle holding deskflow-vhid-bridge (default: /Applications/Deskflow.app)
  DESKFLOW_LOGIN_BRIDGE_SCALE    Seed scale (only authoritative with --scale-fixed)
EOF
}

dry_run=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --scale) SCALE="$2"; shift 2 ;;
    --scale=*) SCALE="${1#--scale=}"; shift ;;
    --scale-fixed) SCALE_FIXED=1; shift ;;
    --dry-run) dry_run=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
  esac
done

# Hard structural guard (same class as install-macos.sh): under bats every
# path this script would write must live in a tmp sandbox.
if [[ -n "${BATS_TEST_FILENAME:-}" && "$dry_run" -eq 0 ]]; then
  for _p in "$AGENT_PLIST" "$LOG_PATH" "$MACHINE_LOCK_DIR"; do
    case "$_p" in
      "$TMPDIR"*|/tmp/*|/private/tmp/*|/private/var/folders/*|"${BATS_TMPDIR:-__unset__}"*|\
      "${BATS_RUN_TMPDIR:-__unset__}"*|"${BATS_TEST_TMPDIR:-__unset__}"*|"${BATS_FILE_TMPDIR:-__unset__}"*) ;;
      *) echo "FATAL: running under bats but '$_p' is not inside a tmp sandbox. Aborting." >&2; exit 90 ;;
    esac
  done
  unset _p
fi

if [[ ! -f "$CONF" ]]; then
  echo "error: settings not found at $CONF" >&2
  exit 1
fi
if [[ ! -x "$BRIDGE" ]]; then
  echo "error: bridge binary not found at $BRIDGE — install Deskflow first" >&2
  exit 1
fi

read_ini() {
  local section="$1" key="$2"
  awk -v section="[$section]" -v key="$key" '
    $0 == section { in_section = 1; next }
    /^\[/ { in_section = 0 }
    in_section && $0 ~ "^" key "=" {
      sub("^" key "=", "")
      gsub(/^"|"$/, "")
      print
      exit
    }
  ' "$CONF"
}

computer_name="$(read_ini core computerName)"
port="$(read_ini core port)"
peers_raw="$(read_ini coordination peers)"
if [[ -z "$SCALE" ]]; then
  SCALE="$(read_ini coordination loginBridgeScale)"
fi
if [[ -n "$SCALE" ]] && ! [[ "$SCALE" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
  echo "error: --scale must be a positive number, got '$SCALE'" >&2
  exit 1
fi
scale_args=()
if [[ -n "$SCALE" ]]; then
  scale_args+=("<string>--scale=${SCALE}</string>")
fi
if [[ "$SCALE_FIXED" -eq 1 ]]; then
  if [[ -z "$SCALE" ]]; then
    echo "error: --scale-fixed requires --scale N (or loginBridgeScale in config)" >&2
    exit 1
  fi
  scale_args+=("<string>--scale-fixed</string>")
else
  scale_args+=("<string>--calibrate</string>")
fi
scale_xml="$(printf '    %s\n' "${scale_args[@]}")"

if [[ -z "$computer_name" ]]; then
  echo "error: core/computerName missing in $CONF" >&2
  exit 1
fi
if [[ -z "$port" ]]; then
  port=24800
fi

lower() { printf '%s' "$1" | tr '[:upper:]' '[:lower:]'; }
trim() { local v="$1"; v="${v#"${v%%[![:space:]]*}"}"; printf '%s' "${v%"${v##*[![:space:]]}"}"; }

# A host is a hostname or IP: never a MAC (six hex pairs) and never anything
# with whitespace or shell metacharacters (a wake command).
host_ok() {
  [[ -n "$1" ]] || return 1
  [[ "$1" =~ ^([0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}$ ]] && return 1
  [[ "$1" =~ ^[A-Za-z0-9._:-]+$ ]]
}

hosts=()
add_host() {
  local h="$1" existing
  host_ok "$h" || return 0
  for existing in ${hosts[@]+"${hosts[@]}"}; do
    [[ "$existing" == "$h" ]] && return 0
  done
  hosts+=("$h")
}

# Peer syntax (src/lib/coordination/Peer.h): `name=address[|lan[|mac[|wakeCommand]]]`
# or a bare name/address. Only fields 1-2 after `=` are hosts.
IFS=',' read -r -a peer_entries <<< "$peers_raw"
for entry in ${peer_entries[@]+"${peer_entries[@]}"}; do
  entry="$(trim "$entry")"
  [[ -z "$entry" ]] && continue
  if [[ "$entry" == *=* ]]; then
    name="$(trim "${entry%%=*}")"
    [[ "$(lower "$name")" == "$(lower "$computer_name")" ]] && continue
    IFS='|' read -r addr lan _ <<< "${entry#*=}"
    add_host "$(trim "${addr:-}")"
    add_host "$(trim "${lan:-}")"
  else
    [[ "$(lower "$entry")" == "$(lower "$computer_name")" ]] && continue
    add_host "$entry"
    # Bare machine name: the core also tries <name>.local as the LAN candidate.
    [[ "$entry" == *.* ]] || add_host "$entry.local"
  fi
done

if [[ ${#hosts[@]} -eq 0 ]]; then
  echo "error: no coordination peers configured (excluding $computer_name)" >&2
  exit 1
fi

hosts_csv="$(IFS=,; echo "${hosts[*]}")"
staged="$(mktemp "${TMPDIR:-/tmp}/deskflow-login-bridge.XXXXXX.plist")"
trap 'rm -f "$staged"' EXIT

# ExitTimeOut 3: launchd SIGKILLs the bridge 3 s after SIGTERM at session
# handoff so the user core is not refused (kMsgEBusy) for the default 20 s.
cat >"$staged" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>${AGENT_LABEL}</string>
  <key>ProgramArguments</key>
  <array>
    <string>${BRIDGE}</string>
    <string>${hosts_csv}</string>
    <string>${computer_name}</string>
    <string>${port}</string>
${scale_xml}
  </array>
  <key>LimitLoadToSessionType</key><string>LoginWindow</string>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>ExitTimeOut</key><integer>3</integer>
  <key>StandardOutPath</key><string>${LOG_PATH}</string>
  <key>StandardErrorPath</key><string>${LOG_PATH}</string>
</dict>
</plist>
EOF

{
  echo "== Login bridge agent =="
  echo "  Bridge:  $BRIDGE"
  echo "  Screen:  $computer_name"
  echo "  Port:    $port"
  if [[ "$SCALE_FIXED" -eq 1 ]]; then
    echo "  Scale:   $SCALE (fixed, no calibration)"
  else
    echo "  Scale:   self-calibrating${SCALE:+ (seed $SCALE)}"
  fi
  echo "  Servers: $hosts_csv"
  echo "  Plist:   $AGENT_PLIST"
} >&2

if [[ "$dry_run" -eq 1 ]]; then
  cat "$staged"
  exit 0
fi

# The log holds relayed input from the login window; only root may read it.
# The machine lock dir is where every core/bridge takes the machine-scope lock;
# nothing non-root creates it otherwise (SingleInstanceLock falls back to /tmp).
install_cmd="install -d '$(dirname "$AGENT_PLIST")' && \
install -m 644 '$staged' '$AGENT_PLIST' && \
rm -f '$LEGACY_PLIST' && \
touch '$LOG_PATH' && chmod 600 '$LOG_PATH' && \
install -d -m 1777 '$MACHINE_LOCK_DIR'"

if [[ "$(id -u)" -eq 0 ]]; then
  bash -c "$install_cmd"
else
  escaped="${install_cmd//\\/\\\\}"
  escaped="${escaped//\"/\\\"}"
  osascript -e "do shell script \"${escaped}\" with administrator privileges"
fi

echo "== Installed $AGENT_PLIST =="

# The legacy per-user launcher used to pkill deskflow-core and spawn its own;
# it must not survive next to launchd-owned agents. ~/.kvm-autoswitch stays
# on disk for the operator to delete.
legacy_uid="${SUDO_UID:-$(id -u)}"
if launchctl print "gui/$legacy_uid/$LEGACY_USER_LABEL" >/dev/null 2>&1; then
  launchctl bootout "gui/$legacy_uid/$LEGACY_USER_LABEL" || true # fleet:allow best effort; reported below
  echo "Retired legacy gui/$legacy_uid/$LEGACY_USER_LABEL (delete ~/.kvm-autoswitch by hand)."
fi
if [[ -f "$LEGACY_PLIST" ]]; then
  echo "warning: legacy plist still present at $LEGACY_PLIST" >&2
fi
echo
echo "Next: log out or restart, then test from your elected server Mac."
echo "Log:  sudo tail -f $LOG_PATH"
