> **Shared content:** Identical to [Quest_Stages/01c_requirements](../../Quest_Stages/01c_requirements/README.md). Update both copies when this stage changes.

# Stage 1c: Requirements

> **Pipeline position:** after `optimization_preferences` → before `asset_manifest`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_requirements.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_requirements.py), [`requirements_ledger.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/requirements_ledger.py)  
> **Primary artifacts:** `projects/<project_id>/requirements.json`, `requirements_report.json`

## Main stage summary

### What this stage is

One hard-tier structured call (through `LlmClient`) that turns the prompt into
atomic, checkable requirements -- "a Glock pistol", "the guns rest on the
left table", "a red button starts a 20-second challenge", "the goblins all
respawn once every one is dead" -- each with a kind (`asset`, `visual`,
`placement`, `layout`, `interaction`, `mechanic`, `rule`, `npc`) and the
prompt's VERBATIM quote. `requirements_ledger.validate` drops any whose
quote is not the prompt's own words.

### Why it exists

The pipeline used to lose requirements silently: the shooting range asked
for lane-1 targets destroyed by bullets or grenades and for grenades that
explode, the mechanic plan had neither, and the APK shipped without them.

### What later stages do with it

* **Claims.** `asset_manifest`, `mechanic_plan`, `scene_spec` and
  `world_generation` claim the requirements they deliver (`satisfies` on
  items / entities, `w.satisfies(id, how)` in a world builder; asset and
  visual ones can be claimed by name).
* **Gates.** After each of those stages `requirements_ledger.gate` returns
  the must-requirements the stage owns that nobody claimed; the stage gets
  one repair round, then fails with `RequirementGateError` naming them.
  Owners: asset_manifest -- asset, visual; mechanic_plan -- interaction,
  mechanic, rule, npc; scene_spec -- placement; world_generation -- layout.
* **Results.** `scene_review` (renders + vision) and, on Quest,
  `quest_runtime_verify` record pass / fail / unverified;
  `requirements_report.json` is shown on the project page.

A project without `requirements.json` (older than this stage) has no gates.

## Detailed stage deep dive

See [docs/requirements.md](../../../requirements.md).
