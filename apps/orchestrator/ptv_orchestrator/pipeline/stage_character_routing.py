"""Stage 7b: decide whether Tripo or Uthana rigs and animates each character.

Runs right after ``tripo_models`` because Tripo's (free) rig-check needs the
finished mesh, and before every rigging stage because they all read the
answer from ``character_routing.json``.

The decision, per character, in this order:

1. **Context.** Each NPC's brief is built from the NpcPlan, the AssetManifest,
   the BuildSpec mechanics that mention it, the user's prompt, and the exact
   prompt Gemini drew its reference image from -- plus the image itself, so
   the agent judges the body from what was actually drawn.
2. **Body and riggability.** Code runs Tripo rig-check for every NPC and
   caches it in ``tripo_rig_check.json``. The routing agent classifies the
   body plan. Any upright two-legged character with arms is a
   ``biped_humanoid``, human or not.
3. **Provider.** Fixed rules decide almost everything:

   * biped humanoid that Tripo cannot rig -> Uthana (no agent needed);
   * non-humanoid creature Tripo can rig -> Tripo;
   * non-humanoid Tripo cannot rig -> none (static / slide);

   and the agent decides only the one open case -- a biped humanoid that
   BOTH providers can rig -- by comparing what the NPC needs against both
   animation libraries. The player's avatar body always goes to Uthana (for
   its finger joints).

Every Uthana-animated character is given the shared library idle
(``settings.uthana_idle_motion_id``). One-shot actions Uthana cannot supply
are recorded as ``unmet`` with the NPC's ``animation_fallback``.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any, Optional

from ptv_providers import ProviderConfigError, TripoClient, TripoConfig
from ptv_providers.animation_catalog import (
    TRIPO_BIPED_V1_PRESETS,
    TRIPO_MAX_PRESETS_PER_ASSET,
    UTHANA_STYLES_FALLBACK,
    tripo_catalog_text,
    tripo_preset_key,
)
from ptv_providers.tripo_client import presets_for_rig
from ptv_schemas import (
    AnimationFallback,
    BodyPlan,
    CharacterRoute,
    CharacterRoutingPlan,
    MotionKind,
    MotionSource,
    MovementStyle,
    NpcPlan,
    NpcSpec,
    PlayerExperience,
    ProjectStage,
    ProjectStatus,
    RequiredMotion,
    RigProvider,
    RoutingAgentOutput,
    RoutingDecision,
    TripoRigCheckRecord,
    UnmetMotion,
)
from ptv_schemas.asset_manifest import AssetCategory, AssetItem, CharacterRole, ReferencePose

from .character_artifacts import (
    RIG_CHECK_NAME,
    load_json,
    manifest_asset_ids,
    prune_to_assets,
    rel,
    save_json,
    save_routing,
    sha256_file,
)
from .context import PipelineContext, StageResult
from .prompts import CHARACTER_ROUTING_SYSTEM
from .stage_asset_rigging import _read_task_id, _source_glb
from .stage_reference_images import compose_prompt, resolve_pose
from .structured_agent import AgentBrief, AgentImage, StructuredAgent, StructuredAgentError

_STAGE = ProjectStage.character_routing

_LOCOMOTION_BEHAVIORS = {"patrol", "chase", "flee", "reposition", "follow_leader", "take_cover"}
_LOCOMOTION_INTENTS = {"walk", "run"}
_CREATURE_RIG_TYPES = {"quadruped", "hexapod", "octopod", "avian", "serpentine", "aquatic"}
_IMAGE_EXTS = (".png", ".jpg", ".jpeg", ".webp")


def run(ctx: PipelineContext) -> StageResult:
    return run_with(ctx)


def run_with(
    ctx: PipelineContext,
    *,
    agent: Optional[StructuredAgent] = None,
    tripo_client: Any = None,
    styles: Optional[list[str]] = None,
) -> StageResult:
    """Stage entry point with injectable collaborators (tests pass fakes)."""
    ctx.emit(_STAGE, "running", "Routing characters to Tripo or Uthana")
    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(state, _STAGE, ProjectStatus.running)

    build_spec = ctx.build_spec or ctx.store.load_build_spec(ctx.project_id)
    manifest = ctx.asset_manifest or ctx.store.load_asset_manifest(ctx.project_id)
    npc_plan = _load_npc_plan(ctx)
    by_id = {i.id: i for i in manifest.items}

    avatars = [
        i for i in manifest.items
        if i.category == AssetCategory.character
        and i.character_role == CharacterRole.player_avatar
        and getattr(build_spec, "player_experience", PlayerExperience.full_avatar)
        == PlayerExperience.full_avatar
    ]
    npcs = [
        n for n in npc_plan.npcs
        if n.asset_id in by_id
        and by_id[n.asset_id].category == AssetCategory.character
        and n.movement_style != MovementStyle.slide_no_animation
        and n.animation_intents
    ]

    if not avatars and not npcs:
        plan = CharacterRoutingPlan(prompt_id=build_spec.prompt_id)
        art = save_routing(ctx.project_dir, plan)
        message = "No rigged characters; nothing to route"
        ctx.store.record_stage(state, _STAGE, ProjectStatus.succeeded, message=message,
                               artifacts=[rel(ctx.settings.repo_root, art)])
        ctx.emit(_STAGE, "succeeded", message)
        return StageResult(stage=_STAGE, artifacts=[rel(ctx.settings.repo_root, art)], data=plan)

    uthana_on, uthana_reason = uthana_available(ctx)
    if not uthana_on:
        ctx.emit(_STAGE, "warning",
                 f"Uthana unavailable ({uthana_reason}); every character routes "
                 "to Tripo and the avatar will have no finger curls")
    if styles is None:
        styles = _live_styles(ctx) if uthana_on else list(UTHANA_STYLES_FALLBACK)

    rig_checks = _rig_checks(ctx, [by_id[n.asset_id] for n in npcs], tripo_client)

    decisions: dict[str, RoutingDecision] = {}
    if npcs and uthana_on:
        decisions = _ask_agent(ctx, agent, build_spec, manifest, npcs, by_id,
                               rig_checks, styles)

    idle_id = ctx.settings.uthana_idle_motion_id
    routes: list[CharacterRoute] = []
    for item in avatars:
        routes.append(avatar_route(item, build_spec, uthana_on, idle_id))
    for npc in npcs:
        routes.append(
            npc_route(npc, by_id[npc.asset_id], rig_checks.get(npc.asset_id),
                      decisions.get(npc.asset_id), uthana_on, styles, idle_id)
        )

    plan = CharacterRoutingPlan(prompt_id=build_spec.prompt_id, routes=routes)
    art = save_routing(ctx.project_dir, plan)

    if _reconcile_manifest(manifest, plan):
        ctx.store.save_asset_manifest(ctx.project_id, manifest)
        ctx.asset_manifest = manifest

    for r in routes:
        ctx.emit(_STAGE, "info",
                 f"{r.asset_id}: {r.body_plan.value} -> rig {r.rig_provider.value}, "
                 f"animate {r.animation_provider.value} ({r.decided_by})")
        ctx.store.append_log(ctx.project_id, "character_routing",
                             f"{r.asset_id}: {r.model_dump_json()}")

    counts: dict[str, int] = {}
    for r in routes:
        counts[r.rig_provider.value] = counts.get(r.rig_provider.value, 0) + 1
    message = ", ".join(f"{v} {k}" for k, v in sorted(counts.items()))
    artifacts = [rel(ctx.settings.repo_root, art)]
    ctx.store.record_stage(state, _STAGE, ProjectStatus.succeeded, message=message,
                           artifacts=artifacts)
    ctx.emit(_STAGE, "succeeded", message)
    return StageResult(stage=_STAGE, artifacts=artifacts, data=plan)


# ---------------------------------------------------------------------------
# Availability + inputs
# ---------------------------------------------------------------------------


def uthana_available(ctx: PipelineContext) -> tuple[bool, str]:
    if not ctx.settings.uthana_enabled:
        return False, "uthana_enabled=false"
    if ctx.settings.uthana_api_key:
        os.environ.setdefault("UTHANA_API_KEY", ctx.settings.uthana_api_key)
        return True, ""
    try:
        from ptv_providers import load_uthana_api_key
        load_uthana_api_key()
    except ProviderConfigError:
        return False, "UTHANA_API_KEY is not set"
    return True, ""


def _load_npc_plan(ctx: PipelineContext) -> NpcPlan:
    if ctx.npc_plan is not None:
        return ctx.npc_plan
    try:
        return ctx.store.load_npc_plan(ctx.project_id)
    except (FileNotFoundError, ValueError):
        return NpcPlan(prompt_id="none")


def _live_styles(ctx: PipelineContext) -> list[str]:
    try:
        from ptv_providers import UthanaClient
        styles = UthanaClient(ctx.settings.uthana_api_key).list_locomotion_styles()
    except Exception as exc:  # noqa: BLE001 - the fallback list is fine
        ctx.emit(_STAGE, "warning", f"locomotion style query failed ({exc}); using the cached list")
        return list(UTHANA_STYLES_FALLBACK)
    return [s.lower() for s in styles] or list(UTHANA_STYLES_FALLBACK)


def _rig_checks(
    ctx: PipelineContext, items: list[AssetItem], client: Any
) -> dict[str, TripoRigCheckRecord]:
    """Free Tripo rig-check per NPC, cached against the source mesh hash."""
    cache_path = ctx.project_dir / RIG_CHECK_NAME
    cache = load_json(cache_path)
    out: dict[str, TripoRigCheckRecord] = {}
    for item in items:
        item_dir = ctx.project_dir / "assets" / item.id
        source = _source_glb(item_dir)
        if source is None:
            out[item.id] = TripoRigCheckRecord(error="no Tripo mesh on disk")
            continue
        digest = sha256_file(source)
        prior = cache.get(item.id)
        if isinstance(prior, dict) and prior.get("source_sha256") == digest and not prior.get("error"):
            out[item.id] = TripoRigCheckRecord(
                riggable=bool(prior.get("riggable")),
                rig_type=str(prior.get("rig_type") or ""),
                task_id=str(prior.get("task_id") or ""),
            )
            continue
        try:
            if client is None:
                client = TripoClient(TripoConfig(
                    api_key=ctx.settings.tripo_api_key,
                    **({"base_url": ctx.settings.tripo_base_url}
                       if ctx.settings.tripo_base_url else {}),
                ))
            task_id = _read_task_id(item_dir)
            check = client.rig_check(task_id=task_id, glb_path=None if task_id else source)
            record = TripoRigCheckRecord(riggable=bool(check.riggable),
                                         rig_type=str(check.rig_type or ""),
                                         task_id=str(check.task_id or ""))
        except Exception as exc:  # noqa: BLE001 - routing still has rules for this
            record = TripoRigCheckRecord(error=str(exc)[:300])
            ctx.emit(_STAGE, "warning", f"{item.id}: Tripo rig-check failed ({exc})")
        out[item.id] = record
        cache[item.id] = {**record.model_dump(), "source_sha256": digest}
    prune_to_assets(cache, manifest_asset_ids(ctx))
    save_json(cache_path, cache)
    return out


# ---------------------------------------------------------------------------
# The agent
# ---------------------------------------------------------------------------


def _reference_prompt(ctx: PipelineContext, item: AssetItem, npc_ids: set[str]) -> str:
    saved = ctx.project_dir / "images" / item.id / "reference_prompt.txt"
    if saved.is_file():
        return saved.read_text(encoding="utf-8")
    return compose_prompt(item, resolve_pose(item, npc_ids))


def _reference_image(ctx: PipelineContext, asset_id: str) -> Optional[Path]:
    d = ctx.project_dir / "images" / asset_id
    if not d.is_dir():
        return None
    files = sorted(p for p in d.iterdir() if p.suffix.lower() in _IMAGE_EXTS)
    return files[0] if files else None


def _mentions(build_spec: Any, item: AssetItem) -> list[dict[str, str]]:
    needles = {item.id.lower(), item.name.lower()}
    out = []
    for m in build_spec.mechanics:
        text = f"{m.id} {m.name} {m.summary}".lower()
        if any(n and n in text for n in needles):
            out.append({"id": m.id, "summary": m.summary[:600]})
    return out[:8]


def build_brief(
    ctx: PipelineContext,
    build_spec: Any,
    npcs: list[NpcSpec],
    by_id: dict[str, AssetItem],
    rig_checks: dict[str, TripoRigCheckRecord],
    styles: list[str],
) -> AgentBrief:
    npc_ids = {n.asset_id for n in npcs}
    prompt_path = ctx.project_dir / "prompt.txt"
    game_prompt = prompt_path.read_text(encoding="utf-8")[:8000] if prompt_path.is_file() else ""
    sections = []
    images: list[AgentImage] = []
    for npc in npcs:
        item = by_id[npc.asset_id]
        check = rig_checks.get(npc.asset_id) or TripoRigCheckRecord(error="not checked")
        sections.append(json.dumps({
            "asset_id": item.id,
            "name": item.name,
            "style_notes": item.style_notes[:800],
            "target_height_m": item.target_scale_meters,
            "npc_plan": json.loads(npc.model_dump_json()),
            "buildspec_mechanics": _mentions(build_spec, item),
            "reference_image_prompt": _reference_prompt(ctx, item, npc_ids)[:2000],
            "tripo_rig_check": check.model_dump(),
        }, indent=2))
        image = _reference_image(ctx, item.id)
        if image is not None:
            images.append(AgentImage(label=f"reference image of {item.id}", path=image))
    text = (
        "Decide the body plan and provider for each NPC below, then list the motions "
        "each needs. Return one decision per NPC.\n\n"
        "================ Game prompt (verbatim) ================\n"
        f"{game_prompt}\n\n"
        "================ Tripo v1.0 biped presets ================\n"
        f"{tripo_catalog_text()}\n\n"
        "================ Uthana locomotion styles (live) ================\n"
        f"{', '.join(styles)}\n\n"
        "================ NPCs ================\n"
        + "\n\n".join(sections)
    )
    return AgentBrief(text=text, images=images)


def _ask_agent(
    ctx: PipelineContext,
    agent: Optional[StructuredAgent],
    build_spec: Any,
    manifest: Any,
    npcs: list[NpcSpec],
    by_id: dict[str, AssetItem],
    rig_checks: dict[str, TripoRigCheckRecord],
    styles: list[str],
) -> dict[str, RoutingDecision]:
    if ctx.settings.openai_api_key and not os.environ.get("OPENAI_API_KEY"):
        os.environ["OPENAI_API_KEY"] = ctx.settings.openai_api_key
    if agent is None:
        agent = StructuredAgent(
            name=f"ptv-character-router-{ctx.project_id}",
            instructions=CHARACTER_ROUTING_SYSTEM,
            output_model=RoutingAgentOutput,
            model=ctx.settings.character_routing_model,
            reasoning_effort=ctx.settings.character_routing_reasoning_effort,
            verbosity=ctx.settings.character_routing_verbosity,
            max_turns=ctx.settings.character_routing_agent_max_turns,
            max_retries=ctx.settings.character_routing_max_retries,
            log=lambda t: ctx.store.append_log(ctx.project_id, "character_routing", t),
        )
    brief = build_brief(ctx, build_spec, npcs, by_id, rig_checks, styles)
    try:
        out = agent.run(brief)
    except StructuredAgentError as exc:
        ctx.emit(_STAGE, "warning",
                 f"routing agent unavailable ({exc}); using the deterministic fallback")
        return {}
    return {d.asset_id: d for d in out.decisions if d.asset_id in by_id}


# ---------------------------------------------------------------------------
# Rules (pure functions, unit-tested)
# ---------------------------------------------------------------------------


def guess_body_plan(item: AssetItem, check: Optional[TripoRigCheckRecord]) -> BodyPlan:
    """Body plan without the agent: from the rig-check type and the pose hint."""
    rig_type = (check.rig_type if check else "").lower()
    if rig_type in _CREATURE_RIG_TYPES:
        return BodyPlan(rig_type)
    if item.reference.pose == ReferencePose.neutral_quadruped:
        return BodyPlan.quadruped
    # A character reference that was drawn T-posed is a humanoid in practice.
    return BodyPlan.biped_humanoid


def decide_provider(
    body: BodyPlan,
    check: Optional[TripoRigCheckRecord],
    agent_choice: Optional[RigProvider],
    uthana_on: bool,
) -> tuple[RigProvider, str]:
    """``(provider, decided_by)`` from the fixed rules plus the agent's pick."""
    riggable = bool(check and check.riggable)
    unknown = check is None or bool(check.error)
    if body == BodyPlan.biped_humanoid:
        if not uthana_on:
            return (RigProvider.tripo if riggable or unknown else RigProvider.none), "code"
        if not riggable:
            # Tripo cannot (or could not be asked to) rig it; Uthana can.
            return RigProvider.uthana, "code"
        if agent_choice in (RigProvider.uthana, RigProvider.tripo):
            return agent_choice, "agent"
        return RigProvider.tripo, "code"
    if body == BodyPlan.non_creature:
        return RigProvider.none, "code"
    return (RigProvider.tripo if riggable or unknown else RigProvider.none), "code"


def _fallback_for(npc: NpcSpec) -> AnimationFallback:
    return npc.animation_fallback or AnimationFallback.none


def _uthana_motions(
    npc: NpcSpec,
    decision: Optional[RoutingDecision],
    styles: set[str],
    idle_id: str,
) -> tuple[list[RequiredMotion], list[UnmetMotion]]:
    required: list[RequiredMotion] = []
    unmet: list[UnmetMotion] = []
    seen: set[str] = set()

    def add_locomotion(key: str, purpose: str, hint: Optional[str]) -> None:
        if key in seen:
            return
        seen.add(key)
        required.append(RequiredMotion(
            key=key, kind=MotionKind.locomotion, purpose=purpose,
            source=MotionSource.uthana_locomotion,
            style_hint=hint if hint and hint.lower() in styles else None,
        ))

    for m in (decision.required_motions if decision else []):
        base = m.key.split(".", 1)[0]
        if m.kind == MotionKind.idle or m.key == "idle":
            continue  # the library idle is added below
        if m.kind == MotionKind.locomotion and base in _LOCOMOTION_INTENTS:
            add_locomotion(m.key, m.purpose, m.style_hint)
        else:
            unmet.append(UnmetMotion(key=m.key, reason="Uthana has no one-shot actions",
                                     fallback=_fallback_for(npc)))
    for u in (decision.unmet if decision else []):
        if u.key not in {x.key for x in unmet}:
            unmet.append(u)

    for intent in (a.value for a in npc.animation_intents):
        if intent in _LOCOMOTION_INTENTS:
            add_locomotion(intent, f"{intent} gait", None)
        elif intent != "idle" and intent not in {x.key for x in unmet}:
            unmet.append(UnmetMotion(key=intent, reason="Uthana has no one-shot actions",
                                     fallback=_fallback_for(npc)))
    moves = {b.value for b in npc.behaviors} & _LOCOMOTION_BEHAVIORS
    if moves and "walk" not in seen:
        add_locomotion("walk", "travels (" + ", ".join(sorted(moves)) + ")", None)

    # Every Uthana character stands still at some point; the shared library
    # idle is what it plays then.
    required.insert(0, RequiredMotion(
        key="idle", kind=MotionKind.idle, purpose="standing still",
        source=MotionSource.uthana_library, library_motion_id=idle_id,
    ))
    return required, unmet


def _tripo_motions(
    npc: NpcSpec,
    decision: Optional[RoutingDecision],
    body: BodyPlan,
    check: Optional[TripoRigCheckRecord],
) -> tuple[list[RequiredMotion], list[UnmetMotion]]:
    rig_type = (check.rig_type if check and check.rig_type else "biped")
    if body == BodyPlan.biped_humanoid:
        rig_type = "biped"
    try:
        table = presets_for_rig(rig_type)
    except ProviderConfigError:
        table = presets_for_rig("biped")
    required: list[RequiredMotion] = []
    unmet: list[UnmetMotion] = []
    keys: set[str] = set()

    def kind_of(key: str) -> MotionKind:
        if key == "idle":
            return MotionKind.idle
        return MotionKind.locomotion if key in _LOCOMOTION_INTENTS else MotionKind.action

    for intent in (a.value for a in npc.animation_intents):
        preset = table.get(intent)
        if not preset:
            unmet.append(UnmetMotion(key=intent, reason=f"no {rig_type} preset",
                                     fallback=_fallback_for(npc)))
            continue
        keys.add(intent)
        required.append(RequiredMotion(key=intent, kind=kind_of(intent),
                                       source=MotionSource.tripo_preset,
                                       tripo_preset=preset, purpose=f"{intent} clip"))
    if rig_type == "biped":
        for m in (decision.required_motions if decision else []):
            preset = m.tripo_preset
            if not preset or preset not in TRIPO_BIPED_V1_PRESETS:
                continue
            key = tripo_preset_key(preset)
            if m.key in table and table[m.key] == preset:
                key = m.key
            if key in keys:
                continue
            keys.add(key)
            required.append(RequiredMotion(key=key, kind=kind_of(key),
                                           source=MotionSource.tripo_preset,
                                           tripo_preset=preset, purpose=m.purpose))
    if len({m.tripo_preset for m in required}) > TRIPO_MAX_PRESETS_PER_ASSET:
        kept: list[RequiredMotion] = []
        presets: set[str] = set()
        for m in required:
            if m.tripo_preset in presets or len(presets) < TRIPO_MAX_PRESETS_PER_ASSET:
                presets.add(m.tripo_preset or "")
                kept.append(m)
            else:
                unmet.append(UnmetMotion(key=m.key, reason="over Tripo's 5-preset limit",
                                         fallback=_fallback_for(npc)))
        required = kept
    for u in (decision.unmet if decision else []):
        if u.key not in keys and u.key not in {x.key for x in unmet}:
            unmet.append(u)
    return required, unmet


def npc_route(
    npc: NpcSpec,
    item: AssetItem,
    check: Optional[TripoRigCheckRecord],
    decision: Optional[RoutingDecision],
    uthana_on: bool,
    styles: list[str],
    idle_id: str,
) -> CharacterRoute:
    body = decision.body_plan if decision else guess_body_plan(item, check)
    provider, decided_by = decide_provider(
        body, check, decision.rig_provider if decision else None, uthana_on
    )
    rationale = decision.rationale if decision else "deterministic rules (no agent decision)"
    if provider == RigProvider.uthana:
        required, unmet = _uthana_motions(npc, decision, {s.lower() for s in styles}, idle_id)
    elif provider == RigProvider.tripo:
        required, unmet = _tripo_motions(npc, decision, body, check)
    else:
        required = []
        unmet = [UnmetMotion(key=a.value, reason="not rigged", fallback=_fallback_for(npc))
                 for a in npc.animation_intents]
    return CharacterRoute(
        asset_id=item.id,
        character_role="npc",
        body_plan=body,
        tripo_rig_check=check,
        rig_provider=provider,
        animation_provider=provider,
        required_motions=required,
        unmet=unmet[:12],
        rationale=rationale[:2000],
        decided_by=decided_by,
    )


def avatar_route(item: AssetItem, build_spec: Any, uthana_on: bool, idle_id: str) -> CharacterRoute:
    """The player's body: always Uthana (finger joints), walk/run/idle."""
    legs = bool(getattr(getattr(build_spec, "player_avatar", None), "leg_animation", True))
    provider = RigProvider.uthana if uthana_on else RigProvider.tripo
    required: list[RequiredMotion] = []
    if provider == RigProvider.uthana:
        required.append(RequiredMotion(key="idle", kind=MotionKind.idle, purpose="standing still",
                                       source=MotionSource.uthana_library,
                                       library_motion_id=idle_id))
        if legs:
            for gait in ("walk", "run"):
                required.append(RequiredMotion(key=gait, kind=MotionKind.locomotion,
                                               purpose=f"player {gait}s",
                                               source=MotionSource.uthana_locomotion))
    else:
        required = tripo_avatar_motions(item, legs)
    return CharacterRoute(
        asset_id=item.id,
        character_role="player_avatar",
        body_plan=BodyPlan.biped_humanoid,
        rig_provider=provider,
        animation_provider=provider if (legs or provider == RigProvider.uthana) else RigProvider.none,
        required_motions=required,
        rationale="player avatar: always Uthana for finger joints"
        if provider == RigProvider.uthana else "Uthana unavailable; Tripo avatar without fingers",
        decided_by="code",
    )


def tripo_avatar_motions(item: AssetItem, legs: bool) -> list[RequiredMotion]:
    """Tripo clips for an avatar that could not go to Uthana."""
    intents = [a.value for a in item.animations] or ["idle", "walk", "run"]
    if not legs:
        intents = [i for i in intents if i not in _LOCOMOTION_INTENTS]
    table = presets_for_rig("biped")
    out: list[RequiredMotion] = []
    for intent in intents:
        preset = table.get(intent)
        if not preset:
            continue
        kind = (MotionKind.idle if intent == "idle"
                else MotionKind.locomotion if intent in _LOCOMOTION_INTENTS
                else MotionKind.action)
        out.append(RequiredMotion(key=intent, kind=kind, source=MotionSource.tripo_preset,
                                  tripo_preset=preset, purpose=f"player {intent}"))
    return out


def _reconcile_manifest(manifest: Any, plan: CharacterRoutingPlan) -> bool:
    changed = False
    for item in manifest.items:
        route = plan.route_for(item.id)
        if route is None:
            continue
        rigged = route.rig_provider != RigProvider.none
        if item.rigged != rigged:
            item.rigged = rigged
            changed = True
    return changed
