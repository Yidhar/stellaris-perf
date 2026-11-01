# Measured effect (Stellaris 4.5.2)

All numbers are game days played per wall-clock time under the plugin's defaults ("all on") against the same plugin with
every optimization switched off ("all off"), in one game session per group, measured with
[`scripts/ab_setting.py`](scripts/ab_setting.py) (ABBA / BAAB segments, the game reloaded for every group).
The effect is estimated by [`scripts/regress_ab.py`](scripts/regress_ab.py): a log-linear model with a fixed effect per
group and per segment position, because the time per day changes a lot between positions in one run (a battle weakens,
month starts cost more). Raw per-segment numbers are in [`data/overall-4.5.2/`](data/overall-4.5.2/).

"All off" sets `fleet_manager_cache`, `fleet_manager_reinforce_ms`, `flag_simd`, `flag_expiry_skip`,
`fleet_parallel_grain1`, `alloc_cache`, `combat_no_copy` and `tick_render_fps` to `0`; `frame_smoothing` is the same in
both. Plugin build: commit `9293e1c`.

## All options on against all off

| Save | Setup | Wall time per game day | Process CPU per game day |
|---|---|---|---|
| Late game (2344 autosave) | 8 groups, 6 days per segment, 30 days warm-up | **-6.2 %** (95 % CI -9.8 .. -2.5) | -2.0 % (-16.5 .. +15.2), inconclusive |
| Arena (16k ships in one battle) | 6 groups, 4 days per segment, 5 days warm-up | **-23.1 %** (-34.1 .. -10.1) | **-22.3 %** (-32.0 .. -11.2) |

- Arena: wall time went down in all 6 groups (-7.7 % .. -35.8 %), CPU time in 5 of 6.
- Late game: wall time went down in 6 of 8 groups; the two others were +6.9 % and +4.7 %. The CPU difference per group
  ranged from -39 % to +20 %, so no CPU saving is claimed there.
- The runs compare the whole set with nothing, so they do not split the gain by option.

## One option at a time

Measured as the options were added, each against the same build with only that option off.

| Option | Save | Wall time | Process CPU |
|---|---|---|---|
| `alloc_cache` (large-block recycler) | arena | -7 .. -8 % | -16 .. -27 % |
| `tick_render_fps` (30) | late game | -4.4 % (-7.1 .. -1.7) | -9.3 % (-20.0 .. +2.8), inconclusive |
| `combat_no_copy` | arena | works, but the gain overlaps `alloc_cache`'s (same allocations) | |

## Choosing the in-tick frame cap (`tick_render_fps`)

Late-game save (an autosave of 2347, pinned as a normal save so autosave rotation cannot delete it), everything else at
the defaults. The main thread was sampled for 100 s per setting (about 39k samples each, stack unwound from the exe's
unwind info); the table is the share of samples inside `RenderFrame`.

| `tick_render_fps` | `RenderFrame` share of the main thread | Wall time per game day |
|---|---|---|
| 30 | 18.9 % | 250 ms |
| 10 | 14.3 % | 251 ms (sampled) |
| 5 | 14.0 % | 242 ms (sampled) |

The sampled wall times are within the noise. Frames are already scarce at about one or two per game day (one frame
costs tens of milliseconds late in the game), so the cap only binds for the frames above that; 10 and 5 save the same
4.6 points. Interleaved A/B runs (8 groups each) of 30 against 10 gave -0.7 % (95 % CI -7.3 .. +6.4) and of 30 against
5 gave -13.1 % (-19.6 .. -6.1): both are compatible with the roughly 5 % the profile predicts, the second one is
probably a lucky draw. The default is 10.

## Frame cap between ticks (`render_cap_between_ticks`)

At full speed in the late-game save the engine's frame function (RVA 0x332EF0: input, GUI update, tick, GUI draw) runs
about 20 times a second, two ticks each; the ticks take 75 to 80 % of its time and the GUI update and draw the rest.
`RenderFrame` is also called from the main loop between ticks, which `tick_render_fps` did not cover.

| Change | Save | Wall time | Process CPU |
|---|---|---|---|
| Cap those calls too, while the simulation runs (8 groups, 6 days per segment, 40 days warm-up) | late game | **-9.9 %** (95 % CI -15.1 .. -4.3) | -12.9 % (-26.1 .. +2.6), inconclusive |
| the same, first 8-group run (30 days warm-up, build without the "simulation is running" condition) | late game | -5.5 % (-12.7 .. +2.3) | -2.6 % (-18.8 .. +16.7) |

Main-thread samples (60 s each, two repeats): `RenderFrame` 14.4 / 15.1 % without it, 6.0 / 6.4 % with it.
Tried and rejected: dropping the draw step (RVA 0x334160) of each loop iteration at 10 fps. The game got slower
(231 to about 280 ms per day): the engine then ran the GUI update (RVA 0x9293a0, 0x1c0a7a0, 0x1A4D610) and `RenderFrame`
more than twice as often (their samples went from 6.3 / 3.4 / 2.7 % to 15.5 / 8.7 / 7.5 %).

## Tried, no measurable effect (not shipped)

| Idea | Save | Wall time | Process CPU |
|---|---|---|---|
| Outliner update every 500 ms instead of the engine's 100 ms while the simulation runs (hook on `CCountryTabbedOutliner::Update`, the function is two wall-clock timers and nothing else) | late game, 8 groups | -0.1 % (-4.4 .. +4.5) | -9.4 % (-20.6 .. +3.4), inconclusive |

The static sweep estimated 2 to 4 % of the main thread for the outliner; with the frame cap already on, the interval
change was not distinguishable from noise, so the hook was dropped.

## Where the time goes, per kind of day (`phase_profile`)

Late-game save `late_base` (2347.01.01, pinned), the plugin's defaults plus `phase_profile=1`, 400 game days at speed 4,
no mods, machine otherwise idle. The plugin times the engine's phases with rdtsc at their entry points and keeps one
record per game day (`bench/scripts/phase_report.py`, raw data in [`data/phases-late-4.5.2/`](data/phases-late-4.5.2/)).
Means over the days of each kind; incomplete days and outliers (wall time above 3x the group's median) are left out.
Milliseconds per game day; the share is of that day's wall time.

| | normal day (387 days) | 1st of a month (11) | 1 January | 1 July |
|---|---|---|---|---|
| wall time | 211 (median 193) | 699 | 1435 | 1550 |
| micro update (10 ticks) | 65 (31 %) | 68 | 69 | 63 |
| daily update | 79 (38 %) | 84 | 74 | 122 |
| monthly update | - | 422 (60 %) | 375 | 420 |
| yearly update | - | - | 10 | - |
| AI update, including the wait for the workers | 16 (7 %) | 24 | 15 | 37 |
| autosave: game-state dump on the main thread (`CSaveGameTaskManager::Update`) | - | - | 622 (43 %) | 636 (41 %) |
| frames drawn inside ticks | 12 (6 %, 1.9 frames) | 57 (5.8 frames) | 63 (7 frames) | 58 (7 frames) |
| frames drawn between ticks | 0.4 (0.1 frames) | 0 | 161 (27 frames, while the save waits) | 179 (28 frames) |
| the frame function's own GUI update and draw (5 iterations a day) | 37 (18 %) | 43 | 44 | 35 |
| unaccounted | 0.5 | 0.4 | 1.7 | 0 |

- The accounting is complete: the phases add up to the wall time within about 1 ms.
- A game year on this save: 348 normal days at 211 ms = 73 s, twelve month starts about 8 s (the monthly update alone
  about 5 s), two autosaves about 1.6 s. The spikes are about 8 % of the wall time, but they are all of the hitching.
- The frame function runs five times a game day (two ticks per iteration at this speed); with the frame caps only about
  two frames a day are drawn on a normal day.
- An earlier run of the same 400 days measured normal days at 201 ms (median 187): the machine's load moves the absolute
  numbers by about 10 % between runs, the shares stay.

## The monthly update, decomposed (`phase_extra`)

`phase_extra` hooks any functions of the exe (by RVA, function starts only) while the game runs and times them like the
fixed phases, so a phase can be split level by level without rebuilding. Four rounds on the same save, five month starts
each: every round hooked the direct callees of the previous round's biggest items. The settings of the last round are in
[`data/phases-late-4.5.2/monthly_r6_settings.ini`](data/phases-late-4.5.2/monthly_r6_settings.ini), the records next to it.
A first-of-month day is 620 to 710 ms (a normal day about 200); `CGameState::MonthlyUpdate` is about 415 ms of it.
Exclusive time per leaf, ms per month start, 4.5.2 RVAs:

| Leaf | ms | calls x each | What it is |
|---|---|---|---|
| Pop-group size changes (growth, decline, migration landing) | ~135 | | `CColony::ApplyMonthlyGrowth` -> `AccessOrAddPopGroupAddSize` -> `CPopGroup::AddSize` / `MoveSizeFrom` |
| &nbsp;&nbsp;`CModifierNodeBase::Update` (0x238300): modifier nodes recomputed | 61 | 45.8k x 1.3 us | every size change invalidates the colony's nodes, the next read recomputes; a normal day has 9.6k calls, 1.6 ms |
| &nbsp;&nbsp;`CPopGroup::MoveSizeFrom` (0x67E240) | 38 | 1650 x 23 us | migration categories, ethics attraction, species deconversion, Robin-Hood tables |
| &nbsp;&nbsp;invalidation helper 0xDB0D20 (modifier manager AddInvalid, InvalidateChildren) | 31 | 800 x 39 us | |
| `COnActionDatabase::PerformEvent` (0x3F9F50) | 70-89 | 2500 x 28-35 us | on_pop_growth and the like: the trigger of every event attached to the on_action is evaluated (`CEvent::IsValid`), most do not fire. Normal days: 325-440 calls, 6-7 ms (3 %) |
| Pop job assignment: `NPlanetJobs::UpdatePopJobs` -> 0xB36CC0 (the AssignPopsToJobs core) and its callees | 45-75 | 8-9 calls | the core's own part 17-47 ms (includes the wait for its parallel part), 0xB358B0 12, 0xB340B0 8, 0xB34790 5 |
| Parallel phases the main thread waits for: 0x2ADC10 (a country monthly phase), ship upkeep (0x1B4D690) | 24 + 20 | 1 each | the slowest chunk |
| `on_monthly` on_action (0xE1C3F0) | 19 | 1 | script events |
| Armies (`NPlanetArmies`, 0x945C60) | 14 | 446 x 31 us | |
| Leader cost and cap per country (0x775220) | 10 | 78 x 134 us | |
| Remaining own time: `MonthlyUpdate` itself 6-36, country serial update 15, colony update 10, `UpdatePopJobs` 10, 0xDBADC0 11, assembly 6, decline 5, ... | ~80 | | |

On a month start the daily update also costs 15-20 ms more than usual and 5.8 frames are drawn inside the long tick
(+45 ms). Candidates, by value against risk: batching the modifier invalidation over a colony's monthly pass (up to about
-70 ms, needs the callers of the 45.8k node updates checked first, since growth code may read the intermediate values);
skipping the second pop-job pass when the first changed nothing (-30 to -40 ms, semantics to confirm); longest-first
ordering of the parallel phases (up to -20 ms). The on_action cost is script trigger evaluation and has no safe shortcut
in the plugin.

## Caveats

- The arena save is a stress case (16k ships fighting); the late-game save is closer to a normal long game and shows the
  smaller gain. Expect gains between the two, and less on a small early game.
- The engine is not deterministic in the arena save (two runs with the same settings diverge by day 5 to 10), so these
  are timing comparisons, not result comparisons.
- Small differences (under about 5 %) need many groups to see; the late-game interval is still wide.
