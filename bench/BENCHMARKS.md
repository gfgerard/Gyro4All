# Gyro4All performance and benchmarks

Everything about Gyro4All's performance cost: tools, method, results and optimisation analysis.
Project context: `../AGENTS.md`; functional history: `../MODLOG.md`.

**Status (2026-10-08): closed.** No measurable FPS/frametime cost in Returnal (GPU-bound); ~110 µs of game
thread per rendered frame. No optimisation work is planned. Reopen only if a CPU-bound game shows a cost, starting
with the "Pending" item under *Optimisation analysis*. Returnal was left in `mod` mode (`version.dll` active,
`ui.menu_key=F10`).

## Tools

- `tools/bench.sh mode <vanilla|mod|mod-nopanel> "<Win64 dir>"` switches the setup (game closed):
  - `vanilla` renames the Gyro4All proxy (`winmm.dll` or `version.dll`) to `.off`, so Gyro4All isn't loaded and settings are kept;
  - `mod` is Gyro4All with the F10 panel hooks (`ui.menu_key=F10`);
  - `mod-nopanel` is Gyro4All with no graphics hooks (`ui.menu_key=` empty, `ui.gamepad_menu_shortcut=0`).
- `tools/bench.sh capture <label> <process.exe>` starts PresentMon 2.6 (`--qpc_time --v1_metrics`) for the whole
  session, plus `tools/bench_marker.py`. The marker records the QPC tick of each global **Ctrl+Shift+F9**
  press (high beep = start, low beep = stop).
  - PresentMon's own `--hotkey` doesn't work when it runs in the background.
  - `--terminate_on_proc_exit` needs elevation, so stop PresentMon by hand after the game exits.
- `tools/bench.sh report` (`tools/bench_report.py`) prints a table per capture, keeping only the frames between
  each mark pair: avg FPS, 1% / 0.1% lows, avg / p99 / p99.9 / max frametime, and frames > 2x median.
- Gyro4All self-timing (v0.3.6): every 10 s `Gyro4All.log` gets a `perf:` line with count / avg / p99 / max in µs:
  - `idle`: game-thread work in frames where the gyro didn't move the camera;
  - `active`: frames where it applied a rotation;
  - `present`: the Present hook on the render thread.

## Returnal, 2026-10-08 (GyroUE v0.3.5 / v0.3.6)

Setup:
- Returnal's built-in benchmark, the same ~59 s window marked in every run;
- V-Sync off (`SyncInterval 0`), tearing allowed, Hardware Independent Flip, uncapped;
- RTX 4090, ReShade + RenoDX + SCSKiller present in all runs.

| Run | avg FPS | 1% low | 0.1% low | p99 ms | p99.9 ms | max ms | >2x median |
|---|---|---|---|---|---|---|---|
| 1 mod (panel hooks) | 96.6 | 62.5 | 47.6 | 14.38 | 18.78 | 24.65 | 2 |
| 2 vanilla | 96.3 | 61.3 | 46.2 | 14.38 | 19.97 | 22.27 | 5 |
| 3 mod-nopanel | 96.4 | 59.1 | 42.8 | 14.65 | 19.81 | 31.51 | 4 |

**No measurable difference.** The spread is run-to-run noise: run 3 does the least work and has the worst lows.
During the benchmark there is no player (`view: none`), so sensor reading and GyroLib processing ran every
frame, but no camera writes happened.

Gyro4All self-timing:

| | game thread per hook call | render thread |
|---|---|---|
| mod | ~52 µs avg / ~170 µs p99 | Present hook ~8 µs (panel closed) |
| mod-nopanel | ~38 µs | — |

- The panel's per-frame checks (F-key poll + `gl_overlay_process`) cost ~14 µs on the game thread.
- One-time spikes: ~175 ms on the first frame (GyroLib/SDL load + engine scan) and 5-30 ms around level loads.

In-game, active vs idle (v0.3.6; gyro on L2/R2 hold, alternating aimed and unaimed play, 20:56-20:57):

| Frames | Count | avg | p99 |
|---|---|---|---|
| idle | 8751 | 52.3 µs | 127-216 µs |
| active | 6727 | 56.1 µs | 132-247 µs |

Applying the rotation adds ~4 µs.

**The hook runs about twice per rendered frame.** In 10 s there are about 2200 PeekMessageW-empty calls but
about 1100 Presents, because Unreal pumps messages twice per frame (engine loop + Slate). So the real cost is
about 110 µs of game thread per rendered frame (~1% at 110 FPS). That's invisible while GPU-bound, as here.

## Optimisation analysis

**Rejected: run the gyro update only once per rendered frame.**
- Gain: about 55 µs of game-thread time per frame (0.3-1.3% between 60 and 240 FPS). It's only visible in games
  bound by the main thread; Returnal and Pacific Drive on this PC are GPU-bound.
- Risk, latency: one pump is before the world tick (a write there is used in the same frame) and one is after
  (Slate; a write there is seen next frame).
  - Keeping only the pre-tick pump changes nothing.
  - Keeping only the post-tick one adds up to a frame of gyro latency (~9 ms at 110 FPS).
  - From PeekMessageW we can't reliably tell which pump is which: it varies by engine version and game, and
    Present runs 1-2 frames behind on another thread. Any time or Present-sync rule could silently add latency
    in some game.
- GyroLib itself is fine with fewer calls (its filters are time-based).

**Pending (no behaviour change), only if a CPU-bound game needs it:** make each call cheaper.
- `ue::prop_of` / `find_prop` build a `std::string` key for every lookup (~10 per call). Key the cache by
  (class pointer, `const char*` name pointer) with no allocation.
- Cache the resolved object chain/offsets per class.
- First add a breakdown to the perf line (SDL/GyroLib vs engine reads) to see what dominates.

## Raw data

`bench/returnal-1-mod.*`, `returnal-2-vanilla.*`, `returnal-3-mod-nopanel.*`: PresentMon CSV + QPC marks.
