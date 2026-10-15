> **Shared content:** Identical to [PC_VR_Stages/07b_character_routing](../../PC_VR_Stages/07b_character_routing/README.md). Update both copies when this stage changes.

# Stage 07b: Character Routing (Tripo vs Uthana)

> **Pipeline position:** after `tripo_models` -> before `uthana_rigging`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_character_routing.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_character_routing.py)  
> **Primary artifacts:** `character_routing.json`, `tripo_rig_check.json`

## Main stage summary

### What this stage is

Decides, for every character, which provider rigs and animates it: **Uthana** (bipedal humanoids, finger joints, styled locomotion) or **Tripo** (whole creatures, preset clips), or neither.

### How it decides

1. **Context per NPC**: NpcPlan entry, AssetManifest item, the BuildSpec mechanics that mention it, the user's prompt, the exact Gemini prompt (`images/<id>/reference_prompt.txt`, or recomposed) and the reference image itself.
2. **Body and riggability**: Tripo's free rig-check runs for every NPC (cached in `tripo_rig_check.json`, reused by `asset_rigging`). The routing agent classifies the body plan. "Biped humanoid" means two legs and two arms, human or not.
3. **Provider**:
   - biped humanoid that Tripo cannot rig -> Uthana (code);
   - non-humanoid creature Tripo can rig -> Tripo (code);
   - non-humanoid not riggable -> none (code);
   - biped humanoid both can rig -> the agent compares needs against the 101 Tripo v1.0 presets and the live Uthana styles.
   - The player avatar always goes to Uthana.
4. **Motions**: every Uthana character gets the shared library idle (`uthana_idle_motion_id`). Actions Uthana cannot provide go to `unmet` with the NPC's `animation_fallback`. Tripo routes may name any v1.0 preset (at most 5).

### Outputs

- `character_routing.json`: one `CharacterRoute` per character (body plan, rig-check, providers, required motions with their sources, unmet, rationale, `decided_by`).
- Manifest `rigged` reconciled.

Agent: `character_routing_model` @ `character_routing_reasoning_effort` (default `high`). On agent failure, deterministic rules apply.

See [docs/uthana.md](../../../uthana.md) for the full Uthana flow.
