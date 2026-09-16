"""Tests for harness/behavior-bench.{py,sh} and harness/canary.sh.

Pure statistics / baseline logic is exercised in-process; the CLIs are
exercised via subprocess with synthetic latency arrays and PATH-style shims.
No input events are ever injected.
"""

import importlib.util
import json
import os
import stat
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
HARNESS = ROOT / "harness"
BENCH_PY = HARNESS / "behavior-bench.py"
BENCH_SH = HARNESS / "behavior-bench.sh"
CANARY_SH = HARNESS / "canary.sh"


def _load_module():
    spec = importlib.util.spec_from_file_location("behavior_bench", BENCH_PY)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


bb = _load_module()


def _runs(*latency_lists, sent=None):
    return [{"sent": sent if sent is not None else len(l), "latencies_ms": list(l)}
            for l in latency_lists]


# --------------------------------------------------------------------------
# statistics
# --------------------------------------------------------------------------


def test_percentile_nearest_rank():
    vals = list(range(1, 101))
    assert bb.percentile(vals, 50) == 50
    assert bb.percentile(vals, 95) == 95
    assert bb.percentile([7.0], 95) == 7.0
    assert bb.percentile([3, 1, 2], 100) == 3


def test_summarize_drops_first_run_and_pools_the_rest():
    warmup = [500.0] * 200  # an outlier run that must not count
    steady = [float(i % 10) for i in range(200)]
    runs = _runs(warmup, steady, steady, steady, steady)
    r = bb.summarize(runs, "deskflow-core", "hackintosh")
    assert r["runs"] == 4
    assert r["sent"] == 800
    assert r["delivered"] == 800
    assert r["p50_ms"] == 4.0   # nearest-rank median of 0..9 repeated
    assert r["p95_ms"] == 9.0
    assert set(r) == set(bb.SCHEMA_KEYS)


def test_summarize_single_run_is_not_dropped():
    r = bb.summarize(_runs([1.0, 2.0, 3.0]), "mouser", "s")
    assert r["runs"] == 1 and r["delivered"] == 3 and r["sent"] == 3


def test_summarize_counts_lost_events():
    runs = _runs([1.0] * 200, [1.0] * 198, [1.0] * 200, sent=200)
    r = bb.summarize(runs, "mouser", "s")
    assert r["sent"] == 400 and r["delivered"] == 398


# --------------------------------------------------------------------------
# tolerance / compare
# --------------------------------------------------------------------------


@pytest.mark.parametrize("cur,base,ok", [
    (100.0, 100.0, True),
    (110.0, 100.0, True),    # exactly +10 % passes
    (90.0, 100.0, True),     # exactly -10 % passes
    (110.01, 100.0, False),  # just over
    (89.99, 100.0, False),   # just under
    (0.0, 0.0, True),
    (0.1, 0.0, False),
    (None, 100.0, False),
])
def test_within_tolerance_boundary(cur, base, ok):
    assert bb.within_tolerance(cur, base) is ok


def test_compare_fails_when_delivered_ne_sent():
    base = {"p50_ms": 10.0, "p95_ms": 20.0}
    good = {"p50_ms": 10.0, "p95_ms": 20.0, "delivered": 800, "sent": 800}
    lossy = dict(good, delivered=799)
    assert bb.compare(good, base)["pass"] is True
    c = bb.compare(lossy, base)
    assert c["pass"] is False
    assert c["checks"]["delivered"] is False
    assert c["checks"]["p50_ms"] is True


def test_compare_fails_on_p95_regression_only():
    base = {"p50_ms": 10.0, "p95_ms": 20.0}
    r = {"p50_ms": 10.5, "p95_ms": 22.5, "delivered": 8, "sent": 8}
    c = bb.compare(r, base)
    assert c["checks"]["p50_ms"] is True
    assert c["checks"]["p95_ms"] is False
    assert c["pass"] is False


# --------------------------------------------------------------------------
# baseline write / merge / compare
# --------------------------------------------------------------------------


def test_write_baseline_merges_per_proc(tmp_path):
    r1 = bb.summarize(_runs([1.0], [2.0, 4.0]), "deskflow-core", "hackintosh")
    r2 = bb.summarize(_runs([1.0], [8.0, 9.0]), "mouser", "hackintosh")
    p = bb.write_baseline(r1, tmp_path)
    assert p == tmp_path / "behavior-hackintosh.json"
    bb.write_baseline(r2, tmp_path)
    data = json.loads(p.read_text())
    assert set(data) == {"deskflow-core", "mouser"}
    for proc, entry in data.items():
        assert entry["proc"] == proc
        assert entry["seat"] == "hackintosh"
        assert set(entry) == set(bb.SCHEMA_KEYS)
    # re-writing one proc keeps the other
    r1b = bb.summarize(_runs([1.0], [3.0, 3.0]), "deskflow-core", "hackintosh")
    bb.write_baseline(r1b, tmp_path)
    data = json.loads(p.read_text())
    assert data["deskflow-core"]["p50_ms"] == 3.0
    assert data["mouser"]["p50_ms"] == 8.0


def test_evaluate_without_baseline_is_invalid(tmp_path):
    r = bb.summarize(_runs([1.0], [1.0]), "mouser", "nobody")
    base, cmp_, code = bb.evaluate(r, False, tmp_path)
    assert base is None and cmp_ is None and code == bb.EXIT_INVALID


def test_evaluate_pass_and_fail(tmp_path):
    steady = [10.0] * 190 + [20.0] * 10
    base_res = bb.summarize(_runs(steady, steady, steady), "mouser", "s")
    assert bb.evaluate(base_res, True, tmp_path)[2] == bb.EXIT_PASS
    # +10 % exactly still passes
    edge = [11.0] * 190 + [22.0] * 10
    _, cmp_, code = bb.evaluate(bb.summarize(_runs(edge, edge, edge), "mouser", "s"), False, tmp_path)
    assert code == bb.EXIT_PASS and cmp_["pass"]
    # 11 % regression fails
    bad = [11.1] * 190 + [22.2] * 10
    _, cmp_, code = bb.evaluate(bb.summarize(_runs(bad, bad, bad), "mouser", "s"), False, tmp_path)
    assert code == bb.EXIT_FAIL and not cmp_["pass"]
    # identical timing but a dropped event fails
    lossy = _runs(steady, steady, steady[:-1], sent=200)
    _, cmp_, code = bb.evaluate(bb.summarize(lossy, "mouser", "s"), False, tmp_path)
    assert code == bb.EXIT_FAIL and cmp_["checks"]["delivered"] is False


# --------------------------------------------------------------------------
# observation matching
# --------------------------------------------------------------------------


def test_match_by_tag_ignores_unrelated_and_out_of_order():
    sent = [{"tag": 100, "t_ns": 100}, {"tag": 200, "t_ns": 200}, {"tag": 300, "t_ns": 300}]
    observed = [
        {"tag": 0, "pid": 1, "t_ns": 150},        # untagged noise
        {"tag": 200, "pid": 1, "t_ns": 2_200_000},
        {"tag": 100, "pid": 1, "t_ns": 1_100_000},
        {"tag": 100, "pid": 1, "t_ns": 9_000_000},  # duplicate ignored
    ]
    lat = bb.match_observations(sent, observed, "tag")
    assert lat == pytest.approx([1.0999, 2.1998], abs=1e-3)  # 300 was lost


def test_match_foreign_is_ordinal():
    sent = [{"tag": 1, "t_ns": 1_000_000}, {"tag": 2, "t_ns": 5_000_000}]
    observed = [{"tag": 0, "pid": 9, "t_ns": 3_000_000}, {"tag": 0, "pid": 9, "t_ns": 6_000_000},
                {"tag": 0, "pid": 9, "t_ns": 7_000_000}]
    assert bb.match_observations(sent, observed, "foreign") == [2.0, 1.0]
    with pytest.raises(ValueError):
        bb.match_observations(sent, observed, "bogus")


# --------------------------------------------------------------------------
# CLI (behavior-bench.sh --evaluate, no Quartz involved)
# --------------------------------------------------------------------------


def _write_runs(tmp_path, name, per_proc):
    p = tmp_path / name
    p.write_text(json.dumps(per_proc))
    return p


def _bench(*args, env=None):
    e = dict(os.environ)
    if env:
        e.update(env)
    return subprocess.run(["bash", str(BENCH_SH), *args], capture_output=True, text=True, env=e)


def test_bench_sh_baseline_then_compare_all(tmp_path):
    bdir = tmp_path / "baselines"
    steady = [5.0] * 200
    runs = _write_runs(tmp_path, "runs.json", {
        "deskflow-core": _runs([99.0] * 200, steady, steady, steady, steady),
        "mouser": _runs([99.0] * 200, steady, steady, steady, steady),
    })
    r = _bench("--evaluate", str(runs), "--all", "--seat", "hackintosh", "--baseline",
               "--baseline-dir", str(bdir))
    assert r.returncode == 0, r.stderr
    data = json.loads((bdir / "behavior-hackintosh.json").read_text())
    assert data["deskflow-core"] == {"proc": "deskflow-core", "seat": "hackintosh", "runs": 4,
                                     "p50_ms": 5.0, "p95_ms": 5.0, "delivered": 800, "sent": 800}
    assert set(data) == {"deskflow-core", "mouser"}

    r = _bench("--evaluate", str(runs), "--all", "--seat", "hackintosh", "--json",
               "--baseline-dir", str(bdir))
    assert r.returncode == 0, r.stderr
    out = json.loads(r.stdout)
    assert [o["comparison"]["pass"] for o in out] == [True, True]

    # regress mouser p50 by 20 % -> exit 1, table shows FAIL
    slow = [6.0] * 200
    runs2 = _write_runs(tmp_path, "runs2.json", {
        "deskflow-core": _runs(steady, steady, steady),
        "mouser": _runs(slow, slow, slow),
    })
    r = _bench("--evaluate", str(runs2), "--all", "--seat", "hackintosh", "--baseline-dir", str(bdir))
    assert r.returncode == 1
    assert "FAIL" in r.stdout and "PASS" in r.stdout


def test_bench_sh_baseline_refuses_to_overwrite_unless_operator(tmp_path, monkeypatch):
    bdir = tmp_path / "baselines"
    steady = [5.0] * 200
    runs = _write_runs(tmp_path, "runs.json", _runs(steady, steady, steady))
    env = {"FLEET_OPERATOR": ""}
    r = _bench("--evaluate", str(runs), "--proc", "mouser", "--seat", "hackintosh", "--baseline",
               "--baseline-dir", str(bdir), env=env)
    assert r.returncode == 0, r.stderr
    path = bdir / "behavior-hackintosh.json"
    before = path.read_text()

    slow = [50.0] * 200
    runs2 = _write_runs(tmp_path, "runs2.json", _runs(slow, slow, slow))
    r = _bench("--evaluate", str(runs2), "--proc", "mouser", "--seat", "hackintosh", "--baseline",
               "--baseline-dir", str(bdir), env=env)
    assert r.returncode == 3
    assert "refusing --baseline" in r.stderr and "FLEET_OPERATOR=1" in r.stderr
    assert path.read_text() == before          # untouched
    # another seat's baseline is a different file: not refused
    r = _bench("--evaluate", str(runs2), "--proc", "mouser", "--seat", "macbookpro", "--baseline",
               "--baseline-dir", str(bdir), env=env)
    assert r.returncode == 0, r.stderr
    # --all on an existing file is refused once, up front (not after the first proc wrote)
    r = _bench("--evaluate", str(runs2), "--all", "--seat", "hackintosh", "--baseline",
               "--baseline-dir", str(bdir), env=env)
    assert r.returncode == 3
    assert path.read_text() == before
    # the operator may re-baseline
    r = _bench("--evaluate", str(runs2), "--proc", "mouser", "--seat", "hackintosh", "--baseline",
               "--baseline-dir", str(bdir), env={"FLEET_OPERATOR": "1"})
    assert r.returncode == 0, r.stderr
    assert json.loads(path.read_text())["mouser"]["p50_ms"] == 50.0
    # comparing (no --baseline) never needs the operator flag
    r = _bench("--evaluate", str(runs2), "--proc", "mouser", "--seat", "hackintosh",
               "--baseline-dir", str(bdir), env=env)
    assert r.returncode == 0, r.stderr


def test_bench_sh_no_baseline_exits_2(tmp_path):
    runs = _write_runs(tmp_path, "runs.json", _runs([1.0], [1.0]))
    r = _bench("--evaluate", str(runs), "--proc", "mouser", "--seat", "ghost",
               "--baseline-dir", str(tmp_path / "empty"))
    assert r.returncode == 2
    assert "no baseline" in r.stderr


def test_bench_sh_listener_routes_to_python():
    r = _bench("--listener", "--help")
    assert r.returncode == 0
    assert "listener" in r.stdout and "--count" in r.stdout


# --------------------------------------------------------------------------
# canary.sh with stubbed run-scenario / fleet-soak / behavior-bench
# --------------------------------------------------------------------------


def _shim(path, body):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("#!/usr/bin/env bash\n" + body)
    path.chmod(path.stat().st_mode | stat.S_IEXEC)
    return path


def _canary_env(tmp_path, *, scenario_rc=0, soak_fail_for="", bench_rc=0):
    root = tmp_path / "root"
    scen = root / "harness" / "scenarios"
    scen.mkdir(parents=True)
    (scen / "window-toggle.sh").write_text("#!/usr/bin/env bash\n# proc: deskflow-core\n# automation: full\n")
    (scen / "scroll-im-granted.sh").write_text("#!/usr/bin/env bash\n# proc: mouser\n# automation: full\n")
    (scen / "lock-unlock.sh").write_text("#!/usr/bin/env bash\n# proc: deskflow-core\n# automation: manual\n")
    (scen / "README.txt").write_text("# automation: full\n")  # not a .sh, ignored
    log = tmp_path / "calls.log"
    bin_ = tmp_path / "bin"
    _shim(bin_ / "run-scenario.sh", f'''
echo "run-scenario $*" >> "{log}"
if [ "$2" = "--print-proc" ]; then
  case "$1" in window-toggle) echo deskflow-core;; scroll-im-granted) echo mouser;; esac
  exit 0
fi
exit {scenario_rc}
''')
    _shim(bin_ / "fleet-soak", f'''
echo "fleet-soak $*" >> "{log}"
case "$*" in *"{soak_fail_for or '@@none@@'}"*) exit 1;; esac
exit 0
''')
    _shim(bin_ / "behavior-bench.sh", f'''
echo "behavior-bench $*" >> "{log}"
exit {bench_rc}
''')
    env = dict(os.environ)
    env.update({
        "CANARY_ROOT": str(root),
        "PATH": f"{bin_}:{env['PATH']}",
        "RUN_SCENARIO": "run-scenario.sh",
        "FLEET_SOAK": "fleet-soak",
        "BEHAVIOR_BENCH": "behavior-bench.sh",
        "RUNS_DIR": str(root / "harness" / "runs"),
    })
    return env, log


def _canary(env, *args):
    return subprocess.run(["bash", str(CANARY_SH), *args], capture_output=True, text=True, env=env)


def test_canary_iterates_full_rows_only_and_passes(tmp_path):
    env, log = _canary_env(tmp_path)
    r = _canary(env, "--seat", "hackintosh")
    assert r.returncode == 0, r.stdout + r.stderr
    calls = log.read_text().splitlines()
    assert "run-scenario window-toggle --seat hackintosh --iters 200" in calls
    assert "run-scenario scroll-im-granted --seat hackintosh --iters 200" in calls
    assert "run-scenario window-toggle --print-proc" in calls
    assert not any("lock-unlock" in c for c in calls)
    runs_dir = env["RUNS_DIR"]
    assert (f"fleet-soak report --accelerated --proc deskflow-core --in {runs_dir}/window-toggle.jsonl "
            "--window 1 --slope-max 5 --ports-slope-max 10 --max-restarts 0") in calls
    assert (f"fleet-soak report --accelerated --proc mouser --in {runs_dir}/scroll-im-granted.jsonl "
            "--window 1 --slope-max 5 --ports-slope-max 10 --max-restarts 0") in calls
    assert calls[-1] == "behavior-bench --all --seat hackintosh"
    assert "all green" in r.stdout
    assert "rows=2" in r.stdout


def test_canary_fails_on_soak_regression_but_still_runs_bench(tmp_path):
    env, log = _canary_env(tmp_path, soak_fail_for="--proc mouser")
    r = _canary(env, "--seat", "hackintosh", "--iters", "50")
    assert r.returncode == 1
    calls = log.read_text()
    assert "--iters 50" in calls
    assert "behavior-bench --all --seat hackintosh" in calls
    assert "scenario:scroll-im-granted" in r.stdout and "FAIL" in r.stdout
    assert "1 failure(s)" in r.stdout


def test_canary_fails_when_scenario_driver_fails(tmp_path):
    env, log = _canary_env(tmp_path, scenario_rc=3)
    r = _canary(env, "--seat", "hackintosh")
    assert r.returncode == 1
    assert "fleet-soak" not in log.read_text()  # no report without a run
    assert "2 failure(s)" in r.stdout


def test_canary_fails_when_bench_fails(tmp_path):
    env, _ = _canary_env(tmp_path, bench_rc=1)
    r = _canary(env, "--seat", "hackintosh")
    assert r.returncode == 1
    assert "behavior-bench" in r.stdout and "FAIL" in r.stdout


def test_canary_requires_seat(tmp_path):
    env, _ = _canary_env(tmp_path)
    r = _canary(env)
    assert r.returncode == 2
    assert "--seat" in r.stderr
