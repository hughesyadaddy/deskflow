#!/usr/bin/env bash
# safe-deploy-macos.sh -- pull, build (VS Code task recipe), sign, verify, install Deskflow.
# NEVER touches Deskflow/Mouser settings, never re-enables converge/prio/coordination.
# Usage: scripts/safe-deploy-macos.sh [--source <git-url-or-path>] [--branch <b>] [--no-pull]
#   --source: where to pull from (default: the repo's upstream). e.g. hackintosh:Desktop/deskflow
#   Run on the target Mac itself (GUI login session; codesign needs the login keychain).
set -euo pipefail
REPO="${DESKFLOW_REPO:-$HOME/Desktop/deskflow}"
MOUSER_REPO="${MOUSER_REPO:-$HOME/Desktop/Mouser}"
WORK="${DESKFLOW_DEPLOY_WORK:-$HOME/Library/Caches/deskflow-deploy}"
TEAM=J5KPG8ZR5C; BUNDLE_ID=io.github.hughesyadaddy.deskflow
APP=/Applications/Deskflow.app
SOURCE=""; BRANCH=""; PULL=1
while [ $# -gt 0 ]; do case "$1" in
  --source) SOURCE="$2"; shift 2;; --branch) BRANCH="$2"; shift 2;; --no-pull) PULL=0; shift;;
  *) echo "unknown arg $1"; exit 2;; esac; done
log(){ printf '[safe-deploy] %s\n' "$*"; }
die(){ printf '[safe-deploy] FAIL: %s\n' "$*" >&2; exit 1; }

# ---- 0. snapshot settings (hash + backup) ---------------------------------
TS=$(date +%Y%m%d-%H%M%S); BK="$HOME/Library/Deskflow/deploy-backups/$TS"; mkdir -p "$BK"
SETTINGS=( "$HOME/Library/Deskflow" "$HOME/Library/Application Support/Mouser"
           "$HOME/Library/Preferences/$BUNDLE_ID.plist" "$HOME/Library/LaunchAgents" )
snap(){ for p in "${SETTINGS[@]}"; do [ -e "$p" ] || continue
  find "$p" -type f \( -name '*.conf' -o -name '*.plist' -o -name '*.json' -o -name '*.ini' -o -name '*.token' -o -name '*.pem' \) \
    -not -path '*/deploy-backups/*' -print0 2>/dev/null | sort -z | while IFS= read -r -d '' f; do
      # the GUI rewrites lastVersion= on every new build; that is bookkeeping, not a setting
      printf '%s  %s\n' "$(grep -v '^lastVersion=' "$f" | shasum -a 256 | cut -c1-64)" "$f"; done; done; }
snap > "$BK/settings.sha256"
for p in "${SETTINGS[@]}"; do [ -e "$p" ] && rsync -a --exclude deploy-backups --exclude '*.log' "$p" "$BK/" ; done
log "settings backed up to $BK ($(wc -l < "$BK/settings.sha256") files hashed)"

# ---- 1. pull ---------------------------------------------------------------
cd "$REPO"; BRANCH="${BRANCH:-$(git rev-parse --abbrev-ref HEAD)}"
if [ "$PULL" = 1 ]; then
  [ -z "$(git status --porcelain --untracked-files=no)" ] || die "$REPO has uncommitted changes"
  if [ -n "$SOURCE" ]; then git fetch -q --no-tags "$SOURCE" "$BRANCH" && git merge --ff-only -q FETCH_HEAD
  else git pull --ff-only -q --no-tags; fi
  if [ -d "$MOUSER_REPO/.git" ]; then (cd "$MOUSER_REPO" && [ -z "$(git status --porcelain --untracked-files=no)" ] \
      && git pull --ff-only -q 2>/dev/null && log "Mouser repo pulled") || log "Mouser repo not pulled (dirty/no upstream) -- left as is"; fi
fi
log "deskflow $BRANCH @ $(git log --oneline -1 | cut -c1-70)"

# ---- 2. build: tag-free clone so the build number = commit count ----------
SRC="$WORK/src"; STAGE="$WORK/stage"; mkdir -p "$WORK"
if [ ! -d "$SRC/.git" ]; then git clone -q --no-tags "$REPO" "$SRC"; fi
(cd "$SRC" && git fetch -q --no-tags origin "$BRANCH" && git checkout -q -B "$BRANCH" FETCH_HEAD && git tag -l | xargs -r git tag -d >/dev/null)
cp -p "$REPO/.env" "$SRC/.env" 2>/dev/null || true
cd "$SRC"
# == .vscode/tasks.json "Deskflow: Configure (signed)" + "Deskflow: Build + Sign" ==
set -a; [ -f .env ] && . ./.env; set +a
ID="${DESKFLOW_CODESIGN_ID:-$(security find-identity -v -p codesigning | awk '/Apple Development/{print $2; exit}')}"
[ -n "$ID" ] || die "no codesigning identity"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$(brew --prefix qt);$(brew --prefix openssl@3)" \
  -DAPPLE_CODESIGN_DEV="$ID" -DSKIP_BUILD_TESTS=ON -DBUILD_TESTS=OFF >/dev/null
cmake --build build -j"$(sysctl -n hw.ncpu)" >"$WORK/build.log" 2>&1 || { tail -30 "$WORK/build.log"; die "build failed"; }
rm -rf "$STAGE"; cmake --install build --prefix "$STAGE" >"$WORK/install.log" 2>&1 || die "staged install failed"
NEW="$STAGE/Deskflow.app"

# ---- 3. verify bundle ------------------------------------------------------
[ -x "$NEW/Contents/MacOS/deskflow-core" ] || die "no deskflow-core in bundle"
QT=$(ls "$NEW/Contents/Frameworks" 2>/dev/null | grep -c '^Qt') ; [ "$QT" -ge 10 ] || die "only $QT Qt frameworks"
MB=$(du -sm "$NEW" | cut -f1); [ "$MB" -ge 80 ] || die "bundle only ${MB} MB"
codesign --verify --deep --strict "$NEW" || die "codesign verify failed"
codesign -dv "$NEW" 2>&1 | grep -q "TeamIdentifier=$TEAM" || die "wrong team"
[ "$(/usr/libexec/PlistBuddy -c 'Print CFBundleIdentifier' "$NEW/Contents/Info.plist")" = "$BUNDLE_ID" ] || die "wrong bundle id"
VER=$(/usr/libexec/PlistBuddy -c 'Print CFBundleVersion' "$NEW/Contents/Info.plist")
log "verified $VER: ${MB} MB, $QT Qt frameworks, team $TEAM, $BUNDLE_ID"

# ---- 4. install with rollback (no install-macos.sh: it re-renders prio) -----
OLDVER=$(/usr/libexec/PlistBuddy -c 'Print CFBundleVersion' "$APP/Contents/Info.plist" 2>/dev/null || echo none)
RB="/tmp/Deskflow.app.prev-$OLDVER"
rm -rf "$RB"; [ -d "$APP" ] && mv "$APP" "$RB"
ditto "$NEW" "$APP" || { rm -rf "$APP"; mv "$RB" "$APP"; die "copy failed; rolled back"; }
codesign --verify --deep --strict "$APP" || { rm -rf "$APP"; mv "$RB" "$APP"; die "installed copy invalid; rolled back"; }
U="gui/$(id -u)"
for l in io.github.hughesyadaddy.deskflow-core io.github.hughesyadaddy.deskflow; do
  launchctl kickstart -k "$U/$l" 2>/dev/null || log "note: $l not loaded"; done
log "installed $VER (rollback: $RB -> mv back to $APP)"

# ---- 5. guards: settings unchanged, converge/prio/coordination off --------
sleep 8
snap > "$BK/settings.after.sha256"
if ! diff -q "$BK/settings.sha256" "$BK/settings.after.sha256" >/dev/null; then
  diff "$BK/settings.sha256" "$BK/settings.after.sha256" || true
  die "settings changed during deploy! originals in $BK"; fi
if launchctl list | grep -Eq 'deskflow-(converge|prio)'; then die "converge/prio agent is loaded -- investigate (not touched by this script)"; fi
awk -F= '/^\[/{sec=tolower($0)} sec=="[coordination]" && $1=="enabled" && $2=="true"{f=1} END{exit !f}' "$HOME/Library/Deskflow/Deskflow.conf" 2>/dev/null && die "coordination enabled in Deskflow.conf"
pgrep -fl 'MacOS/deskflow-core' | cut -c1-90 || die "core not running"
log "OK: settings unchanged, converge/prio absent, $VER running"
