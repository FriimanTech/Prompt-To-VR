"""Map the game loop onto builtin.game_flow / builtin.game_hud.

Once a project has a GameDesign, lives, respawn, the run flow, the end
screens, restart and quit are the engine's. A mechanic plan that still asks
for a generated ``survival_run_flow`` or ``player_health_and_damage`` would
build a second loop that fights the first -- which is the exact shape of the
humans-vs-aliens failure, where the generated run flow paused the director at
zero health and its results screen was wired to nothing.

So, deterministically and with every change named:

* a generated item that IS the loop (run / round / game flow, lives,
  respawn, game over, victory / results / end screens, restart, pause /
  quit, player health) becomes ``builtin+config`` of ``builtin.game_flow``;
* a generated HUD that shows only what ``builtin.game_hud`` shows (health,
  lives, timer, objective) becomes ``builtin.game_hud``;
* a generated HUD with game-specific content (ammo, waves, a crosshair)
  stays generated, told not to redraw what the built-in HUD already shows;
* items that depended on a converted task lose the dependency and are told
  to read the loop through ``ctx.game`` instead.
"""

from __future__ import annotations

import re
from typing import Any, Optional

FLOW_ID = "builtin.game_flow"
HUD_ID = "builtin.game_hud"

_FLOW_NAME = re.compile(
    r"(run|round|match|game|session|survival)[\s_-]*(flow|loop|state|manager|director)"
    r"|\blives?\b|life[\s_-]*system|respawn|game[\s_-]*over|win[\s_-]*(condition|state)"
    r"|lose[\s_-]*(condition|state)|victory[\s_-]*(screen|panel|flow)"
    r"|results?[\s_-]*(screen|panel)|end[\s_-]*screen|restart|play[\s_-]*again"
    r"|try[\s_-]*again|pause[\s_-]*menu|quit|leave[\s_-]*game"
    r"|player[\s_-]*health|health[\s_-]*(system|and[\s_-]*damage|pool)",
    re.IGNORECASE,
)
_NOT_FLOW = re.compile(r"pick[\s_-]*up|potion|medkit|regen[\s_-]*pickup|enemy|npc|boss",
                       re.IGNORECASE)
_HUD_NAME = re.compile(r"\bhud\b|_hud\b|hud_|heads[\s_-]*up", re.IGNORECASE)
_HUD_SPECIFIC = re.compile(r"ammo|weapon|crosshair|reticle|wave|compass|minimap|radar",
                           re.IGNORECASE)

_HUD_NOTE = (
    " ENGINE NOTE: builtin.game_hud already shows the player's health, lives, "
    "the objective timer and the current objective on the left wrist. Do NOT "
    "draw those again; read the loop through ctx.game (state(), "
    "gameplay_active(), elapsed_s(), time_remaining_s(), lives()) only for what "
    "this HUD adds."
)


def _name_of(item: Any) -> str:
    task = item.code_task
    title = getattr(task, "title", "") if task is not None else ""
    return f"{item.id} {title}"


def reconcile_game_flow_items(mech_plan: Any, design: Optional[Any]) -> tuple[Any, list[str]]:
    """Return ``(plan, notes)``; a no-op without a GameDesign."""
    from ptv_schemas import MechanicClassification

    if design is None:
        return mech_plan, []
    notes: list[str] = []
    converted: dict[str, str] = {}
    items = []
    for it in mech_plan.items:
        if it.builtin_id in (FLOW_ID, HUD_ID):
            items.append(it)
            continue
        task = it.code_task
        if task is None:
            items.append(it)
            continue
        name = _name_of(it)
        if _HUD_NAME.search(name):
            text = f"{name} {task.description}"
            if _HUD_SPECIFIC.search(text):
                if _HUD_NOTE.strip() not in task.description:
                    task = task.model_copy(update={
                        "description": (task.description + _HUD_NOTE)[:10_000]})
                    it = it.model_copy(update={"code_task": task})
                    notes.append(f"'{it.id}' keeps its game-specific HUD but no longer "
                                 "redraws health / lives / timer (builtin.game_hud does)")
                items.append(it)
                continue
            target = HUD_ID
        elif _FLOW_NAME.search(name) and not _NOT_FLOW.search(name):
            target = FLOW_ID
        else:
            items.append(it)
            continue
        items.append(it.model_copy(update={
            "classification": MechanicClassification.builtin_config,
            "builtin_id": target,
            "base_builtin_id": None,
            "code_task": None,
            "config": {},
        }))
        converted[task.id] = target
        notes.append(f"'{it.id}' is the game loop: using {target} (from game_design.json) "
                     "instead of generating it")

    if not converted:
        return mech_plan.model_copy(update={"items": items}), notes
    final = []
    for it in items:
        task = it.code_task
        if task is not None and set(task.depends_on) & set(converted):
            gone = sorted(set(task.depends_on) & set(converted))
            task = task.model_copy(update={
                "depends_on": [d for d in task.depends_on if d not in converted],
                "description": (
                    task.description
                    + f" ENGINE NOTE: {gone} no longer exist -- the game loop is "
                    "builtin.game_flow. Do not include their headers; read the loop "
                    "through ctx.game (state(), state_seq(), gameplay_active(), "
                    "elapsed_s(), time_remaining_s(), lives(), score(), "
                    "add_progress(), complete(), fail())."
                )[:10_000],
            })
            it = it.model_copy(update={"code_task": task})
        final.append(it)
    return mech_plan.model_copy(update={"items": final}), notes
