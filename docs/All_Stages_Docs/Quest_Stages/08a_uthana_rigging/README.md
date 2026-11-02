> **Shared content:** Identical to [PC_VR_Stages/08a_uthana_rigging](../../PC_VR_Stages/08a_uthana_rigging/README.md). Update both copies when this stage changes.

# Stage 8a: Uthana Auto-Rig

> **Pipeline position:** after `character_routing` → before `asset_rigging`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_uthana_rigging.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_uthana_rigging.py)  
> **Primary artifacts:** `projects/<project_id>/assets/<asset_id>/model_uthana_rigged.glb`, `projects/<project_id>/uthana_rig.json`

## Main stage summary

### What this stage is

Uploads each character that `character_routing` sent to Uthana (the player's avatar body and humanoid NPCs) to Uthana's auto-rigger, downloads the rigged GLB with finger joints, checks the skeleton, and centres the origin. It runs **before** Tripo rigging so a character whose Uthana rig fails or is not trusted can fall back to Tripo in the same run.

### Why it exists

Tripo's skeletons have no fingers, so a Tripo avatar's hands cannot curl. Uthana's rig has them, and Uthana's locomotion motions (stage 9c) are retargeted onto the `character_id` this stage records.

### Inputs

| Input | Location |
|-------|----------|
| Routing plan | `character_routing.json` (routes with `rig_provider: uthana`) |
| Un-rigged Tripo mesh | `assets/<id>/model.glb` (skips files with `rigged`, `animated`, `_lod` in the name) |
| Previous ledger | `uthana_rig.json` |

### Outputs

| Output | Location |
|--------|----------|
| Rigged GLB | `assets/<id>/model_uthana_rigged.glb` |
| Ledger | `uthana_rig.json`, per asset: `status`, `character_id`, `auto_rig_confidence`, `params`, `character_role`, `joints`, `finger_joints`, `warnings`, `origin_shift_m`, `rigged_glb`, `source_glb`, `source_sha256` (or `error` / `reason`) |
| Routing update | `character_routing.json` — `fallback` / `fallback_reason` set or cleared |
| Manifest update | `rigged: false` for any asset that fell back to `none` |
| Retired Tripo output | stale `model_rigged.glb` / `model_animated.glb` moved to `assets/<id>/.superseded/` |

### Where output goes next

- `asset_rigging` skips Uthana routes unless they fell back to Tripo.
- `uthana_motion_plan` and `uthana_animation` use `character_id` and the rigged GLB.

### Provider / cost

| Call | Detail |
|------|--------|
| Create + auto-rig | GraphQL multipart mutation `create_character` posted directly to `https://uthana.com/graphql` (HTTP basic auth, key as user). Sent directly because the `uthana` SDK (0.5.1) lacks `auto_rig_strip_namespace` / `auto_rig_feet_on_floor` |
| Flags | `auto_rig`, `auto_rig_front_facing`, `include_fingers`, `auto_rig_strip_namespace`, `auto_rig_feet_on_floor` all `true`; `rerig_target: null` |
| Download | SDK `characters.download_sync(character_id, output_format="glb")` |
| Limits | upload must be under 30 MB; client timeout 360 s |

Uthana bills generated motion seconds, not rigging or downloads. No LLM is used.

### Failure behavior

The stage always succeeds. Any of these makes the character **fall back** (loud warning + log):

- Uthana disabled / no key (every target falls back);
- no Tripo mesh on disk;
- upload, download, or GLB parse error;
- `auto_rig_confidence` at or below the threshold (0.20) → `status: low_confidence`, nothing downloaded;
- missing required humanoid joints → `status: invalid_rig`.

Fallback target: the **avatar** goes to Tripo (rig + walk/run, no finger curls); an **NPC** goes to Tripo if its cached rig-check said riggable, otherwise to `none` (and its manifest `rigged` is cleared).

---

## Detailed stage deep dive

### Algorithm (per routed character)

1. Find the un-rigged source GLB and hash it.
2. **Reuse:** ledger `status: rigged` with the same `source_sha256` and the final GLB on disk → clear any fallback, retire Tripo outputs, count as reused.
3. `create_character(source, name="<project_id>__<asset_id>"[:120], AutoRigFlags())`.
4. Compare confidence with `uthana_avatar_min_confidence` or `uthana_npc_min_confidence`.
5. Download to `model_uthana_rigged.download`, then move to `model_uthana_rigged.glb`.
6. `validate_rig`: required joints (namespace-stripped, lowercase) `hips, spine, head, left/rightshoulder, left/rightarm, left/rightforearm, left/righthand, left/rightupleg, left/rightleg, left/rightfoot`. Warnings (not failures) for leftover namespace prefixes, no finger joints (`handindex/middle/ring/pinky/thumb`), and no toe joints.
7. `normalise_origin`: measure the bind mesh's world centre Y (`uthana/glb_measure.measure`); if more than 0.03 m off, shift the hips (`uthana/rig_edit.shift_hips_world`) so the origin is at the character's vertical middle. Uthana returns the **first** character for a repeated identical upload, with that upload's flags, so `feet_on_floor` is never assumed to have taken effect.
8. `supersede_tripo_outputs`: one provider per asset — earlier Tripo rig/animation files are moved (not deleted) to `.superseded/` and their `provider: tripo` entries are removed from `asset_rig.json` / `asset_animation.json`.
9. Save the ledger (pruned to manifest assets), the routing plan, and the manifest sync.

### Settings

| Setting | Env var | Default |
|---------|---------|---------|
| `uthana_enabled` | `UTHANA_ENABLED` | `true` |
| `uthana_api_key` | `UTHANA_API_KEY` (also read from the repo `.env` by the client) | none |
| `uthana_avatar_min_confidence` | `UTHANA_AVATAR_MIN_CONFIDENCE` | 0.20 |
| `uthana_npc_min_confidence` | `UTHANA_NPC_MIN_CONFIDENCE` | 0.20 |

### Resume / regeneration

Delete the asset's entry from `uthana_rig.json` (or `model_uthana_rigged.glb`) to force a re-rig. Because Uthana de-duplicates identical uploads, re-uploading the same mesh returns the same character.

### SSE events

`running` → `info` (uploading, rigged with confidence and `character_id`, retired Tripo output) → `warning` (rig warnings, fallbacks) → `succeeded` (`"N rigged, M reused, K fell back"`).

### Quest notes

Nothing target-specific. The skinned loader later folds the measured model frame into the root joints, so the centred origin is what scene_compile's `model_frame` / `foot_offset_m` expect.

## See also

- [`docs/uthana.md`](../../../uthana.md)
- Previous: [`07b_character_routing`](../07b_character_routing/README.md) · Next: [`08_asset_rigging`](../08_asset_rigging/README.md)
