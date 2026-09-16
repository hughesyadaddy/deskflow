#!/usr/bin/env python3
"""Run a command inside the macOS console (GUI) session when the keychain is out of reach.

Code signing needs the login keychain, and an SSH session is not a member of
the user's GUI session: ``security`` reports "User interaction is not allowed"
and ``codesign`` fails with ``errSecInternalComponent``. The keychain is not
locked, so unlocking it is not the fix; neither is ``sudo`` or
``launchctl asuser``. Ad-hoc signing is not a fix either: every ad-hoc build is
a new app to macOS and resets its Accessibility / Input Monitoring grants.

This helper hands the command to the session that already holds the keychain.
When run from a terminal that can reach the keychain it simply ``exec``s the
command in place; otherwise it drives Terminal.app in the console session,
tails the log, and exits with the command's exit code.

Usage:
    tools/fleet-gui-exec.py [--timeout S] [--cwd DIR] [--log PATH]
                            [--force-gui] [--dry-run] -- cmd args...

Exit codes:
    n    the command's own exit code (direct or routed)
    2    usage error
    3    a precondition for GUI routing failed (message names the human step)
    124  the routed command did not finish within --timeout seconds
"""

from __future__ import annotations

import argparse
import os
import shlex
import socket
import subprocess
import sys
import time
from pathlib import Path

#: Final line of the log; lets the poller tell "finished" from "still running"
#: without racing a partially flushed file.
SENTINEL = "__FLEET_GUI_EXEC_EXIT__"

POLL_SECONDS = 5
DEFAULT_TIMEOUT_SECONDS = 1800

EXIT_USAGE = 2
EXIT_PRECONDITION = 3
EXIT_TIMEOUT = 124

OSA_COUNT_TERMINAL_WINDOWS = 'tell application "Terminal" to count windows'
OSA_COUNT_MODAL_DIALOGS = (
    'tell application "System Events" to count (windows of '
    '(first process whose frontmost is true) whose subrole is "AXDialog")'
)


class PreconditionError(Exception):
    """A GUI-routing precondition failed; the message names the human step."""


# --------------------------------------------------------------------------- probes


def console_user() -> str | None:
    """The user owning the GUI session, or None when nobody is logged in."""
    try:
        owner = subprocess.check_output(
            ["stat", "-f", "%Su", "/dev/console"], text=True, stderr=subprocess.DEVNULL
        ).strip()
    except (FileNotFoundError, subprocess.CalledProcessError):
        return None
    return owner or None


def current_user() -> str:
    user = os.environ.get("USER")
    if user:
        return user
    try:
        import pwd

        return pwd.getpwuid(os.getuid()).pw_name
    except Exception:  # pragma: no cover - last resort
        return ""


def keychain_reachable() -> bool:
    """True when this session can actually use the login keychain.

    Probes the keychain rather than checking for SSH_* env vars: what matters
    is session membership, and SSH is only the most common way to be outside it.
    """
    keychain = Path.home() / "Library" / "Keychains" / "login.keychain-db"
    try:
        result = subprocess.run(
            ["security", "show-keychain-info", str(keychain)],
            capture_output=True,
            text=True,
        )
    except FileNotFoundError:
        return False
    return result.returncode == 0


def osascript(script: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["osascript", "-e", script], capture_output=True, text=True, check=False
    )


def check_preconditions() -> None:
    """Raise PreconditionError unless Terminal.app in the console session is drivable."""
    if sys.platform != "darwin":
        raise PreconditionError(
            "GUI-session routing only works on macOS; run the command directly."
        )

    me = current_user()
    owner = console_user()
    if not owner or owner == "root":
        raise PreconditionError(
            "no user is logged in at the console (stat /dev/console reports "
            f"{owner or 'nothing'}). Human step: log in to the Mac's GUI session "
            f"as {me or 'the target user'} and leave it logged in."
        )
    if owner != me:
        raise PreconditionError(
            f"console user is '{owner}' but this command runs as '{me}'. "
            f"Human step: log in to the Mac's GUI session as '{me}' (fast user "
            "switching leaves the wrong user owning /dev/console)."
        )

    probe = osascript(OSA_COUNT_TERMINAL_WINDOWS)
    if probe.returncode != 0:
        detail = (probe.stderr or probe.stdout or "").strip()
        raise PreconditionError(
            "cannot drive Terminal.app via Automation"
            + (f": {detail}" if detail else "")
            + ". Human step: run this tool once from a local Terminal on the Mac "
            "and click 'OK' on the 'wants access to control Terminal' prompt, or "
            "enable it under System Settings > Privacy & Security > Automation."
        )

    probe = osascript(OSA_COUNT_MODAL_DIALOGS)
    if probe.returncode != 0:
        detail = (probe.stderr or probe.stdout or "").strip()
        raise PreconditionError(
            "cannot query System Events for modal dialogs"
            + (f": {detail}" if detail else "")
            + ". Human step: grant this terminal Accessibility and Automation "
            "access to System Events under System Settings > Privacy & Security."
        )
    try:
        dialogs = int((probe.stdout or "0").strip() or "0")
    except ValueError:
        dialogs = 0
    if dialogs > 0:
        raise PreconditionError(
            f"{dialogs} modal dialog(s) are open in the frontmost app on the "
            "console; a keychain or permission prompt would block the build. "
            "Human step: dismiss the dialog on the Mac's screen, then retry."
        )


# --------------------------------------------------------------------------- routing


def default_log_path() -> str:
    host = socket.gethostname().split(".")[0] or "unknown"
    return f"/tmp/fleet-gui-exec-{host}-{int(time.time())}-{os.getpid()}.log"


def build_shell_line(cmd: list[str], cwd: str, log_path: str) -> str:
    """POSIX shell line run inside the GUI session, logging to *log_path*."""
    return (
        f"cd {shlex.quote(cwd)} && {shlex.join(cmd)} "
        f"> {shlex.quote(log_path)} 2>&1; "
        f"echo {SENTINEL}=$? >> {shlex.quote(log_path)}"
    )


def applescript_quote(text: str) -> str:
    """Escape *text* for use inside an AppleScript double-quoted string."""
    return text.replace("\\", "\\\\").replace('"', '\\"')


def build_applescript(shell_line: str) -> str:
    return f'tell application "Terminal" to do script "{applescript_quote(shell_line)}"'


def parse_sentinel(line: str) -> int | None:
    prefix = f"{SENTINEL}="
    if not line.startswith(prefix):
        return None
    try:
        return int(line[len(prefix):].strip())
    except ValueError:
        return None


def wait_for_exit(log_path: str, timeout: float, out=None) -> int:
    """Poll *log_path*, streaming new lines to *out*, until the sentinel appears.

    Returns the command's exit code, or EXIT_TIMEOUT after *timeout* seconds.
    """
    out = out if out is not None else sys.stdout
    deadline = time.monotonic() + timeout
    offset = 0
    pending = ""
    while True:
        try:
            with open(log_path, "r", errors="replace") as fh:
                fh.seek(offset)
                chunk = fh.read()
                offset = fh.tell()
        except OSError:
            chunk = ""
        if chunk:
            pending += chunk
            lines = pending.split("\n")
            pending = lines.pop()  # partial trailing line, if any
            for line in lines:
                code = parse_sentinel(line)
                if code is not None:
                    out.flush()
                    return code
                out.write(line + "\n")
            out.flush()
        # A sentinel without trailing newline (should not happen, but be safe).
        code = parse_sentinel(pending)
        if code is not None:
            return code
        if time.monotonic() >= deadline:
            print(
                f"error: command did not finish within {int(timeout)}s; "
                f"log: {log_path}",
                file=sys.stderr,
            )
            return EXIT_TIMEOUT
        time.sleep(POLL_SECONDS)


# --------------------------------------------------------------------------- main


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog="fleet-gui-exec.py",
        description=__doc__.split("\n\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--timeout", type=float, default=DEFAULT_TIMEOUT_SECONDS,
        help="seconds to wait for the routed command (default 1800)",
    )
    parser.add_argument("--cwd", default=None, help="working directory (default: cwd)")
    parser.add_argument("--log", default=None, help="log path for the routed command")
    parser.add_argument(
        "--force-gui", action="store_true",
        help="route through the GUI session even if the keychain is reachable",
    )
    parser.add_argument(
        "--dry-run", action="store_true",
        help="print the resolved plan (direct vs gui, the AppleScript) and exit 0",
    )
    parser.add_argument("cmd", nargs=argparse.REMAINDER, help="-- cmd args...")
    ns = parser.parse_args(argv)
    cmd = list(ns.cmd)
    if cmd and cmd[0] == "--":
        cmd = cmd[1:]
    if not cmd:
        parser.error("no command given (usage: ... -- cmd args...)")
    ns.cmd = cmd
    ns.cwd = os.path.abspath(ns.cwd or os.getcwd())
    return ns


def main(argv: list[str] | None = None) -> int:
    try:
        ns = parse_args(list(sys.argv[1:] if argv is None else argv))
    except SystemExit as exc:  # argparse uses 2 for usage errors already
        return int(exc.code) if isinstance(exc.code, int) else EXIT_USAGE

    route_gui = ns.force_gui or not keychain_reachable()

    if not route_gui:
        if ns.dry_run:
            print("plan: direct (keychain reachable)")
            print(f"  cwd: {ns.cwd}")
            print(f"  cmd: {shlex.join(ns.cmd)}")
            return 0
        os.chdir(ns.cwd)
        try:
            os.execvp(ns.cmd[0], ns.cmd)
        except OSError as exc:
            print(f"error: cannot exec {ns.cmd[0]!r}: {exc}", file=sys.stderr)
            return 127
        return 0  # pragma: no cover - execvp does not return

    log_path = ns.log or default_log_path()
    shell_line = build_shell_line(ns.cmd, ns.cwd, log_path)
    script = build_applescript(shell_line)

    if ns.dry_run:
        why = "--force-gui" if ns.force_gui else "keychain unreachable"
        print(f"plan: gui ({why})")
        print(f"  cwd: {ns.cwd}")
        print(f"  cmd: {shlex.join(ns.cmd)}")
        print(f"  log: {log_path}")
        print(f"  timeout: {int(ns.timeout)}s")
        print("  applescript:")
        print(f"    osascript -e {shlex.quote(script)}")
        return 0

    try:
        check_preconditions()
    except PreconditionError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return EXIT_PRECONDITION

    # Truncate up front so a stale log from a previous run can never satisfy
    # the poller.
    try:
        Path(log_path).write_text("")
    except OSError as exc:
        print(f"error: cannot create log {log_path}: {exc}", file=sys.stderr)
        return EXIT_PRECONDITION

    print(
        f"Keychain is unreachable here; running in {console_user()}'s GUI "
        f"session.\n  log: {log_path}",
        flush=True,
    )
    launched = osascript(script)
    if launched.returncode != 0:
        detail = (launched.stderr or launched.stdout or "").strip()
        print(
            "error: osascript failed to start the command in Terminal"
            + (f": {detail}" if detail else ""),
            file=sys.stderr,
        )
        return EXIT_PRECONDITION

    return wait_for_exit(log_path, ns.timeout)


if __name__ == "__main__":
    raise SystemExit(main())
