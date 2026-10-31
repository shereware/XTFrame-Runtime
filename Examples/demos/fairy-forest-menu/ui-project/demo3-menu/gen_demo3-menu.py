"""Generate demo3-menu.xtflow: an empty signal graph.

The colour reveal that follows the pointer is no longer a flow (Docs/design/27-interaction.md
27.4.3: a flow drives, it never interpolates). Three copies of the colour background
(#trail-1..3) sit above the gray one; each carries a static radial `gradient-mask` effect in
demo3-menu.xts, and `bind:effect.gradient-mask.p0x|p0y|p1x|p1y` in demo3-menu.xt moves its two
points with the `px`/`py` ports that the root's `on:pointermove` writes from `pointer.local()`.
The lag comes from `| tween(<d>, ease-out)` per layer (90 ms, 320 ms, 700 ms), so the slower
layers trail behind. The reveal radius is the `offset()` of the second point (130, 170, 210 px).
The `.xtui` keeps referencing this file, so it stays as a valid graph with no nodes.
"""
import json
from pathlib import Path

out = Path(__file__).with_name("demo3-menu.xtflow")
out.write_text(json.dumps({"version": 1, "nodes": [], "edges": []}, indent=2) + "\n", encoding="utf-8")
print(f"wrote {out.name}: 0 nodes, 0 edges")
