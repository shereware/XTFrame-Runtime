#!/usr/bin/env python3
"""Project-level golden corpus check (T-401, docs/design/43-compatibility-and-versions.md 43.4).

For every project under tests/corpus/<project>/ (project.json): copy the source UI Project to a
work directory, pack it with `xtframe-pack --strict`, render every UI headless (SW path) at the
listed frames, run the behaviour scripts (one xtframe-view run per step: the events of every step
so far, a screenshot and the port / flow event lines of the run), dump each UI's port table, and
compare all of it with tests/corpus/<project>/expected/: screenshots with the tests/golden SW rule
(a pixel differs when a channel moves by more than XT_GOLDEN_CHANNEL, default 2; at most
XT_GOLDEN_RATIO of the pixels may differ, default 0), manifest.json (normalized), ports.json and
the script logs byte for byte. A differing screenshot gets a `<name>.diff.png` next to the actual
image (differing pixels red, the rest the expected image's luminance).

Usage: corpus_check.py [--update] [--project <name>] [--view EXE] [--pack EXE] [--work DIR]
  --update (or XT_CORPUS_UPDATE=1) rewrites expected/ from this run instead of comparing.
Exit codes: 0 every project matches (or was updated), 1 a difference or a failed run, 2 usage.
Standard library only.
"""

import argparse
import glob
import json
import os
import shutil
import struct
import subprocess
import sys
import zlib

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORPUS = os.path.join(REPO, "tests", "corpus")
EXE = ".exe" if os.name == "nt" else ""


# ---------------------------------------------------------------------------------------------
# PNG (8-bit RGB / RGBA / gray / gray-alpha, non-interlaced: what xt::sw::writePng writes).
# ---------------------------------------------------------------------------------------------

def read_png(path):
    """Returns (width, height, RGBA bytes)."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("%s: not a PNG" % path)
    pos = 8
    idat = []
    width = height = depth = ctype = interlace = None
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
    channels = {0: 1, 2: 3, 4: 2, 6: 4}.get(ctype)
    if depth != 8 or channels is None or interlace != 0:
        raise ValueError("%s: unsupported PNG (depth %s, color type %s)" % (path, depth, ctype))
    raw = zlib.decompress(b"".join(idat))
    stride = width * channels
    out = bytearray(stride * height)
    prev = bytearray(stride)
    src = 0
    for y in range(height):
        ftype = raw[src]
        line = bytearray(raw[src + 1:src + 1 + stride])
        src += 1 + stride
        if ftype == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif ftype == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ftype == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif ftype == 4:
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = prev[i]
                c = prev[i - channels] if i >= channels else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
        elif ftype != 0:
            raise ValueError("%s: bad filter %d" % (path, ftype))
        out[y * stride:(y + 1) * stride] = line
        prev = line
    if channels == 4:
        return width, height, bytes(out)
    rgba = bytearray(width * height * 4)
    for i in range(width * height):
        if channels == 3:
            rgba[i * 4:i * 4 + 3] = out[i * 3:i * 3 + 3]
            rgba[i * 4 + 3] = 255
        elif channels == 1:
            rgba[i * 4:i * 4 + 3] = bytes((out[i],)) * 3
            rgba[i * 4 + 3] = 255
        else:
            rgba[i * 4:i * 4 + 3] = bytes((out[i * 2],)) * 3
            rgba[i * 4 + 3] = out[i * 2 + 1]
    return width, height, bytes(rgba)


def write_png(path, width, height, rgba):
    def chunk(kind, body):
        crc = zlib.crc32(kind + body) & 0xFFFFFFFF
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", crc)

    stride = width * 4
    raw = b"".join(b"\x00" + rgba[y * stride:(y + 1) * stride] for y in range(height))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def tolerance():
    channel, ratio = 2, 0.0
    try:
        channel = int(os.environ.get("XT_GOLDEN_CHANNEL", "2"))
    except ValueError:
        pass
    try:
        ratio = float(os.environ.get("XT_GOLDEN_RATIO", "0"))
    except ValueError:
        pass
    return channel, ratio


def compare_png(expected, actual, diff_path):
    """tests/golden's compareRgba() rule. Returns None on a pass, else a one-line reason."""
    if expected_bytes_equal(expected, actual):
        return None
    ew, eh, a = read_png(expected)
    aw, ah, b = read_png(actual)
    if (ew, eh) != (aw, ah):
        return "size %dx%d, expected %dx%d" % (aw, ah, ew, eh)
    channel, ratio = tolerance()
    differ = bytearray(ew * eh)
    count = 0
    max_diff = 0
    # Fast path: compare row slices first, then pixels only on differing rows.
    stride = ew * 4
    for y in range(eh):
        ra = a[y * stride:(y + 1) * stride]
        rb = b[y * stride:(y + 1) * stride]
        if ra == rb:
            continue
        for x in range(ew):
            i = x * 4
            d = max(abs(ra[i] - rb[i]), abs(ra[i + 1] - rb[i + 1]), abs(ra[i + 2] - rb[i + 2]),
                    abs(ra[i + 3] - rb[i + 3]))
            if d > max_diff:
                max_diff = d
            if d > channel:
                differ[y * ew + x] = 1
                count += 1
    if count <= ew * eh * ratio:
        return None
    out = bytearray(ew * eh * 4)
    for i in range(ew * eh):
        if differ[i]:
            out[i * 4:i * 4 + 4] = b"\xff\x00\x00\xff"
        else:
            r, g, bl = a[i * 4], a[i * 4 + 1], a[i * 4 + 2]
            lum = (r * 299 + g * 587 + bl * 114) // 1000
            out[i * 4:i * 4 + 4] = bytes((lum, lum, lum, 255))
    write_png(diff_path, ew, eh, bytes(out))
    return "%d px differ, max channel diff %d (allowed: ratio %g, channel %d)" % (count, max_diff, ratio,
                                                                                   channel)


def expected_bytes_equal(a, b):
    with open(a, "rb") as fa, open(b, "rb") as fb:
        return fa.read() == fb.read()


# ---------------------------------------------------------------------------------------------
# Tools and runs.
# ---------------------------------------------------------------------------------------------

def find_tool(name, given):
    if given:
        return given
    env = os.environ.get("XT_CORPUS_" + name.upper().replace("XTFRAME-", ""))
    if env:
        return env
    found = []
    for pattern in ("build/*/tools/*/%s%s", "build/*/tools/%s%s", "build/*/bin/%s%s"):
        found += glob.glob(os.path.join(REPO, pattern % (name, EXE)))
    if not found:
        return None
    return max(found, key=os.path.getmtime)


def run(cmd, log_path=None):
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, stdin=subprocess.DEVNULL)
    out = proc.stdout.decode("utf-8", "replace").replace("\r\n", "\n")
    err = proc.stderr.decode("utf-8", "replace").replace("\r\n", "\n")
    if log_path:
        with open(log_path, "a", encoding="utf-8", newline="\n") as f:
            f.write("$ " + " ".join(cmd) + "\n" + out + err + "\n")
    return proc.returncode, out, err


def normalize_manifest(path):
    """manifest.json without its timestamps (`builtAt` and friends) and the product version
    (`bundle.xtframe`, which every release changes), object keys sorted and lists of entries that
    carry a `path` / `id` / `name` / `file` key sorted by it; hashes and every other value kept."""
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    drop = {"builtAt", "generatedAt", "packedAt", "timestamp"}
    if isinstance(data.get("bundle"), dict):
        data["bundle"].pop("xtframe", None)

    def clean(value):
        if isinstance(value, dict):
            return {k: clean(v) for k, v in value.items() if k not in drop}
        if isinstance(value, list):
            items = [clean(v) for v in value]
            if items and all(isinstance(v, dict) for v in items):
                key = next((k for k in ("path", "id", "name", "file") if all(k in v for v in items)), None)
                if key is not None:
                    return sorted(items, key=lambda v: str(v[key]))
            return items
        return value

    return json.dumps(clean(data), indent=2, sort_keys=True, ensure_ascii=False) + "\n"


LOG_PREFIXES = ("xtframe-view: flow event ", "xtframe-view: port ", "xtframe-view: drop ", "text=")


def log_lines(out):
    return [line for line in out.split("\n") if line.startswith(LOG_PREFIXES)]


class Checker:
    def __init__(self, args):
        self.args = args
        self.update = args.update or os.environ.get("XT_CORPUS_UPDATE", "") == "1"
        self.failures = []

    def fail(self, project, message):
        self.failures.append("%s: %s" % (project, message))
        print("[FAIL] %s: %s" % (project, message), flush=True)

    def view(self, bundle, ui, frames, extra, screenshot, project, label, runlog):
        cmd = [self.args.view, "--project", bundle, "--ui", ui["id"], "--headless", "--frames", str(frames),
               "--width", str(ui["width"]), "--height", str(ui["height"]), "--screenshot", screenshot] + extra
        rc, out, err = run(cmd, runlog)
        if rc != 0 or not os.path.isfile(screenshot):
            self.fail(project, "%s: xtframe-view exited %d (see %s)" % (label, rc, runlog))
            return None
        return out

    def check_project(self, name):
        pdir = os.path.join(CORPUS, name)
        with open(os.path.join(pdir, "project.json"), encoding="utf-8") as f:
            spec = json.load(f)
        work = os.path.join(self.args.work, name)
        shutil.rmtree(work, ignore_errors=True)
        actual = os.path.join(work, "actual")
        os.makedirs(actual)
        runlog = os.path.join(work, "run.log")
        # The pack may write into its project (standard symbols, measured asset fields): pack a copy.
        src = os.path.join(work, "source")
        shutil.copytree(os.path.join(REPO, spec["source"]), src)
        bundle = os.path.join(work, "bundle")
        rc, out, err = run([self.args.pack, "--strict", src, "-o", bundle], runlog)
        if rc != 0:
            self.fail(name, "xtframe-pack --strict exited %d (see %s)" % (rc, runlog))
            return
        shutil.copyfile(os.path.join(bundle, "manifest.json"), os.path.join(actual, "manifest.raw.json"))
        with open(os.path.join(actual, "manifest.json"), "w", encoding="utf-8", newline="\n") as f:
            f.write(normalize_manifest(os.path.join(bundle, "manifest.json")))
        if spec.get("dpr", 1) != 1:
            self.fail(name, "dpr %s: xtframe-view renders at dpr 1 only" % spec["dpr"])
            return
        ports = {}
        uis = {ui["id"]: ui for ui in spec["uis"]}
        for ui in spec["uis"]:
            udir = os.path.join(actual, ui["id"])
            os.makedirs(udir)
            for i, frame in enumerate(spec["frames"]):
                extra = []
                if i == 0:
                    extra = ["--dump-ports", os.path.join(work, ui["id"] + ".ports.json")]
                shot = os.path.join(udir, "frame-%d.png" % frame)
                if self.view(bundle, ui, frame + 1, extra, shot, name, "%s frame %d" % (ui["id"], frame),
                             runlog) is None:
                    return
            with open(os.path.join(work, ui["id"] + ".ports.json"), encoding="utf-8") as f:
                ports[ui["id"]] = json.load(f)["ports"]
        with open(os.path.join(actual, "ports.json"), "w", encoding="utf-8", newline="\n") as f:
            f.write(json.dumps(ports, indent=2, ensure_ascii=False) + "\n")
        for script_name in spec.get("scripts", []):
            with open(os.path.join(pdir, "scripts", script_name), encoding="utf-8") as f:
                script = json.load(f)
            ui = uis[script["ui"]]
            base = os.path.splitext(script_name)[0]
            udir = os.path.join(actual, ui["id"])
            log = ["# script %s (ui %s)" % (script_name, ui["id"])]
            extra = []
            for k, step in enumerate(script["steps"]):
                for event in step.get("events", []):
                    extra += ["--event", event]
                for pointer in step.get("pointers", []):
                    extra += ["--pointer", pointer]
                args = extra + ["--log-ports"]
                for element in script.get("printText", []):
                    args += ["--print-text", element]
                shot = os.path.join(udir, "script-%s-step%d.png" % (base, k + 1))
                out = self.view(bundle, ui, step["frame"] + 1, args, shot, name,
                                "script %s step %d" % (script_name, k + 1), runlog)
                if out is None:
                    return
                log.append("## step %d: frame %d %s" % (k + 1, step["frame"], step.get("note", "")))
                log += log_lines(out)
            with open(os.path.join(udir, "script-%s.log" % base), "w", encoding="utf-8", newline="\n") as f:
                f.write("\n".join(log) + "\n")
        os.remove(os.path.join(actual, "manifest.raw.json"))
        expected = os.path.join(pdir, "expected")
        if self.update:
            shutil.rmtree(expected, ignore_errors=True)
            shutil.copytree(actual, expected)
            print("[UPDATED] %s -> %s" % (name, os.path.relpath(expected, REPO)), flush=True)
            return
        self.compare_tree(name, expected, actual)

    def compare_tree(self, name, expected, actual):
        def files(root):
            result = set()
            for dirpath, _, filenames in os.walk(root):
                for fn in filenames:
                    if not fn.endswith(".diff.png"):
                        result.add(os.path.relpath(os.path.join(dirpath, fn), root).replace("\\", "/"))
            return result

        if not os.path.isdir(expected):
            self.fail(name, "no expected/ (run with --update)")
            return
        exp_files, act_files = files(expected), files(actual)
        for missing in sorted(exp_files - act_files):
            self.fail(name, "expected/%s not produced" % missing)
        for extra in sorted(act_files - exp_files):
            self.fail(name, "new output %s (not in expected/)" % os.path.join(actual, extra))
        bad = 0
        for rel in sorted(exp_files & act_files):
            e, a = os.path.normpath(os.path.join(expected, rel)), os.path.normpath(os.path.join(actual, rel))
            if rel.endswith(".png"):
                diff = a[:-4] + ".diff.png"
                reason = compare_png(e, a, diff)
                if reason:
                    bad += 1
                    self.fail(name, "%s: %s; actual %s, diff %s" % (rel, reason, a, diff))
            elif not expected_bytes_equal(e, a):
                bad += 1
                self.fail(name, "%s differs: diff %s %s" % (rel, e, a))
        if bad == 0 and exp_files == act_files:
            print("[PASS] %s (%d files)" % (name, len(exp_files)), flush=True)


def main():
    parser = argparse.ArgumentParser(description="XTFrame project-level golden corpus check (T-401)")
    parser.add_argument("--update", action="store_true", help="rewrite expected/ from this run")
    parser.add_argument("--project", action="append", default=[], help="only this corpus project (repeatable)")
    parser.add_argument("--view", help="xtframe-view executable (default: newest under build/)")
    parser.add_argument("--pack", help="xtframe-pack executable (default: newest under build/)")
    parser.add_argument("--work", help="work directory (default: build/corpus-work)")
    args = parser.parse_args()
    args.view = find_tool("xtframe-view", args.view)
    args.pack = find_tool("xtframe-pack", args.pack)
    if not args.view or not os.path.isfile(args.view) or not args.pack or not os.path.isfile(args.pack):
        print("corpus_check: xtframe-view / xtframe-pack not found (build them, or pass --view / --pack)")
        return 2
    args.view, args.pack = os.path.abspath(args.view), os.path.abspath(args.pack)
    args.work = os.path.abspath(args.work or os.path.join(REPO, "build", "corpus-work"))
    names = sorted(d for d in os.listdir(CORPUS) if os.path.isfile(os.path.join(CORPUS, d, "project.json")))
    for p in args.project:
        if p not in names:
            print("corpus_check: no corpus project '%s' (have: %s)" % (p, ", ".join(names)))
            return 2
    if args.project:
        names = [n for n in names if n in args.project]
    print("corpus_check: view %s, pack %s, work %s" % (args.view, args.pack, args.work), flush=True)
    checker = Checker(args)
    for name in names:
        checker.check_project(name)
    if checker.failures:
        print("CORPUS CHECK: FAIL (%d)" % len(checker.failures))
        return 1
    print("CORPUS CHECK: %s (%d project(s))" % ("UPDATED" if checker.update else "PASS", len(names)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
