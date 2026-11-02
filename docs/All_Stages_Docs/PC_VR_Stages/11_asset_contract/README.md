# Stage 11: Asset Contract (Sockets + Model Frame)

> **Pipeline position:** after `socket_generation` → before `world_generation`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_asset_contract.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_asset_contract.py), [`pipeline/model_frame.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/model_frame.py)  
> **Primary artifacts:** `projects/<project_id>/asset_contract.json`

## Main stage summary

### What this stage is

The **asset contract stage** writes one record per asset with:

- its **named sockets** (`muzzle`, `grip`, …) in unit space — taken from `socket_generation` when that stage covered the asset, or, for projects without an interaction design, derived from geometry by the deterministic `ptv_glb_sockets` tool;
- for characters and skinned assets, its measured **model frame** (sole height, height, forward direction).

### Why it exists

Code-generation agents never see a mesh; they write `ctx.sockets->socket_world(self, "muzzle")` and need the socket **names** in context. The scene compiler needs exact numbers behind those names, and needs to know how to stand and face every character: Tripo rigs stand on their origin and face +X, Uthana rigs are hips-centred and face +Z, and the engine faces −Z. Measuring once here replaces per-provider guesses that left NPCs floating or walking sideways.

### Inputs (what enters)

| Input | Location |
|-------|----------|
| AssetManifest | `asset_manifest.json` — `socket_hints`, `category`, `rigged`, `forward_hint` |
| Normalization index | `asset_normalization.json` — GLB per asset (`glb_source` or `glb_path`, repo-relative) |
| Socket generation | `socket_report.json` |
| InteractionDesign | `interaction_design.json` (if present, assets it gave no sockets get none) |

### Outputs (what it produces)

| Output | Location |
|--------|----------|
| Asset contract | `asset_contract.json` |

Every manifest asset gets an entry: `status` (`ok`, `failed`, `skipped`, `missing`), `reason`, `space: "unit"`, `sockets`, `socket_confidence`, `socket_hints`, `unsatisfied_hints`, `bounds`, `source_glb`; from socket generation also `hold`, `part_regions`, `cross_section_unit`, `overlay_paths`, `warnings`, `method`; from the geometry tool `principal_axis` and AABB; and `frame` when measured.

### Where output goes next

- **`code_generation`** — socket names in each agent's context.
- **`scene_compile`** — `mesh.sockets` / hold in metres (× `target_scale_meters`), `mesh.model_frame`, `foot_offset_m`, height-based `unit_scale`; validates CodeTask `required_sockets`; fails on a skinned character with no frame.

### Models / providers

None. Deterministic; the only external piece is the `ptv_glb_sockets` C++ tool (principal-axis PCA + mass asymmetry), located under `engine/build` or built once with CMake.

### Failure behavior

`asset_contract.json` is always written first; then the stage **fails** (`AssetContractError`), listing every problem, when:

- a GLB `asset_normalization.json` vouched for is not on disk (`status: missing`);
- an asset with explicit `socket_hints` failed geometric derivation (`status: failed`);
- a rigged asset's model frame could not be measured (for example arm joints but no foot/toe joint).

Failures on hint-less assets, and unrigged frames that fail, are recorded but tolerated. Not finding or building `ptv_glb_sockets` raises `RuntimeError`.

---

## Detailed stage deep dive

### Per-asset decision

For each asset whose normalization status is `ok` / `warn_no_material` (paths resolved against `settings.repo_root`, never the working directory):

1. **Frame** (`_frame_for`): for `category == character`, or an asset normalization marks `skinned`, `model_frame.try_measure(glb, rigged, forward_hint)`.
2. **Socket generation covered it** (in `socket_report.json`) → `_from_generated`: sockets, confidences, hold, part regions, cross-section and overlays copied over; `status` `ok` or `failed`.
3. **Interaction design exists but gave this asset no sockets** → `skipped`, `sockets: {}` (no guessed muzzles on targets and racks).
4. **Legacy project (no design)**: `_not_socketable` — explicit `socket_hints` always derive; otherwise only `prop` gets the implicit `muzzle` + `grip`. Characters (rig joints / `BodySlots`), UI (body slot + `IInteractive`) and FX (spawned at another asset's socket) are skipped with a written `reason`.
5. **Derive** (`_derive`): `ptv_glb_sockets --input <glb> --output <json>`; with hints, only hinted sockets are published and the rest are `unsatisfied_hints`; a hint-less prop gets the full derived set; confidences default to `heuristic`.

Assets with no usable GLB get `skipped` with reason "no valid GLB in asset_normalization.json", so the scene compiler can say "no contract" instead of finding nothing.

### Model frame (`model_frame.measure`)

From the skinned bind pose (≤20,000 sampled vertices) and joint world positions:

- `sole_y`, `top_y`, `height`, `center_xz`, `raw_longest_axis`, `lowest_foot_joint_y`;
- **forward** = up × the shoulder line (`upper_arm` or `shoulder` joints), confirmed by the foot → toe direction; if they disagree the toes win (mirrored side names) with a warning; with no skeleton, the manifest `forward_hint` (`+x`, `-x`, `+z`, `-z`), else `+z` with a warning; snapped to an axis within 12°;
- `yaw_offset_rad`, `forward_snapped`, `method` (`shoulders`, `shoulders+toes`, `toes`, `hint`, `default`), `status` (`measured` / `hint` / `default` / `failed`), `biped`, `t_posed`, `warnings`, `units: "glb"`.

A rigged mesh with arm joints but no foot/toe joint raises `ModelFrameError` (blocking). A non-biped rig without feet uses the mesh minimum with a warning.

### Unit space

Socket positions are divided by the longest-axis extent of the source GLB, so they survive the LOD chain built later by `asset_optimization`; `scene_compile` multiplies by `target_scale_meters`, as it does for `bounds_radius_m`.

### Tool build

`_ensure_tool` builds target `ptv_glb_sockets` into `engine/build/vs2026-x64` using `cmake_generator`, `cmake_generator_platform`, `cmake_build_config`, `vcpkg_root`. This also warms the engine build tree used later by codegen's compile-verify. It is only needed for legacy derivation.

### Settings

No settings of its own; uses the CMake/vcpkg settings above (`CMAKE_GENERATOR`, `CMAKE_GENERATOR_PLATFORM`, `CMAKE_BUILD_CONFIG`, `VCPKG_ROOT`).

### Quest notes

Nothing Quest-specific. The frame and sockets are what `quest_runtime_verify` later checks against (feet within 5 cm of the floor, shots from within 2 cm of the muzzle).

## See also

- [`docs/sockets.md`](../../../sockets.md) — sockets and holds
- [`docs/architecture.md`](../../../architecture.md) — socket contract
- [`docs/uthana.md`](../../../uthana.md) — Uthana rig orientation
- [`docs/npc-and-ai.md`](../../../npc-and-ai.md) — how characters are stood and faced
