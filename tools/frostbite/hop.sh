#!/usr/bin/env bash
# One WriteWatch hop: beep, set the address in the ini, wait for the 10 s watch, long beep, print the new log lines.
# Usage: hop.sh <WriteWatch value: hex address, x:<address>, r:<address> or r:di+8> <Gyro4All folder of the game (where Gyro4All.ini and Gyro4All.log are)>
ADDR="$1"; I="$2/Gyro4All.ini"; L="$2/Gyro4All.log"
n=$(wc -l < "$L")
python -c "import winsound; winsound.Beep(880,300)"
# Through "on" first: WriteWatch only arms when the value changes (e.g. the same r:di+8 twice in a row).
sed -i "s/^WriteWatch=.*/WriteWatch=on\r/" "$I"; sleep 1
sed -i "s/^WriteWatch=.*/WriteWatch=$ADDR\r/" "$I"
sleep 12
python -c "import winsound; winsound.Beep(300,700)"
tail -n +$((n+1)) "$L" | grep writewatch | cut -c1-420
