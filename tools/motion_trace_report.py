"""Summarise Gyro4All's motion-trace.csv (MotionTrace=1): where gyro samples get bunched and how smooth the
rotation applied per frame is.

    python -I tools/motion_trace_report.py "<Win64>/Gyro4All/motion-trace.csv"
"""
import csv
import statistics
import sys
from collections import Counter


def pct(values, p):
    values = sorted(values)
    return values[min(len(values) - 1, int(p / 100 * len(values)))] if values else 0


def describe(name, values, unit="ms"):
    if not values:
        print(f"  {name}: none")
        return
    print(f"  {name}: n={len(values)} mean={statistics.mean(values):.2f} median={statistics.median(values):.2f} "
          f"p10={pct(values, 10):.2f} p90={pct(values, 90):.2f} max={max(values):.2f} {unit}")


def smoothness(frames, idx):
    """Coefficient of variation of rotation speed (deg/s) over frames while turning: lower = smoother."""
    speeds = []
    for prev, cur in zip(frames, frames[1:]):
        dt = (cur[0] - prev[0]) / 1e9
        if dt <= 0 or dt > 0.1:
            continue
        speeds.append(abs(cur[idx]) / dt)
    moving = [v for v in speeds if v > 0] if speeds else []
    if len(speeds) < 10:
        return None
    # Judge only stretches where the player is clearly turning (median speed of a 9-frame window > 10 deg/s).
    cvs = []
    for i in range(0, len(speeds) - 9, 9):
        window = speeds[i:i + 9]
        m = statistics.mean(window)
        if m > 10:
            cvs.append(statistics.pstdev(window) / m)
    zero_share = 1 - len(moving) / len(speeds)
    return (statistics.mean(cvs) if cvs else None), zero_share, len(cvs)


def main(path):
    samples, frames = [], []
    with open(path, newline="") as f:
        for row in csv.reader(f):
            if row[0] == "S":
                samples.append((int(row[1]), int(row[2])))
            elif row[0] == "F":
                # time, new samples, raw GyroLib yaw, applied yaw (pitch is left out: yaw dominates turning)
                frames.append((int(row[1]), int(row[2]), float(row[3]), float(row[5])))
    if not frames:
        print("no frames recorded")
        return
    span = (frames[-1][0] - frames[0][0]) / 1e9
    print(f"{path}\n  {len(samples)} samples, {len(frames)} frames over {span:.1f} s "
          f"({len(samples) / span:.0f} samples/s, {len(frames) / span:.0f} frames/s)")

    sensor_gaps = [(b[0] - a[0]) / 1e6 for a, b in zip(samples, samples[1:]) if b[0] > a[0]]
    arrivals = sorted({a for _, a in samples})
    arrival_gaps = [(b - a) / 1e6 for a, b in zip(arrivals, arrivals[1:])]
    per_arrival = Counter(a for _, a in samples)
    describe("sensor interval between samples", sensor_gaps)
    describe("interval between arrivals (distinct arrival times)", arrival_gaps)
    print(f"  samples per arrival: {dict(sorted(Counter(per_arrival.values()).items()))}")
    frame_gaps = [(b[0] - a[0]) / 1e6 for a, b in zip(frames, frames[1:])]
    describe("frame interval", frame_gaps)
    print(f"  new samples per frame: {dict(sorted(Counter(fr[1] for fr in frames).items()))}")

    for label, idx in (("GyroLib output yaw", 2), ("applied yaw", 3)):
        result = smoothness(frames, idx)
        if result is None:
            print(f"  {label}: not enough frames")
            continue
        cv, zero_share, windows = result
        print(f"  {label}: frames with no rotation {zero_share:.0%}; speed variation while turning "
              f"{'n/a' if cv is None else f'{cv:.2f}'} (CV over {windows} windows; lower is smoother)")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "motion-trace.csv")
