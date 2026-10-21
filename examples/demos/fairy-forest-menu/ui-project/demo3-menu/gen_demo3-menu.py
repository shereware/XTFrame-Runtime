"""Generate demo3-menu.xtflow: a colour reveal that follows the pointer with a short trail.

Three copies of the colour background (#trail-1..3) sit above the gray one. Each gets a radial
gradient-mask centred on the pointer. The lag comes from each layer's own `transition: effect`
on the board (a runtime setStyle cannot carry `transition`), so the slower layers trail behind.
"""
import json
from pathlib import Path

# id, mask radius px, peak alpha of the reveal (lag: the board's transition per layer)
TRAILS = [
    ("trail-1", 130, "ff"),
    ("trail-2", 170, "b3"),
    ("trail-3", 210, "73"),
]

STYLE = (
    "visibility: visible; "
    "effect: gradient-mask(radial, {{a}}px {{b}}px, {{c}}px {{b}}px, "
    "#000000{alpha} 0%, #000000{alpha} 35%, #00000000 100%)"
)

nodes, edges = [], []


def node(id_, type_, **params):
    nodes.append({"id": id_, "type": type_, "params": params})


def edge(src, dst, **params):
    e = {"from": src, "to": dst}
    if params:
        e["params"] = params
    edges.append(e)


for tid, radius, alpha in TRAILS:
    key = tid.replace("-", "")
    node(f"{key}_r", "constant", value=radius)
    node(f"{key}_edge", "math", op="add")          # mask edge point: pointerX + radius
    edge("#document.pointerX", f"{key}_edge.a")
    edge(f"{key}_r.out", f"{key}_edge.b")
    node(f"{key}_style", "format", format=STYLE.format(alpha=alpha), decimals=0)
    edge("#document.pointerX", f"{key}_style.a")
    edge("#document.pointerY", f"{key}_style.b")
    edge(f"{key}_edge.out", f"{key}_style.c")
    edge(f"{key}_style.out", f"#{tid}.setStyle")

out = Path(__file__).with_name("demo3-menu.xtflow")
out.write_text(json.dumps({"version": 1, "nodes": nodes, "edges": edges}, indent=2) + "\n", encoding="utf-8")
print(f"wrote {out.name}: {len(nodes)} nodes, {len(edges)} edges")
