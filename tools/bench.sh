#!/usr/bin/env bash
# A/B benchmark of Gyro4All with PresentMon (frametimes) + Gyro4All's own perf log lines.
#   bash tools/bench.sh mode <vanilla|mod|mod-nopanel> <Win64 dir>   switch setup (game closed)
#   bash tools/bench.sh capture <label> <process.exe>                record; Ctrl+Shift+F9 marks start/stop
#   bash tools/bench.sh report [dir]                                 summary table of bench/*.csv
# Modes: vanilla = the Gyro4All proxy (winmm.dll or version.dll) renamed to .off (Gyro4All not loaded, settings kept)
#        mod = Gyro4All with the F10 panel hooks; mod-nopanel = Gyro4All without any graphics hooks.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# PresentMon console app: $PRESENTMON if set, else the newest one from the default Intel PresentMon install.
PM="${PRESENTMON:-$(ls -1 "/c/Program Files/Intel/PresentMon/PresentMonConsoleApplication/"PresentMon-*-x64.exe 2>/dev/null | sort -V | tail -1)}"
cmd="${1:?mode|capture|report}"; shift
case "$cmd" in
mode)
  m="${1:?vanilla|mod|mod-nopanel}"; W="${2:?Win64 dir}"; ini="$W/Gyro4All/gyrolib.ini"
  case "$m" in
    vanilla) for p in winmm.dll version.dll; do
               if grep -aqsE '(Gyro4All|GyroUE) [0-9]+\.[0-9]+\.[0-9]+ in ' "$W/$p"; then mv "$W/$p" "$W/$p.off"; fi
             done ;;
    mod|mod-nopanel)
      for p in winmm.dll version.dll; do if [ -f "$W/$p.off" ]; then mv "$W/$p.off" "$W/$p"; fi; done
      if [ "$m" = mod ]; then sed -i 's/^ui\.menu_key=.*/ui.menu_key=F10/' "$ini"
      else sed -i 's/^ui\.menu_key=.*/ui.menu_key=/; s/^ui\.gamepad_menu_shortcut=.*/ui.gamepad_menu_shortcut=0/' "$ini"; fi ;;
    *) echo "unknown mode $m"; exit 1 ;;
  esac
  echo "mode $m: $(ls "$W" | grep -E '^(winmm|version)\.dll' | tr '\n' ' ')$(grep -E '^ui\.(menu_key|gamepad)' "$ini" 2>/dev/null | tr '\n' ' ')" ;;
capture)
  [ -n "$PM" ] && [ -f "$PM" ] || { echo "PresentMon not found: install it or set PRESENTMON=<path to PresentMon-*-x64.exe>"; exit 1; }
  # PresentMon records the whole session with QPC timestamps; bench_marker.py stores the QPC of each
  # Ctrl+Shift+F9 press (beep: high = start, low = stop). The report keeps only frames between marks.
  label="${1:?label}"; proc="${2:?process exe}"; out="$ROOT/bench/$label.csv"; marks="$ROOT/bench/$label.marks"
  stop="$ROOT/bench/.stop-$label"; rm -f "$stop" "$marks"
  python -I "$ROOT/tools/bench_marker.py" "$(cygpath -w "$marks")" "$(cygpath -w "$stop")" &
  echo "recording $proc until it exits; press CTRL+SHIFT+F9 at benchmark start and again at the end"
  "$PM" --process_name "$proc" --qpc_time --v1_metrics --no_console_stats --terminate_on_proc_exit         --stop_existing_session --session_name GyroBench --output_file "$(cygpath -w "$out")" || true
  touch "$stop"; wait
  echo "marks: $(wc -l < "$marks" 2>/dev/null || echo 0)" ;;
report)
  python -I "$ROOT/tools/bench_report.py" "${1:-$ROOT/bench}" ;;
*) echo "unknown command $cmd"; exit 1 ;;
esac
