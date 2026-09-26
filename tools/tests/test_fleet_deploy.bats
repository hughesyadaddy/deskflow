#!/usr/bin/env bats
# scripts/fleet-deploy.sh controller tests. No real hosts: ssh, hostname, git
# and bash are PATH shims that log what they were asked to do.
#
# Run:  tools/tests/bats/bin/bats tools/tests/test_fleet_deploy.bats

setup() {
  REPO_SRC="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
  WORK="$(mktemp -d "${BATS_TMPDIR:-/tmp}/fleet-deploy.XXXXXX")"
  REPO="$WORK/repo"
  SHIM="$WORK/shim"
  LOG="$WORK/log"
  mkdir -p "$REPO/scripts" "$REPO/tools" "$SHIM" "$LOG" "$WORK/mouser/.git"
  cp "$REPO_SRC/scripts/fleet-deploy.sh" "$REPO/scripts/fleet-deploy.sh"
  : > "$REPO/scripts/fleet-deploy-macos.sh"
  SCRIPT="$REPO/scripts/fleet-deploy.sh"

  cat > "$REPO/scripts/fleet.env" <<EOF
FLEET_BRANCH=main
FLEET_HOSTS="hackintosh macbookpro tiny11"
# "local" here must NOT make hackintosh local — the seat is macbookpro.
FLEET_SSH_hackintosh=local
FLEET_SSH_macbookpro=macbookpro
FLEET_SSH_tiny11=tiny11
FLEET_SSH_USER_hackintosh=alex
FLEET_SSH_USER_tiny11=alexh
FLEET_DEPLOY_DESKFLOW=1
FLEET_DEPLOY_MOUSER=1
FLEET_DESKFLOW_PATH_macos=$REPO
FLEET_MOUSER_PATH_macos=$WORK/mouser
FLEET_DESKFLOW_PATH_windows=C:/Users/alexh/Desktop/deskflow
FLEET_MOUSER_PATH_windows=C:/Users/alexh/Desktop/Mouser
EOF

  # --- shims -------------------------------------------------------------
  cat > "$SHIM/hostname" <<'EOF'
#!/bin/sh
echo "${SHIM_HOSTNAME:-MacBookPro}"
EOF
  cat > "$SHIM/ssh" <<'EOF'
#!/bin/sh
# drop options
while [ $# -gt 0 ]; do
  case "$1" in
    -o) shift 2 ;;
    -t|-T|-q) shift ;;
    *) break ;;
  esac
done
target="$1"; shift
printf '%s\t%s\n' "$target" "$*" >> "$SHIM_LOG/ssh.log"
# A piped stdin is the controller's password transport: record it per target
# (under bats a test's stdin is a socket, so this reads only the pipe case).
if [ -p /dev/stdin ]; then cat > "$SHIM_LOG/ssh-stdin.$target"; fi
case " ${SHIM_SSH_DOWN:-} " in *" $target "*) exit 255 ;; esac
case " ${SHIM_SSH_FAIL:-} " in *" $target "*) exit 7 ;; esac
# SHIM_SSH_EXEC=1: behave like the far end and run the remote command through
# SHIM_SSH_SHELL (default /bin/sh; the fleet's login shell is zsh) with the
# recorded stdin, so the read -rs preamble, the git shims and the seat-script
# stub in $REPO all really execute.
if [ -n "${SHIM_SSH_EXEC:-}" ]; then
  case "$*" in *fleet-deploy-macos.sh*)
    if [ -f "$SHIM_LOG/ssh-stdin.$target" ]; then
      "${SHIM_SSH_SHELL:-/bin/sh}" -c "$*" < "$SHIM_LOG/ssh-stdin.$target"
    else
      "${SHIM_SSH_SHELL:-/bin/sh}" -c "$*" < /dev/null
    fi
    exit $? ;;
  esac
fi
case "$*" in *rev-parse*) echo "cafe0000$(printf '%s' "$target" | cksum | cut -c1-8)" ;; esac
# A deploy command's seat output: SHIM_SSH_SETTINGS_<host> fakes the
# FLEET_SETTINGS=... marker the per-OS scripts print for Mouser.
case "$*" in *fleet-deploy-*) h="${target#*@}"; v="$(eval "printf '%s' \"\${SHIM_SSH_SETTINGS_$h:-}\"")"; [ -n "$v" ] && echo "== seat output ==" && echo "FLEET_SETTINGS=$v" ;; esac
exit 0
EOF
  cat > "$SHIM/git" <<'EOF'
#!/bin/sh
printf 'git %s\n' "$*" >> "$SHIM_LOG/git.log"
case "$*" in *rev-parse*) echo "beef000000000000000000000000000000000001" ;; esac
exit 0
EOF
  cat > "$SHIM/bash" <<'EOF'
#!/bin/sh
# Record any inner `bash -c '<cmd>'` and `bash <script>` invocations, then run
# the -c command through the real bash so the git/ssh shims see it.
printf 'bash %s\n' "$*" >> "$SHIM_LOG/bash.log"
if [ "$1" = "-euo" ] && [ "$3" = "-c" ]; then
  printf '%s\n' "$4" >> "$SHIM_LOG/local-cmd.log"
  exec "$REAL_BASH" -euo pipefail -c "$4"
fi
# The local seat script (a stub in $REPO/scripts) really runs so a test can
# make it print the FLEET_SETTINGS marker.
case "$1" in *fleet-deploy-macos.sh) exec "$REAL_BASH" "$@" ;; esac
exit 0
EOF
  cat > "$SHIM/brew" <<'EOF'
#!/bin/sh
exit 0
EOF
  chmod +x "$SHIM"/*
  export REAL_BASH="$BASH"
  export SHIM_LOG="$LOG"
  export PATH="$SHIM:$PATH"
  unset FLEET_LOCAL_ID SHIM_SSH_DOWN SHIM_SSH_FAIL SHIM_SSH_EXEC SHIM_SSH_SHELL
  unset DESKFLOW_KEYCHAIN_PASSWORD DESKFLOW_SUDO_PASSWORD
  # Most tests have no .env: the preflight that refuses a Mac seat without a
  # FLEET_SEAT_PASSWORD_<id> line is exercised by its own tests below.
  export FLEET_ALLOW_PROMPTS=1
}

# A stand-in fleet-deploy-macos.sh (local seat via the bash shim, remote seat
# via SHIM_SSH_EXEC) that records the two seat variables and whether any
# FLEET_SEAT_PASSWORD_* line reached its environment.
stub_seat_script() {
  cat > "$REPO/scripts/fleet-deploy-macos.sh" <<'EOF'
#!/usr/bin/env bash
seat=""; for v in ${!FLEET_SEAT_PASSWORD_@}; do seat="$seat $v"; done
printf 'KC=%s SUDO=%s SEAT=%s\n' "${DESKFLOW_KEYCHAIN_PASSWORD:-unset}" "${DESKFLOW_SUDO_PASSWORD:-unset}" "${seat:-none}" >> "$SHIM_LOG/seat-env.log"
echo "== seat script ran =="
EOF
  chmod +x "$REPO/scripts/fleet-deploy-macos.sh"
}

write_dotenv() { # lines... -> $REPO/.env (mode 600)
  printf '%s\n' "$@" > "$REPO/.env"
  chmod 600 "$REPO/.env"
}

# $output / string assertions that FAIL the test: under bash 3.2 a false
# `[[ ... ]]` mid-test is silently ignored by bats; a function returning 1 is not.
out_has() { if [[ "$output" != *"$1"* ]]; then echo "expected in output: $1" >&2; return 1; fi; }
out_lacks() { if [[ "$output" == *"$1"* ]]; then echo "unexpected in output: $1" >&2; return 1; fi; }
str_has() { if [[ "$1" != *"$2"* ]]; then echo "expected in string: $2" >&2; return 1; fi; }
str_lacks() { if [[ "$1" == *"$2"* ]]; then echo "unexpected in string: $2" >&2; return 1; fi; }
file_lacks() { if grep -qF -- "$2" "$1" 2>/dev/null; then echo "unexpected in $1: $2" >&2; return 1; fi; }

teardown() {
  rm -rf "$WORK"
}

run_deploy() { run "$REAL_BASH" "$SCRIPT" "$@"; }

have_real_flock() { command -v flock >/dev/null 2>&1; }

# --- dry-run / planning ------------------------------------------------------

@test "dry-run --json - has the {hosts:[{id,target,order,role}]} shape" {
  run_deploy --dry-run --json -
  [ "$status" -eq 0 ]
  echo "$output" | jq -e '.hosts | length == 3' >/dev/null
  echo "$output" | jq -e 'all(.hosts[]; has("id") and has("target") and has("order") and has("role"))' >/dev/null
  echo "$output" | jq -e '[.hosts[].order] == [1,2,3]' >/dev/null
}

@test "LOCAL_ID comes from hostname (case-insensitive), not FLEET_SSH_x=local" {
  run_deploy --dry-run --json -
  [ "$status" -eq 0 ]
  echo "$output" | jq -e '.hosts[] | select(.id=="macbookpro") | .target == "local"' >/dev/null
  echo "$output" | jq -e '.hosts[] | select(.id=="hackintosh") | .target == "alex@hackintosh"' >/dev/null
  echo "$output" | jq -e '.hosts[] | select(.id=="tiny11") | .target == "alexh@tiny11"' >/dev/null
}

@test "three distinct targets" {
  run_deploy --dry-run --json -
  [ "$status" -eq 0 ]
  echo "$output" | jq -e '.hosts | map(.target) | unique | length == 3' >/dev/null
}

@test "server is last (default hackintosh)" {
  run_deploy --dry-run --json -
  [ "$status" -eq 0 ]
  echo "$output" | jq -e '.hosts | last | .id == "hackintosh" and .role == "server"' >/dev/null
  echo "$output" | jq -e '[.hosts[] | select(.role=="client")] | length == 2' >/dev/null
}

@test "FLEET_ROLE_<id>=server moves the server to the end" {
  echo 'FLEET_ROLE_macbookpro=server' >> "$REPO/scripts/fleet.env"
  run_deploy --dry-run --json -
  [ "$status" -eq 0 ]
  echo "$output" | jq -e '.hosts | last | .id == "macbookpro" and .role == "server"' >/dev/null
  echo "$output" | jq -e '.hosts[0].id == "hackintosh" and .hosts[0].role == "client"' >/dev/null
}

@test "hostname matching no FLEET_HOSTS entry is an error" {
  SHIM_HOSTNAME=stranger run_deploy --dry-run --json -
  [ "$status" -ne 0 ]
  [[ "$output" == *"no FLEET_HOSTS entry matches"* ]]
}

@test "FLEET_LOCAL_ID overrides the hostname" {
  SHIM_HOSTNAME=stranger FLEET_LOCAL_ID=Hackintosh run_deploy --dry-run --json -
  [ "$status" -eq 0 ]
  echo "$output" | jq -e '.hosts[] | select(.id=="hackintosh") | .target == "local"' >/dev/null
}

@test "--host filters the plan and rejects unknown ids" {
  run_deploy --dry-run --json - --host tiny11
  [ "$status" -eq 0 ]
  echo "$output" | jq -e '.hosts | length == 1 and .[0].id == "tiny11"' >/dev/null
  run_deploy --dry-run --host nope
  [ "$status" -ne 0 ]
}

@test "dry-run runs nothing" {
  run_deploy --dry-run --json -
  [ "$status" -eq 0 ]
  [ ! -e "$LOG/ssh.log" ]
  [ ! -e "$LOG/bash.log" ]
  [ ! -e "$REPO/tools/state/deploy.lock.d" ]
}

# --- full deploy -------------------------------------------------------------

@test "every host targeted once: local deployed once, never over ssh, server last" {
  run_deploy --json "$WORK/report.json"
  [ "$status" -eq 0 ]
  # local seat ran the macOS script exactly once and was never an ssh target
  [ "$(grep -c 'fleet-deploy-macos.sh' "$LOG/local-cmd.log")" -eq 1 ]
  ! grep -q 'macbookpro' "$LOG/ssh.log"
  # deploy commands (not the rev-parse queries): tiny11 then hackintosh
  deploys="$(grep -v 'rev-parse' "$LOG/ssh.log" | cut -f1 | tr '\n' ' ')"
  [ "$deploys" = "alexh@tiny11 alex@hackintosh " ]
  # the local deploy happened before the server's remote deploy
  grep -q 'fleet-deploy-windows.ps1' "$LOG/ssh.log"
  grep -q 'fleet-deploy-macos.sh' "$LOG/ssh.log"
  jq -e '.ok == true and (.hosts | length == 6)' "$WORK/report.json" >/dev/null
  jq -e '[.hosts[] | select(.result != "ok")] | length == 0' "$WORK/report.json" >/dev/null
}

@test "remote macOS command exports the env, pulls the branch, and carries no keychain password" {
  run_deploy --host hackintosh
  [ "$status" -eq 0 ]
  cmd="$(grep -v rev-parse "$LOG/ssh.log" | head -n1 | cut -f2-)"
  [[ "$cmd" == *'export FLEET_BRANCH="main"'* ]]
  [[ "$cmd" == *'FLEET_DEPLOY_DESKFLOW="1"'* ]]
  [[ "$cmd" == *'FLEET_DEPLOY_MOUSER="1"'* ]]
  [[ "$cmd" == *'FLEET_RECONFIGURE="0"'* ]]
  [[ "$cmd" == *'FLEET_DESKFLOW_ROOT='* ]]
  [[ "$cmd" == *'FLEET_MOUSER_ROOT='* ]]
  [[ "$cmd" == *'git fetch origin && git checkout "main" && git pull --ff-only origin "main" && bash scripts/fleet-deploy-macos.sh'* ]]
  ! grep -qi 'KEYCHAIN' "$LOG/ssh.log"
}

@test "remote Windows command goes through powershell and propagates \$LASTEXITCODE" {
  run_deploy --host tiny11
  [ "$status" -eq 0 ]
  cmd="$(grep -v rev-parse "$LOG/ssh.log" | head -n1 | cut -f2-)"
  [[ "$cmd" == *'powershell.exe -NoProfile -ExecutionPolicy Bypass -Command'* ]]
  [[ "$cmd" == *'fleet-deploy-windows.ps1'* ]]
  [[ "$cmd" == *'exit $LASTEXITCODE'* ]]
}

@test "ssh exit 255 is reported as unreachable and fails the run" {
  SHIM_SSH_DOWN="alex@hackintosh" run_deploy --json "$WORK/r.json"
  [ "$status" -ne 0 ]
  [[ "$output" == *"unreachable(ssh 255)"* ]]
  jq -e '.ok == false' "$WORK/r.json" >/dev/null
  jq -e '[.hosts[] | select(.id=="hackintosh") | .result] | all(. == "unreachable(ssh 255)")' "$WORK/r.json" >/dev/null
  jq -e '[.hosts[] | select(.id=="tiny11") | .result] | all(. == "ok")' "$WORK/r.json" >/dev/null
}

@test "a non-255 remote failure is reported with its exit code" {
  SHIM_SSH_FAIL="alexh@tiny11" run_deploy --host tiny11
  [ "$status" -ne 0 ]
  [[ "$output" == *"fail(7)"* ]]
}

@test "--deskflow-only / --mouser-only / --app flip the deploy flags" {
  run_deploy --host hackintosh --deskflow-only
  [ "$status" -eq 0 ]
  grep -q 'FLEET_DEPLOY_MOUSER="0"' "$LOG/ssh.log"
  rm "$LOG/ssh.log"
  run_deploy --host hackintosh --app mouser
  [ "$status" -eq 0 ]
  grep -q 'FLEET_DEPLOY_DESKFLOW="0"' "$LOG/ssh.log"
  run_deploy --host hackintosh --app nope
  [ "$status" -ne 0 ]
}

@test "--pull-only syncs git without running the deploy scripts" {
  run_deploy --pull-only
  [ "$status" -eq 0 ]
  ! grep -q 'fleet-deploy-macos.sh' "$LOG/ssh.log"
  ! grep -q 'fleet-deploy-windows.ps1' "$LOG/ssh.log"
  grep -q 'git pull --ff-only origin' "$LOG/ssh.log"
  ! grep -q 'fleet-deploy-macos.sh' "$LOG/local-cmd.log"
  [ ! -e "$REPO/tools/state/last-good.json" ]
}

@test "the per-host table has the eleven columns (signing counts + settings)" {
  run_deploy
  [ "$status" -eq 0 ]
  [[ "$output" == *"host         | app      | commit       | signed-by                | apple | adhoc | hard  | tcc    | mesh   | settings | result"* ]]
  [[ "$output" == *"tiny11       | deskflow | cafe0000"* ]]
  [[ "$output" == *"macbookpro   | mouser   | beef00000000"* ]]
}

# --- Mouser sync (the per-OS scripts run with FLEET_SKIP_GIT_PULL=1) ---------

@test "normal branch deploy fast-forwards Mouser from fork on remote Macs (remote added if missing)" {
  run_deploy --host hackintosh
  [ "$status" -eq 0 ]
  cmd="$(grep -v rev-parse "$LOG/ssh.log" | head -n1 | cut -f2-)"
  [[ "$cmd" == *'FLEET_SKIP_GIT_PULL=1'* ]]
  [[ "$cmd" == *'FLEET_MOUSER_BRANCH="main"'* ]]
  [[ "$cmd" == *'git -C "$HOME/Desktop/Mouser" remote get-url fork >/dev/null 2>&1 || git -C "$HOME/Desktop/Mouser" remote add fork "https://github.com/hughesyadaddy/Mouser.git"'* ]]
  [[ "$cmd" == *'git -C "$HOME/Desktop/Mouser" fetch fork && git -C "$HOME/Desktop/Mouser" checkout "main" && git -C "$HOME/Desktop/Mouser" pull --ff-only fork "main"'* ]]
  # Mouser sync happens before the per-OS script runs
  [[ "${cmd%%bash scripts/fleet-deploy-macos.sh*}" == *'pull --ff-only fork "main"'* ]]
}

@test "normal branch deploy fast-forwards Mouser from fork on the local seat" {
  run_deploy --host macbookpro
  [ "$status" -eq 0 ]
  grep -q "git -C $WORK/mouser fetch fork" "$LOG/git.log"
  grep -q "git -C $WORK/mouser checkout main" "$LOG/git.log"
  grep -q "git -C $WORK/mouser pull --ff-only fork main" "$LOG/git.log"
  grep -q 'fleet-deploy-macos.sh' "$LOG/local-cmd.log"
}

@test "normal branch deploy syncs Mouser on Windows seats inside the powershell command" {
  run_deploy --host tiny11
  [ "$status" -eq 0 ]
  cmd="$(grep -v rev-parse "$LOG/ssh.log" | head -n1 | cut -f2-)"
  [[ "$cmd" == *"\$env:FLEET_MOUSER_BRANCH='main'"* ]]
  [[ "$cmd" == *"if (Test-Path 'C:/Users/alexh/Desktop/Mouser/.git') { git -C 'C:/Users/alexh/Desktop/Mouser' remote get-url fork; if (\$LASTEXITCODE) { git -C 'C:/Users/alexh/Desktop/Mouser' remote add fork 'https://github.com/hughesyadaddy/Mouser.git'"* ]]
  [[ "$cmd" == *"git -C 'C:/Users/alexh/Desktop/Mouser' fetch fork; if (\$LASTEXITCODE) { exit \$LASTEXITCODE }; git -C 'C:/Users/alexh/Desktop/Mouser' checkout 'main'; if (\$LASTEXITCODE) { exit \$LASTEXITCODE }; git -C 'C:/Users/alexh/Desktop/Mouser' pull --ff-only fork 'main'"* ]]
  [[ "${cmd%%fleet-deploy-windows.ps1*}" == *"pull --ff-only fork 'main'"* ]]
}

@test "FLEET_MOUSER_BRANCH overrides the Mouser branch; --mouser-only/--deskflow-only gate the sync" {
  echo 'FLEET_MOUSER_BRANCH=mouser-dev' >> "$REPO/scripts/fleet.env"
  run_deploy --host hackintosh
  [ "$status" -eq 0 ]
  cmd="$(grep -v rev-parse "$LOG/ssh.log" | head -n1 | cut -f2-)"
  [[ "$cmd" == *'FLEET_MOUSER_BRANCH="mouser-dev"'* ]]
  [[ "$cmd" == *'checkout "mouser-dev" && git -C "$HOME/Desktop/Mouser" pull --ff-only fork "mouser-dev"'* ]]
  [[ "$cmd" == *'git checkout "main" && git pull --ff-only origin "main"'* ]]
  rm "$LOG/ssh.log"
  run_deploy --host hackintosh --deskflow-only
  [ "$status" -eq 0 ]
  ! grep -q 'fetch fork' "$LOG/ssh.log"
  rm "$LOG/ssh.log"
  run_deploy --host tiny11 --deskflow-only
  [ "$status" -eq 0 ]
  ! grep -q 'fetch fork' "$LOG/ssh.log"
}

@test "--ref HEAD leaves Mouser untouched; --ref X detaches Mouser at X" {
  run_deploy --host hackintosh --ref HEAD
  [ "$status" -eq 0 ]
  ! grep -q 'fetch fork' "$LOG/ssh.log"
  rm "$LOG/ssh.log"
  run_deploy --host hackintosh --ref v1.2.3
  [ "$status" -eq 0 ]
  cmd="$(grep -v rev-parse "$LOG/ssh.log" | head -n1 | cut -f2-)"
  [[ "$cmd" == *'git -C "$HOME/Desktop/Mouser" fetch fork && git -C "$HOME/Desktop/Mouser" checkout --detach "v1.2.3"'* ]]
  [[ "$cmd" != *'pull --ff-only fork'* ]]
  rm "$LOG/ssh.log"
  run_deploy --host tiny11 --ref v1.2.3
  [ "$status" -eq 0 ]
  cmd="$(grep -v rev-parse "$LOG/ssh.log" | head -n1 | cut -f2-)"
  [[ "$cmd" == *"git -C 'C:/Users/alexh/Desktop/Mouser' checkout --detach 'v1.2.3'"* ]]
}

# --- last-good / rollback ----------------------------------------------------

@test "last-good.json records {host:{app:{commit,ts}}} after a healthy deploy" {
  run_deploy
  [ "$status" -eq 0 ]
  f="$REPO/tools/state/last-good.json"
  [ -f "$f" ]
  jq -e '.macbookpro.deskflow.commit == "beef000000000000000000000000000000000001"' "$f" >/dev/null
  jq -e '.macbookpro.mouser.commit == "beef000000000000000000000000000000000001"' "$f" >/dev/null
  jq -e '.hackintosh.deskflow.commit | startswith("cafe0000")' "$f" >/dev/null
  jq -e '.tiny11.deskflow.ts | test("^[0-9]{4}-[0-9]{2}-[0-9]{2}T")' "$f" >/dev/null
  jq -e 'keys | length == 3' "$f" >/dev/null
}

@test "last-good is only recorded when tools/fleet-health --host passes" {
  cat > "$REPO/tools/fleet-health" <<'EOF'
#!/bin/sh
case "$*" in *"--host tiny11"*) exit 3 ;; esac
exit 0
EOF
  chmod +x "$REPO/tools/fleet-health"
  run_deploy --json "$WORK/r.json"
  [ "$status" -ne 0 ]
  f="$REPO/tools/state/last-good.json"
  jq -e '.macbookpro.deskflow.commit != null and .hackintosh.deskflow.commit != null' "$f" >/dev/null
  jq -e '.tiny11 == null' "$f" >/dev/null
  jq -e '[.hosts[] | select(.id=="tiny11") | .result] | all(. == "unhealthy")' "$WORK/r.json" >/dev/null
}

@test "--rollback checks out the commits recorded in last-good.json" {
  mkdir -p "$REPO/tools/state"
  cat > "$REPO/tools/state/last-good.json" <<'EOF'
{"hackintosh":{"deskflow":{"commit":"d15ea5e0000000000000000000000000deadbeef","ts":"2026-09-16T00:00:00Z"},
               "mouser":{"commit":"a11ce0000000000000000000000000000000c0de","ts":"2026-09-16T00:00:00Z"}},
 "macbookpro":{"deskflow":{"commit":"0ddba11000000000000000000000000000000001","ts":"2026-09-16T00:00:00Z"}}}
EOF
  run_deploy --rollback --host hackintosh
  [ "$status" -eq 0 ]
  cmd="$(grep -v rev-parse "$LOG/ssh.log" | head -n1 | cut -f2-)"
  [[ "$cmd" == *'git checkout --detach "d15ea5e0000000000000000000000000deadbeef"'* ]]
  [[ "$cmd" == *'git -C "$HOME/Desktop/Mouser" fetch fork && git -C "$HOME/Desktop/Mouser" checkout --detach "a11ce0000000000000000000000000000000c0de"'* ]]
  [[ "$cmd" == *'FLEET_DESKFLOW_REF="d15ea5e0000000000000000000000000deadbeef"'* ]]
  [[ "$cmd" == *'FLEET_SKIP_GIT_PULL=1'* ]]
  [[ "$cmd" != *'git pull --ff-only origin'* ]]
  [[ "$cmd" == *'bash scripts/fleet-deploy-macos.sh'* ]]
}

@test "--rollback --app deskflow only needs the deskflow entry and rolls back the local seat" {
  mkdir -p "$REPO/tools/state"
  echo '{"macbookpro":{"deskflow":{"commit":"0ddba11000000000000000000000000000000001","ts":"x"}}}' > "$REPO/tools/state/last-good.json"
  run_deploy --rollback --host macbookpro --app deskflow
  [ "$status" -eq 0 ]
  grep -q 'git checkout --detach 0ddba11000000000000000000000000000000001' "$LOG/git.log"
  grep -q 'fleet-deploy-macos.sh' "$LOG/local-cmd.log"
}

@test "--rollback refuses without last-good for the host/app" {
  run_deploy --rollback --host hackintosh
  [ "$status" -ne 0 ]
  [[ "$output" == *"last-good.json does not exist"* ]]
  mkdir -p "$REPO/tools/state"
  echo '{"macbookpro":{"deskflow":{"commit":"abc","ts":"x"}}}' > "$REPO/tools/state/last-good.json"
  run_deploy --rollback --host hackintosh
  [ "$status" -ne 0 ]
  [[ "$output" == *"no last-good commit for hackintosh/deskflow"* ]]
  [ ! -e "$LOG/ssh.log" ]
}

# --- --ref / --self-test -----------------------------------------------------

@test "--ref HEAD rebuilds what is checked out without pulling; other refs detach" {
  run_deploy --host hackintosh --ref HEAD
  [ "$status" -eq 0 ]
  cmd="$(grep -v rev-parse "$LOG/ssh.log" | head -n1 | cut -f2-)"
  [[ "$cmd" != *'git fetch'* ]]
  [[ "$cmd" == *'FLEET_DESKFLOW_REF="HEAD"'* ]]
  rm "$LOG/ssh.log"
  run_deploy --host tiny11 --ref v1.2.3
  [ "$status" -eq 0 ]
  cmd="$(grep -v rev-parse "$LOG/ssh.log" | head -n1 | cut -f2-)"
  [[ "$cmd" == *"git checkout --detach 'v1.2.3'"* ]]
}

@test "--self-test writes {ok,hosts:[{id,target,app,commit,signedBy,tcc,mesh,result}]} from fleet-health" {
  cat > "$REPO/tools/fleet-health" <<'EOF'
#!/bin/sh
case "$*" in
  *"--check all"*) echo '{"hosts":[{"id":"hackintosh","signedBy":"Apple Development: Alex","tcc":"ok","mesh":"ok","ok":true},{"id":"macbookpro","signedBy":"Apple Development: Alex","tcc":"ok","mesh":"ok","ok":true},{"id":"tiny11","signedBy":"thumb","tcc":"n/a","mesh":"ok","ok":true}]}' ;;
esac
exit 0
EOF
  chmod +x "$REPO/tools/fleet-health"
  run_deploy --self-test --json "$WORK/st.json"
  [ "$status" -eq 0 ]
  jq -e '.ok == true' "$WORK/st.json" >/dev/null
  jq -e 'all(.hosts[]; has("id") and has("target") and has("app") and has("commit") and has("signedBy") and has("tcc") and has("mesh") and has("result"))' "$WORK/st.json" >/dev/null
  jq -e '.hosts[] | select(.id=="hackintosh" and .app=="deskflow") | .signedBy == "Apple Development: Alex" and .tcc == "ok" and .mesh == "ok"' "$WORK/st.json" >/dev/null
  # --self-test implies --ref HEAD: no pulls anywhere
  ! grep -q 'git pull' "$LOG/ssh.log"
  ! grep -q 'git pull' "$LOG/git.log"
}

@test "--self-test exits non-zero when fleet-health marks a host unhealthy" {
  cat > "$REPO/tools/fleet-health" <<'EOF'
#!/bin/sh
case "$*" in
  *"--check all"*) echo '{"hosts":[{"id":"hackintosh","ok":true},{"id":"macbookpro","ok":false,"tcc":"denied"},{"id":"tiny11","ok":true}]}'; exit 1 ;;
esac
exit 0
EOF
  chmod +x "$REPO/tools/fleet-health"
  run_deploy --self-test --json "$WORK/st.json"
  [ "$status" -ne 0 ]
  jq -e '.ok == false' "$WORK/st.json" >/dev/null
  jq -e '[.hosts[] | select(.id=="macbookpro") | .result] | all(. == "unhealthy")' "$WORK/st.json" >/dev/null
  jq -e '.hosts[] | select(.id=="macbookpro" and .app=="deskflow") | .tcc == "denied"' "$WORK/st.json" >/dev/null
}

# Real tools/fleet-health --json shape: {ok, results:[{host,check,status,detail}]}.
write_real_health() { # $1 = exit code, stdin = JSON
  local rc="$1" json
  json="$(cat)"
  cat > "$REPO/tools/fleet-health" <<EOF
#!/bin/sh
printf '%s\n' "\$*" >> "$LOG/health.log"
case "\$*" in
  *"--check all"*) cat <<'JSON'
$json
JSON
    exit $rc ;;
esac
exit 0
EOF
  chmod +x "$REPO/tools/fleet-health"
}

REAL_HEALTH_OK='{"ok": true, "results": [
  {"host":"hackintosh","check":"sign","status":"PASS","detail":"12 Mach-Os signed by Apple Development* team ABCDE12345; apple=12 adhoc=0 hardened=true"},
  {"host":"hackintosh","check":"no-adhoc","status":"PASS","detail":"12 Mach-Os, none ad-hoc"},
  {"host":"hackintosh","check":"identifiers","status":"PASS","detail":"12 identifiers stable/allowlisted"},
  {"host":"hackintosh","check":"tcc","status":"PASS","detail":"4 rows cert-based; AX+ListenEvent granted; --check-permissions ok"},
  {"host":"hackintosh","check":"session","status":"PASS","detail":"io.github.hughesyadaddy.mouser loaded; Deskflow GUI running"},
  {"host":"hackintosh","check":"mesh","status":"PASS","detail":"hackintosh -> macbookpro (macbookpro:24800) ok"},
  {"host":"hackintosh","check":"mesh","status":"PASS","detail":"hackintosh -> tiny11 (tiny11:24800) ok"},
  {"host":"macbookpro","check":"sign","status":"PASS","detail":"12 Mach-Os signed by Apple Development* team ABCDE12345; apple=12 adhoc=0 hardened=true"},
  {"host":"macbookpro","check":"tcc","status":"PASS","detail":"4 rows cert-based"},
  {"host":"macbookpro","check":"mesh","status":"PASS","detail":"macbookpro -> hackintosh (hackintosh:24800) ok"},
  {"host":"macbookpro","check":"mesh","status":"PASS","detail":"macbookpro -> tiny11 (tiny11:24800) ok"},
  {"host":"tiny11","check":"sign","status":"SKIP","detail":"macOS-only check"},
  {"host":"tiny11","check":"tcc","status":"SKIP","detail":"macOS-only check"},
  {"host":"tiny11","check":"authenticode","status":"PASS","detail":"3 files signed by thumbprint 0123"},
  {"host":"tiny11","check":"session","status":"PASS","detail":"alexh Active"},
  {"host":"tiny11","check":"mesh","status":"PASS","detail":"tiny11 -> hackintosh ok"}
]}'

@test "--self-test folds fleet-health's real {ok,results:[{host,check,status,detail}]} shape per host" {
  printf '%s' "$REAL_HEALTH_OK" | write_real_health 0
  run_deploy --self-test --json "$WORK/st.json"
  [ "$status" -eq 0 ]
  jq -e '.ok == true' "$WORK/st.json" >/dev/null
  jq -e '.hosts[] | select(.id=="hackintosh" and .app=="deskflow") | .signedBy == "12 Mach-Os signed by Apple Development* team ABCDE12345; apple=12 adhoc=0 hardened=true" and .tcc == "PASS" and .mesh == "PASS"' "$WORK/st.json" >/dev/null
  # the apple/adhoc/hardened tail of the sign detail becomes three columns
  jq -e '.hosts[] | select(.id=="hackintosh" and .app=="deskflow") | .apple == "12" and .adhoc == "0" and .hardened == "true"' "$WORK/st.json" >/dev/null
  # Windows: `sign` is SKIP, so signed-by falls back to the authenticode detail; no Mach-O counts
  jq -e '.hosts[] | select(.id=="tiny11" and .app=="mouser") | .signedBy == "3 files signed by thumbprint 0123" and .tcc == "SKIP" and .mesh == "PASS" and .result == "ok" and .apple == "-" and .adhoc == "-" and .hardened == "-"' "$WORK/st.json" >/dev/null
  jq -e '[.hosts[] | .result] | all(. == "ok")' "$WORK/st.json" >/dev/null
  [[ "$output" == *"| 12 Mach-Os signed by App | 12    | 0     | true  | PASS   | PASS   | -        | ok"* ]]  # signed-by column is 24 chars wide
}

@test "--self-test fails the run when a Mac seat has adhoc>0 even though fleet-health scored the host ok" {
  printf '%s' "$REAL_HEALTH_OK" | jq -c '
    .results |= map(if .host == "macbookpro" and .check == "sign" then .detail = "libcrypto.3.dylib: Signature=adhoc [apple=11 adhoc=1 hardened=true]" else . end)' \
    | write_real_health 0
  run_deploy --self-test --json "$WORK/st.json"
  [ "$status" -ne 0 ]
  jq -e '.ok == false' "$WORK/st.json" >/dev/null
  jq -e '[.hosts[] | select(.id=="macbookpro") | .result] | all(. == "unhealthy")' "$WORK/st.json" >/dev/null
  jq -e '.hosts[] | select(.id=="macbookpro" and .app=="deskflow") | .adhoc == "1" and .apple == "11" and .hardened == "true"' "$WORK/st.json" >/dev/null
  jq -e '[.hosts[] | select(.id!="macbookpro") | .result] | all(. == "ok")' "$WORK/st.json" >/dev/null
  [[ "$output" == *"| 11    | 1     | true  |"* ]]
}

@test "--self-test fails the run when a Mac seat reports hardened=false (hackintosh flags=0x0 build)" {
  printf '%s' "$REAL_HEALTH_OK" | jq -c '
    .results |= map(if .host == "hackintosh" and .check == "sign" then .detail = "deskflow-prio: not hardened (CodeDirectory flags lack runtime) [apple=12 adhoc=0 hardened=false]" else . end)' \
    | write_real_health 0
  run_deploy --self-test --json "$WORK/st.json"
  [ "$status" -ne 0 ]
  jq -e '.ok == false' "$WORK/st.json" >/dev/null
  jq -e '[.hosts[] | select(.id=="hackintosh") | .result] | all(. == "unhealthy")' "$WORK/st.json" >/dev/null
  jq -e '.hosts[] | select(.id=="hackintosh" and .app=="mouser") | .hardened == "false" and .adhoc == "0"' "$WORK/st.json" >/dev/null
  [[ "$output" == *"| 12    | 0     | false |"* ]]
}

@test "the settings column carries each seat's FLEET_SETTINGS marker on the mouser row (ok / changed)" {
  printf '#!/usr/bin/env bash\necho "== Mouser settings proof =="\necho "FLEET_SETTINGS=changed"\n' >"$REPO/scripts/fleet-deploy-macos.sh"
  SHIM_SSH_SETTINGS_hackintosh=ok SHIM_SSH_SETTINGS_tiny11=ok run_deploy --json "$WORK/r.json"
  [ "$status" -eq 0 ]
  jq -e '.ok == true' "$WORK/r.json" >/dev/null
  jq -e '.hosts[] | select(.id=="macbookpro" and .app=="mouser") | .settings == "changed" and .result == "ok"' "$WORK/r.json" >/dev/null
  jq -e '.hosts[] | select(.id=="hackintosh" and .app=="mouser") | .settings == "ok"' "$WORK/r.json" >/dev/null
  jq -e '.hosts[] | select(.id=="tiny11" and .app=="mouser") | .settings == "ok"' "$WORK/r.json" >/dev/null
  jq -e '[.hosts[] | select(.app=="deskflow") | .settings] | all(. == "-")' "$WORK/r.json" >/dev/null
  [[ "$output" == *"macbookpro   | mouser   | beef00000000 | -                        | -     | -     | -     | -      | -      | changed  | ok"* ]]
  # the seat's own output still reaches the terminal
  [[ "$output" == *"== Mouser settings proof =="* ]]
}

@test "a seat reporting FLEET_SETTINGS=FAIL fails the run even when its script exited 0" {
  SHIM_SSH_SETTINGS_tiny11=FAIL run_deploy --json "$WORK/r.json"
  [ "$status" -ne 0 ]
  jq -e '.ok == false' "$WORK/r.json" >/dev/null
  jq -e '.hosts[] | select(.id=="tiny11" and .app=="mouser") | .settings == "FAIL" and .result == "settings-fail"' "$WORK/r.json" >/dev/null
  jq -e '[.hosts[] | select(.id!="tiny11") | .result] | all(. == "ok")' "$WORK/r.json" >/dev/null
  # no last-good is recorded for a seat that lost settings
  run jq -e '.tiny11' "$REPO/tools/state/last-good.json"
  [ "$status" -ne 0 ]
}

@test "a seat without the marker (no Mouser / old script) shows '-' for settings" {
  run_deploy --deskflow-only --json "$WORK/r.json"
  [ "$status" -eq 0 ]
  jq -e '[.hosts[] | .settings] | all(. == "-")' "$WORK/r.json" >/dev/null
}

@test "--self-test marks a host unhealthy when any of its real results is FAIL (one mesh leg is enough)" {
  printf '%s' "$REAL_HEALTH_OK" | jq -c '
    .ok = false
    | .results |= map(if .host == "macbookpro" and .check == "tcc" then .status = "FAIL" | .detail = "kTCCServiceAccessibility: no deskflow client with auth_value=2" else . end)
    | .results |= map(if .host == "hackintosh" and .detail == "hackintosh -> tiny11 (tiny11:24800) ok" then .status = "FAIL" | .detail = "hackintosh -> tiny11 (tiny11:24800) rc=1" else . end)' \
    | write_real_health 1
  run_deploy --self-test --json "$WORK/st.json"
  [ "$status" -ne 0 ]
  jq -e '.ok == false' "$WORK/st.json" >/dev/null
  jq -e '[.hosts[] | select(.id=="macbookpro") | .result] | all(. == "unhealthy")' "$WORK/st.json" >/dev/null
  jq -e '.hosts[] | select(.id=="macbookpro" and .app=="deskflow") | .tcc == "FAIL"' "$WORK/st.json" >/dev/null
  # first mesh leg PASS, second FAIL -> mesh column is FAIL and the host is unhealthy
  jq -e '.hosts[] | select(.id=="hackintosh" and .app=="deskflow") | .mesh == "FAIL" and .result == "unhealthy"' "$WORK/st.json" >/dev/null
  jq -e '[.hosts[] | select(.id=="tiny11") | .result] | all(. == "ok")' "$WORK/st.json" >/dev/null
}

@test "fleet-health is always invoked with --env pointing at the controller's fleet.env" {
  printf '%s' "$REAL_HEALTH_OK" | write_real_health 0
  run_deploy --self-test
  [ "$status" -eq 0 ]
  # per-host gate (3 hosts) + the fleet-wide --check all
  [ "$(wc -l < "$LOG/health.log" | tr -d ' ')" -eq 4 ]
  [ "$(grep -c -- "--env $REPO/scripts/fleet.env" "$LOG/health.log")" -eq 4 ]
  grep -q -- "--host tiny11 --env" "$LOG/health.log"
  grep -q -- "--check all --host all --json --env" "$LOG/health.log"
}

@test "FLEET_ENV_FILE is honoured by the controller and forwarded to fleet-health" {
  mv "$REPO/scripts/fleet.env" "$WORK/custom.env"
  printf '%s' "$REAL_HEALTH_OK" | write_real_health 0
  FLEET_ENV_FILE="$WORK/custom.env" run_deploy --self-test
  [ "$status" -eq 0 ]
  [ "$(grep -c -- "--env $WORK/custom.env" "$LOG/health.log")" -eq 4 ]
  run_deploy --dry-run
  [ "$status" -ne 0 ]
  [[ "$output" == *"Missing"* ]]
}

@test "--self-test without tools/fleet-health cannot pass" {
  run_deploy --self-test --json "$WORK/st.json"
  [ "$status" -ne 0 ]
  jq -e '.ok == false' "$WORK/st.json" >/dev/null
}

# --- lock --------------------------------------------------------------------

@test "refuses to run while the mkdir lock is held by a live process" {
  if have_real_flock; then skip "flock present: mkdir fallback not exercised on this platform"; fi
  mkdir -p "$REPO/tools/state/deploy.lock.d"
  echo "$$" > "$REPO/tools/state/deploy.lock.d/pid"
  run_deploy
  [ "$status" -ne 0 ]
  [[ "$output" == *"deploy lock held"* ]]
  [ ! -e "$LOG/ssh.log" ]
  [ -d "$REPO/tools/state/deploy.lock.d" ]
}

@test "reclaims a stale mkdir lock whose pid is dead, and releases its own on exit" {
  if have_real_flock; then skip "flock present: mkdir fallback not exercised on this platform"; fi
  mkdir -p "$REPO/tools/state/deploy.lock.d"
  echo "2147483000" > "$REPO/tools/state/deploy.lock.d/pid"
  run_deploy --dry-run
  [ "$status" -eq 0 ]
  run_deploy
  [ "$status" -eq 0 ]
  [[ "$output" == *"reclaiming stale deploy lock"* ]]
  [ ! -e "$REPO/tools/state/deploy.lock.d" ]
}

@test "refuses to run when flock(1) reports the lock held" {
  cat > "$SHIM/flock" <<'EOF'
#!/bin/sh
exit 1
EOF
  chmod +x "$SHIM/flock"
  run_deploy
  [ "$status" -ne 0 ]
  [[ "$output" == *"deploy lock held"* ]]
  [ ! -e "$LOG/ssh.log" ]
}

@test "dry-run never touches the lock" {
  mkdir -p "$REPO/tools/state/deploy.lock.d"
  echo "$$" > "$REPO/tools/state/deploy.lock.d/pid"
  run_deploy --dry-run --json -
  [ "$status" -eq 0 ]
}

# --- seat passwords: FLEET_SEAT_PASSWORD_<id> in the controller's .env ------------

remote_cmd() { grep -v rev-parse "$LOG/ssh.log" | grep "^$1"$'\t' | head -n1 | cut -f2-; }

@test "FLEET_SEAT_PASSWORD_<id> reaches a remote Mac on the SSH session's stdin only (never argv), and only the seat script sees it" {
  write_dotenv "FLEET_SEAT_PASSWORD_hackintosh='hunter 2'"
  stub_seat_script
  SHIM_SSH_EXEC=1 run_deploy --host hackintosh
  [ "$status" -eq 0 ]
  out_has "seat password: FLEET_SEAT_PASSWORD_hackintosh from $REPO/.env (on the SSH session's stdin"
  out_has "== seat script ran =="
  out_lacks "hunter 2"
  # argv of ssh (the remote command) never holds the value; stdin does, as exactly one line
  file_lacks "$LOG/ssh.log" "hunter"
  [ "$(cat "$LOG/ssh-stdin.alex@hackintosh")" = "hunter 2" ]
  [ "$(wc -l < "$LOG/ssh-stdin.alex@hackintosh" | tr -d ' ')" -eq 1 ]
  cmd="$(remote_cmd alex@hackintosh)"
  str_has "$cmd" 'IFS= read -rs FLEET_SEAT_PASSWORD || FLEET_SEAT_PASSWORD=""; set -euo pipefail;'
  str_has "$cmd" '&& DESKFLOW_KEYCHAIN_PASSWORD="$FLEET_SEAT_PASSWORD" DESKFLOW_SUDO_PASSWORD="$FLEET_SEAT_PASSWORD" bash scripts/fleet-deploy-macos.sh'
  # the far end: git sync ran without the variables, the seat script got both and no FLEET_SEAT_PASSWORD_* line
  [ "$(cat "$LOG/seat-env.log")" = "KC=hunter 2 SUDO=hunter 2 SEAT=none" ]
  grep -q '^git fetch origin main' "$LOG/git.log"
}

@test "the remote preamble also works under zsh (the seats' login shell)" {
  command -v zsh >/dev/null 2>&1 || skip "zsh not installed"
  write_dotenv 'FLEET_SEAT_PASSWORD_hackintosh="p@ss #word"'
  stub_seat_script
  SHIM_SSH_EXEC=1 SHIM_SSH_SHELL="$(command -v zsh)" run_deploy --host hackintosh
  [ "$status" -eq 0 ]
  [ "$(cat "$LOG/seat-env.log")" = "KC=p@ss #word SUDO=p@ss #word SEAT=none" ]
  file_lacks "$LOG/ssh.log" "p@ss"
}

@test ".env values parse like source: quotes stripped, an unquoted value ends at a trailing comment, the last line wins" {
  write_dotenv "FLEET_SEAT_PASSWORD_hackintosh=first" "export FLEET_SEAT_PASSWORD_hackintosh=plain # not part of it"
  run_deploy --host hackintosh
  [ "$status" -eq 0 ]
  [ "$(cat "$LOG/ssh-stdin.alex@hackintosh")" = "plain" ]
}

@test "a seat without a line gets nothing on stdin and no preamble; Windows seats never get one" {
  write_dotenv "FLEET_SEAT_PASSWORD_macbookpro=local-pw" "FLEET_SEAT_PASSWORD_tiny11=never-sent"
  run_deploy --host hackintosh
  [ "$status" -eq 0 ]
  [ ! -e "$LOG/ssh-stdin.alex@hackintosh" ]
  cmd="$(remote_cmd alex@hackintosh)"
  str_lacks "$cmd" "read -rs"
  str_lacks "$cmd" "PASSWORD"
  out_lacks "seat password"

  rm -f "$LOG/ssh.log"
  run_deploy --host tiny11
  [ "$status" -eq 0 ]
  [ ! -e "$LOG/ssh-stdin.alexh@tiny11" ]
  cmd="$(remote_cmd alexh@tiny11)"
  str_lacks "$cmd" "PASSWORD"
  str_lacks "$cmd" "never-sent"
  out_lacks "seat password"
  file_lacks "$LOG/ssh.log" "never-sent"
}

@test "the local seat gets the two in-process exports (and no FLEET_SEAT_PASSWORD_* line); they are gone once its script returns" {
  write_dotenv "FLEET_SEAT_PASSWORD_macbookpro=local-pw" "FLEET_SEAT_PASSWORD_hackintosh=remote-pw"
  stub_seat_script
  run_deploy --host macbookpro
  [ "$status" -eq 0 ]
  [ "$(cat "$LOG/seat-env.log")" = "KC=local-pw SUDO=local-pw SEAT=none" ]
  out_has "seat password: FLEET_SEAT_PASSWORD_macbookpro from $REPO/.env (in-process"
  out_lacks "local-pw"
  file_lacks "$LOG/bash.log" "local-pw"
  file_lacks "$LOG/local-cmd.log" "local-pw"
  # a following ssh deploy in the same run inherits nothing from the local step
  rm -f "$LOG/seat-env.log"
  SHIM_SSH_EXEC=1 run_deploy
  [ "$status" -eq 0 ]
  grep -q '^KC=remote-pw SUDO=remote-pw SEAT=none$' "$LOG/seat-env.log"
  grep -q '^KC=local-pw SUDO=local-pw SEAT=none$' "$LOG/seat-env.log"
  [ "$(grep -c . "$LOG/seat-env.log")" -eq 2 ]
}

@test "bash -x never echoes a seat password (controller side)" {
  write_dotenv "FLEET_SEAT_PASSWORD_hackintosh=hunter2" "FLEET_SEAT_PASSWORD_macbookpro=local-pw"
  stub_seat_script
  SHIM_SSH_EXEC=1 run "$REAL_BASH" -x "$SCRIPT" --host hackintosh
  [ "$status" -eq 0 ]
  out_has "+ deploy_host"       # the trace really was on
  out_lacks "hunter2"
  out_lacks "local-pw"
  file_lacks "$LOG/ssh.log" "hunter2"
  run "$REAL_BASH" -x "$SCRIPT" --host macbookpro
  [ "$status" -eq 0 ]
  out_lacks "local-pw"
  out_lacks "hunter2"
}

@test "a .env holding a password that is not mode 600 is refused before anything runs" {
  printf 'FLEET_SEAT_PASSWORD_hackintosh=hunter2\n' > "$REPO/.env"
  chmod 644 "$REPO/.env"
  run_deploy --host hackintosh
  [ "$status" -ne 0 ]
  out_has "$REPO/.env holds FLEET_SEAT_PASSWORD_hackintosh but is mode 644; run: chmod 600 $REPO/.env"
  out_lacks "hunter2"
  [ ! -e "$LOG/ssh.log" ]
  [ ! -e "$REPO/tools/state/deploy.lock.d" ]
  # even a dry run is refused: the file is misconfigured
  run_deploy --dry-run
  [ "$status" -ne 0 ]
  # an empty template value needs no mode
  printf 'FLEET_SEAT_PASSWORD_hackintosh=\nDESKFLOW_KEYCHAIN_PASSWORD=""\n' > "$REPO/.env"
  run_deploy --dry-run
  [ "$status" -eq 0 ]
}

@test "--pull-only never transports a password" {
  write_dotenv "FLEET_SEAT_PASSWORD_hackintosh=hunter2" "FLEET_SEAT_PASSWORD_macbookpro=local-pw"
  run_deploy --pull-only
  [ "$status" -eq 0 ]
  [ ! -e "$LOG/ssh-stdin.alex@hackintosh" ]
  file_lacks "$LOG/ssh.log" "read -rs"
  out_lacks "seat password"
}

# --- preflight: a deploy that would prompt is refused up front ---------------------

@test "without FLEET_ALLOW_PROMPTS a Mac seat with no FLEET_SEAT_PASSWORD_<id> line is refused with the key to add; --allow-prompts and a local .env password let it through" {
  unset FLEET_ALLOW_PROMPTS
  run_deploy --host hackintosh
  [ "$status" -ne 0 ]
  out_has "no FLEET_SEAT_PASSWORD_hackintosh in $REPO/.env -- deploying hackintosh would prompt"
  out_has "printf 'FLEET_SEAT_PASSWORD_hackintosh=…\\n' >> $REPO/.env && chmod 600 $REPO/.env"
  out_has "--allow-prompts"
  [ ! -e "$LOG/ssh.log" ]
  [ ! -e "$REPO/tools/state/deploy.lock.d" ]
  # the whole fleet: refused on the FIRST Mac lacking a line (server or client), Windows never needs one
  write_dotenv "FLEET_SEAT_PASSWORD_hackintosh=hunter2"
  run_deploy
  [ "$status" -ne 0 ]
  out_has "no FLEET_SEAT_PASSWORD_macbookpro in $REPO/.env"
  [ ! -e "$LOG/ssh.log" ]
  # the local seat is also covered by its own .env keys
  write_dotenv "FLEET_SEAT_PASSWORD_hackintosh=hunter2" "DESKFLOW_SUDO_PASSWORD=local-pw"
  run_deploy
  [ "$status" -eq 0 ]
  # --allow-prompts / FLEET_ALLOW_PROMPTS=1 opt out (seat's own .env or GUI route)
  rm -f "$REPO/.env" "$LOG/ssh.log"
  run_deploy --host hackintosh --allow-prompts
  [ "$status" -eq 0 ]
  grep -q 'fleet-deploy-macos.sh' "$LOG/ssh.log"
  rm -f "$LOG/ssh.log"
  FLEET_ALLOW_PROMPTS=1 run_deploy --host hackintosh
  [ "$status" -eq 0 ]
  # a pull or a dry run never needs a password
  rm -f "$LOG/ssh.log"
  run_deploy --pull-only --host hackintosh
  [ "$status" -eq 0 ]
  run_deploy --dry-run
  [ "$status" -eq 0 ]
  # Windows-only runs never need one either
  run_deploy --host tiny11
  [ "$status" -eq 0 ]
}
