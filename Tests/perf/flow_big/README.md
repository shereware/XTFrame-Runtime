# Flow benchmark fixtures (T-403, T-475)

Docs/design/21-perf-tracing.md §21.6 (flow budget) and 27-interaction.md §27.4.2 (M37).

| Path | What |
|---|---|
| `gen_flow_big.py` | Writes `large/` (500 nodes, 100 sinks) and `typical/` (200 nodes, 40 sinks). Deterministic; re-running rewrites identical files. |
| `flow_bench.py` | Runs both fixtures and prints the per-frame table (average / p95 over the last `--keep` frames), or JSON with `--json`. |
| `large/`, `typical/` | The generated UI Projects (one UI each: `.xt`, `.xts`, `.xtflow`). |

## Two runners

- `--view <xtframe-view>`: `xtframe-view --project <fixture> --headless --perf --synth-pointer 16
  --synth-ports 20 --perf-csv <tmp>`. The full viewer (SW rendering, system fonts). The fixtures are
  compiled at behavior 1 (T-476: their `.xtui` `spec.behavior`), so value signals are coalesced
  per frame (43 §43.3). `allocs/tick` is always 0 here.
- `--runner <xt_perf_flow>`: `Tests/perf/flow_perf_main.cpp`, the same frame loop without a
  rendering backend or fonts. It replaces `operator new` (`flow_alloc_hook.cpp`,
  `XT_PERF_ALLOC_COUNT`), so `FrameStats::flowAllocs` holds the allocations of each
  `FlowRuntime::tick()`, the sink writes into the Document included (T-478; only a `setStyle`
  that goes in as text, i.e. the core's CSS parse, is left out). The fixture compiles at its
  `.xtui` behavior (1);
  `--behavior N` overrides it (`--behavior 0`: the uncoalesced path).
  `xt_perf_flow --plain` turns tracing off and times only the tick from outside, which shows what
  the stage timers cost.

Measure a Release build:

```
cmake --build --preset vs2022-editor-release --target xtframe-view xt_perf_flow
python Tests/perf/flow_big/flow_bench.py --view bin/win_x64/Release/xtframe-view.exe
python Tests/perf/flow_big/flow_bench.py --runner bin/win_x64/Release/tests/xt_perf_flow.exe --frames 2000 --keep 2000
```

## Columns

- `flow tick`: `FrameStats::flowMs`, the host's timer around `FlowRuntime::tick()`.
- `deliver`, `eval`, `sink`, `poll`: `flowDeliverMs`, `flowEvalMs`, `flowSinkMs`, `flowPollMs`.
  Exclusive times, taken only inside `tick()`: a nested stage stops the outer one, so an
  evaluation that writes a sink counts the write in `sink`, not in `eval`. `other` = the tick minus
  the four (clock / timer nodes, the `values()` sync, the tick's own frame).
- `binding`: `updateMs` minus its five timed phases, the upper bound of the port-binding
  re-evaluation (21 §21.6).
- Counters (`FrameStats::flowSignals`, `flowSignalsCoalesced`, `flowNodeEvals`, `flowSinkWrites`,
  `flowFanOutEdges`) count the whole frame, the input dispatch inside `Context::update()` included.
  They need `FlowRuntime::setPerfTracker()` (the BundleContext sets its Context's tracker by
  itself) and an enabled tracker.

The ctest entries `xt_perf_flow_big_large` / `_typical` (label `perf-report`) only print
xtframe-view's `--perf` summary; nothing here is a gate. `flow_bench.py --assert` (T-478, for the
release check) fails when a fixture's average `flow+binding` (flow tick + binding) is over the
21 §21.6 budget (typical 0.2 ms, large 0.8 ms) or, with `--runner`, when a measured tick allocated:

```
python Tests/perf/flow_big/flow_bench.py --runner bin/win_x64/Release/tests/xt_perf_flow.exe --frames 2000 --keep 1800 --assert
```
