# Stage 10b: Socket Generation

> **Pipeline position:** after `asset_normalization` → before `asset_contract`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_socket_generation.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_socket_generation.py), [`pipeline/sockets/`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/sockets/), [`packages/provider-clients/ptv_providers/gemini_vision_client.py`](../../../../packages/provider-clients/ptv_providers/gemini_vision_client.py)  
> **Primary artifacts:** `assets/<id>/sockets/result.json`, `projects/<project_id>/socket_report.json`

## Main stage summary

### What this stage is

The **socket generation stage** finds, on each real mesh, the sockets the interaction design asked for: where the palm goes on a handle, where shots leave a barrel (and the bore diameter), a shield's straps, a blade's base and tip, a hammer's face, a bow's string and limb tips, a projectile's nose and nock. It renders the mesh, has a vision model segment the named parts, maps the masks onto the mesh's triangles, solves exact frames with geometry, then draws the result on the mesh and asks the vision model whether it is right.

### Why it exists

Tripo returns one mesh with no part names. A purely geometric guess put pistol grips on slides and RPG muzzles on the exhaust; a wrong socket is a gun held upside down or a shot that starts inside the hand.

### Inputs (what enters)

| Input | Location |
|-------|----------|
| InteractionDesign | `interaction_design.json` — per item `sockets` (name, `kind`, `part_description`, `required`), `hold`, `usage`, `weapon` / `melee` / `bow` |
| Normalized GLBs | `asset_normalization.json` (`glb_source` or `glb_path`, status `ok` / `warn_no_material`) |
| AssetManifest | `asset_manifest.json` — name, `style_notes`, `target_scale_meters`, `socket_hints` |

Projects from before `interaction_design` use `fallback_design(manifest)` restricted to items that have `socket_hints`.

### Outputs (what it produces)

| Output | Location |
|--------|----------|
| Per-asset result (cache) | `assets/<id>/sockets/result.json` — `status`, `sockets` (unit space), `hold`, `part_regions`, `cross_section_unit`, `overlays`, `verified`, `method`, `warnings`, `missing_required`, `cache_key`, `source_glb` |
| Working files | `assets/<id>/sockets/views/` (renders, id buffers, overlays), `views_alt/`, `vision_round<N>.json`, `markers*.json`, `region_<part>.json` |
| Stage summary | `socket_report.json` (all results keyed by asset id) |

Each socket: `position` (unit space = mesh units ÷ longest extent), `rotation` (quaternion xyzw), `kind`, `confidence` (`vision_verified`, `vision`, `heuristic`, `low`, `synthesized`) and, when relevant, `bore_diameter_unit`, `handle_thickness_unit`, `lateral_offset_unit`, `mirror`, `wrap_of`.

### Where output goes next

- **`asset_contract`** merges sockets, hold, part regions and cross-section into `asset_contract.json`.
- **`scene_compile`** emits `mesh.sockets` / hold in metres and uses the muzzle bore and projectile cross-section to size projectiles.

### Models / providers

- **Gemini** `socket_vision_model` = `gemini-3.8-flash` via the Interactions API (`GeminiVisionClient`). The stage requests `thinking_level="minimal"`; the client falls back to `low` (and remembers it) because this model rejects `minimal`.
- **`ptv_glb_views`** (C++ engine tool): renders views plus a triangle-id buffer and depth; found under `engine/build` or built once with CMake (target `ptv_glb_views`).

### Cost

Gemini calls per view per round; cached by GLB hash + spec + pipeline version, so a Resume makes none.

### Failure behavior

- Without `GOOGLE_API_KEY`, or with `socket_vision_enabled=false` → warning, geometry-only regions, every socket `heuristic`.
- Verification unavailable → warning, the vision solve is kept.
- Any **required** socket that cannot be produced (or an asset with no normalized GLB on disk) → `socket_report.json` is written, then the stage **fails** (`SocketGenerationError`) listing every one.
- `ptv_glb_views` cannot be built → `RuntimeError`.

---

## Detailed stage deep dive

### Work list and parallelism

Items in the design that have `sockets` and exist in the manifest. Two assets run at a time (`ThreadPoolExecutor(max_workers=2)`); each fans out its views up to `socket_vision_max_parallel`.

### Cache

`cache_key` = SHA-256 of the GLB bytes + the item's `sockets`, `hold`, `usage`, `weapon`, `melee`, `bow` + `SOCKET_PIPELINE_VERSION` (currently `"2"`) + the model name (or `geometry`). A matching `result.json` is returned as-is (`cached: true`), so a resume never re-bills and never moves a socket.

### Per-asset algorithm (`sockets/generate.py: generate_for_asset`)

1. **Render** (`views.render_views`): `ptv_glb_views --input <glb> --out-dir <dir> --size <socket_view_size>` writes ten views, per-view id buffers and the merged mesh, all in the mesh's own coordinates.
2. **Segment** (`vision.segment_views`): `parts_for(item)` lists the parts to find (socket parts plus context parts); Gemini returns masks per view. `decode_mask` tries each coordinate convention and keeps the one matching the returned `box_2d`.
3. **Fuse** (`fusion.Fuser.region`): every mask pixel names a triangle. Score per triangle = mean over views that see it of (mask pixels ÷ visible pixels); triangles with score ≥ 0.5 form the region, cut to its largest connected piece (by position-quantised shared vertices, so UV seams do not split it).
4. **Solve** (`solvers.solve_item`): per-kind numpy solvers — handles (axis + palm-height centre, thickness), muzzles (bore centre + diameter), support grips, blade base/tip, hammer face, projectile nose/nock, bow string and limb tips, shield straps. Hand sockets are in the OpenXR grip frame; the rest face +Z.
5. **Verify** (`verify.verify`, if `socket_verify_rounds > 0`): markers are rendered on upright views and Gemini answers per socket whether it is right. Failures trigger a re-segment with the critique as a note. After the last round, the geometry fallback (`fallback.heuristic_regions`) is solved and verified for the failing sockets; per socket the passing answer wins (labelled `heuristic`), otherwise the vision solve stays, labelled `low`. `synthesized` sockets (parts the mesh does not have, e.g. an engine-drawn bowstring) are not verified.
6. **Fallback**: no vision, or vision produced nothing → geometry regions, all `heuristic`.
7. **Projectiles / ammo**: `cross_section_unit` = 2 × 98th-percentile radius across the travel axis.
8. Final overlays are always rendered for the UI; `missing_required` lists required sockets not produced, which sets `status: failed`.

### Settings

| Setting | Env var | Default |
|---------|---------|---------|
| `socket_vision_model` | `SOCKET_VISION_MODEL` | `gemini-3.8-flash` |
| `socket_vision_enabled` | `SOCKET_VISION_ENABLED` | `true` |
| `socket_vision_max_parallel` | `SOCKET_VISION_MAX_PARALLEL` | `6` (1–32) |
| `socket_view_size` | `SOCKET_VIEW_SIZE` | `768` (256–1536) |
| `socket_verify_rounds` | `SOCKET_VERIFY_ROUNDS` | `1` (0–3; 0 = no verify) |
| `socket_vision_timeout_s` | `SOCKET_VISION_TIMEOUT_S` | `120.0` |
| `google_api_key` | `GOOGLE_API_KEY` | — |

### Quest notes

Nothing Quest-specific here. On Quest, `quest_runtime_verify`'s weapons launch checks that shots leave within 2 cm of the muzzle these sockets define.

## See also

- [`docs/sockets.md`](../../../sockets.md) — socket kinds, frames, confidence
- [`docs/weapons.md`](../../../weapons.md) — how weapons use sockets
- [`docs/provider-setup.md`](../../../provider-setup.md) — Google key
