> **Shared content:** Identical to [PC_VR_Stages/15b_scene_review](../../PC_VR_Stages/15b_scene_review/README.md). Update both copies when this stage changes.

# Stage 15b: Scene Review

> **Pipeline position:** after `scene_compile` → before the build  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_scene_review.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_scene_review.py), [`engine/tools/scene_views.cpp`](../../../../engine/tools/scene_views.cpp)  
> **Primary artifacts:** `projects/<project_id>/scene_review.json`, `builds/scene_review/<scene>/*.png`, `verify_viewpoints` in `scene.json`

## Main stage summary

### What this stage is

A look at the compiled layout before paying for a build. `ptv_scene_views`
renders each compiled `scene.json` with the CPU rasteriser `ptv_glb_views`
uses (`engine/tools/cpu_raster.h`, near-plane clipped), writing an
entity-id buffer beside every image: the player's spawn view, an overview
with the roof cut away (`max_y`), and per requirement an eye-level view
(from walkable floor) and a view from above. Then:

1. **Deterministic checks** -- a requirement NONE of whose claimed assets
   is in the scene (some missing is only a warning: name matching also
   claims effects drawn at runtime), requirement entities that show fewer
   than 60 pixels from their viewpoints, a spawn whose centre pixel is a
   wall under 0.6 m away. A requirement whose entities are all hidden
   templates (an NPC wave, a challenge's targets) gets no cameras and no
   vision check: it is unverified here and judged on the headset.
2. **Vision** -- Gemini (`socket_vision_model`) judges each placement /
   layout / visual requirement from its own viewpoints (yes / no / unclear,
   what it sees, what is wrong), up to `scene_review_max_vision_checks`.

Outcomes go into the requirements ledger and `requirements_report.json`.
The spawn and eye-level viewpoints are written into the shipped scene
documents as `verify_viewpoints`, where the headset's interactions pass
takes a screenshot at each (re-attached after a memory-correction
re-compile).

### Why it exists

The shooting range compiled and ran with duplicate tables, guns beside the
wrong table and lane-2 targets on the floor; every earlier check was about
one thing at a time and nothing ever looked at the layout the prompt
described.

### Failure behaviour

Report-only: issues are warnings, the stage succeeds. Without a Google key
the deterministic checks still run.

## Detailed stage deep dive

See [docs/requirements.md](../../../requirements.md) and
[docs/placement.md](../../../placement.md).
