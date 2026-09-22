"""Tests for tools/fleet-health using a fake runner keyed by (host, command)."""

import importlib.machinery
import importlib.util
import json
import sys
from pathlib import Path

import pytest

TOOL = Path(__file__).resolve().parents[1] / "fleet-health"
spec = importlib.util.spec_from_file_location(
    "fleet_health", TOOL, loader=importlib.machinery.SourceFileLoader("fleet_health", str(TOOL))
)
fh = importlib.util.module_from_spec(spec)
sys.modules["fleet_health"] = fh
spec.loader.exec_module(fh)

TEAM = "ABCDE12345"
ALLOWLIST = fh.load_identifier_allowlist()


# --------------------------------------------------------------- fixtures


def codesign_block(path, identifier, team=TEAM, authority="Apple Development: Duff Hughes (ABCDE12345)",
                   adhoc=False, verify_rc=0):
    lines = [f"=== {path}", f"Executable={path}", f"Identifier={identifier}", "Format=Mach-O thin (arm64)"]
    if adhoc:
        lines += ["Signature=adhoc", "TeamIdentifier=not set"]
    else:
        lines += [f"Authority={authority}", "Authority=Apple Worldwide Developer Relations Certification Authority",
                  "Authority=Apple Root CA", f"TeamIdentifier={team}"]
    lines += ["--- verify"]
    if verify_rc:
        lines += [f"{path}: invalid signature (code or signature have been modified)"]
    lines += [f"rc={verify_rc}"]
    return "\n".join(lines) + "\n"


def good_scan():
    return (
        codesign_block("/Applications/Deskflow.app/Contents/MacOS/deskflow", "org.deskflow.deskflow")
        + codesign_block("/Applications/Deskflow.app/Contents/MacOS/deskflow-core", "org.deskflow.deskflow-core")
        + codesign_block("/Applications/Deskflow.app/Contents/Frameworks/QtCore.framework/Versions/A/QtCore",
                         "org.qt-project.QtCore")
        + codesign_block("/Applications/Mouser.app/Contents/MacOS/Mouser", "io.github.hughesyadaddy.mouser")
    )


CSREQ_CERT = ('identifier "org.deskflow.deskflow-core" and anchor apple generic and '
              'certificate leaf[subject.CN] = "Apple Development: Duff Hughes (ABCDE12345)" and '
              'certificate 1[field.1.2.840.113635.100.6.2.1] /* exists */')
CSREQ_CDHASH = "cdhash H\"0123456789abcdef0123456789abcdef01234567\""


def tcc_rows(ax=2, listen=2):
    return (
        f"kTCCServiceAccessibility|org.deskflow.deskflow-core|{ax}|FADE0C00AA\n"
        f"kTCCServiceListenEvent|org.deskflow.deskflow-core|{listen}|FADE0C00BB\n"
        f"kTCCServiceAccessibility|io.github.hughesyadaddy.mouser|2|FADE0C00CC\n"
    )


CTL_OK = "deskflow-ctl assert-single: OK (core=1 launchd-owned, gui=1, bridge=0)"
CTL_FAIL = ("deskflow-ctl assert-single: FAIL\n"
            "  deskflow-core count=2 (want 1)\n"
            "  non-canonical process pid=777 /tmp/build/bin/deskflow-core\n")

# Mouser's reply to {"t":"status"} on 127.0.0.1:19795 when deskflow-core is attached.
BRIDGE_OK = '{"t":"status","attached":true,"peer":"deskflow-core","session":"a1b2","proto":2}'

# `sudo -n launchctl print loginwindow/org.deskflow.vhid-bridge` at a login window.
LOGIN_BRIDGE_PRINT = ("loginwindow/org.deskflow.vhid-bridge = {\n\tactive count = 1\n\tpath = /Library/LaunchAgents/"
                      "org.deskflow.vhid-bridge.plist\n\tstate = running\n\n\tprogram = /Applications/Deskflow.app/"
                      "Contents/MacOS/deskflow-vhid-bridge\n\tpid = 611\n}\n")

# Tail of /var/log/deskflow-vhid-bridge.log from a K3 bridge: a stale run, then
# a clean start that reaches the daemon in 7 s.
LOGIN_BRIDGE_LOG_OK = (
    "2026-09-22T08:00:00-0400 [bridge] starting pid=400\n"
    "2026-09-22T08:00:01-0400 [bridge] vhid connect_failed: 61\n"
    "2026-09-22T08:00:05-0400 [bridge] waiting for virtual HID daemon (5s)\n"
    "2026-09-22T08:00:09-0400 [bridge] virtual HID ready; server candidates: hackintosh, 10.0.0.5 port 24800\n"
    "2026-09-22T08:00:10-0400 [bridge] stopping (signal)\n"
    "2026-09-22T09:00:00-0400 [bridge] another deskflow-vhid-bridge is already running: lock held\n"
    "2026-09-22T09:00:02-0400 [bridge] starting pid=611\n"
    "2026-09-22T09:00:02-0400 [bridge] vhid connect_failed: 61\n"
    "2026-09-22T09:00:07-0400 [bridge] waiting for virtual HID daemon (5s)\n"
    "2026-09-22T09:00:09-0400 [bridge] virtual HID ready; server candidates: hackintosh, 10.0.0.5 port 24800\n"
    "2026-09-22T09:00:09-0400 [bridge] motion scale seed 8.000000 (backing 2.000000 x factor 4.000000)\n"
    "2026-09-22T09:00:10-0400 [bridge] connected to host hackintosh\n"
    "2026-09-22T09:01:00-0400 [bridge] enter 10,20 of 2560x1440; [keys] session letters shifted=0 unshifted=0 caps-edges=0\n"
)

# Fixture JSON as emitted by tools/fleet-health.ps1 for --check instances.
WIN_INSTANCES_PASS = [{"check": "instances", "status": "PASS",
                       "detail": "deskflow-ctl assert-single: OK (daemon=1 session 0 pid 1000; core=1 child of service in session 1; gui=1; bridge=0)"}]
WIN_INSTANCES_FAIL = [{"check": "instances", "status": "FAIL",
                       "detail": "deskflow-ctl assert-single: FAIL; deskflow-core.exe count=2 (want 1); deskflow.exe count=0 (want 1)"}]


class FakeRunner:
    """Canned (rc, stdout, stderr) keyed by (host_id, command)."""

    def __init__(self, table=None):
        self.table = dict(table or {})
        self.calls = []

    def __call__(self, host, cmd):
        self.calls.append((host.id, cmd))
        if (host.id, cmd) in self.table:
            return self.table[(host.id, cmd)]
        raise AssertionError(f"unexpected command on {host.id}: {cmd}")


def mac(hid="macbookpro", target=None):
    return fh.Host(hid, target or hid, "alexhughes", "macos", hid)


def win(hid="tiny11"):
    return fh.Host(hid, hid, "alexh", "windows", hid)


def mac_ok_table(hid="macbookpro", peers=()):
    t = {
        (hid, fh.macho_scan_cmd()): (0, good_scan(), ""),
        # AX + ListenEvent live in the system DB; the user DB usually has nothing for us.
        (hid, fh.tcc_query_cmd(fh.SYSTEM_TCC_DB)): (0, tcc_rows(), ""),
        (hid, fh.tcc_query_cmd(fh.USER_TCC_DB)): (0, "", ""),
        (hid, fh.csreq_decode_cmd("FADE0C00AA")): (0, CSREQ_CERT + "\n", ""),
        (hid, fh.csreq_decode_cmd("FADE0C00BB")): (0, CSREQ_CERT + "\n", ""),
        (hid, fh.csreq_decode_cmd("FADE0C00CC")): (0, CSREQ_CERT.replace("deskflow-core", "mouser") + "\n", ""),
        (hid, fh.check_permissions_cmd()): (0, "accessibility: granted\niohid: granted\n", ""),
        (hid, fh.launchctl_cmd()): (0, "gui/501 => {\n\tservices = {\n\t\tio.github.hughesyadaddy.mouser\n"
                                    "\t\tio.github.hughesyadaddy.deskflow-core\n\t}\n}", ""),
        (hid, fh.deskflow_gui_running_cmd()): (0, "4242\n", ""),
        (hid, fh.mouser_running_cmd()): (0, "4243\n", ""),
        (hid, fh.ctl_assert_single_cmd()): (0, CTL_OK + "\n", ""),
        (hid, fh.bridge_status_cmd()): (0, BRIDGE_OK + "\n", ""),
        (hid, fh.login_bridge_plist_cmd()): (0, fh.LOGIN_BRIDGE_BIN + "\n", ""),
        (hid, fh.login_bridge_log_mode_cmd()): (0, "600\n", ""),
        (hid, fh.sudo_probe_cmd()): (0, "", ""),
        (hid, fh.login_bridge_agent_cmd()): (0, LOGIN_BRIDGE_PRINT, ""),
        (hid, fh.login_bridge_keystroke_cmd()): (1, "0\n", ""),
        (hid, fh.login_bridge_calibrate_cmd()): (0, "1\n", ""),
        (hid, fh.login_bridge_log_since_start_cmd()): (0, LOGIN_BRIDGE_LOG_OK, ""),
    }
    for p in peers:
        t[(hid, fh.nc_cmd(p, fh.DEFAULT_MESH_PORT))] = (0, "", "")
    return t


def run_checks(hosts, table, checks, env=None, explicit=True, osenv=None):
    runner = FakeRunner(table)
    checker = fh.FleetHealth(hosts, env or {}, runner=runner, allowlist=ALLOWLIST, osenv=osenv or {})
    return checker.run(checks, explicit), runner


def by_check(results, check):
    return [r for r in results if r.check == check]


# ------------------------------------------------------------- env parsing


def test_parse_env_file(tmp_path):
    p = tmp_path / "fleet.env"
    p.write_text('# comment\nFLEET_HOSTS="hackintosh macbookpro tiny11"\nexport FLEET_SSH_tiny11=tiny11\n'
                 "FLEET_SSH_hackintosh=local # self\nFLEET_DESKFLOW_PORT=24801\n")
    env = fh.parse_env_file(p)
    assert env["FLEET_HOSTS"] == "hackintosh macbookpro tiny11"
    assert env["FLEET_SSH_tiny11"] == "tiny11"
    assert env["FLEET_SSH_hackintosh"] == "local"
    assert env["FLEET_DESKFLOW_PORT"] == "24801"


def test_load_hosts_all_and_filter():
    env = {"FLEET_HOSTS": "hackintosh macbookpro tiny11", "FLEET_SSH_hackintosh": "local",
           "FLEET_SSH_macbookpro": "mbp.lan", "FLEET_SSH_USER_tiny11": "alexh"}
    hosts = fh.load_hosts(env, "all", this_id="hackintosh")
    assert [h.id for h in hosts] == ["hackintosh", "macbookpro", "tiny11"]
    assert hosts[0].is_local and hosts[0].os == "macos"
    assert hosts[1].target == "mbp.lan" and hosts[1].addr == "mbp.lan"
    assert hosts[2].os == "windows" and hosts[2].user == "alexh"
    only = fh.load_hosts(env, "tiny11", this_id="hackintosh")
    assert [h.id for h in only] == ["tiny11"]
    local = fh.load_hosts(env, "local", this_id="macbookpro")
    assert [h.id for h in local] == ["macbookpro"] and local[0].is_local
    with pytest.raises(SystemExit):
        fh.load_hosts(env, "nope", this_id="hackintosh")


def test_load_hosts_os_override_and_addr():
    env = {"FLEET_HOSTS": "winbox mini", "FLEET_OS_mini": "windows", "FLEET_ADDR_winbox": "10.0.0.9"}
    hosts = fh.load_hosts(env, "all", this_id="other")
    assert hosts[0].os == "windows" and hosts[0].addr == "10.0.0.9"
    assert hosts[1].os == "windows"


# ------------------------------------------------------------ codesign parse


def test_parse_macho_scan():
    entries = fh.parse_macho_scan(good_scan() + codesign_block("/x/adhoc", "org.deskflow.deskflow-5555", adhoc=True))
    assert len(entries) == 5
    assert entries[0].identifier == "org.deskflow.deskflow" and entries[0].team == TEAM
    assert entries[0].authorities[0].startswith("Apple Development: Duff Hughes")
    assert entries[0].verify_rc == 0 and not entries[0].adhoc
    assert entries[4].adhoc and entries[4].team == "not set"


def test_identifier_allowlist_rejects_uuid_suffix():
    assert fh.identifier_allowed("org.deskflow.deskflow", ALLOWLIST)
    assert fh.identifier_allowed("org.qt-project.QtGui", ALLOWLIST)
    assert not fh.identifier_allowed("org.deskflow.deskflow-5555", ALLOWLIST)
    assert not fh.identifier_allowed("io.github.hughesyadaddy.mouser-5555", ALLOWLIST)


# ------------------------------------------------------------------ sign


def test_sign_pass():
    results, _ = run_checks([mac()], mac_ok_table(), ["sign"])
    (r,) = results
    assert r.status == "PASS" and TEAM in r.detail


def test_sign_fails_on_wrong_authority():
    results, _ = run_checks([mac()], mac_ok_table(), ["sign"], osenv={"DESKFLOW_EXPECT_AUTHORITY": "Developer ID"})
    assert results[0].status == "FAIL" and "no Authority=Developer ID" in results[0].detail


def test_sign_fails_on_team_mismatch():
    scan = good_scan() + codesign_block("/Applications/Mouser.app/Contents/Frameworks/x.dylib", "io.github.hughesyadaddy.mouser", team="ZZZ")
    t = mac_ok_table()
    t[("macbookpro", fh.macho_scan_cmd())] = (0, scan, "")
    results, _ = run_checks([mac()], t, ["sign"])
    assert results[0].status == "FAIL" and "TeamIdentifier=ZZZ" in results[0].detail


def test_sign_uses_expect_team_env():
    results, _ = run_checks([mac()], mac_ok_table(), ["sign"], osenv={"DESKFLOW_EXPECT_TEAM": "OTHER"})
    assert results[0].status == "FAIL" and "!= OTHER" in results[0].detail


def test_sign_fails_on_verify_error():
    scan = good_scan() + codesign_block("/Applications/Deskflow.app/Contents/MacOS/deskflow-vhid-bridge",
                                        "org.deskflow.deskflow-vhid-bridge", verify_rc=1)
    t = mac_ok_table()
    t[("macbookpro", fh.macho_scan_cmd())] = (0, scan, "")
    results, _ = run_checks([mac()], t, ["sign"])
    assert results[0].status == "FAIL" and "verify --deep --strict rc=1" in results[0].detail


def test_sign_fails_when_nothing_found():
    t = {("macbookpro", fh.macho_scan_cmd()): (0, "", "")}
    results, _ = run_checks([mac()], t, ["sign"])
    assert results[0].status == "FAIL" and "no Mach-Os" in results[0].detail


def test_scan_is_cached_across_checks():
    results, runner = run_checks([mac()], mac_ok_table(), ["sign", "no-adhoc", "identifiers"], explicit=False)
    assert [r.status for r in results] == ["PASS", "PASS", "PASS"]
    assert sum(1 for _, c in runner.calls if c == fh.macho_scan_cmd()) == 1


# -------------------------------------------------------------- no-adhoc


def test_no_adhoc_pass_and_fail():
    results, _ = run_checks([mac()], mac_ok_table(), ["no-adhoc"])
    assert results[0].status == "PASS"
    scan = good_scan() + codesign_block("/Applications/Deskflow.app/Contents/MacOS/deskflow-core",
                                        "org.deskflow.deskflow-core", adhoc=True)
    t = mac_ok_table()
    t[("macbookpro", fh.macho_scan_cmd())] = (0, scan, "")
    results, _ = run_checks([mac()], t, ["no-adhoc"])
    assert results[0].status == "FAIL" and "Signature=adhoc: deskflow-core" in results[0].detail


def test_adhoc_binary_also_fails_sign():
    scan = codesign_block("/Applications/Deskflow.app/Contents/MacOS/deskflow", "org.deskflow.deskflow", adhoc=True)
    t = {("macbookpro", fh.macho_scan_cmd()): (0, scan, "")}
    results, _ = run_checks([mac()], t, ["sign"])
    assert results[0].status == "FAIL" and "no Authority=Apple Development" in results[0].detail


# ------------------------------------------------------------ identifiers


def test_identifiers_pass_and_uuid_suffix_fail():
    results, _ = run_checks([mac()], mac_ok_table(), ["identifiers"])
    assert results[0].status == "PASS"
    scan = good_scan() + codesign_block("/Applications/Deskflow.app/Contents/MacOS/deskflow-vhid-bridge",
                                        "org.deskflow.deskflow-vhid-bridge-5555")
    t = mac_ok_table()
    t[("macbookpro", fh.macho_scan_cmd())] = (0, scan, "")
    results, _ = run_checks([mac()], t, ["identifiers"])
    assert results[0].status == "FAIL"
    assert "deskflow-vhid-bridge=org.deskflow.deskflow-vhid-bridge-5555" in results[0].detail


def test_identifier_filename_derived_rule():
    ok = fh.identifier_is_filename_derived
    assert ok("/A/Contents/Frameworks/libpcre2-16.0.dylib", "libpcre2-16")
    assert ok("/A/Contents/Frameworks/libglib-2.0.0.dylib", "libglib-2")
    assert ok("/A/Contents/Frameworks/_ssl.cpython-313-darwin.so", "_ssl.cpython-313-darwin")
    assert ok("/A/Contents/Frameworks/libz.1.3.1.zlib-ng.dylib", "libz.1.3.1.zlib-ng")
    assert ok("/A/Contents/Frameworks/QtCore.framework/Versions/A/QtCore", "QtCore")
    assert not ok("/A/Contents/Frameworks/libfoo.dylib", "libfoo-5555")
    assert not ok("/A/Contents/Frameworks/libfoo.1.dylib", "libbar")
    assert not ok("/A/Contents/Frameworks/libfoo.dylib", "")


def test_identifiers_third_party_filename_derived_passes_but_first_party_must_be_allowlisted():
    scan = good_scan() + codesign_block("/Applications/Mouser.app/Contents/Frameworks/libssl.3.dylib", "libssl.3")
    t = mac_ok_table()
    t[("macbookpro", fh.macho_scan_cmd())] = (0, scan, "")
    results, _ = run_checks([mac()], t, ["identifiers"])
    assert results[0].status == "PASS"
    # A first-party binary with a bare filename-derived identifier is NOT acceptable.
    scan = good_scan() + codesign_block("/Applications/Deskflow.app/Contents/MacOS/deskflow-core", "deskflow-core")
    t[("macbookpro", fh.macho_scan_cmd())] = (0, scan, "")
    results, _ = run_checks([mac()], t, ["identifiers"])
    assert results[0].status == "FAIL" and "deskflow-core=deskflow-core" in results[0].detail
    # A third-party dylib with a UUID-style suffix fails too.
    scan = good_scan() + codesign_block("/Applications/Mouser.app/Contents/Frameworks/libssl.3.dylib", "libssl.3-5555")
    t[("macbookpro", fh.macho_scan_cmd())] = (0, scan, "")
    results, _ = run_checks([mac()], t, ["identifiers"])
    assert results[0].status == "FAIL" and "libssl.3.dylib=libssl.3-5555" in results[0].detail


def test_identifiers_fail_on_empty_allowlist():
    runner = FakeRunner(mac_ok_table())
    results = fh.FleetHealth([mac()], {}, runner=runner, allowlist=[], osenv={}).run(["identifiers"], True)
    assert results[0].status == "FAIL" and "allowlist empty" in results[0].detail


# ------------------------------------------------------------------- tcc


def test_tcc_pass():
    results, _ = run_checks([mac()], mac_ok_table(), ["tcc"])
    assert results[0].status == "PASS", results[0].detail


def test_tcc_rejects_cdhash_csreq():
    t = mac_ok_table()
    t[("macbookpro", fh.csreq_decode_cmd("FADE0C00AA"))] = (0, CSREQ_CDHASH + "\n", "")
    results, _ = run_checks([mac()], t, ["tcc"])
    assert results[0].status == "FAIL" and "cdhash-based" in results[0].detail


def test_tcc_requires_leaf_cn():
    t = mac_ok_table()
    t[("macbookpro", fh.csreq_decode_cmd("FADE0C00BB"))] = (0, 'identifier "org.deskflow.deskflow-core" and anchor apple generic\n', "")
    results, _ = run_checks([mac()], t, ["tcc"])
    assert results[0].status == "FAIL" and "lacks certificate leaf[subject.CN]" in results[0].detail


def test_tcc_requires_auth_value_2():
    t = mac_ok_table()
    t[("macbookpro", fh.tcc_query_cmd(fh.SYSTEM_TCC_DB))] = (0, tcc_rows(listen=0), "")
    results, _ = run_checks([mac()], t, ["tcc"])
    assert results[0].status == "FAIL"
    assert "kTCCServiceListenEvent org.deskflow.deskflow-core auth_value=0" in results[0].detail
    assert "kTCCServiceListenEvent: no deskflow client with auth_value=2" in results[0].detail


def test_tcc_fails_when_no_rows():
    t = mac_ok_table()
    t[("macbookpro", fh.tcc_query_cmd(fh.SYSTEM_TCC_DB))] = (0, "", "")
    results, _ = run_checks([mac()], t, ["tcc"])
    assert results[0].status == "FAIL" and "kTCCServiceAccessibility: no deskflow client" in results[0].detail


def test_tcc_sqlite_error():
    t = mac_ok_table()
    t[("macbookpro", fh.tcc_query_cmd(fh.SYSTEM_TCC_DB))] = (1, "", "Error: unable to open database")
    results, _ = run_checks([mac()], t, ["tcc"])
    assert results[0].status == "FAIL" and "sqlite3 system TCC.db rc=1" in results[0].detail


def test_tcc_check_permissions_denied():
    t = mac_ok_table()
    t[("macbookpro", fh.check_permissions_cmd())] = (1, "accessibility: granted\niohid: denied\n", "")
    results, _ = run_checks([mac()], t, ["tcc"])
    assert results[0].status == "FAIL" and "--check-permissions rc=1" in results[0].detail


def test_tcc_check_permissions_flag_unsupported():
    t = mac_ok_table()
    t[("macbookpro", fh.check_permissions_cmd())] = (2, "", "deskflow-core: unrecognized option '--check-permissions'")
    results, _ = run_checks([mac()], t, ["tcc"])
    assert results[0].status == "FAIL" and "deskflow-core lacks --check-permissions" in results[0].detail


# --------------------------------------------------------------- session


def test_session_mac_pass_and_fail():
    results, _ = run_checks([mac()], mac_ok_table(), ["session"])
    assert results[0].status == "PASS"
    t = mac_ok_table()
    t[("macbookpro", fh.launchctl_cmd())] = (0, "gui/501 => { services = { com.apple.foo } }", "")
    t[("macbookpro", fh.deskflow_gui_running_cmd())] = (1, "", "")
    t[("macbookpro", fh.mouser_running_cmd())] = (1, "", "")
    results, _ = run_checks([mac()], t, ["session"])
    assert results[0].status == "FAIL"
    assert "io.github.hughesyadaddy.deskflow-core not loaded" in results[0].detail
    assert "Deskflow GUI not running" in results[0].detail
    assert "Mouser not running" in results[0].detail
    # Mouser is intentionally not launchd-managed: a process check is what
    # matters, launchd not loading it must not fail the check on its own.
    t = mac_ok_table()
    t[("macbookpro", fh.launchctl_cmd())] = (0, "gui/501 => { services = { io.github.hughesyadaddy.deskflow-core } }", "")
    results, _ = run_checks([mac()], t, ["session"])
    assert results[0].status == "PASS"


# ------------------------------------------------------------- instances


def test_instances_cmd_expands_home_and_targets_the_ctl():
    assert fh.ctl_assert_single_cmd() == '"$HOME"/Desktop/deskflow/scripts/deskflow-ctl assert-single'
    assert fh.ctl_assert_single_cmd("/opt/df/") == "/opt/df/scripts/deskflow-ctl assert-single"
    assert "instances" in fh.ALL_CHECKS


def test_instances_mac_pass_and_fail():
    results, runner = run_checks([mac()], mac_ok_table(), ["instances"])
    assert results[0].status == "PASS" and "core=1" in results[0].detail
    assert ("macbookpro", fh.ctl_assert_single_cmd()) in runner.calls
    t = mac_ok_table()
    t[("macbookpro", fh.ctl_assert_single_cmd())] = (1, "", CTL_FAIL)
    results, _ = run_checks([mac()], t, ["instances"])
    assert results[0].status == "FAIL"
    assert "rc=1" in results[0].detail
    assert "deskflow-core count=2 (want 1)" in results[0].detail
    assert "non-canonical process pid=777" in results[0].detail


def test_instances_mac_root_from_env():
    t = {("macbookpro", fh.ctl_assert_single_cmd("/srv/deskflow")): (0, CTL_OK, "")}
    results, _ = run_checks([mac()], t, ["instances"], env={"FLEET_DESKFLOW_PATH_macos": "/srv/deskflow"})
    assert results[0].status == "PASS"


def test_instances_windows_via_ps1_fixture_json():
    cmd = fh.ps1_cmd(fh.WIN_DESKFLOW_ROOT, ["instances"], "", fh.DEFAULT_MESH_PORT, [])
    assert "-Checks instances" in cmd
    t = {("tiny11", cmd): (0, json.dumps(WIN_INSTANCES_PASS), "")}
    results, _ = run_checks([win()], t, ["instances"])
    assert results[0].status == "PASS" and "core=1 child of service" in results[0].detail
    t = {("tiny11", cmd): (1, json.dumps(WIN_INSTANCES_FAIL), "")}
    results, _ = run_checks([win()], t, ["instances"])
    assert results[0].status == "FAIL" and "deskflow-core.exe count=2" in results[0].detail


def test_instances_is_included_in_all(tmp_path, capsys):
    env_file = write_env(tmp_path)
    runner = FakeRunner(mac_ok_table())
    rc = fh.main(["--json", "--env", str(env_file)], runner=runner)
    out = json.loads(capsys.readouterr().out)
    assert rc == 0
    assert [r for r in out["results"] if r["check"] == "instances"][0]["status"] == "PASS"
    assert ("macbookpro", fh.ctl_assert_single_cmd()) in runner.calls


def test_scan_includes_deskflow_prio_and_flags_it_when_adhoc():
    assert "ls /usr/local/bin/deskflow-prio 2>/dev/null" in fh.macho_scan_cmd()
    t = mac_ok_table()
    t[("macbookpro", fh.macho_scan_cmd())] = (0, good_scan() + codesign_block("/usr/local/bin/deskflow-prio", "dfprio", adhoc=True), "")
    results, _ = run_checks([mac()], t, ["sign", "no-adhoc", "identifiers"])
    by = {r.check: r for r in results}
    assert by["no-adhoc"].status == "FAIL" and "deskflow-prio" in by["no-adhoc"].detail
    assert by["sign"].status == "FAIL" and "deskflow-prio" in by["sign"].detail
    assert by["identifiers"].status == "FAIL" and "deskflow-prio=dfprio" in by["identifiers"].detail


# ---------------------------------------------------------------- bridge


def test_bridge_cmd_targets_loopback_lego_port_with_nc_timeout():
    cmd = fh.bridge_status_cmd()
    assert "bridge" in fh.ALL_CHECKS
    assert "nc -w2 127.0.0.1 19795" in cmd
    assert '{"t":"status"}' in cmd.replace("\\n", "")
    assert fh.BRIDGE_PORT == 19795 and fh.BRIDGE_PEER == "deskflow-core"


def test_parse_bridge_status():
    ok, detail = fh.parse_bridge_status(BRIDGE_OK)
    assert ok and "attached to deskflow-core" in detail and "session=a1b2" in detail
    ok, detail = fh.parse_bridge_status("")
    assert not ok and "no reply" in detail
    ok, detail = fh.parse_bridge_status("garbage\n")
    assert not ok and "unparseable" in detail
    # attached=false with the right peer is the correct steady state before any
    # device-connect has been observed over the link -- the link itself is what
    # proves the bridge is wired up, so this is a PASS with a note, not a FAIL.
    ok, detail = fh.parse_bridge_status('{"attached":false,"peer":"deskflow-core"}')
    assert ok and "not yet attached" in detail
    ok, detail = fh.parse_bridge_status('{"attached":true,"peer":"mouser-gui"}')
    assert not ok and 'peer="mouser-gui"' in detail
    ok, detail = fh.parse_bridge_status('{"attached":true}')
    assert not ok and "peer=null" in detail
    # Only the first JSON object line counts; a noisy banner before it is ignored.
    ok, _ = fh.parse_bridge_status("hello\n" + BRIDGE_OK + "\n")
    assert ok


def test_bridge_mac_pass_and_fail():
    results, runner = run_checks([mac()], mac_ok_table(), ["bridge"])
    assert [r.check for r in results] == ["bridge"]
    assert results[0].status == "PASS" and "deskflow-core" in results[0].detail
    assert ("macbookpro", fh.bridge_status_cmd()) in runner.calls

    # Linked but idle (no device-connect observed yet): PASS, noted as not-yet-attached.
    t = mac_ok_table()
    t[("macbookpro", fh.bridge_status_cmd())] = (0, '{"t":"status","attached":false,"peer":"deskflow-core"}\n', "")
    results, _ = run_checks([mac()], t, ["bridge"])
    assert results[0].status == "PASS" and "not yet attached" in results[0].detail

    # Mouser up but no peer at all (legacy/off mode): FAIL, named.
    t[("macbookpro", fh.bridge_status_cmd())] = (0, '{"t":"status","attached":false,"peer":null}\n', "")
    results, _ = run_checks([mac()], t, ["bridge"])
    assert results[0].status == "FAIL" and "peer=null" in results[0].detail

    # Nothing listening on 19795: nc exits 1 with no output.
    t[("macbookpro", fh.bridge_status_cmd())] = (1, "", "")
    results, _ = run_checks([mac()], t, ["bridge"])
    assert results[0].status == "FAIL" and "19795" in results[0].detail

    # Wrong peer attached (e.g. a stray build from another path).
    t[("macbookpro", fh.bridge_status_cmd())] = (0, '{"attached":true,"peer":"deskflow-core-dev"}\n', "")
    results, _ = run_checks([mac()], t, ["bridge"])
    assert results[0].status == "FAIL" and "deskflow-core-dev" in results[0].detail


def test_bridge_windows_via_ps1_fixture_json():
    cmd = fh.ps1_cmd(fh.WIN_DESKFLOW_ROOT, ["bridge"], "", fh.DEFAULT_MESH_PORT, [])
    assert "-Checks bridge" in cmd
    t = {("tiny11", cmd): (0, json.dumps([{"check": "bridge", "status": "PASS",
                                            "detail": "attached to deskflow-core session=a1b2 proto=2"}]), "")}
    results, _ = run_checks([win()], t, ["bridge"])
    assert results[0].status == "PASS" and "deskflow-core" in results[0].detail
    t = {("tiny11", cmd): (1, json.dumps([{"check": "bridge", "status": "FAIL",
                                            "detail": "attached=false (want true); peer=null (want 'deskflow-core')"}]), "")}
    results, _ = run_checks([win()], t, ["bridge"])
    assert results[0].status == "FAIL" and "attached=false" in results[0].detail


def test_bridge_is_included_in_all(tmp_path, capsys):
    env_file = write_env(tmp_path)
    runner = FakeRunner(mac_ok_table())
    rc = fh.main(["--json", "--env", str(env_file)], runner=runner)
    out = json.loads(capsys.readouterr().out)
    assert rc == 0
    assert [r for r in out["results"] if r["check"] == "bridge"][0]["status"] == "PASS"
    assert ("macbookpro", fh.bridge_status_cmd()) in runner.calls


# ----------------------------------------------------------- loginbridge


def test_loginbridge_cmds_target_root_paths_and_use_sudo_n_only():
    assert "loginbridge" in fh.ALL_CHECKS and "loginbridge" in fh.MAC_ONLY
    assert fh.login_bridge_plist_cmd().startswith("test -f /Library/LaunchAgents/org.deskflow.vhid-bridge.plist && plutil -lint")
    assert "ProgramArguments.0" in fh.login_bridge_plist_cmd()
    assert fh.login_bridge_log_mode_cmd() == "stat -f %Lp /var/log/deskflow-vhid-bridge.log"
    assert fh.sudo_probe_cmd() == "sudo -n true"
    assert fh.login_bridge_agent_cmd() == "sudo -n launchctl print loginwindow/org.deskflow.vhid-bridge"
    assert fh.login_bridge_keystroke_cmd() == "sudo -n grep -c 'key down id=' /var/log/deskflow-vhid-bridge.log"
    assert fh.login_bridge_calibrate_cmd() == "grep -c -- --calibrate /Library/LaunchAgents/org.deskflow.vhid-bridge.plist"
    cmd = fh.login_bridge_log_since_start_cmd()
    assert cmd.startswith("sudo -n awk '") and cmd.endswith("' /var/log/deskflow-vhid-bridge.log")
    assert "/\\[bridge\\] starting/{n=0; buf=\"\"}" in cmd and "n<400" in cmd and "vhid connect_failed" in cmd
    # never an interactive sudo in any command string the tool ships
    import re as _re
    for m in _re.finditer(r'f?"([^"\n]*sudo[^"\n]*)"', TOOL.read_text()):
        assert "sudo -n" in m.group(1), m.group(1)


def test_loginbridge_pass_reports_pid_and_zero_keystrokes():
    results, runner = run_checks([mac()], mac_ok_table(), ["loginbridge"])
    assert [r.check for r in results] == ["loginbridge"]
    assert results[0].status == "PASS", results[0].detail
    assert "agent pid 611" in results[0].detail and "0 keystrokes" in results[0].detail
    assert "--calibrate" in results[0].detail and "virtual HID ready 7s after start" in results[0].detail
    assert ("macbookpro", fh.login_bridge_keystroke_cmd()) in runner.calls
    assert ("macbookpro", fh.login_bridge_log_since_start_cmd()) in runner.calls


def test_parse_login_bridge_log_uses_newest_start_and_ready_pair():
    ok, detail = fh.parse_login_bridge_log(LOGIN_BRIDGE_LOG_OK)
    assert ok and detail == "virtual HID ready 7s after start, no connect_failed since"
    # the connect_failed BEFORE ready (daemon not up yet) is the normal cold-boot path
    assert "connect_failed" not in detail.split("no ")[0]


def test_parse_login_bridge_log_fails_on_slow_ready_or_connect_failed_after_ready():
    slow = LOGIN_BRIDGE_LOG_OK.replace("2026-09-22T09:00:09-0400 [bridge] virtual HID ready",
                                       "2026-09-22T09:00:33-0400 [bridge] virtual HID ready")
    ok, detail = fh.parse_login_bridge_log(slow)
    assert not ok and "31s after start (want <= 30s)" in detail
    ok, _ = fh.parse_login_bridge_log(slow, max_ready_s=40)
    assert ok

    broken = LOGIN_BRIDGE_LOG_OK + "2026-09-22T09:02:00-0400 [bridge] vhid connect_failed: 61\n"
    ok, detail = fh.parse_login_bridge_log(broken)
    assert not ok and "1 'vhid connect_failed' line(s) after virtual HID ready" in detail


def test_parse_login_bridge_log_fails_when_never_ready_or_untimestamped():
    waiting = LOGIN_BRIDGE_LOG_OK.split("2026-09-22T09:00:09-0400 [bridge] virtual HID ready")[0] + \
        "2026-09-22T09:00:42-0400 [bridge] waiting for virtual HID daemon (40s)\n"
    ok, detail = fh.parse_login_bridge_log(waiting)
    assert not ok and "still waiting for the virtual HID daemon (40s)" in detail

    ok, detail = fh.parse_login_bridge_log("")
    assert not ok and "no '[bridge] starting' line" in detail

    # a pre-K3 bridge: no timestamps, no start line at all
    old = "[bridge] vhid connect_failed: 61\n[bridge] virtual HID device not ready (is the Karabiner daemon running?)\n"
    ok, detail = fh.parse_login_bridge_log(old)
    assert not ok and "pre-K3 build" in detail

    untimestamped_start = "[bridge] starting pid=1\n[bridge] virtual HID ready; server candidates: x port 24800\n"
    ok, detail = fh.parse_login_bridge_log(untimestamped_start)
    assert not ok and "no timestamp" in detail


def test_loginbridge_fails_when_plist_lacks_calibrate_or_log_shows_slow_daemon():
    t = mac_ok_table()
    t[("macbookpro", fh.login_bridge_calibrate_cmd())] = (1, "0\n", "")
    results, _ = run_checks([mac()], t, ["loginbridge"])
    assert results[0].status == "FAIL" and "plist lacks --calibrate" in results[0].detail

    t = mac_ok_table()
    t[("macbookpro", fh.login_bridge_log_since_start_cmd())] = (
        0, LOGIN_BRIDGE_LOG_OK + "2026-09-22T09:03:00-0400 [bridge] vhid connect_failed: 61\n", "")
    results, _ = run_checks([mac()], t, ["loginbridge"])
    assert results[0].status == "FAIL" and "after virtual HID ready" in results[0].detail

    t = mac_ok_table()
    t[("macbookpro", fh.login_bridge_log_since_start_cmd())] = (1, "", "tail: /var/log/deskflow-vhid-bridge.log: No such file")
    results, _ = run_checks([mac()], t, ["loginbridge"])
    assert results[0].status == "FAIL" and "log read failed" in results[0].detail


def test_login_bridge_since_start_awk_cuts_at_newest_start_and_keeps_late_connect_failed(tmp_path):
    # Run the real awk program (without sudo) over a log where the newest start
    # is followed by more than the cap and a late connect_failed past it.
    import subprocess
    log = tmp_path / "bridge.log"
    body = LOGIN_BRIDGE_LOG_OK + "".join(
        f"2026-09-22T09:0{1 + i // 60}:{i % 60:02d}-0400 [bridge] enter 1,1 of 2x2; [keys] session letters shifted=0 unshifted=0 caps-edges=0\n"
        for i in range(500)) + "2026-09-22T10:00:00-0400 [bridge] vhid connect_failed: 61\n"
    log.write_text(body)
    cmd = fh.login_bridge_log_since_start_cmd(str(log), max_lines=20).replace("sudo -n ", "", 1)
    out = subprocess.run(cmd, shell=True, capture_output=True, text=True, check=True).stdout
    lines = out.splitlines()
    assert lines[0].endswith("[bridge] starting pid=611")
    assert "starting pid=400" not in out
    assert len(lines) == 20 + 1 and lines[-1].endswith("vhid connect_failed: 61")
    ok, detail = fh.parse_login_bridge_log(out)
    assert not ok and "after virtual HID ready" in detail


def test_loginbridge_skips_privileged_parts_without_passwordless_sudo():
    t = mac_ok_table()
    t[("macbookpro", fh.sudo_probe_cmd())] = (1, "", "sudo: a password is required")
    results, runner = run_checks([mac()], t, ["loginbridge"])
    assert results[0].status == "SKIP" and results[0].ok
    assert "passwordless sudo" in results[0].detail and "log mode 600" in results[0].detail
    assert ("macbookpro", fh.login_bridge_agent_cmd()) not in runner.calls
    assert ("macbookpro", fh.login_bridge_keystroke_cmd()) not in runner.calls
    assert ("macbookpro", fh.login_bridge_log_since_start_cmd()) not in runner.calls
    # the unprivileged --calibrate check still ran
    assert ("macbookpro", fh.login_bridge_calibrate_cmd()) in runner.calls
    # unprivileged problems still FAIL even without sudo
    t[("macbookpro", fh.login_bridge_log_mode_cmd())] = (0, "644\n", "")
    results, _ = run_checks([mac()], t, ["loginbridge"])
    assert results[0].status == "FAIL" and "mode 644" in results[0].detail


def test_loginbridge_fails_on_missing_plist_wrong_program_or_agent_not_loaded():
    t = mac_ok_table()
    t[("macbookpro", fh.login_bridge_plist_cmd())] = (1, "", "")
    results, _ = run_checks([mac()], t, ["loginbridge"])
    assert results[0].status == "FAIL" and "missing or does not lint" in results[0].detail

    t = mac_ok_table()
    t[("macbookpro", fh.login_bridge_plist_cmd())] = (0, "/usr/local/bin/deskflow-vhid-bridge\n", "")
    results, _ = run_checks([mac()], t, ["loginbridge"])
    assert results[0].status == "FAIL" and "plist program /usr/local/bin/deskflow-vhid-bridge !=" in results[0].detail

    t = mac_ok_table()
    t[("macbookpro", fh.login_bridge_agent_cmd())] = (113, "", "Could not find service")
    results, _ = run_checks([mac()], t, ["loginbridge"])
    assert results[0].status == "FAIL" and "not loaded" in results[0].detail


def test_loginbridge_fails_when_the_log_holds_keystrokes_or_grep_breaks():
    t = mac_ok_table()
    t[("macbookpro", fh.login_bridge_keystroke_cmd())] = (0, "641\n", "")
    results, _ = run_checks([mac()], t, ["loginbridge"])
    assert results[0].status == "FAIL" and "641 'key down id=' lines" in results[0].detail
    t[("macbookpro", fh.login_bridge_keystroke_cmd())] = (2, "", "grep: /var/log/deskflow-vhid-bridge.log: No such file")
    results, _ = run_checks([mac()], t, ["loginbridge"])
    assert results[0].status == "FAIL" and "keystroke grep failed" in results[0].detail


def test_loginbridge_agent_loaded_without_pid_is_fine_outside_the_login_window():
    t = mac_ok_table()
    t[("macbookpro", fh.login_bridge_agent_cmd())] = (0, LOGIN_BRIDGE_PRINT.replace("\tpid = 611\n", ""), "")
    results, _ = run_checks([mac()], t, ["loginbridge"])
    assert results[0].status == "PASS" and "no pid" in results[0].detail


def test_loginbridge_is_mac_only_and_included_in_all(tmp_path, capsys):
    results, runner = run_checks([win()], {}, ["loginbridge"])
    assert results[0].status == "SKIP" and runner.calls == []
    env_file = write_env(tmp_path)
    runner = FakeRunner(mac_ok_table())
    rc = fh.main(["--json", "--env", str(env_file)], runner=runner)
    out = json.loads(capsys.readouterr().out)
    assert rc == 0
    assert [r for r in out["results"] if r["check"] == "loginbridge"][0]["status"] == "PASS"


def test_bridge_ps1_collector_declares_the_check():
    ps1 = (Path(fh.__file__).parent / "fleet-health.ps1").read_text()
    assert '"bridge"       { $results += Test-Bridge $BridgePort }' in ps1
    assert "System.Net.Sockets.TcpClient" in ps1
    assert '"instances", "bridge")' in ps1  # part of "all"


# ------------------------------------------------------------------ mesh


def test_mesh_all_pairs_mac_and_windows():
    hosts = [mac("hackintosh"), mac("macbookpro"), win("tiny11")]
    t = {}
    t.update(mac_ok_table("hackintosh", peers=["macbookpro", "tiny11"]))
    t.update(mac_ok_table("macbookpro", peers=["hackintosh", "tiny11"]))
    t[("macbookpro", fh.nc_cmd("tiny11", fh.DEFAULT_MESH_PORT))] = (1, "", "")
    win_cmd = fh.ps1_cmd(
        "C:/Users/alexh/Desktop/deskflow", ["mesh"], "", fh.DEFAULT_MESH_PORT, ["hackintosh", "macbookpro"]
    )
    t[("tiny11", win_cmd)] = (0, json.dumps([
        {"check": "mesh", "status": "PASS", "detail": "TINY11 -> hackintosh:24851 ok"},
        {"check": "mesh", "status": "PASS", "detail": "TINY11 -> macbookpro:24851 ok"},
    ]), "")
    results, runner = run_checks(hosts, t, ["mesh"])
    mesh = by_check(results, "mesh")
    assert len(mesh) == 6
    failed = [r for r in mesh if r.status == "FAIL"]
    assert len(failed) == 1 and failed[0].host == "macbookpro" and "macbookpro -> tiny11" in failed[0].detail
    assert any("Test-NetConnection" not in c and "-Peers hackintosh,macbookpro" in c for _, c in runner.calls)


def test_mesh_port_from_env_and_single_host_skip():
    hosts = [mac("hackintosh"), mac("macbookpro")]
    t = {("hackintosh", fh.nc_cmd("macbookpro", 24801)): (0, "", ""),
         ("macbookpro", fh.nc_cmd("hackintosh", 24801)): (0, "", "")}
    results, _ = run_checks(hosts, t, ["mesh"], env={"FLEET_MESH_PORT": "24801"})
    assert [r.status for r in results] == ["PASS", "PASS"]
    results, _ = run_checks([mac("solo")], {}, ["mesh"])
    assert results[0].status == "SKIP" and results[0].ok


# --------------------------------------------------------------- windows


def test_windows_authenticode_and_session_via_ps1():
    env = {"FLEET_DESKFLOW_PATH_windows": "C:/Users/alexh/Desktop/deskflow", "DESKFLOW_SIGN_THUMBPRINT": "AB12"}
    cmd = fh.ps1_cmd("C:/Users/alexh/Desktop/deskflow", ["authenticode", "session"], "AB12", fh.DEFAULT_MESH_PORT, [])
    assert cmd.startswith("powershell.exe -NoProfile -ExecutionPolicy Bypass -File")
    assert "fleet-health.ps1" in cmd and "-Thumbprint AB12" in cmd
    t = {("tiny11", cmd): (0, json.dumps([
        {"check": "authenticode", "status": "PASS", "detail": "12 binaries Valid with thumbprint AB12"},
        {"check": "session", "status": "FAIL", "detail": "service Deskflow is STOPPED; Mouser only in session 0"},
    ]), "")}
    results, _ = run_checks([win()], t, ["authenticode", "session"], env=env, explicit=False)
    assert by_check(results, "authenticode")[0].status == "PASS"
    assert by_check(results, "session")[0].status == "FAIL"
    assert "STOPPED" in by_check(results, "session")[0].detail


def test_windows_authenticode_thumbprint_mismatch():
    cmd = fh.ps1_cmd(fh.WIN_DESKFLOW_ROOT, ["authenticode"], "", fh.DEFAULT_MESH_PORT, [])
    t = {("tiny11", cmd): (1, json.dumps([{"check": "authenticode", "status": "FAIL",
                                            "detail": "deskflow.exe: thumbprint DEADBEEF"}]), "")}
    results, _ = run_checks([win()], t, ["authenticode"])
    assert results[0].status == "FAIL" and "DEADBEEF" in results[0].detail


def test_windows_ps1_garbage_output_fails_every_check():
    cmd = fh.ps1_cmd(fh.WIN_DESKFLOW_ROOT, ["authenticode", "session"], "", fh.DEFAULT_MESH_PORT, [])
    t = {("tiny11", cmd): (255, "", "ssh: connect to host tiny11 port 22: Connection refused")}
    results, _ = run_checks([win()], t, ["authenticode", "session"], explicit=False)
    assert [r.status for r in results] == ["FAIL", "FAIL"]
    assert "Connection refused" in results[0].detail


def test_windows_ps1_missing_result_is_fail():
    cmd = fh.ps1_cmd(fh.WIN_DESKFLOW_ROOT, ["authenticode", "session"], "", fh.DEFAULT_MESH_PORT, [])
    t = {("tiny11", cmd): (0, json.dumps([{"check": "authenticode", "status": "PASS", "detail": ""}]), "")}
    results, _ = run_checks([win()], t, ["authenticode", "session"], explicit=False)
    assert by_check(results, "session")[0].status == "FAIL"
    assert "no result" in by_check(results, "session")[0].detail


def test_mac_only_checks_skip_on_windows_when_explicit():
    results, runner = run_checks([win()], {}, ["tcc"])
    assert results[0].status == "SKIP" and results[0].ok and runner.calls == []
    results, _ = run_checks([mac()], {}, ["authenticode"])
    assert results[0].status == "SKIP" and results[0].ok


# ------------------------------------------------------------------ main


def write_env(tmp_path):
    p = tmp_path / "fleet.env"
    p.write_text('FLEET_HOSTS="macbookpro"\nFLEET_SSH_macbookpro=macbookpro\n')
    return p


def test_main_exit_codes_and_json(tmp_path, capsys):
    env_file = write_env(tmp_path)
    runner = FakeRunner(mac_ok_table())
    rc = fh.main(["--host", "all", "--check", "sign", "--json", "--env", str(env_file)], runner=runner)
    out = json.loads(capsys.readouterr().out)
    assert rc == 0 and out["ok"] is True
    assert out["results"][0] == {"host": "macbookpro", "check": "sign", "status": "PASS",
                                 "detail": out["results"][0]["detail"]}

    t = mac_ok_table()
    t[("macbookpro", fh.macho_scan_cmd())] = (0, good_scan() + codesign_block("/a/b", "org.deskflow.deskflow", adhoc=True), "")
    rc = fh.main(["--check", "no-adhoc", "--env", str(env_file)], runner=FakeRunner(t))
    text = capsys.readouterr().out
    assert rc == 1
    assert "host" in text.splitlines()[0] and "| check" in text.splitlines()[0]
    assert "FAIL" in text and "Signature=adhoc" in text


def test_main_all_checks_single_mac_host(tmp_path, capsys):
    env_file = write_env(tmp_path)
    rc = fh.main(["--env", str(env_file)], runner=FakeRunner(mac_ok_table()))
    text = capsys.readouterr().out
    assert rc == 0
    for check in ("sign", "no-adhoc", "identifiers", "tcc", "session", "mesh", "instances", "bridge", "loginbridge"):
        assert f"| {check}" in text
    assert "authenticode" not in text  # windows-only, not shown for a mac unless explicit


def test_main_missing_env(tmp_path, capsys):
    rc = fh.main(["--env", str(tmp_path / "nope.env")], runner=FakeRunner())
    assert rc == 2 and "missing" in capsys.readouterr().err


def test_main_env_defaults_to_fleet_env_file(tmp_path, capsys, monkeypatch):
    # The controllers export FLEET_ENV_FILE; fleet-health must read the same variable (FLEET_ENV still works).
    env_file = write_env(tmp_path)
    monkeypatch.setenv("FLEET_ENV_FILE", str(env_file))
    monkeypatch.delenv("FLEET_ENV", raising=False)
    rc = fh.main(["--host", "all", "--check", "sign", "--json"], runner=FakeRunner(mac_ok_table()))
    out = json.loads(capsys.readouterr().out)
    assert rc == 0 and out["ok"] is True and out["results"][0]["host"] == "macbookpro"

    monkeypatch.setenv("FLEET_ENV_FILE", str(tmp_path / "absent.env"))
    rc = fh.main(["--check", "sign"], runner=FakeRunner(mac_ok_table()))
    assert rc == 2 and "absent.env" in capsys.readouterr().err


def test_main_json_shape_is_ok_plus_results(tmp_path, capsys):
    # The shape fleet-deploy.sh / .ps1 --self-test fold per host: {ok, results:[{host,check,status,detail}]}.
    env_file = write_env(tmp_path)
    rc = fh.main(["--json", "--env", str(env_file)], runner=FakeRunner(mac_ok_table()))
    out = json.loads(capsys.readouterr().out)
    assert rc == 0 and set(out) == {"ok", "results"}
    assert all(set(r) == {"host", "check", "status", "detail"} for r in out["results"])
    assert {r["check"] for r in out["results"]} >= {"sign", "tcc", "mesh"}
    assert "hosts" not in out


def test_local_id_honours_fleet_local_id(monkeypatch):
    monkeypatch.setattr(fh.socket, "gethostname", lambda: "Stranger.local")
    monkeypatch.delenv("FLEET_LOCAL_ID", raising=False)
    assert fh.local_id() == "stranger"
    monkeypatch.setenv("FLEET_LOCAL_ID", "MacBookPro")
    assert fh.local_id() == "macbookpro"


def test_subprocess_runner_replaces_undecodable_powershell_bytes(monkeypatch):
    # fleet-health.ps1 output over ssh can contain 0x83 (not UTF-8); the runner must not raise.
    import subprocess

    def fake_run(argv, **kw):
        assert kw.get("errors") == "replace"
        return subprocess.CompletedProcess(argv, 0, b"[]\x83".decode("utf-8", errors=kw["errors"]), "")

    monkeypatch.setattr(fh.subprocess, "run", fake_run)
    rc, out, _ = fh.subprocess_runner(fh.Host("tiny11", "tiny11", "alexh", "windows", "tiny11"), "quser")
    assert rc == 0 and out.startswith("[]")


def test_format_table_columns():
    text = fh.format_table([fh.Result("h", "sign", "PASS", "fine"), fh.Result("hackintosh", "mesh", "FAIL", "x")])
    lines = text.splitlines()
    assert lines[0].startswith("host       | check | status | detail")
    assert lines[2].startswith("h          | sign  | PASS   | fine")


# ------------------------------------------------------------------ --watch


def test_fold_status_collapses_mesh_legs_and_drops_skips():
    results = [
        fh.Result("a", "mesh", "PASS", "a -> b ok"),
        fh.Result("a", "mesh", "FAIL", "a -> c rc=1"),
        fh.Result("a", "mesh", "PASS", "a -> d ok"),
        fh.Result("a", "tcc", "SKIP", "n/a"),
        fh.Result("b", "sign", "PASS", "fine"),
    ]
    folded = fh.fold_status(results)
    assert folded == {("a", "mesh"): ("FAIL", "a -> c rc=1"), ("b", "sign"): ("PASS", "fine")}


def test_watch_state_debounces_single_pass_blips():
    st = fh.WatchState(confirm=2)
    st.baseline({("h", "session"): ("PASS", "ok")})
    # one failing pass: pending, not reported
    assert st.observe({("h", "session"): ("FAIL", "gui down")}) == []
    # back to PASS: pending cleared, nothing reported
    assert st.observe({("h", "session"): ("PASS", "ok")}) == []
    # two consecutive FAILs: reported once
    assert st.observe({("h", "session"): ("FAIL", "gui down")}) == []
    assert st.observe({("h", "session"): ("FAIL", "gui down")}) == [("h", "session", "PASS", "FAIL", "gui down")]
    # steady FAIL: silent
    assert st.observe({("h", "session"): ("FAIL", "gui down")}) == []
    # recovery also needs two passes
    assert st.observe({("h", "session"): ("PASS", "ok")}) == []
    assert st.observe({("h", "session"): ("PASS", "ok")}) == [("h", "session", "FAIL", "PASS", "ok")]
    # a pair that appears later is adopted silently as its own baseline
    assert st.observe({("h", "session"): ("PASS", "ok"), ("h", "bridge"): ("FAIL", "x")}) == []
    assert st.reported[("h", "bridge")] == "FAIL"


def test_watch_prints_only_confirmed_transitions_and_notifies(tmp_path, capsys):
    env_file = write_env(tmp_path)
    runner = FakeRunner(mac_ok_table())
    good = runner.table[("macbookpro", fh.deskflow_gui_running_cmd())]
    bad = (1, "", "")
    notes = []
    sleeps = []

    def sleep(n):
        sleeps.append(n)
        # pass 1 baseline done. Before pass 2 and 3: GUI gone. Before pass 4/5: back.
        if len(sleeps) in (1, 2):
            runner.table[("macbookpro", fh.deskflow_gui_running_cmd())] = bad
        elif len(sleeps) in (3, 4):
            runner.table[("macbookpro", fh.deskflow_gui_running_cmd())] = good
        else:
            raise KeyboardInterrupt

    rc = fh.main(["--check", "session", "--env", str(env_file), "--watch", "7"], runner=runner,
                 notifier=lambda title, msg: notes.append((title, msg)), sleep=sleep)
    out = capsys.readouterr().out
    assert rc == 0
    assert sleeps == [7, 7, 7, 7, 7]
    assert "| session" in out.splitlines()[2]  # baseline table printed once
    assert "watching every 7s" in out
    transitions = [ln for ln in out.splitlines() if " -> " in ln]
    assert len(transitions) == 2
    assert "macbookpro session PASS -> FAIL" in transitions[0]
    assert "macbookpro session FAIL -> PASS" in transitions[1]
    assert [n[1][:len("session PASS -> FAIL")] for n in notes] == ["session PASS -> FAIL", "session FAIL -> PASS"]
    assert notes[0][0] == "fleet-health macbookpro"


def test_watch_single_blip_is_silent(tmp_path, capsys):
    env_file = write_env(tmp_path)
    runner = FakeRunner(mac_ok_table())
    good = runner.table[("macbookpro", fh.deskflow_gui_running_cmd())]
    notes = []
    sleeps = []

    def sleep(n):
        sleeps.append(n)
        if len(sleeps) == 1:
            runner.table[("macbookpro", fh.deskflow_gui_running_cmd())] = (255, "", "ssh: timed out")
        elif len(sleeps) == 2:
            runner.table[("macbookpro", fh.deskflow_gui_running_cmd())] = good
        else:
            raise KeyboardInterrupt

    rc = fh.main(["--check", "session", "--env", str(env_file), "--watch", "5"], runner=runner,
                 notifier=lambda t, m: notes.append(m), sleep=sleep)
    out = capsys.readouterr().out
    assert rc == 0 and notes == []
    assert not [ln for ln in out.splitlines() if " -> " in ln]


def test_watch_ctrl_c_during_checks_exits_zero(tmp_path, capsys):
    env_file = write_env(tmp_path)
    runner = FakeRunner(mac_ok_table())
    calls = {"n": 0}

    def interrupting_runner(host, cmd):
        calls["n"] += 1
        if calls["n"] > 3:
            raise KeyboardInterrupt
        return runner(host, cmd)

    rc = fh.main(["--check", "session", "--env", str(env_file), "--watch", "1"], runner=interrupting_runner,
                 notifier=lambda t, m: None, sleep=lambda n: None)
    assert rc == 0 and "watching every 1s" in capsys.readouterr().out


def test_watch_rejects_zero_interval(tmp_path, capsys):
    env_file = write_env(tmp_path)
    rc = fh.main(["--check", "sign", "--env", str(env_file), "--watch", "0"], runner=FakeRunner(mac_ok_table()))
    assert rc == 2 and "--watch" in capsys.readouterr().err


def test_notify_macos_uses_osascript_only_on_darwin(monkeypatch):
    calls = []
    monkeypatch.setattr(fh.subprocess, "run", lambda argv, **kw: calls.append(argv))
    monkeypatch.setattr(fh.sys, "platform", "linux")
    fh.notify_macos("t", "m")
    assert calls == []
    monkeypatch.setattr(fh.sys, "platform", "darwin")
    fh.notify_macos("fleet-health macbookpro", 'session PASS -> FAIL: "gui" down')
    assert calls and calls[0][0] == "osascript"
    assert 'display notification "session PASS -> FAIL: \\"gui\\" down" with title "fleet-health macbookpro"' in calls[0][2]
