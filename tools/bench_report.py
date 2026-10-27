"""Summarise PresentMon (--v1_metrics, --qpc_time) captures in a folder: one row per capture.
If <label>.marks exists (QPC ticks from bench_marker.py), only frames between mark pairs
(start, stop) are kept; otherwise the whole capture is used."""
import csv, glob, os, statistics, sys

TIME_COLUMNS = ("QPCTime", "TimeInQPC", "CPUStartQPC", "TimeInSeconds")


def load(path):
    with open(path, newline="", encoding="utf-8-sig") as f:
        reader = csv.DictReader(f)
        cols = reader.fieldnames or []
        tcol = next((c for c in TIME_COLUMNS if c in cols), None)
        ft_col = next((c for c in ("msBetweenPresents", "MsBetweenPresents") if c in cols), None)
        rows = []
        for r in reader:
            try:
                rows.append((float(r[tcol]) if tcol else 0.0, float(r[ft_col])))
            except (TypeError, ValueError, KeyError):
                pass
        return tcol, rows


def windows(marks_path):
    if not os.path.exists(marks_path):
        return None
    ticks = [int(x) for x in open(marks_path).read().split()]
    return [(ticks[i], ticks[i + 1]) for i in range(0, len(ticks) - 1, 2)]


def pct(s, p):
    return s[min(len(s) - 1, int(len(s) * p))]


def row(name, ft):
    s = sorted(ft)
    n = len(s)
    w1, w01 = s[-max(1, n // 100):], s[-max(1, n // 1000):]
    med = statistics.median(s)
    return (f"{name[:38]:38} {n:7d} {sum(ft) / 1000:6.1f} {1000 / statistics.mean(ft):7.1f} "
            f"{1000 / statistics.mean(w1):7.1f} {1000 / statistics.mean(w01):8.1f} {statistics.mean(ft):7.2f} "
            f"{pct(s, .99):7.2f} {pct(s, .999):7.2f} {s[-1]:7.2f} {sum(1 for x in ft if x > 2 * med):6d}")


def main(folder):
    files = sorted(glob.glob(os.path.join(folder, "*.csv")))
    if not files:
        print("no CSV files in", folder)
        return
    print(f"{'capture [window]':38} {'frames':>7} {'secs':>6} {'avgFPS':>7} {'1%low':>7} {'0.1%low':>8} "
          f"{'avg ms':>7} {'p99 ms':>7} {'p99.9':>7} {'max ms':>7} {'>2xmed':>6}")
    for path in files:
        label = os.path.splitext(os.path.basename(path))[0]
        tcol, rows = load(path)
        wins = windows(os.path.join(folder, label + ".marks"))
        if wins and tcol and tcol != "TimeInSeconds":
            for i, (a, b) in enumerate(wins, 1):
                ft = [f for t, f in rows if a <= t <= b]
                print(row(f"{label} [{i}]", ft) if len(ft) >= 100 else f"{label} [{i}]: too few frames ({len(ft)})")
        else:
            ft = [f for _, f in rows][1:]
            note = " (no marks: whole capture)" if not wins else f" (marks ignored: time column {tcol})"
            print(row(label + note, ft) if len(ft) >= 100 else f"{label}: too few frames ({len(ft)})")


if __name__ == "__main__":
    main(sys.argv[1])
