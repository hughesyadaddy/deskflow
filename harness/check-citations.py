#!/usr/bin/env python3
"""Verify hotspot evidence citations in a harness registry.

Usage: check-citations.py <registry.json> [--repo NAME] [--commit REV] [--all]

For every hotspot whose ``repo`` matches this checkout (or ``--repo``), the
``evidence`` object ``{file, line, token}`` must resolve: ``file`` exists at
the pinned commit (registry-level or per-hotspot ``commit``; ``--commit``;
default HEAD) and line ``line`` (1-based) contains ``token``.

Exit 0 when every citation resolves, 1 listing each failure, 2 on usage or
registry errors.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys


def git(repo_root: str, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["git", "-C", repo_root, *args], capture_output=True, text=True, errors="replace"
    )


def repo_root_for(registry_path: str) -> str:
    start = os.path.dirname(os.path.abspath(registry_path)) or "."
    res = git(start, "rev-parse", "--show-toplevel")
    if res.returncode == 0 and res.stdout.strip():
        return res.stdout.strip()
    # Not a git checkout: treat the registry's parent's parent as the root
    # (registry lives at <root>/harness/registry.json).
    return os.path.abspath(os.path.join(start, os.pardir))


def infer_repo_names(repo_root: str) -> list[str]:
    """Candidate repo names: toplevel basename, then origin URL basename."""
    names = [os.path.basename(repo_root.rstrip(os.sep))]
    res = git(repo_root, "remote", "get-url", "origin")
    if res.returncode == 0 and res.stdout.strip():
        url = res.stdout.strip().rstrip("/")
        base = url.rsplit("/", 1)[-1].rsplit(":", 1)[-1]
        if base.endswith(".git"):
            base = base[:-4]
        if base and base not in names:
            names.append(base)
    return names


def file_lines(repo_root: str, rel: str, commit: str | None) -> list[str] | None:
    """Lines of ``rel`` at ``commit`` (None = working tree). None if missing."""
    if commit is None:
        path = os.path.join(repo_root, rel)
        if not os.path.isfile(path):
            return None
        with open(path, encoding="utf-8", errors="replace") as fh:
            return fh.read().splitlines()
    res = git(repo_root, "show", f"{commit}:{rel}")
    if res.returncode != 0:
        return None
    return res.stdout.splitlines()


def check_hotspot(repo_root: str, hotspot: dict, commit: str | None) -> str | None:
    """Return a failure message or None when the citation resolves."""
    hid = hotspot.get("id", "<no id>")
    ev = hotspot.get("evidence")
    if not isinstance(ev, dict):
        return f"{hid}: no evidence object"
    for key in ("file", "line", "token"):
        if key not in ev:
            return f"{hid}: evidence missing '{key}'"
    rel, line, token = ev["file"], ev["line"], ev["token"]
    if not isinstance(line, int) or line < 1:
        return f"{hid}: evidence.line must be a positive integer, got {line!r}"
    pinned = hotspot.get("commit") or commit
    lines = file_lines(repo_root, rel, pinned)
    where = f"{rel}@{pinned or 'worktree'}"
    if lines is None:
        return f"{hid}: {where} does not exist"
    if line > len(lines):
        return f"{hid}: {where}:{line} is past EOF ({len(lines)} lines)"
    if token not in lines[line - 1]:
        hits = [i + 1 for i, text in enumerate(lines) if token in text]
        hint = f"; token found at line(s) {hits[:6]}" if hits else "; token not found anywhere in file"
        return f"{hid}: {where}:{line} does not contain {token!r}{hint}"
    return None


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("registry")
    ap.add_argument("--repo", help="only check hotspots whose repo == NAME (default: inferred)")
    ap.add_argument("--commit", help="git rev to read files at (default: registry 'commit' or HEAD)")
    ap.add_argument("--all", action="store_true", help="check every hotspot regardless of repo")
    ap.add_argument("--worktree", action="store_true", help="read the working tree instead of a commit")
    args = ap.parse_args(argv)

    try:
        with open(args.registry, encoding="utf-8") as fh:
            registry = json.load(fh)
    except (OSError, ValueError) as exc:
        print(f"check-citations: cannot read {args.registry}: {exc}", file=sys.stderr)
        return 2
    hotspots = registry.get("hotspots")
    if not isinstance(hotspots, list):
        print("check-citations: registry has no hotspots[]", file=sys.stderr)
        return 2

    repo_root = repo_root_for(args.registry)
    if args.worktree:
        commit = None
    else:
        commit = args.commit or registry.get("commit") or "HEAD"
        if git(repo_root, "rev-parse", "--verify", "-q", f"{commit}^{{commit}}").returncode != 0:
            # Not a git checkout (or unknown rev): fall back to the working tree.
            commit = None

    if args.all:
        selected = hotspots
    elif args.repo:
        selected = [h for h in hotspots if h.get("repo") == args.repo]
        if not selected:
            print(f"check-citations: no hotspots have repo == {args.repo!r}", file=sys.stderr)
            return 2
    else:
        selected = []
        for name in infer_repo_names(repo_root):
            selected = [h for h in hotspots if h.get("repo") == name]
            if selected:
                break
        if not selected:
            print(
                f"check-citations: no hotspots match repo {infer_repo_names(repo_root)}; "
                "pass --repo NAME or --all",
                file=sys.stderr,
            )
            return 2

    failures = [m for m in (check_hotspot(repo_root, h, commit) for h in selected) if m]
    for msg in failures:
        print(f"FAIL {msg}")
    ok = len(selected) - len(failures)
    print(f"check-citations: {ok}/{len(selected)} citations resolve at {commit or 'worktree'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
