"""Tests for tools/fleet-doctor. No ssh: a fake runner maps (host, command) -> output."""

import io
import json
import subprocess

import pytest

LOCAL_USER = "alexhughes"
MAC_ID = "ABCDEF0123456789ABCDEF0123456789ABCDEF01"
WIN_TP = "0123456789ABCDEF0123456789ABCDEF01234567"
HEAD = "deadbeefcafe0000deadbeefcafe0000deadbeef"

FLEET_ENV = """
FLEET_BRANCH=main
FLEET_HOSTS="hackintosh macbookpro tiny11"
FLEET_SSH_hackintosh=local
FLEET_SSH_macbookpro=macbookpro
FLEET_SSH_tiny11=tiny11
FLEET_SSH_USER_tiny11=alexh
FLEET_DESKFLOW_PATH_macos=~/Desktop/deskflow
FLEET_MOUSER_PATH_macos=~/Desktop/Mouser
FLEET_DESKFLOW_PATH_windows=C:/Users/alexh/Desktop/deskflow
FLEET_MOUSER_PATH_windows=C:/Users/alexh/Desktop/Mouser
"""

MAC_DOTENV = f"DESKFLOW_CODESIGN_ID={MAC_ID}\nQT_PREFIX=/opt/Qt/6.8.3/macos\n"
WIN_DOTENV = f"DESKFLOW_SIGN_THUMBPRINT={WIN_TP}\nQT_PREFIX=C:/Qt/6.8.3/msvc2022_64\n"
CMAKE_CACHE = "APPLE_CODESIGN_DEV:STRING=ABCDEF\nFLEET_STRICT_SIGNING:BOOL=ON\n"


class FakeRunner:
    """Maps (host_id, command substring) -> (rc, stdout[, stderr]); longest substring wins."""

    def __init__(self, responses, unreachable=()):
        self.responses = dict(responses)
        self.unreachable = set(unreachable)
        self.calls = []

    def __call__(self, host, cmd):
        self.calls.append((host.id, cmd))
        if host.id in self.unreachable:
            raise self.unreachable_exc("connection refused")
        best = None
        for (hid, needle), resp in self.responses.items():
            if hid == host.id and needle in cmd and (best is None or len(needle) > len(best[0])):
                best = (needle, resp)
        if best is None:
            return 1, "", f"no fake response for {host.id}: {cmd}"
        rc, out, *err = best[1]
        return rc, out, err[0] if err else ""

    def set(self, host, needle, rc, out="", err=""):
        self.responses[(host, needle.replace("{q}", QUOTE[host]))] = (rc, out, err)


# Per-host path quoting as emitted by fleet-doctor: local mac -> expanded home in
# single quotes; remote mac -> "$HOME/..." in double quotes; windows -> PS single quotes.
QUOTE = {"hackintosh": "'", "macbookpro": '"', "tiny11": "'"}


def mac_responses(hid, python="3.14", console=LOCAL_USER):
    q = QUOTE[hid]
    return {
        (hid, "echo fleet-doctor-ok"): (0, "fleet-doctor-ok\n"),
        (hid, f'deskflow/.env{q}'): (0, MAC_DOTENV),
        (hid, f'scripts/fleet.env{q}'): (0, "FLEET_HOSTS=x\n"),
        (hid, "scripts/fleet.env*"): (0, "/Users/u/Desktop/deskflow/scripts/fleet.env\n/Users/u/Desktop/deskflow/scripts/fleet.env.example\n"),
        (hid, f'fleet.env.example{q}'): (0, "# example\nFLEET_HOSTS=\n"),
        (hid, "fleet.env.bak-*"): (1, ""),
        (hid, f'deskflow{q} status --porcelain'): (0, ""),
        (hid, f'Mouser{q} status --porcelain'): (0, ""),
        (hid, f'deskflow{q} rev-parse --abbrev-ref HEAD'): (0, "main\n"),
        (hid, f'Mouser{q} rev-parse --abbrev-ref HEAD'): (0, "main\n"),
        (hid, f'deskflow{q} rev-parse HEAD'): (0, HEAD + "\n"),
        (hid, f'Mouser{q} rev-parse HEAD'): (0, HEAD + "\n"),
        (hid, f'Mouser{q} remote'): (0, "fork\norigin\n"),
        (hid, "CMakeCache.txt"): (0, CMAKE_CACHE),
        (hid, "security find-identity"): (0, f'  1) {MAC_ID} "Apple Development: Alex (TEAM)"\n     1 valid identities found\n'),
        (hid, "stat -f %Su /dev/console"): (0, console + "\n"),
        (hid, "osascript"): (0, "2\n"),                       # Terminal: 2 windows
        (hid, 'tell application "System Events"'): (0, "0\n"),  # no AXDialog open
        (hid, "python3 --version"): (0, f"Python {python}.1\n"),
        (hid, "cmake --version"): (0, "cmake version 3.31.2\n"),
        (hid, "test -d '/opt/Qt/6.8.3/macos'"): (0, ""),
        (hid, "launchctl list | grep com.cursor.worker"): (1, ""),
        (hid, "grep -c vitest"): (1, "0\n"),
    }


def win_responses(hid="tiny11"):
    return {
        (hid, "echo fleet-doctor-ok"): (0, "fleet-doctor-ok\n"),
        (hid, "deskflow/.env'"): (0, WIN_DOTENV),
        (hid, "scripts/fleet.env'"): (0, "FLEET_HOSTS=x\n"),
        (hid, "scripts/fleet.env*'"): (0, "C:\\Users\\alexh\\Desktop\\deskflow\\scripts\\fleet.env\n"),
        (hid, "fleet.env.bak-*'"): (0, ""),
        (hid, "deskflow' status --porcelain"): (0, ""),
        (hid, "Mouser' status --porcelain"): (0, ""),
        (hid, "deskflow' rev-parse --abbrev-ref HEAD"): (0, "main\n"),
        (hid, "Mouser' rev-parse --abbrev-ref HEAD"): (0, "main\n"),
        (hid, "deskflow' rev-parse HEAD"): (0, HEAD + "\n"),
        (hid, "Mouser' rev-parse HEAD"): (0, HEAD + "\n"),
        (hid, "Mouser' remote"): (0, "fork\norigin\n"),
        (hid, "Cert:\\LocalMachine\\My"): (0, WIN_TP + "\n"),
        (hid, "quser"): (0, " USERNAME  SESSIONNAME  ID  STATE   IDLE TIME  LOGON TIME\n>alexh     console      1   Active     none    9/16/2026 8:00 AM\n"),
        (hid, "python --version"): (0, "Python 3.12.4\n"),
        (hid, "cmake --version"): (0, "cmake version 3.31.2\n"),
        (hid, "Test-Path -PathType Container 'C:/Qt/6.8.3/msvc2022_64'"): (0, "True\n"),
    }


@pytest.fixture(autouse=True)
def fixed_home(monkeypatch):
    monkeypatch.setenv("HOME", "/Users/u")


@pytest.fixture
def fleet_env(tmp_path):
    path = tmp_path / "fleet.env"
    path.write_text(FLEET_ENV)
    return str(path)


@pytest.fixture
def healthy():
    responses = {}
    responses.update(mac_responses("hackintosh", python="3.14"))
    responses.update(mac_responses("macbookpro", python="3.13"))
    responses.update(win_responses("tiny11"))
    runner = FakeRunner(responses)
    runner.unreachable_exc = None  # set by run()
    return runner


# The seat under test is hackintosh: locality is derived from the hostname
# (this_id), NOT from FLEET_SSH_hackintosh=local in FLEET_ENV above.
THIS_ID = "hackintosh"


def run(fd, runner, fleet_env, *argv, this_id=THIS_ID):
    runner.unreachable_exc = fd.HostUnreachable
    out = io.StringIO()
    code = fd.main(list(argv), runner=runner, env_path=fleet_env, local_user=LOCAL_USER, out=out, this_id=this_id)
    return code, out.getvalue()


def run_json(fd, runner, fleet_env, *argv, this_id=THIS_ID):
    code, text = run(fd, runner, fleet_env, "--json", *argv, this_id=this_id)
    return code, json.loads(text)


def rows(payload, host=None, check=None, status=None):
    return [r for r in payload["results"]
            if (host is None or r["host"] == host) and (check is None or r["check"] == check)
            and (status is None or r["status"] == status)]


# ----------------------------------------------------------------------------- healthy


def test_all_hosts_all_checks_pass(fleet_doctor, healthy, fleet_env):
    code, payload = run_json(fleet_doctor, healthy, fleet_env, "--host", "all")
    assert code == 0, payload
    assert payload["hosts"] == ["hackintosh", "macbookpro", "tiny11"]
    assert not rows(payload, status="fail")
    assert not rows(payload, status="unreachable")
    for check in fleet_doctor.CHECKS:
        assert rows(payload, host="hackintosh", check=check), check
    assert rows(payload, host="fleet", check="repo", status="pass")


def test_table_output_has_header_and_rows(fleet_doctor, healthy, fleet_env):
    code, text = run(fleet_doctor, healthy, fleet_env, "--host", "local", "--check", "env")
    assert code == 0
    lines = text.splitlines()
    assert lines[0].split("|")[0].strip() == "host"
    assert [c.strip() for c in lines[0].split("|")] == ["host", "check", "status", "detail"]
    assert all(line.split("|")[0].strip() == "hackintosh" for line in lines[2:])


def test_host_local_selects_the_local_seat(fleet_doctor, healthy, fleet_env):
    code, payload = run_json(fleet_doctor, healthy, fleet_env, "--host", "local", "--check", "toolchain")
    assert code == 0
    assert payload["hosts"] == ["hackintosh"]
    assert {c for c, _ in healthy.calls} == {"hackintosh"}


def test_host_id_and_check_subset(fleet_doctor, healthy, fleet_env):
    code, payload = run_json(fleet_doctor, healthy, fleet_env, "--host", "tiny11", "--check", "env,session")
    assert code == 0
    assert payload["checks"] == ["env", "session"]
    assert {r["check"] for r in payload["results"]} == {"env", "session"}


def test_unknown_host_or_check_exit_2(fleet_doctor, healthy, fleet_env):
    assert run(fleet_doctor, healthy, fleet_env, "--host", "nope")[0] == 2
    assert run(fleet_doctor, healthy, fleet_env, "--check", "bogus")[0] == 2


def test_missing_fleet_env_exit_2(fleet_doctor, healthy, tmp_path):
    assert run(fleet_doctor, healthy, str(tmp_path / "absent.env"))[0] == 2


# ----------------------------------------------------------------------------- reachability


def test_unreachable_host_exit_2_even_with_failures(fleet_doctor, healthy, fleet_env):
    healthy.unreachable.add("macbookpro")
    healthy.set("hackintosh", 'deskflow{q} status --porcelain', 0, " M src/x.cpp\n")
    code, payload = run_json(fleet_doctor, healthy, fleet_env)
    assert code == 2
    assert rows(payload, host="macbookpro", status="unreachable")
    assert not rows(payload, host="macbookpro", status="pass")
    assert rows(payload, host="hackintosh", check="repo", status="fail")
    assert rows(payload, host="tiny11", status="pass")


def test_probe_failure_marks_unreachable(fleet_doctor, healthy, fleet_env):
    healthy.set("tiny11", "echo fleet-doctor-ok", 1, "", "boom")
    code, payload = run_json(fleet_doctor, healthy, fleet_env, "--host", "tiny11", "--check", "env")
    assert code == 2
    assert rows(payload, host="tiny11", status="unreachable")[0]["detail"] == "boom"


# ----------------------------------------------------------------------------- env


@pytest.mark.parametrize("host,needle,resp,expect", [
    ("hackintosh", 'deskflow/.env{q}', (1, ""), ".env missing"),
    ("hackintosh", 'deskflow/.env{q}', (0, "DESKFLOW_CODESIGN_ID=\n"), "lacks DESKFLOW_CODESIGN_ID"),
    ("tiny11", "deskflow/.env'", (0, "QT_PREFIX=C:/Qt\n"), "lacks DESKFLOW_SIGN_THUMBPRINT"),
    ("macbookpro", 'scripts/fleet.env{q}', (1, ""), "scripts/fleet.env missing"),
    ("hackintosh", 'fleet.env.example{q}', (0, "FLEET_KEYCHAIN_PASSWORD_hackintosh=hunter2\n"), "KEYCHAIN_PASSWORD in fleet.env.example"),
    ("hackintosh", "fleet.env.bak-*", (0, "/x/scripts/fleet.env.bak-20260901\n"), "fleet.env.bak-20260901"),
    ("tiny11", "fleet.env.bak-*'", (0, "C:\\x\\scripts\\fleet.env.bak-1\n"), "fleet.env.bak-1"),
])
def test_env_failures(fleet_doctor, healthy, fleet_env, host, needle, resp, expect):
    healthy.set(host, needle, *resp)
    code, payload = run_json(fleet_doctor, healthy, fleet_env, "--host", host, "--check", "env")
    assert code == 1
    fails = rows(payload, host=host, check="env", status="fail")
    assert len(fails) == 1 and expect in fails[0]["detail"], fails


@pytest.mark.parametrize("dotenv,expect_ok", [
    (f"DESKFLOW_CODESIGN_ID={MAC_ID}\n", True),
    (f"  DESKFLOW_CODESIGN_ID = {MAC_ID}\n", True),
    (f"export DESKFLOW_CODESIGN_ID={MAC_ID}\n", True),
    ("DESKFLOW_CODESIGN_ID=\n", False),
    # The old `\s*=\s*\S+` regex spanned the newline and read QT_PREFIX as the value.
    ("DESKFLOW_CODESIGN_ID=\nQT_PREFIX=/opt/Qt\n", False),
    ("DESKFLOW_CODESIGN_ID=   \n\nQT_PREFIX=/opt/Qt\n", False),
    ('DESKFLOW_CODESIGN_ID=""\n', False),
    ("DESKFLOW_CODESIGN_ID= # fill me in\n", False),
    ("# DESKFLOW_CODESIGN_ID=abc\n", False),
])
def test_env_key_regex_does_not_span_lines(fleet_doctor, healthy, fleet_env, dotenv, expect_ok):
    assert fleet_doctor.env_key_has_value(dotenv, "DESKFLOW_CODESIGN_ID") is expect_ok
    healthy.set("hackintosh", 'deskflow/.env{q}', 0, dotenv)
    code, payload = run_json(fleet_doctor, healthy, fleet_env, "--host", "hackintosh", "--check", "env")
    lacks = [r for r in rows(payload, host="hackintosh", check="env") if "lacks DESKFLOW_CODESIGN_ID" in r["detail"]]
    assert bool(lacks) is (not expect_ok), payload


def test_env_secret_not_echoed(fleet_doctor, healthy, fleet_env):
    healthy.set("hackintosh", 'scripts/fleet.env{q}', 0, "FLEET_KEYCHAIN_PASSWORD_hackintosh=hunter2\n")
    code, text = run(fleet_doctor, healthy, fleet_env, "--host", "hackintosh", "--check", "env")
    assert code == 1
    assert "hunter2" not in text


# ----------------------------------------------------------------------------- repo


@pytest.mark.parametrize("host,needle,resp,expect", [
    ("hackintosh", 'deskflow{q} status --porcelain', (0, " M a\n?? b\n"), "deskflow: dirty (2 paths)"),
    ("macbookpro", 'Mouser{q} status --porcelain', (0, " M a\n"), "Mouser: dirty"),
    ("tiny11", "deskflow' status --porcelain", (0, " M a\n"), "deskflow: dirty"),
    ("hackintosh", 'deskflow{q} rev-parse --abbrev-ref HEAD', (0, "feature\n"), "deskflow: on feature, expected main"),
    ("tiny11", "Mouser' rev-parse --abbrev-ref HEAD", (0, "dev\n"), "Mouser: on dev, expected main"),
    ("hackintosh", 'Mouser{q} remote', (0, "origin\n"), "Mouser: remote fork missing"),
    ("macbookpro", 'Mouser{q} status --porcelain', (128, ""), "Mouser: not a git repo"),
])
def test_repo_failures(fleet_doctor, healthy, fleet_env, host, needle, resp, expect):
    healthy.set(host, needle, *resp)
    code, payload = run_json(fleet_doctor, healthy, fleet_env, "--host", host, "--check", "repo")
    assert code == 1
    fails = rows(payload, host=host, check="repo", status="fail")
    assert len(fails) == 1 and expect in fails[0]["detail"], fails


def test_repo_head_mismatch_across_hosts(fleet_doctor, healthy, fleet_env):
    healthy.set("tiny11", "Mouser' rev-parse HEAD", 0, "0123456789ab\n")
    code, payload = run_json(fleet_doctor, healthy, fleet_env, "--check", "repo")
    assert code == 1
    fleet = rows(payload, host="fleet", check="repo")
    assert [r["status"] for r in fleet] == ["pass", "fail"]
    assert "Mouser: HEAD differs" in fleet[1]["detail"]
    assert "tiny11=01234567" in fleet[1]["detail"]
    assert not rows(payload, status="fail", host="tiny11")


def test_repo_single_host_has_no_fleet_row(fleet_doctor, healthy, fleet_env):
    code, payload = run_json(fleet_doctor, healthy, fleet_env, "--host", "hackintosh", "--check", "repo")
    assert code == 0
    assert not rows(payload, host="fleet")


# ----------------------------------------------------------------------------- signing


@pytest.mark.parametrize("host,needle,resp,expect", [
    ("hackintosh", "CMakeCache.txt", (0, "APPLE_CODESIGN_DEV:STRING=-\nFLEET_STRICT_SIGNING:BOOL=ON\n"), "APPLE_CODESIGN_DEV=- (ad-hoc)"),
    ("hackintosh", "CMakeCache.txt", (0, "APPLE_CODESIGN_DEV:STRING=ABC\nFLEET_STRICT_SIGNING:BOOL=OFF\n"), "FLEET_STRICT_SIGNING=OFF"),
    ("macbookpro", "security find-identity", (0, "     0 valid identities found\n"), "not found by security find-identity"),
    ("macbookpro", 'deskflow/.env{q}', (0, "QT_PREFIX=/opt/Qt\n"), "DESKFLOW_CODESIGN_ID not set"),
    ("tiny11", "Cert:\\LocalMachine\\My", (0, ""), "not in Cert:\\LocalMachine\\My"),
    ("tiny11", "deskflow/.env'", (0, "QT_PREFIX=C:/Qt\n"), "DESKFLOW_SIGN_THUMBPRINT not set"),
])
def test_signing_failures(fleet_doctor, healthy, fleet_env, host, needle, resp, expect):
    healthy.set(host, needle, *resp)
    code, payload = run_json(fleet_doctor, healthy, fleet_env, "--host", host, "--check", "signing")
    assert code == 1
    fails = rows(payload, host=host, check="signing", status="fail")
    assert len(fails) == 1 and expect in fails[0]["detail"], fails


def test_signing_strict_off_fails_and_prints_the_flip(fleet_doctor, healthy, fleet_env):
    healthy.set("hackintosh", "CMakeCache.txt", 0, "APPLE_CODESIGN_DEV:STRING=ABC\nFLEET_STRICT_SIGNING:BOOL=OFF\n")
    code, payload = run_json(fleet_doctor, healthy, fleet_env, "--host", "hackintosh", "--check", "signing")
    assert code == 1
    fails = rows(payload, host="hackintosh", check="signing", status="fail")
    assert len(fails) == 1
    assert fails[0]["detail"] == fleet_doctor.STRICT_SIGNING_FLIP
    assert "-DFLEET_STRICT_SIGNING=ON" in fails[0]["detail"]
    assert "CMakeLists.txt default stays OFF" in fails[0]["detail"]


def test_signing_no_cmakecache_is_not_a_failure(fleet_doctor, healthy, fleet_env):
    healthy.set("macbookpro", "CMakeCache.txt", 1, "")
    code, payload = run_json(fleet_doctor, healthy, fleet_env, "--host", "macbookpro", "--check", "signing")
    assert code == 0
    assert "no build/CMakeCache.txt" in rows(payload, check="signing")[0]["detail"]


def test_windows_cert_lookup_uses_thumbprint(fleet_doctor, healthy, fleet_env):
    run_json(fleet_doctor, healthy, fleet_env, "--host", "tiny11", "--check", "signing")
    cert_cmds = [c for h, c in healthy.calls if "Cert:\\LocalMachine\\My" in c]
    assert cert_cmds and WIN_TP in cert_cmds[0]
    assert '"' not in cert_cmds[0], "double quotes would break the powershell -Command wrapper"


# ----------------------------------------------------------------------------- session


@pytest.mark.parametrize("host,needle,resp,expect", [
    ("hackintosh", "stat -f %Su /dev/console", (0, "root\n"), "console user root, expected alexhughes"),
    ("hackintosh", "stat -f %Su /dev/console", (1, ""), "console user ?"),
    ("macbookpro", "osascript", (1, "", "execution error: Not authorized to send Apple events to Terminal. (-1743)"), "Terminal Automation probe failed"),
    ("tiny11", "quser", (0, " USERNAME SESSIONNAME ID STATE IDLE TIME LOGON TIME\n alexh    rdp-tcp#3   2  Disc   1:00  9/16/2026\n"), "no Active session for alexh"),
    ("tiny11", "quser", (0, " USERNAME SESSIONNAME ID STATE\n>other  console 1 Active none\n"), "no Active session for alexh"),
    ("tiny11", "quser", (1, "", "No User exists for *"), "no Active session"),
])
def test_session_failures(fleet_doctor, healthy, fleet_env, host, needle, resp, expect):
    healthy.set(host, needle, *resp)
    code, payload = run_json(fleet_doctor, healthy, fleet_env, "--host", host, "--check", "session")
    assert code == 1
    fails = rows(payload, host=host, check="session", status="fail")
    assert len(fails) == 1 and expect in fails[0]["detail"], fails


def test_session_probes_system_events_like_fleet_gui_exec(fleet_doctor, healthy, fleet_env):
    # Same osascript as tools/fleet-gui-exec.py's AXDialog precondition, with its human step.
    code, payload = run_json(fleet_doctor, healthy, fleet_env, "--host", "hackintosh", "--check", "session")
    assert code == 0
    probes = [c for _, c in healthy.calls if "System Events" in c]
    assert probes == ["osascript -e '" + fleet_doctor.OSA_COUNT_MODAL_DIALOGS + "'"]
    assert any("System Events Automation granted" in r["detail"] for r in rows(payload, check="session"))

    healthy.set("hackintosh", 'tell application "System Events"', 1, "", "Not authorized to send Apple events to System Events. (-1743)")
    code, payload = run_json(fleet_doctor, healthy, fleet_env, "--host", "hackintosh", "--check", "session")
    assert code == 1
    fails = rows(payload, host="hackintosh", check="session", status="fail")
    assert len(fails) == 1
    assert "System Events Automation probe failed" in fails[0]["detail"]
    assert "Human step:" in fails[0]["detail"] and "Privacy & Security" in fails[0]["detail"]

    healthy.set("hackintosh", 'tell application "System Events"', 0, "1\n")
    code, payload = run_json(fleet_doctor, healthy, fleet_env, "--host", "hackintosh", "--check", "session")
    assert code == 1
    fails = rows(payload, host="hackintosh", check="session", status="fail")
    assert len(fails) == 1 and "1 modal dialog(s) open" in fails[0]["detail"] and "dismiss the dialog" in fails[0]["detail"]


def test_session_terminal_failure_names_the_human_step(fleet_doctor, healthy, fleet_env):
    healthy.set("macbookpro", "osascript", 1, "", "Not authorized to send Apple events to Terminal. (-1743)")
    healthy.set("macbookpro", 'tell application "System Events"', 0, "0\n")
    code, payload = run_json(fleet_doctor, healthy, fleet_env, "--host", "macbookpro", "--check", "session")
    assert code == 1
    fails = rows(payload, host="macbookpro", check="session", status="fail")
    assert len(fails) == 1 and "Human step:" in fails[0]["detail"] and "control Terminal" in fails[0]["detail"]


def test_session_uses_fleet_ssh_user_override(fleet_doctor, healthy, tmp_path):
    env = tmp_path / "fleet.env"
    env.write_text(FLEET_ENV + "FLEET_SSH_USER_macbookpro=bob\n")
    healthy.set("macbookpro", "stat -f %Su /dev/console", 0, "bob\n")
    code, payload = run_json(fleet_doctor, healthy, str(env), "--host", "macbookpro", "--check", "session")
    assert code == 0
    assert rows(payload, check="session")[0]["detail"] == "console user bob"


# ----------------------------------------------------------------------------- toolchain


@pytest.mark.parametrize("host,needle,resp,expect", [
    ("hackintosh", "python3 --version", (0, "Python 3.13.2\n"), "python 3.13, pinned 3.14"),
    ("macbookpro", "python3 --version", (0, "Python 3.14.0\n"), "python 3.14, pinned 3.13"),
    ("tiny11", "python --version", (0, "Python 3.13.0\n"), "python 3.13, pinned 3.12"),
    ("macbookpro", "python3 --version", (127, "", "not found"), "python missing"),
    ("hackintosh", "cmake --version", (127, "", "not found"), "cmake missing"),
    ("hackintosh", "test -d '/opt/Qt/6.8.3/macos'", (1, ""), "QT_PREFIX /opt/Qt/6.8.3/macos missing"),
    ("tiny11", "Test-Path -PathType Container 'C:/Qt/6.8.3/msvc2022_64'", (0, "False\n"), "QT_PREFIX C:/Qt/6.8.3/msvc2022_64 missing"),
])
def test_toolchain_failures(fleet_doctor, healthy, fleet_env, host, needle, resp, expect):
    healthy.set(host, needle, *resp)
    code, payload = run_json(fleet_doctor, healthy, fleet_env, "--host", host, "--check", "toolchain")
    assert code == 1
    fails = rows(payload, host=host, check="toolchain", status="fail")
    assert len(fails) == 1 and expect in fails[0]["detail"], fails


def test_toolchain_python_pin_is_major_minor_only(fleet_doctor, healthy, fleet_env):
    healthy.set("hackintosh", "python3 --version", 0, "Python 3.14.7\n")
    code, payload = run_json(fleet_doctor, healthy, fleet_env, "--host", "hackintosh", "--check", "toolchain")
    assert code == 0


def test_toolchain_qt_prefix_unset_is_skipped(fleet_doctor, healthy, fleet_env):
    healthy.set("macbookpro", 'deskflow/.env{q}', 0, f"DESKFLOW_CODESIGN_ID={MAC_ID}\n")
    code, payload = run_json(fleet_doctor, healthy, fleet_env, "--host", "macbookpro", "--check", "toolchain")
    assert code == 0
    assert any("QT_PREFIX not set" in r["detail"] for r in rows(payload, check="toolchain"))


# ----------------------------------------------------------------------------- noise


def test_noise_skipped_on_non_hackintosh(fleet_doctor, healthy, fleet_env):
    code, payload = run_json(fleet_doctor, healthy, fleet_env, "--check", "noise")
    assert code == 0
    assert rows(payload, host="macbookpro", check="noise", status="skip")
    assert rows(payload, host="tiny11", check="noise", status="skip")
    assert rows(payload, host="hackintosh", check="noise", status="pass")
    assert not any("launchctl" in c for h, c in healthy.calls if h != "hackintosh")


def test_noise_cursor_workers_loaded_fails(fleet_doctor, healthy, fleet_env):
    healthy.set("hackintosh", "launchctl list | grep com.cursor.worker", 0,
                "123\t0\tcom.cursor.worker.1\n-\t0\tcom.cursor.worker.2\n")
    code, payload = run_json(fleet_doctor, healthy, fleet_env, "--host", "hackintosh", "--check", "noise")
    assert code == 1
    fails = rows(payload, check="noise", status="fail")
    assert len(fails) == 1
    assert "com.cursor.worker.1, com.cursor.worker.2" in fails[0]["detail"]
    assert not any("bootout" in c for _, c in healthy.calls)


def test_noise_vitest_processes_fail(fleet_doctor, healthy, fleet_env):
    healthy.set("hackintosh", "grep -c vitest", 0, "3\n")
    code, payload = run_json(fleet_doctor, healthy, fleet_env, "--host", "hackintosh", "--check", "noise")
    assert code == 1
    fails = rows(payload, check="noise", status="fail")
    assert len(fails) == 1 and "3 vitest processes" in fails[0]["detail"]


def test_noise_fix_boots_out_each_worker(fleet_doctor, healthy, fleet_env):
    listing = ["123\t0\tcom.cursor.worker.a\n-\t0\tcom.cursor.worker.b\n"]

    def launchctl_list(host, cmd):
        return 0 if listing[0] else 1, listing[0], ""

    class Runner(FakeRunner):
        def __call__(self, host, cmd):
            self.calls.append((host.id, cmd))
            if "launchctl list | grep com.cursor.worker" in cmd:
                return launchctl_list(host, cmd)
            if "launchctl bootout" in cmd:
                listing[0] = ""
                return 0, "", ""
            return super().__call__(host, cmd)

    runner = Runner(healthy.responses)
    code, payload = run_json(fleet_doctor, runner, fleet_env, "--host", "hackintosh", "--check", "noise", "--fix")
    assert code == 0
    bootouts = [c for _, c in runner.calls if "launchctl bootout" in c]
    assert bootouts == [
        'launchctl bootout "gui/$(id -u)/com.cursor.worker.a"',
        'launchctl bootout "gui/$(id -u)/com.cursor.worker.b"',
    ]
    assert "bootout 2/2 cursor workers" in rows(payload, check="noise")[0]["detail"]


def test_noise_fix_reports_stubborn_worker(fleet_doctor, healthy, fleet_env):
    healthy.set("hackintosh", "launchctl list | grep com.cursor.worker", 0, "1\t0\tcom.cursor.worker.z\n")
    healthy.set("hackintosh", "launchctl bootout", 0, "")
    code, payload = run_json(fleet_doctor, healthy, fleet_env, "--host", "hackintosh", "--check", "noise", "--fix")
    assert code == 1
    assert "still loaded: com.cursor.worker.z" in rows(payload, check="noise", status="fail")[0]["detail"]


# ----------------------------------------------------------------------------- config + runner plumbing


def test_parse_env_file_quotes_and_comments(fleet_doctor, tmp_path):
    p = tmp_path / "f.env"
    p.write_text('# c\nA="x y"\nexport B=\'z\'\nC=~/Desktop/d\n\nbad line\nD=v=w\n')
    cfg = fleet_doctor.parse_env_file(str(p))
    assert cfg == {"A": "x y", "B": "z", "C": "~/Desktop/d", "D": "v=w"}
    assert fleet_doctor.parse_env_file(str(tmp_path / "nope")) == {}


def test_tilde_expands_per_host_not_locally(fleet_doctor):
    local = fleet_doctor.Host("hackintosh", "local", "u", "macos", "~/Desktop/deskflow", "~/m")
    remote = fleet_doctor.Host("macbookpro", "macbookpro", "u", "macos", "~/Desktop/deskflow", "~/m")
    win = fleet_doctor.Host("tiny11", "tiny11", "alexh", "windows", "C:/Users/alexh/Desktop/deskflow", "C:/m")
    assert fleet_doctor.q(local, local.deskflow_path) == "'/Users/u/Desktop/deskflow'"
    assert fleet_doctor.q(remote, remote.deskflow_path) == '"$HOME/Desktop/deskflow"'
    assert fleet_doctor.q(win, win.deskflow_path) == "'C:/Users/alexh/Desktop/deskflow'"
    assert fleet_doctor.expand(remote, "~/x/fleet.env*") == "$HOME/x/fleet.env*"


def test_load_hosts_defaults(fleet_doctor, fleet_env):
    hosts = fleet_doctor.load_hosts(fleet_doctor.parse_env_file(fleet_env), "me", this_id="hackintosh")
    by_id = {h.id: h for h in hosts}
    assert by_id["hackintosh"].is_local and by_id["hackintosh"].user == "me"
    assert by_id["macbookpro"].ssh_dest == "me@macbookpro"
    assert by_id["tiny11"].is_windows and by_id["tiny11"].ssh_dest == "alexh@tiny11"
    assert by_id["tiny11"].deskflow_path == "C:/Users/alexh/Desktop/deskflow"
    assert by_id["macbookpro"].mouser_path.endswith("/Desktop/Mouser")


# ----------------------------------------------------------------------------- locality (hostname, not FLEET_SSH_x=local)


def test_locality_comes_from_hostname_not_fleet_ssh_local(fleet_doctor, fleet_env):
    # fleet.env says FLEET_SSH_hackintosh=local, but this seat is macbookpro:
    # hackintosh must be an ssh target and macbookpro the local one.
    hosts = fleet_doctor.load_hosts(fleet_doctor.parse_env_file(fleet_env), "me", this_id="MacBookPro")
    by_id = {h.id: h for h in hosts}
    assert by_id["macbookpro"].is_local
    assert not by_id["hackintosh"].is_local
    assert by_id["hackintosh"].ssh_dest == "me@hackintosh"   # "local" never becomes an ssh target


def test_locality_defaults_to_socket_hostname(fleet_doctor, fleet_env, monkeypatch):
    monkeypatch.delenv("FLEET_LOCAL_ID", raising=False)
    monkeypatch.setattr(fleet_doctor.socket, "gethostname", lambda: "Tiny11.lan")
    hosts = fleet_doctor.load_hosts(fleet_doctor.parse_env_file(fleet_env), "me")
    by_id = {h.id: h for h in hosts}
    assert by_id["tiny11"].is_local and not by_id["hackintosh"].is_local


def test_locality_honours_fleet_local_id_env(fleet_doctor, fleet_env, monkeypatch):
    monkeypatch.setattr(fleet_doctor.socket, "gethostname", lambda: "stranger")
    monkeypatch.setenv("FLEET_LOCAL_ID", "MacBookPro")
    hosts = fleet_doctor.load_hosts(fleet_doctor.parse_env_file(fleet_env), "me")
    assert [h.id for h in hosts if h.is_local] == ["macbookpro"]


def test_host_local_uses_hostname_seat(fleet_doctor, healthy, fleet_env):
    healthy.responses.update(mac_responses("macbookpro", python="3.13"))
    code, payload = run_json(fleet_doctor, healthy, fleet_env, "--host", "local", "--check", "toolchain",
                             this_id="macbookpro")
    assert code == 0
    assert payload["hosts"] == ["macbookpro"]
    assert {c for c, _ in healthy.calls} == {"macbookpro"}


def test_fleet_addr_overrides_bare_ssh_target(fleet_doctor, tmp_path):
    env = tmp_path / "fleet.env"
    env.write_text(FLEET_ENV + "FLEET_ADDR_macbookpro=192.168.1.11\nFLEET_ADDR_tiny11=192.168.1.12\n"
                   "FLEET_SSH_tiny11=tiny11.tailnet.ts.net\n")
    hosts = fleet_doctor.load_hosts(fleet_doctor.parse_env_file(str(env)), "me", this_id="hackintosh")
    by_id = {h.id: h for h in hosts}
    assert by_id["macbookpro"].ssh_dest == "me@192.168.1.11"          # bare id -> IP override
    assert by_id["tiny11"].ssh_dest == "alexh@tiny11.tailnet.ts.net"  # explicit ssh target wins


# ----------------------------------------------------------------------------- env file resolution


def test_env_path_honours_fleet_env_file(fleet_doctor, healthy, tmp_path, monkeypatch):
    env = tmp_path / "elsewhere.env"
    env.write_text(FLEET_ENV)
    monkeypatch.setenv("FLEET_ENV_FILE", str(env))
    out = io.StringIO()
    healthy.unreachable_exc = fleet_doctor.HostUnreachable
    code = fleet_doctor.main(["--host", "hackintosh", "--check", "toolchain", "--json"], runner=healthy,
                             local_user=LOCAL_USER, out=out, this_id="hackintosh")
    assert code == 0
    assert json.loads(out.getvalue())["hosts"] == ["hackintosh"]


# ----------------------------------------------------------------------------- powershell bytes


def test_default_runner_replaces_undecodable_powershell_bytes(fleet_doctor, monkeypatch):
    # PowerShell over ssh can emit 0x83 (not valid UTF-8); the probe must not raise UnicodeDecodeError.
    def fake_run(argv, **kw):
        assert kw.get("errors") == "replace", "subprocess.run must decode with errors='replace'"
        return subprocess.CompletedProcess(argv, 0, b"ok \x83 done".decode("utf-8", errors=kw["errors"]), "")

    monkeypatch.setattr(fleet_doctor.subprocess, "run", fake_run)
    win = fleet_doctor.Host("tiny11", "tiny11", "alexh", "windows", "C:/d", "C:/m")
    rc, out, _ = fleet_doctor.default_runner(win, "quser")
    assert rc == 0 and out.startswith("ok ") and out.endswith(" done")


def test_default_runner_wraps_windows_ssh_and_detects_255(fleet_doctor, monkeypatch):
    seen = {}

    def fake_run(argv, **kw):
        seen["argv"] = argv
        return subprocess.CompletedProcess(argv, seen.get("rc", 0), "out", "err")

    monkeypatch.setattr(fleet_doctor.subprocess, "run", fake_run)
    win = fleet_doctor.Host("tiny11", "tiny11", "alexh", "windows", "C:/d", "C:/m")
    assert fleet_doctor.default_runner(win, "quser") == (0, "out", "err")
    assert seen["argv"][0] == "ssh" and seen["argv"][-2] == "alexh@tiny11"
    assert seen["argv"][-1] == 'powershell -NoProfile -Command "quser"'

    mac = fleet_doctor.Host("macbookpro", "macbookpro", "alexhughes", "macos", "~/d", "~/m")
    fleet_doctor.default_runner(mac, "true")
    assert seen["argv"][-2:] == ["alexhughes@macbookpro", "true"]

    local = fleet_doctor.Host("hackintosh", "local", "alexhughes", "macos", "~/d", "~/m")
    fleet_doctor.default_runner(local, "true")
    assert seen["argv"] == ["/bin/sh", "-c", "true"]

    seen["rc"] = 255
    with pytest.raises(fleet_doctor.HostUnreachable):
        fleet_doctor.default_runner(mac, "true")
    assert fleet_doctor.default_runner(local, "true")[0] == 255  # local 255 is just an exit code


def test_json_exit_code_matches_return(fleet_doctor, healthy, fleet_env):
    healthy.set("hackintosh", "grep -c vitest", 0, "1\n")
    code, payload = run_json(fleet_doctor, healthy, fleet_env, "--host", "hackintosh", "--check", "noise")
    assert code == 1 == payload["exit_code"]
    assert set(payload["results"][0]) == {"host", "check", "status", "detail"}
