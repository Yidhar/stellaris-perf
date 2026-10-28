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

## Caveats

- The arena save is a stress case (16k ships fighting); the late-game save is closer to a normal long game and shows the
  smaller gain. Expect gains between the two, and less on a small early game.
- The engine is not deterministic in the arena save (two runs with the same settings diverge by day 5 to 10), so these
  are timing comparisons, not result comparisons.
- Small differences (under about 5 %) need many groups to see; the late-game interval is still wide.
