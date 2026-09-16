"""Tests for tools/fleet-gui-exec.py (all subprocess calls are simulated)."""

from __future__ import annotations

import importlib.util
import io
import os
import subprocess
import sys
from pathlib import Path

import pytest

TOOL = Path(__file__).resolve().parents[1] / "fleet-gui-exec.py"


@pytest.fixture(scope="module")
def mod():
    spec = importlib.util.spec_from_file_location("fleet_gui_exec", TOOL)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _cp(args, returncode=0, stdout="", stderr=""):
    return subprocess.CompletedProcess(args, returncode, stdout, stderr)


class Sim:
    """Simulated host: keychain, console user, Automation grant, modal dialogs."""

    def __init__(
        self,
        keychain=True,
        console_user="alex",
        automation=True,
        dialogs=0,
        launch_ok=True,
    ):
        self.keychain = keychain
        self.console_user = console_user
        self.automation = automation
        self.dialogs = dialogs
        self.launch_ok = launch_ok
        self.osascripts: list[str] = []

    def run(self, args, **kwargs):
        if args[0] == "security":
            return _cp(args, 0 if self.keychain else 1, stderr="" if self.keychain else
                       "User interaction is not allowed.")
        if args[0] == "osascript":
            script = args[2]
            self.osascripts.append(script)
            if script.startswith('tell application "Terminal" to count windows'):
                if not self.automation:
                    return _cp(args, 1, stderr="execution error: Not authorized to "
                               "send Apple events to Terminal. (-1743)")
                return _cp(args, 0, stdout="1\n")
            if 'System Events' in script:
                return _cp(args, 0, stdout=f"{self.dialogs}\n")
            if script.startswith('tell application "Terminal" to do script'):
                return _cp(args, 0 if self.launch_ok else 1,
                           stderr="" if self.launch_ok else "boom")
            raise AssertionError(f"unexpected osascript: {script}")
        raise AssertionError(f"unexpected subprocess.run: {args}")

    def check_output(self, args, **kwargs):
        if args[:3] == ["stat", "-f", "%Su"]:
            if self.console_user is None:
                raise subprocess.CalledProcessError(1, args)
            return self.console_user + "\n"
        raise AssertionError(f"unexpected check_output: {args}")


@pytest.fixture
def host(mod, monkeypatch):
    """Install a Sim into the module; tests tweak its attributes."""
    sim = Sim()
    monkeypatch.setattr(mod.subprocess, "run", sim.run)
    monkeypatch.setattr(mod.subprocess, "check_output", sim.check_output)
    monkeypatch.setattr(mod.sys, "platform", "darwin")
    monkeypatch.setenv("USER", "alex")
    monkeypatch.setattr(mod, "POLL_SECONDS", 0)
    return sim


@pytest.fixture
def fast_clock(mod, monkeypatch):
    """Fake monotonic clock; each sleep() advances it by POLL step (5 s)."""
    state = {"t": 0.0, "sleeps": 0, "on_sleep": None}

    def monotonic():
        return state["t"]

    def sleep(_s):
        state["sleeps"] += 1
        state["t"] += 5.0
        if state["on_sleep"]:
            state["on_sleep"](state["sleeps"])

    monkeypatch.setattr(mod.time, "monotonic", monotonic)
    monkeypatch.setattr(mod.time, "sleep", sleep)
    return state


# ------------------------------------------------------------------ direct path


def test_direct_exec_when_keychain_reachable(mod, host, monkeypatch, tmp_path):
    calls = {}

    def fake_execvp(file, args):
        calls["file"] = file
        calls["args"] = args
        calls["cwd"] = os.getcwd()
        raise SystemExit(0)

    monkeypatch.setattr(mod.os, "execvp", fake_execvp)
    monkeypatch.setattr(mod.os, "chdir", lambda d: calls.setdefault("chdir", d))
    with pytest.raises(SystemExit):
        mod.main(["--cwd", str(tmp_path), "--", "echo", "hi there"])
    assert calls["file"] == "echo"
    assert calls["args"] == ["echo", "hi there"]
    assert calls["chdir"] == str(tmp_path)
    assert host.osascripts == []


def test_direct_exec_missing_binary_returns_127(mod, host, monkeypatch, tmp_path):
    def fake_execvp(file, args):
        raise FileNotFoundError(file)

    monkeypatch.setattr(mod.os, "execvp", fake_execvp)
    monkeypatch.setattr(mod.os, "chdir", lambda d: None)
    assert mod.main(["--cwd", str(tmp_path), "--", "no-such-cmd"]) == 127


def test_dry_run_direct(mod, host, capsys, tmp_path):
    assert mod.main(["--dry-run", "--cwd", str(tmp_path), "--", "make", "-j4"]) == 0
    out = capsys.readouterr().out
    assert "plan: direct" in out
    assert "make -j4" in out


def test_no_command_is_usage_error(mod, host, capsys):
    assert mod.main(["--"]) == 2
    assert mod.main([]) == 2


# ------------------------------------------------------------------ dry run gui


def test_dry_run_gui_shows_quoted_applescript(mod, host, capsys, tmp_path):
    host.keychain = False
    code = mod.main([
        "--dry-run", "--cwd", str(tmp_path), "--log", "/tmp/x.log",
        "--", "sh", "-c", 'echo "it\'s a $test"',
    ])
    assert code == 0
    out = capsys.readouterr().out
    assert "plan: gui (keychain unreachable)" in out
    assert "/tmp/x.log" in out
    assert "applescript:" in out
    # No preconditions were probed and nothing was launched.
    assert host.osascripts == []


def test_dry_run_force_gui(mod, host, capsys, tmp_path):
    assert mod.main(["--dry-run", "--force-gui", "--cwd", str(tmp_path), "--", "true"]) == 0
    assert "plan: gui (--force-gui)" in capsys.readouterr().out


# ------------------------------------------------------------------ quoting


def test_shell_line_and_applescript_quote_properly(mod):
    cmd = ["cmake", "--build", "build dir", "--", "-DX=it's \"quoted\"", "$HOME"]
    line = mod.build_shell_line(cmd, "/Users/alex/my repo", "/tmp/l.log")
    assert line.startswith("cd '/Users/alex/my repo' && ")
    assert "'build dir'" in line
    assert "'$HOME'" in line
    assert "> /tmp/l.log 2>&1; echo __FLEET_GUI_EXEC_EXIT__=$? >> /tmp/l.log" in line
    # Reparsing the shell line reproduces the argv exactly.
    import shlex
    tokens = shlex.split(line.split(" && ", 1)[1].split(" > /tmp/l.log")[0])
    assert tokens == cmd

    script = mod.build_applescript(line)
    assert script.startswith('tell application "Terminal" to do script "')
    assert script.endswith('"')
    body = script[len('tell application "Terminal" to do script "'):-1]
    # Every inner double quote and backslash is escaped for AppleScript.
    unescaped = body.replace('\\\\', '\x00').replace('\\"', '\x01')
    assert '"' not in unescaped
    assert '\\' not in unescaped
    # And unescaping gives back the exact shell line.
    assert unescaped.replace('\x00', '\\').replace('\x01', '"') == line


# ------------------------------------------------------------------ preconditions


@pytest.mark.parametrize(
    "tweak, needle",
    [
        (dict(console_user="bob"), "console user is 'bob'"),
        (dict(console_user="root"), "no user is logged in"),
        (dict(console_user=None), "no user is logged in"),
        (dict(automation=False), "Automation"),
        (dict(dialogs=1), "modal dialog"),
    ],
)
def test_precondition_failures_exit_3(mod, host, capsys, tmp_path, tweak, needle):
    host.keychain = False
    for k, v in tweak.items():
        setattr(host, k, v)
    code = mod.main(["--cwd", str(tmp_path), "--log", str(tmp_path / "l.log"), "--", "true"])
    assert code == 3
    err = capsys.readouterr().err
    assert needle in err
    assert "Human step" in err
    assert not any("do script" in s for s in host.osascripts)


def test_precondition_not_darwin(mod, host, monkeypatch, capsys, tmp_path):
    host.keychain = False
    monkeypatch.setattr(mod.sys, "platform", "linux")
    assert mod.main(["--cwd", str(tmp_path), "--", "true"]) == 3
    assert "macOS" in capsys.readouterr().err


def test_osascript_launch_failure_exits_3(mod, host, capsys, tmp_path):
    host.keychain = False
    host.launch_ok = False
    code = mod.main(["--cwd", str(tmp_path), "--log", str(tmp_path / "l.log"), "--", "true"])
    assert code == 3
    assert "osascript failed" in capsys.readouterr().err


# ------------------------------------------------------------------ routed run


def test_gui_route_streams_log_and_returns_sentinel_code(
    mod, host, fast_clock, capsys, tmp_path
):
    host.keychain = False
    log = tmp_path / "run.log"
    cwd = tmp_path / "repo dir"
    cwd.mkdir()

    def on_sleep(n):
        # Simulate the command writing output over several polls, then exiting.
        if n == 1:
            log.write_text("line one\n")
        elif n == 2:
            log.write_text("line one\nline two\n")
        elif n == 3:
            log.write_text(
                "line one\nline two\nlast\n" + mod.SENTINEL + "=7\n"
            )

    fast_clock["on_sleep"] = on_sleep
    code = mod.main([
        "--cwd", str(cwd), "--log", str(log), "--timeout", "100",
        "--", "cmake", "--build", "build", "--target", "it's",
    ])
    assert code == 7
    out = capsys.readouterr().out
    assert "line one\nline two\nlast\n" in out
    assert mod.SENTINEL not in out
    # Exactly one launch, after the two precondition probes.
    launches = [s for s in host.osascripts if "do script" in s]
    assert len(launches) == 1
    script = launches[0]
    assert f"cd {mod.shlex.quote(str(cwd))}" in script.replace('\\"', '"')
    assert "cmake --build build --target 'it'\"'\"'s'" in script.replace('\\"', '"')
    assert f"echo {mod.SENTINEL}=$? >> {mod.shlex.quote(str(log))}" in script
    assert host.osascripts[0] == mod.OSA_COUNT_TERMINAL_WINDOWS
    assert host.osascripts[1] == mod.OSA_COUNT_MODAL_DIALOGS


def test_gui_route_force_gui_when_keychain_reachable(mod, host, fast_clock, tmp_path):
    host.keychain = True
    log = tmp_path / "run.log"
    fast_clock["on_sleep"] = lambda n: log.write_text(mod.SENTINEL + "=0\n")
    code = mod.main(["--force-gui", "--cwd", str(tmp_path), "--log", str(log), "--", "true"])
    assert code == 0
    assert any("do script" in s for s in host.osascripts)


def test_gui_route_timeout_exits_124_and_prints_log_path(
    mod, host, fast_clock, capsys, tmp_path
):
    host.keychain = False
    log = tmp_path / "hang.log"
    fast_clock["on_sleep"] = lambda n: log.write_text("still going\n")
    code = mod.main([
        "--cwd", str(tmp_path), "--log", str(log), "--timeout", "20", "--", "sleep", "999",
    ])
    assert code == 124
    captured = capsys.readouterr()
    assert str(log) in captured.err
    assert "did not finish within 20s" in captured.err
    assert "still going" in captured.out
    # 20 s / 5 s per poll -> 4 sleeps before the deadline trips.
    assert fast_clock["sleeps"] == 4


def test_stale_log_is_truncated_before_launch(mod, host, fast_clock, tmp_path):
    host.keychain = False
    log = tmp_path / "stale.log"
    log.write_text(mod.SENTINEL + "=0\n")  # from a previous run
    fast_clock["on_sleep"] = lambda n: log.write_text(mod.SENTINEL + "=5\n") if n == 2 else None
    code = mod.main(["--cwd", str(tmp_path), "--log", str(log), "--", "true"])
    assert code == 5
    assert fast_clock["sleeps"] == 2


def test_wait_for_exit_handles_partial_lines(mod, fast_clock, tmp_path):
    log = tmp_path / "p.log"
    log.write_text("partial")

    def on_sleep(n):
        if n == 1:
            log.write_text("partial line done\n" + mod.SENTINEL + "=3")

    fast_clock["on_sleep"] = on_sleep
    out = io.StringIO()
    assert mod.wait_for_exit(str(log), 60, out=out) == 3
    assert out.getvalue() == "partial line done\n"


def test_default_log_path_is_unique_per_host_and_pid(mod):
    p = mod.default_log_path()
    assert p.startswith("/tmp/fleet-gui-exec-")
    assert p.endswith(f"-{os.getpid()}.log")
