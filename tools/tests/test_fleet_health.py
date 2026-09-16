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
        (hid, fh.ctl_assert_single_cmd()): (0, CTL_OK + "\n", ""),
        (hid, fh.bridge_status_cmd()): (0, BRIDGE_OK + "\n", ""),
    }
    for p in peers:
        t[(hid, fh.nc_cmd(p, 24800))] = (0, "", "")
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
    results, _ = run_checks([mac()], t, ["session"])
    assert results[0].status == "FAIL"
    assert "io.github.hughesyadaddy.mouser not loaded" in results[0].detail
    assert "io.github.hughesyadaddy.deskflow-core not loaded" in results[0].detail
    assert "Deskflow GUI not running" in results[0].detail
    # launchd owns deskflow-core: a loaded Mouser alone is not enough
    t = mac_ok_table()
    t[("macbookpro", fh.launchctl_cmd())] = (0, "gui/501 => { services = { io.github.hughesyadaddy.mouser } }", "")
    results, _ = run_checks([mac()], t, ["session"])
    assert results[0].status == "FAIL" and "deskflow-core not loaded" in results[0].detail


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
    cmd = fh.ps1_cmd(fh.WIN_DESKFLOW_ROOT, ["instances"], "", 24800, [])
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
    cmd = fh.ps1_cmd(fh.WIN_DESKFLOW_ROOT, ["bridge"], "", 24800, [])
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
    t[("macbookpro", fh.nc_cmd("tiny11", 24800))] = (1, "", "")
    win_cmd = fh.ps1_cmd("C:/Users/alexh/Desktop/deskflow", ["mesh"], "", 24800, ["hackintosh", "macbookpro"])
    t[("tiny11", win_cmd)] = (0, json.dumps([
        {"check": "mesh", "status": "PASS", "detail": "TINY11 -> hackintosh:24800 ok"},
        {"check": "mesh", "status": "PASS", "detail": "TINY11 -> macbookpro:24800 ok"},
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
    results, _ = run_checks(hosts, t, ["mesh"], env={"FLEET_DESKFLOW_PORT": "24801"})
    assert [r.status for r in results] == ["PASS", "PASS"]
    results, _ = run_checks([mac("solo")], {}, ["mesh"])
    assert results[0].status == "SKIP" and results[0].ok


# --------------------------------------------------------------- windows


def test_windows_authenticode_and_session_via_ps1():
    env = {"FLEET_DESKFLOW_PATH_windows": "C:/Users/alexh/Desktop/deskflow", "DESKFLOW_SIGN_THUMBPRINT": "AB12"}
    cmd = fh.ps1_cmd("C:/Users/alexh/Desktop/deskflow", ["authenticode", "session"], "AB12", 24800, [])
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
    cmd = fh.ps1_cmd(fh.WIN_DESKFLOW_ROOT, ["authenticode"], "", 24800, [])
    t = {("tiny11", cmd): (1, json.dumps([{"check": "authenticode", "status": "FAIL",
                                            "detail": "deskflow.exe: thumbprint DEADBEEF"}]), "")}
    results, _ = run_checks([win()], t, ["authenticode"])
    assert results[0].status == "FAIL" and "DEADBEEF" in results[0].detail


def test_windows_ps1_garbage_output_fails_every_check():
    cmd = fh.ps1_cmd(fh.WIN_DESKFLOW_ROOT, ["authenticode", "session"], "", 24800, [])
    t = {("tiny11", cmd): (255, "", "ssh: connect to host tiny11 port 22: Connection refused")}
    results, _ = run_checks([win()], t, ["authenticode", "session"], explicit=False)
    assert [r.status for r in results] == ["FAIL", "FAIL"]
    assert "Connection refused" in results[0].detail


def test_windows_ps1_missing_result_is_fail():
    cmd = fh.ps1_cmd(fh.WIN_DESKFLOW_ROOT, ["authenticode", "session"], "", 24800, [])
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
    for check in ("sign", "no-adhoc", "identifiers", "tcc", "session", "mesh", "instances", "bridge"):
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
