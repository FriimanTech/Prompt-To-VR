"""Deterministic checks that generated C++ talks to the engine the way the
engine actually works.

``bounds.py`` decides what a generated file may INCLUDE. This module decides
whether what it does with those headers can work at runtime. Each rule here
is a bug that compiled cleanly, linked, shipped in an APK and did nothing:

* An AI written against the context-free ``IBehaviour`` form has no
  ``MechanicContext``: no navigation, no perception, no combat. It cannot
  find the real player, so it steers toward whatever it guesses.
* ``world.find("player_rig")`` reaches the scene's rig entity by name. The
  player is ``ctx.hands->player_entity()``; anything else is a guess about
  which entity moves with the headset.
* A private ``*HitInbox`` / ``*DamageQueue`` is a damage channel only its own
  task writes. The built-in enemies damage the player through ``ctx.combat``,
  so a HUD listening to the inbox never moves and the run never ends.

Violations are fed back to the codegen agent as repair context. The same
rules mark code generated before they existed as stale, so a fix to this
module regenerates exactly the tasks it applies to.
"""

from __future__ import annotations

import re
from typing import Mapping

#: Bumped whenever a rule below changes what counts as valid. The codegen
#: board stores it and re-checks every finished task when it moves.
API_CONTRACT_VERSION = 6

# --- Weapons (projects with an interaction_design.json) ---------------------
# builtin.weapon / bow / melee_weapon / shield run every designed weapon. A
# task that fires one too builds a second gun -- and the humans-vs-aliens
# pistol showed what a private one does: its ray started at the muzzle, hit
# the pistol's own collider, and no shot ever reached an alien.
_READS_MUZZLE = re.compile(r"""socket_world\s*\([^;]*"muzzle"\s*\)""")
_UNFILTERED_RAY = re.compile(r"\bphysics\s*->\s*raycast\s*\(")
_PRIVATE_AMMO = re.compile(
    r"\b(?:int|unsigned|std::uint32_t|std::int32_t|uint32_t|int32_t)\s+"
    r"\w*(?:ammo|rounds|magazine|mag_count|bullets)\w*\s*[{=;]")
# ...that the task itself spends (a HUD's display copy is fine).
_SPENDS_AMMO = re.compile(
    r"\b\w*(?:ammo|rounds|magazine|mag_count|bullets)\w*\s*(?:--|-=)"
    r"|--\s*\w*(?:ammo|rounds|magazine|mag_count|bullets)\w*")
_READS_WEAPONS = re.compile(r"\bweapons\s*->\s*(?:state|held_weapon|hits_since|hit_seq)\s*\(")
_PRIVATE_PROJECTILE = re.compile(
    r"\b(?:struct|class)\s+\w*(?:Projectile|Bullet|Tracer)\w*\b(?!\s*;)")
_FIRES_ON_TRIGGER = re.compile(r"\btrigger_pressed\s*\(|\.trigger\s*>=?")

# --- Game loop (projects with a game_design.json) ---------------------------
# builtin.game_flow owns lives, respawn, restart and the end screens. A task
# that keeps its own copy builds a second loop that fights the engine's -- the
# shape of the humans-vs-aliens failure, where the generated run flow froze
# the arena at zero health.
_PRIVATE_LIVES = re.compile(
    r"\b(?:int|unsigned|std::uint32_t|std::int32_t|uint32_t|int32_t)\s+"
    r"(?:\w+_)?(?:lives|lives_left|lives_remaining|remaining_lives)(?:_)?\s*[{=;]")
_PRIVATE_RESPAWN = re.compile(r"\b(?:void|bool)\s+\w*respawn\w*\s*\(")
_TELEPORTS = re.compile(r"\bteleport_to\s*\(|\bplace_player\s*\(")
_RESTARTS = re.compile(r"\brestart_current\s*\(|\brevive\s*\(")

_REGISTER_BEHAVIOUR = re.compile(r"\bPTV_REGISTER_BEHAVIOUR\s*\(")
# The context-taking on_tick: ``on_tick(World&, uint32_t, float, const
# InputSnapshot&, MechanicContext&)``. Parameters may span lines.
_CTX_ON_TICK = re.compile(r"\bon_tick\s*\([^;{)]*\bMechanicContext\b", re.S)
_PLAYER_BY_NAME = re.compile(
    r"""\bfind\s*\(\s*(?:std::string\s*\(\s*)?"(?:player_rig|__player)"\s*\)?\s*\)"""
)
_PRIVATE_DAMAGE_CHANNEL = re.compile(
    r"\b(?:struct|class)\s+(\w*(?:Hit|Damage)\w*(?:Inbox|Queue|Mailbox))\b"
)
# A health pool of the task's own: ``float current_health``. Legitimate only
# if it mirrors the engine's (ctx.combat); otherwise nothing that damages the
# player through ctx.combat -- every built-in enemy -- ever moves it.
_PRIVATE_HEALTH = re.compile(r"\b(?:float|double|int)\s+current_health\b")
# ...that the task itself takes damage off (a display copy is fine).
_DAMAGES_OWN_POOL = re.compile(
    r"current_health\s*(?:-=|=\s*std::max\s*\(\s*0(?:\.0)?f?\s*,)")
_READS_COMBAT = re.compile(
    r"\bcombat\s*->\s*(?:damage_seq|damage_events_since|health|max_health|is_alive)\s*\(")
# --- Interactions (v6) -------------------------------------------------------
# builtin.press_button / world_display / target_challenge / destructible /
# explosive run these. The shooting range's red button waited for
# ctx.contacts->began(button, hand) -- the hands were never colliders, so it
# never pressed; its grenade hub ran a fuse and never exploded; its timer HUD
# drew nothing anyone saw.
_CONTACT_TEST = re.compile(r"\bcontacts\s*->\s*(?:began|touching)\s*\(")
_BUTTONISH = re.compile(r"\b\w*(?:button|press|switch|lever)\w*\b", re.IGNORECASE)
_FUSE = re.compile(r"\b\w*fuse\w*\b", re.IGNORECASE)
_BLAST_DAMAGE = re.compile(
    r"(?:blast|explo\w*|radius)[^;]*;[\s\S]{0,600}?combat\s*->\s*apply_(?:weapon_)?damage"
    r"|combat\s*->\s*apply_(?:weapon_)?damage[^;]*(?:blast|explo|radius|falloff)",
    re.IGNORECASE)
# ctx.spawn->instance(template, Vec3(12.0f, 0.0f, -4.5f), ...): a place typed
# into the code instead of one the world declared.
_LITERAL_SPAWN = re.compile(
    r"spawn\s*->\s*instance\s*\([^;]*?Vec3\s*\(\s*-?\d+(?:\.\d*)?f?\s*,\s*-?\d", re.S)
_READS_ANCHORS = re.compile(r"\banchors\s*->\s*(?:point|pose_at|count|anchor)\s*\(")
_DRAWS_TEXT = re.compile(r"\btext\s*->\s*text\s*\(")
_SCOREBOARD_TEXT = re.compile(
    r'"[^"\n]*(?:time|timer|score|hits|best|seconds|left)[^"\n]*"', re.IGNORECASE)
_WRITES_VARS = re.compile(r"\bvars\s*->\s*set_(?:number|text)\s*\(")

# Functions a header offers other tasks to call: "submit_hit", "push_damage".
_OFFERED_ENTRY = re.compile(
    r"\b(?:inline\s+)?[\w:<>]+\s+((?:submit|push|enqueue|report|post|send|apply|receive)_\w+)\s*\("
)


def lint_task_sources(kind: str, files: Mapping[str, str], *,
                      game_flow: bool = False, weapons: bool = False,
                      interactions: bool = False, anchors: bool = False,
                      displays: bool = False) -> list[str]:
    """Problems with ONE task's files, phrased for the agent that wrote them.

    ``kind`` is the CodeTask kind (``ai``, ``mechanic``, ``dialogue``);
    ``files`` maps file name -> contents. ``game_flow`` is true when the
    project has a game design, i.e. ``builtin.game_flow`` runs the loop;
    ``weapons`` when its interaction design hands weapons to the engine's
    weapon built-ins; ``interactions`` when it has an interaction design at
    all (buttons, explosives and destructibles are built-ins); ``anchors``
    when the world declares gameplay anchors / anchor sets; ``displays``
    when the design has an in-world display."""

    problems: list[str] = []
    joined = "\n".join(files.values())

    if interactions:
        if _CONTACT_TEST.search(joined) and _BUTTONISH.search(joined):
            problems.append(
                "presses a button by testing contacts with the hand. Buttons are "
                "builtin.press_button (index-finger poke through the cap, or the "
                "trigger beside it); it raises a named signal. React to it with "
                "ctx.signals->fired_since(\"<signal>\", seen) -- never by contact.")
        if _FUSE.search(joined) and _BLAST_DAMAGE.search(joined):
            problems.append(
                "runs its own fuse and blast damage. Explosives are builtin.explosive "
                "(arming, fuse, radial damage through ctx.combat, flash, restock); "
                "react to the '<entity>.detonated' / 'explosive.detonated' signals "
                "on ctx.signals and to the damage through ctx.combat.")
    if anchors and _LITERAL_SPAWN.search(joined) and not _READS_ANCHORS.search(joined):
        problems.append(
            "spawns at coordinates typed into the code. The world declares where "
            "things go: read them with ctx.anchors->count(set) / point(set, i) / "
            "pose_at(set, i, &pos, &rot) and spawn there.")
    if displays and _DRAWS_TEXT.search(joined) and _SCOREBOARD_TEXT.search(joined) \
            and not _WRITES_VARS.search(joined):
        problems.append(
            "draws its own scoreboard / timer text. Screens in the world are "
            "builtin.world_display: publish the numbers with "
            "ctx.vars->set_number(\"<id>.<key>\", v) and let the display's lines "
            "template them (\"TIME {<id>.time_left:0.0}\").")

    if _READS_MUZZLE.search(joined) and _UNFILTERED_RAY.search(joined):
        problems.append(
            "casts ctx.physics->raycast from a muzzle socket. The first thing "
            "that ray hits is the weapon's own collider (or the hand holding "
            "it), so no shot ever lands. Read hits from ctx.weapons->"
            "hits_since(...) instead; if you truly need a ray, use "
            "ctx.physics->raycast_ignoring with the weapon and its wielder.")
    if weapons:
        if _READS_MUZZLE.search(joined) and _FIRES_ON_TRIGGER.search(joined):
            problems.append(
                "fires a weapon from its muzzle on the trigger. builtin.weapon "
                "fires every designed weapon (rate, spread, ammo, recoil, "
                "tracer, damage) -- remove the firing and REACT to shots with "
                "ctx.weapons->hits_since(seq, ...) / state(weapon, &s).")
        if (_PRIVATE_AMMO.search(joined) and _SPENDS_AMMO.search(joined)
                and not _READS_WEAPONS.search(joined)):
            problems.append(
                "keeps and spends its own ammo / magazine count. Ammo and "
                "reloading are builtin.weapon's; read ctx.weapons->state(weapon, &s) "
                "(s.ammo_in_magazine, s.magazine_size, s.reserve, s.infinite, "
                "s.reloading) with the weapon from ctx.weapons->held_weapon(hand).")
        if _PRIVATE_PROJECTILE.search(joined):
            problems.append(
                "defines its own projectile / bullet / tracer type. Projectiles "
                "are ctx.projectiles (pooled, swept, faction-checked) and "
                "tracers ctx.vfx; launch through them or, for a designed "
                "weapon, let builtin.weapon do it.")

    if game_flow:
        if _PRIVATE_LIVES.search(joined):
            problems.append(
                "keeps its own lives counter. builtin.game_flow owns lives, "
                "respawn and Game Over in this game; read ctx.game->lives() "
                "and ctx.game->state() instead.")
        if _PRIVATE_RESPAWN.search(joined) and _TELEPORTS.search(joined):
            problems.append(
                "implements a respawn of its own (a *respawn* function that "
                "teleports the player). builtin.game_flow respawns the player "
                "under a fade with invulnerability; remove it.")
        if _RESTARTS.search(joined):
            problems.append(
                "restarts the scene or revives the player itself. Try Again / "
                "Play Again / respawn are builtin.game_flow's; a game-specific "
                "loss is ctx.game->fail(\"reason\"), a win ctx.game->complete(id).")

    if _REGISTER_BEHAVIOUR.search(joined) and not _CTX_ON_TICK.search(joined):
        problems.append(
            "this AI registers with PTV_REGISTER_BEHAVIOUR but only implements "
            "the context-free on_tick(world, entity, dt). Override "
            "on_attach(world, entity, params, MechanicContext& ctx) and "
            "on_tick(world, entity, dt, const InputSnapshot& input, "
            "MechanicContext& ctx), and use ctx.hands->player_entity(), "
            "ctx.nav, ctx.perception and ctx.combat."
        )

    if (_PRIVATE_HEALTH.search(joined) and _DAMAGES_OWN_POOL.search(joined)
            and not _READS_COMBAT.search(joined)):
        problems.append(
            "keeps its own health pool (current_health) but never reads "
            "ctx.combat. Enemies -- built-in and generated -- damage through "
            "ctx.combat->apply_damage, so this pool never moves. Mirror "
            "ctx.combat->health(entity) / max_health(entity) and react to hits "
            "with ctx.combat->damage_seq(entity) / damage_events_since(...); "
            "defeat is !ctx.combat->is_alive(entity)."
        )

    for name, text in files.items():
        if _PLAYER_BY_NAME.search(text):
            problems.append(
                f"{name}: looks the player up by name (find(\"player_rig\")). "
                "The player is ctx.hands->player_entity(); their position is "
                "ctx.locomotion->body_position()."
            )
        for m in _PRIVATE_DAMAGE_CHANNEL.finditer(text):
            problems.append(
                f"{name}: defines a private damage channel '{m.group(1)}'. "
                "Built-in enemies and other tasks damage through "
                "ctx.combat->apply_damage and never write it. Read hits with "
                "ctx.combat->damage_seq(entity) / damage_events_since(...)."
            )
    del kind  # every rule applies to every kind; kept for call-site clarity
    return problems


def stale_contract_reasons(files: Mapping[str, str], *,
                           game_flow: bool = False, weapons: bool = False,
                           interactions: bool = False, anchors: bool = False,
                           displays: bool = False) -> list[str]:
    """Why already-generated code no longer satisfies the contract (empty =
    still fine). Same rules as :func:`lint_task_sources`."""

    return lint_task_sources("", files, game_flow=game_flow, weapons=weapons,
                             interactions=interactions, anchors=anchors, displays=displays)


def find_dead_contracts(task_sources: Mapping[str, Mapping[str, str]]) -> list[str]:
    """Entry points a task's HEADER offers other tasks that nobody calls.

    ``task_sources`` maps task id -> {file name: contents}. A
    ``submit_hit()`` that only its own task mentions is a channel whose
    producer was never written -- the shape of "the HUD never moves"."""

    warnings: list[str] = []
    for tid, files in task_sources.items():
        offered: set[str] = set()
        for name, text in files.items():
            if name.lower().endswith((".h", ".hpp")):
                offered.update(m.group(1) for m in _OFFERED_ENTRY.finditer(text))
        for fn in sorted(offered):
            pattern = re.compile(rf"\b{re.escape(fn)}\s*\(")
            used_elsewhere = any(
                pattern.search(text)
                for other, other_files in task_sources.items()
                if other != tid
                for text in other_files.values()
            )
            if not used_elsewhere:
                warnings.append(
                    f"{tid}: offers {fn}() to other tasks but no other task "
                    "calls it, so whatever it receives never arrives"
                )
    return warnings
