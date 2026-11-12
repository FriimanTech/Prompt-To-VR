"""API contract v6: generated code wires the interaction built-ins together
instead of re-implementing them -- no presses by hand contact, no private
fuse and blast, no spawns at typed-in coordinates when the world declares
anchors, no private scoreboard text when the game has a world display."""

from __future__ import annotations

from ptv_orchestrator.pipeline.codegen_contracts import API_CONTRACT_VERSION, lint_task_sources

CONTACT_BUTTON = """
void tick(World& w, std::uint32_t self, float, const InputSnapshot&, MechanicContext& ctx) {
    const auto hand = ctx.hands->hand_entity(1);
    if (ctx.contacts->began(red_button_, hand)) start_challenge(ctx);
}
"""

SIGNAL_BUTTON = """
void tick(World& w, std::uint32_t self, float, const InputSnapshot&, MechanicContext& ctx) {
    if (ctx.signals->fired_since("lane2.start", seen_)) start_challenge(ctx);
}
"""

PRIVATE_GRENADE = """
void tick(World& w, std::uint32_t self, float dt, const InputSnapshot&, MechanicContext& ctx) {
    fuse_timer_ -= dt;
    if (fuse_timer_ <= 0.0f) {
        const float blast_radius = 6.0f;
        for (auto e : targets_) ctx.combat->apply_damage(e, 100.0f, self);
    }
}
"""

LITERAL_SPAWN = """
void spawn_targets(MechanicContext& ctx) {
    ctx.spawn->instance(tmpl_, Vec3(12.0f, 3.5f, -20.0f), Quat(1, 0, 0, 0));
}
"""

ANCHOR_SPAWN = """
void spawn_targets(MechanicContext& ctx) {
    Vec3 p; Quat r;
    if (ctx.anchors->pose_at(tmpl_, "lane2_hang_points", 0, &p, &r))
        ctx.spawn->instance(tmpl_, Vec3(0.0f, 0.0f, 0.0f) + p, r);
}
"""

PRIVATE_HUD = """
void tick(World& w, std::uint32_t self, float, const InputSnapshot&, MechanicContext& ctx) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "TIME LEFT %.1f", left_);
    ctx.text->text(buf, pos_, rot_, 0.08f, Vec4(1), TextAlign::Center);
}
"""


def _lint(code: str, **flags) -> list[str]:
    return lint_task_sources("mechanic", {"task.cpp": code}, **flags)


def test_version_bumped():
    assert API_CONTRACT_VERSION == 6


def test_hand_contact_press_is_rejected_signal_is_fine():
    assert any("press_button" in p for p in _lint(CONTACT_BUTTON, interactions=True))
    assert _lint(SIGNAL_BUTTON, interactions=True) == []
    assert _lint(CONTACT_BUTTON) == []     # no interaction design: rule off


def test_private_fuse_and_blast_is_rejected():
    assert any("builtin.explosive" in p for p in _lint(PRIVATE_GRENADE, interactions=True))


def test_literal_spawn_only_when_the_world_has_anchors():
    assert any("ctx.anchors" in p for p in _lint(LITERAL_SPAWN, anchors=True))
    assert _lint(LITERAL_SPAWN) == []
    assert _lint(ANCHOR_SPAWN, anchors=True) == []


def test_private_scoreboard_text_when_there_is_a_world_display():
    assert any("world_display" in p for p in _lint(PRIVATE_HUD, displays=True))
    assert _lint(PRIVATE_HUD) == []
    with_vars = PRIVATE_HUD + '\nvoid publish(MechanicContext& ctx) { ctx.vars->set_number("x", 1); }'
    assert _lint(with_vars, displays=True) == []
