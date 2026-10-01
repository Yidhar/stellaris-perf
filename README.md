# stellaris-perf

[English](README.md) | [简体中文](README.zh-CN.md)

A performance plugin for **Stellaris 4.5.1** (Windows x64, the `-dx11` build): a small DLL that is loaded into
`stellaris.exe` and replaces a few slow engine code paths with equivalent, cheaper ones. It needs no mod, does
not touch save files, and does not depend on any other tool. The repository also contains the benchmark and
stress-test tooling used to find and check these optimizations.

| Release file | Contents |
|---|---|
| `stellaris-perf-<version>.zip` | `stellaris_perf.dll`, `scripts/perfctl.py` (load / unload), this README |
| `stellaris-perf-bench-<version>.zip` | `stellaris_bench.dll`, the benchmark scripts and the stress-mod generator (see [`bench/`](bench/README.md)) |

## Compatibility

- Works with **one exact game build**: the `stellaris.exe` whose PE timestamp is printed in the release notes
  (checked against the SDK at load time). With any other build the DLL logs the mismatch and installs nothing.
  After a game patch the SDK subset has to be regenerated and the DLL rebuilt (see [Building](#building)).
- Engine addresses are not hard-coded: they come from an SDK generated from the installed executable
  (`sdk/stellaris_sdk.hpp`, a subset written by `tools/extract_sdk.py`).
- Mods are fine: the plugin changes how the engine computes, not what scripts or data say.
- Single player is the tested case. For multiplayer see [Multiplayer](#multiplayer).

## Install and use

1. Download `stellaris-perf-<version>.zip` from the Releases page and unpack it anywhere.
2. Start Stellaris (a save may or may not be loaded) and run
   `python scripts\perfctl.py load`
   (Python 3.8+, no packages). To inject automatically at launch, run `python scripts\perfctl.py load --wait`
   first and then start the game.
3. Settings live in `stellaris_perf.ini` next to `stellaris.exe`. The file is created with defaults on first run and
   re-read every 2 seconds, so settings can be changed while the game runs.
4. `python scripts\perfctl.py unload` removes the hooks and unloads the DLL; `status` shows whether it is loaded.

The injection lasts for that game session only. The plugin writes just two files, both next to `stellaris.exe`:
`stellaris_perf.ini` and `stellaris_perf.log` (hooks installed, settings, counters every 30 seconds).

## What it optimizes

Every optimization is a hook (or a 5-byte code patch) that is switched by one `stellaris_perf.ini` key. Hooks that
are off pass straight through to the engine.

### On by default: the simulation result is unchanged

- **Fleet update, one fleet per work chunk** (`fleet_parallel_grain1`). The engine's per-micro-tick fleet and ship
  update divides the fleets over its worker threads in chunks of `fleets / tasks / 3`. With a few very large fleets
  one chunk holds the big fleet while the other threads sit idle. A 5-byte patch of the chunk-size clamp makes every
  fleet its own chunk. This phase runs with random numbers disabled and fleets are independent of each other, so which
  thread updates which fleet cannot change a result (the engine's own chunking already varies with the thread count).
  The original bytes are restored when the key is turned off.
- **Fleet manager window: military power cache** (`fleet_manager_cache`). The template list recomputes every ship's
  full stats each frame only to print a fleet's power. The result is cached inside `CFleetManagerView::Update` alone
  (the AI and the simulation call the same engine function and are not touched), for at most one second of real time
  and never across a game day, keyed also by the fleet's ship array and ship count so a merge or split shows at once.
- **Fleet manager window: reinforcement recalculation throttle** (`fleet_manager_reinforce_ms`, default 1000). With
  the window open the engine reruns the "which ships need reinforcement" pass for 10 ms of every 80 ms whether or not
  anything changed; the plugin lets it run at most once per interval. Display only.

### Optional, exact: same answer as the engine

- **`has_*_flag` scan** (`flag_simd`). The engine compares one flag id per loop iteration against an unordered array.
  `1` answers the same question with an SSE2 scan, 8 ids per compare, never reading at or past the array end. `2` also
  runs the engine's own scan and counts any disagreement. Dynamic `flag@scope` names and unreadable containers are left
  to the engine.
- **Daily flag expiry** (`flag_expiry_skip`). The expiry pass touches only flags that have a timer; containers
  without a timed flag are skipped, since the engine would change nothing there.
- **Frame smoothing** (`frame_smoothing`). Sets the engine's own `g_bFrameSmoothing` (the `smooth` console command):
  with it on, the game renders frames in the middle of a tick (smoother UI, slower simulation). `-1` leaves the game's
  setting alone.

### Optional, approximations: per game day caches (off)

These memoize an engine computation for one game day. They return a stale value if the underlying state changes
within that day, so they are not the engine's exact behaviour and are off by default.

- **Opinion cache** (`opinion_cache`): `CCountry::CalcOurOpinionOfOther` without a reason string.
- **Game rule cache** (`rule_cache`): every scripted game rule (`CScriptedRule::Evaluate`), keyed by the rule and the
  scope's this / from / root / prev objects. Calls that ask for a reason, and scopes that carry parameters, bypass it.
  - `1`: cache every rule.
  - `2`: **adaptive**. Every ~30 s a short learning window sends all rules through the cache; each rule is then judged on
    what was measured, its expected cost with the cache (hit rate × hit cost + miss rate × miss cost) against the
    engine's own evaluation cost, and the cache is used only for rules where it is clearly cheaper (switched on below
    85 % of the evaluation cost, off again above 95 %).
  - The table is sized to a quarter of one core's L2 cache, read from the CPU at load time, so it stays cache-resident
    on any processor instead of being tuned to one machine.

### Experimental (off)

- **Modifier graph flush fast path** (`modifier_flush`, `modifier_flush_max`). The engine flushes its modifier node
  graph after every effect block and command with a roughly 100-task parallel job, even when one node is dirty. This
  rebuilds a small, fully known dirty set serially on the main thread. It follows the engine's category order, and
  mode `2` verifies the result node by node against the engine's own flush. Not recommended for play.

### Diagnostics

- `profile` times every call of the hooked functions whose cache is off, split into the main thread and the other
  threads, per game day (written to the log).
- `rule_profile` writes calls and cycles per game rule and per path (engine, bypass, hit, miss) to
  `stellaris_perf_rules.csv` every ~30 s.
- `scope_profile` times event-target resolution (`CEventTarget::GetScope`, dynamic `name@target` flags, scope copies).
- The current settings and counters are also published in a read-only shared-memory block
  (`Local\stellaris_perf_stats_<pid>`, layout in `perf/include/perf_shared.hpp`) that the benchmark scripts read.

### Investigated, not shipped

Measured with the stress-test tooling and left alone because they did not pay off: event-target and dynamic-name
lookup (the profiler stays as `scope_profile`), month-start pop job assignment and migration, and memoizing modifier
clones.

## Settings reference

| Key | Default | Meaning |
|---|---|---|
| `frame_smoothing` | `-1` | `-1` leave the game's setting, `0` off, `1` on |
| `fleet_parallel_grain1` | `1` | one fleet per parallel work chunk |
| `fleet_manager_cache` | `1` | cache fleet military power in the fleet manager window |
| `fleet_manager_reinforce_ms` | `1000` | minimum ms between reinforcement recalculations (`0` = the game's own throttle) |
| `flag_simd` | `0` | `0` engine scan, `1` SSE2 scan, `2` verify |
| `flag_expiry_skip` | `0` | skip daily expiry on containers without timed flags |
| `opinion_cache` | `0` | per-game-day opinion cache |
| `rule_cache` | `0` | `0` off, `1` every rule, `2` adaptive |
| `modifier_flush` | `0` | `0` engine, `1` serial fast path, `2` verify, `3` control |
| `modifier_flush_max` | `32` | largest dirty set that takes the serial path |
| `profile`, `rule_profile`, `scope_profile` | `0` | diagnostics |

## How it is built to be safe

- Inline hooks use [MinHook](https://github.com/TsudaKageyu/minhook); only the game's own threads ever run engine code.
  There is no pipe, no console access and no cross-thread call into the engine.
- Raw memory reads sit behind SEH guards, so a bad pointer is skipped (and counted) instead of crashing the game.
- Per-thread cache tables, with counters kept per thread and published in batches, so the bookkeeping does not
  become the cost it is trying to remove.
- Unloading is done by the DLL itself: the hooks are removed, in-flight detours are drained, then the DLL frees itself.
  Nothing calls `FreeLibrary` from outside while a game thread may still be inside a detour.

## Multiplayer

Stellaris multiplayer is lock-step: every client simulates the same game and compares checksums. The default
settings are designed not to change any result, but they have **not been tested in a multiplayer session**. The
approximation caches and the modifier fast path can change results; if you try them, every player must use
identical settings, or an out-of-sync error is likely.

## Building

Requirements: Visual Studio 2022 (MSVC, x64) and CMake 3.20+. MinHook is fetched by CMake.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release        # build\Release\stellaris_perf.dll, stellaris_bench.dll
```

After a game patch, regenerate the SDK in the
[Stellaris MCP repository](https://github.com/Yidhar/stellaris-mcp) (`python tools/sdk_dumper/dump.py`), then:

```powershell
python tools\extract_sdk.py <path to the full stellaris_sdk.hpp>   # rewrites sdk\stellaris_sdk.hpp
```

and rebuild. Which game build a release was made for is printed in its release notes.

## Releases and CI

`.github/workflows/build-release.yml` builds both DLLs on every push and pull request and keeps the packaged zips as
workflow artifacts. Pushing a tag such as `v0.1.0` also publishes a GitHub Release with both zips and their SHA-256
files.

## Benchmarks and stress testing

[`bench/`](bench/README.md) holds an automated A/B benchmark (ABBA-ordered runs of two settings in one game
session, with automatic save loading), a generator for a stress-test mod built from load shapes seen in popular
mods (modifier storms, large fleet battles, event-target-heavy triggers, extra AI population), and profiling helpers.

## License

MIT, see [LICENSE](LICENSE).
