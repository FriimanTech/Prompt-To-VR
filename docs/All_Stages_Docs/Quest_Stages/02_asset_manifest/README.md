> **Shared content:** Identical to [PC_VR_Stages/02_asset_manifest](../../PC_VR_Stages/02_asset_manifest/README.md). Update both copies when this stage changes.

# Stage 2: AssetManifest

> **Pipeline position:** after `build_spec` → before `npc_plan`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_asset_manifest.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_asset_manifest.py)  
> **Primary artifacts:** `projects/<project_id>/asset_manifest.json`

## Main stage summary

### What this stage is

The **AssetManifest stage** plans every **3D object** the game needs: characters, props, weapons, FX, and UI meshes. It produces `asset_manifest.json`, a list of `AssetItem` entries with generation strategy, scale, collision hints, rigging flags, animation intents, and socket hints.

### Why it exists

BuildSpec says *what* the game is; AssetManifest says *which GLB files* Tripo must produce. Separating object assets from the VR **environment** (which is C++ world generation, not Tripo) keeps the 3D pipeline focused and cheaper.

### Inputs (what enters)

| Input | Location |
|-------|----------|
| BuildSpec | `projects/<id>/build_spec.json` |

### Outputs (what it produces)

| Output | Location |
|--------|----------|
| AssetManifest | `projects/<id>/asset_manifest.json` |

Each item includes: `id`, `category`, `strategy` (`image_to_3d` or `text_to_3d`), `reference` prompt text, `tripo` settings, `rigged`, `animations[]`, `socket_hints`, `target_scale_meters`, `collision_shape`.

### Where output goes next

- **`npc_plan`** may rewrite `rigged` / `animations` to match prompt intent.
- **`reference_images`**, **`tripo_models`**, rigging, animation, normalization, contract, optimization, and **`scene_compile`** all read the manifest.

### Key concepts and terms

- **AssetItem** — One row in the manifest; maps 1:1 to a folder under `projects/<id>/assets/<asset_id>/`.
- **Environment category** — Items tagged `environment` are **dropped** if any non-environment items exist. The world is built in C++ (`world_generation`), not as a Tripo GLB.
- **Rigging / animations** — Initial hints only; `npc_plan` reconciles them before credits are spent on Tripo rigging.

### End-to-end flow (plain language)

1. Load BuildSpec JSON.
2. Call **Claude Sonnet** with `ASSET_MANIFEST_SYSTEM` and the BuildSpec embedded in the user message.
3. Validate response as `AssetManifest`.
4. Post-filter: remove stray `environment` items when other objects exist.
5. Save manifest and succeed.

---

## Detailed stage deep dive

### Exact execution trace (`stage_asset_manifest.run`)

| Step | Action | Detail |
|------|--------|--------|
| 1 | emit + `record_stage(running)` | Standard stage bookkeeping |
| 2 | Load BuildSpec | `ctx.build_spec` cache or `load_build_spec()` |
| 3 | `make_llm()` | Same factory as stage 1 |
| 4 | Embed full BuildSpec JSON in user message | Instructs model to emit AssetManifest |
| 5 | `generate_json(..., model=claude_light_model, system=ASSET_MANIFEST_SYSTEM)` | **Sonnet**, not Opus — cheaper planning pass |
| 6 | `AssetManifest.model_validate()` | Schema enforcement |
| 7 | **Environment post-filter** | See below |
| 8 | `save_asset_manifest()` | Writes disk; caches on `ctx.asset_manifest` |
| 9 | Success message | Includes dropped environment count if any |

### Environment filter (deterministic, after LLM)

```python
env_items = [i for i in manifest.items if i.category == environment]
non_env = [i for i in manifest.items if i.category != environment]
if env_items and non_env:
    manifest = manifest with items=non_env only
```

The VR **environment** is authored in C++ (`world_generation`), not Tripo. The LLM prompt discourages environment GLBs, but this filter is belt-and-suspenders. If the manifest would become empty (only environment items), Pydantic may fail — that is a real failure mode.

### Per-AssetItem contract for downstream stages

| Field | Purpose | Overwritten later? |
|-------|---------|-------------------|
| `id` | Directory `assets/<id>/`, all references | No |
| `category` | `character` → npc_plan; props/weapons/etc. | No |
| `strategy` | `image_to_3d` → stage 6+7; `text_to_3d` skips images | No |
| `reference.*` | Image/Tripo prompts | No |
| `rigged`, `animations[]` | Rigging/animation gates | **Yes — npc_plan reconcile** |
| `socket_hints` | Names for `ptv_glb_sockets` | No |
| `target_scale_meters` | World scale, capsule height, socket scale | No |
| `collision_shape` | Metadata → scene physics | No |

### Artifact I/O

| Read | Write |
|------|-------|
| `build_spec.json` | `asset_manifest.json` |
| | `project_state.json` stage record |

**Note:** Stage 3 may rewrite `asset_manifest.json` in the same pipeline pass after saving npc_plan.

### SSE events

`running` → `succeeded` with `"N assets"` or `"N assets (dropped M environment asset(s); world is C++-generated)"`.

### Resume / idempotency

No skip logic — each run calls the LLM and overwrites the manifest. If you resume at stage 2, stage 1's BuildSpec is assumed already on disk.

### Failure modes

| Condition | Result |
|-----------|--------|
| LLM invalid JSON | Stage fails |
| Manifest empty after filter | Validation error (requires ≥1 item) |
| npc_plan reconcile (stage 3) | Clears `rigged`/`animations` for slide NPCs — not a failure |

### Important files

| File | Role |
|------|------|
| `stage_asset_manifest.py` | Stage + filter |
| `asset_manifest.py` (schema) | `AssetItem`, `AssetCategory`, strategies |
| `prompts.py` | `ASSET_MANIFEST_SYSTEM` — objects only, not environment |

### Settings

| Setting | Value |
|---------|-------|
| `claude_light_model` | `claude-sonnet-4-6` (explicitly passed to API call) |

### LLM prompt contract

`ASSET_MANIFEST_SYSTEM` instructs objects-only output, rig/animation hints, socket_hints, and scale. The environment filter is code-enforced because models sometimes still emit environment GLBs.

### After stage 3

Post-npc_plan manifest is authoritative for `rigged`/`animations` — stage 2 values may be overwritten.

---

## Extend this folder

- `artifacts.md` — AssetItem field reference
- `schemas.md` — category and strategy enums
- `failure-modes.md` — reconcile with npc_plan

## See also

- [`docs/architecture.md`](../../../architecture.md) — objects vs C++ environment split
- Stage 3 [`03_npc_plan`](../03_npc_plan/README.md) — manifest reconciliation
