#!/usr/bin/env python3
"""Behavior bench helper for harness/behavior-bench.sh.

Measures input-path latency for the fleet processes and compares it against a
per-seat baseline:

  deskflow-core  200 synthetic CGEvent mouse moves (CGEventCreateMouseEvent +
                 CGEventPost) injected on the server seat, each tagged with a
                 unique wall-clock ns timestamp in kCGEventSourceUserData, and
                 timestamped again when a listen-only CGEventTap observes them
                 on the client seat.
  mouser         200 synthetic scroll ticks (CGEventCreateScrollWheelEvent),
                 timing tick -> the event Mouser re-injects (an event whose
                 source pid is not ours) as seen by the tap.

5 runs x 200 events, the first run is dropped, and p50_ms / p95_ms / delivered
/ sent are computed over the remaining runs.

Baselines live in harness/baselines/behavior-<seat>.json, one object per proc
keyed by proc name, each `{proc, seat, runs, p50_ms, p95_ms, delivered, sent}`.
A comparison passes when p50 and p95 are within +/-10 % of the baseline AND
delivered == sent.

Only the python standard library is required. Quartz (PyObjC) is imported
lazily and only for the `bench` and `listener` subcommands that touch the real
event system; everything else (statistics, baseline write/compare, matching)
is pure python so it can be unit-tested without injecting input.

Exit codes: 0 pass, 1 regression, 2 invalid (no baseline, no Quartz, bad args).
"""

from __future__ import annotations

import argparse
import json
import math
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

PROCS = ("deskflow-core", "mouser")
DEFAULT_RUNS = 5
DEFAULT_COUNT = 200
TOLERANCE = 0.10
SCHEMA_KEYS = ("proc", "seat", "runs", "p50_ms", "p95_ms", "delivered", "sent")

HERE = Path(__file__).resolve().parent
DEFAULT_BASELINE_DIR = HERE / "baselines"

EXIT_PASS = 0
EXIT_FAIL = 1
EXIT_INVALID = 2


# --------------------------------------------------------------------------
# Statistics (pure)
# --------------------------------------------------------------------------


def percentile(values, pct):
    """Nearest-rank percentile. `pct` in [0, 100]. Empty input -> NaN."""
    if not values:
        return float("nan")
    ordered = sorted(values)
    rank = int(math.ceil(pct / 100.0 * len(ordered)))
    rank = min(max(rank, 1), len(ordered))
    return float(ordered[rank - 1])


def summarize(runs, proc, seat, drop_first=True):
    """Collapse per-run results into the baseline schema.

    `runs` is a list of dicts `{"sent": int, "latencies_ms": [float, ...]}`.
    The first run is a warm-up and is dropped (when there is more than one).
    `delivered` is the number of latency samples in the kept runs, `sent` the
    number of events injected in the kept runs, `runs` the number of kept runs.
    """
    kept = list(runs)
    if drop_first and len(kept) > 1:
        kept = kept[1:]
    latencies = [float(x) for run in kept for x in run.get("latencies_ms", [])]
    sent = sum(int(run.get("sent", 0)) for run in kept)
    return {
        "proc": proc,
        "seat": seat,
        "runs": len(kept),
        "p50_ms": round(percentile(latencies, 50), 3) if latencies else None,
        "p95_ms": round(percentile(latencies, 95), 3) if latencies else None,
        "delivered": len(latencies),
        "sent": sent,
    }


def within_tolerance(current, baseline, tol=TOLERANCE):
    """True when `current` is within +/-tol of `baseline` (inclusive)."""
    if current is None or baseline is None:
        return False
    if baseline == 0:
        return current == 0
    return abs(current - baseline) <= tol * abs(baseline) + 1e-9


def compare(result, baseline, tol=TOLERANCE):
    """Return a dict describing the comparison. `pass` is the verdict."""
    checks = {
        "p50_ms": within_tolerance(result.get("p50_ms"), baseline.get("p50_ms"), tol),
        "p95_ms": within_tolerance(result.get("p95_ms"), baseline.get("p95_ms"), tol),
        "delivered": result.get("delivered") == result.get("sent"),
    }
    return {"checks": checks, "pass": all(checks.values())}


# --------------------------------------------------------------------------
# Baseline files (pure)
# --------------------------------------------------------------------------


def baseline_path(seat, baseline_dir=None):
    base = Path(baseline_dir) if baseline_dir else DEFAULT_BASELINE_DIR
    return base / f"behavior-{seat}.json"


def load_baselines(seat, baseline_dir=None):
    path = baseline_path(seat, baseline_dir)
    if not path.exists():
        return {}
    with open(path, "r", encoding="utf-8") as fh:
        data = json.load(fh)
    if not isinstance(data, dict):
        raise ValueError(f"{path}: expected an object keyed by proc")
    return data


def write_baseline(result, baseline_dir=None):
    """Merge `result` into behavior-<seat>.json keyed by proc; return the path."""
    entry = {k: result[k] for k in SCHEMA_KEYS}
    path = baseline_path(entry["seat"], baseline_dir)
    path.parent.mkdir(parents=True, exist_ok=True)
    data = load_baselines(entry["seat"], baseline_dir)
    data[entry["proc"]] = entry
    tmp = path.with_suffix(".json.tmp")
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(data, fh, indent=2, sort_keys=True)
        fh.write("\n")
    os.replace(tmp, path)
    return path


# --------------------------------------------------------------------------
# Observation matching (pure)
# --------------------------------------------------------------------------


def match_observations(sent, observed, mode):
    """Pair injected events with tap observations; return latencies in ms.

    `sent` is a list of `{"tag": int, "t_ns": int}` in injection order.
    `observed` is a list of `{"tag": int, "pid": int, "t_ns": int}` in the
    order the tap saw them.

    mode "tag":     match on kCGEventSourceUserData (the tag survives on the
                    seat where the event was posted).
    mode "foreign": ordinal match against observations whose source pid is
                    not `self_pid` (events re-injected by another process,
                    e.g. Mouser or the Deskflow client). Because the injecting
                    process's pid is unknown to this pure function, callers
                    pre-filter `observed` down to foreign events.
    """
    latencies = []
    if mode == "tag":
        seen = {}
        for obs in observed:
            tag = obs.get("tag")
            if tag and tag not in seen:
                seen[tag] = obs["t_ns"]
        for ev in sent:
            t = seen.get(ev["tag"])
            if t is not None and t >= ev["t_ns"]:
                latencies.append((t - ev["t_ns"]) / 1e6)
    elif mode == "foreign":
        for ev, obs in zip(sent, observed):
            if obs["t_ns"] >= ev["t_ns"]:
                latencies.append((obs["t_ns"] - ev["t_ns"]) / 1e6)
    else:
        raise ValueError(f"unknown match mode {mode!r}")
    return latencies


# --------------------------------------------------------------------------
# Quartz-backed measurement (only used by `bench` / `listener`)
# --------------------------------------------------------------------------


def _quartz():
    try:
        import Quartz  # type: ignore
    except Exception as exc:  # pragma: no cover - depends on host
        print(f"behavior-bench: Quartz (PyObjC) is required for live measurement: {exc}",
              file=sys.stderr)
        raise SystemExit(EXIT_INVALID)
    return Quartz


def _event_mask(Quartz, proc):
    if proc == "deskflow-core":
        return Quartz.CGEventMaskBit(Quartz.kCGEventMouseMoved)
    return Quartz.CGEventMaskBit(Quartz.kCGEventScrollWheel)


def run_listener(proc, count, timeout, out=sys.stdout):
    """Listen-only CGEventTap; emit one JSON line per observed event.

    Emits `{"ready": true}` once the tap is armed, then
    `{"tag", "pid", "t_ns", "type"}` per event until `count` events have been
    seen or `timeout` seconds elapse, then `{"done": true, "observed": n}`.
    Never modifies or injects events.
    """
    Quartz = _quartz()
    seen = {"n": 0}

    def callback(_proxy, etype, event, _refcon):
        rec = {
            "tag": int(Quartz.CGEventGetIntegerValueField(event, Quartz.kCGEventSourceUserData)),
            "pid": int(Quartz.CGEventGetIntegerValueField(event, Quartz.kCGEventSourceUnixProcessID)),
            "t_ns": time.time_ns(),
            "type": int(etype),
        }
        seen["n"] += 1
        out.write(json.dumps(rec) + "\n")
        out.flush()
        return event

    tap = Quartz.CGEventTapCreate(
        Quartz.kCGSessionEventTap,
        Quartz.kCGHeadInsertEventTap,
        Quartz.kCGEventTapOptionListenOnly,
        _event_mask(Quartz, proc),
        callback,
        None,
    )
    if tap is None:
        print("behavior-bench: CGEventTapCreate failed (Accessibility/Input Monitoring?)", file=sys.stderr)
        raise SystemExit(EXIT_INVALID)
    source = Quartz.CFMachPortCreateRunLoopSource(None, tap, 0)
    loop = Quartz.CFRunLoopGetCurrent()
    Quartz.CFRunLoopAddSource(loop, source, Quartz.kCFRunLoopCommonModes)
    Quartz.CGEventTapEnable(tap, True)
    out.write(json.dumps({"ready": True}) + "\n")
    out.flush()
    deadline = time.monotonic() + timeout
    while seen["n"] < count and time.monotonic() < deadline:
        Quartz.CFRunLoopRunInMode(Quartz.kCFRunLoopDefaultMode, 0.05, False)
    Quartz.CGEventTapEnable(tap, False)
    out.write(json.dumps({"done": True, "observed": seen["n"]}) + "\n")
    out.flush()


def _inject_events(proc, count, interval_s):
    """Post `count` synthetic events; return the `sent` list for matching."""
    Quartz = _quartz()
    sent = []
    if proc == "deskflow-core":
        probe = Quartz.CGEventCreate(None)
        origin = Quartz.CGEventGetLocation(probe)
        ox, oy = float(origin.x), float(origin.y)
    for i in range(count):
        tag = time.time_ns()
        if proc == "deskflow-core":
            # Jitter within a 4 px box so the pointer never leaves the screen.
            x = ox + (i % 4)
            y = oy + ((i // 4) % 4)
            ev = Quartz.CGEventCreateMouseEvent(None, Quartz.kCGEventMouseMoved, (x, y), 0)
        else:
            # One line up on alternating ticks so scroll position stays put.
            delta = 1 if i % 2 == 0 else -1
            ev = Quartz.CGEventCreateScrollWheelEvent(None, Quartz.kCGScrollEventUnitLine, 1, delta)
        Quartz.CGEventSetIntegerValueField(ev, Quartz.kCGEventSourceUserData, tag)
        Quartz.CGEventPost(Quartz.kCGHIDEventTap, ev)
        sent.append({"tag": tag, "t_ns": tag})
        time.sleep(interval_s)
    return sent


def _listener_cmd(args, count, timeout):
    script = str(Path(__file__).resolve())
    local_cmd = [sys.executable, script, "listener", "--proc", args.proc,
                 "--count", str(count), "--timeout", str(timeout)]
    if args.local or args.seat in (socket.gethostname().split(".")[0], "local"):
        return local_cmd
    # Remote client seat: the tap must run in that seat's GUI session. The
    # remote checkout is expected at the same relative path; override with
    # BEHAVIOR_BENCH_REMOTE_SH.
    remote_sh = os.environ.get("BEHAVIOR_BENCH_REMOTE_SH", "harness/behavior-bench.sh")
    return ["ssh", args.seat, remote_sh, "--listener", "--proc", args.proc,
            "--count", str(count), "--timeout", str(timeout)]


def _read_listener(proc_handle, deadline):
    """Read JSON lines until `done` or the deadline; return observations."""
    observed = []
    while time.monotonic() < deadline:
        line = proc_handle.stdout.readline()
        if not line:
            break
        try:
            rec = json.loads(line)
        except json.JSONDecodeError:
            continue
        if rec.get("done"):
            break
        if "t_ns" in rec:
            observed.append(rec)
    return observed


def measure_run(args, count):
    """One run: start listener, inject `count` events, collect + match."""
    timeout = args.timeout
    cmd = _listener_cmd(args, count, timeout)
    with subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True) as lp:
        ready = lp.stdout.readline()
        if "ready" not in ready:
            lp.kill()
            print(f"behavior-bench: listener did not become ready ({cmd[0]})", file=sys.stderr)
            raise SystemExit(EXIT_INVALID)
        sent = _inject_events(args.proc, count, args.interval_ms / 1000.0)
        observed = _read_listener(lp, time.monotonic() + timeout)
        try:
            lp.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            lp.kill()
    me = os.getpid()
    if args.proc == "mouser":
        foreign = [o for o in observed if o.get("pid") != me]
        latencies = match_observations(sent, foreign, "foreign")
    else:
        latencies = match_observations(sent, observed, "tag")
        if not latencies and observed:
            # Client seat: userData does not survive the wire; fall back to
            # ordinal matching against the client's injected events.
            foreign = [o for o in observed if o.get("pid") != me]
            latencies = match_observations(sent, foreign, "foreign")
    return {"sent": count, "latencies_ms": latencies}


# --------------------------------------------------------------------------
# Reporting / evaluation
# --------------------------------------------------------------------------


def _fmt(v):
    if v is None:
        return "-"
    if isinstance(v, float):
        return f"{v:.3f}"
    return str(v)


def render_table(rows):
    """rows: list of (result, baseline|None, comparison|None). Returns str."""
    header = ("proc", "seat", "runs", "p50_ms", "p95_ms", "delivered", "sent",
              "base_p50", "base_p95", "result")
    lines = [" ".join(f"{h:>10}" for h in header)]
    for result, base, cmp_ in rows:
        if cmp_ is None:
            verdict = "BASELINE"
        else:
            verdict = "PASS" if cmp_["pass"] else "FAIL"
        cells = [result["proc"], result["seat"], result["runs"], result["p50_ms"],
                 result["p95_ms"], result["delivered"], result["sent"],
                 base.get("p50_ms") if base else None,
                 base.get("p95_ms") if base else None, verdict]
        lines.append(" ".join(f"{_fmt(c):>10}" for c in cells))
    return "\n".join(lines)


def evaluate(result, baseline_mode, baseline_dir=None):
    """Write or compare; return (baseline, comparison, exit_code)."""
    if baseline_mode:
        write_baseline(result, baseline_dir)
        return None, None, EXIT_PASS
    base = load_baselines(result["seat"], baseline_dir).get(result["proc"])
    if base is None:
        return None, None, EXIT_INVALID
    cmp_ = compare(result, base)
    return base, cmp_, (EXIT_PASS if cmp_["pass"] else EXIT_FAIL)


def emit(rows, codes, as_json):
    if as_json:
        payload = []
        for (result, base, cmp_), code in zip(rows, codes):
            item = dict(result)
            item["baseline"] = base
            item["comparison"] = cmp_
            item["exit"] = code
            payload.append(item)
        print(json.dumps(payload if len(payload) != 1 else payload[0], indent=2))
    else:
        print(render_table(rows))
        for (result, base, _), code in zip(rows, codes):
            if code == EXIT_INVALID and base is None:
                print(f"no baseline for {result['proc']} on {result['seat']} "
                      f"(run with --baseline first)", file=sys.stderr)


def _combine_codes(codes):
    if any(c == EXIT_INVALID for c in codes):
        return EXIT_INVALID
    if any(c == EXIT_FAIL for c in codes):
        return EXIT_FAIL
    return EXIT_PASS


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------


def cmd_bench(args):
    _quartz()  # fail fast with a clear message before spawning listeners
    procs = list(PROCS) if args.all else [args.proc]
    rows, codes = [], []
    for proc in procs:
        args.proc = proc
        runs = [measure_run(args, args.count) for _ in range(args.runs)]
        result = summarize(runs, proc, args.seat)
        base, cmp_, code = evaluate(result, args.baseline, args.baseline_dir)
        rows.append((result, base, cmp_))
        codes.append(code)
    emit(rows, codes, args.json)
    return _combine_codes(codes)


def cmd_evaluate(args):
    """Evaluate pre-collected runs (JSON) without touching the event system."""
    # Runs file is either a bare list of runs (single proc) or an object
    # keyed by proc name, each value a list of runs.
    with open(args.runs_file, "r", encoding="utf-8") as fh:
        data = json.load(fh)
    if isinstance(data, list):
        data = {args.proc: data}
    rows, codes = [], []
    procs = list(PROCS) if args.all else [args.proc]
    for proc in procs:
        runs = data.get(proc)
        if runs is None:
            print(f"no runs for {proc} in {args.runs_file}", file=sys.stderr)
            codes.append(EXIT_INVALID)
            rows.append((summarize([], proc, args.seat), None, None))
            continue
        result = summarize(runs, proc, args.seat)
        base, cmp_, code = evaluate(result, args.baseline, args.baseline_dir)
        rows.append((result, base, cmp_))
        codes.append(code)
    emit(rows, codes, args.json)
    return _combine_codes(codes)


def cmd_listener(args):
    run_listener(args.proc, args.count, args.timeout)
    return EXIT_PASS


def build_parser():
    p = argparse.ArgumentParser(prog="behavior-bench.py", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    def common(sp):
        sp.add_argument("--proc", choices=PROCS, default="deskflow-core")
        sp.add_argument("--seat", required=True)
        sp.add_argument("--all", action="store_true", help="run both procs")
        sp.add_argument("--baseline", action="store_true", help="write the baseline instead of comparing")
        sp.add_argument("--json", action="store_true")
        sp.add_argument("--baseline-dir", default=None)

    b = sub.add_parser("bench", help="live measurement (needs Quartz)")
    common(b)
    b.add_argument("--runs", type=int, default=DEFAULT_RUNS)
    b.add_argument("--count", type=int, default=DEFAULT_COUNT)
    b.add_argument("--interval-ms", type=float, default=4.0)
    b.add_argument("--timeout", type=float, default=5.0, help="listener drain timeout (s)")
    b.add_argument("--local", action="store_true", help="listener runs on this seat (no ssh)")
    b.set_defaults(fn=cmd_bench)

    e = sub.add_parser("evaluate", help="summarize/compare pre-collected runs JSON")
    common(e)
    e.add_argument("--runs-file", required=True)
    e.set_defaults(fn=cmd_evaluate)

    l = sub.add_parser("listener", help="listen-only CGEventTap; JSON lines on stdout")
    l.add_argument("--proc", choices=PROCS, default="deskflow-core")
    l.add_argument("--count", type=int, default=DEFAULT_COUNT)
    l.add_argument("--timeout", type=float, default=5.0)
    l.set_defaults(fn=cmd_listener)
    return p


def main(argv=None):
    args = build_parser().parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
