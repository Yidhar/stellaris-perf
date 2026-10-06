"""Generates the "zz perf stress" test mod: mod-scale load shapes found in real mods
(Ancient Empire, Gigastructural Engineering, Star Wars: Dawn of the Kuat, Star Oath, Tidy Tradition, Nyto), reproduced on vanilla content without changing
gameplay (modifiers are tiny, triggers never match, game rules keep their vanilla result).

    python gen_stress_mod.py [--out DIR] [--key value ...]      # writes the mod folder + descriptor
    python gen_stress_mod.py --enable | --disable                # edit dlc_load.json (backup kept)

Modules (scale knobs, defaults in PARAMS):
  M1 ship modifier storm (Ancient Empire / Nyto / Kuat combat loops): every `ship_period` days each
     country removes and re-adds `ship_mods` static modifiers on every military ship, outside a
     modifier batch (a ship modifier rebuild per change).
  M2 planet modifier churn (Tidy Tradition): monthly, every colony removes and re-adds
     `planet_mods` static modifiers.
  M3 country modifier churn (Gigastructures AMB): with M1, `country_mods` country modifiers.
  M4 fleet system-entry fan-out (Giga 25 / AE 56 events): `fleet_events` on_entering_system_fleet
     events whose triggers check global flags, the owner, the system and every ship, then fail.
  M5 global flags (Giga: 717 names): `global_flags` global flags set once.
  M6 heavier game rules (AE system_blocks_sensors 7 -> ~30 checks; Giga/Kuat can_orbital_bombard):
     vanilla rule copied, plus `rule_checks` checks that never change its result.
  M7 triggered opinion modifiers (AE +86): `opinion_mods` of them, triggers that fail.
  M8 mean-time-to-happen events (Kuat 7 planet + 9 country): trigger fails after a flag check.
  M9 arena (Ancient Empire / Kuat late-game doomstack wars): a sustained, huge fleet battle in one system.
     Setup, once (global flag perfstress_arena_init), from the monthly pulse of the human country
     (`is_ai = no`): pick an empty system (tier 1: unowned, no colony, no fleet of any kind, at least
     `arena_min_jumps` jumps from the player capital; tier 2: same without the distance; tier 3: any
     system without a colony), create the enemy with vanilla `create_country = { type = faction
     auto_delete = no }` (the space-monster country type: `faction = { hostile = yes }`,
     `ai = { enabled = no }`, so its fleets never get movement orders and need no war or diplomacy)
     plus `set_faction_hostility = { set_hostile = yes }`, and spawn `arena_player_size` fleet size of
     PLAYER fleets (`arena_player_fleet_size` each; the player's own designs via
     `random_existing_design`: battleship, else cruiser, else corvette, plus `arena_player_titans`
     titans each if the player has tech_titans). `create_ship` adds ships with the template flag, so
     these are normal fleet-manager fleets with templates. Enemy: `arena_enemy_size` fleet size in
     `arena_enemy_fleet_size` fleets of vanilla global event designs NAME_Mindwarden_Battleship
     (+ `arena_enemy_titans` NAME_Mindwarden_Titan per fleet: Perdition Beam, collateral/AoE damage),
     stance aggressive, aggro range `arena_aggro` measured from the return point.
     Sustain: every `arena_period` days (a self-rescheduling chain; a timed global flag lets the monthly
     pulse restart it if it ever dies) every enemy fleet slot is topped up to its size or re-created,
     and every `arena_player_refill_every`-th player fleet is topped up (re-created if destroyed, at most
     `arena_player_respawn_max` times); the other player fleets keep their losses, so their templates
     keep reinforcement demand (08 §E: D x (S + 2R) per template). `arena_no_retreat` puts
     `ship_disengage_chance_mult = -1` on arena player ships so the fight stays in the system;
     `arena_navy_mod` gives the player +naval cap and -90% ship upkeep so the economy survives.
  M9a battle fan-out (Star Oath 63 / AE 22 on_entering_battle): `battle_events` country events whose
     triggers do flag misses and then fail.
  M9b per-hit fan-out (Nyto, on_damage_taken: THIS = damaged ship, FROM = attacker): a wrapper ship
     event sets a 1-day timed flag on the attacker, `set_update_modifiers_batch` begin/end (`hit_batch`)
     and fire_on_action of a custom on_action with `hit_events` events; with `hit_cd_broken` = 1 the
     first of them removes the attacker's cooldown flag again (Nyto's never-holding gate), the others
     fail a check_modifier_value trigger.
  M9c daily in-combat loop (AE ag_ancient.2102 / Nyto weapons system): on_entering_battle starts, per
     fleet, a `days = 1` self-rescheduling fleet event (fleet flag mutex perfstress_cl_loop) that, while
     `is_in_combat = yes`, on every ship does `combat_reads` check_modifier_value, a change_variable, a
     ship flag flip and remove/add of `combat_mods` static modifier pairs (_1/_2 alternate daily),
     outside a modifier batch unless `combat_batch` = 1; when combat ends it cleans up and stops.
  M9d AoE: collateral damage comes from the enemy titans' Perdition Beam (vanilla has no chain weapons).
  M9e ship-destroyed fan-out (AE 58 victim + 34 perp): `destroyed_events` country events on each of
     on_ship_destroyed_victim / _perp, flag-miss triggers that fail.
  M10 AI pops (player pop counts in modded games are several times the AI's): once (global flag), every
     AI colony (`is_ai = yes`, default country type, pop_amount < `ai_pop_cap`) has each pop group scaled
     by `ai_pop_mult` (`scale_pop_amount`). Housing for `ai_pop_housing`% of the added pops and worker
     jobs (technician/farmer/miner, drone variants for gestalts) for `ai_pop_job_fill`% of them come from
     static planet modifiers with `multiplier` = added pop amount; colonies alternate between
     fill + `ai_pop_job_skew`% and fill - skew% jobs, so some colonies have unemployment and others vacant
     jobs, which keeps migration and job assignment busy. `ai_pop_topup` = 1 re-adds pops monthly to the
     post-scaling amount if a colony drops below it.
Set a count/size knob to 0 to disable that module (ai_pop_mult <= 1 disables M10).
  M11 event targets (Ancient Empire: 401 distinct global targets, 9398 `event_target:` refs, opinion
     modifiers comparing event targets): `et_globals` global targets saved once on random planets;
     opinion modifier triggers and fleet system-entry triggers resolve them; the M9c combat loop saves
     `et_locals` local targets on its chain base and reads `et_local_reads` of them per ship (every
     ship iteration copies the scope, i.e. deep-copies that container). Ancient Empire's hot shape, a
     dynamic name plus a second segment (`event_target:ag_shell_world_prev_layer_@this.carrier`) in
     building potential/allow and game-rule triggers, is reproduced by `et_dynamic` saved dynamic
     globals, `et_buildings` buildings and `et_rule_dyn` checks in the overridden game rules; every
     read is guarded by `exists` first, as the mod does (a missing target would log an error).
     `et_ship_dyn` scales it to per-ship work: every country saves `perfstress_cty_@this`, and the M9c
     combat loop checks, per ship and day, half owner-keyed hits with a `.capital_scope` second segment
     and half `perfstress_layer_@this` lookups from the owner (mostly misses).
All flag names are static and bounded (the 65535 flag-name cap).
Only writing to the default --out folder also registers mod/zz_perf_stress.mod in Documents.
"""
import argparse
import json
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts"))
from stellaris_paths import require_game_dir, stellaris_data_dir  # noqa: E402

DOCS = stellaris_data_dir()
NAME = "zz_perf_stress"
PARAMS = {
    "ship_period": 10,    # M1: days between ship modifier storms (per country chain)
    "ship_mods": 2,       # M1: static modifiers removed + re-added per military ship
    "country_mods": 3,    # M3
    "planet_mods": 2,     # M2
    "fleet_events": 40,   # M4
    "global_flags": 700,  # M5
    "rule_checks": 25,    # M6
    "opinion_mods": 86,   # M7
    "et_globals": 400,    # M11 global event targets saved once (AE: 401 distinct)
    "et_opinion": 1,      # M11 opinion modifier triggers resolve event targets (AE style)
    "et_fleet": 1,        # M11 fleet system-entry events gate on event targets
    "et_locals": 10,      # M11 local event targets saved on the M9c combat loop's chain base
    "et_local_reads": 5,  # M11 local event targets read per ship per combat day
    "et_dynamic": 200,    # M11 dynamic global targets `perfstress_layer_@this` saved on random planets
    "et_buildings": 6,    # M11 buildings whose potential resolves `event_target:perfstress_layer_@this.solar_system`
    "et_rule_dyn": 3,     # M11 dynamic event-target checks added to the two overridden game rules
    "et_ship_dyn": 10,    # M11 dynamic event-target checks per ship per combat day (M9c loop, main thread)
    "mtth_planet": 7,     # M8
    "mtth_country": 9,    # M8
    # M9 arena (fleet size = sum of ship-size fleet_slot_size: corvette 1, cruiser 3, battleship 4, titan 8)
    "arena_player_size": 5000,        # player fleet size spawned in the arena (0 = no arena)
    "arena_enemy_size": 6000,         # enemy fleet size kept in the arena
    "arena_player_fleet_size": 200,   # per player fleet (a late-game command limit)
    "arena_enemy_fleet_size": 400,    # per enemy fleet
    "arena_player_titans": 1,         # titans per player fleet (only with tech_titans)
    "arena_enemy_titans": 1,          # Mindwarden titans per enemy fleet (Perdition Beam AoE)
    "arena_period": 10,               # days between top-ups
    "arena_player_refill_every": 2,   # every Nth player fleet is refilled by script; others need reinforcing
    "arena_player_respawn_max": 50,   # cap on re-creating destroyed refilled player fleets (templates pile up)
    "arena_min_jumps": 5,             # tier-1 arena: at least this many jumps from the player capital
    "arena_aggro": 400,               # enemy aggro range (system units) around its spawn point
    "arena_no_retreat": 1,            # player arena ships get ship_disengage_chance_mult = -1
    "arena_navy_mod": 1,              # player gets +arena_player_size naval cap and -90% ship upkeep
    # M9a-e combat fan-out
    "battle_events": 22,              # M9a on_entering_battle events (AE 22, Star Oath 63)
    "hit_events": 3,                  # M9b events per hit behind the on_damage_taken wrapper (Nyto 3)
    "hit_cd_broken": 1,               # M9b 1 = the cooldown flag is removed again (Nyto), 0 = real cooldown
    "hit_batch": 1,                   # M9b wrap the fan-out in set_update_modifiers_batch (Nyto does)
    "combat_reads": 54,               # M9c check_modifier_value per ship per day (AE 54)
    "combat_mods": 2,                 # M9c static modifier pairs flipped per ship per day
    "combat_batch": 0,                # M9c 1 = batch the daily loop (AE has it commented out)
    "destroyed_events": 46,           # M9e events on each of on_ship_destroyed_victim / _perp
    # M10 AI pops (pop amounts are raw units: 100 = one pre-4.0 pop)
    "ai_pop_mult": 3,                 # each AI colony's pop groups are scaled by this (<= 1 disables)
    "ai_pop_cap": 50000,              # skip colonies whose pop_amount is already at or above this
    "ai_pop_housing": 100,            # % of the added pops that get housing
    "ai_pop_job_fill": 80,            # % of the added pops that get jobs, on average
    "ai_pop_job_skew": 40,            # colonies alternate fill+skew % and fill-skew % jobs
    "ai_pop_topup": 0,                # monthly: re-add pops up to the post-scaling amount (off by default)
}


def block(text, key):
    """The `key = { ... }` block of a vanilla file, braces matched."""
    m = re.search(r"(?m)^" + re.escape(key) + r"\s*=\s*\{", text)
    if not m:
        raise SystemExit(f"{key} not found in vanilla game rules")
    depth, i = 0, m.end() - 1
    while True:
        c = text[i]
        depth += c == "{"
        depth -= c == "}"
        i += 1
        if depth == 0:
            return text[m.start():i]


def ind(text, n):
    """Indent every non-empty line of `text` by n tabs."""
    return "".join((chr(9) * n + ln if ln.strip() else ln) for ln in text.splitlines(True))


# ---------------------------------------------------------------- M9 arena

ENEMY_DESIGN = ("NAME_Mindwarden_Battleship", 4)  # vanilla global event design (battleship hull)
ENEMY_TITAN = ("NAME_Mindwarden_Titan", 8)         # titan hull, Perdition Beam (collateral_damage)
ENEMY_GFX = "mindwarden_01"
NO_TITAN = "NOT = { any_owned_ship = { is_ship_size = titan } } "  # top-up: re-add titans only when all died


def enemy_ship(design):
    return f'create_ship = {{ name = random design = "{design}" prefix = no graphical_culture = "{ENEMY_GFX}" }}\n'


def player_ships(p, loop):
    """Ships for one player fleet (fleet scope, owner already set). loop(n, create) wraps each size."""
    per = p["arena_player_fleet_size"]
    t = p["arena_player_titans"]
    rest = max(per - 8 * t, 0)
    titans = ""
    if t > 0:
        titans = ("if = {\n\tlimit = { owner = { has_technology = tech_titans } }\n"
                  + ind(loop(t, "create_ship = { name = random random_existing_design = titan }\n", NO_TITAN), 1) + "}\n")
    body = ("if = {\n\tlimit = { owner = { has_technology = tech_battleships } }\n"
            + ind(loop(-(-rest // 4), "create_ship = { name = random random_existing_design = battleship }\n"), 1)
            + "}\nelse_if = {\n\tlimit = { owner = { has_technology = tech_cruisers } }\n"
            + ind(loop(-(-per // 3), "create_ship = { name = random random_existing_design = cruiser }\n"), 1)
            + "}\nelse = {\n"
            + ind(loop(per, "create_ship = { name = random random_existing_design = corvette }\n"), 1) + "}\n")
    return titans + body


def spawn_loop(n, create, extra=""):
    return f"while = {{\n\tcount = {n}\n{ind(create, 1)}}}\n" if n > 0 else ""


def topup_loop(target):
    # the fleet_size check sits inside the loop: correct whether or not `while` re-checks a limit
    def loop(n, create, extra=""):
        if n <= 0:
            return ""
        return (f"while = {{\n\tcount = {n}\n\tif = {{\n\t\tlimit = {{ fleet_size < {target} {extra}}}\n"
                f"{ind(create, 2)}\t}}\n}}\n")
    return loop


def hold_mods(p):
    if not p["arena_no_retreat"]:
        return ""
    return ("every_owned_ship = {\n\tlimit = { NOT = { has_modifier = perfstress_arena_hold } }\n"
            "\tadd_modifier = { modifier = perfstress_arena_hold days = -1 }\n}\n")


def m9_arena(p):
    """Events, on_action hooks and static modifiers of the arena (M9)."""
    if p["arena_player_size"] <= 0 or p["arena_enemy_size"] <= 0:
        return [], ""
    np_ = max(1, -(-p["arena_player_size"] // max(p["arena_player_fleet_size"], 1)))
    ne = max(1, -(-p["arena_enemy_size"] // max(p["arena_enemy_fleet_size"], 1)))
    per_e = p["arena_enemy_fleet_size"]
    et = min(p["arena_enemy_titans"], per_e // ENEMY_TITAN[1])
    eb = -(-(per_e - et * ENEMY_TITAN[1]) // ENEMY_DESIGN[1])

    def player_fleet(i):
        return f"""create_fleet = {{
	name = "NAME_Perf_Arena_Fleet"
	effect = {{
		set_owner = event_target:perfstress_arena_player
{ind(player_ships(p, spawn_loop), 2)}		set_location = {{ target = event_target:perfstress_arena_anchor distance = 30 angle = random }}
		set_fleet_stance = aggressive
		set_fleet_flag = perfstress_arena_pf_{i}
{ind(hold_mods(p), 2)}	}}
}}
"""

    def enemy_fleet_ships(loop):
        return (loop(et, enemy_ship(ENEMY_TITAN[0]), NO_TITAN) if et > 0 else "") + loop(eb, enemy_ship(ENEMY_DESIGN[0]))

    enemy_blocks = ""
    for i in range(1, ne + 1):
        enemy_blocks += f"""if = {{
	limit = {{ any_owned_fleet = {{ has_fleet_flag = perfstress_arena_ef_{i} }} }}
	random_owned_fleet = {{
		limit = {{ has_fleet_flag = perfstress_arena_ef_{i} }}
{ind(enemy_fleet_ships(topup_loop(per_e)), 2)}	}}
}}
else = {{
	create_fleet = {{
		name = "NAME_Perf_Arena_Horde"
		effect = {{
			set_owner = event_target:perfstress_arena_enemy
{ind(enemy_fleet_ships(spawn_loop), 3)}			set_location = {{ target = event_target:perfstress_arena_anchor distance = 30 angle = random }}
			set_fleet_stance = aggressive
			set_aggro_range_measure_from = return_point
			set_aggro_range = {p["arena_aggro"]}
			set_fleet_flag = perfstress_arena_ef_{i}
		}}
	}}
}}
"""

    refill = ""
    every = p["arena_player_refill_every"]
    if every > 0:
        for i in range(1, np_ + 1):
            if i % every:
                continue
            refill += f"""if = {{
	limit = {{ any_owned_fleet = {{ has_fleet_flag = perfstress_arena_pf_{i} }} }}
	random_owned_fleet = {{
		limit = {{ has_fleet_flag = perfstress_arena_pf_{i} }}
{ind(player_ships(p, topup_loop(p["arena_player_fleet_size"])), 2)}{ind(hold_mods(p), 2)}	}}
}}
else_if = {{
	limit = {{ check_variable = {{ which = perfstress_arena_respawns value < {p["arena_player_respawn_max"]} }} }}
	change_variable = {{ which = perfstress_arena_respawns value = 1 }}
{ind(player_fleet(i), 1)}}}
"""

    def system_pick(limit):
        return (f"if = {{\n\tlimit = {{ NOT = {{ exists = event_target:perfstress_arena }} }}\n"
                f"\trandom_system = {{\n\t\tlimit = {{\n{ind(limit, 3)}\t\t}}\n"
                f"\t\tsave_global_event_target_as = perfstress_arena\n\t}}\n}}\n")

    empty = ("NOT = { any_system_planet = { is_colony = yes } }\n"
             "NOT = { any_fleet_in_system = { always = yes } }\n")
    picks = (system_pick("has_owner = no\n" + empty +
                         f"distance = {{ source = root.capital_scope min_jumps = {p['arena_min_jumps']} }}\n")
             + system_pick("has_owner = no\n" + empty)
             + system_pick("NOT = { any_system_planet = { is_colony = yes } }\nis_capital_system = no\n"))
    navy = "add_modifier = { modifier = perfstress_arena_navy days = -1 }\n" if p["arena_navy_mod"] else ""
    players = "".join(player_fleet(i) for i in range(1, np_ + 1))
    period = max(p["arena_period"], 1)

    ev = [f"""
# M9 arena setup (human country, once)
country_event = {{
	id = perfstress.500
	hide_window = yes
	is_triggered_only = yes
	immediate = {{
		set_global_flag = perfstress_arena_init
		save_global_event_target_as = perfstress_arena_player
		set_variable = {{ which = perfstress_arena_respawns value = 0 }}
{ind(picks, 2)}		if = {{
			limit = {{ exists = event_target:perfstress_arena }}
			event_target:perfstress_arena = {{
				set_star_flag = perfstress_arena
				random_system_planet = {{
					limit = {{ is_star = yes }}
					save_global_event_target_as = perfstress_arena_anchor
				}}
				if = {{
					limit = {{ NOT = {{ exists = event_target:perfstress_arena_anchor }} }}
					random_system_planet = {{ save_global_event_target_as = perfstress_arena_anchor }}
				}}
			}}
		}}
		if = {{
			limit = {{ exists = event_target:perfstress_arena_anchor }}
			create_country = {{
				name = "NAME_Perf_Arena_Horde"
				type = faction
				auto_delete = no
				flag = {{
					icon = {{ category = "pirate" file = "flag_pirate_7.dds" }}
					background = {{ category = "backgrounds" file = "00_solid.dds" }}
					colors = {{ "red" "red" "null" "null" }}
				}}
			}}
			last_created_country = {{
				save_global_event_target_as = perfstress_arena_enemy
				set_country_flag = perfstress_arena_enemy
				set_faction_hostility = {{ set_hostile = yes }}
				set_faction_hostility = {{ target = root set_hostile = yes }}
			}}
{ind(navy, 3)}{ind(players, 3)}			country_event = {{ id = perfstress.510 }}
		}}
	}}
}}

# M9 arena sustain chain (every arena_period days)
country_event = {{
	id = perfstress.510
	hide_window = yes
	is_triggered_only = yes
	immediate = {{
		set_timed_global_flag = {{ flag = perfstress_arena_alive days = {period * 3} }}
		if = {{
			limit = {{
				exists = event_target:perfstress_arena_enemy
				exists = event_target:perfstress_arena_anchor
			}}
			event_target:perfstress_arena_enemy = {{
{ind(enemy_blocks, 4)}			}}
		}}
		if = {{
			limit = {{
				exists = event_target:perfstress_arena_player
				exists = event_target:perfstress_arena_anchor
			}}
			event_target:perfstress_arena_player = {{
{ind(refill, 4)}			}}
		}}
		country_event = {{ id = perfstress.510 days = {period} }}
	}}
}}
"""]
    # started from perfstress.1 (monthly pulse, human country)
    hook = """if = {
	limit = { is_ai = no NOT = { has_global_flag = perfstress_arena_init } }
	country_event = { id = perfstress.500 }
}
else_if = {
	limit = { is_ai = no has_global_flag = perfstress_arena_init NOT = { has_global_flag = perfstress_arena_alive } }
	country_event = { id = perfstress.510 }
}
"""
    return ev, hook


def m9_fanout(p):
    """M9a battle events, M9b per-hit events, M9c daily combat loop, M9e ship-destroyed events.
    Returns (events, {on_action: [event ids]}, static modifier lines)."""
    ev, hooks, mods = [], {}, []
    # M9a
    for i in range(1, p["battle_events"] + 1):
        ev.append(f"""
country_event = {{
	id = perfstress.{2000 + i}
	hide_window = yes
	is_triggered_only = yes
	trigger = {{
		NOT = {{ has_global_flag = perfstress_be_gate_{i} }}
		NOT = {{ has_country_flag = perfstress_be_{i} }}
		exists = fromfrom
		fromfrom = {{ has_fleet_flag = perfstress_be_flag_{i} }}
	}}
	immediate = {{ set_global_flag = perfstress_never_happens }}
}}
""")
        hooks.setdefault("on_entering_battle", []).append(f"perfstress.{2000 + i}")
    # M9b
    if p["hit_events"] > 0:
        b0, b1 = ("\t\tset_update_modifiers_batch = begin\n", "\t\tset_update_modifiers_batch = end\n") if p["hit_batch"] else ("", "")
        ev.append(f"""
ship_event = {{
	id = perfstress.3000
	hide_window = yes
	is_triggered_only = yes
	trigger = {{
		has_global_flag = perfstress_init
		exists = from
		from = {{ NOT = {{ has_ship_flag = perfstress_hit_cd }} }}
	}}
	immediate = {{
		from = {{ set_timed_ship_flag = {{ flag = perfstress_hit_cd days = 1 }} }}
{b0}		fire_on_action = {{ on_action = perfstress_on_hit_f scopes = {{ from = from }} }}
{b1}	}}
}}
""")
        hooks.setdefault("on_damage_taken", []).append("perfstress.3000")
        for i in range(1, p["hit_events"] + 1):
            if i == 1 and p["hit_cd_broken"]:
                trig = "\ttrigger = { exists = from }\n"
                imm = "\timmediate = { from = { remove_ship_flag = perfstress_hit_cd } }\n"
            else:
                trig = ("\ttrigger = {\n\t\tcheck_modifier_value = { modifier = ship_armor_mult value > 1000 }\n"
                        f"\t\thas_ship_flag = perfstress_hit_flag_{i}\n\t}}\n")
                imm = "\timmediate = { set_global_flag = perfstress_never_happens }\n"
            ev.append(f"\nship_event = {{\n\tid = perfstress.{3000 + i}\n\thide_window = yes\n\tis_triggered_only = yes\n{trig}{imm}}}\n")
            hooks.setdefault("perfstress_on_hit_f", []).append(f"perfstress.{3000 + i}")
    # M9c
    if p["combat_reads"] > 0 or p["combat_mods"] > 0:
        pairs = [(f"perfstress_cl_{k}_1", f"perfstress_cl_{k}_2") for k in range(1, p["combat_mods"] + 1)]
        for a, b in pairs:
            mods += [f"{a} = {{ ship_fire_rate_mult = 0.00001 }}", f"{b} = {{ ship_fire_rate_mult = 0.00001 }}"]
        reads = "".join(
            f"if = {{ limit = {{ check_modifier_value = {{ modifier = {m} value > 1000 }} }} set_ship_flag = perfstress_never_{k % 4} }}\n"
            for k, m in ((k, ("ship_fire_rate_mult", "ship_weapon_damage", "ship_armor_mult")[k % 3])
                         for k in range(p["combat_reads"])))
        flip_to_1 = "".join(f"remove_modifier = {b}\nadd_modifier = {{ modifier = {a} days = -1 }}\n" for a, b in pairs)
        flip_to_2 = "".join(f"remove_modifier = {a}\nadd_modifier = {{ modifier = {b} days = -1 }}\n" for a, b in pairs)
        clean = "".join(f"remove_modifier = {a}\nremove_modifier = {b}\n" for a, b in pairs)
        b0, b1 = ("set_update_modifiers_batch = begin\n", "set_update_modifiers_batch = end\n") if p["combat_batch"] else ("", "")
        local_reads = "".join(
            f"if = {{ limit = {{ NOT = {{ exists = event_target:perfstress_lt_{1 + k % max(p['et_locals'], 1)} }} }} "
            f"set_ship_flag = perfstress_never_lt }}\n" for k in range(p["et_local_reads"] if p["et_locals"] else 0))
        local_saves = "".join(
            f"{('owner', 'solar_system', 'this')[k % 3]} = {{ save_event_target_as = perfstress_lt_{k + 1} }}\n"
            for k in range(p["et_locals"]))
        dyn = []
        for k in range(p["et_ship_dyn"]):
            if k % 2 == 0:
                dyn.append("\tif = {\n\t\tlimit = {\n\t\t\texists = event_target:perfstress_cty_@this\n"
                           f"\t\t\tevent_target:perfstress_cty_@this.capital_scope = {{ has_planet_flag = perfstress_never_{k} }}\n"
                           "\t\t}\n\t\tset_country_flag = perfstress_never_lt\n\t}\n")
            else:
                dyn.append("\tif = {\n\t\tlimit = {\n\t\t\texists = event_target:perfstress_layer_@this\n"
                           f"\t\t\tevent_target:perfstress_layer_@this.solar_system = {{ has_star_flag = perfstress_never_{k} }}\n"
                           "\t\t}\n\t\tset_country_flag = perfstress_never_lt\n\t}\n")
        ship_dyn = f"owner = {{\n{''.join(dyn)}}}\n" if dyn else ""
        ship_body = (reads + local_reads + ship_dyn + "change_variable = { which = perfstress_cl_days value = 1 }\n"
                     + "if = {\n\tlimit = { NOT = { has_ship_flag = perfstress_cl_flip } }\n\tset_ship_flag = perfstress_cl_flip\n"
                     + ind(flip_to_1, 1) + "}\nelse = {\n\tremove_ship_flag = perfstress_cl_flip\n" + ind(flip_to_2, 1) + "}\n")
        start = ("if = {\n\tlimit = { NOT = { has_fleet_flag = perfstress_cl_loop } }\n\tset_fleet_flag = perfstress_cl_loop\n"
                 "\tfleet_event = { id = perfstress.801 days = 1 }\n}\n")
        ev.append(f"""
# M9c: start one daily loop per fleet entering battle (fleet flag = mutex)
country_event = {{
	id = perfstress.800
	hide_window = yes
	is_triggered_only = yes
	immediate = {{
		if = {{
			limit = {{ exists = fromfrom }}
			fromfrom = {{
{ind(start, 4)}			}}
		}}
		if = {{
			limit = {{ exists = fromfromfrom }}
			fromfromfrom = {{
{ind(start, 4)}			}}
		}}
	}}
}}

fleet_event = {{
	id = perfstress.801
	hide_window = yes
	is_triggered_only = yes
	immediate = {{
		if = {{
			limit = {{ is_in_combat = yes }}
{ind(local_saves, 3)}{ind(b0, 3)}			every_owned_ship = {{
{ind(ship_body, 4)}			}}
{ind(b1, 3)}			fleet_event = {{ id = perfstress.801 days = 1 }}
		}}
		else = {{
			remove_fleet_flag = perfstress_cl_loop
			every_owned_ship = {{
				remove_ship_flag = perfstress_cl_flip
{ind(clean, 4)}			}}
		}}
	}}
}}
""")
        hooks.setdefault("on_entering_battle", []).append("perfstress.800")
    # M9e
    for side, base in (("victim", 4000), ("perp", 5000)):
        for i in range(1, p["destroyed_events"] + 1):
            ev.append(f"""
country_event = {{
	id = perfstress.{base + i}
	hide_window = yes
	is_triggered_only = yes
	trigger = {{
		NOT = {{ has_global_flag = perfstress_sd_gate_{i} }}
		exists = from
		from = {{ NOT = {{ has_country_flag = perfstress_sd_{i} }} }}
		exists = fromfrom
		fromfrom = {{ has_ship_flag = perfstress_sd_flag_{i} }}
	}}
	immediate = {{ set_global_flag = perfstress_never_happens }}
}}
""")
            hooks.setdefault(f"on_ship_destroyed_{side}", []).append(f"perfstress.{base + i}")
    return ev, hooks, mods


# ---------------------------------------------------------------- M10 AI pops

def m10_ai_pops(p):
    """Returns (events, hook for perfstress.1, static modifier lines)."""
    mult = p["ai_pop_mult"]
    if mult <= 1:
        return [], "", []
    fill, skew = p["ai_pop_job_fill"] / 100, p["ai_pop_job_skew"] / 100
    hi, lo = fill + skew, max(fill - skew, 0.0)
    mods = ["perfstress_ai_housing = { planet_housing_add = 1 }",
            "perfstress_ai_jobs = { job_technician_add = 0.34 job_farmer_add = 0.33 job_miner_add = 0.33 }",
            "perfstress_ai_jobs_gestalt = { job_technician_drone_add = 0.34 job_agri_drone_add = 0.33 job_mining_drone_add = 0.33 }"]
    ev = [f"""
# M10 AI pops: once, for every AI empire colony
country_event = {{
	id = perfstress.900
	hide_window = yes
	is_triggered_only = yes
	immediate = {{
		set_global_flag = perfstress_aipops_init
		every_country = {{
			limit = {{ is_ai = yes is_country_type = default }}
			every_owned_planet = {{
				limit = {{
					is_colony = yes
					NOT = {{ has_planet_flag = perfstress_aipop_done }}
					pop_amount > 0
					pop_amount < {p["ai_pop_cap"]}
				}}
				set_planet_flag = perfstress_aipop_done
				set_variable = {{ which = perfstress_pop_add value = trigger:pop_amount }}
				every_owned_pop_group = {{ scale_pop_amount = {mult} }}
				set_variable = {{ which = perfstress_pop_target value = trigger:pop_amount }}
				multiply_variable = {{ which = perfstress_pop_add value = {mult - 1} }}
				set_variable = {{ which = perfstress_pop_housing value = perfstress_pop_add }}
				multiply_variable = {{ which = perfstress_pop_housing value = {p["ai_pop_housing"] / 100:g} }}
				add_modifier = {{ modifier = perfstress_ai_housing days = -1 multiplier = perfstress_pop_housing }}
				set_variable = {{ which = perfstress_pop_jobs value = perfstress_pop_add }}
				if = {{
					limit = {{ owner = {{ has_country_flag = perfstress_aipop_alt }} }}
					multiply_variable = {{ which = perfstress_pop_jobs value = {hi:g} }}
					owner = {{ remove_country_flag = perfstress_aipop_alt }}
				}}
				else = {{
					multiply_variable = {{ which = perfstress_pop_jobs value = {lo:g} }}
					owner = {{ set_country_flag = perfstress_aipop_alt }}
				}}
				if = {{
					limit = {{ owner = {{ is_gestalt = yes }} }}
					add_modifier = {{ modifier = perfstress_ai_jobs_gestalt days = -1 multiplier = perfstress_pop_jobs }}
				}}
				else = {{
					add_modifier = {{ modifier = perfstress_ai_jobs days = -1 multiplier = perfstress_pop_jobs }}
				}}
			}}
		}}
	}}
}}
"""]
    hook = ("if = {\n\tlimit = { NOT = { has_global_flag = perfstress_aipops_init } }\n"
            "\tcountry_event = { id = perfstress.900 }\n}\n")
    if p["ai_pop_topup"]:
        hook += """if = {
	limit = { is_ai = yes has_global_flag = perfstress_aipops_init }
	every_owned_planet = {
		limit = {
			has_planet_flag = perfstress_aipop_done
			pop_amount > 0
			pop_amount < perfstress_pop_target
		}
		set_variable = { which = perfstress_pop_def value = perfstress_pop_target }
		subtract_variable = { which = perfstress_pop_def value = trigger:pop_amount }
		# add_pop_amount with amount = -1 divides by zero in the engine (crash, 4.5.1): only
		# positive deficits reach it
		if = {
			limit = { check_variable = { which = perfstress_pop_def value >= 1 } }
			random_owned_pop_group = {
				add_pop_amount = { amount = prev.perfstress_pop_def }
			}
		}
	}
}
"""
    return ev, hook, mods


def gen(p, out):
    files = {}
    ship = [f"perfstress_ship_{i}" for i in range(1, p["ship_mods"] + 1)]
    country = [f"perfstress_country_{i}" for i in range(1, p["country_mods"] + 1)]
    planet = [f"perfstress_planet_{i}" for i in range(1, p["planet_mods"] + 1)]

    # tiny, non-empty modifiers: an empty one would make the engine's merge return at once
    files["common/static_modifiers/zz_perf_stress_modifiers.txt"] = "\n".join(
        [f"{m} = {{ ship_fire_rate_mult = 0.00001 }}" for m in ship] +
        [f"{m} = {{ country_unity_produces_mult = 0.00001 }}" for m in country] +
        [f"{m} = {{ planet_stability_add = 0.001 }}" for m in planet]) + "\n"
    arena_ev, arena_hook = m9_arena(p)
    fan_ev, fan_hooks, fan_mods = m9_fanout(p)
    pop_ev, pop_hook, pop_mods = m10_ai_pops(p)
    extra_mods = fan_mods + pop_mods
    if arena_ev:
        extra_mods.append("perfstress_arena_hold = { ship_disengage_chance_mult = -1 }")
        extra_mods.append(f"perfstress_arena_navy = {{ country_naval_cap_add = {p['arena_player_size']} ships_upkeep_mult = -0.9 }}")
    files["common/static_modifiers/zz_perf_stress_modifiers.txt"] += "".join(m + "\n" for m in extra_mods)

    def churn(mods):
        return "".join(f"\t\t\t\tremove_modifier = {m}\n\t\t\t\tadd_modifier = {{ modifier = {m} days = -1 }}\n" for m in mods)

    gflags = "".join(f"\t\t\tset_global_flag = perfstress_gf_{i}\n" for i in range(1, p["global_flags"] + 1))
    # M11: global event targets, saved once (a guard of their own, so older stress saves get them too)
    et_hook, et_ev = "", []
    if p["et_globals"] > 0:
        saves = "".join(f"\t\trandom_galaxy_planet = {{ save_global_event_target_as = perfstress_gt_{i} }}\n"
                        for i in range(1, p["et_globals"] + 1))
        if p["et_dynamic"]:
            # one dynamic global target per colony, keyed by the colony (Ancient Empire keys its
            # shell-world layers the same way); `@this` inside random_galaxy_planet gave one name only
            saves += ("\t\tevery_galaxy_planet = {\n\t\t\tlimit = { is_colony = yes }\n"
                      "\t\t\tsave_global_event_target_as = perfstress_layer_@this\n\t\t}\n")
        et_ev.append(f"""
country_event = {{
	id = perfstress.1100
	hide_window = yes
	is_triggered_only = yes
	immediate = {{
		set_global_flag = perfstress_et_init
{saves}	}}
}}
""")
        et_hook = ("if = {\n\tlimit = { is_ai = no NOT = { has_global_flag = perfstress_et_init } }\n"
                   "\tcountry_event = { id = perfstress.1100 }\n}\n")
    if p["et_ship_dyn"]:
        et_ev.append("""
country_event = {
	id = perfstress.1101
	hide_window = yes
	is_triggered_only = yes
	immediate = {
		set_global_flag = perfstress_et_cty_init
		every_country = { save_global_event_target_as = perfstress_cty_@this }
	}
}
""")
        et_hook += ("if = {\n\tlimit = { is_ai = no NOT = { has_global_flag = perfstress_et_cty_init } }\n"
                    "\tcountry_event = { id = perfstress.1101 }\n}\n")
    ev = [f"namespace = perfstress\n"]
    # M5 setup + M1/M3 chain start + M2, from the monthly country pulse
    ev.append(f"""
country_event = {{
	id = perfstress.1
	hide_window = yes
	is_triggered_only = yes
	trigger = {{ is_country_type = default }}
	immediate = {{
		if = {{
			limit = {{ NOT = {{ has_global_flag = perfstress_init }} }}
			set_global_flag = perfstress_init
{gflags}		}}
		if = {{
			limit = {{ NOT = {{ has_country_flag = perfstress_chain }} }}
			set_country_flag = perfstress_chain
			country_event = {{ id = perfstress.2 days = {p["ship_period"]} }}
		}}
		every_owned_planet = {{
			limit = {{ is_colony = yes }}
{churn(planet).replace(chr(9) * 4, chr(9) * 3)}		}}
{ind(arena_hook + pop_hook + et_hook, 2)}	}}
}}
""")
    ev += arena_ev + pop_ev + fan_ev + et_ev
    ev.append(f"""
country_event = {{
	id = perfstress.2
	hide_window = yes
	is_triggered_only = yes
	immediate = {{
		every_owned_fleet = {{
			limit = {{ is_ship_class = shipclass_military }}
			every_owned_ship = {{
{churn(ship)}			}}
		}}
{churn(country).replace(chr(9) * 4, chr(9) * 2)}		country_event = {{ id = perfstress.2 days = {p["ship_period"]} }}
	}}
}}
""")
    def et_fleet_gate(i):
        if not (p["et_fleet"] and p["et_globals"] > 0):
            return ""
        a = 1 + (i * 7) % p["et_globals"]
        b = 1 + (i * 13) % p["et_globals"]
        return (f"\t\texists = event_target:perfstress_gt_{a}\n"
                f"\t\tNOT = {{ is_same_value = event_target:perfstress_gt_{b} }}\n")

    # M4 fleet system-entry fan-out
    for i in range(1, p["fleet_events"] + 1):
        ev.append(f"""
fleet_event = {{
	id = perfstress.{100 + i}
	hide_window = yes
	is_triggered_only = yes
	trigger = {{
		has_global_flag = perfstress_gf_{1 + (i * 17) % max(p["global_flags"], 1)}
		NOT = {{ has_global_flag = perfstress_gate_{i} }}
{et_fleet_gate(i)}
		exists = owner
		owner = {{ is_country_type = default }}
		solar_system = {{ NOT = {{ has_star_flag = perfstress_star_{i} }} }}
		any_owned_ship = {{ has_ship_flag = perfstress_ship_flag_{i} }}
	}}
	immediate = {{ set_global_flag = perfstress_never_happens }}
}}
""")
    # M8 mean-time-to-happen events
    for i in range(1, p["mtth_planet"] + 1):
        ev.append(f"""
planet_event = {{
	id = perfstress.{300 + i}
	hide_window = yes
	trigger = {{
		is_colony = yes
		has_planet_flag = perfstress_mtth_{i}
	}}
	mean_time_to_happen = {{
		months = 120
		modifier = {{ factor = 0.5 has_planet_flag = perfstress_mtth_mod_{i} }}
	}}
	immediate = {{ remove_planet_flag = perfstress_mtth_{i} }}
}}
""")
    for i in range(1, p["mtth_country"] + 1):
        ev.append(f"""
country_event = {{
	id = perfstress.{400 + i}
	hide_window = yes
	trigger = {{
		is_country_type = default
		has_country_flag = perfstress_mtth_{i}
	}}
	mean_time_to_happen = {{
		months = 120
		modifier = {{ factor = 0.5 has_country_flag = perfstress_mtth_mod_{i} }}
	}}
	immediate = {{ remove_country_flag = perfstress_mtth_{i} }}
}}
""")
    files["events/zz_perf_stress_events.txt"] = "".join(ev)

    hooks = {"on_monthly_pulse_country": ["perfstress.1"],
             "on_entering_system_fleet": [f"perfstress.{100 + i}" for i in range(1, p["fleet_events"] + 1)]}
    for k, v in fan_hooks.items():
        hooks.setdefault(k, []).extend(v)
    # vanilla on_actions get our events appended (on_action events lists merge); perfstress_on_hit_f is ours
    files["common/on_actions/zz_perf_stress_on_actions.txt"] = "\n".join(
        f"{k} = {{\n\tevents = {{\n" + "".join(f"\t\t{e}\n" for e in v) + "\t}\n}\n"
        for k, v in hooks.items() if v)

    # M6 game rules: vanilla definition + checks that never change the result
    rules = open(os.path.join(require_game_dir(), "common", "game_rules", "00_rules.txt"), encoding="utf-8-sig").read()
    rules = rules.replace("\r\n", "\n")
    sensors = block(rules, "system_blocks_sensors")  # OR: add checks that are always false
    extra_or = "".join(f"\t\thas_star_flag = perfstress_rule_{i}\n" for i in range(1, p["rule_checks"] + 1))
    if p["et_rule_dyn"]:
        # OR: false for every system (the planet-keyed names never match a system; the star flag never exists)
        extra_or += "".join("\t\tAND = {\n\t\t\texists = event_target:perfstress_layer_@this\n"
                            "\t\t\tevent_target:perfstress_layer_@this.solar_system = { has_star_flag = perfstress_never }\n\t\t}\n"
                            for _ in range(p["et_rule_dyn"]))
    sensors = sensors.replace("\tOR = {\n", "\tOR = {\n" + extra_or, 1)
    bombard = block(rules, "can_orbital_bombard")  # implicit AND: add checks that are always true
    extra_and = "".join(f"\tNOT = {{ has_global_flag = perfstress_rule_{i} }}\n" for i in range(1, p["rule_checks"] + 1))
    if p["et_rule_dyn"]:
        # implicit AND: true for every planet (NOT of a check that needs a star flag nobody sets)
        extra_and += "".join("\tNOT = {\n\t\tAND = {\n\t\t\texists = event_target:perfstress_layer_@this\n"
                             "\t\t\tevent_target:perfstress_layer_@this.solar_system = { has_star_flag = perfstress_never }\n\t\t}\n\t}\n"
                             for _ in range(p["et_rule_dyn"]))
    bombard = bombard.replace("{\n", "{\n" + extra_and, 1)
    files["common/game_rules/zz_perf_stress_rules.txt"] = sensors + "\n\n" + bombard + "\n"

    def et_opinion_check(i):
        if not (p["et_opinion"] and p["et_globals"] > 0):
            return ""
        a = 1 + (i * 11) % p["et_globals"]
        return (f"\t\texists = event_target:perfstress_gt_{a}\n"
                f"\t\tFROM = {{ NOT = {{ is_same_value = event_target:perfstress_gt_{a} }} }}\n")

    # M11 buildings: potential shaped like Ancient Empire's shell-world buildings, never true
    if p["et_buildings"] and p["et_dynamic"]:
        files["common/buildings/zz_perf_stress_buildings.txt"] = "".join(f"""
perfstress_building_{i} = {{
	base_buildtime = 360
	category = amenity
	potential = {{
		exists = owner
		OR = {{
			AND = {{
				exists = event_target:perfstress_layer_@this
				event_target:perfstress_layer_@this.solar_system = {{ has_star_flag = perfstress_never_{i} }}
			}}
			AND = {{
				exists = event_target:perfstress_layer_@this
				event_target:perfstress_layer_@this.owner = {{ has_country_flag = perfstress_never_{i} }}
			}}
		}}
	}}
	resources = {{
		category = planet_buildings
		cost = {{ minerals = 100 }}
	}}
}}
""" for i in range(1, p["et_buildings"] + 1))

    # M7 triggered opinion modifiers (THIS = opinion holder, FROM = target)
    files["common/opinion_modifiers/zz_perf_stress_opinion.txt"] = "".join(f"""
triggered_opinion_perfstress_{i} = {{
	trigger = {{
		is_country_type = default
{et_opinion_check(i)}		FROM = {{ has_country_flag = perfstress_opinion_{i} }}
	}}
	opinion = {{ base = 1 }}
}}
""" for i in range(1, p["opinion_mods"] + 1))

    if os.path.isdir(out):
        shutil.rmtree(out)
    for rel, text in files.items():
        path = os.path.join(out, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:  # script files: no BOM (the game reads it as part of the first token)
            f.write(text)
    desc = f'name="zz perf stress"\nversion="1"\ntags={{\n\t"Utilities"\n}}\nsupported_version="v4.5.*"\n'
    with open(os.path.join(out, "descriptor.mod"), "w", encoding="utf-8") as f:
        f.write(desc)
    default_out = os.path.join(DOCS, "mod", NAME)
    if os.path.normcase(os.path.abspath(out)) == os.path.normcase(os.path.abspath(default_out)):
        # register the live mod only; a scratch --out must not repoint the launcher's descriptor
        with open(os.path.join(DOCS, "mod", NAME + ".mod"), "w", encoding="utf-8") as f:
            f.write(desc + f'path="{out.replace(os.sep, "/")}"\n')
    with open(os.path.join(out, "params.json"), "w", encoding="utf-8") as f:
        json.dump(p, f, indent=1)
    print(f"wrote {len(files)} files to {out}: {p}")


def set_enabled(on):
    path = os.path.join(DOCS, "dlc_load.json")
    backup = path + ".perfstress_backup"
    if on and not os.path.exists(backup):
        shutil.copyfile(path, backup)
    cfg = json.load(open(path, encoding="utf-8"))
    mods = [m for m in cfg.get("enabled_mods", []) if m != f"mod/{NAME}.mod"]
    if on:
        mods.append(f"mod/{NAME}.mod")
    cfg["enabled_mods"] = mods
    with open(path, "w", encoding="utf-8") as f:
        json.dump(cfg, f, separators=(",", ":"))
    print(f"{path}: enabled_mods = {mods}" + (f" (backup {backup})" if on else ""))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join(DOCS, "mod", NAME))
    ap.add_argument("--enable", action="store_true")
    ap.add_argument("--disable", action="store_true")
    for k, v in PARAMS.items():
        ap.add_argument(f"--{k}", type=int, default=v)
    a = ap.parse_args()
    if a.enable or a.disable:
        set_enabled(a.enable)
        return
    gen({k: getattr(a, k) for k in PARAMS}, a.out)


if __name__ == "__main__":
    main()
