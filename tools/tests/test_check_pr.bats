#!/usr/bin/env bats
# Tests for harness/check-pr.sh. gh is shimmed; nothing leaves the machine.

setup() {
  ROOT="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"
  CHECK="$ROOT/harness/check-pr.sh"
  export TMP="$BATS_TEST_TMPDIR"

  # A tiny git repo named 'deskflow' whose registry citation resolves.
  REPO="$TMP/deskflow"
  mkdir -p "$REPO/src" "$REPO/harness"
  printf 'int main() {\n  leak();\n}\n' > "$REPO/src/a.cpp"
  git -C "$REPO" init -q -b main
  git -C "$REPO" -c user.name=t -c user.email=t@t add .
  git -C "$REPO" -c user.name=t -c user.email=t@t commit -qm init >/dev/null
  GOOD_REG="$REPO/harness/registry.json"
  cat > "$GOOD_REG" <<'EOF'
{"version":1,"hotspots":[{"id":"H1","repo":"deskflow","files":["src/a.cpp"],
 "evidence":{"file":"src/a.cpp","line":2,"token":"leak()"}}]}
EOF
  BAD_REG="$REPO/harness/registry-bad.json"
  cat > "$BAD_REG" <<'EOF'
{"version":1,"hotspots":[{"id":"H1","repo":"deskflow","files":["src/a.cpp"],
 "evidence":{"file":"src/a.cpp","line":1,"token":"leak()"}}]}
EOF

  GOOD_BODY="$TMP/good.md"
  printf '## H1\n\nrun-id: hackintosh-20260916-0001\ndeltaMB: 12.5\n\nnotes\n' > "$GOOD_BODY"

  # gh shim: serves a body from $GH_BODY_FILE, records the call.
  mkdir -p "$TMP/bin"
  cat > "$TMP/bin/gh" <<'EOF'
#!/usr/bin/env bash
echo "$@" >> "$GH_CALLS"
if [ "$1" = pr ] && [ "$2" = view ]; then cat "$GH_BODY_FILE"; exit 0; fi
echo "unexpected gh call: $*" >&2; exit 99
EOF
  chmod +x "$TMP/bin/gh"
  export GH_CALLS="$TMP/gh.calls"
  export PATH="$TMP/bin:$PATH"
}

@test "passes with run-id, deltaMB and resolving citations" {
  run "$CHECK" 42 --body-file "$GOOD_BODY" --registry "$GOOD_REG"
  [ "$status" -eq 0 ]
  [[ "$output" == *"check-pr: OK (PR #42)"* ]]
  [ ! -e "$GH_CALLS" ]   # --body-file must not touch gh
}

@test "fails without run-id" {
  printf 'deltaMB: 3\n' > "$TMP/b.md"
  run "$CHECK" 1 --body-file "$TMP/b.md" --registry "$GOOD_REG"
  [ "$status" -eq 1 ]
  [[ "$output" == *"missing a 'run-id"* ]]
}

@test "fails without deltaMB, and with a non-numeric deltaMB" {
  printf 'run-id: r1\n' > "$TMP/b.md"
  run "$CHECK" 1 --body-file "$TMP/b.md" --registry "$GOOD_REG"
  [ "$status" -eq 1 ]
  [[ "$output" == *"missing a numeric 'deltaMB"* ]]
  printf 'run-id: r1\ndeltaMB: lots\n' > "$TMP/b.md"
  run "$CHECK" 1 --body-file "$TMP/b.md" --registry "$GOOD_REG"
  [ "$status" -eq 1 ]
}

@test "empty run-id value is not a run id" {
  printf 'run-id:\ndeltaMB: 1\n' > "$TMP/b.md"
  run "$CHECK" 1 --body-file "$TMP/b.md" --registry "$GOOD_REG"
  [ "$status" -eq 1 ]
}

@test "fails when check-citations fails" {
  run "$CHECK" 1 --body-file "$GOOD_BODY" --registry "$BAD_REG"
  [ "$status" -eq 1 ]
  [[ "$output" == *"FAIL H1"* ]]
  [[ "$output" == *"check-citations.py failed"* ]]
}

@test "reads the body from gh pr view when no --body-file" {
  export GH_BODY_FILE="$GOOD_BODY"
  run "$CHECK" 77 --registry "$GOOD_REG"
  [ "$status" -eq 0 ]
  grep -q '^pr view 77 --json body --jq .body$' "$GH_CALLS"
}

@test "usage errors exit 2" {
  run "$CHECK"
  [ "$status" -eq 2 ]
  run "$CHECK" 1 --body-file "$TMP/does-not-exist" --registry "$GOOD_REG"
  [ "$status" -eq 2 ]
  run "$CHECK" 1 --bogus
  [ "$status" -eq 2 ]
}

@test "--repo is forwarded to check-citations" {
  run "$CHECK" 1 --body-file "$GOOD_BODY" --registry "$GOOD_REG" --repo Mouser
  [ "$status" -eq 1 ]
  [[ "$output" == *"no hotspots match"* ]] || [[ "$output" == *"check-citations.py failed"* ]]
}
