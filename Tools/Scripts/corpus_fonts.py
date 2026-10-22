#!/usr/bin/env python3
"""Corpus font pinning (T-429, Docs/design/43-compatibility-and-versions.md 43.4).

A Tests/corpus/<project>/project.json `fonts` element is either a family name (`"Georgia"`: only
the family must be installed) or an object `{"family": "Georgia", "sha256": "<hex>"}`: the family
must be installed from a file with that SHA-256 (a different build of the same font renders
different glyph edges, so the expected/ images cannot match). This module reads
`xtframe-view --list-fonts` (`family<TAB>path` lines) and hashes the font files; Tools/Scripts/corpus_check.py
uses it for the check, and `--update` here (also run by `corpus_check.py --update`) rewrites every
project's `fonts` to objects carrying this machine's hashes.

When a family is registered from several files (Regular / Bold / Italic in separate files) a check
passes when any of them matches; `--update` keeps a stored hash that still matches one of them and
otherwise pins the file with the shortest base name (`georgia.ttf` over `georgiab.ttf`: the regular
face in the Windows naming), ties broken by name.

Usage: corpus_fonts.py --update [--project <name>] [--view EXE]
Exit codes: 0 updated, 1 a listed family is not installed or xtframe-view failed, 2 usage.
Standard library only.
"""

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CORPUS = os.path.join(REPO, "Tests", "corpus")


def list_fonts(view):
    """{family: [path, ...]} from `xtframe-view --list-fonts`, or None when the run fails."""
    proc = subprocess.run([view, "--list-fonts"], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          stdin=subprocess.DEVNULL)
    if proc.returncode != 0:
        return None
    fonts = {}
    for line in proc.stdout.decode("utf-8", "replace").replace("\r\n", "\n").split("\n"):
        if not line.strip():
            continue
        family, _, path = line.partition("\t")
        paths = fonts.setdefault(family.strip(), [])
        if path and path not in paths:
            paths.append(path)
    return fonts


_hash_cache = {}


def sha256_file(path):
    """Lowercase hex SHA-256 of `path`, or None when it cannot be read."""
    if path not in _hash_cache:
        digest = hashlib.sha256()
        try:
            with open(path, "rb") as f:
                for block in iter(lambda: f.read(1 << 20), b""):
                    digest.update(block)
            _hash_cache[path] = digest.hexdigest()
        except OSError:
            _hash_cache[path] = None
    return _hash_cache[path]


def entry_family(entry):
    return entry["family"] if isinstance(entry, dict) else entry


def preferred_path(paths):
    return min(paths, key=lambda p: (len(os.path.basename(p)), os.path.basename(p).lower(), p))


def font_problem(entry, fonts):
    """None when `entry` (a `fonts` element) is satisfied by `fonts` (list_fonts()), else the
    reason: "font 'F' is not installed" or "font 'F' is a different build (sha256 ... expected ...)"."""
    family = entry_family(entry)
    if family not in fonts:
        return "font '%s' is not installed" % family
    want = entry.get("sha256", "").lower() if isinstance(entry, dict) else ""
    if not want:
        return None
    paths = fonts[family]
    hashes = [sha256_file(p) for p in paths]
    if want in hashes:
        return None
    have = sha256_file(preferred_path(paths)) if paths else None
    return "font '%s' is a different build (sha256 %s... expected %s...)" % (
        family, (have or "unreadable")[:12], want[:12])


def pinned_entry(entry, fonts):
    """`entry` as an object with this machine's hash, or None when its family is not installed."""
    family = entry_family(entry)
    paths = [p for p in fonts.get(family, []) if sha256_file(p)]
    if not paths:
        return None
    old = entry.get("sha256", "").lower() if isinstance(entry, dict) else ""
    hashes = [sha256_file(p) for p in paths]
    digest = old if old in hashes else sha256_file(preferred_path(paths))
    return {"family": family, "sha256": digest}


def update_projects(view, names, fonts=None):
    """Rewrites the `fonts` of Tests/corpus/<name>/project.json for every name; returns the
    number of problems (a family that is not installed, or a failed --list-fonts run)."""
    if fonts is None:
        fonts = list_fonts(view)
    if fonts is None:
        print("corpus_fonts: xtframe-view --list-fonts failed")
        return 1
    problems = 0
    for name in names:
        path = os.path.join(CORPUS, name, "project.json")
        with open(path, encoding="utf-8") as f:
            text = f.read()
        spec = json.loads(text)
        if "fonts" not in spec:
            continue
        pinned = []
        for entry in spec["fonts"]:
            new = pinned_entry(entry, fonts)
            if new is None:
                print("[FONTS] %s: font '%s' is not installed; entry left as is" % (name, entry_family(entry)))
                problems += 1
                new = entry
            pinned.append(new)
        line = '"fonts": ' + json.dumps(pinned, ensure_ascii=False)
        new_text, count = re.subn(r'"fonts":\s*\[[^\]]*\]', lambda m: line, text, count=1)
        if count != 1 or json.loads(new_text).get("fonts") != pinned:
            spec["fonts"] = pinned
            new_text = json.dumps(spec, indent=2, ensure_ascii=False) + "\n"
        if new_text != text:
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(new_text)
        print("[FONTS] %s: %s" % (name, ", ".join(
            "%s %s" % (e["family"], e["sha256"]) if isinstance(e, dict) else e for e in pinned)), flush=True)
    return problems


def main():
    parser = argparse.ArgumentParser(description="XTFrame corpus font pinning (T-429)")
    parser.add_argument("--update", action="store_true", help="pin every project's fonts to this machine's files")
    parser.add_argument("--project", action="append", default=[], help="only this corpus project (repeatable)")
    parser.add_argument("--view", help="xtframe-view executable (default: newest under bin/, then build/)")
    args = parser.parse_args()
    if not args.update:
        parser.print_usage()
        return 2
    view = args.view
    if not view:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from corpus_check import find_tool
        view = find_tool("xtframe-view", None)
    if not view or not os.path.isfile(view):
        print("corpus_fonts: xtframe-view not found (build it, or pass --view)")
        return 2
    names = sorted(d for d in os.listdir(CORPUS) if os.path.isfile(os.path.join(CORPUS, d, "project.json")))
    for p in args.project:
        if p not in names:
            print("corpus_fonts: no corpus project '%s' (have: %s)" % (p, ", ".join(names)))
            return 2
    if args.project:
        names = [n for n in names if n in args.project]
    return 1 if update_projects(os.path.abspath(view), names) else 0


if __name__ == "__main__":
    sys.exit(main())
