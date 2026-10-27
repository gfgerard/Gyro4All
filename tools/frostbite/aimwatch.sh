#!/usr/bin/env bash
# Dead Space: read-watch the active camera's rotation interface while aiming (WriteWatch=r:cam+<offset>: Gyro4All's
# Frostbite module resolves "cam" to the newest camera behaviour's interface and the breakpoint follows it when the game
# replaces it), to find who reads the aim yaw delta (+0x2C4). Protocol: double low beep = get ready (5 s); one high beep
# = press L2 and hold it to the end, stick still; the hop's beep (~2 s later) = move the stick left and right; long low
# beep = done.
# Usage: aimwatch.sh <Gyro4All folder of the game> [offset hex, default 2C4]
D="$1"; OFF="${2:-2C4}"; HERE="$(dirname "$0")"
python -c "import winsound,time; winsound.Beep(330,250); time.sleep(0.1); winsound.Beep(330,250)"
sleep 5
python -c "import winsound; winsound.Beep(1320,200)"
sleep 2
bash "$HERE/hop.sh" "r:cam+$OFF" "$D"
