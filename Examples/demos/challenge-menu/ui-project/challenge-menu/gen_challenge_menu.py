"""Generates challenge-menu.xtflow (XtendUI signal flow, Docs/design/17-editor.md §17.6).

One `radio` holds the selected difficulty. Per row, a `compare` against the row index adds or
removes class `on` (the board's CSS transitions fade the highlight and star in; the row itself stays
put so the hovered hit area never moves). Every per-level value is one `lookup`. Each selection
change toggles class `flip` on the preview parts, which swaps their animation name and replays it.
The pointer shifts the layers by depth (gaze parallax: a pointermove trigger plus bind:left/top in
challenge-menu.xt); selection never moves them.
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

# --- Fly-in -----------------------------------------------------------------------------------
# The gaze parallax and the hologram spin are not in the flow: the flow only triggers and selects
# (Docs/design/27-interaction.md 27.4.3). The root's `on:pointermove` writes the `nx` / `ny` ports,
# `bind:left` / `bind:top` on the layers read them (challenge-menu.xt), and the rings spin through
# `animation` declarations (challenge-menu.xts). What stays here is the intro: for the first 0.15 s
# the two panels are parked off-screen (the `listShift` / `panelShift` ports start at the parked
# offset), then the timer's single tick releases them to 0 and the board's `transition` on left/top
# slides them in.
node("introT", "timer", interval=0.15, autostart=True, repeat=False)
node("parked", "toggle", initial=True)
edge("introT.tick", "parked.setFalse")
# key, port, parked-off-screen offset of `left` from the rest position
for key, shift in (("list", -650), ("panel", 750)):
    node(f"{key}Parked", "constant", value=shift)
    node(f"{key}Free", "constant", value=0)
    node(f"{key}Shift", "select")
    edge("parked.value", f"{key}Shift.cond")
    edge(f"{key}Parked.out", f"{key}Shift.a")
    edge(f"{key}Free.out", f"{key}Shift.b")
    edge(f"{key}Shift.out", f"#challenge-menu.{key}Shift")

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "challenge-menu.xtflow")
with open(out, "w", encoding="utf-8") as f:
    json.dump({"version": 1, "nodes": nodes, "edges": edges}, f, indent=1)
print(f"wrote {out}: {len(nodes)} nodes, {len(edges)} edges")
