# bench: benchmarks and stress testing

[English](README.md) | [简体中文](README.zh-CN.md)

Automated performance measurement for `stellaris_perf.dll`. Nothing here is needed to use the plugin.

| Part | What it does |
|---|---|
| `stellaris_bench.dll` | Patches the `IDXGISwapChain::Present` slot in DXGI's swap chain vtable to count frames, and runs one-line commands on the game's main thread at the next frame. Own pipe `\\.\pipe\stellaris_bench`; commands `status`, `pause 0/1`, `speed 0..5`. Engine addresses come from the generated SDK. |
| Stats of `stellaris_perf.dll` | Read-only shared memory `Local\stellaris_perf_stats_<pid>` (`perf/include/perf_shared.hpp`): the settings in effect, `CFleetManagerView::Update` call counts, cache and throttle counters. The plugin itself has no control channel; settings only come from `stellaris_perf.ini`. |
| `scripts/dllctl.py` | Loads or unloads both DLLs (remote `LoadLibraryW`; unloading asks the DLL to unload itself, never `FreeLibrary` from outside). |
| `scripts/bench_fm.py` | A/B in one continuous run, no save reloads needed. |
| `scripts/game_session.py` | Automatic save loading: restarts the game on a chosen save, loads the DLLs, pauses, opens the fleet manager. |
| `scripts/rule_report.py` | Names the rules in `stellaris_perf_rules.csv` (`rule_profile=1`) from live game memory. |
| `stress_mod/gen_stress_mod.py` | Generates the stress-test mod (below). |
| `tools/` | WPR profile and report script for hardware-counter (cache miss) sampling. |
| `results/` | One JSON per benchmark run (created on first run, not tracked). |

Why a vtable patch and not an inline hook: two inline hooks on the same function crash the game when unloaded in the
wrong order (one writes back a jump into an already unloaded DLL). Patching a vtable slot does not interfere with
inline hooks of other tools.

## Usage

Build both DLLs (see the main README), then, with the game running and a save loaded:

```powershell
python bench\scripts\dllctl.py load all        # perf and bench into the game
python bench\scripts\bench_fm.py               # 4 ABBA blocks, 60 days per segment by default
python bench\scripts\dllctl.py unload all
```

`bench_fm.py` edits `stellaris_perf.ini`, waits for the new settings to show up in the plugin's statistics, lets
`settle` days pass and then times `days` days. Segments are ordered ABBA, so a steady drift (the game slowing down as
it runs) cancels in the paired differences. Event pop-ups that pause the game are un-paused automatically; gaps of more
than 2 seconds between days are not counted; if an event hides the fleet manager window (fewer than 0.9 updates per
frame) it pauses and asks for the window to be reopened, then measures that segment again. The output is mean,
standard deviation and frame rate per configuration, and the ABBA paired difference with its 95 % confidence interval.

```powershell
python bench\scripts\bench_fm.py --config base:fleet_manager_cache=0 --config fix:fleet_manager_cache=1 --blocks 6 --days 60
python bench\scripts\bench_fm.py --no-fm-check --config off:opinion_cache=0 --config on:opinion_cache=1
```

### Automatic save loading

```powershell
python bench\scripts\game_session.py load arena_base --fleet-manager   # closes the game, points continue_game.json at the save,
                                                                      # starts with --continuelastsave, loads both DLLs, waits
                                                                      # for the load, pauses, presses F9 to open the fleet manager
python bench\scripts\game_session.py restore                          # puts the original continue_game.json back
python bench\scripts\bench_fm.py --reload arena_base --warmup 5 --blocks 10 --days 3 --settle 1 --config ... --config ...
```

- `--continuelastsave` takes two dashes: the engine looks for the option `-continuelastsave` and the command line parser
  strips one leading dash. It reads the `title` in `continue_game.json`.
- `--reload` starts every ABBA block from the same save moment, alternating ABBA and BAAB between blocks. The first
  dozens of days after a load drift non-linearly, so the output also gives an order-adjusted estimate.

Set `STELLARIS_DIR` if the game is not in the default Steam folder.

## Stress-test mod

`stress_mod/gen_stress_mod.py` writes a mod ("zz perf stress") that reproduces load shapes found in large mods on
vanilla content, without changing gameplay (modifiers are tiny, triggers never match, game rules keep their vanilla
result):

- ship, planet and country modifier storms (a modifier rebuild per change, with and without batching);
- fleet system-entry event fan-out, many global flags, heavier game rules, triggered opinion modifiers, mean-time-to-happen events;
- an arena: a sustained, huge fleet battle in one system, with combat-day loops, per-hit and ship-destroyed events;
- extra AI population with jobs, some unemployment and some vacancies, so migration keeps running;
- event-target-heavy triggers (hundreds of global targets, dynamic `name@this` targets, local targets per ship).

```powershell
python bench\stress_mod\gen_stress_mod.py                       # write the mod folder and register mod\zz_perf_stress.mod
python bench\stress_mod\gen_stress_mod.py --enable              # add it to dlc_load.json (a backup is kept)
python bench\stress_mod\gen_stress_mod.py --disable             # remove it again
python bench\stress_mod\gen_stress_mod.py --help                # every scale knob
```

Remember to run `--disable` when you are done: with the mod enabled the game is far slower than normal.

## Known limits

- The fleet manager is opened by `--reload` with F9. Without a reload it has to be opened by hand once, and again after
  an event pop-up hides it; the script waits.
- It measures real time: keep the game window in front and not minimised (a minimised game renders no frames and
  commands time out).
