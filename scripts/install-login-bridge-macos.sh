#!/usr/bin/env bash
# Install the Deskflow login-window LaunchAgent (org.deskflow.vhid-bridge).
# Reads coordination settings from ~/Library/Deskflow/Deskflow.conf, stages the
# plist, and installs it with an admin prompt. Retires the legacy kvm-autoswitch
# login-window agent if present.
#
# After install: log out or restart — LoginWindow agents do not hot-load.
set -euo pipefail

CONF="${DESKFLOW_SETTINGS:-$HOME/Library/Deskflow/Deskflow.conf}"
BRIDGE="/Applications/Deskflow.app/Contents/MacOS/deskflow-vhid-bridge"
AGENT_LABEL="org.deskflow.vhid-bridge"
AGENT_PLIST="/Library/LaunchAgents/${AGENT_LABEL}.plist"
LEGACY_PLIST="/Library/LaunchAgents/com.kvm.autoswitch.loginwindow.plist"
LOG_PATH="/var/log/deskflow-vhid-bridge.log"
SCALE="${DESKFLOW_LOGIN_BRIDGE_SCALE:-}"

usage() {
  cat <<'EOF'
Usage: scripts/install-login-bridge-macos.sh [--scale N] [--dry-run]

Installs /Library/LaunchAgents/org.deskflow.vhid-bridge.plist from Deskflow.conf.
Requires Karabiner DriverKit VirtualHIDDevice and deskflow-vhid-bridge in the app bundle.

Environment:
  DESKFLOW_SETTINGS              Path to Deskflow.conf (default: ~/Library/Deskflow/Deskflow.conf)
  DESKFLOW_LOGIN_BRIDGE_SCALE    Override loginBridgeScale from config
EOF
}

dry_run=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --scale) SCALE="$2"; shift 2 ;;
    --dry-run) dry_run=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
  esac
done

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
SCALE="${SCALE:-4}"

if [[ -z "$computer_name" ]]; then
  echo "error: core/computerName missing in $CONF" >&2
  exit 1
fi
if [[ -z "$port" ]]; then
  port=24800
fi

# Peer entries: name or name=addr[|addr...]. Exclude self; collect host candidates.
hosts=()
IFS=',' read -r -a peer_entries <<< "${peers_raw// /}"
for entry in "${peer_entries[@]}"; do
  entry="${entry#"${entry%%[![:space:]]*}"}"
  entry="${entry%"${entry##*[![:space:]]}"}"
  [[ -z "$entry" ]] && continue
  if [[ "$entry" == *=* ]]; then
    name="${entry%%=*}"
    name="${name%"${name##*[![:space:]]}"}"
    addrs="${entry#*=}"
    if [[ "$name" == "$computer_name" ]] || [[ "$(printf '%s' "$name" | tr '[:upper:]' '[:lower:]')" == "$(printf '%s' "$computer_name" | tr '[:upper:]' '[:lower:]')" ]]; then
      continue
    fi
    IFS='|' read -r -a addr_list <<< "$addrs"
    for addr in "${addr_list[@]}"; do
      addr="${addr#"${addr%%[![:space:]]*}"}"
      addr="${addr%"${addr##*[![:space:]]}"}"
      [[ -z "$addr" ]] && continue
      found=0
      for h in "${hosts[@]:-}"; do
        [[ "$h" == "$addr" ]] && found=1 && break
      done
      [[ "$found" -eq 0 ]] && hosts+=("$addr")
    done
  else
    if [[ "$entry" == "$computer_name" ]] || [[ "$(printf '%s' "$entry" | tr '[:upper:]' '[:lower:]')" == "$(printf '%s' "$computer_name" | tr '[:upper:]' '[:lower:]')" ]]; then
      continue
    fi
    found=0
    for h in "${hosts[@]:-}"; do
      [[ "$h" == "$entry" ]] && found=1 && break
    done
    [[ "$found" -eq 0 ]] && hosts+=("$entry")
  fi
done

if [[ ${#hosts[@]} -eq 0 ]]; then
  echo "error: no coordination peers configured (excluding $computer_name)" >&2
  exit 1
fi

hosts_csv="$(IFS=,; echo "${hosts[*]}")"
staged="$(mktemp "${TMPDIR:-/tmp}/deskflow-login-bridge.XXXXXX.plist")"
trap 'rm -f "$staged"' EXIT

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
    <string>--scale=${SCALE}</string>
  </array>
  <key>LimitLoadToSessionType</key><string>LoginWindow</string>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardOutPath</key><string>${LOG_PATH}</string>
  <key>StandardErrorPath</key><string>${LOG_PATH}</string>
</dict>
</plist>
EOF

echo "== Login bridge agent =="
echo "  Bridge:  $BRIDGE"
echo "  Screen:  $computer_name"
echo "  Port:    $port"
echo "  Scale:   $SCALE"
echo "  Servers: $hosts_csv"
echo "  Plist:   $AGENT_PLIST"
echo
echo "Plist preview:"
plutil -p "$staged"
echo

if [[ "$dry_run" -eq 1 ]]; then
  echo "(dry run — not installing)"
  exit 0
fi

escaped_staged="${staged//\\/\\\\}"
escaped_staged="${escaped_staged//\"/\\\"}"
escaped_agent="${AGENT_PLIST//\\/\\\\}"
escaped_agent="${escaped_agent//\"/\\\"}"
escaped_legacy="${LEGACY_PLIST//\\/\\\\}"
escaped_legacy="${escaped_legacy//\"/\\\"}"

install_cmd="install -d /Library/LaunchAgents && install -m 644 -o root -g wheel '${escaped_staged}' '${escaped_agent}' && rm -f '${escaped_legacy}'; pkill -f '.kvm-autoswitch/coordinator.py' || true"

osascript -e "do shell script \"${install_cmd}\" with administrator privileges"

echo "== Installed $AGENT_PLIST =="
if [[ -f "$LEGACY_PLIST" ]]; then
  echo "warning: legacy plist still present at $LEGACY_PLIST" >&2
else
  echo "Retired legacy kvm-autoswitch login-window agent."
fi
echo
echo "Next: log out or restart, then test from your elected server Mac."
echo "Log:  sudo tail -f $LOG_PATH"
