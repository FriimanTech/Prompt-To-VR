# Stage 2b: Interaction Design

> **Pipeline position:** after `asset_manifest` → before `npc_plan`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_interaction_design.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_interaction_design.py)  
> **Primary artifacts:** `projects/<project_id>/interaction_design.json` (and a reconciled `asset_manifest.json`)

## Main stage summary

### What this stage is

The **interaction design** decides, once, how every non-character object is used: its `usage` (held weapon / tool / shield, thrown, worn, ammo, projectile, static prop, not interactive), who wields it, its **hold** (`one_hand`, `two_hand_line`, `two_hand_wrap`, `forearm_strap`), the **sockets** its mesh needs (each a `kind` plus a plain-words `part_description` for the vision stage), and for weapons the `weapon` / `melee` / `shield` / `bow` spec plus the `projectiles` they fire.

### Why it exists

Previously every prop got the same two guessed sockets (`muzzle`, `grip`) and every game generated its own shooting code. Now sockets are found per item on the real mesh (`socket_generation`) and weapons run on engine built-ins tuned from this document. It runs before `npc_plan` and before any paid image / Tripo call, so seen projectiles (rockets, arrows) are added to the manifest in time.

### Inputs

| Input | Location |
|-------|----------|
| User prompt (verbatim, first 20 000 chars) — the source of explicit decisions | `projects/<id>/prompt.txt` |
| BuildSpec | `build_spec.json` |
| AssetManifest | `asset_manifest.json` |

### Outputs

| Output | Location |
|--------|----------|
| `InteractionDesign` (`items` ≤64, `projectiles` ≤16, `omitted`, `notes`) | `projects/<id>/interaction_design.json` |
| Reconciled AssetManifest (written only if changed) | `asset_manifest.json` |
| Agent log and reconcile notes | project log `interaction_design` |

### Where output goes next

- `npc_plan`, `game_design`, `mechanic_plan`: one line per item (`design_excerpt`).
- `mechanic_plan` / `code_generation`: `weapon_plan.py` maps every designed weapon onto `builtin.weapon` / `bow` / `melee_weapon` / `shield` (+ `item_source`, `npc.ranged_attack`).
- `socket_generation`: which sockets to find on each mesh and what the part looks like; `asset_contract` merges the results.
- `asset_optimization`, `scene_compile` (weapon bindings, projectile templates / table, projectile sizing).

### Models, providers and effort

OpenAI Agents SDK agent (Responses API): `interaction_design_model` = `gpt-5.6-sol`, reasoning effort `high` (set explicitly, because the SDK otherwise infers `none` for this model), verbosity `medium`, `WebSearchTool` on (real dimensions, calibres, rates of fire). System prompt `INTERACTION_DESIGN_SYSTEM`. Not routed through the Claude `LlmClient`.

### Failure behavior

- No OpenAI key, or the agent fails → warning, and a deterministic `fallback_design` derived from asset names is used.
- No non-character assets → an empty design (`omitted: ["no held or interactive objects"]`), no agent call.
- The stage **fails** (`InteractionDesignError`) only if violations remain after repair and quote downgrading.

---

## Detailed stage deep dive

### Execution trace (`run_with_designer`)

1. Load BuildSpec, manifest and prompt. Props = items whose category is not `character`.
2. `_design`: build `InteractionDesigner` (session `ptv-interaction-design-<project_id>`) and a message from `build_designer_message` (process steps, extra instructions, prompt, BuildSpec JSON, manifest JSON). Extra instructions name the target device — on Quest: "prefer hybrid over simulated bullets and give a projectile a mesh only when it is seen"; on PC VR: "invisible bullets are hybrid" — and "Only non-character assets belong in `items`".
3. `designer.run`: output schema via `make_capturing_schema(strict=False)`; on a repairable schema error it retries up to `interaction_design_max_retries` times with the issues listed.
4. **Repair rounds**: up to `interaction_design_max_repair_rounds` times, `interaction_violations` findings are sent back to the same agent for a complete corrected design.
5. `repair_interaction_design` (deterministic completion), then `interaction_violations` again; leftovers → `downgrade_unquoted` → check again; still bad → fail.
6. `reconcile_manifest`; save the manifest if it changed, save the design, record `"<source>: N item(s), H held, W weapon(s), P projectile(s), M with a mesh"` (source = `designer`, `fallback` or `empty`).

### Provenance

Every decision (item, hold, weapon, …) carries `provenance`: `explicit` with a `prompt_quote` that must appear verbatim (whitespace/case-normalised) in the prompt, or `inferred` with a `rationale`. Explicit wins ("a Glock that fires a laser beam" fires a beam). Quotes that can't be found are downgraded to inferred.

### Validator (`interaction_rules.interaction_violations`)

Reports: duplicate assets; ids not in the manifest; characters in the design; duplicate socket names; hold sockets not in `sockets`; a held usage with no hold kind; a hold without `primary_socket`; `forearm_strap` without `forearm_socket`; two-hand holds without `support_socket`; scenery with sockets; ranged weapons without a `muzzle`; `projectile` / `hybrid` weapons without a valid `projectile_id`; `beam` without `beam` params; `drawn_launcher` without `bow`; bows missing `arrow_rest` / `string_rest` / `limb_tip_top` / `limb_tip_bottom` or a valid arrow projectile; melee without `strike_tip` / `strike_face`; duplicate projectile ids; unverifiable explicit quotes.

### Deterministic repair (`repair_interaction_design`)

- Drops unknown, character and duplicate entries; scenery loses sockets and hold.
- Held items with no hold get `one_hand` (`forearm_strap` for shields) and the missing sockets (`grip`, `support_grip`, `grip_wrap`, `hand_strap`, `forearm_strap`); two-hand holds default `two_hand: optional`.
- Ranged weapons get a `muzzle`, `aim_aligned`, trigger finger pose, and default wielder `player`.
- Missing projectiles are created (pistol round 360 m/s or energy bolt 60 m/s); a non-explicit `projectile` weapon firing bullets faster than `VISIBLE_PROJECTILE_MAX_MPS` (150 m/s) becomes `hybrid` unless the prompt mentions bullet drop / ballistics / travel time; hybrid with no visual gets a tracer; beams get default `BeamSpec` and `hold_beam`.
- Bows get their four sockets and an `arrow` projectile (0.75 m, sticks on hit, has a mesh).
- Melee gets `strike_face` (blunt) or `strike_base` + `strike_tip`.
- Fast non-explicit bullets lose `needs_mesh`; rockets, arrows and grenades always get one.

### Fallback design (`fallback_design`)

For `prop` items only, by name: bow, launcher (RPG rocket), rifle (hybrid auto 700 rpm, 5.56 mm), pistol / blaster (hybrid; energy bolt for blaster / ray / phaser / laser), shield, sword, blunt weapon. Everything else is left out. It then runs the same repair.

### Manifest reconcile (`reconcile_manifest`)

- `socket_hints` mirror the design's socket names (max 8).
- `real_world_dimensions.length_m` replaces `target_scale_meters` when they differ by more than 15%.
- Held weapons / tools / shields with no `importance` become `hero`.
- Each projectile with `needs_mesh` gets a `prop` asset (`<id>_projectile`, real size, `background`, `max_triangles` 600, `nose` socket; arrows also `nock`) and a matching `projectile` / `ammo` item; bows get `arrow_asset_id`. If the manifest is full (48 items) the projectile is drawn as a tracer instead.
- Engine-drawn effect meshes (muzzle flash, tracer, beam, trail, explosion, impact… `engine_drawn_fx_assets`) that nothing holds, wears or fires are dropped (never to an empty manifest) — `ctx.vfx` draws them.

### Settings (`settings.py`, env var = upper-case name)

| Setting | Default |
|---------|---------|
| `interaction_design_model` | `gpt-5.6-sol` |
| `interaction_design_reasoning_effort` | `high` |
| `interaction_design_verbosity` | `medium` |
| `interaction_design_agent_max_turns` | 40 (4..200) |
| `interaction_design_max_retries` | 1 (0..4) |
| `interaction_design_max_repair_rounds` | 2 (0..6) |
| `interaction_design_web_search` | `true` |
| `openai_api_key` | from `.env` (copied into `OPENAI_API_KEY` for the SDK) |

### Important files

| File | Role |
|------|------|
| `pipeline/stage_interaction_design.py` | Stage |
| `pipeline/interaction_design_agent.py` | `InteractionDesigner`, `build_designer_message` |
| `pipeline/interaction_rules.py` | Validator, repair, fallback, reconcile, `design_excerpt` |
| `pipeline/agent_output_capture.py` | Schema capture / output diagnosis |
| `pipeline/prompts.py` | `INTERACTION_DESIGN_SYSTEM` |
| `packages/schemas/ptv_schemas/interaction_design.py` | Schema |

### Quest-specific notes

Only the device instruction differs: on Quest the agent is steered toward hybrid fire and meshes only for visible projectiles, to save GPU and draw calls on the mobile chip.

## See also

- [`docs/sockets.md`](../../../sockets.md) — socket kinds and frames
- [`docs/weapons.md`](../../../weapons.md) — weapon built-ins driven by this design
- Next stage: [`03_npc_plan`](../03_npc_plan/README.md)
