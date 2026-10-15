> **Shared content:** Identical to [Quest_Stages/08a_uthana_rigging](../../Quest_Stages/08a_uthana_rigging/README.md). Update both copies when this stage changes.

# Stage 08a: Uthana Auto-Rig

> **Pipeline position:** after `character_routing` -> before `asset_rigging`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_uthana_rigging.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_uthana_rigging.py)  
> **Primary artifacts:** `assets/<id>/model_uthana_rigged.glb`, `uthana_rig.json`

## Main stage summary

### What this stage is

Rigs the player avatar and Uthana-routed humanoids with Uthana's auto-rigger, including finger joints.

### Flow

1. Upload `model.glb` with `auto_rig`, `auto_rig_front_facing`, `include_fingers`, `auto_rig_strip_namespace`, `auto_rig_feet_on_floor`. This is a GraphQL multipart call, because SDK 0.5.1 lacks the last two flags.
2. Confidence at or below `uthana_avatar_min_confidence` / `uthana_npc_min_confidence` (0.20) means fallback: the avatar goes to Tripo (no finger curls, loud warning); an NPC goes to Tripo if riggable, else none.
3. Download to `model_uthana_rigged.glb`.
4. Validate humanoid joints, finger chains and namespace.
5. Centre the origin on the character's vertical middle (Uthana's upload dedupe may ignore `feet_on_floor`).
6. Retire an earlier Tripo rig/animation of the same asset to `.superseded/`.

### Outputs

`uthana_rig.json` per asset: `character_id` (used by every later Uthana call), confidence, params, source sha256, joints, finger joints and origin shift. A Resume never re-uploads.

See [docs/uthana.md](../../../uthana.md) for the full Uthana flow.
