# shapes.py: checks the Genome module's code shapes (the byte patterns it reads its offsets from) against a game build,
# offline: after a game patch, or for another store's build, run this before anything else. The patterns are taken from
# the module's source itself (src/engines/genome/genome_engine.cpp and genome_risen1.cpp), so this never drifts from
# the code. Read-only; see README.md.
#
#   uv run --with pefile --with numpy --with capstone python -I tools/genome/shapes.py <exe|dll> [--source engine|risen1|both]
#
# Each pattern is listed with the C++ line it comes from and how often it matches in the binary's code. The shapes the
# module looks for in the whole exe (the camera frame builder, the turn function, the angle call, Risen 3's game mode
# setter, ELEX II's aim mode: the first pattern of each kFrameShapes / kTurnShapes / kAngleShapes entry and the k...Pattern
# constants marked "whole exe" below) must match exactly once for the game they are written for; the others are looked
# for inside one function (a few bytes after a match, or in an export) and may match elsewhere too. Risen's patterns
# (genome_risen1.cpp) are for Game.dll, the others for the exe. Steam's DRM (Risen 3: a .bind section) encrypts the exe on
# disk: dump the running game's image first (pe.py <exe name> dump <out.exe>) and check that.
import argparse, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe import PE

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "src", "engines", "genome")
SOURCES = {"engine": "genome_engine.cpp", "risen1": "genome_risen1.cpp"}
# Patterns the module looks for in the whole exe (find_code / mem::find_pattern over the code sections).
WHOLE = ("kGameModeSetterPattern", "kAimModePattern")


def patterns(path):
    """[(label, pattern, whole)] from a C++ file: runs of adjacent string literals that look like byte patterns."""
    text = open(path, encoding="utf-8").read()
    lines = text.splitlines()
    out = []
    lit = re.compile(r'"((?:[0-9A-F?]{2} )*[0-9A-F?]{2} ?)"')
    run = re.compile(r'(?:"(?:[0-9A-F?]{2} )*[0-9A-F?]{2} ?"\s*)+')
    # the constants' initializers: constexpr ... kName[] = ... ; (braces counted)
    spans = []
    for d in re.finditer(r"constexpr[^;=\n]*?\b(k\w+)\s*(?:\[\])?\s*=", text):
        depth, i = 0, d.end()
        while i < len(text):
            c = text[i]
            if c == "{": depth += 1
            elif c == "}": depth -= 1
            elif c == ";" and depth <= 0: break
            i += 1
        spans.append((d.start(), i, d.group(1)))
    for m in run.finditer(text):
        in_shapes = next((n for s, e, n in spans if s <= m.start() < e), "inline")
        pat = " ".join(" ".join(lit.findall(m.group(0))).split())
        if len(pat.split()) < 5: continue
        line_no = text.count("\n", 0, m.start())
        # the label: the code before the literal on its line, else the nearest code line above
        head = lines[line_no][: m.start() - (text.rfind("\n", 0, m.start()) + 1)].strip()
        k = line_no
        while (not head or head.startswith("//")) and k > 0:
            k -= 1
            head = lines[k].strip()
        first_in_entry = head.endswith(",") and head.startswith("{") or head.startswith('{"')
        whole = in_shapes in WHOLE or (in_shapes in ("kFrameShapes", "kTurnShapes", "kAngleShapes") and first_in_entry)
        out.append((f"{in_shapes}: {head[:70]}", pat, whole))
    return out


def main():
    ap = argparse.ArgumentParser(description="Check the Genome module's code shapes against a game build.")
    ap.add_argument("binary")
    ap.add_argument("--source", choices=("engine", "risen1", "both"), default="both")
    a = ap.parse_args()
    g = PE(a.binary)
    code = [(va, va + sz) for _, va, sz, ch in g.secs if ch & 0x20000000]
    if any(n == ".bind" for n, *_ in g.secs):
        print("note: a .bind section (Steam DRM): the code is encrypted on disk; check a dump of the running game instead")
    for key in (("engine", "risen1") if a.source == "both" else (a.source,)):
        print(f"== {SOURCES[key]}")
        for label, pat, whole in patterns(os.path.join(ROOT, SOURCES[key])):
            rx = re.compile(b"".join(b"." if t == "??" else re.escape(bytes([int(t, 16)])) for t in pat.split()), re.S)
            hits = [m.start() for lo, hi in code for m in rx.finditer(g.img, lo, hi)]
            verdict = ("ok" if len(hits) == 1 else "NOT FOUND" if not hits else f"{len(hits)} matches") if whole else "local"
            shown = ", ".join(f"{h:#x}" for h in hits[:4]) + (" ..." if len(hits) > 4 else "")
            print(f"  [{verdict:>10}] {len(hits):5d}  {label}\n                    {pat[:72]}{'...' if len(pat) > 72 else ''}"
                  + (f"\n                    at {shown}" if hits else ""))


if __name__ == "__main__":
    main()
