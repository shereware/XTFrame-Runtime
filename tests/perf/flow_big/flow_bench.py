"""Run the T-403 flow benchmark and print the per-frame table (docs/tasks/T-403.md item 2).

For each fixture (typical = 200 nodes, large = 500 nodes) this runs

  xtframe-view --project <fixture> --headless --perf --frames 600 --synth-pointer 16
               --synth-ports 20 --perf-csv <tmp>

and reports, over the last 480 frames, the average and the 95th percentile of:

  flow tick   FrameStats::flowMs (FlowRuntime::tick, outside Context::update)
  binding     updateMs - (input + style + layout + animate + transform): the untimed part of
              Document::update, where the held-back port bindings are re-evaluated
              (BindingRuntime::flushDirty / tick, 27 §27.4.1 (b)); an upper bound, it also holds
              the continuous-trigger flush and the event tick
  setStyle    FrameStats::setStyleCalls
  total       updateMs + flowMs (everything a host's update step costs, rendering excluded)

Python 3 standard library only. Usage:
  python3 tests/perf/flow_big/flow_bench.py --view <path to xtframe-view> [--frames 600]
"""
import argparse
import csv
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
FIXTURES = [("typical (200)", HERE / "typical"), ("large (500)", HERE / "large")]
COLUMNS = ["flow tick ms", "binding ms", "setStyle/frame", "total update ms", "input ms"]


def percentile(values, p):
    ordered = sorted(values)
    if not ordered:
        return 0.0
    k = (len(ordered) - 1) * p
    lo = int(k)
    hi = min(lo + 1, len(ordered) - 1)
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (k - lo)


def run(view, fixture, frames, tmp):
    out = Path(tmp) / (fixture.name + ".csv")
    cmd = [str(Path(view).resolve()), "--project", str(fixture), "--headless", "--perf", "--frames", str(frames),
           "--synth-pointer", "16", "--synth-ports", "20", "--perf-csv", str(out)]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout + proc.stderr)
        raise SystemExit(f"xtframe-view failed on {fixture} (exit {proc.returncode})")
    for line in proc.stderr.splitlines():  # The pack / load notes are not interesting.
        if "warning" in line or "error" in line:
            sys.stderr.write(line + chr(10))
    with open(out, newline="") as f:
        rows = list(csv.DictReader(f))
    return rows


def measure(rows, keep):
    rows = rows[-keep:]
    series = {name: [] for name in COLUMNS}
    for r in rows:
        upd = float(r["updateMs"])
        phases = sum(float(r[k]) for k in ("inputMs", "styleMs", "layoutMs", "animateMs", "transformMs"))
        flow = float(r["flowMs"])
        series["flow tick ms"].append(flow)
        series["binding ms"].append(max(0.0, upd - phases))
        series["setStyle/frame"].append(float(r["setStyleCalls"]))
        series["total update ms"].append(upd + flow)
        series["input ms"].append(float(r["inputMs"]))
    return {name: (sum(v) / len(v), percentile(v, 0.95)) for name, v in series.items()}, len(rows)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--view", required=True, help="path to the xtframe-view executable")
    ap.add_argument("--frames", type=int, default=600)
    ap.add_argument("--keep", type=int, default=480, help="frames measured (the last N)")
    ap.add_argument("--repeat", type=int, default=1, help="runs per fixture; the median-total run is kept")
    args = ap.parse_args()
    print(f"| fixture | " + " | ".join(f"{c} avg / p95" for c in COLUMNS) + " |")
    print("|---" * (len(COLUMNS) + 1) + "|")
    with tempfile.TemporaryDirectory() as tmp:
        for label, fixture in FIXTURES:
            results = []
            for _ in range(max(1, args.repeat)):
                stats, n = measure(run(args.view, fixture, args.frames, tmp), args.keep)
                results.append(stats)
            results.sort(key=lambda s: s["total update ms"][0])
            stats = results[len(results) // 2]
            cells = []
            for c in COLUMNS:
                avg, p95 = stats[c]
                cells.append(f"{avg:.1f} / {p95:.0f}" if c == "setStyle/frame" else f"{avg:.3f} / {p95:.3f}")
            print(f"| {label} | " + " | ".join(cells) + f" |  ({n} frames)")


if __name__ == "__main__":
    main()
