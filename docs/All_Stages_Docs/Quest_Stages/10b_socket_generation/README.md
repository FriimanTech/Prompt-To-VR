# Stage 10b: Socket Generation

> **Pipeline position:** after `asset_normalization` → before `asset_contract`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_socket_generation.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_socket_generation.py), [`pipeline/sockets/`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/sockets/)  
> **Primary artifacts:** `assets/<id>/sockets/result.json`, `socket_report.json`

## Main stage summary

### What this stage is

The **socket generation stage** finds, on each real mesh, the sockets the
interaction design asked for: where the palm goes on a handle, where shots
leave a barrel (and how wide the bore is), where a shield's straps are, a
blade's base and tip, a hammer's face, a bow's string and limb tips, a
projectile's nose and nock. It renders the mesh, has a vision model segment
the named parts, maps the masks back onto the mesh's triangles, solves exact
frames with geometry, then draws the result on the mesh and asks the vision
model whether a person would hold it that way.

### Why it exists

Tripo returns one raw mesh with no part names. The old geometric guess put
pistol grips on slides and RPG muzzles on the exhaust. A wrong socket is a
gun held upside down or a shot that starts inside the hand.

### Inputs (what enters)

| Input | Location |
|-------|----------|
| InteractionDesign (sockets, part descriptions, hold) | `interaction_design.json` |
| Normalized GLBs | `asset_normalization.json` → `assets/<id>/...glb` |
| AssetManifest (names, summaries, scale) | `asset_manifest.json` |

### Outputs (what it produces)

| Output | Location |
|--------|----------|
| Per-asset sockets (unit space), hold forward/up, part regions, cross-section, overlays | `assets/<id>/sockets/result.json` |
| Stage summary | `socket_report.json` |

### Where output goes next

- **`asset_contract`** -- merged into `asset_contract.json`
- **`scene_compile`** -- `mesh.sockets` / `mesh.hold` in metres; muzzle bore and projectile cross-section for the projectile scale check

## Detailed stage deep dive

1. `ptv_glb_views` (C++) renders 10 views with a triangle-id buffer and depth.
2. Gemini (`socket_vision_model`, Interactions API) segments each part per
   view; masks are decoded under every coordinate convention and the one
   matching `box_2d` wins.
3. Multi-view voting fuses masks into triangle regions; per-kind numpy
   solvers (`sockets/solvers.py`) produce frames (OpenXR grip frame for hand
   sockets, +Z-facing for the rest).
4. Render-and-verify: overlays are drawn upright and checked; one retry with
   the critique; then the geometry fallback is verified too and whichever
   passes wins, else the socket is kept labelled `low`. A required socket
   that cannot be produced fails the stage.
5. Cache: GLB hash + design spec + `SOCKET_PIPELINE_VERSION`, so a resume
   never re-bills and never moves a socket. Without a Google key the
   geometry fallback runs and sockets are labelled `heuristic`.

### Settings

`socket_vision_model` (gemini-3.8-flash), `socket_vision_enabled`,
`socket_vision_max_parallel`, `socket_view_size`, `socket_verify_rounds`,
`socket_vision_timeout_s`.

See [docs/sockets.md](../../../sockets.md).
