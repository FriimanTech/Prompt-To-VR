"""Which environments get generated, and how they stay isolated.

The stage used to produce exactly one environment per project. A six-scene
game therefore shipped one real place and five scenes of gameplay entities
floating in an empty void -- and nothing reported it, because one world had
been produced successfully.

These tests cover the two halves of the fix: deciding the environment list,
and keeping each one's C++ isolated from the others. Isolation is the part
that matters most: without it two agents share a translation-unit set, so a
syntax error in one environment fails another's compile and spends its repair
budget on someone else's mistake.
"""

from __future__ import annotations

from ptv_orchestrator.pipeline.stage_world_generation import (
    _env_brief,
    _environment_jobs,
    _ns_token,
    _worldgen_build_dir,
)
from ptv_schemas.build_spec import (
    AssetEntry,
    BuildSpec,
    EnvironmentMode,
    GameType,
    MechanicEntry,
    NestedHost,
    SceneEntry,
    SceneLink,
    TestEntry,
)


class _Settings:
    world_max_environments = 12
    world_max_parallel_workers = 3


class _Store:
    def __init__(self, spec: BuildSpec) -> None:
        self._spec = spec

    def load_build_spec(self, _project_id: str) -> BuildSpec:
        return self._spec


class _Ctx:
    def __init__(self, spec: BuildSpec, project_dir=None) -> None:
        self.settings = _Settings()
        self.store = _Store(spec)
        self.project_id = "demo"
        self.project_dir = project_dir
        self.emitted: list[tuple[str, str]] = []

    def emit(self, _stage, kind, message) -> None:
        self.emitted.append((kind, message))


def _spec(scenes: list[SceneEntry]) -> BuildSpec:
    return BuildSpec(
        prompt_id="demo",
        title="Demo",
        pitch="Hub and missions.",
        game_type=GameType.shooter,
        scenes=scenes,
        mechanics=[MechanicEntry(id="m", name="M", summary="s")],
        assets=[AssetEntry(id="a", name="A", category="prop", summary="s")],
        acceptance_tests=[TestEntry(id="t", description="d")],
    )


# --- choosing the environment list ------------------------------------


def test_every_scene_gets_its_own_environment():
    # The bug this replaces: five of six scenes got no environment at all.
    ctx = _Ctx(
        _spec(
            [
                SceneEntry(id="hub", name="Hub", summary="Base.", entry_point=True),
                SceneEntry(id="hospital", name="Hospital", summary="Mission."),
                SceneEntry(id="warehouse", name="Warehouse", summary="Mission."),
            ]
        )
    )
    jobs = _environment_jobs(ctx)
    assert [j.scene_id for j in jobs] == ["hub", "hospital", "warehouse"]


def test_a_scene_that_borrows_an_environment_is_not_generated_twice():
    # Paying for a second xhigh agent run to produce the same place is the
    # cost this avoids.
    ctx = _Ctx(
        _spec(
            [
                SceneEntry(id="hub", name="Hub", summary="Base.", entry_point=True),
                SceneEntry(
                    id="hub_night",
                    name="Hub at night",
                    summary="Same place.",
                    needs_unique_environment=False,
                    reuse_environment_from="hub",
                ),
            ]
        )
    )
    assert [j.scene_id for j in _environment_jobs(ctx)] == ["hub"]


def test_the_environment_cap_keeps_the_entry_point():
    # Dropping the first scene would leave the game with nowhere to start,
    # so the cap must never discard the entry point.
    scenes = [
        SceneEntry(id=f"mission_{i}", name=f"M{i}", summary="Mission.")
        for i in range(20)
    ]
    scenes.append(
        SceneEntry(id="hub", name="Hub", summary="Base.", entry_point=True)
    )
    ctx = _Ctx(_spec(scenes))
    jobs = _environment_jobs(ctx)
    assert len(jobs) == _Settings.world_max_environments
    assert any(j.scene_id == "hub" for j in jobs)
    assert any("capping" in m for _, m in ctx.emitted)


def test_a_single_scene_game_produces_one_job():
    ctx = _Ctx(_spec([SceneEntry(id="room", name="Room", summary="One room.")]))
    assert len(_environment_jobs(ctx)) == 1


# --- isolation --------------------------------------------------------


def test_each_environment_gets_its_own_build_tree():
    # Two architects compiling at once would otherwise overwrite each other's
    # object files and binaries.
    from pathlib import Path

    ctx = _Ctx(_spec([SceneEntry(id="hub", name="Hub", summary="B.")]),
               project_dir=Path("/proj"))
    a = _worldgen_build_dir(ctx, "hub")
    b = _worldgen_build_dir(ctx, "hospital")
    assert a != b
    # And the single-environment layout is unchanged, so existing projects
    # keep building in the same place.
    assert _worldgen_build_dir(ctx, "") == Path("/proj") / "builds" / "worldgen"


def test_namespace_tokens_are_cpp_safe():
    assert _ns_token("hub") == "hub"
    assert _ns_token("train-station") == "train_station"
    assert _ns_token("mission.01") == "mission_01"
    # Never empty, or the generated namespace declaration will not compile.
    assert _ns_token("---") == "env"
    assert _ns_token("") == "env"


# --- the per-environment brief ----------------------------------------


def test_the_brief_tells_each_agent_it_authors_only_its_own_environment():
    # The agents share no history by design; the brief is the whole contract.
    ctx = _Ctx(
        _spec(
            [
                SceneEntry(
                    id="hub",
                    name="Hub",
                    summary="A base of operations.",
                    entry_point=True,
                    links=[SceneLink(to_scene="hospital", spawn_point_id="ward")],
                ),
                SceneEntry(id="hospital", name="Hospital", summary="A mission."),
            ]
        )
    )
    jobs = _environment_jobs(ctx)
    brief = "\n".join(_env_brief(jobs[0], len(jobs)))
    assert "ONE environment of 2" in brief
    assert "Do NOT reference another" in brief
    # Exits have to be in the brief or the environment has no way out.
    assert "hospital" in brief
    assert "ward" in brief


def test_a_nested_environment_is_told_to_author_at_human_scale():
    # Authoring a walkable level inside a 0.22 m box produces unusable
    # geometry, so the instruction to scale afterwards is load-bearing.
    ctx = _Ctx(
        _spec(
            [
                SceneEntry(id="class", name="Classroom", summary="A room.",
                           entry_point=True),
                SceneEntry(
                    id="inside_head",
                    name="Inside the head",
                    summary="Skull interior.",
                    environment_mode=EnvironmentMode.nested,
                    host=NestedHost(asset_id="student", anchor_slot="head"),
                    authoring_scale_m=30.0,
                ),
            ]
        )
    )
    jobs = _environment_jobs(ctx)
    nested = next(j for j in jobs if j.scene_id == "inside_head")
    assert nested.nested is True
    brief = "\n".join(_env_brief(nested, len(jobs)))
    assert "HUMAN scale" in brief
    assert "student" in brief and "head" in brief
    assert "0.22" in brief
    assert "30 m across" in brief


def test_a_standalone_brief_mentions_no_host():
    ctx = _Ctx(_spec([SceneEntry(id="room", name="Room", summary="A room.")]))
    brief = "\n".join(_env_brief(_environment_jobs(ctx)[0], 1))
    assert "NESTED" not in brief
