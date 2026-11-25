# phases.py: runs synthetic-input phases on a Genome test build (test-switches.patch applied, never shipped): for each
# phase it writes the test switches into Gyro4All.ini (the DLL reads them on save), waits, and at the end prints the DLL's
# per-phase stats from Gyro4All.log (heading turn per frame, elevation, what was sent). The player only stands still in
# gameplay: one high beep at the start, three falling beeps at the end. See README.md.
#
#   python -I tools/genome/phases.py <exe> "<folder with Gyro4All\>" [PHASE ...]
#   PHASE: label:seconds[:KEY=VALUE,...], e.g. "gyro right 50:3:GenomeTestYaw=50" (default: the standard set below)
# Switches: GenomeTestYaw / GenomeTestPitch (millidegrees a frame), GenomeTestStickX (right stick X, -32768..32767),
# GenomeForceNoCamera=1 (the unknown-build fallback: gyro as right stick), ELEX family: GenomeTestNoise (millidegrees,
# random), GenomeSpuriousFix=0 (the spurious-turn filter off).
import os, re, sys, time, winsound
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "camtrace"))
import camtrace as ct

KEYS = ("GenomeTestYaw", "GenomeTestPitch", "GenomeTestStickX", "GenomeTestNoise", "GenomeSpuriousFix",
        "GenomeForceNoCamera")
STANDARD = ["baseline:4", "gyro right 5:4:GenomeTestYaw=5", "gyro left 5:4:GenomeTestYaw=-5",
            "gyro right 50:3:GenomeTestYaw=50", "gyro left 50:3:GenomeTestYaw=-50", "pitch up 50:3:GenomeTestPitch=50",
            "pitch down 50:3:GenomeTestPitch=-50", "stick right:3:GenomeTestStickX=12000",
            "stick right + gyro left 30:3:GenomeTestStickX=12000,GenomeTestYaw=-30", "rest:4"]


def mark(t): print(f"{time.strftime('%H:%M:%S')} {t}", flush=True)


def set_switches(ini, values):
    text = open(ini, "rb").read().decode("utf-8", "replace")
    for k in KEYS: text = re.sub(r"\r?\n" + k + r"=-?\d+", "", text)
    add = "".join(f"\r\n{k}={v}" for k, v in values.items())
    open(ini, "wb").write((text.rstrip("\r\n") + add + "\r\n").encode("utf-8"))


def main():
    if len(sys.argv) < 3: sys.exit(__doc__ if __doc__ else "usage: phases.py <exe> <game folder> [PHASE ...]")
    exe, folder = sys.argv[1], sys.argv[2]
    ini, log = os.path.join(folder, "Gyro4All", "Gyro4All.ini"), os.path.join(folder, "Gyro4All", "Gyro4All.log")
    phases = []
    for spec in sys.argv[3:] or STANDARD:
        label, secs, *rest = spec.split(":")
        values = dict(kv.split("=") for kv in rest[0].split(",")) if rest and rest[0] else {}
        phases.append((label, float(secs), values))
    if not os.path.exists(ini): sys.exit(f"{ini} not found (start the game with Gyro4All once)")
    p = ct.Proc(exe)
    if not p.wait_front(timeout=900): sys.exit("the game never came to the front")
    start = os.path.getsize(log) if os.path.exists(log) else 0
    winsound.Beep(1400, 300)
    try:
        for label, secs, values in phases:
            set_switches(ini, values)
            mark(f"{label} ({secs:g} s) {values or ''}")
            time.sleep(secs)
    finally:
        set_switches(ini, {})
    for fr in (1200, 1000, 800): winsound.Beep(fr, 150); time.sleep(0.1)
    time.sleep(1.0)
    text = open(log, encoding="utf-8", errors="replace").read()[start:]
    stats = [l for l in text.splitlines() if "TEST phase stats" in l]
    print(f"\n{len(stats)} phase stats from the log (one per switch change; the first covers the time before phase 1):")
    for (label, _, _), line in zip([("(before)", 0, {})] + phases, stats):
        print(f"  {label:30s} {line.split('TEST phase stats: ', 1)[-1]}")


if __name__ == "__main__":
    main()
