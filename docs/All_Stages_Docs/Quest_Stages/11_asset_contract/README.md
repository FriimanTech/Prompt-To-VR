> **Shared content:** Identical to [PC_VR_Stages/11_asset_contract](../../PC_VR_Stages/11_asset_contract/README.md). Update both copies when this stage changes.

# Stage 11: Asset Contract (Sockets)

> **Pipeline position:** after `asset_normalization` → before `code_generation`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_asset_contract.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_asset_contract.py)  
> **Primary artifacts:** `projects/<project_id>/asset_contract.json`

## Main stage summary

### What this stage is

The **asset contract stage** derives **named sockets** (attachment points like `muzzle` and `grip`) from each asset's geometry using deterministic math — no AI calls. Results are stored in **unit space** (normalized by longest axis) so they survive LOD scaling.

### Why it exists

The code-generation agent never sees the GLB file. It cannot know where a gun barrel ends. Instead it writes `ctx.sockets->socket_world(self, "muzzle")` — but only if this stage proved a `muzzle` socket exists on that asset.

Running **before codegen** gives agents the contract in context. Running **before optimization** is safe because sockets are in unit space; scene compile multiplies by `target_scale_meters`.

### Inputs (what enters)

| Input | Location |
|-------|----------|
| AssetManifest | `socket_hints` per item (e.g. `["muzzle", "grip"]`) |
| Normalization | `asset_normalization.json` (valid `glb_path` per asset) |

### Outputs (what it produces)

| Output | Location |
|--------|----------|
| Socket contract | `asset_contract.json` |

Per asset: socket name → position + orientation in unit space; `unsatisfied_hints` when geometry cannot support a requested socket.

### Where output goes next

- **`code_generation`** — agents read socket names in context
- **`scene_compile`** — scales sockets to metres; validates `required_sockets` from CodeTasks

### Key concepts and terms

- **Socket** — A named local transform on an asset ("muzzle points here"). Used for weapon firing, attachments, and equip.
- **Unit space** — Coordinates divided by longest-axis extent so decimation/LOD does not invalidate offsets.
- **ptv_glb_sockets** — C++ tool using PCA (Principal Component Analysis) + mass asymmetry to guess muzzle/grip locations from mesh shape.
- **Socket contract (three-clause)** — Manifest hints → this stage derives → scene compile validates against codegen requirements. See [`docs/architecture.md`](../../../architecture.md).

### End-to-end flow (plain language)

1. For each resolvable asset, run `ptv_glb_sockets` subprocess on source GLB.
2. Collect requested hints from manifest.
3. Record satisfied sockets + unsatisfied hints.
4. Save `asset_contract.json`.

---

## Detailed stage deep dive

### Why before codegen

Codegen agents never see GLB bytes. They write `ctx.sockets->socket_world(self, "muzzle")`. This stage proves socket **names** exist in unit space before agents run.

### Execution

1. Load manifest + `asset_normalization.json`
2. `_resolvable_assets()` — status `ok` or `warn_no_material` with valid `glb_path`
3. **`_ensure_tool()`** — locate `ptv_glb_sockets` binary; if missing, run `configure_and_build` to compile it (also warms engine build tree for later stages)
4. Per asset: subprocess `ptv_glb_sockets --input <glb> --output <json>`
5. Filter derived sockets to manifest `socket_hints` only
6. Record `unsatisfied_hints` when geometry cannot support requested name
7. Write `asset_contract.json` — positions in **unit space** (longest-axis normalized)

Scene compile multiplies by `target_scale_meters` to metres.

### Socket contract three-clause model

| Clause | Owner |
|--------|-------|
| Which names to derive | manifest `socket_hints` |
| Geometry → unit positions | **this stage** |
| Codegen requires + compile validates | scene_compile `_check_required_sockets()` |

### Failure modes

Cannot build/find tool → RuntimeError. Per-asset derive error → `status: failed` in report entry; stage may still succeed.

### SSE

`running` → `info` (building tool) → `log` (cmake output lines) → `succeeded`

### Three-clause socket model

Hints (manifest) → derive (this stage) → validate (scene_compile vs codegen required_sockets). No single stage owns all three.

---

## Extend this folder

- `concepts-sockets.md` — PCA and unit space explained
- `artifacts.md` — contract JSON schema

## See also

- [`docs/architecture.md`](../../../architecture.md) — socket contract table
- Stage 12 [`12_code_generation`](../12_code_generation/README.md)
- Stage 15 [`15_scene_compile`](../15_scene_compile/README.md) — socket validator
