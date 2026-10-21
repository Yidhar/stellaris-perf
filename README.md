# stellaris-perf

[English](README.md) | [简体中文](README.zh-CN.md)

A performance plugin for **Stellaris 4.5.2** (Windows x64, the `-dx11` build): a small DLL that is loaded into
`stellaris.exe` and replaces a few slow engine code paths with equivalent, cheaper ones. It needs no mod and does not
touch save files. It is a plugin of the **Stellaris launcher** (plugin spec v2): the launcher installs it, shows its
settings file for editing, checks that it was made for the installed game build, and loads it when it starts the game.
The repository also contains the benchmark and stress-test tooling used to find and check these optimizations.

| Release file | Contents |
|---|---|
| `stellaris-perf-<version>.zip` | **the plugin folder itself**: `stl-plugin.json`, `stellaris_perf.dll`, `defaults\stellaris_perf.ini`, this README |
| `stellaris-perf-bench-<version>.zip` | `stellaris_bench.dll`, the benchmark scripts and the stress-mod generator (see [`bench/`](bench/README.md)); not a plugin |

## Compatibility

- Works with **one exact game build**: the `stellaris.exe` whose PE timestamp is in the manifest (`game.exe_timestamps`)
  and in the release notes. The launcher does not load the plugin into another build, and the DLL checks it again
  against the SDK and installs nothing on a mismatch. After a game patch the SDK subset has to be regenerated and the
  DLL rebuilt (see [Building](#building)).
- Engine addresses are not hard-coded: they come from an SDK generated from the installed executable
  (`sdk/stellaris_sdk.hpp`, a subset written by `tools/extract_sdk.py`).
- Mods are fine: the plugin changes how the engine computes, not what scripts or data say.
- Multiplayer works like a gameplay mod: every player needs the plugin with the same settings (see
  [Multiplayer](#multiplayer)).

## Install and use

1. Download `stellaris-perf-<version>.zip` from the [Releases page](https://github.com/Yidhar/stellaris-perf/releases)
   (the `.sha256` file next to it holds the checksum).
2. Put it where the launcher keeps plugins: unpack the zip into
   `Documents\Paradox Interactive\Stellaris\plugins\stellaris-perf\`
   (the zip has no top folder: `stl-plugin.json` ends up directly in that folder). Or unpack it anywhere and install that
   folder with the launcher: `stl plugin install <folder>`, or **Plugins > Install** in the launcher.
3. Enable the plugin in your playset (`stl plugin enable stellaris-perf`, or the Plugins page).
4. **Start the game with the Stellaris launcher** (`stl launch`, or its Play button). The launcher waits for the game's
   window and loads the plugin. Starting the game from Steam or the Paradox launcher starts it **without** plugins; that
   is how plugins work, there is no stand-in DLL or any other loader.

Updates: the manifest names this repository (`update.github`), so the launcher's Plugins page (or `stl plugin update
stellaris-perf`) offers a newer release, checks its SHA-256 and installs it over the old one, keeping `config\`. The game
has to be closed for that, because its DLL is in use.

The plugin folder is the plugin's only place:

| | |
|---|---|
| `config\stellaris_perf.ini` | the settings; the launcher makes it from `defaults\stellaris_perf.ini` and its Plugins page edits it. The plugin checks the file's modification time every 2 seconds and re-reads it when it changed, so settings can be changed while the game runs. A missing file means the built-in defaults. |
| `logs\stellaris_perf.log` | hooks installed, the settings in effect, counters every 30 seconds and on every change |
| `logs\stellaris_perf_rules.csv` | only with `rule_profile=1` |

Nothing is written into the game folder. An old `stellaris_perf.ini` next to `stellaris.exe` (from before the plugin
had its own folder) is copied into `config\` once if `config\` has no file when the plugin starts, and the game folder
copy is ignored from then on. When the launcher already made `config\stellaris_perf.ini` from the defaults, copy your
old settings over it by hand.

## What it optimizes

Every optimization is a hook (or a 5-byte code patch) that is switched by one `config\stellaris_perf.ini` key. Hooks that
are off pass straight through to the engine.

### On by default: the answer is the engine's own

- **`has_*_flag` scan** (`flag_simd`, default `1`). The engine compares one flag id per loop iteration against an
  unordered array. `1` answers the same question with an SSE2 scan, 8 ids per compare, never reading at or past the
  array end. `2` also runs the engine's own scan and counts any disagreement. Dynamic `flag@scope` names and unreadable
  containers are left to the engine.
- **Daily flag expiry** (`flag_expiry_skip`). The expiry pass touches only flags that have a timer; containers
  without a timed flag are skipped, since the engine would change nothing there.
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

### Optional

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
  `logs\stellaris_perf_rules.csv` every ~30 s.
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
| `flag_simd` | `1` | `0` engine scan, `1` SSE2 scan, `2` verify |
| `flag_expiry_skip` | `1` | skip daily expiry on containers without timed flags |
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

Stellaris multiplayer is lock-step: every client simulates the same game and compares checksums. The plugin is meant to
be used the way a gameplay mod is: **every player in the game runs it, with the same settings**. There is no
multiplayer-specific code in it, and a game where only some players have the plugin is not a supported setup.

What the plugin has to guarantee is that two clients that both run it compute the same results. Every optimization that
is on by default returns the engine's own answer (or an answer that does not depend on which thread or in which order it
was computed), so the simulation stays identical. The per-day caches (`opinion_cache`, `rule_cache`) and the modifier
fast path (`modifier_flush`) are off because their results can depend on which worker thread filled a cache first; they
stay out until that is redesigned. A settings file that differs between players changes the plugin's behaviour on one
client only, so keep the files the same.

## Building

Requirements: Visual Studio 2022 (MSVC, x64) and CMake 3.20+. MinHook is fetched by CMake.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release        # build\Release\stellaris_perf.dll, stellaris_bench.dll
```

The build also assembles the plugin folder, `build\plugin\stellaris-perf` (manifest, DLL, `defaults\`). Install it with
`stl plugin install build\plugin\stellaris-perf`, or work on it in place with `stl plugin install --link
build\plugin\stellaris-perf` (then `config\` and `logs\` are created inside the build folder). `python
tools\check_plugin.py [--dir build\plugin\stellaris-perf]` checks the manifest against the plugin spec and the SDK.

For development, `stl inject build\plugin\stellaris-perf\stellaris_perf.dll` loads a build into the running game by
hand (the DLL then reads `config\` and writes `logs\` in that build folder), and `python bench\scripts\dllctl.py
unload perf` asks the DLL to unload itself (it removes its hooks, waits for calls in flight and frees itself), so a
new build can be loaded without restarting the game. Players do not need either: the launcher loads the plugin.

After a game patch, regenerate the SDK in the
[Stellaris MCP repository](https://github.com/Yidhar/stellaris-mcp) (`python tools/sdk_dumper/dump.py`), then:

```powershell
python tools\extract_sdk.py <path to the full stellaris_sdk.hpp>   # rewrites sdk\stellaris_sdk.hpp
```

and rebuild. Which game build a release was made for is printed in its release notes.

## Releases and CI

Releases: <https://github.com/Yidhar/stellaris-perf/releases>. The latest one is built by CI from its
tag, with both zips and their SHA-256 files attached.

`.github/workflows/build-release.yml` builds both DLLs on every push and pull request, checks the plugin folder with
`tools/check_plugin.py` (manifest schema 2, the exe build matches the SDK, no stray files) and keeps the packaged zips
as workflow artifacts. Pushing a tag `v<version>`, where the version is the one in `plugin/stl-plugin.json` (for
example `git tag v0.2.0 && git push origin v0.2.0`; a different tag fails the check), makes the same workflow publish a
GitHub Release with both zips, their SHA-256 files and release notes that state the game build the DLLs were made for.
A tag with a `-` in it (such as `v0.2.0-rc1`) is published as a pre-release.

## Benchmarks and stress testing

[`bench/`](bench/README.md) holds an automated A/B benchmark (ABBA-ordered runs of two settings in one game
session, with automatic save loading), a generator for a stress-test mod built from load shapes seen in popular
mods (modifier storms, large fleet battles, event-target-heavy triggers, extra AI population), and profiling helpers.

## License

MIT, see [LICENSE](LICENSE).
