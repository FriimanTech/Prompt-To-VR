"""Character routing + the Uthana stages, end to end, with no network.

The Uthana client and the Tripo client are fakes; the GLBs are real Uthana
output (a rigged character and an in-place locomotion download) so the
measurement, origin and merge math run on the exact file layout Uthana
produces: an ``Armature`` node scaled 0.01 and rotated 90 degrees about X.
"""

from __future__ import annotations

import json
import shutil
from dataclasses import dataclass
from pathlib import Path

import pytest

from ptv_orchestrator.events import EventBus
from ptv_orchestrator.pipeline import (
    stage_character_routing,
    stage_uthana_animation,
    stage_uthana_motion_plan,
    stage_uthana_rigging,
)
from ptv_orchestrator.pipeline.character_artifacts import load_json, load_routing
from ptv_orchestrator.pipeline.context import PipelineContext
from ptv_orchestrator.pipeline.stage_character_routing import decide_provider
from ptv_orchestrator.pipeline.stage_uthana_motion_plan import plan_character_motions
from ptv_orchestrator.pipeline.structured_agent import StructuredAgent
from ptv_orchestrator.pipeline.uthana.glb_measure import measure
from ptv_orchestrator.pipeline.uthana.glb_merge import MotionInput, merge_character
from ptv_orchestrator.pipeline.uthana.motion_library import (
    LibraryResolver,
    LocomotionResolver,
    MotionCatalogue,
    motion_verification,
)
from ptv_orchestrator.pipeline.uthana.motion_names import (
    format_locomotion_name,
    parse_locomotion_name,
)
from ptv_orchestrator.settings import Settings
from ptv_orchestrator.storage import ProjectStore
from ptv_providers import CreatedCharacter, MotionRecord
from ptv_schemas import (
    AnimationIntent,
    AssetEntry,
    AssetItem,
    AssetManifest,
    BodyPlan,
    BuildSpec,
    CharacterRoute,
    MechanicEntry,
    MotionKind,
    MotionSource,
    MovementStyle,
    NpcBehavior,
    NpcPlan,
    NpcSpec,
    NpcRole,
    PlannedCharacterMotions,
    PlannedLocomotion,
    ReferenceImageRequest,
    RequiredMotion,
    RigProvider,
    RoutingAgentOutput,
    RoutingDecision,
    SceneEntry,
    TestEntry,
    TripoRigCheckRecord,
    UthanaMotionAgentOutput,
    UthanaMotionSpec,
)
from ptv_schemas.asset_manifest import AssetCategory, CharacterRole
from ptv_schemas.build_spec import GameType

FIXTURES = Path(__file__).parent / "fixtures" / "uthana"
RIGGED = FIXTURES / "rigged_character.glb"
MOTION = FIXTURES / "motion_in_place.glb"
IDLE_ID = "msf9nhQ4yyTJ"


# ---------------------------------------------------------------------------
# Fakes
# ---------------------------------------------------------------------------


class _Bus(EventBus):
    def __init__(self) -> None:
        super().__init__()
        self.events: list[tuple[str, str]] = []

    def publish(self, event) -> None:  # type: ignore[override]
        self.events.append((getattr(event, "status", ""), getattr(event, "message", "")))


@dataclass
class _Check:
    riggable: bool
    rig_type: str
    task_id: str = "rc-1"


class FakeTripo:
    def __init__(self, answers: dict[str, _Check]):
        self.answers = answers
        self.calls = 0

    def rig_check(self, *, task_id=None, glb_path=None):
        self.calls += 1
        key = Path(glb_path).parent.name if glb_path else task_id
        return self.answers[key]


class FakeUthana:
    """Records every call; serves the fixture GLBs."""

    def __init__(self, *, confidence: float = 0.9, motions: list[MotionRecord] | None = None):
        self.confidence = confidence
        self.motions = list(motions or [])
        self.calls: list[tuple] = []
        self._next = 0

    # characters
    def create_character(self, path, name, flags):
        self.calls.append(("create_character", name, flags.as_variables()))
        return CreatedCharacter(character_id="cAvatar01", name=name,
                                auto_rig_confidence=self.confidence)

    def download_character(self, character_id):
        self.calls.append(("download_character", character_id))
        return RIGGED.read_bytes()

    # motions
    def list_locomotion_styles(self):
        return ["neutral_male_a", "zombie", "punching", "neutral_non_gendered"]

    def list_motions(self, limit=500):
        self.calls.append(("list_motions", limit))
        return self.motions if limit is None else self.motions[:limit]

    def get_motion(self, motion_id):
        for m in self.motions:
            if m.motion_id == motion_id:
                return m
        if motion_id == IDLE_ID:
            return MotionRecord(IDLE_ID, "idle-library")
        return None

    def create_locomotion(self, character_id, *, strides, move_speed, style_id, travel_angle):
        self._next += 1
        mid = f"mNew{self._next}"
        self.calls.append(("create_locomotion", character_id, strides, move_speed, style_id, travel_angle))
        self.motions.append(MotionRecord(mid, "untitled"))
        return mid

    def rename_motion(self, motion_id, name):
        self.calls.append(("rename_motion", motion_id, name))
        for i, m in enumerate(self.motions):
            if m.motion_id == motion_id:
                self.motions[i] = MotionRecord(motion_id, name)

    def is_download_allowed(self, character_id, motion_id):
        return True

    def download_motion(self, character_id, motion_id, *, no_mesh=True, in_place=True):
        self.calls.append(("download_motion", character_id, motion_id, no_mesh, in_place))
        return MOTION.read_bytes()


# ---------------------------------------------------------------------------
# Project helpers
# ---------------------------------------------------------------------------


def _project(tmp_path: Path, *, with_npc: bool = True):
    settings = Settings(
        projects_dir=tmp_path / "projects",
        engine_dir=tmp_path / "engine",
        samples_dir=tmp_path / "samples",
        repo_root=tmp_path,
        uthana_api_key="test-key",
        openai_api_key=None,
    )
    store = ProjectStore(settings.projects_dir)
    state = store.create("Zombie range", "A zombie shooter. Zombies shamble, then run at you.")
    pid = state.project_id
    store.save_build_spec(pid, BuildSpec(
        prompt_id="zombies", title="Zombies", pitch="Zombie shooter.",
        game_type=GameType.shooter,
        scenes=[SceneEntry(id="yard", name="Yard", summary="a yard", entry_point=True)],
        mechanics=[MechanicEntry(id="zombie_ai", name="Zombie AI", summary="zombie walks then runs")],
        assets=[AssetEntry(id="hero", name="Hero", category="character", summary="the player")],
        acceptance_tests=[TestEntry(id="t1", description="zombies come at you")],
    ))
    items = [
        AssetItem(id="hero", name="Hero", category=AssetCategory.character,
                  character_role=CharacterRole.player_avatar, style_notes="player body",
                  reference=ReferenceImageRequest(prompt="a soldier"), target_scale_meters=1.8,
                  rigged=True, animations=[AnimationIntent.idle, AnimationIntent.walk, AnimationIntent.run]),
    ]
    if with_npc:
        items.append(AssetItem(id="zombie", name="Zombie", category=AssetCategory.character,
                               character_role=CharacterRole.npc, style_notes="a shambling zombie",
                               reference=ReferenceImageRequest(prompt="a zombie"),
                               target_scale_meters=1.8, rigged=True,
                               animations=[AnimationIntent.idle, AnimationIntent.walk,
                                           AnimationIntent.run, AnimationIntent.attack]))
        items.append(AssetItem(id="dog", name="Dog", category=AssetCategory.character,
                               character_role=CharacterRole.npc, style_notes="a guard dog",
                               reference=ReferenceImageRequest(prompt="a dog"),
                               target_scale_meters=0.8, rigged=True,
                               animations=[AnimationIntent.walk]))
    store.save_asset_manifest(pid, AssetManifest(prompt_id="zombies", items=items))
    npcs = []
    if with_npc:
        npcs = [
            NpcSpec(asset_id="zombie", role=NpcRole.enemy, movement_style=MovementStyle.animated_locomotion,
                    behaviors=[NpcBehavior.patrol, NpcBehavior.chase, NpcBehavior.melee_attack],
                    animation_intents=[AnimationIntent.idle, AnimationIntent.walk,
                                       AnimationIntent.run, AnimationIntent.attack],
                    notes="shambles like a zombie"),
            NpcSpec(asset_id="dog", role=NpcRole.enemy, behaviors=[NpcBehavior.chase],
                    animation_intents=[AnimationIntent.walk]),
        ]
    store.save_npc_plan(pid, NpcPlan(prompt_id="zombies", npcs=npcs))
    for item in items:
        d = store.dir_for(pid) / "assets" / item.id
        d.mkdir(parents=True, exist_ok=True)
        shutil.copy(RIGGED, d / "model.glb")  # any GLB stands in for the Tripo mesh
    bus = _Bus()
    ctx = PipelineContext(project_id=pid, settings=settings, store=store, bus=bus)
    return ctx, bus


def _routing_agent(decisions: list[RoutingDecision]) -> StructuredAgent:
    return StructuredAgent(name="t", instructions="x", output_model=RoutingAgentOutput,
                           runner=lambda _b: RoutingAgentOutput(decisions=decisions))


def _motion_agent(chars: list[PlannedCharacterMotions]) -> StructuredAgent:
    return StructuredAgent(name="t", instructions="x", output_model=UthanaMotionAgentOutput,
                           runner=lambda _b: UthanaMotionAgentOutput(characters=chars))


# ---------------------------------------------------------------------------
# Part 1: routing rules
# ---------------------------------------------------------------------------


def test_routing_rules():
    no = TripoRigCheckRecord(riggable=False, rig_type="biped", task_id="x")
    yes = TripoRigCheckRecord(riggable=True, rig_type="biped", task_id="x")
    quad = TripoRigCheckRecord(riggable=True, rig_type="quadruped", task_id="x")
    # biped humanoid Tripo cannot rig -> Uthana, decided in code
    assert decide_provider(BodyPlan.biped_humanoid, no, RigProvider.tripo, True) == (RigProvider.uthana, "code")
    # both can rig -> the agent decides
    assert decide_provider(BodyPlan.biped_humanoid, yes, RigProvider.uthana, True) == (RigProvider.uthana, "agent")
    assert decide_provider(BodyPlan.biped_humanoid, yes, RigProvider.tripo, True) == (RigProvider.tripo, "agent")
    # creatures stay with Tripo; unriggable non-humanoids get nothing
    assert decide_provider(BodyPlan.quadruped, quad, RigProvider.uthana, True) == (RigProvider.tripo, "code")
    assert decide_provider(BodyPlan.serpentine, TripoRigCheckRecord(riggable=False), None, True)[0] == RigProvider.none
    # Uthana unavailable -> Tripo as before
    assert decide_provider(BodyPlan.biped_humanoid, yes, RigProvider.uthana, False)[0] == RigProvider.tripo


def test_character_routing_stage(tmp_path):
    ctx, _ = _project(tmp_path)
    tripo = FakeTripo({"zombie": _Check(True, "biped"), "dog": _Check(True, "quadruped")})
    agent = _routing_agent([
        RoutingDecision(asset_id="zombie", body_plan=BodyPlan.biped_humanoid, rig_provider=RigProvider.uthana,
                        required_motions=[
                            RequiredMotion(key="walk", kind=MotionKind.locomotion,
                                           source=MotionSource.uthana_locomotion, style_hint="zombie"),
                            RequiredMotion(key="run.punching", kind=MotionKind.locomotion,
                                           source=MotionSource.uthana_locomotion, style_hint="punching"),
                            RequiredMotion(key="attack", kind=MotionKind.action,
                                           source=MotionSource.uthana_locomotion),
                        ], rationale="zombie gait"),
        RoutingDecision(asset_id="dog", body_plan=BodyPlan.quadruped, rig_provider=RigProvider.uthana),
    ])
    stage_character_routing.run_with(ctx, agent=agent, tripo_client=tripo,
                                     styles=["zombie", "punching", "neutral_male_a"])
    plan = load_routing(ctx.project_dir)
    hero, zombie, dog = (plan.route_for(a) for a in ("hero", "zombie", "dog"))
    assert hero.rig_provider == RigProvider.uthana and hero.decided_by == "code"
    assert {m.key for m in hero.required_motions} == {"idle", "walk", "run"}
    assert zombie.rig_provider == RigProvider.uthana and zombie.decided_by == "agent"
    keys = {m.key: m for m in zombie.required_motions}
    assert keys["idle"].source == MotionSource.uthana_library and keys["idle"].library_motion_id == IDLE_ID
    assert keys["walk"].style_hint == "zombie"
    assert "run.punching" in keys and "run" in keys
    assert "attack" in {u.key for u in zombie.unmet}  # Uthana has no one-shots
    assert dog.rig_provider == RigProvider.tripo  # agent's pick ignored for creatures
    # rig-check is cached against the mesh hash: a second run calls Tripo zero times
    calls = tripo.calls
    stage_character_routing.run_with(ctx, agent=agent, tripo_client=tripo, styles=["zombie"])
    assert tripo.calls == calls


# ---------------------------------------------------------------------------
# Names + verification + resolvers
# ---------------------------------------------------------------------------


def test_motion_name_round_trip():
    name = format_locomotion_name("Zombie", 5, 1.45, 0)
    assert name == "zombie-str5-ms1.5-deg0"
    p = parse_locomotion_name("neutral_non_gendered-str3-ms1.4-deg0")
    assert (p.style, p.strides, p.move_speed, p.travel_angle) == ("neutral_non_gendered", 3, 1.4, 0.0)
    assert p.as_api_kwargs("cX")["style_id"] == "neutral_non_gendered"
    with pytest.raises(ValueError):
        parse_locomotion_name("zombie walk")


def test_motion_verification_messages():
    motions = [MotionRecord("m1", "zombie-str3-ms1.4-deg0")]
    msg, rec = motion_verification("zombie-str3-ms1.4-deg0", motions)
    assert msg == "match found: zombie-str3-ms1.4-deg0 m1" and rec.motion_id == "m1"
    msg, rec = motion_verification("zombie-str3-ms2.2-deg0", motions)
    assert rec is None and msg == "no match is found in the Uthana Organization for that motion requested"


def test_locomotion_resolver_reuses_or_generates_then_renames():
    client = FakeUthana(motions=[MotionRecord("mOld", "zombie-str5-ms1.4-deg0")])
    cat = MotionCatalogue(client, limit=500)
    cat.refresh()
    r = LocomotionResolver(client, cat)
    spec = UthanaMotionSpec(key="walk", kind=MotionSource.uthana_locomotion, gait="walk",
                            style_id="zombie", strides=5, move_speed=1.4, motion_name="zombie-str5-ms1.4-deg0")
    assert r.resolve(spec, "cA").source == "reused"
    assert not any(c[0] == "create_locomotion" for c in client.calls)
    spec2 = spec.model_copy(update={"move_speed": 4.0, "motion_name": "zombie-str5-ms4.0-deg0"})
    got = r.resolve(spec2, "cA")
    assert got.source == "created"
    ops = [c[0] for c in client.calls]
    assert ops.index("create_locomotion") < ops.index("rename_motion")
    create = next(c for c in client.calls if c[0] == "create_locomotion")
    assert create[1:] == ("cA", 5, 4.0, "zombie", 0.0)
    assert ("rename_motion", got.motion_id, "zombie-str5-ms4.0-deg0") in client.calls
    # now in the catalogue: a second character reuses it
    assert r.resolve(spec2, "cB").source == "reused"


def test_unrenamed_motion_is_recovered_not_regenerated():
    # Uthana's default name for a fresh locomotion encodes its parameters.
    client = FakeUthana(motions=[MotionRecord("mOrphan", "neutral_non_gendered-4.1-5steps-0deg")])
    cat = MotionCatalogue(client, limit=500)
    cat.refresh()
    spec = UthanaMotionSpec(key="run", kind=MotionSource.uthana_locomotion, gait="run",
                            style_id="neutral_non_gendered", strides=5, move_speed=4.1,
                            motion_name="neutral_non_gendered-str5-ms4.1-deg0")
    got = LocomotionResolver(client, cat).resolve(spec, "cA")
    assert got.motion_id == "mOrphan" and got.source == "reused"
    assert not any(c[0] == "create_locomotion" for c in client.calls)
    assert ("rename_motion", "mOrphan", "neutral_non_gendered-str5-ms4.1-deg0") in client.calls


def test_library_resolver_makes_no_calls():
    spec = UthanaMotionSpec(key="idle", kind=MotionSource.uthana_library, motion_name="idle-library",
                            motion_id=IDLE_ID)
    assert LibraryResolver().resolve(spec, "cA").motion_id == IDLE_ID


# ---------------------------------------------------------------------------
# Part 3A / 3B
# ---------------------------------------------------------------------------


def test_measure_uthana_fixture():
    r = measure(RIGGED, 1.8)
    assert 1.8 < r.world_height_m < 1.9  # through Armature scale 0.01 + 90 deg X
    assert r.bind_raw_longest_axis_name == "y"
    assert r.walk_move_speed == pytest.approx(0.78 * r.world_height_m)


def test_plan_corrections():
    route = CharacterRoute(asset_id="z", body_plan=BodyPlan.biped_humanoid, rig_provider=RigProvider.uthana,
                           animation_provider=RigProvider.uthana,
                           required_motions=[
                               RequiredMotion(key="walk", kind=MotionKind.locomotion,
                                              source=MotionSource.uthana_locomotion, style_hint="zombie"),
                               RequiredMotion(key="run", kind=MotionKind.locomotion,
                                              source=MotionSource.uthana_locomotion),
                           ])
    notes: list[str] = []
    answers = [
        PlannedLocomotion(key="walk", gait="walk", style_id="Zombie", strides=5, move_speed=3.0,
                          motion_name="whatever"),
        PlannedLocomotion(key="run", gait="run", style_id="not_a_style", strides=5,
                          move_speed=4.0, motion_name="x"),
    ]
    specs = {s.key: s for s in plan_character_motions(route, 1.8, answers, {"zombie", "neutral_non_gendered"}, 5, notes)}
    # 3.0 m/s is far from 0.78 x 1.8 = 1.4 -> replaced; the name is canonical.
    assert specs["walk"].move_speed == 1.4 and specs["walk"].motion_name == "zombie-str5-ms1.4-deg0"
    # An unknown style falls back to the character's own walking style.
    assert specs["run"].move_speed == 4.0 and specs["run"].style_id == "zombie"
    assert any("not a Uthana style" in n for n in notes)
    tall = {s.key: s for s in plan_character_motions(route, 4.0, [], {"zombie"}, 5, [])}
    assert tall["run"].move_speed == 8.0  # 2.2 x 4.0 = 8.8, clamped to Uthana's max


# ---------------------------------------------------------------------------
# Part 5
# ---------------------------------------------------------------------------


def test_merge_puts_feet_on_the_bind_sole(tmp_path):
    base = tmp_path / "model_uthana_rigged.glb"
    shutil.copy(RIGGED, base)
    stage_uthana_rigging.normalise_origin(base)
    out = tmp_path / "model_animated.glb"
    rep = merge_character(base, [MotionInput("walk", "zombie-str5-ms1.4-deg0", MOTION),
                                 MotionInput("idle", "idle-library", MOTION)], out)
    assert rep.ok, rep.warnings
    assert all(abs(c.min_toe_y_m - rep.bind_toe_y_m) < 0.03 for c in rep.clips)
    assert rep.hips_offset_m < -0.8  # clips had their feet at the origin
    doc = json.loads(out.read_bytes()[20:20 + int.from_bytes(out.read_bytes()[12:16], "little")])
    assert [a["name"] for a in doc["animations"]] == ["zombie-str5-ms1.4-deg0", "idle-library"]


# ---------------------------------------------------------------------------
# Parts 2-5 through the stages
# ---------------------------------------------------------------------------


def _route_avatar_only(ctx):
    stage_character_routing.run_with(ctx, agent=_routing_agent([]), tripo_client=FakeTripo({}),
                                     styles=["neutral_male_a"])


def test_avatar_low_confidence_falls_back_to_tripo(tmp_path):
    ctx, bus = _project(tmp_path, with_npc=False)
    _route_avatar_only(ctx)
    stage_uthana_rigging.run_with(ctx, client=FakeUthana(confidence=0.2))
    route = load_routing(ctx.project_dir).route_for("hero")
    assert route.fallback == RigProvider.tripo
    assert not (ctx.project_dir / "assets" / "hero" / "model_uthana_rigged.glb").exists()
    assert any("will NOT curl" in m for _, m in bus.events)


def test_full_uthana_flow_for_the_avatar(tmp_path):
    ctx, _ = _project(tmp_path, with_npc=False)
    _route_avatar_only(ctx)
    client = FakeUthana(confidence=0.95)
    stage_uthana_rigging.run_with(ctx, client=client)
    create = next(c for c in client.calls if c[0] == "create_character")
    assert create[2] == {"auto_rig": True, "auto_rig_front_facing": True, "rerig_target": None,
                         "include_fingers": True, "auto_rig_strip_namespace": True,
                         "auto_rig_feet_on_floor": True}
    rig = load_json(ctx.project_dir / "uthana_rig.json")["hero"]
    assert rig["status"] == "rigged" and rig["character_id"] == "cAvatar01" and rig["finger_joints"]

    agent = _motion_agent([PlannedCharacterMotions(asset_id="hero", motions=[
        PlannedLocomotion(key="walk", gait="walk", style_id="neutral_male_a", move_speed=1.4,
                          motion_name="n"),
        PlannedLocomotion(key="idle", gait="walk", style_id="neutral_male_a", move_speed=1.0,
                          motion_name="agent idle must be dropped"),
    ])])
    stage_uthana_motion_plan.run_with(ctx, client=client, agent=agent)
    plan = json.loads((ctx.project_dir / "uthana_motion_plan.json").read_text())
    motions = {m["key"]: m for m in plan["characters"][0]["motions"]}
    assert motions["idle"]["kind"] == "uthana_library" and motions["idle"]["motion_id"] == IDLE_ID
    # 1.4 is within 5% of 0.78 x 1.86 m = 1.45, so the agent's value stands.
    assert motions["walk"]["motion_name"] == "neutral_male_a-str5-ms1.4-deg0"
    # run was not answered by the agent: code fills it from 2.2 x height, in
    # the same style the character walks in.
    assert motions["run"]["motion_name"] == "neutral_male_a-str5-ms4.1-deg0"
    assert set(motions) == {"idle", "walk", "run"}

    stage_uthana_animation.run_with(ctx, client=client)
    downloads = [c for c in client.calls if c[0] == "download_motion"]
    played = [c for c in downloads if c[4] is True]
    assert len(played) == 3  # idle, walk, run -- the clips the game plays
    assert all(c[1] == "cAvatar01" and c[3] is True for c in downloads)
    assert ("download_motion", "cAvatar01", IDLE_ID, True, True) in downloads
    # walk and run are also fetched WITH root motion (free) to measure speed;
    # the idle is not, it has no gait.
    root = [c for c in downloads if c[4] is False]
    assert len(root) == 2 and IDLE_ID not in {c[2] for c in root}
    # the idle is never generated or renamed
    assert not any(c[0] in ("create_locomotion", "rename_motion") and IDLE_ID in c for c in client.calls)
    anim = load_json(ctx.project_dir / "asset_animation.json")["hero"]
    assert anim["status"] == "animated" and anim["provider"] == "uthana"
    assert anim["animations"]["idle"] == "idle-library"
    assert set(anim["clip_speeds"]) == {"walk", "run"}
    assert (ctx.project_dir / "assets" / "hero" / "model_animated.glb").is_file()

    # Resume: nothing is generated or downloaded again
    before = len(client.calls)
    stage_uthana_animation.run_with(ctx, client=client)
    assert [c for c in client.calls[before:] if c[0] in ("create_locomotion", "download_motion")] == []


def test_completion_gate_blocks_the_merge(tmp_path):
    ctx, bus = _project(tmp_path, with_npc=False)
    _route_avatar_only(ctx)
    client = FakeUthana()
    stage_uthana_rigging.run_with(ctx, client=client)
    stage_uthana_motion_plan.run_with(ctx, client=client, agent=_motion_agent([]))

    def broken(*a, **k):
        raise RuntimeError("Uthana is down")

    client.create_locomotion = broken  # walk and run cannot be generated
    stage_uthana_animation.run_with(ctx, client=client)
    assert not (ctx.project_dir / "assets" / "hero" / "model_animated.glb").exists()
    assert load_json(ctx.project_dir / "asset_animation.json")["hero"]["status"] == "incomplete"


def test_missing_library_idle_fails_clearly(tmp_path):
    ctx, _ = _project(tmp_path, with_npc=False)
    ctx.settings.uthana_idle_motion_id = "doesNotExist"
    _route_avatar_only(ctx)
    client = FakeUthana()
    stage_uthana_rigging.run_with(ctx, client=client)
    with pytest.raises(RuntimeError, match="UTHANA_IDLE_MOTION_ID"):
        stage_uthana_motion_plan.run_with(ctx, client=client, agent=_motion_agent([]))
