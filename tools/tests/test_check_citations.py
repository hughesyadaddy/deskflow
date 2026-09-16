"""Tests for harness/check-citations.py.

Runs the checker as a subprocess (it is a script, not a package) against
fixture registries in a throwaway git repo and against the real
harness/registry.json at the current HEAD.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
CHECKER = ROOT / "harness" / "check-citations.py"
REGISTRY = ROOT / "harness" / "registry.json"


def run(*argv: str, cwd: Path | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(CHECKER), *argv],
        capture_output=True,
        text=True,
        cwd=str(cwd or ROOT),
    )


def git(repo: Path, *args: str) -> None:
    subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True)


@pytest.fixture
def repo(tmp_path: Path) -> Path:
    """A tiny git repo named 'deskflow' with a committed file and a registry."""
    r = tmp_path / "deskflow"
    (r / "src").mkdir(parents=True)
    (r / "harness").mkdir()
    (r / "src" / "a.cpp").write_text("int main() {\n  leak();  // token here\n  return 0;\n}\n")
    git(r, "init", "-q", "-b", "main")
    git(r, "-c", "user.name=t", "-c", "user.email=t@t", "add", ".")
    git(r, "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qm", "init")
    return r


def write_registry(repo: Path, hotspots: list[dict], **top) -> Path:
    p = repo / "harness" / "registry.json"
    p.write_text(json.dumps({"version": 1, "hotspots": hotspots, **top}))
    return p


def hotspot(**kw) -> dict:
    base = {
        "id": "H1",
        "repo": "deskflow",
        "files": ["src/a.cpp"],
        "evidence": {"file": "src/a.cpp", "line": 2, "token": "leak()"},
    }
    base.update(kw)
    return base


def test_good_citation_passes(repo: Path) -> None:
    reg = write_registry(repo, [hotspot()])
    res = run(str(reg), cwd=repo)
    assert res.returncode == 0, res.stdout + res.stderr
    assert "1/1 citations resolve" in res.stdout


def test_wrong_line_fails_with_hint(repo: Path) -> None:
    reg = write_registry(repo, [hotspot(evidence={"file": "src/a.cpp", "line": 3, "token": "leak()"})])
    res = run(str(reg), cwd=repo)
    assert res.returncode == 1
    assert "FAIL H1" in res.stdout
    assert "found at line(s) [2]" in res.stdout


def test_missing_file_fails(repo: Path) -> None:
    reg = write_registry(repo, [hotspot(evidence={"file": "src/nope.cpp", "line": 1, "token": "x"})])
    res = run(str(reg), cwd=repo)
    assert res.returncode == 1
    assert "does not exist" in res.stdout


def test_uncommitted_change_not_seen_at_head_but_seen_in_worktree(repo: Path) -> None:
    (repo / "src" / "a.cpp").write_text("int main() {\n  fine();\n  leak();\n}\n")
    reg = write_registry(repo, [hotspot(evidence={"file": "src/a.cpp", "line": 3, "token": "leak()"})])
    assert run(str(reg), cwd=repo).returncode == 1  # HEAD still has leak() on line 2
    assert run(str(reg), "--worktree", cwd=repo).returncode == 0


def test_pinned_commit_from_registry_and_per_hotspot(repo: Path) -> None:
    first = subprocess.check_output(["git", "-C", str(repo), "rev-parse", "HEAD"], text=True).strip()
    (repo / "src" / "a.cpp").write_text("// moved\nint main() {\n  leak();\n}\n")
    git(repo, "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qam", "move")
    # Line 2 is stale at HEAD but correct at the registry-level pinned commit.
    reg = write_registry(repo, [hotspot()], commit=first)
    assert run(str(reg), cwd=repo).returncode == 0
    assert run(str(reg), "--commit", "HEAD", cwd=repo).returncode == 1
    # A per-hotspot pin beats --commit; H2 is written against HEAD.
    reg = write_registry(
        repo,
        [hotspot(commit=first), hotspot(id="H2", evidence={"file": "src/a.cpp", "line": 3, "token": "leak()"})],
    )
    assert run(str(reg), cwd=repo).returncode == 0
    assert run(str(reg), "--commit", "HEAD", cwd=repo).returncode == 0
    assert run(str(reg), "--commit", first, cwd=repo).returncode == 1  # H2 wrong at first


def test_repo_filter_skips_other_repos(repo: Path) -> None:
    other = hotspot(id="M1", repo="Mouser", evidence={"file": "core/x.py", "line": 1, "token": "y"})
    reg = write_registry(repo, [hotspot(), other])
    assert run(str(reg), cwd=repo).returncode == 0
    assert run(str(reg), "--all", cwd=repo).returncode == 1
    assert run(str(reg), "--repo", "Mouser", cwd=repo).returncode == 1
    res = run(str(reg), "--repo", "nothing", cwd=repo)
    assert res.returncode == 2


def test_bad_registry_is_usage_error(tmp_path: Path) -> None:
    bad = tmp_path / "r.json"
    bad.write_text("{not json")
    assert run(str(bad)).returncode == 2
    assert run(str(tmp_path / "missing.json")).returncode == 2


# ---------------------------------------------------------------------------
# The real registry at the current HEAD.
#
# Two deskflow citations in harness/registry.json are wrong at HEAD
# (735d5ea67 and later):
#   D2-automode-epoch-rebuild  cites AutoModeRunner.cpp:119 for "epochLoop";
#                              the definition is at line 90 (call site at 77).
#   D5-unretained-runloops     cites src/lib/platform/OSXKeyboardRelayMonitor.mm;
#                              the file is src/lib/coordination/OSXKeyboardRelayMonitor.mm
#                              (line 288 "CFRunLoopGetCurrent" is correct there).
# The registry is owned by another agent; this test pins the exact failure
# set so that the four good citations are proven to resolve and any registry
# change (fix or new breakage) flips the test.
# ---------------------------------------------------------------------------
KNOWN_BAD_AT_HEAD = {"D2-automode-epoch-rebuild", "D5-unretained-runloops"}


def test_real_registry_deskflow_citations_at_head() -> None:
    res = run(str(REGISTRY), "--repo", "deskflow")
    failing = {line.split()[1].rstrip(":") for line in res.stdout.splitlines() if line.startswith("FAIL ")}
    assert failing == KNOWN_BAD_AT_HEAD, res.stdout + res.stderr
    assert res.returncode == (1 if failing else 0)
    assert "4/6 citations resolve" in res.stdout


def test_real_registry_known_bad_have_documented_fixes() -> None:
    """The corrections in the comment above must themselves be true at HEAD."""
    amr = (ROOT / "src/apps/deskflow-core/AutoModeRunner.cpp").read_text().splitlines()
    assert "epochLoop" in amr[90 - 1]
    kbm = (ROOT / "src/lib/coordination/OSXKeyboardRelayMonitor.mm").read_text().splitlines()
    assert "CFRunLoopGetCurrent" in kbm[288 - 1]
    assert not (ROOT / "src/lib/platform/OSXKeyboardRelayMonitor.mm").exists()


def test_real_registry_infers_repo_without_flag() -> None:
    """Inside a worktree the toplevel basename is not 'deskflow'; the origin URL is."""
    res = run(str(REGISTRY))
    assert res.returncode != 2, res.stderr
    assert "/6 citations resolve" in res.stdout
