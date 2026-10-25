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
API_CONTRACT_VERSION = 3

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
# Functions a header offers other tasks to call: "submit_hit", "push_damage".
_OFFERED_ENTRY = re.compile(
    r"\b(?:inline\s+)?[\w:<>]+\s+((?:submit|push|enqueue|report|post|send|apply|receive)_\w+)\s*\("
)


def lint_task_sources(kind: str, files: Mapping[str, str]) -> list[str]:
    """Problems with ONE task's files, phrased for the agent that wrote them.

    ``kind`` is the CodeTask kind (``ai``, ``mechanic``, ``dialogue``);
    ``files`` maps file name -> contents."""

    problems: list[str] = []
    joined = "\n".join(files.values())

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


def stale_contract_reasons(files: Mapping[str, str]) -> list[str]:
    """Why already-generated code no longer satisfies the contract (empty =
    still fine). Same rules as :func:`lint_task_sources`."""

    return lint_task_sources("", files)


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
