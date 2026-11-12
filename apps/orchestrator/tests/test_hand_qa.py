"""Hands with five fingers: scoring, finding a T-posed character's hands, and
the reference-image loop (regenerate with the critique, keep the first pass,
else the best attempt with a loud warning)."""

from __future__ import annotations

import json
from pathlib import Path
from types import SimpleNamespace

import numpy as np

from ptv_schemas.asset_manifest import CharacterRole, ReferencePose

from ptv_orchestrator.pipeline import hand_qa
from ptv_orchestrator.pipeline.stage_reference_images import (
    _T_POSE_CLAUSE,
    _T_POSE_NEGATIVES,
    _generate_with_hand_qa,
    image_size_for,
)

GOOD = {"which": "left", "visible": True, "open": True, "finger_count": 5,
        "fingers_separated": True, "covering": "full_finger_glove"}


def test_scores_and_pass_rule():
    assert hand_qa.hand_score(GOOD) == 1.0 and hand_qa.hand_ok(GOOD)
    fingerless = dict(GOOD, covering="fingerless_glove", finger_count=3, fingers_separated=False)
    assert not hand_qa.hand_ok(fingerless) and hand_qa.hand_score(fingerless) < 0.5
    one = hand_qa.report_from({"hands": [GOOD], "critique": ""}, "image")
    assert not one.passed and one.score < 0.75   # a missing hand is a zero
    both = hand_qa.report_from({"hands": [GOOD, dict(GOOD, which="right")], "critique": ""},
                               "image")
    assert both.passed and both.score == 1.0
    reports = [hand_qa.HandReport("image", score=0.4), hand_qa.HandReport("image", score=0.7),
               hand_qa.HandReport("image", score=0.7)]
    assert hand_qa.best(reports) == 1
    reports[2].passed = True
    assert hand_qa.best(reports) == 2


def test_locates_both_hands_of_a_t_pose_and_refuses_arms_down():
    rng = np.random.default_rng(1)
    body = rng.uniform([-0.2, 0.0, -0.12], [0.2, 1.8, 0.12], size=(4000, 3))
    arms = rng.uniform([-0.85, 1.35, -0.05], [0.85, 1.45, 0.05], size=(3000, 3))
    sites, arm = hand_qa.locate_hands(np.vstack([body, arms]))
    assert arm.tolist() == [1.0, 0.0, 0.0]
    xs = sorted(s.center[0] for s in sites)
    assert abs(xs[0] + 0.75) < 0.05 and abs(xs[1] - 0.75) < 0.05
    assert all(1.3 < s.center[1] < 1.5 and abs(s.radius - 0.135) < 0.01 for s in sites)
    assert hand_qa.locate_hands(body) is None


def test_hand_clause_and_sizes():
    assert "five fingers" in _T_POSE_CLAUSE and "fingerless gloves" in _T_POSE_NEGATIVES
    settings = SimpleNamespace(reference_avatar_image_size="4K", reference_character_image_size="2K")
    avatar = SimpleNamespace(character_role=CharacterRole.player_avatar)
    npc = SimpleNamespace(character_role=CharacterRole.npc)
    assert image_size_for(avatar, ReferencePose.t_pose, settings) == "4K"
    assert image_size_for(npc, ReferencePose.t_pose, settings) == "2K"
    assert image_size_for(npc, ReferencePose.natural, settings) is None


class _Images:
    def __init__(self):
        self.prompts: list[str] = []
        self.sizes: list[str] = []

    def generate(self, *, prompt, out_dir, image_size, filename_prefix, **_):
        self.prompts.append(prompt)
        self.sizes.append(image_size)
        out_dir.mkdir(parents=True, exist_ok=True)
        p = out_dir / f"{filename_prefix}_00.png"
        p.write_bytes(b"png" + str(len(self.prompts)).encode())
        return [SimpleNamespace(path=p)]


def _ctx(tmp_path: Path, retries: int = 2):
    events: list[tuple[str, str]] = []
    return SimpleNamespace(
        settings=SimpleNamespace(hand_qa_max_retries=retries),
        raise_if_cancelled=lambda: None,
        emit=lambda stage, kind, msg: events.append((kind, msg)),
        store=SimpleNamespace(append_log=lambda *a: None),
        project_id="p", events=events)


def _item():
    return SimpleNamespace(id="avatar", reference=SimpleNamespace(aspect_ratio="1:1"))


def test_reference_loop_retries_with_the_critique_and_keeps_the_pass(tmp_path, monkeypatch):
    answers = iter([
        hand_qa.HandReport("image", score=0.3, critique="the gloves are fingerless"),
        hand_qa.HandReport("image", score=1.0, passed=True),
    ])
    monkeypatch.setattr(hand_qa, "check_image", lambda v, p: next(answers))
    ctx, images = _ctx(tmp_path), _Images()
    out = _generate_with_hand_qa(ctx, _item(), tmp_path, "a soldier", None, "4K", images, object())
    assert len(images.prompts) == 2 and "fingerless" in images.prompts[1]
    assert images.sizes == ["4K", "4K"]
    assert out[0].path == tmp_path / "avatar_ref_00.png"
    assert out[0].path.read_bytes() == b"png2"
    record = json.loads((tmp_path / "hand_qa.json").read_text())
    assert record["passed"] and record["chosen"] == 1
    assert not any(k == "warning" for k, _ in ctx.events)


def test_reference_loop_keeps_the_best_and_warns(tmp_path, monkeypatch):
    answers = iter([hand_qa.HandReport("image", score=0.5, critique="x", hands=[{}]),
                    hand_qa.HandReport("image", score=0.8, critique="y", hands=[{}]),
                    hand_qa.HandReport("image", score=0.6, critique="z", hands=[{}])])
    monkeypatch.setattr(hand_qa, "check_image", lambda v, p: next(answers))
    ctx, images = _ctx(tmp_path), _Images()
    out = _generate_with_hand_qa(ctx, _item(), tmp_path, "a soldier", None, "2K", images, object())
    assert len(images.prompts) == 3 and out[0].path.read_bytes() == b"png2"
    assert any(k == "warning" and msg.startswith("HANDS:") for k, msg in ctx.events)
    assert json.loads((tmp_path / "hand_qa.json").read_text())["passed"] is False
