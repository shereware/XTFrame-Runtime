"""Run the flow benchmark and print the per-frame table (Docs/tasks/T-403.md item 2, T-475 item 3).

For each fixture (typical = 200 nodes, large = 500 nodes) this runs either

  xtframe-view --project <fixture> --headless --perf --frames 600 --synth-pointer 16
               --synth-ports 20 --perf-csv <tmp>                                   (--view)
  xt_perf_flow --fixture <fixture> --frames 600 --synth-pointer 16 --synth-ports 20
               [--behavior <n>] --csv <tmp>                                        (--runner)

and reports, over the last --keep frames, the average and the 95th percentile of:

  flow tick   FrameStats::flowMs (FlowRuntime::tick, outside Context::update)
  deliver     flowDeliverMs: delivering the coalesced value signals (T-475; exclusive time)
  eval        flowEvalMs: settleNodes() / evaluateNode() (exclusive)
  sink        flowSinkMs: applyDocumentInput(), the writes into the Document (exclusive)
  poll        flowPollMs: pollDocumentOutputs(), the polled `#document.*` / `localTime` ports
  other       flow tick minus the four stages (clock / timer nodes, values() sync, the tick frame)
  binding     updateMs - (input + style + layout + animate + transform): the untimed part of
              Document::update, where the held-back port bindings are re-evaluated
              (BindingRuntime::flushDirty / tick, 27 §27.4.1 (b)); an upper bound, it also holds
              the continuous-trigger flush and the event tick
  flow+binding  flow tick + binding: what the 21 §21.6 budget is stated in (T-478)
  total       updateMs + flowMs (everything a host's update step costs, rendering excluded)
  setStyle    FrameStats::setStyleCalls
  signals, coalesced, evals, sink writes, fan-out edges, sink typed / text, allocs
              FrameStats::flowSignals / flowSignalsCoalesced / flowNodeEvals / flowSinkWrites /
              flowFanOutEdges / flowSinkTyped / flowSinkText (T-477: the `set(port)` and
              `setStyle` writes that went in typed vs as text) / flowAllocs per frame
              (flowAllocs is filled by xt_perf_flow only; xtframe-view reports 0)

--assert (T-478; for the release check, not a CTest gate) fails (exit 1) when a fixture's average
flow+binding exceeds the 21 §21.6 budget (typical <= 0.2 ms, large <= 0.8 ms), or, with --runner,
when a measured frame allocated inside the flow tick (27 §27.4.2 item 5; keep the warm-up frames
out with --keep).

The stage times are only taken inside FlowRuntime::tick(); the counters also count what the
input dispatch inside Context::update() reports. See Tests/perf/flow_big/README.md.

Python 3 standard library only. Usage:
  python3 Tests/perf/flow_big/flow_bench.py --view <xtframe-view> [--frames 600] [--keep 480]
  python3 Tests/perf/flow_big/flow_bench.py --runner <xt_perf_flow> [--behavior N] [--json] [--assert]
"""
import argparse
import csv
import json
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
FIXTURES = [("typical (200)", HERE / "typical"), ("large (500)", HERE / "large")]
# T-478 (21 §21.6, M37): average flow+binding ms per frame.
BUDGETS = {"typical (200)": 0.2, "large (500)": 0.8}
STAGES = ["flowDeliverMs", "flowEvalMs", "flowSinkMs", "flowPollMs"]
PHASES = ["inputMs", "styleMs", "layoutMs", "animateMs", "transformMs"]
# (label, how to read it from one CSV row, is a count)
COLUMNS = [
    ("flow tick ms", lambda r: r["flowMs"], False),
    ("deliver ms", lambda r: r["flowDeliverMs"], False),
    ("eval ms", lambda r: r["flowEvalMs"], False),
    ("sink ms", lambda r: r["flowSinkMs"], False),
    ("poll ms", lambda r: r["flowPollMs"], False),
    ("other ms", lambda r: max(0.0, r["flowMs"] - sum(r[k] for k in STAGES)), False),
    ("binding ms", lambda r: max(0.0, r["updateMs"] - sum(r[k] for k in PHASES)), False),
    ("flow+binding ms", lambda r: r["flowMs"] + max(0.0, r["updateMs"] - sum(r[k] for k in PHASES)), False),
    ("total update ms", lambda r: r["updateMs"] + r["flowMs"], False),
    ("input ms", lambda r: r["inputMs"], False),
    ("setStyle/frame", lambda r: r["setStyleCalls"], True),
    ("signals/frame", lambda r: r["flowSignals"], True),
    ("coalesced/frame", lambda r: r["flowSignalsCoalesced"], True),
    ("evals/frame", lambda r: r["flowNodeEvals"], True),
    ("sink writes/frame", lambda r: r["flowSinkWrites"], True),
    ("fan-out edges/frame", lambda r: r["flowFanOutEdges"], True),
    ("sink typed/frame", lambda r: r.get("flowSinkTyped", 0.0), True),
    ("sink text/frame", lambda r: r.get("flowSinkText", 0.0), True),
    ("allocs/tick", lambda r: r["flowAllocs"], True),
]


def percentile(values, p):
    ordered = sorted(values)
    if not ordered:
        return 0.0
    k = (len(ordered) - 1) * p
    lo = int(k)
    hi = min(lo + 1, len(ordered) - 1)
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (k - lo)


def run(args, fixture, tmp):
    out = Path(tmp) / (fixture.name + ".csv")
    common = ["--frames", str(args.frames), "--synth-pointer", "16", "--synth-ports", "20"]
    if args.runner:
        behavior = [] if args.behavior is None else ["--behavior", str(args.behavior)]
        cmd = [str(Path(args.runner).resolve()), "--fixture", str(fixture)] + behavior + ["--csv", str(out)] + common
    else:
        cmd = [str(Path(args.view).resolve()), "--project", str(fixture), "--headless", "--perf",
               "--perf-csv", str(out)] + common
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout + proc.stderr)
        raise SystemExit(f"{cmd[0]} failed on {fixture} (exit {proc.returncode})")
    for line in proc.stderr.splitlines():  # The pack / load notes are not interesting.
        if "warning" in line or "error" in line:
            sys.stderr.write(line + chr(10))
    with open(out, newline="") as f:
        rows = [{k: float(v) for k, v in r.items()} for r in csv.DictReader(f)]
    return rows


def measure(rows, keep):
    rows = rows[-keep:]
    stats = {}
    for label, read, _ in COLUMNS:
        values = [read(r) for r in rows]
        stats[label] = (sum(values) / len(values), percentile(values, 0.95))
    stats["max allocs/tick"] = max((r["flowAllocs"] for r in rows), default=0.0)
    return stats, len(rows)


def check_budgets(args, report):
    """T-478: the 21 §21.6 budget, and with --runner zero allocations per measured tick."""
    failures = []
    out = sys.stderr if args.json else sys.stdout  # --json keeps stdout one JSON object.
    for label, (stats, _) in report.items():
        avg = stats["flow+binding ms"][0]
        budget = BUDGETS[label]
        verdict = "ok" if avg <= budget else "OVER"
        print(f"budget {label}: flow+binding {avg:.3f} ms <= {budget} ms: {verdict}", file=out)
        if avg > budget:
            failures.append(f"{label}: flow+binding {avg:.3f} ms > {budget} ms")
        if args.runner:
            worst = stats["max allocs/tick"]
            print(f"allocs {label}: max {worst:.0f} per tick: {'ok' if worst == 0 else 'FAIL'}", file=out)
            if worst != 0:
                failures.append(f"{label}: a measured tick allocated {worst:.0f} times")
    for f in failures:
        sys.stderr.write("flow_bench --assert: " + f + chr(10))
    return not failures


def main():
    ap = argparse.ArgumentParser()
    who = ap.add_mutually_exclusive_group(required=True)
    who.add_argument("--view", help="path to the xtframe-view executable")
    who.add_argument("--runner", help="path to the xt_perf_flow executable (also reports allocs/tick)")
    ap.add_argument("--frames", type=int, default=600)
    ap.add_argument("--keep", type=int, default=480, help="frames measured (the last N)")
    ap.add_argument("--repeat", type=int, default=1, help="runs per fixture; the median-total run is kept")
    ap.add_argument("--behavior", type=int, default=None,
                    help="--runner only: the compile behavior version (default: the fixture's .xtui, 1)")
    ap.add_argument("--json", action="store_true", help="print one JSON object instead of the table")
    ap.add_argument("--assert", dest="assert_budget", action="store_true",
                    help="exit 1 when the 21 §21.6 budget (or, with --runner, zero allocs/tick) is not met")
    args = ap.parse_args()
    report = {}
    with tempfile.TemporaryDirectory() as tmp:
        for label, fixture in FIXTURES:
            results = []
            for _ in range(max(1, args.repeat)):
                results.append(measure(run(args, fixture, tmp), args.keep))
            results.sort(key=lambda s: s[0]["total update ms"][0])
            report[label] = results[len(results) // 2]
    if args.json:
        out = {label: {"frames": n, **{c: {"avg": stats[c][0], "p95": stats[c][1]} for c, _, _ in COLUMNS}}
               for label, (stats, n) in report.items()}
        print(json.dumps(out, indent=2))
        if args.assert_budget and not check_budgets(args, report):
            raise SystemExit(1)
        return
    print("| fixture | " + " | ".join(f"{c} avg / p95" for c, _, _ in COLUMNS) + " |")
    print("|---" * (len(COLUMNS) + 1) + "|")
    for label, (stats, n) in report.items():
        cells = []
        for c, _, is_count in COLUMNS:
            avg, p95 = stats[c]
            cells.append(f"{avg:.1f} / {p95:.0f}" if is_count else f"{avg:.3f} / {p95:.3f}")
        print(f"| {label} | " + " | ".join(cells) + f" |  ({n} frames)")
    if args.assert_budget and not check_budgets(args, report):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
