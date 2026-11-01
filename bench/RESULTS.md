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

**Where the 45k `CModifierNodeBase::Update` calls come from** (`phase_callers=0x238300`, call sites recorded per day
kind, [`monthly_r7_modnode_update_call_sites.json`](data/phases-late-4.5.2/monthly_r7_modnode_update_call_sites.json);
times here are inclusive of nested node updates, so they overlap):

| On a month start (per month start) | calls | inclusive | each |
|---|---|---|---|
| the modifier manager's flush loop (0x231B20): every invalidated node recomputed on demand | 29k | 64 ms | 2.2 us |
| inside the pop-group add path (0xDB0BB0) | 500 | 17 ms | 34 us |
| pop-group helpers 0xDAFAB8 / 0xDAFB10 | 1230 / 730 | 12 / 8.5 ms | 10-12 us |
| pop job assignment (0xB35F30), `UpdatePopJobs` | 370 / 1130 | 8 / 7 ms | 22 / 6 us |
| 0xDB0C60 (pop-group removal path) | 240 | 8 ms | 33 us |

The 10-34 us calls are each a flush: a pop-group change invalidates the colony's modifier subtree and the next modifier
read, inside the same change or the next step of the colony's monthly pass, recomputes it, about 2,900 times per month
start. On a normal day the whole thing is 1.4 ms (0.6 ms from `CCountry::DailyUpdateSerial`, 0.4 ms from the flush
loop).

**The colony pass's modifier work (`colony_monthly_batch`).** Read out of `CColony::MonthlyUpdateSerial` (0xDC43E0)
and its helpers: the pass applies amounts the parallel demography pass computed beforehand (migration, growth,
assembly, decline), one pop group at a time; after each change `CColony::OnAddPops` / `OnRemovePops` force the colony's
node up to date (`CModifierNodeBase::Update` at 0xDAFAD3 / 0xDAFB55), recompute the cached values
(`MicroUpdateCachedValues` at 0xDAFAE2 / 0xDAFB64) and invalidate the node again when they changed, and a new pop
group's node is refreshed at creation (0xDB0C34, 0xDB0CF0 / 0xDB0CF9). Nothing in the pass needs those intermediate
refreshes (the amounts are precomputed), so the first version skipped them for the colony whose pass is running and did
them once at its end: measured in one continuous run with the setting alternating from month start to month start (24
month starts, [`colony_batch_interleave_24m.json`](data/phases-late-4.5.2/colony_batch_interleave_24m.json), the mode
of each in `..._modes.json`), 37.9k forced refreshes skipped, monthly update 380.7 ms on against 378.6 ms off: nothing.
`Update` only works when the node's dirty flag (+0x108) is set, and the events fired by every change (2,400
`PerformEvent` calls per month start) read the colony's modifiers in between, so the node was rebuilt on demand before
the forced refresh ran; the invalidation (`CColony::InvalidateModifier` 0xDACEF0: dirty flag, `InvalidateChildren`
0x238430, `AddInvalid`) and the reads drive the rebuilds. What does save: `1` additionally holds the modifier
manager's flush while a colony's pass runs (about 14 per month start fall inside the passes; the next flush after the
pass rebuilds the dirty nodes once; on-demand readers are unaffected), `2` also freezes the colony's node during its own
pass (about 1,300 reads of it per month start from inside the pass get the pre-pass node; it is rebuilt once at the end
and the colony invalidated again). Three-way run, 36 month starts
([`colony_batch_interleave3_36m.json`](data/phases-late-4.5.2/colony_batch_interleave3_36m.json)): monthly update
369.7 ms off, 358.3 ms with `1`, 345.2 ms with `2` (medians 366 / 356 / 345); a second run of 72 month starts
([`colony_batch_interleave3_72m.json`](data/phases-late-4.5.2/colony_batch_interleave3_72m.json), the autosave days
spread over the modes): 397.3 / 387.1 / 349.3 ms (medians 403 / 361 / 356). `2` takes 25-48 ms off the month-start
monthly update (-7 to -12 %), about -1 ms per day averaged; `1` alone is within the noise of these runs (-3 to -10 ms
means, -10 to -42 ms medians). 124k forced refreshes skipped, 1,051 flushes held and 31k reads of a frozen node over
the 72 months, no mismatch reported by the engine.

**The second pop-job pass (`pop_jobs_single_pass`).** `NColonyUpdate::MonthlyUpdate` (0xE3BFA0) calls
`NPlanetJobs::UpdatePopJobs(date, span of all colonies)` (0xB25DF0) twice in a row with the same arguments (calls at
0xE3C189 and 0xE3C1AE; the cheat-mode daily variant 0xE3BF20 has the same pair). Per month start on the late save
(`phase_callers=0xB25DF0`): the first call 64 ms, the second 32 ms, inclusive of their parallel weight phases; the
daily single-colony path (0xB264A0, colonies whose buildings changed) 3-4 calls a day of 0.3 ms. What the second pass
does, read out of `AssignPopsToJobs`: it rebuilds everything from the first pass's output. The first pass's promotion
tail moves a promoted pop group into the job's category and re-adds it to the job (`CPopJob::AddPopGroup`), so
promoted pops are not left jobless; what the second pass can change is what the first's modifier invalidations (job
checksums -> `UpdateEconomicResources` -> `CCountry::InvalidateModifier`) move in job slots and workforce, and the job
weights that read the assignment (11 of the 365 vanilla jobs: the four zoo animals' `num_assigned_jobs`, healthcare's
and five others' `free_amenities`, one `free_jobs`). The engine's own daily recompute is a single pass. The option
skips the second call (both pairs); the month's assignment is then the first pass's, one iteration behind the engine's
where those inputs changed, until the next recompute. Measured in one run with the setting alternating from month start
to month start (36 month starts,
[`pop_jobs_single_pass_interleave_36m.json`](data/phases-late-4.5.2/pop_jobs_single_pass_interleave_36m.json)): monthly
update 346.0 ms off, 309.5 ms on (medians 335.8 / 298.0), -36 ms or -11 % of the month-start monthly update, about
-1.2 ms per day averaged; 18 second passes skipped, no error.

**The daily AI phase is one country's update (`ai_parallel_lpt`: no gain).** `UpdateAIParallel` (0xB86900) is a
parallel-for over the `CCountryAI` array (78 entries on the late save): tasks take entries one by one from a shared
counter and call vtable slot 7 (0xB6EF20: skips countries without an AI, 34 of the 78, then the base update, which runs
the AI through slot 8 and more). Hooking slot 7 per class and timing each country with rdtsc: up to 16 countries in
flight at once, all updates together 55-58 ms of thread time, the heaviest country 16.5-18 ms, the phase 17-18 ms,
i.e. the phase is that one empire's update, taken in the first wave in array order anyway; the rest is finished by
12 ms. The longest-processing-time order therefore changes nothing: in one run alternating it every month (36 months,
[`ai_parallel_lpt_interleave_36m.json`](data/phases-late-4.5.2/ai_parallel_lpt_interleave_36m.json)) the `ai` phase of
a normal day is 14.67 ms off and 14.80 ms on (medians 13.65 / 13.05). What the waiting main thread needs is a shorter
update for the biggest empire, which is work inside one country's AI (its ministers' and economic plans' script
evaluation), not scheduling. The per-country measurement stays in the plugin (the stats line: phase, heaviest, sum).

**Inside the heaviest country's update** (sampled from outside the game while it runs, 886 samples; the plugin
publishes the thread in its shared statistics, `research/ai_heavy_country_profile.md` in the research repository):
ship building and design 30 % (`CAIInteriorMinister::GetOrRequestDesign` -> `CCountry::CreateShipDesign` 19 %),
economic strategy 18 %, planet buildings and districts 8 %, military stratagems 5 %, espionage 4.5 %, diplomatic
acceptance 4.4 %; about 37 % of all of it inside script trigger evaluation. `CreateShipDesign` runs, per growth stage,
the weapon and utility passes twice (`AddWeaponsToGrowthStage` -> `AddWeaponsToShipDesignSection` -> a chooser per
slot, `AddUtilities`, three `RemoveNonPoweredComponentsInShipDesign`) and then `UpgradeComponents`; every chooser
evaluates each candidate component's `CanBeBuiltBy`, `IsAllowed`, `IsRestricted` and `CalcAIWeight`, the last one
`CMeanTimeToHappen::GetRawFactor(weight, out, country scope)`.

**Scripted weights memoised within a phase (`ai_weight_cache`).** `GetRawFactor` (0x3BC290: hidden 8-byte return
buffer, the factor list at +0x8B8 writes the base and the modifiers scale it) hooked with a per-thread table keyed by
(weight, scope identity: self / from / root / prev objects, no parameters or saved event targets), valid for one AI
phase and used only inside a country's update. 36 months alternating monthly
([`ai_weight_cache_interleave_36m.json`](data/phases-late-4.5.2/ai_weight_cache_interleave_36m.json)): 2,600
evaluations per phase, 90.6 % hits; the `ai` phase of a normal day 15.54 ms off, 15.35 ms on (medians 13.54 / 13.20,
90th percentile 28.65 / 27.03). Exact and cheap, so on, but small: the repeats are few (about 30 per country), and the
component choosers' own loops and the other checks cost more than the weight factors.

**Nested parallel-fors of the AI update run inline (`ai_nested_inline`: within noise).** About 36 parallel-fors per
phase are started from inside a country's update (the economic forecast among them); their tasks queue behind the
other workers' countries and the forking thread spin-waits (5 % of the heaviest empire's update in the sampled
profile). The parallel-for descriptor's constructor (0x1B50060; the Windows build ignores the max-task argument) is
hooked and a descriptor built inside a country's update gets its task count at +0x14 set to 1, which makes
`CPdxTaskScheduler::CreateTasks` take its serial path. 36 months alternating monthly
([`ai_nested_inline_interleave_36m.json`](data/phases-late-4.5.2/ai_nested_inline_interleave_36m.json)): the `ai`
phase of a normal day 15.58 ms off, 15.54 ms on (medians 14.43 / 14.07, 90th percentile 27.40 / 26.61, 95th 31.3 /
32.9): the work the thread now runs itself replaces the wait almost one for one. Kept, off.

## The autosave spike: the serialization, sampled

The half-yearly autosave costs the main thread a `save` phase of 600-670 ms on the late save: `CSaveGameTaskManager::Update`
(0xA89AD0) waits for the serialization task (on a worker thread; the compression and the write are another task it
does not wait for) while drawing frames. The serializing thread sampled from outside the game (the plugin flags the
window in its shared statistics, `sample_thread.py ... save` in the research repository picks the busiest thread; 1,234
samples of one autosave): `CWriter::WriteTopLevelList` recursing through the serializers; of the time, the C
runtime's printf engine 22-25 % (every number goes through the engine's `sprintf` / `snprintf` thunks: `CToken(int)`
`"%d"`, `CToken(unsigned)` `"%u"`, `CWriter::Write(CToken)` `"%s%lld.%0.5d"`, `CToken(CFixedPoint)` `"%0*lld"` and
`"%lld.%s"`), `CBlob` growth copies 9 % (only the first save of a process grows the buffer; later ones keep it), the
per-token `CFile::WriteData` path 12 % exclusive, quoted strings with two `ReplaceAll` each 10 %.

**`save_fast_format`.** The two thunks (0xD38AA0 `sprintf`, 0x1B486D0 `snprintf`, both into
`__stdio_common_vsprintf`) are hooked; the eight formats above plus `"%s"` and `"%lld"` are produced by plain
integer-to-decimal code with printf's precision and zero-padding rules and snprintf's C99 truncation, everything else
goes to the runtime unchanged. At install every fast format is compared with the runtime byte for byte on a battery of
values (8 of 8 match on 4.5.2). 72 months with the setting alternating per autosave
([`save_fast_format_interleave_72m.json`](data/phases-late-4.5.2/save_fast_format_interleave_72m.json)): the `save`
phase 617 ms off, 572 ms on per autosave (medians 613 / 569, 6 autosaves each), 20 million numbers through the fast
paths. What was left in the serialization was the writer itself: next.

**`save_fast_writer`.** The writer's own machinery, reversed on the Windows build: every piece of text (indentation,
key, `=`, value, newline) becomes a temporary `CToken` (the token table's string copied into a 256-byte hybrid array
after a grow check; `CWriter::WriteToken` 0x16AA50, `WriteIndent` 0x1BB4510 which writes the tab token `depth` times,
`Write(CString)` 0x1BB57F0 for keys), goes through `CWriter::Write(CToken)` 0x1BB46D0, which in text mode is
`CFile::Write(const char*)` (strlen, then slot 9), and lands in `CMemoryFile::WriteData` 0x1B49F40, which asks the file
its size through slot 16, calls `CBlob::Append` 0x1B4B6A0, which calls `Reserve` 0x16B140 and the runtime's memcpy, then
advances the position: 31 million such writes of a few bytes per late save. `CWriter::WriteString` 0x1BB3FF0 copies a
quoted string into a `CString` and runs two `ReplaceAll` (a memchr scan each) before writing it between two quote writes.
Five hooks: `WriteData` appends in place when the file is a memory file over a blob with room (position at the end,
capacity enough; the engine's path otherwise); a token, a key and the indentation are written straight from the engine's
token table through slot 9 with the known length (the table's strings come from C strings), the indentation as one run
of tabs; a quoted string with no quote or backslash is written with its quotes in one piece, with an escape it takes the
engine's path. Binary writers and files whose slot 6 is not the plain `CFile::Write(const char*)` keep the engine's
path. Bytes checked from one paused game (`save_compare.py`, the bench DLL's new `save` command): off and on identical,
gamestate 100,635,333 bytes, meta 1,005. 72 months with the setting alternating per autosave, `save_fast_format` on in
both arms ([`save_fast_writer_interleave_72m.json`](data/phases-late-4.5.2/save_fast_writer_interleave_72m.json)): the
`save` phase 677 ms off, 375 ms on per autosave (medians 669 / 361, 6 autosaves each), the autosave day's wall time
1543 ms to 1245 ms; per autosave 31 million appends, 14 million tokens, 7 million indentations, 4 million keys and
1 million quoted strings through the fast paths. What is left: the number tokens still each build a temporary `CToken`
before `Write(CToken)`, and the recursion of `WriteTopLevelList` itself.

Two runs of the same save cannot be compared save for save: the engine reseeds a loaded game from the clock unless its
`UseSavedRandom` console variable is set (`stellaris_bench.dll` now sets it), and even with the seed kept two runs of
`late_base` differ in 70k places of the gamestate after six months (the engine's own threading), which also puts
+-50-90 ms of world noise on any month start compared across runs. A/B measurements of month starts are therefore done
inside one run, alternating the setting (the ini is re-read every 2 s).

## Caveats

- The arena save is a stress case (16k ships fighting); the late-game save is closer to a normal long game and shows the
  smaller gain. Expect gains between the two, and less on a small early game.
- The engine is not deterministic in the arena save (two runs with the same settings diverge by day 5 to 10), so these
  are timing comparisons, not result comparisons.
- Small differences (under about 5 %) need many groups to see; the late-game interval is still wide.
