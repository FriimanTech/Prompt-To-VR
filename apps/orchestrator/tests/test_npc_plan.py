"""The NPC plan stage: reading what characters are FOR, before anything is built.

The three example prompts this has to tell apart:

* a book-reading experience, which has no NPCs at all;
* ten ducks that slide at the player with no animation whatsoever;
* a squad game where allies and enemies fight each other as well as the player.

Getting the second one wrong is not cosmetic. A duck marked ``rigged`` costs a
~30-credit rig plus ~20 per clip, for animation nothing will ever play.
"""

from __future__ import annotations

import json
from pathlib import Path

from ptv_schemas import (
    AnimationIntent,
    AssetCategory,
    AssetItem,
    AssetManifest,
    BuildSpec,
    GameType,
    MovementStyle,
    NpcBehavior,
    NpcPlan,
    NpcRole,
    NpcSpec,
    ProjectStage,
    ProjectStatus,
    ReferenceImageRequest,
)
from ptv_schemas.asset_manifest import CharacterRole
from ptv_orchestrator.events import EventBus
from ptv_orchestrator.pipeline import stage_npc_plan
from ptv_orchestrator.pipeline.context import PipelineContext
from ptv_orchestrator.pipeline.npc_plan_agent import (
    NpcDirector,
    NpcPlanAgentError,
    NpcPlanAgentInput,
    build_director_message,
)
from ptv_orchestrator.settings import Settings
from ptv_orchestrator.storage import ProjectStore


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------


def _asset(asset_id: str, category: AssetCategory, **kw) -> AssetItem:
    return AssetItem(
        id=asset_id,
        name=asset_id,
        category=category,
        style_notes="notes",
        reference=ReferenceImageRequest(prompt="p"),
        **kw,
    )


def _build_spec() -> BuildSpec:
    return BuildSpec(
        prompt_id="game",
        title="Game",
        pitch="A game.",
        game_type=GameType.shooter,
        scenes=[{"id": "s1", "name": "Arena", "summary": "arena",
                 "entry_point": True}],
        mechanics=[{"id": "m1", "name": "Shoot", "summary": "shoot things"}],
        assets=[{"id": "duck", "name": "Duck", "category": "character",
                 "summary": "a duck"}],
        acceptance_tests=[{"id": "t1", "description": "it runs"}],
    )


def _ctx(tmp_path: Path, manifest: AssetManifest) -> PipelineContext:
    settings = Settings(projects_dir=tmp_path)
    store = ProjectStore(settings.projects_dir)
    state = store.create("game", "ducks slide at you, no animations")
    ctx = PipelineContext(
        project_id=state.project_id,
        settings=settings,
        store=store,
        bus=EventBus(),
    )
    ctx.build_spec = _build_spec()
    ctx.asset_manifest = manifest
    store.save_build_spec(ctx.project_id, ctx.build_spec)
    store.save_asset_manifest(ctx.project_id, manifest)
    return ctx


class _Director(NpcDirector):
    """Stub that returns a canned plan and records the prompt it was given."""

    def __init__(self, plan: NpcPlan | None = None, error: str = "") -> None:
        self.seen_message = ""
        self._plan = plan
        self._error = error

    def run(self, message: str) -> NpcPlan:  # type: ignore[override]
        self.seen_message = message
        if self._error:
            raise NpcPlanAgentError(self._error)
        assert self._plan is not None
        return self._plan


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


def test_a_game_with_no_characters_skips_the_agent(tmp_path: Path):
    """A book-reading experience has no NPCs. That is a complete answer, and
    it must not cost an agent turn."""
    manifest = AssetManifest(
        prompt_id="game",
        items=[
            _asset("book", AssetCategory.prop),
            _asset("hud", AssetCategory.ui),
        ],
    )
    ctx = _ctx(tmp_path, manifest)
    director = _Director(error="the agent must not be called")

    result = stage_npc_plan.run_with_director(ctx, director=director)

    assert director.seen_message == ""
    assert ctx.npc_plan.npcs == []
    saved = json.loads(
        (ctx.project_dir / "npc_plan.json").read_text(encoding="utf-8")
    )
    assert saved["npcs"] == []
    assert result.stage is ProjectStage.npc_plan


def test_the_players_own_avatar_is_not_an_npc(tmp_path: Path):
    """A first-person shooting range has one character asset -- the player's
    own body -- and zero NPCs.

    Filtering on ``category`` alone counted that avatar as a candidate, so the
    empty-plan short circuit never fired and the director was called to be
    told what the manifest already said. The player is not a NON-player
    character; ``character_role`` is the field that says so.
    """
    manifest = AssetManifest(
        prompt_id="game",
        items=[
            _asset(
                "player-range-avatar",
                AssetCategory.character,
                character_role=CharacterRole.player_avatar,
            ),
            _asset("pistol", AssetCategory.prop),
        ],
    )
    ctx = _ctx(tmp_path, manifest)
    director = _Director(error="the agent must not be called")

    stage_npc_plan.run_with_director(ctx, director=director)

    assert director.seen_message == ""
    assert ctx.npc_plan.npcs == []


def test_a_character_with_no_declared_role_is_still_a_candidate(tmp_path: Path):
    """Only ``player_avatar`` is excluded, and deliberately so.

    The role is set by an upstream agent. Requiring it to say ``npc`` would
    mean an unset value silently removes a real enemy from the plan -- and
    then nothing reconciles the manifest, so its rigging guesses go through
    unchallenged and bill credits nobody vetted.
    """
    manifest = AssetManifest(
        prompt_id="game",
        items=[_asset("goblin", AssetCategory.character)],
    )
    assert manifest.items[0].character_role is CharacterRole.none

    ctx = _ctx(tmp_path, manifest)
    director = _Director(plan=NpcPlan(prompt_id="game"))

    stage_npc_plan.run_with_director(ctx, director=director)

    assert "goblin" in director.seen_message


def test_an_avatar_alongside_real_npcs_still_runs_the_director(tmp_path: Path):
    """Excluding the avatar must not exclude the enemies standing next to it."""
    manifest = AssetManifest(
        prompt_id="game",
        items=[
            _asset(
                "avatar",
                AssetCategory.character,
                character_role=CharacterRole.player_avatar,
            ),
            _asset(
                "goblin",
                AssetCategory.character,
                character_role=CharacterRole.npc,
            ),
        ],
    )
    ctx = _ctx(tmp_path, manifest)
    director = _Director(plan=NpcPlan(prompt_id="game"))

    stage_npc_plan.run_with_director(ctx, director=director)

    # The director is told about the goblin and NOT about the avatar, so it
    # cannot plan behaviour for the player's own body.
    assert "goblin" in director.seen_message
    candidates_line = [
        line
        for line in director.seen_message.splitlines()
        if line.startswith("- Character assets in this manifest:")
    ]
    assert candidates_line, "the candidate list was not passed to the director"
    assert "avatar" not in candidates_line[0]


def test_a_slide_only_npc_never_reaches_the_rigging_stage(tmp_path: Path):
    """The credit-saving case. The manifest agent marked the duck rigged and
    asked for walk/run clips; the prompt says the ducks just slide. Rigging
    reads the manifest, so the manifest is what has to change."""
    manifest = AssetManifest(
        prompt_id="game",
        items=[
            _asset(
                "duck",
                AssetCategory.character,
                rigged=True,
                animations=[AnimationIntent.walk, AnimationIntent.run],
            )
        ],
    )
    ctx = _ctx(tmp_path, manifest)
    plan = NpcPlan(
        prompt_id="game",
        npcs=[
            NpcSpec(
                asset_id="duck",
                role=NpcRole.enemy,
                faction="enemy",
                hostile_to=["player"],
                movement_style=MovementStyle.slide_no_animation,
                behaviors=[NpcBehavior.chase, NpcBehavior.contact_damage],
            )
        ],
    )

    stage_npc_plan.run_with_director(ctx, director=_Director(plan))

    duck = ctx.asset_manifest.items[0]
    assert duck.rigged is False
    assert duck.animations == []
    # And it has to be persisted, because the rigging stage re-reads the file.
    on_disk = json.loads(
        (ctx.project_dir / "asset_manifest.json").read_text(encoding="utf-8")
    )
    assert on_disk["items"][0]["rigged"] is False
    assert on_disk["items"][0]["animations"] == []


def test_an_animated_npc_gets_exactly_the_clips_the_plan_asked_for(
    tmp_path: Path,
):
    manifest = AssetManifest(
        prompt_id="game",
        items=[_asset("goblin", AssetCategory.character)],
    )
    ctx = _ctx(tmp_path, manifest)
    plan = NpcPlan(
        prompt_id="game",
        npcs=[
            NpcSpec(
                asset_id="goblin",
                role=NpcRole.enemy,
                faction="enemy",
                hostile_to=["player", "ally"],
                behaviors=[NpcBehavior.chase, NpcBehavior.ranged_attack],
                animation_intents=[
                    AnimationIntent.idle,
                    AnimationIntent.walk,
                    AnimationIntent.run,
                    AnimationIntent.shoot,
                ],
            )
        ],
    )

    stage_npc_plan.run_with_director(ctx, director=_Director(plan))

    goblin = ctx.asset_manifest.items[0]
    # The manifest agent never marked it rigged; the plan is the authority.
    assert goblin.rigged is True
    assert [a.value for a in goblin.animations] == [
        "idle", "walk", "run", "shoot",
    ]


def test_entries_for_assets_that_do_not_exist_are_dropped(tmp_path: Path):
    """A hallucinated asset id would otherwise surface as an NPC with no mesh,
    much later and much less clearly."""
    manifest = AssetManifest(
        prompt_id="game", items=[_asset("goblin", AssetCategory.character)]
    )
    ctx = _ctx(tmp_path, manifest)
    plan = NpcPlan(
        prompt_id="game",
        npcs=[
            NpcSpec(asset_id="goblin", role=NpcRole.enemy),
            NpcSpec(asset_id="dragon_that_was_never_made", role=NpcRole.boss),
        ],
    )

    stage_npc_plan.run_with_director(ctx, director=_Director(plan))

    assert [n.asset_id for n in ctx.npc_plan.npcs] == ["goblin"]


def test_a_failing_director_does_not_sink_the_run(tmp_path: Path):
    """Without a plan the pipeline behaves as it did before this stage
    existed, which is a far better outcome than failing the build."""
    manifest = AssetManifest(
        prompt_id="game",
        items=[_asset("goblin", AssetCategory.character, rigged=True)],
    )
    ctx = _ctx(tmp_path, manifest)

    result = stage_npc_plan.run_with_director(
        ctx, director=_Director(error="model unavailable")
    )

    assert result.stage is ProjectStage.npc_plan
    assert ctx.npc_plan.npcs == []
    # The manifest is left exactly as it was, so rigging still happens.
    assert ctx.asset_manifest.items[0].rigged is True
    state = ctx.store.load_state(ctx.project_id)
    # The stage records "running" then "succeeded", so the latest entry is
    # the verdict.
    record = [r for r in state.stages if r.stage is ProjectStage.npc_plan][-1]
    assert record.status is ProjectStatus.succeeded


def test_the_director_sees_the_users_own_words(tmp_path: Path):
    """The decisions here hinge on phrasing a BuildSpec summary discards --
    "the ducks should just slide" is exactly what gets condensed away."""
    manifest = AssetManifest(
        prompt_id="game", items=[_asset("duck", AssetCategory.character)]
    )
    ctx = _ctx(tmp_path, manifest)
    director = _Director(NpcPlan(prompt_id="game"))

    stage_npc_plan.run_with_director(ctx, director=director)

    assert "ducks slide at you, no animations" in director.seen_message
    assert "AssetManifest" in director.seen_message


def test_director_message_carries_prompt_spec_and_manifest():
    message = build_director_message(
        NpcPlanAgentInput(
            prompt_text="ten ducks chase you",
            build_spec_json='{"title": "Ducks"}',
            asset_manifest_json='{"items": []}',
            extra_instructions=["Character assets in this manifest: duck"],
        )
    )
    assert "ten ducks chase you" in message
    assert '{"title": "Ducks"}' in message
    assert "Character assets in this manifest: duck" in message


def test_the_plan_reaches_every_stage_that_has_to_agree_with_it():
    """Three agents downstream would otherwise each re-read the prompt and
    reach three different conclusions about the same characters."""
    from ptv_orchestrator.pipeline.codegen_agent import (
        CodegenAgentInput,
        build_user_message,
    )
    from ptv_orchestrator.pipeline.mechanic_plan_agent import (
        MechanicPlanAgentInput,
        build_planner_message,
    )
    from ptv_orchestrator.pipeline.stage_scene_spec import _compress_npcs

    plan = NpcPlan(
        prompt_id="g",
        npcs=[
            NpcSpec(
                asset_id="goblin",
                role=NpcRole.enemy,
                faction="enemy",
                hostile_to=["player"],
                behaviors=[NpcBehavior.chase],
            )
        ],
    )
    plan_json = plan.model_dump_json(indent=2)

    # Mechanic planner.
    msg = build_planner_message(
        MechanicPlanAgentInput(
            build_spec_json="{}",
            asset_manifest_json="{}",
            catalog_text="ENGINE CATALOG",
            npc_plan_json=plan_json,
        )
    )
    assert "NpcPlan" in msg and "goblin" in msg
    # ...and the planner must be told not to duplicate what the built-in does.
    assert "builtin.character_locomotion" in msg

    # Absent for a game with no NPCs, so a puzzle game pays no tokens for it.
    quiet = build_planner_message(
        MechanicPlanAgentInput(
            build_spec_json="{}",
            asset_manifest_json="{}",
            catalog_text="ENGINE CATALOG",
        )
    )
    assert "NpcPlan" not in quiet

    # Codegen.
    code_msg = build_user_message(
        CodegenAgentInput(
            project_id="p",
            task_id="t",
            output_subdir="engine/generated/mechanics/p",
            task_json="{}",
            ledger_snapshot_json="{}",
            dep_exports_json="{}",
            scene_context_json="[]",
            build_spec_excerpt="",
            instructions="",
            npc_plan_json=plan_json,
        )
    )
    assert "NPC PLAN" in code_msg and "goblin" in code_msg
    assert "CLocomotionIntent" in code_msg

    # Scene composer gets only the fields that change PLACEMENT.
    compact = _compress_npcs(plan)
    assert compact[0]["asset_id"] == "goblin"
    assert compact[0]["faction"] == "enemy"
    assert compact[0]["max_health"] == 100.0
    # Behaviours are the codegen agent's business, and paying for them once
    # per scene is what made this stage expensive.
    assert "behaviors" not in compact[0]
    assert _compress_npcs(None) == []


def test_plan_helpers_answer_what_the_rigging_stages_ask():
    plan = NpcPlan(
        prompt_id="g",
        npcs=[
            NpcSpec(
                asset_id="goblin",
                role=NpcRole.enemy,
                animation_intents=[AnimationIntent.idle, AnimationIntent.walk],
            ),
            NpcSpec(
                asset_id="duck",
                role=NpcRole.enemy,
                movement_style=MovementStyle.slide_no_animation,
            ),
            NpcSpec(asset_id="statue", role=NpcRole.neutral,
                    movement_style=MovementStyle.stationary),
        ],
    )
    assert plan.rigged_asset_ids() == {"goblin"}
    assert [i.value for i in plan.intents_for("goblin")] == ["idle", "walk"]
    assert plan.intents_for("duck") == []
    # A stationary NPC with no clips is not riggable either, but asking about
    # an unknown asset must not raise.
    assert plan.intents_for("nobody") == []
