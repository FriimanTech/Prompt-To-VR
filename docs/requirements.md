# Requirements: every atomic thing the prompt asks for, tracked to the headset

`requirements.json` lists what the prompt asked for -- one atomic, checkable
statement per item, each with the prompt's VERBATIM quote -- and every later
stage either claims the items it delivers or fails. Scene review and the
headset record what was actually seen; `requirements_report.json` (and the
project page) say pass / fail / unverified per requirement.

The shooting range is why: the prompt asked for lane-1 targets destroyed by
bullets OR grenades and for grenades that explode; the mechanic plan had
neither, nothing noticed, and the APK shipped without them.

## Schema

`packages/schemas/ptv_schemas/requirements.py`:

* `Requirement`: `id`, `kind` (`asset`, `visual`, `placement`, `layout`,
  `interaction`, `mechanic`, `rule`, `npc`), `priority` (`must` / `should`),
  `statement`, `quote` (verbatim, checked), `subjects`, `acceptance`.
* `RequirementsLedger`: `requirements`, `claims` (requirement id -> who
  claimed it: stage + item), `results` (requirement id -> outcome, evidence,
  source).

## The `requirements` stage

Runs after `optimization_preferences` (hard tier through `LlmClient`,
structured output). `requirements_ledger.validate` keeps only requirements
whose quote is the prompt's own words.

## Claims and the coverage gate

Planning artifacts carry `satisfies: [requirement ids]`: asset items,
mechanic-plan items, scene entities; a world builder calls
`w.satisfies(id, how)`. Asset and visual requirements can also be claimed by
name (`name_claims`: a subject's HEAD noun in an asset's id or name -- any
shared word, matched against style notes too, let "all-black tactical
combat outfit" claim every black asset; `scene_review` also prefers a
planner's explicit claims over name matches when it picks what to look
at). Placement
requirements are also claimed by what the artifacts deliver
(`placement_claims`), because composers place things correctly and forget
to list ids: a scene entity named for the subject the requirement places
(`placed_subject`: the subject its statement names first -- "The
regular-sized Goblins are positioned at the front of the third lane"
places the goblins) that has a placement intent; a world-owned asset of
that name; or an encounter group whose `zone_tags` share a band word
(front / middle / back ...) with the requirement, or whose `zone_quote`
is the requirement's quote. A scene that still fails the gate is saved to
`logs/scene_spec.<scene>.rejected.json`. After each
stage `requirements_ledger.gate` records that stage's claims and returns the
must-requirements it OWNS (`STAGE_OWNS`) that nobody has claimed; the stage
gets ONE repair round with the uncovered list, then fails with
`RequirementGateError` naming them. Owned kinds:

| stage | owns |
| --- | --- |
| `asset_manifest` | asset, visual |
| `mechanic_plan` | interaction, mechanic, rule, npc |
| `scene_spec` | placement |
| `world_generation` | layout |

## Results

Each verifying stage owns its verdicts: a re-run first drops what that
stage recorded last time (`clear_results`), so a failure a fixed stage no
longer finds does not outlive the fix; a verified pass or fail is still
never overwritten by another stage's "unverified".

* **`scene_review`** (after scene_compile): `ptv_scene_views` renders the
  compiled scene from the spawn, an overview and two viewpoints per
  requirement; deterministic checks (no claimed asset in the scene,
  entities no viewpoint shows, a spawn facing a wall) then Gemini vision per
  placement / layout / visual requirement -- except one only runtime
  templates carry (waves, challenge targets), left to the headset
  (`scene_review.json`, images
  under `builds/scene_review/<scene>/`). The viewpoints are written into the
  shipped scene as `verify_viewpoints`.
* **`quest_runtime_verify`** (interactions pass): interaction / mechanic
  requirements whose entities the scripted player grabbed, pressed or threw
  pass or fail on what happened; requirement viewpoints get a Quest
  screenshot.

A verified failure is never overwritten by a later "unverified". The report
is rewritten after each, with a summary (`pass`, `fail`, `unverified`,
`unclaimed`).

## Tests

`test_requirements_ledger.py`, `test_scene_review.py`,
`test_verify_interactions.py`.
