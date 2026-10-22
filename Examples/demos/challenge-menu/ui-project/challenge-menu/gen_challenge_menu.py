"""Generates challenge-menu.xtflow (XtendUI signal flow, Docs/design/17-editor.md §17.6).

One `radio` holds the selected difficulty. Per row, a `compare` against the row index adds or
removes class `on` (the board's CSS transitions fade the highlight and star in; the row itself stays
put so the hovered hit area never moves). Every per-level value is one `lookup`. Each selection
change toggles class `flip` on the preview parts, which swaps their animation name and replays it.
The pointer shifts the layers by depth (gaze parallax, via left/top); selection never moves them.
"""
import json
import os

LEVELS = [
    dict(id="recruit", name="RECRUIT", tone=1.0,
         desc="A WALK IN THE PARK. THE COMBAT SUIT WILL HELP TO KEEP YOU ALIVE.",
         hint="A GOOD PLACE TO LEARN WHAT THE SUIT CAN DO."),
    dict(id="soldier", name="SOLDIER", tone=0.95,
         desc="ENEMIES FIGHT BACK. USE THE ARMOR AND THE CLOAK WISELY.",
         hint="THE WAY THE JUNGLE WAS MEANT TO BE FOUGHT."),
    dict(id="veteran", name="VETERAN", tone=0.88,
         desc="HOSTILES FLANK AND SUPPRESS. MISTAKES ARE PUNISHED QUICKLY.",
         hint="FOR THOSE WHO ALREADY KNOW THE JUNGLE."),
    dict(id="supersoldier", name="SUPERSOLDIER", tone=0.8,
         desc="NO CROSSHAIR, NO HIT MARKERS. THE ENEMY SPEAKS ITS OWN LANGUAGE.",
         hint="NO ASSISTS. TRUST YOUR INSTINCTS."),
    dict(id="posthuman", name="POST-HUMAN", tone=0.72,
         desc="ONE LIFE. SUIT ENERGY DRAINS FAST AND THE ENEMY NEVER MISSES.",
         hint="ONLY THE SUIT WILL REMEMBER YOUR NAME."),
    dict(id="impossible", name="IMPOSSIBLE", tone=0.62,
         desc="NOTHING WILL SAVE YOU. EVERY SHOT IS LETHAL, EVERY SHADOW IS HOSTILE.",
         hint="NO SUIT CAN SAVE YOU HERE. WHY ARE YOU TRYING?"),
]
INITIAL = 5  # IMPOSSIBLE, matches the board (class "on" on its row)
SWAPPED = ["#preview-frame", "#preview-tag", "#preview-desc", "#hint-text"]

nodes, edges = [], []


def node(nid, ntype, **params):
    n = {"id": nid, "type": ntype}
    if params:
        n["params"] = params
    nodes.append(n)
    return nid


def edge(src, dst, **params):
    e = {"from": src, "to": dst}
    if params:
        e["params"] = params
    edges.append(e)


def table(nid, values, *sinks, index="sel.index", **sink_params):
    node(nid, "lookup", values=list(values))
    edge(index, f"{nid}.index")
    for sink in sinks:
        edge(f"{nid}.out", sink, **sink_params)
    return f"{nid}.out"


# --- Selection -------------------------------------------------------------------------------
node("sel", "radio", count=len(LEVELS), initial=INITIAL)
for i, lv in enumerate(LEVELS):
    for trigger in ("PointerEnter", "Focus", "Click"):
        edge(f"#diff-{lv['id']}.{trigger}", f"sel.set{i}")

# Row state: class "on" on the row, its highlight, its star and its label.
for i, lv in enumerate(LEVELS):
    k = lv["id"]
    node(f"idx_{k}", "constant", value=i)
    node(f"is_{k}", "compare", op="eq")
    edge("sel.index", f"is_{k}.a")
    edge(f"idx_{k}.out", f"is_{k}.b")
    for target in (f"#row-{k}", f"#hl-{k}", f"#star-{k}", f"#diff-{k}"):
        edge(f"is_{k}.true", f"{target}.addClass", **{"class": "on"})
        edge(f"is_{k}.false", f"{target}.removeClass", **{"class": "on"})

# Replay the swap fades on every change.
for target in SWAPPED:
    edge("sel.changed", f"{target}.toggleClass", **{"class": "flip"})

# The preview image pops toward the camera and settles back: class "pop" for 0.16 s; the board's
# transition on .frame-z does the motion both ways.
node("popTimer", "timer", interval=0.16, autostart=False, repeat=False)
edge("sel.changed", "#preview-frame.addClass", **{"class": "pop"})
edge("sel.changed", "popTimer.start")
edge("popTimer.tick", "#preview-frame.removeClass", **{"class": "pop"})

# --- Preview panel ---------------------------------------------------------------------------
table("tagText", [lv["name"] for lv in LEVELS], "#preview-tag.setText")
table("descText", [lv["desc"] for lv in LEVELS], "#preview-desc.setText")
table("hintText", [lv["hint"] for lv in LEVELS], "#hint-text.setText")
table("imageTone", [f"opacity: {lv['tone']:.2f}" for lv in LEVELS], "#preview-image.setStyle")

# --- Gaze parallax and fly-in ------------------------------------------------------------------
# Only the pointer moves these layers; selection never does. Each layer's `left`/`top` is its
# authored position plus a small offset for its depth, the way things in front of you shift when
# your eyes move: the two panels against the gaze, the far background a little with it. The values
# go through setStyle, and the board's `transition` on left/top glides each layer to the new spot.
# (Transforms are not an option here: the flow's setProperty TranslateX/Y/Z never renders, and a
# transform set from a style string renders wrong inside the 3D scene.)
#
# The intro uses the same path: for the first 0.15 s the two panels are parked off-screen with the
# transition switched off, then the normal position takes over and the transition slides them in.
node("clock", "clock")
node("introEnd", "constant", value=0.15)
node("introOn", "compare", op="lt")
edge("clock.seconds", "introOn.a")
edge("introEnd.out", "introOn.b")

GLIDE = "transition: left 700ms cubic-bezier(.2,.7,.2,1), top 700ms cubic-bezier(.2,.7,.2,1)"
GAZE = [
    # key, node, rest x, rest y, x shift, y shift (px at the screen edge), parked-off-screen x
    ("list", "#diff-list", 130, 196, 14, 8, -520),
    ("panel", "#panel-preview", 650, 166, 20, 10, 1400),
    ("world", "#world-layer", -40, -24, -26, -14, None),
]
for key, target, x0, y0, rx, ry, park in GAZE:
    node(f"{key}X", "math", op="map", inLo=-1, inHi=1, outLo=x0 + rx, outHi=x0 - rx)
    node(f"{key}Y", "math", op="map", inLo=-1, inHi=1, outLo=y0 + ry, outHi=y0 - ry)
    edge("#document.pointerNX", f"{key}X.a")
    edge("#document.pointerNY", f"{key}Y.a")
    node(f"{key}Pos", "format", format="left: {a}px; top: {b}px; " + GLIDE, decimals=1)
    edge(f"{key}X.out", f"{key}Pos.a")
    edge(f"{key}Y.out", f"{key}Pos.b")
    out = f"{key}Pos.out"
    if park is not None:
        node(f"{key}Park", "constant", value=f"left: {park}px; top: {y0}px; transition: left 0ms, top 0ms")
        node(f"{key}Style", "select")
        edge("introOn.out", f"{key}Style.cond")
        edge(f"{key}Park.out", f"{key}Style.a")
        edge(out, f"{key}Style.b")
        out = f"{key}Style.out"
    edge(out, f"{target}.setStyle")

# Hologram rings: spun from the clock, counter-rotating, inside the board's scaleY wrapper.
for nid, target, speed in (("ringSpin", "#holo-ring", 50), ("arcSpin", "#holo-arc", -110)):
    node(f"{nid}_k", "constant", value=speed)
    node(nid, "math", op="mul")
    edge("clock.seconds", f"{nid}.a")
    edge(f"{nid}_k.out", f"{nid}.b")
    node(f"{nid}Style", "format", format="transform: rotate({a}deg)", decimals=2)
    edge(f"{nid}.out", f"{nid}Style.a")
    edge(f"{nid}Style.out", f"{target}.setStyle")

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "challenge-menu.xtflow")
with open(out, "w", encoding="utf-8") as f:
    json.dump({"version": 1, "nodes": nodes, "edges": edges}, f, indent=1)
print(f"wrote {out}: {len(nodes)} nodes, {len(edges)} edges")
