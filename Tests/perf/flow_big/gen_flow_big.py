"""Generate the T-403 large-flow benchmark fixtures (Docs/tasks/T-403.md item 1).

Writes two UI Projects next to this script, each a single UI (`.xt` + `.xts` + `.xtflow`):

  large/    500 flow nodes, 100 sinks  (the "large graph" budget, 21 "flow budget")
  typical/  200 flow nodes,  40 sinks  (the "typical" budget)

Both read the same 50 value-type source signals:
  8 document signals   #document.pointerX/Y/NX/NY, pointerDown, viewportWidth/Height, clock
  20 document ports    src0..src19 ("number out"; xtframe-view --synth-ports 20 writes them
                       every frame, a flow reads only `out` ports)
  22 hover outputs     #hv0..#hv21.hover (cells on the viewport diagonal, which
                       xtframe-view --synth-pointer sweeps)
25 of them change every frame (pointer x/y/nx/ny, clock, the 20 ports); the hovers and
pointerDown change now and then, the viewport never.

Nodes are math / format / compare / select / lookup in layers 1..8 (depth <= 8): layers 1..7
are a seeded random mix, layer 8 holds one node per sink -- a `format` for each setStyle and
setText sink, a `math clamp` (0..100) for each set(port) sink, which writes a `number in` port
`inN` whose `bind:width` the binding runtime re-evaluates. The 20 source ports are bound too
(`bind:height`), so every frame also has 20 host-driven binding re-evaluations.

T-552 (27 §27.4.4 / §27.4.5, spec review 7.7): three of the nodes are the event-driven ones the
zero-allocation test and `flow_bench.py --assert` must also cover -- a chain that feeds the last
set(port) sink: `#document.pointerNX -> filter (deadband) -> throttle (frames 3, trailing)`, whose
`passed` pulse drives a two-state `state` node (`tick`: idle <-> busy); the state's `index` is that
sink's clamp input and its `state` string feeds the last setText sink's format. pointerNX changes
every frame under --synth-pointer, so the throttle holds a trailing value two frames out of three
and flushes it at the end of each window (the pending-trailing path of `tick()`).

Deterministic: a fixed seed per fixture and sorted output; re-running rewrites identical files.
Python 3 standard library only. Run: python3 Tests/perf/flow_big/gen_flow_big.py
"""
import json
import random
from pathlib import Path

HERE = Path(__file__).resolve().parent
WIDTH, HEIGHT = 1280, 720
LAYERS = 8  # Layer 8 is the sink layer.

DOC_NUMBER_SOURCES = ["pointerX", "pointerY", "pointerNX", "pointerNY", "viewportWidth",
                      "viewportHeight", "clock"]
DOC_BOOL_SOURCES = ["pointerDown"]
SOURCE_PORTS = 20
HOVER_CELLS = 22

MATH_OPS = ["add", "sub", "min", "max", "clamp", "map", "abs", "round"]
COMPARE_OPS = ["eq", "ne", "lt", "le", "gt", "ge"]
# (weight, type) of the layer 1..7 mix.
NODE_MIX = [(35, "math"), (20, "format"), (15, "compare"), (15, "select"), (15, "lookup")]

STYLE_TEMPLATES = [
    "transform: translate({a}px, {b}px)",
    "opacity: {a}",
    "transform: rotate({a}deg)",
    "left: {a}px; top: {b}px",
    "width: {a}px",
]
TEXT_TEMPLATES = ["{a}", "v {a} / {b}", "{a} | {b} | {c}"]
LOOKUP_STRINGS = ["idle", "walk", "run", "jump", "fall", "hit", "dash", "rest"]
LOOKUP_NUMBERS = [0, 10, 20, 35, 50, 65, 80, 100]

FIXTURES = [
    # dir, ui id, project name, seed, total nodes, setStyle / setText / set(port) sinks
    ("large", "flow-big", "Flow Big Large", 5000, 500, (40, 30, 30)),
    ("typical", "flow-typical", "Flow Big Typical", 2000, 200, (16, 12, 12)),
]


class Graph:
    """One fixture's flow graph plus the element/port lists the UI needs."""

    def __init__(self, seed):
        self.rng = random.Random(seed)
        self.nodes = []
        self.edges = []
        # Output pools by type: list of (endpoint, layer).
        self.pools = {"num": [], "bool": [], "str": []}
        # Sources not yet read by any node, by type (each must be read at least once).
        self.unused = {"num": [], "bool": []}
        self.count = 0

    def add_source(self, endpoint, kind):
        self.pools[kind].append((endpoint, 0))
        self.unused[kind].append(endpoint)

    def node(self, type_, layer, params):
        self.count += 1
        node_id = f"n{self.count:03d}_{type_}"
        self.nodes.append({"id": node_id, "type": type_, "params": params})
        return node_id

    def edge(self, src, dst):
        self.edges.append({"from": src, "to": dst})

    def pick(self, kind, layer, need_previous=False):
        """An output of `kind` from a layer below `layer`; unread sources first, then (when
        `need_previous`) the layer just below, else anything below."""
        if self.unused.get(kind):
            return self.unused[kind].pop(0)
        pool = [ep for ep, l in self.pools[kind] if l < layer]
        if need_previous:
            prev = [ep for ep, l in self.pools[kind] if l == layer - 1]
            if prev:
                return self.rng.choice(prev)
        return self.rng.choice(pool)

    def pick_typed_node_output(self, kind, layer):
        """Like pick(), but never a declared document port (`#document.srcN`)."""
        pool = [ep for ep, l in self.pools[kind] if l < layer and not ep.startswith("#document.src")]
        return self.rng.choice(pool)

    def pick_any(self, layer, need_previous=False):
        kind = self.rng.choices(["num", "bool", "str"], weights=[60, 20, 20])[0]
        if not [ep for ep, l in self.pools[kind] if l < layer]:
            kind = "num"
        return self.pick(kind, layer, need_previous)


def math_params(rng, op=None):
    op = op or rng.choice(MATH_OPS)
    params = {"op": op}
    if op == "clamp":
        params.update({"lo": 0, "hi": rng.choice([100, 500, 1000])})
    elif op == "map":
        params.update({"inLo": 0, "inHi": WIDTH, "outLo": 0, "outHi": rng.choice([1, 100, 360])})
    return params


def build_general_node(g, layer, type_):
    rng = g.rng
    if type_ == "math":
        nid = g.node("math", layer, math_params(rng))
        g.edge(g.pick("num", layer, True), f"{nid}.a")
        g.edge(g.pick("num", layer), f"{nid}.b")
        g.pools["num"].append((f"{nid}.out", layer))
    elif type_ == "compare":
        nid = g.node("compare", layer, {"op": rng.choice(COMPARE_OPS)})
        g.edge(g.pick("num", layer, True), f"{nid}.a")
        g.edge(g.pick("num", layer), f"{nid}.b")
        g.pools["bool"].append((f"{nid}.out", layer))
    elif type_ == "select":
        nid = g.node("select", layer, {})
        g.edge(g.pick("bool", layer, True), f"{nid}.cond")
        # The type inference of `select` does not see declared ports, so a / b never read one.
        for port in ("a", "b"):
            g.edge(g.pick_typed_node_output("num", layer), f"{nid}.{port}")
        # Its output type is inferred, which a typed `math` input does not accept: only the
        # flexible `format` inputs read it.
        g.pools["str"].append((f"{nid}.out", layer))
    elif type_ == "lookup":
        strings = rng.random() < 0.5
        values = LOOKUP_STRINGS if strings else LOOKUP_NUMBERS
        nid = g.node("lookup", layer, {"values": list(values)})
        g.edge(g.pick("num", layer, True), f"{nid}.index")
        g.pools["str" if strings else "num"].append((f"{nid}.out", layer))
    elif type_ == "format":
        nid = g.node("format", layer, {"format": "{a}:{b}", "decimals": 1})
        g.edge(g.pick_any(layer, True), f"{nid}.a")
        g.edge(g.pick_any(layer), f"{nid}.b")
        g.pools["str"].append((f"{nid}.out", layer))
    else:
        raise ValueError(type_)


CONTROL_NODES = 3  # T-552: filter, throttle, state (see the module docstring)


def build_control_chain(g):
    """T-552: pointerNX -> filter -> throttle -> state; returns the state node's id."""
    filt = g.node("filter", 1, {"op": "deadband", "threshold": 0.01})
    g.edge("#document.pointerNX", f"{filt}.in")
    thr = g.node("throttle", 2, {"unit": "frames", "rate": 3, "trailing": True})
    g.edge(f"{filt}.out", f"{thr}.in")
    g.pools["num"].append((f"{thr}.out", 2))
    state = g.node("state", 3, {
        "states": ["idle", "busy"],
        "initial": "idle",
        "transitions": [{"from": "idle", "on": "tick", "to": "busy"},
                        {"from": "busy", "on": "tick", "to": "idle"}],
    })
    g.edge(f"{thr}.passed", f"{state}.tick")
    return state


def build_graph(seed, total_nodes, sinks):
    n_style, n_text, n_port = sinks
    n_sinks = n_style + n_text + n_port
    g = Graph(seed)
    for name in DOC_NUMBER_SOURCES:
        g.add_source(f"#document.{name}", "num")
    for i in range(SOURCE_PORTS):
        g.add_source(f"#document.src{i}", "num")
    for name in DOC_BOOL_SOURCES:
        g.add_source(f"#document.{name}", "bool")
    for i in range(HOVER_CELLS):
        g.add_source(f"#hv{i}.hover", "bool")

    general = total_nodes - n_sinks - CONTROL_NODES
    weights = [w for w, _ in NODE_MIX]
    kinds = [k for _, k in NODE_MIX]
    for layer in range(1, LAYERS):
        per_layer = general // (LAYERS - 1) + (1 if layer <= general % (LAYERS - 1) else 0)
        for _ in range(per_layer):
            type_ = g.rng.choices(kinds, weights=weights)[0]
            # A select needs a bool below it; layer 1 always has the hover / pointerDown sources.
            build_general_node(g, layer, type_)
    assert not g.unused["num"] and not g.unused["bool"], g.unused
    control = build_control_chain(g)

    # Layer 8: one node per sink, reading the layer-7 outputs (and anything below).
    style_sinks, text_sinks, port_sinks = [], [], []
    for i in range(n_style):
        template = STYLE_TEMPLATES[i % len(STYLE_TEMPLATES)]
        nid = g.node("format", LAYERS, {"format": template, "decimals": 1})
        g.edge(g.pick("num", LAYERS, True), f"{nid}.a")
        if "{b}" in template:
            g.edge(g.pick("num", LAYERS), f"{nid}.b")
        g.edge(f"{nid}.out", f"#st{i}.setStyle")
        style_sinks.append(f"st{i}")
    for i in range(n_text):
        template = TEXT_TEMPLATES[i % len(TEXT_TEMPLATES)]
        nid = g.node("format", LAYERS, {"format": template, "decimals": 1})
        # T-552: the last setText shows the control chain's state name.
        g.edge(f"{control}.state" if i == n_text - 1 else g.pick_any(LAYERS, True), f"{nid}.a")
        for port in ("b", "c"):
            if "{" + port + "}" in template:
                g.edge(g.pick_any(LAYERS), f"{nid}.{port}")
        g.edge(f"{nid}.out", f"#tx{i}.setText")
        text_sinks.append(f"tx{i}")
    for i in range(n_port):
        nid = g.node("math", LAYERS, {"op": "clamp", "lo": 0, "hi": 100})
        # T-552: the last set(port) sink is driven by the control chain's state index.
        g.edge(f"{control}.index" if i == n_port - 1 else g.pick("num", LAYERS, True), f"{nid}.a")
        g.edge(f"{nid}.out", f"#document.in{i}")
        port_sinks.append(f"in{i}")
    assert len(g.nodes) == total_nodes, len(g.nodes)
    return g, style_sinks, text_sinks, port_sinks


def write_xt(path, ui_id, title, style_sinks, text_sinks, port_sinks):
    ports = [f'port:src{i}="number out = 0"' for i in range(SOURCE_PORTS)]
    ports += [f'port:{p}="number in = 0"' for p in port_sinks]
    lines = [
        "<xt>",
        "  <head>",
        f'    <link href="{ui_id}.xts"/>',
        f"    <title>{title}</title>",
        "  </head>",
        "  <body>",
        f'    <div id="{ui_id}" ' + " ".join(ports) + ">",
    ]
    for i in range(HOVER_CELLS):
        lines.append(f'      <div id="hv{i}" class="cell"/>')
    for sid in style_sinks:
        lines.append(f'      <div id="{sid}" class="box"/>')
    for tid in text_sinks:
        lines.append(f'      <span id="{tid}" class="label">0</span>')
    for i in range(SOURCE_PORTS):
        lines.append(f'      <div id="sb{i}" class="bar" bind:height="src{i} | range(0,100,0px,40px)"/>')
    for i, port in enumerate(port_sinks):
        lines.append(f'      <div id="ib{i}" class="bar" bind:width="{port} | range(0,100,0px,120px)"/>')
    lines += ["    </div>", "  </body>", "</xt>", ""]
    path.write_text("\n".join(lines), encoding="utf-8", newline="\n")


def write_xts(path, ui_id, style_sinks, text_sinks, port_sinks):
    out = [
        "/* Generated by Tests/perf/flow_big/gen_flow_big.py (T-403). Do not edit. */",
        f"#{ui_id} {{ position: relative; width: {WIDTH}px; height: {HEIGHT}px; "
        "background-color: #20242c; overflow: hidden; }",
        ".cell { position: absolute; width: 40px; height: 40px; background-color: #3a6ea5; }",
        ".cell:hover { background-color: #e0a030; }",
        ".box { position: absolute; width: 24px; height: 24px; background-color: #8fbf6a; }",
        ".label { position: absolute; color: #f0f0f0; font-size: 12px; }",
        ".bar { position: absolute; width: 8px; height: 8px; background-color: #c05050; }",
    ]
    # Hover cells centred on the viewport diagonal (the --synth-pointer sweep).
    for i in range(HOVER_CELLS):
        cx = WIDTH * (i + 0.5) / HOVER_CELLS
        cy = cx * HEIGHT / WIDTH
        out.append(f"#hv{i} {{ left: {cx - 20:.0f}px; top: {cy - 20:.0f}px; }}")
    for i, sid in enumerate(style_sinks):
        out.append(f"#{sid} {{ left: {40 + (i % 10) * 32}px; top: {420 + (i // 10) * 32}px; }}")
    for i, tid in enumerate(text_sinks):
        out.append(f"#{tid} {{ left: {720 + (i % 5) * 110}px; top: {40 + (i // 5) * 20}px; }}")
    for i in range(SOURCE_PORTS):
        out.append(f"#sb{i} {{ left: {720 + i * 12}px; top: 300px; }}")
    for i in range(len(port_sinks)):
        out.append(f"#ib{i} {{ left: 1000px; top: {360 + i * 10}px; }}")
    path.write_text("\n".join(out) + "\n", encoding="utf-8", newline="\n")


def write_project(path, ui_id, name, project_id):
    project = {
        "version": 1,
        "name": name,
        "id": project_id,
        # T-476: behavior 1 (43 §43.3), coalesced value signals -- what M37's targets measure.
        "spec": {"behavior": 1},
        "uis": [{
            "id": ui_id,
            "doc": f"ui/{ui_id}/{ui_id}.xt",
            "flow": f"ui/{ui_id}/{ui_id}.xtflow",
            "size": [WIDTH, HEIGHT],
        }],
    }
    path.write_text(json.dumps(project, indent=2) + "\n", encoding="utf-8", newline="\n")


def main():
    for directory, ui_id, name, seed, total, sinks in FIXTURES:
        g, style_sinks, text_sinks, port_sinks = build_graph(seed, total, sinks)
        root = HERE / directory
        ui_dir = root / "ui" / ui_id
        ui_dir.mkdir(parents=True, exist_ok=True)
        write_project(root / f"{ui_id}.xtui", ui_id, name, ui_id)
        write_xt(ui_dir / f"{ui_id}.xt", ui_id, name, style_sinks, text_sinks, port_sinks)
        write_xts(ui_dir / f"{ui_id}.xts", ui_id, style_sinks, text_sinks, port_sinks)
        flow = {"version": 1, "nodes": g.nodes, "edges": g.edges}
        (ui_dir / f"{ui_id}.xtflow").write_text(json.dumps(flow, indent=1) + "\n", encoding="utf-8",
                                                newline="\n")
        print(f"wrote {directory}: {len(g.nodes)} nodes, {len(g.edges)} edges, "
              f"{len(style_sinks) + len(text_sinks) + len(port_sinks)} sinks")


if __name__ == "__main__":
    main()
