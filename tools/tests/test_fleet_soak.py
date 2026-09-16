"""Tests for tools/fleet-soak (stdlib sampler + Theil-Sen verdict reporter).

No real libproc / top / lsof / launchctl calls: collectors are monkeypatched.
"""

from __future__ import annotations

import importlib.machinery
import importlib.util
import json
import os
import sys
from pathlib import Path

import pytest

TOOL = Path(__file__).resolve().parents[1] / "fleet-soak"


@pytest.fixture(scope="module")
def fs():
    loader = importlib.machinery.SourceFileLoader("fleet_soak", str(TOOL))
    spec = importlib.util.spec_from_loader("fleet_soak", loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


# --------------------------------------------------------------------------
# synthetic JSONL generation
# --------------------------------------------------------------------------

T0 = 1_760_000_000  # arbitrary epoch base


def header(metric="phys_footprint", scenario_source="observed", interval=60, **over):
    h = {
        "header": True,
        "metric": metric,
        "scenario_source": scenario_source,
        "sampler_sha": "deadbeef",
        "seat": "hackintosh",
        "proc": "mouser",
        "interval": interval,
    }
    h.update(over)
    return h


def ts(t: float) -> str:
    import datetime as dt

    return dt.datetime.fromtimestamp(t, dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def sample(t, mb, *, pid=100, start="2026-09-10T00:00:00Z", ports=300, rc=1,
           scenario="device-connected", exe="/Applications/Mouser.app/Contents/MacOS/Mouser"):
    return {
        "ts": ts(t),
        "pid": pid,
        "start_time": start,
        "exe": exe,
        "phys_footprint_mb": round(mb, 3),
        "metric_source": "proc_pid_rusage",
        "rss_mb": round(mb, 3),
        "compressed_mb": 1.0,
        "mach_ports": ports,
        "threads": 12,
        "fds": 60,
        "scenario": scenario,
        "restart_count": rc,
        "heartbeat": True,
    }


def series(hours: float, slope_mb_h: float = 0.0, base: float = 100.0,
           interval: int = 60, noise=None, **kw):
    """Samples every `interval` s for `hours`, footprint = base + slope*h (+ noise)."""
    n = int(hours * 3600 / interval) + 1
    out = []
    for i in range(n):
        t = T0 + i * interval
        h = i * interval / 3600
        mb = base + slope_mb_h * h
        if noise:
            mb += noise(i)
        out.append(sample(t, mb, **kw))
    return out


def write_jsonl(path: Path, hdr, samples):
    lines = []
    if hdr is not None:
        lines.append(json.dumps(hdr))
    lines += [json.dumps(s) for s in samples]
    path.write_text("\n".join(lines) + "\n")
    return path


def report(fs, path, *extra, capsys=None):
    argv = ["report", "--in", str(path), "--json", *extra]
    code = fs.main(argv)
    if capsys is None:
        return code, None
    out = capsys.readouterr().out
    return code, json.loads(out)


# --------------------------------------------------------------------------
# Theil-Sen
# --------------------------------------------------------------------------


def test_theil_sen_known_series(fs):
    xs = [0, 1, 2, 3, 4, 5]
    ys = [1, 3, 5, 7, 9, 11]
    assert fs.theil_sen(xs, ys) == pytest.approx(2.0)


def test_theil_sen_robust_to_outlier(fs):
    xs = list(range(20))
    ys = [2 * x + 5 for x in xs]
    ys[10] = 500  # single outlier should not move the median slope much
    assert fs.theil_sen(xs, ys) == pytest.approx(2.0, abs=0.05)


def test_theil_sen_degenerate(fs):
    assert fs.theil_sen([], []) is None
    assert fs.theil_sen([1.0], [2.0]) is None
    assert fs.theil_sen([1.0, 1.0], [2.0, 3.0]) is None
    assert fs.theil_sen([0, 1, 2], [5, 5, 5]) == pytest.approx(0.0)


def test_theil_sen_bucketing_preserves_slope(fs):
    n = 5 * fs.THEIL_SEN_MAX_POINTS
    xs = [i / 60 for i in range(n)]
    ys = [10 + 0.5 * x + ((i * 7919) % 13 - 6) * 0.01 for i, x in enumerate(xs)]
    assert fs.theil_sen(xs, ys) == pytest.approx(0.5, abs=0.01)


# --------------------------------------------------------------------------
# report: verdicts on synthetic JSONL
# --------------------------------------------------------------------------


def test_flat_series_passes(fs, tmp_path, capsys):
    p = write_jsonl(tmp_path / "flat.jsonl", header(), series(80, 0.0))
    code, res = report(
        fs, p, "--window", "168", "--min-hours", "72", "--slope-max", "0.1",
        "--ports-slope-max", "1", "--max-step", "5", "--max-restarts", "0",
        "--validate", "--seat", "hackintosh", "--proc", "mouser", capsys=capsys,
    )
    assert code == 0, res
    assert res["verdict"] == "PASS"
    assert abs(res["slope_mb_h"]) < 1e-6
    assert res["ports_slope_h"] == pytest.approx(0.0)
    assert res["restarts"] == 0
    assert res["completeness"] == pytest.approx(1.0)
    assert res["span_h"] == pytest.approx(80.0, abs=0.01)
    assert res["max_step_pct"] == pytest.approx(0.0)


def test_leaking_series_fails_with_slope(fs, tmp_path, capsys):
    noise = lambda i: ((i * 7919) % 11 - 5) * 0.2  # +-1 MB jitter
    p = write_jsonl(tmp_path / "leak.jsonl", header(), series(80, 24.0, noise=noise))
    code, res = report(fs, p, "--slope-max", "0.1", "--max-restarts", "0", capsys=capsys)
    assert code == 1
    assert res["verdict"] == "FAIL"
    assert res["slope_mb_h"] == pytest.approx(24.0, abs=0.5)
    assert any("slope" in f for f in res["failures"])


def test_warmup_excluded_two_hours(fs, tmp_path, capsys):
    # steep climb during first 2 h, flat afterwards: slope must be ~0
    s = series(30, 0.0, base=200.0)
    for i, rec in enumerate(s):
        h = i / 60
        if h < 2:
            rec["phys_footprint_mb"] = round(100 + 50 * h, 3)
    p = write_jsonl(tmp_path / "warm.jsonl", header(), s)
    code, res = report(fs, p, "--slope-max", "0.1", capsys=capsys)
    assert code == 0, res
    assert res["warmup_excluded"] == 120
    assert abs(res["slope_mb_h"]) < 1e-6


def test_accelerated_excludes_ten_minutes(fs, tmp_path, capsys):
    s = series(2, 0.0, base=50.0)
    for i, rec in enumerate(s):
        if i < 10:
            rec["phys_footprint_mb"] = 10.0 + i  # ramp inside the first 10 min
    p = write_jsonl(tmp_path / "acc.jsonl", header(), s)
    code, res = report(fs, p, "--accelerated", "--window", "2", "--slope-max", "5",
                       "--ports-slope-max", "10", "--max-restarts", "0", capsys=capsys)
    assert code == 0, res
    assert res["warmup_excluded"] == 10
    assert abs(res["slope_mb_h"]) < 1e-6
    # without --accelerated the whole 2 h series is warm-up -> invalid verdict
    code2, res2 = report(fs, p, "--window", "2", "--slope-max", "5", capsys=capsys)
    assert code2 == 2
    assert res2["verdict"] == "INVALID"


def test_restart_midway_counts_tuple_change_and_runs(fs, tmp_path, capsys):
    a = series(40, 0.0, pid=100, start="2026-09-10T00:00:00Z", rc=1)
    b = series(40, 0.0, pid=200, start="2026-09-11T16:00:00Z", rc=2)
    for i, rec in enumerate(b):
        rec["ts"] = ts(T0 + (len(a) + i) * 60)
    p = write_jsonl(tmp_path / "restart.jsonl", header(), a + b)
    code, res = report(fs, p, "--max-restarts", "0", capsys=capsys)
    assert code == 1
    assert res["restart_tuple_changes"] == 1
    assert res["restart_count_increases"] == 1
    assert res["restarts"] == 2
    assert any("restarts" in f for f in res["failures"])
    # tolerant threshold passes
    code2, _ = report(fs, p, "--max-restarts", "2", capsys=capsys)
    assert code2 == 0


def test_restart_via_pid_change_only(fs, tmp_path, capsys):
    # deskflow-core has no launchd job: restart_count is null throughout
    a = series(10, 0.0, pid=100, rc=None)
    b = series(10, 0.0, pid=101, rc=None)
    for i, rec in enumerate(b):
        rec["ts"] = ts(T0 + (len(a) + i) * 60)
    p = write_jsonl(tmp_path / "r2.jsonl", header(proc="deskflow-core"), a + b)
    code, res = report(fs, p, "--max-restarts", "0", capsys=capsys)
    assert code == 1
    assert res["restarts"] == 1


def test_gap_invalidates_verdict_only_with_validate(fs, tmp_path, capsys):
    s = series(80, 0.0)
    # remove 20 minutes of samples in the middle
    cut = [rec for i, rec in enumerate(s) if not (2000 <= i < 2020)]
    p = write_jsonl(tmp_path / "gap.jsonl", header(), cut)
    code, res = report(fs, p, "--validate", "--slope-max", "0.1", capsys=capsys)
    assert code == 2
    assert res["verdict"] == "INVALID"
    assert any("gap" in i for i in res["invalid"])
    code2, res2 = report(fs, p, "--slope-max", "0.1", capsys=capsys)
    assert code2 == 0, res2


def test_validate_completeness_and_min_hours(fs, tmp_path, capsys):
    # sparse: every other sample missing -> completeness ~0.5 but no 5-min gap
    s = series(80, 0.0, interval=120)
    p = write_jsonl(tmp_path / "sparse.jsonl", header(interval=60), s)
    code, res = report(fs, p, "--validate", capsys=capsys)
    assert code == 2
    assert any("completeness" in i for i in res["invalid"])
    assert not any("gap" in i for i in res["invalid"])

    short = write_jsonl(tmp_path / "short.jsonl", header(), series(10, 0.0))
    code, res = report(fs, short, "--validate", "--min-hours", "72", capsys=capsys)
    assert code == 2
    assert any("min-hours" in i for i in res["invalid"])
    code, res = report(fs, short, "--validate", "--min-hours", "8", capsys=capsys)
    assert code == 0, res


def test_missing_header_is_invalid(fs, tmp_path, capsys):
    p = write_jsonl(tmp_path / "nohdr.jsonl", None, series(80, 0.0))
    code, res = report(fs, p, "--slope-max", "0.1", capsys=capsys)
    assert code == 2
    assert "missing header line" in res["invalid"]
    code, res = report(fs, p, "--validate", capsys=capsys)
    assert code == 2


def test_manual_scenario_source_is_invalid(fs, tmp_path, capsys):
    p = write_jsonl(tmp_path / "manual.jsonl", header(scenario_source="manual"), series(80, 0.0))
    code, res = report(fs, p, "--validate", capsys=capsys)
    assert code == 2
    assert any("scenario_source" in i for i in res["invalid"])


def test_cap_uses_p95(fs, tmp_path, capsys):
    p = write_jsonl(tmp_path / "cap.jsonl", header(), series(30, 0.0, base=160.0))
    code, res = report(fs, p, "--cap", "150", capsys=capsys)
    assert code == 1
    assert res["p95_mb"] == pytest.approx(160.0)
    code, _ = report(fs, p, "--cap", "200", capsys=capsys)
    assert code == 0


def test_daily_step_and_scenario_filter(fs, tmp_path, capsys):
    s = series(72, 0.0, base=100.0)
    for i, rec in enumerate(s):
        if i * 60 >= 48 * 3600:
            rec["phys_footprint_mb"] = 110.0  # +10 % on day 3
        rec["scenario"] = "device-absent" if i % 2 else "device-connected"
    p = write_jsonl(tmp_path / "step.jsonl", header(), s)
    code, res = report(fs, p, "--max-step", "5", capsys=capsys)
    assert code == 1
    assert res["max_step_pct"] == pytest.approx(10.0, abs=0.01)
    code, res = report(fs, p, "--scenario", "device-absent", capsys=capsys)
    assert code == 0
    assert res["samples"] == len(s) // 2
    code, res = report(fs, p, "--scenario", "nope", capsys=capsys)
    assert code == 2


def test_windows_private_bytes_metric(fs, tmp_path, capsys):
    s = series(30, 0.0, base=30.0)
    for rec in s:
        rec["private_bytes_mb"] = rec.pop("phys_footprint_mb")
        rec["mach_ports"] = None
        rec["handles"] = 400
    p = write_jsonl(tmp_path / "win.jsonl", header(metric="private_bytes", seat="tiny11",
                                                    proc="deskflow-daemon"), s)
    code, res = report(fs, p, "--slope-max", "0.1", "--ports-slope-max", "1",
                       "--cap", "40", capsys=capsys)
    assert code == 0, res
    assert res["metric"] == "private_bytes"
    assert res["ports_slope_h"] == pytest.approx(0.0)


def test_missing_file_is_invalid(fs, tmp_path):
    assert fs.main(["report", "--in", str(tmp_path / "nope.jsonl")]) == 2


# --------------------------------------------------------------------------
# sample: header assertions and soak layout (collectors mocked)
# --------------------------------------------------------------------------


@pytest.fixture
def mocked_mac(fs, monkeypatch):
    monkeypatch.setattr(fs, "IS_WIN", False)
    monkeypatch.setattr(fs, "git_sha", lambda: "abc123")
    monkeypatch.setattr(fs, "seat_name", lambda: "hackintosh")
    monkeypatch.setattr(fs, "mac_find_pid", lambda exe: (4242, "2026-09-16T10:00:00-0400"))
    monkeypatch.setattr(fs, "mac_phys_footprint", lambda pid: 123 * fs.MB)
    monkeypatch.setattr(fs, "mac_top", lambda pid: {"rss_mb": 130.0, "compressed_mb": 4.0,
                                                    "mach_ports": 311, "threads": 14})
    monkeypatch.setattr(fs, "mac_fds", lambda pid: 77)
    monkeypatch.setattr(fs, "mac_restart_count", lambda label: 3)
    monkeypatch.setattr(fs, "derive_scenario", lambda proc, home=None, log_path=None: "device-connected")
    return fs


def test_sample_header_macos(mocked_mac, tmp_path):
    fs = mocked_mac
    out = tmp_path / "s.jsonl"
    code = fs.main(["sample", "--label", "io.github.hughesyadaddy.mouser",
                    "--exe", "/Applications/Mouser.app/Contents/MacOS/Mouser",
                    "--out", str(out), "--once"])
    assert code == 0
    lines = out.read_text().splitlines()
    hdr = json.loads(lines[0])
    assert hdr == {"header": True, "metric": "phys_footprint", "scenario_source": "observed",
                   "sampler_sha": "abc123", "seat": "hackintosh", "proc": "mouser",
                   "interval": 60}
    rec = json.loads(lines[1])
    assert rec["pid"] == 4242
    assert rec["phys_footprint_mb"] == 123.0
    assert rec["metric_source"] == "proc_pid_rusage"
    assert rec["mach_ports"] == 311 and rec["threads"] == 14 and rec["fds"] == 77
    assert rec["compressed_mb"] == 4.0 and rec["rss_mb"] == 130.0
    assert rec["restart_count"] == 3
    assert rec["scenario"] == "device-connected"
    assert rec["heartbeat"] is True
    assert rec["ts"].endswith("Z")

    # second run appends without a second header
    assert fs.main(["sample", "--label", "io.github.hughesyadaddy.mouser",
                    "--exe", "/Applications/Mouser.app/Contents/MacOS/Mouser",
                    "--out", str(out), "--once"]) == 0
    lines = out.read_text().splitlines()
    assert len(lines) == 3
    assert sum(1 for ln in lines if json.loads(ln).get("header")) == 1


def test_sample_footprint_fallback_marks_source(mocked_mac, tmp_path, monkeypatch):
    fs = mocked_mac
    monkeypatch.setattr(fs, "mac_phys_footprint", lambda pid: None)
    out = tmp_path / "fb.jsonl"
    assert fs.main(["sample", "--label", "deskflow-core", "--exe", "/x/deskflow-core",
                    "--out", str(out), "--once"]) == 0
    rec = json.loads(out.read_text().splitlines()[1])
    assert rec["metric_source"] == "top-mem-fallback"
    assert rec["phys_footprint_mb"] == 130.0


def test_sample_process_absent_still_heartbeats(mocked_mac, tmp_path, monkeypatch):
    fs = mocked_mac
    monkeypatch.setattr(fs, "mac_find_pid", lambda exe: (None, None))
    out = tmp_path / "absent.jsonl"
    assert fs.main(["sample", "--label", "deskflow-core", "--exe", "/x/deskflow-core",
                    "--out", str(out), "--once"]) == 0
    rec = json.loads(out.read_text().splitlines()[1])
    assert rec["pid"] is None and rec["phys_footprint_mb"] is None
    assert rec["heartbeat"] is True


def test_sample_header_windows_private_bytes(fs, tmp_path, monkeypatch):
    monkeypatch.setattr(fs, "IS_WIN", True)
    monkeypatch.setattr(fs, "git_sha", lambda: "abc123")
    monkeypatch.setattr(fs, "seat_name", lambda: "tiny11")
    probe = tmp_path / "probe.json"
    probe.write_text(json.dumps({"pid": 9, "start_time": "2026-09-16T00:00:00Z",
                                 "exe": "C:\\Deskflow\\deskflow-daemon.exe",
                                 "private_bytes_mb": 22.5, "rss_mb": 30.0, "handles": 210,
                                 "threads": 9, "gdi": 0, "user": 0, "restart_count": 5,
                                 "scenario": "server"}))
    out = tmp_path / "w.jsonl"
    code = fs.main(["sample", "--label", "deskflow-daemon", "--exe", "C:\\Deskflow\\deskflow-daemon.exe",
                    "--out", str(out), "--once", "--probe-json", str(probe)])
    assert code == 0
    lines = out.read_text().splitlines()
    hdr = json.loads(lines[0])
    assert hdr["metric"] == "private_bytes"
    assert hdr["scenario_source"] == "observed"
    assert hdr["seat"] == "tiny11" and hdr["proc"] == "deskflow-daemon"
    rec = json.loads(lines[1])
    assert rec["private_bytes_mb"] == 22.5
    assert rec["fds"] == 210 and rec["handles"] == 210
    assert rec["restart_count"] == 5 and rec["scenario"] == "server"


def test_start_soak_layout_and_latest_symlink(mocked_mac, tmp_path):
    fs = mocked_mac
    base = tmp_path / "harness" / "soak"
    code = fs.main(["sample", "--start-soak", str(base), "--once",
                    "--label", "io.github.hughesyadaddy.mouser", "--exe", "/a/Mouser",
                    "--label", "deskflow-core", "--exe", "/a/deskflow-core"])
    assert code == 0
    latest = base / "latest"
    assert latest.is_symlink()
    day = base / os.readlink(latest)
    assert day.is_dir() and (day / "logs").is_dir()
    assert (latest / "hackintosh-mouser.jsonl").is_file()
    assert (latest / "hackintosh-deskflow-core.jsonl").is_file()
    hdr = json.loads((latest / "hackintosh-deskflow-core.jsonl").read_text().splitlines()[0])
    assert hdr["proc"] == "deskflow-core"
    # --soak appends into the existing latest rather than starting a new day
    assert fs.main(["sample", "--soak", str(base), "--once",
                    "--label", "io.github.hughesyadaddy.mouser", "--exe", "/a/Mouser"]) == 0
    assert len((latest / "hackintosh-mouser.jsonl").read_text().splitlines()) == 3


def test_sample_refuses_headerless_existing_file(mocked_mac, tmp_path):
    fs = mocked_mac
    out = tmp_path / "bad.jsonl"
    out.write_text(json.dumps(sample(T0, 1.0)) + "\n")
    with pytest.raises(SystemExit):
        fs.main(["sample", "--label", "x", "--exe", "/x", "--out", str(out), "--once"])


# --------------------------------------------------------------------------
# parsers for the real macOS tools (pure functions on canned output)
# --------------------------------------------------------------------------


def test_parse_ps_matches_exe_and_start(fs):
    text = (
        "    1 Mon Sep 14 04:43:42 2026     /sbin/launchd\n"
        " 2698 Mon Sep 14 05:37:55 2026     /Applications/Mouser.app/Contents/MacOS/Mouser\n"
        " 7378 Wed Sep 16 12:12:33 2026     /Applications/Deskflow.app/Contents/MacOS/deskflow-core auto\n"
        " 7400 Wed Sep 16 12:12:40 2026     /Applications/Deskflow.app/Contents/MacOS/deskflow-core-helper\n"
    )
    m = fs.parse_ps(text, "/Applications/Deskflow.app/Contents/MacOS/deskflow-core")
    assert [x[0] for x in m] == [7378]
    assert m[0][1].startswith("2026-09-16T12:12:33")
    assert fs.parse_ps(text, "/Applications/Mouser.app/Contents/MacOS/Mouser")[0][0] == 2698


def test_parse_top_units(fs):
    text = "PID MEM CMPRS  #PORTS #TH\n2698 1070M 1030M 413    17\n"
    assert fs.parse_top(text) == {"rss_mb": 1070.0, "compressed_mb": 1030.0,
                                  "mach_ports": 413, "threads": 17}
    text = "Processes: ...\n\nPID MEM CMPRS  #PORTS #TH\n1   28M 8928K- 4783   7/1\n"
    got = fs.parse_top(text)
    assert got["rss_mb"] == 28.0 and got["compressed_mb"] == pytest.approx(8.719, abs=0.001)
    assert got["mach_ports"] == 4783 and got["threads"] == 7
    assert fs.parse_top("") == {}


def test_derive_scenario_from_logs(fs, tmp_path):
    home = tmp_path
    mlog = home / "Library/Logs/Mouser/mouser.log"
    mlog.parent.mkdir(parents=True)
    mlog.write_text("x [MouseHook] Device Connected\ny [MouseHook] Device Disconnected\n")
    assert fs.derive_scenario("mouser", home) == "device-absent"
    mlog.write_text("y [MouseHook] Device Disconnected\nz [MouseHook] Device Connected\n")
    assert fs.derive_scenario("mouser", home) == "device-connected"
    dlog = home / "deskflow.log"
    dlog.write_text("INFO: coordination: starting client epoch towards 1.2.3.4\n"
                    "INFO: coordination: starting server epoch\n")
    assert fs.derive_scenario("deskflow-core", home) == "server"
    assert fs.derive_scenario("deskflow-core", home, log_path=str(mlog)) == "unknown"
    assert fs.derive_scenario("something-else", home) == "unknown"
    (home / "Library/Logs").mkdir(exist_ok=True)
    assert fs.derive_scenario("deskflow-core", tmp_path / "empty") == "unknown"


def test_proc_from_label(fs):
    assert fs.proc_from_label("io.github.hughesyadaddy.mouser") == "mouser"
    assert fs.proc_from_label("application.io.github.hughesyadaddy.deskflow.406638075.406638081") == "deskflow"
    assert fs.proc_from_label("deskflow-core") == "deskflow-core"
    assert fs.proc_from_label("deskflow-daemon") == "deskflow-daemon"
