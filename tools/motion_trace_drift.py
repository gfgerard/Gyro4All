"""Find slow slides of the view and bias changes in a long motion-trace.csv (MotionTrace=<seconds>), for drift reports.

    python -I tools/motion_trace_drift.py "<game folder>/Gyro4All/motion-trace.csv" [--rate 0.2]

A slide is at least 1 s with the gyro on, no fast motion (raw gyro within 3 deg/s of the bias) and an applied turn over
--rate deg/s. Each is labelled by where its rate comes from:
  bias off    GyroLib's bias differs by over 0.1 deg/s from what the controller reads when GyroLib calls it still
  pad reads   the bias is right, and the raw gyro itself reads above it (real motion, or a sensor offset)
The file is read as a stream into 100 ms bins, so traces of hours fit in memory.
"""
import argparse
import statistics

BIN = 100_000_000


def new_bin():
    # samples, sum x, y, z, peak |x|, |y|, |z|, applied yaw, pitch, frames
    return [0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0]


def load(path):
    bins, biases, marks = {}, [], []
    with open(path, newline="") as f:
        for line in f:
            p = line.rstrip().split(",")
            if p[0] == "S" and len(p) >= 6:
                b = bins.setdefault(int(p[2]) // BIN, new_bin())
                b[0] += 1
                for i in range(3):
                    v = float(p[3 + i])
                    b[1 + i] += v
                    b[4 + i] = max(b[4 + i], abs(v))
            elif p[0] == "F" and len(p) >= 7:
                b = bins.setdefault(int(p[1]) // BIN, new_bin())
                b[7] += float(p[5])
                b[8] += float(p[6])
                b[9] += 1
            elif p[0] == "B" and len(p) >= 7:
                biases.append((int(p[1]), float(p[2]), float(p[3]), float(p[4]), int(p[5]), int(p[6])))
            elif p[0] == "M":
                marks.append(int(p[1]))
    return bins, biases, marks


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--rate", type=float, default=0.2, help="applied deg/s that counts as a slide")
    args = ap.parse_args()
    bins, biases, marks = load(args.path)
    if not bins:
        print("no S or F lines")
        return
    keys = sorted(bins)
    t0 = keys[0]
    sec = lambda ns: ns / 1e9 - t0 * BIN / 1e9
    print(f"{(keys[-1] - t0) / 10:.0f} s, {len(biases)} bias lines, {len(marks)} marks")

    # Bias at a bin, and what the pad reads while GyroLib calls it still (median, per 60 s).
    bias_at, rest = {}, {}
    for t, x, y, z, _, still in biases:
        k = t // BIN
        bias_at[k] = (x, y, z)
        b = bins.get(k)
        if still and b and b[0]:
            rest.setdefault(k // 600, []).append([b[i + 1] / b[0] for i in range(3)])
    rest_at = {w: [statistics.median(r[i] for r in v) for i in range(3)] for w, v in rest.items()}

    print("\nbias changes over 0.05 deg/s (s; from -> to; biggest raw reading in the 2 s before):")
    last = None
    for t, x, y, z, _, _ in biases:
        if last is None:
            last = (x, y, z)
        elif max(abs(x - last[0]), abs(y - last[1]), abs(z - last[2])) > 0.05:
            k = t // BIN
            pk = max((max(bins[j][4:7]) for j in range(k - 20, k + 1) if j in bins), default=0)
            print(f"  {sec(t):8.1f}  ({last[0]:.2f} {last[1]:.2f} {last[2]:.2f}) -> ({x:.2f} {y:.2f} {z:.2f})  peak {pk:.0f} deg/s")
            last = (x, y, z)

    # Slides: 1 s windows stepped by 100 ms, merged. Without B lines (older traces) the bias is the trace's median.
    cur = [statistics.median(bins[k][i + 1] / bins[k][0] for k in keys if bins[k][0]) for i in range(3)]
    slides, run = [], None
    for k in range(keys[0], keys[-1] + 1):
        cur = bias_at.get(k, cur)
        w = [bins.get(j) for j in range(k, k + 10)]
        if not all(b and b[9] and b[0] for b in w):
            continue
        if max(b[4 + i] for b in w for i in range(3)) - max(abs(v) for v in cur) >= 3:
            continue
        if max(abs(sum(b[7] for b in w)), abs(sum(b[8] for b in w))) <= args.rate:
            continue
        if run and k <= run[1] + 1:
            run[1] = k
        else:
            run = [k, k, list(cur)]
            slides.append(run)

    print(f"\nslides over {args.rate} deg/s (start s, length, camera yaw/pitch deg, raw - bias x y z, label, last fast motion):")
    for a, e, bias in slides:
        w = [bins[j] for j in range(a, e + 10) if j in bins]
        n = sum(b[0] for b in w) or 1
        res = [sum(b[i + 1] for b in w) / n - bias[i] for i in range(3)]
        r = rest_at.get(a // 600)
        off = r and max(abs(bias[i] - r[i]) for i in range(3)) > 0.1
        prev = next((j for j in range(a - 1, max(a - 600, keys[0]), -1) if j in bins and max(bins[j][4:7]) > 30), None)
        ago = f"{(a - prev) / 10:.1f} s before" if prev else "none in 60 s"
        print(f"  {sec(a * BIN):8.1f} {(e + 10 - a) / 10:5.1f}s yaw {sum(b[7] for b in w):+6.2f} pitch {sum(b[8] for b in w):+6.2f} | "
              f"{res[0]:+.2f} {res[1]:+.2f} {res[2]:+.2f} | {'bias off' if off else 'pad reads'} | {ago}")

    print("\nmarks (s; applied yaw/pitch in the 10 s before; bias):")
    for t in marks:
        k = t // BIN
        w = [bins[j] for j in range(k - 100, k) if j in bins]
        near = [b for b in biases if b[0] <= t]
        bias = f"({near[-1][1]:.2f} {near[-1][2]:.2f} {near[-1][3]:.2f})" if near else "-"
        print(f"  {sec(t):8.1f}  yaw {sum(b[7] for b in w):+6.2f} pitch {sum(b[8] for b in w):+6.2f}  {bias}")


if __name__ == "__main__":
    main()
