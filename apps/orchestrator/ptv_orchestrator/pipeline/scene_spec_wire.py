"""Strict-mode transport schema for the SceneSpec agent.

``SceneSpec`` is the artifact contract and stays exactly as it is. What it
cannot be is the *wire* schema for OpenAI structured outputs in **strict**
mode, and strict mode is the only thing that makes the output reliable:
with ``strict: true`` the model is constrained during decoding, so invalid
JSON and pattern-violating ids become impossible rather than merely
discouraged.

Two -- and only two -- things in ``SceneSpec`` block strict mode, both
open-ended dicts, which strict mode forbids because it requires
``additionalProperties: false`` everywhere:

* ``MeshRef.sockets: Dict[str, SocketRef]`` -- dropped here entirely. The
  scene compiler fills sockets in from ``asset_contract.json``; the agent
  was already told to leave it empty.
* ``MechanicBinding.params: Dict[str, object]`` -- becomes a list of
  ``(key, value_json)`` pairs.

Everything else (``Transform``, ``Vec3``, ``QuatXYZW``, ``PhysicsProps``,
``AttachmentRef``, ``HealthProps``, ``FactionProps``, ``EquipSlotSpec``,
``LightSpec``, ``PlayerSpawn``) is already strict-compatible and is reused
verbatim, so there is one definition of a transform rather than two that
drift.

Anything the wire schema does not declare, the agent CANNOT EMIT -- strict
mode permits exactly the declared surface and nothing else. So a new
``EntitySpec`` field is not optional work here: leaving it out silently makes
the field unreachable for every future game, not just the one in front of
you.

For the record, the strict-mode subset does allow ``pattern``, ``maxLength``,
``minItems``/``maxItems`` and numeric bounds for non-fine-tuned models -- only
the composition keywords (``allOf``, ``not``, ``if``/``then``/``else``) are
out. That is why the ids keep their ``IdStr`` pattern: constrained decoding
enforces it for us.

This is an LLM transport detail, not a persisted artifact, so it lives with
the stage rather than in ``packages/schemas`` and needs no exported JSON
Schema or C++ consumer.
"""

from __future__ import annotations

import json
from typing import Any, List, Optional

from pydantic import Field

from ptv_schemas import SceneSpec
from ptv_schemas.common import IdStr, StrictModel
from ptv_schemas.scene_spec import (
    AttachmentRef,
    EntitySpec,
    EquipSlotSpec,
    FactionProps,
    HealthProps,
    LightSpec,
    MechanicBinding,
    MeshRef,
    PhysicsProps,
    PlayerSpawn,
    Transform,
)

_DEFAULT_SKY = [0.05, 0.07, 0.11]


class WireParam(StrictModel):
    """One entry of what ``MechanicBinding.params`` is on the artifact side.

    ``value_json`` is the value encoded as JSON (``"0.35"``, ``"true"``,
    ``"[0,1,0]"``, ``"\\"right\\""``). A bare unquoted string is tolerated by
    the decoder below, because that is the one mistake worth forgiving.
    """

    key: str = Field(..., max_length=64)
    value_json: str = Field(
        ...,
        max_length=4096,
        description=(
            "The value as JSON: 0.35, true, [0,1,0], {\"a\":1} or a quoted"
            ' string like "right".'
        ),
    )


class WireMechanicBinding(StrictModel):
    mechanic_id: str = Field(
        ...,
        max_length=128,
        description=(
            "Either 'builtin.<name>' or 'generated.<code_task_id>', taken"
            " from the MECHANICS block."
        ),
    )
    param_overrides: List[WireParam] = Field(
        default_factory=list,
        max_length=64,
        description=(
            "Per-entity OVERRIDES only. The MechanicPlan config is merged in"
            " deterministically by the scene compiler -- do not re-emit it."
        ),
    )


class WireMeshRef(StrictModel):
    asset_id: IdStr
    glb_path: str = Field(
        ...,
        max_length=512,
        description="Use the pre-computed glb_path from the ASSETS block.",
    )


class WireEntity(StrictModel):
    id: IdStr
    name: str = Field(..., max_length=128)
    transform: Transform = Field(default_factory=Transform)
    mesh: Optional[WireMeshRef] = None
    physics: Optional[PhysicsProps] = None
    mechanic_bindings: List[WireMechanicBinding] = Field(
        default_factory=list, max_length=64
    )
    tags: List[str] = Field(default_factory=list, max_length=16)
    health: Optional[HealthProps] = None
    faction: Optional[FactionProps] = None
    attached_to: Optional[AttachmentRef] = None
    equip_slots: List[EquipSlotSpec] = Field(
        default_factory=list, max_length=24
    )


class WireSceneSpec(StrictModel):
    prompt_id: IdStr
    scene_id: IdStr
    name: str = Field(..., max_length=128)
    player_spawn: PlayerSpawn = Field(default_factory=PlayerSpawn)
    entities: List[WireEntity] = Field(..., min_length=0, max_length=1000)
    lights: List[LightSpec] = Field(default_factory=list, max_length=32)
    sky_color: List[float] = Field(
        default_factory=lambda: list(_DEFAULT_SKY),
        min_length=3,
        max_length=3,
        description="Linear RGB, each component 0..1.",
    )
    ambient_intensity: float = Field(default=0.25, ge=0.0, le=5.0)


# ---------------------------------------------------------------------------
# Deterministic wire -> artifact mapping
# ---------------------------------------------------------------------------


def decode_param_value(value_json: str) -> Any:
    """Decode one ``WireParam.value_json``.

    A model that writes ``right`` instead of ``"right"`` produces something
    that is not JSON but is obviously a string, and throwing away a whole
    scene over a missing pair of quotes would be silly.
    """
    try:
        return json.loads(value_json)
    except (json.JSONDecodeError, TypeError):
        return value_json


def _params(binding: WireMechanicBinding) -> dict[str, Any]:
    return {p.key: decode_param_value(p.value_json) for p in binding.param_overrides}


def _sky(values: List[float]) -> List[float]:
    """Three components, clamped to 0..1."""
    if len(values) != 3:
        return list(_DEFAULT_SKY)
    return [min(1.0, max(0.0, float(v))) for v in values]


def to_scene_spec(wire: WireSceneSpec) -> SceneSpec:
    """Map the strict wire object onto the real :class:`SceneSpec`."""
    entities = [
        EntitySpec(
            id=e.id,
            name=e.name,
            transform=e.transform,
            mesh=(
                MeshRef(asset_id=e.mesh.asset_id, glb_path=e.mesh.glb_path, sockets={})
                if e.mesh is not None
                else None
            ),
            physics=e.physics,
            mechanic_bindings=[
                MechanicBinding(mechanic_id=b.mechanic_id, params=_params(b))
                for b in e.mechanic_bindings
            ],
            tags=list(e.tags),
            health=e.health,
            faction=e.faction,
            attached_to=e.attached_to,
            equip_slots=list(e.equip_slots),
        )
        for e in wire.entities
    ]

    return SceneSpec(
        prompt_id=wire.prompt_id,
        scene_id=wire.scene_id,
        name=wire.name,
        player_spawn=wire.player_spawn,
        entities=entities,
        lights=list(wire.lights),
        sky_color=_sky(wire.sky_color),
        ambient_intensity=wire.ambient_intensity,
    )
