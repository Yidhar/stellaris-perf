# stellaris-perf

[English](README.md) | [简体中文](README.zh-CN.md)

A performance plugin for **Stellaris 4.5.4** (Windows x64, the `-dx11` build): a small DLL that is loaded into
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
- Nothing about the engine is hard-coded: addresses, structure offsets, virtual slot indices and token ids all come
  from an SDK generated from the installed executable (`sdk/stellaris_sdk.hpp`, a subset written by
  `tools/extract_sdk.py`). Every function the plugin hooks and every call instruction it patches is located by a
  structural rule in the Stellaris MCP repository's `tools/sdk_dumper/functions.py` (`sdk::fn`, `sdk::site`), and every
  layout it reads or writes is read out of the engine code that uses it by `tools/sdk_dumper/anchors.py` (`sdk::rt`,
  `sdk::vt`), so a game patch needs a new dump, not new addresses or offsets.
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
- **The rest of the in-game GUI: military power cache** (`ui_power_cache`). The entry of a selected fleet in the fleet
  window (`CFleetViewEntry::OnUpdate`, once per shown entry and frame), the outliner's fleet rows (ten times a second,
  every fleet) and a fleet's tooltip get the power they show from the same engine function, again asking for every
  ship's hull, armor, shields, evasion and damage from scratch. The fleet manager's cache answers them too, under the
  same limits (one second of real time, never across a game day, the fleet's ship array and ship count in the key),
  wherever the calling thread is inside one of the per-frame GUI updates. The simulation never is: a tick suspends
  that context and the worker threads have none. Display only.
- **Fleet window: the upgrade button's check** (`fleet_upgrade_cache`, default `1`). Every frame the fleet window asks
  each selected fleet's upgrade button whether it is valid, and the button works out the fleet's upgrade cost for it:
  every ship, component by component (resource tables), against the treasury and the income, with the localized cost
  text as the reason. With a large fleet selected that was the largest single item of the fleet window's update.
  The button's call (told apart by its return address; the upgrade command's own check is not touched) gets the last
  answer and reason text back for the same fleet with the same ship array and count, on the same game day, as long as
  no key or mouse button was pressed or released since (and none in the 100 ms before; moving the mouse changes
  nothing), for at most one second. `2` lets the
  engine answer every time and counts how often the kept answer differed. Display only.
- **Galaxy view: the cursor's path check** (`cursor_path_cache`, default `1`). With fleets of yours selected and the
  mouse over a system, the cursor update (for the first selected fleet) and the hovered system icon's indicator (for
  every selected fleet) ask every frame whether the fleet can move there: a path search across the galaxy unless the
  fleet's current path already leads there, only to pick the cursor and the indicator. Those two calls get the last
  answer for the same fleet and system back on the same game day, for at most half a second. `2` verifies as above.
  Display only; the order you give is checked by the engine as before.

  Measured with a fleet of 139 ships selected, per GUI update: the fleet window entry's update 0.95 ms without and
  0.34 ms with the power and upgrade caches; the cursor's path check 0.15 to 0.25 ms, then computed twice a second; a
  hovered system's tooltip 0.2 ms, then built about once in forty updates while the mouse rests. The verify modes found
  no difference in a paused game and two stale tooltips in 122 game days at speed 3
  ([`bench/RESULTS.md`](bench/RESULTS.md)).
- **Tooltips kept instead of rebuilt every frame** (`tooltip_cache`, default `1`; `tooltip_cache_ms`, default 250).
  Every frame the tooltip manager resets its tooltip and asks for the content again, first the hovered GUI object's
  tooltip handler, then the idler (the map and what lies on it), only to compare the answer with the text already on
  screen. A ship's tooltip writes out every component for it (each weapon's damage lines, the cost and upkeep resource
  tables, all localized); a fleet's walks its ships. Both requests are virtual calls made at one place each; the plugin
  hooks those two call instructions (six bytes each, located by the SDK) and keeps what the provider last put into the
  tooltip and what it returned. The same provider asked about the same object gets that back as long as nothing that
  could change it has happened: no key or mouse button pressed or released since it was built and none during the
  last 100 ms, the mouse where it was, the same game day and plugin settings, and at most `tooltip_cache_ms` of real
  time. The rest of
  the manager's update (hover timers, the comparison, placing the window) runs every frame as before. `2` asks the
  provider every frame anyway and counts how often the kept content would have differed. Display only: while the game
  runs, or after a change that needs no input, a tooltip can be up to `tooltip_cache_ms` behind.
- **Large-block recycler** (`alloc_cache`, `alloc_cache_mb`). In a big battle the engine builds and frees several
  hundred-KB arrays per fleet on every combat tick (the targeting tables), each with a plain `operator new` / `free`
  pair, so every tick takes cold memory from the heap and gives it back. The plugin hooks `operator new` and `free`
  and keeps the last few blocks of 128 KB and more that each thread freed (up to `alloc_cache_mb` per thread, 8 MB by
  default); the next request of that size on the thread gets the same block back, still warm in the core's cache. The
  blocks are ordinary heap blocks without a header, so the engine can free one at any time, also after the plugin is
  gone, and a block's contents are never assumed to be zero (`operator new` does not promise that either). A block is
  only reused when the heap still reports it large enough. Switching the key off removes the hooks and returns the
  cached blocks to the heap.
- **Combat participants shared, not copied** (`combat_no_copy`). The engine's parallel ship update looks up each
  ship's fleet in its combat-participants table and, for a fleet that has an entry, deep-copies the entry (seven arrays,
  each as long as the number of ships in the system) for every ship, only to pass it as a read-only argument to
  `CShip::MicroUpdateParallel`, and frees the copy again. The plugin patches that copy away (an 11-byte code patch
  found by its byte pattern, which must match exactly once): the callee gets the table's own object and the code that
  would destroy the copy is skipped. The table is not changed while the ships are updated, so every ship sees the data
  it saw before. The patch is written and removed only between ticks, while the worker threads are idle.
- **Less modifier work inside a colony's monthly pass** (`colony_monthly_batch`, default `2`). The colony monthly
  pass (`CColony::MonthlyUpdateSerial`: migration, growth, assembly, decline, with amounts the parallel demography pass
  computed beforehand) changes pop-group sizes one group at a time. Every change invalidates the colony's modifier node,
  and the node is rebuilt on demand by the next reader: the triggers of the events the change fires, the refresh the
  engine forces after each change (node, then the cached amenities, crime and stability, then another invalidation), a
  new pop group's refresh — about 1,300 rebuilds of the node per month start on a late save, plus a few runs of the
  modifier manager's flush inside the passes. `1`: the forced refreshes are skipped while a colony's own pass runs and
  done once when it ends, in the engine's order, and the manager's flush is held during a pass (its dirty nodes are
  rebuilt by the next flush after it); readers rebuild a dirty node on demand, so nothing a script or the engine reads
  changes. `2`: in addition the colony's node is frozen during its own pass: every read of it from inside the pass
  (event triggers on `on_pop_growth` and the like, the pass's own code) gets the node as it was when the pass began;
  it is rebuilt once at the end and the colony invalidated again, so whatever was computed from the frozen node is
  rebuilt afterwards. Deterministic, so two clients that both run it agree. What differs from the engine in both
  modes: a colony whose amenities or housing tier moves during its own pass gets the stability that goes with the tier
  from before the pass (the engine: from before the pass's last change), and an `on_pop_growth` / `on_pop_purged`
  event reading the cached values sees the pre-pass ones. Measured on a late save, the month-start monthly update in
  two runs of 36 and 72 month starts: `0` 370 / 397 ms, `1` 358 / 387 ms, `2` 345 / 349 ms (see
  [`bench/RESULTS.md`](bench/RESULTS.md)).
- **Monthly pop-job assignment once instead of twice** (`pop_jobs_single_pass`). `NColonyUpdate::MonthlyUpdate`
  calls `NPlanetJobs::UpdatePopJobs` over all colonies twice in a row with the same arguments: the second call is one
  more fixed-point iteration over what the first changed (pop groups the first collapsed or promoted keep the jobs
  they were given; the modifiers the first invalidated, through job checksums reaching the country's economy, are
  rebuilt and can move job slots and workforce; the few job weights that read the assignment, such as the zoo animals'
  `num_assigned_jobs` and healthcare's `free_amenities`, see the new one). The engine's own daily recompute of a colony
  whose buildings changed is a single pass. With this on the second monthly call is skipped (and the second of the
  pair in the cheat-mode daily variant): the month's assignment is the first pass's, and the next recompute picks up
  whatever the second would have changed. Deterministic, so two clients that both run it agree. On a late save the
  first pass takes 64 ms of a month start and the second 32 ms (`phase_callers`); measured in one run with the
  setting alternating from month start to month start (36 month starts), the month-start monthly update is 346 ms
  off and 310 ms on (medians 336 / 298; see [`bench/RESULTS.md`](bench/RESULTS.md)).
- **Faction support and approval once per month start** (`pop_faction_batch`, default `2`). Every time pops move
  between groups (the collapses and promotions of the pop-job pass, growth, migration), the engine runs
  `CPopFaction::UpdateSupportAndApproval` for every faction the group belongs to: `CalcSupport`, then `CalcApproval`
  over the faction type's scripted demands (triggers), then the member groups invalidated when the approval changed.
  On a late save that is 325 calls in one pop-job pass for 55 factions, 19 % of the pass; nothing in the window reads
  a faction's support or approval (the promotion loop decides by precomputed category flags, the job scripts and the
  growth and migration code do not read factions). With this on the calls inside a window are recorded and run once
  per faction when it ends: `1` the window is each pop-job pass, `2` the whole monthly colony update. The values are
  those of the same final state (verify modes `3` / `4` run the engine's calls and then check every touched faction
  against a recomputation from the final state: 0 mismatches in 1,400 checks); a faction whose values changed and
  changed back inside the window is not invalidated then, and a mod script that read them inside the window would
  see the values from before it. Measured on the late save: the first pop-job pass 69 ms with the engine's calls,
  57 ms with `1`, 51 ms with `2` (the recomputation then takes 3 ms at the end of the monthly update); over 36 months
  alternating monthly, the month-start monthly update 345 ms off and 335 ms on with `2` (medians 326 / 313).
- **No forced modifier refresh for the pop groups a promotion creates** (`pop_jobs_node_batch`, default `2`). When
  the pop-job pass promotes pops into a group that does not exist yet, the engine creates the group, moves the pops
  and then refreshes two modifier nodes at once, the source group's and the new group's (`CModifierNodeBase::Update`,
  which the next reader of a dirty node would run anyway). Each refresh rebuilds the colony's and the planet's node
  first, and a group's rebuild makes them dirty again, so every new group costs those two rebuilds (scripted
  triggers): on a late save 134 new groups per pass, 6 ms of a 50 ms pass. Nothing in the rest of the pass reads a
  pop group's node (the promotions decide by precomputed category flags), and watched over the month that follows,
  half of the nodes refreshed are never read before the next pass (the source groups emptied by a whole-group
  promotion, which are dead from then on, and groups nobody consults), a tenth are rebuilt by the modifier manager's
  flush, the rest on demand by a reader inside the monthly update. With this on the two refreshes are skipped and the
  nodes left dirty for their next reader, as any node the engine invalidates; deterministic. `1` instead rebuilds the
  nodes once each when the pass ends (the same values, from the pass's final state), which measures no gain because
  the colony and planet rebuilds repeat; `3` is `2` plus the statistics of who rebuilds the nodes later. Measured on
  the late save from the same load: the first pop-job pass 49.8 ms with the engine's refreshes, 41.4 ms without, and
  the readers that rebuild the nodes later take 2.3 ms per month; over 36 months alternating monthly, the month-start
  monthly update 301 ms off and 296 ms on (medians 298 / 300: within the noise of that measurement, see
  [`bench/RESULTS.md`](bench/RESULTS.md)).
- **The pop-job pass's colony refresh loop** (`pop_jobs_flush_first`, experimental, off). Every pass starts by
  refreshing every colony's modifier node, one after the other on the main thread, and after the colony monthly pass
  every colony's node and its planet's are dirty (a colony's invalidation also invalidates its planet, and each
  rebuild of one re-invalidates the other): on a late save 317 of 560 colonies rebuilt with their planets, 8.8 ms of a
  42 ms pass, three quarters of it the planets. `1` runs the modifier manager's own parallel flush before the pass so
  the loop finds the nodes clean: the loop drops to 1 ms, but the flush, which rebuilds every dirty node of the game
  (thousands of pop groups too), costs 13 ms. `2` skips the loop: the pass is 9 ms shorter and does not rebuild the
  colonies on demand (it works from their last rebuilt state), and over the month that follows the colony rebuilds
  rise by a quarter and the planet rebuilds by a third (the daily flush rebuilds them again). Neither pays; both are
  kept for measurement, with the counts of colony and planet rebuilds by context in the statistics line
  (`node_rebuilds_*`): on the late save the planets alone are rebuilt about 1,260 times a day, 14.5 ms of thread time,
  and 3,250 times more at a month start.
- **No country-wide modifier cascade when the country's modifier did not change** (`modifier_cascade_gate`, default
  `2`). A country's modifier node is invalidated several times a day: its daily update when a faction's support or
  approval moved, the relay network update when the set of systems in the network changed, a planet's daily update,
  the daily pop-job recompute, leader and starbase changes. Every invalidation marks all of the node's children dirty
  at once (`CModifierNodeBase::InvalidateChildren`, every category): the planets (colonized or not), pop groups,
  fleets, ships, armies, starbases and leaders of the country, and the modifier manager's next flush rebuilds them
  all: on a late save 10,000 rebuilds and 15 ms of wall time a day, over 90 % of them producing the modifier the node
  already had, while the country's own rebuilt modifier is identical nine times out of ten. The same for the relay
  network node, a parent of every planet. With this on the cascade from the country's main node and from the relay
  node is held at its three call sites: the node is rebuilt at once on the invalidating thread (the engine's daily
  path rebuilds it right there anyway) and its children are invalidated the engine's way only when the rebuilt
  content differs from the content they were built from (the entries by type and the source records' multiplier,
  category and name content, compared as sets). Deterministic. Measured on the late save: 11 held invalidations a
  day, 93 % with the content unchanged; the daily flush drops from 10,100 to 3,500 rebuilds and from 15 to 7.5 ms of
  wall time; over 36 months alternating monthly, a normal day's `DailyUpdate` 75.2 → 67.3 ms (medians 57.1 / 53.2) and
  the day 195.9 → 188.3 ms (medians 179.3 / 175.2), the month-start monthly update 294 → 284 ms. What differs from the
  engine: a child whose own inputs changed without an invalidation of its own, which the engine refreshes through the
  next country-wide cascade, keeps its modifier until its next invalidation or the next change of the country's
  content: about seven nodes a day on the late save, colonies and planets mostly (see
  [`bench/RESULTS.md`](bench/RESULTS.md)). `1` holds only the invalidations made on the main thread (the relay network
  update runs in a parallel task; its invalidations keep the engine's cascade), `3` counts without holding, `0` is the
  engine's.
- **The daily AI update's countries heaviest first** (`ai_parallel_lpt`, experimental, off). `UpdateAIParallel` is a
  parallel-for over the `CCountryAI` array: its tasks take the countries one by one from a shared counter, in array
  order, and the main thread takes its share and then waits for the last task. With this on the plugin's hook of the
  per-country update processes, when the parallel-for reaches array slot i, the country that ranks i-th by the cost
  measured the day before (longest-processing-time order); every country is still updated exactly once per phase, and
  the update neither draws random numbers nor touches shared game state (the engine forbids both during the phase),
  so the result does not depend on the order. Measured: no gain, because on a late save the phase *is* the heaviest
  country's update: 78 countries, 16 in flight at once, all per-country updates together 58 ms but one empire's 18 ms
  alone, and that one is taken in the first wave either way (see [`bench/RESULTS.md`](bench/RESULTS.md)). The
  per-country measurement stays on: the stats line shows the phase against its heaviest country.
- **Scripted weights memoised within one AI phase** (`ai_weight_cache`). The AI's choices (ship design components,
  buildings and districts, techs, resolutions, operations) are scored by scripted weights (`ai_weight`, `weight`,
  `ai_will_do`: a base and modifiers with triggers), all evaluated by `CMeanTimeToHappen::GetRawFactor(weight, out,
  scope)`. One ship design alone evaluates every candidate component's weight for every slot it could go into, twice
  (`AddSectionComponentsToShipDesign` runs the weapon and utility passes twice per growth stage, then upgrades), so
  the same (weight, scope) pair is evaluated many times in one country's update, and the heaviest empire's update spends
  about a tenth of its time there. During the parallel AI phase the game state does not change (the engine closes
  global state access and forbids random numbers), so the pair gives the same factor all phase long. With this on,
  each thread keeps a table of (weight, scope identity: the self, from, root and prev objects) → factor for the phase
  in progress and answers repeats from it; scopes with parameters or saved event targets are not cached, and the
  table is used only inside a country's update of the phase (the player's designer, tooltips and events evaluate as
  before). Same results. Measured on a late save (36 months, the setting alternating monthly): 90.6 % of the 2,600
  weight evaluations a phase makes are repeats, and the daily AI phase goes from 15.5 to 15.4 ms (medians 13.5 /
  13.2, 90th percentile 28.7 / 27.0): small, because the repeats are few; the component choosers' own loops cost more
  than the weights (see [`bench/RESULTS.md`](bench/RESULTS.md)).
- **Nested parallel-fors of the AI update run inline** (`ai_nested_inline`, experimental, off). Parts of a country's
  AI update (the economic forecast among them) start a parallel-for of their own from the worker thread that runs the
  country, about 36 per phase on a late save; its tasks go to the scheduler where the other workers are busy with
  their own countries, and the forking thread spin-waits for them (5 % of the heaviest empire's update in the sampled
  profile). With this on the plugin's hook of the parallel-for descriptor's constructor sets the task count of a
  descriptor built inside a country's update to one, so the engine takes its own serial path and the thread runs the
  whole range itself; same results, a parallel-for's chunks are independent of each other and of the thread. Measured
  on a late save (36 months, alternating monthly): the daily AI phase 15.58 ms off, 15.54 ms on (medians 14.4 / 14.1,
  90th percentile 27.4 / 26.6), within noise; the work the thread now does itself replaces the wait almost one for one.
- **Save files formatted without printf** (`save_fast_format`). A save is written as text by one thread while the
  game waits (about 0.6 s on a late save, half-yearly for the autosave). Every number goes through the engine's
  `sprintf` / `snprintf` thunks into the C runtime's printf machinery: `CToken(int)` with `"%d"`, `CToken(unsigned)`
  with `"%u"`, `CWriter::Write(CToken)` with `"%s%lld.%0.5d"` for fixed points, `CToken(CFixedPoint)` with
  `"%0*lld"` and `"%lld.%s"`; in the sampled profile of the serializing thread the runtime's printf engine is about a
  quarter of the serialization. The plugin hooks the two thunks and produces those formats (and `"%s"`, `"%lld"`) with
  plain integer-to-decimal code; every other format goes to the runtime as before. The text is identical: when the
  plugin loads, each fast format is checked against the runtime on a battery of values (zero, signs, extremes, every
  padding width) and is only used if every byte and the return value match. Measured on a late save over 72 months
  with the setting alternating per autosave: the save phase 617 ms off, 572 ms on per autosave (medians 613 / 569),
  20 million numbers formatted by the fast paths (see [`bench/RESULTS.md`](bench/RESULTS.md)).
- **Save files written without the per-token machinery** (`save_fast_writer`). Each piece of the text (indentation,
  key, `=`, value, newline) becomes a temporary `CToken` (a copy into a 256-byte hybrid array with a grow check), is
  written through `CWriter::Write(CToken)` as a C string (`strlen`), and lands in `CMemoryFile::WriteData`, which asks
  the file its size through a virtual call and calls `CBlob::Append`, which calls the grow check and the runtime's
  `memcpy`, for one to a few bytes each time: 31 million such writes per late save, the indentation one of them per
  tab. A quoted string is first copied into a `CString` and scanned twice for the characters to escape. In the sampled
  profile that machinery, not the formatting, is the larger half of the serialization. The plugin hooks five
  functions: `WriteData` appends in place when the blob has room; a token, a key and the indentation are written
  straight from the engine's own token table, the indentation as one run of tabs; a quoted string without a quote or
  backslash in it is written with its quotes in one piece (the engine's escaping path otherwise). Binary writers and
  files that are not plain memory files keep the engine's path. The bytes are the same: from one paused late game,
  saves with the setting off and on are identical, 100.6 MB of gamestate (`bench/scripts/save_compare.py`). Measured
  over 72 months with the setting alternating per autosave: the save phase 677 ms off, 375 ms on per autosave (medians
  669 / 361), the autosave day's wall time 1543 ms to 1245 ms.

- **Frame cap inside ticks** (`tick_render_fps`, default 10). With frame smoothing on, the engine draws a frame from inside
  the simulation whenever its refresh-rate based timer says one is due; on a high-refresh monitor that is up to the
  monitor's rate, and every such frame walks and draws the whole GUI window tree on the simulation thread. The plugin
  hooks the engine's `RenderFrame` (all 35 call sites, found by their byte pattern, must call the same function) and
  drops the calls made inside a tick that come closer together than 1 / `tick_render_fps` seconds; `0` leaves the game's
  own rate. Display only.

- **The same cap between ticks** (`render_cap_between_ticks`, default on). The engine also calls `RenderFrame` from its
  main loop between ticks, and those calls were about half of all `RenderFrame` time. With this on they follow the same
  cap, but only while the simulation is running (a tick ended in the last 250 ms); paused, in a menu or loading, nothing is
  capped. (Dropping the draw step of each loop iteration instead made the game slower: the engine's own frame logic then
  redid the GUI update more often.)

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
- `phase_profile` times the engine's phases per game day with rdtsc at their entry points: the tick and its parts (micro
  update, daily, monthly and yearly update, the AI update including the wait for the workers, the autosave), the frames
  drawn inside a tick and between ticks, and the frame function's own GUI update and draw. Every phase gets its exclusive
  time. One record per game day goes to `logs\stellaris_perf_phases.json`; `python bench\scripts\phase_report.py` groups
  the days (normal, first of a month, 1 January, 1 July) and prints ms per day and the share of the wall time, so the
  month-start and autosave spikes are visible instead of averaged away. `phase_extra` (`0xRVA[=name], ...`) adds any
  functions of the exe to the timing while the game runs (only function starts are accepted), to drill into a phase
  without rebuilding; `phase_callers` (`0xRVA` of one timed function) records that function's calls and time per call
  site, for normal and first-of-month days separately.
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
| `alloc_cache` | `1` | large-block recycler for the engine's `operator new` / `free` |
| `alloc_cache_mb` | `8` | most memory each thread keeps cached (MB, `0` = off) |
| `combat_no_copy` | `1` | share the combat participants table's object instead of copying it per ship |
| `colony_monthly_batch` | `2` | colony monthly pass: `1` forced refreshes once at the end, flush held; `2` also the colony's node frozen during its pass |
| `pop_jobs_single_pass` | `1` | monthly pop-job assignment once instead of twice (the engine's second call is one more iteration over the first's result) |
| `pop_faction_batch` | `2` | a faction's support and approval recomputed once per faction at the end of a window instead of after every pop move in it: `1` each pop-job pass, `2` the whole monthly colony update; `0` the engine's calls; `3` / `4` verify |
| `pop_jobs_node_batch` | `2` | the two modifier refreshes forced for every pop group a promotion creates: `2` skipped, the nodes left to their next reader; `1` done once each when the pass ends (no gain); `0` the engine's; `3` measurement |
| `pop_jobs_flush_first` | `0` | experimental: the pass's serial colony refresh loop: `1` the modifier manager's parallel flush first (costs more), `2` the loop skipped (not the engine's result, more rebuilds later); measurement only |
| `modifier_cascade_gate` | `2` | the country's and the relay network node's invalidations rebuild the node at once and invalidate its children only when its content changed: `2` every invalidation, `1` main-thread ones only, `3` count only, `0` the engine's cascades |
| `node_trace` | `0` | measurement: the modifier graph's work by source per 30 game days (invalidations by call site and cascade root, rebuilds by category and context, the flush), written to the log |
| `ai_parallel_lpt` | `0` | experimental: daily AI update with the countries processed heaviest first; measured no gain |
| `ai_weight_cache` | `1` | scripted weight factors memoised per (weight, scope) within one daily AI phase |
| `ai_nested_inline` | `0` | experimental: a parallel-for started inside a country's AI update runs on that thread; measured within noise |
| `save_fast_format` | `1` | the numbers of a save file formatted by plain integer code instead of the runtime's printf (same text) |
| `save_fast_writer` | `1` | the text of a save file put into the memory file without the engine's per-token temporaries, virtual size query, append and runtime memcpy (same bytes) |
| `tick_render_fps` | `10` | most frames per second drawn in the middle of a tick (`0` = the game's own rate) |
| `render_cap_between_ticks` | `1` | the same cap for the frames drawn between ticks, while the simulation runs |
| `fleet_manager_cache` | `1` | cache fleet military power in the fleet manager window |
| `fleet_manager_reinforce_ms` | `1000` | minimum ms between reinforcement recalculations (`0` = the game's own throttle) |
| `ui_power_cache` | `1` | cache the fleet military power the rest of the in-game GUI asks for (fleet window, outliner rows, tooltips) |
| `fleet_upgrade_cache` | `1` | fleet window: keep the upgrade button's affordability check for up to 1 s: `0` off, `1` on, `2` verify |
| `cursor_path_cache` | `1` | galaxy view: keep the cursor's and hover indicator's path check for up to 0.5 s: `0` off, `1` on, `2` verify |
| `tooltip_cache` | `1` | keep a tooltip's content while nothing that could change it happened: `0` off, `1` on, `2` verify |
| `tooltip_cache_ms` | `250` | the longest a tooltip is kept, in ms of real time |
| `flag_simd` | `1` | `0` engine scan, `1` SSE2 scan, `2` verify |
| `flag_expiry_skip` | `1` | skip daily expiry on containers without timed flags |
| `opinion_cache` | `0` | per-game-day opinion cache |
| `rule_cache` | `0` | `0` off, `1` every rule, `2` adaptive |
| `modifier_flush` | `0` | `0` engine, `1` serial fast path, `2` verify, `3` control |
| `modifier_flush_max` | `32` | largest dirty set that takes the serial path |
| `profile`, `rule_profile`, `scope_profile`, `phase_profile` | `0` | diagnostics |

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
build\Release\tooltip_test.exe              # the tooltip cache against a stand-in for the engine's code; no game needed
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

`.github/workflows/build-release.yml` builds both DLLs on every push and pull request, runs `tooltip_test.exe` (the
tooltip cache's call-site hooks and logic, no game needed), checks the plugin folder with
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
