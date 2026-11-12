# Rendering

The engine ships a small **forward renderer** (Vulkan) tuned to make the worlds
authored by the world-generation agent read as polished environments while
staying smooth over Quest Link on a mid-range gaming PC. It is deliberately
conservative: MSAA + a gradient sky + scene-driven lighting + a filmic tonemap,
with **no heavy fullscreen post** and **no shadow maps**.

Everything here lives in the engine (`engine/runtime/src/render/Renderer.cpp`,
`engine/runtime/shaders/`). None of it changes the schemas, the pipeline, or the
generated C++ — the world already carries everything the renderer needs in
`world.scene.json`.

## Frame flow (both eyes in one pass on Quest)

```
xrWaitFrame → locate both eye views once (culling and rendering share them)
build the draw list: exact two-eye frustum, room, distance, small-object cull,
  projected-error LOD, merged static batches
begin render pass (multiview: one 2-layer swapchain image, viewMask 0b11;
  MSAA color + depth are lazily allocated transient attachments; the runtime's
  fragment density map is attached for FFR)
  → geometry pass : sorted by pipeline / material / mesh, identical draws
                    instanced (per-draw data in an instance SSBO indexed by
                    gl_InstanceIndex), base-colour texture x factor x COLOR_0
  → sky pass      : after opaque geometry, at the far plane with LESS_OR_EQUAL,
                    so it only shades pixels nothing covered
  → fx pass       : weapon effects -- additive then alpha quads
  → text pass     : SDF glyph quads
  → vignette      : comfort tunnel or gameplay fade to black
end render pass  → MSAA resolve into the swapchain image
submit (two frames in flight on Quest: the CPU records frame N+1 while the
  GPU renders frame N)
```

Every shader selects its eye with `gl_ViewIndex + view_base`, so the same
SPIR-V serves multiview (`view_base` 0) and the one-pass-per-eye fallback
(desktop, or a device without multiview). The frame UBO carries both eyes'
matrices; the push constant block is 48 bytes.

Per-frame resources (command buffer, fence, frame UBO, joint palettes,
instances, FX and text vertices, a timestamp pair) exist once per frame in
flight. Desktop builds keep one frame in flight: Quest Link has shown white
frames when a swapchain image was released with its work still queued.
Meshes evicted while a frame may still draw them are retired and freed once
every such frame has passed its fence.

## Weapon effects (`FxRenderer`, `fx.vert` / `fx.frag`)

`ctx.vfx` (`EffectsSystem`) turns muzzle flashes, tracers, beams, impacts,
decals, explosions, glows and streaks into a fixed ring of procedural quads
(`max_fx_quads`, oldest recycled first), uploaded once per frame and drawn in
two pipelines: additive for light (flashes, tracers, beams, sparks) and
alpha for matter (smoke, dust, decals). Nothing is textured -- shapes are
evaluated in `fx.frag` -- so there is no FX atlas to load. Muzzle flashes and
explosions also add a short-lived point light to the scene UBO, capped at
`max_flash_lights`. Projectiles with a mesh (rockets, arrows) are drawn by
render-only proxy entities copied from their hidden template, so they get
LOD and culling like any mesh (`max_projectile_meshes`). See
[weapons.md](weapons.md).

## World-space text

The engine draws text with a signed-distance-field atlas
(`TextAtlas`, `TextRenderer`): at start-up stb_truetype bakes printable
ASCII from the bundled font (`engine/runtime/assets/fonts/`,
Atkinson Hyperlegible Bold, SIL OFL 1.1 -- chosen for legibility) into one
R8 texture. Each frame `ctx.text->text(...)` submissions are laid out on the
CPU into world-space quads, uploaded once and drawn after the geometry pass
for each eye. `text.frag` derives its edge width from `fwidth`, so glyphs
stay one pixel soft at any distance and do not shimmer as the head moves,
and draws a dark outline so text reads over a bright sky or a dark wall.
Size VR text by angle (`ITextDraw::height_for_angle(distance, 1.4)`), not by
metres. `builtin.ui_panel` draws its title, subtitle and row labels this
way, and `builtin.game_hud` its numbers. A missing font or shader disables
text (with a warning), never the game.

`IApp::request_fade(amount)` reuses the vignette pass with a zero clear
angle to fade the whole view to black; the game loop's respawns and
restarts use it, independent of the player's comfort-vignette setting.

Without multiview, both eyes are recorded sequentially into the same frame's
command buffer (`view_base` 0 then 1). Frames in flight follow the frame-flow
section above: up to two on Quest (`frames_in_flight`), always one on desktop.

## MSAA

- The sample count is queried from the device
  (`framebufferColorSampleCounts & framebufferDepthSampleCounts`) and **clamped
  to 4x**, falling back to 2x then 1x. The chosen level is logged at startup
  (`Renderer ready: … MSAA Nx`).
- When MSAA is active the render pass uses three attachments — a multisampled
  color target, a multisampled depth target (both shared across eyes), and a
  single-sampled **resolve** attachment that is the per-eye swapchain image. At
  1x there is no resolve attachment and the swapchain image is rendered directly.

## Scene-driven lighting (UBO)

Lighting is no longer hardcoded. A single uniform buffer (`set 0, binding 0`,
persistently mapped, refreshed once per frame) carries the environment to both
the sky and geometry shaders:

| Field | Meaning |
|---|---|
| `sun_dir` | Sun **travel** direction (points away from the sun). |
| `sun_color` (`.w`) | Sun color and intensity. |
| `sky_top` (`.w`) | Zenith color (also used as the up-hemisphere ambient + rim); `.w` = exposure. |
| `sky_horizon` (`.w`) | Horizon/fog color and fog density. |
| `ambient` (`.w`) | Ground-bounce color and ambient intensity. |
| `camera_pos[2]` | Each eye's world position (renderer-computed). |
| `misc` (`.x`) | Active point-light count. |
| `points[8]` | Up to 8 point lights (position + range, color + intensity). |

The block starts with both eyes' `view_proj`, `inv_view_proj` and `inv_proj`
matrices.

### The environment is fully author-controlled (no engine overrides)

The renderer is **faithful and neutral**: it imposes no brightness floor and no
hidden heuristics. Every value above comes straight from what the
world-generation agent authored, so a world can be as bright or as dark as
intended. The agent controls all of it through `SkyDesc` / `LightDesc` (see
[`world-generation.md`](world-generation.md) and
[`../engine/world_author_guide.md`](../engine/world_author_guide.md)):

- `ambient_color` + `ambient_intensity` (baseline fill),
- `sun_direction` / `sun_color` / `sun_intensity`,
- per-**point-light** `range` (authored, not derived),
- `fog_density`, and
- `exposure` (global multiplier applied before the tonemap, in both the sky and
  geometry passes).

`Application::build_render_env()` fills a `Renderer::RenderEnv` once after the
scene loads, straight from `scene.sky.*` (and per-light `range`) with **no
floors or derived values**. The only defaults live in the "no authored sky"
branch, so a scene **without** a sky block (e.g. the hand-built starter room)
still looks good; a light that omits `range` falls back to a 20 m default.

## Geometry shading (`scene.frag`)

Per fragment: half-Lambert sun diffuse + a subtle Blinn-Phong specular,
hemisphere ambient (ground → zenith), range-attenuated point lights, a cheap
in-shader **contact AO** (darkens low, downward-facing fragments — a stand-in
for shadow maps), a very-low-frequency, distance-faded albedo variation to break
up flat surfaces without shimmering in VR, distance fog toward the horizon color,
and a Narkowicz **ACES** tonemap. Material IDs are preserved:

- `0` generic lit object
- `1` floor (adds a soft, non-"debug" world-space grid)
- `2` hand / grab highlight (stronger rim)
- `3` unlit emissive marker (teleport reticle)

`scene.vert` computes the normal matrix as `transpose(inverse(mat3(model)))` so
non-uniformly scaled primitives light correctly.

## Textures (base colour)

Every Tripo asset keeps its base-colour texture from the source GLB to the
headset:

- **Decimation** (`ptv_glb_optimize`) keeps per-material submeshes, welds
  vertices per material, weights UVs in the simplifier and protects seams, so
  no LOD smears a texture across a seam. The textures are written once per
  content hash to `assets/ptv_textures/` (no leading underscore: Android's aapt
  drops asset folders named `_*`).
- **Compression** (`ptv_texture_compress`, vcpkg port `ktx`): KTX2 with ASTC
  6x6 (4x4 for UI; 4x4 with the `high` texture override), sRGB, full mip chain
  built in linear space, sized by `texture_policy.py` from how close the player
  can get (a held pistol, a crate by the path, a skyline nobody reaches), never
  above the source and inside `texture_memory_budget_mb`. The GLB's material
  points at the KTX2 through the `PTV_texture_astc` extension and keeps the PNG /
  JPEG as a fallback (desktop).
- **Runtime** (`TextureCache`): KTX2 levels are uploaded as is (memory-mapped
  from the APK on Quest), PNG/JPEG decode through stb with GPU-blitted mips.
  Textures are shared by content hash and reference counted; images of 4 MB or
  more get dedicated allocations; uploads go through one 32 MB staging buffer.
- **Shading:** `scene.frag` samples base colour x `baseColorFactor` x `COLOR_0`
  x the draw's tint; `alphaMode` MASK is honoured (BLEND draws as MASK 0.5 with a
  warning).
- **Diagnostics:** a texture that should exist but failed draws as a magenta
  checker in verification and debug builds; the 1 Hz `tex:` line reports
  resident / ASTC / decoded / missing counts. Verification adds a texture
  gallery pass that screenshots every textured asset on the headset (see
  `docs/quest-build.md`).

## Skinned geometry (`scene_skinned.vert`)

Rigged meshes go through a **second graphics pipeline**. It shares the render
pass, the pipeline layout, the push-constant block and `scene.frag` with the
static one; only the vertex shader and the vertex input differ, so switching
between them costs a single `vkCmdBindPipeline`.

| | Static | Skinned |
|---|---|---|
| GPU vertex | `GpuVertex`, 24 B | `GpuSkinnedVertex`, 32 B |
| CPU load struct | `LitVertex`, 48 B | `SkinnedVertex`, 56 B |
| Attributes | pos, octahedral normal, uv, color | + `JOINTS_0` (`R8G8B8A8_UINT`), `WEIGHTS_0` (`R8G8B8A8_UNORM`) |
| Vertex shader | `scene.vert` | `scene_skinned.vert` |

Static meshes deliberately keep the narrower vertex. Quest 3 is
memory-bandwidth bound, and widening every vertex in the scene by 8 bytes to
serve the handful that are rigged is the wrong trade.

Skinning runs **in the vertex shader**, not a compute prepass: nothing else in
the engine consumes the deformed positions (no shadow pass, no ray tracing, no
GPU collision), so writing them out to memory would cost bandwidth and buy
nothing.

**Joint palettes.** Every skinned draw in a frame shares one storage buffer at
`set 0, binding 1` — host-visible, persistently mapped, grown in powers of two,
and filled once per frame *before* the per-eye loop (palettes are world-space,
so both eyes read the same data). A draw records only where its slice starts,
carried in its instance record's `params.y` (`instances.inst[gl_InstanceIndex]`,
`set 0, binding 2`), so instanced clones of one character each carry their own
pose and the 48-byte push-constant block does not grow. The alternative — a descriptor set per skinned mesh — would put every character on
its own binding change, which lands directly on the draw-call budget below.

Normals use `mat3(model * skin)` rather than a full inverse-transpose, the
standard assumption that joint matrices carry no non-uniform scale.

Joints are limited to **256 per skin** by the byte-packed indices; a skin that
exceeds it fails the load loudly rather than wrapping onto the wrong bone.
Typical humanoid skeletons land near 65 joints and an OpenXR hand is 26.

Culling uses the **bind-pose** bounding sphere, so a heavily posed character is
culled slightly optimistically — much cheaper than re-deriving bounds per frame
from the palette.

## Gradient sky (`sky.vert` / `sky.frag`)

A fullscreen triangle (no vertex input, depth off) reconstructs a world-space
view ray from the per-eye `inverse(projection * view)` (a small mat4 push
constant) and renders a top→horizon gradient with a soft sun disk and glow, then
ACES-tonemaps. It is drawn AFTER the opaque geometry, at the far plane with a
`LESS_OR_EQUAL` depth test, so it only shades pixels nothing covered (drawing it
first shaded the whole screen and then overdrew most of it); geometry fog reuses
the same horizon color so the world melts into the sky at distance.

## Primitive shapes

`spawn_primitive` kinds now render as real meshes instead of always being cubes.
Unit meshes (sphere, cylinder, plane, ramp) are generated once at init at unit
extent — identical to the cube — so `build_items_from_world` keeps scaling by the
primitive's `size` and driving color through the per-item tint. `box`/unknown
still uses the cube. These match the collision shapes built in `PhysicsWorld`, so
what you see is what you can stand on.

## Gameplay tags are data, not renderer code

Which tags the renderer reacts to lives in one table,
`engine/runtime/src/render/VisualTagTable.h/.cpp`. `build_items_from_world`
resolves an entity's tags **once** per frame into a `TagVisuals`, and the mesh,
primitive and placeholder paths all read that.

These decisions used to be string comparisons scattered through four places in
the draw loop — a `template` check near the top, an `enemy`/`grabbable` tint
further down, a `ground` test in the primitive path, and another
`ground`/`grabbable`/`trigger` test in the placeholder path. Adding one
affordance meant finding all four, and one WAS missed: a declared door had a row
in the scene document and a mechanic to drive it, but no row here, so it drew as
an orange grab-cube. Adding an affordance is now a row, and
`ptv_smoke_visual_tags` asserts every affordance kind has one.

Each row carries roles, and a tag may have several:

| Role | Effect |
| --- | --- |
| `kAlwaysHidden` | Never drawn (`template`: construct prefabs parked at a staging coordinate for runtime spawning; `collider_only` / `player_part`: physics proxies such as the player's finger, palm, forearm and shin colliders). |
| `kHiddenWhenMeshless` | Drawn only with a real mesh (`player_system`, `tutorial`, `checkpoint`, `spawn_point`). Marker entities have a transform and nothing to look at; without this they fell through to the procedural cube path and littered the world with debug cubes on top of the real environment. |
| `kGround` | Oversized with the grid material, so the play-area edge never shows in the headset. |
| `kMeshTint` | A resting tint on a real mesh. |
| `kPlaceholder` | A distinct size and colour when the entity has no mesh yet. |

Row order is priority: `ground` sits above the affordances so a floor also
tagged `grabbable` still draws as a floor.

**Meshless entities draw only on request.** An entity with no mesh,
primitive or inline mesh is drawn as a placeholder shape only when one of
its tags has the `kPlaceholder` role (`ground`, `door`, `grabbable`,
`trigger`, ... or the explicit `placeholder` row used by the starter room's
walls); `render_tags::draws_meshless` is the rule. Anything else -- a logic
host, a HUD anchor, a weapon hub, a collider proxy -- is not drawn, counted
in `FrameStats::meshless_hidden` and named once in the log
(`render: '<id>' has nothing to draw ...`). The old default drew every one
as a 0.3 m grey cube, including one per finger collider over the player's
hands. scene_compile also tags such entities `mechanic_host`.

## Affordance cues (`CHighlight`)

"You can act on this, right now" is published by gameplay and only *drawn* by
the renderer. The affordance built-ins write `CHighlight { intensity, color }`
every frame; `build_items_from_world` blends it into whatever tint the entity
already has via `apply_highlight` — a multiply, so a cue reads as "lit up"
against the object's own look rather than flattening every highlighted object to
one colour.

The renderer does not decide whether something is actionable. That depends on
reach, on whether the object is locked, and on per-scene interaction policy, all
of which the affordance built-ins already evaluate in order to decide whether to
respond. Recomputing it here would be a second source of truth, and the two
would drift: an object would glow while its mechanic refused to act on it.
Advertising an interaction that will be refused is worse than showing no cue at
all, so the mechanic that will refuse is the one that owns the cue.

`publish_cue` in `AffordanceCommon.h` fades over ~120 ms each way rather than
switching, because a hand drifting along the edge of reach would otherwise
flicker the cue every frame — which in a headset reads as a rendering fault, not
as feedback. The component is only added once something is actually near, so an
untouched world pays nothing per entity.

## Performance budget

Targeted at a **smooth 72–90 Hz**, on Quest 3 standalone as well as over Quest
Link. There is no shadow pass, no bloom, and no fullscreen post; the costs are
MSAA, one extra fullscreen sky pass, and a slightly heavier fragment shader.

Meta's published guidance for Quest 3 is **1.3M–1.8M triangles per frame** and
**200–300 draw calls** for a busy simulation. With multiview one draw covers
both eyes; without it (desktop, or a Quest scene `scene_complexity` left at
or below 120 estimated p95 draws) each eye records its own pass, so draw
calls are about `2 x` the drawn (instanced / batched) items.

"Busy simulation" is Meta's own term for an app with many animated NPCs, and
they call out animation and skinning specifically as work that delays the
render thread. So animated characters carry a **separate budget** from
geometry — see [Animated agents](#animated-agents) below. A scene can sit
comfortably inside its triangle and draw-call numbers and still miss frame
rate on skeletons alone, and without counting them that looks like a
rendering problem.

### Budget tiers (no content caps)

The pipeline never limits how many assets a prompt may ask for. Instead the
`asset_optimization` stage derives a tier from the target device and the amount
of GLB content, and everything downstream is sized from it. A compact game
keeps full detail and unlimited view distance; a massive one decimates harder
and culls sooner. Policy lives in one place:
`packages/schemas/ptv_schemas/performance.py`.

- `compact` (<= 12 assets, <= 40 mesh entities): 60k tri/asset ceiling,
  distance culling effectively OFF, MSAA 4x, foveation off.
- `standard` (<= 24 assets, <= 100 entities): 35k ceiling, culling off,
  MSAA 4x, foveation low.
- `large` (<= 250 mesh entities, any asset count): 18k ceiling, culling on at 120 m,
  MSAA 2x, foveation medium.
- `massive` (> 250 mesh entities): 10k ceiling, culling at 80 m, foveation
  high, LRU mesh eviction on.
- `pcvr` multiplies the geometry budgets ~5x and disables the mobile
  compromises.

Per-asset budgets are weighted by importance (`hero` / `standard` /
`background`, derived from category, scale and interactivity) with a hard
floor so small props never become blobs, and are never raised above the source
mesh — decimation only.

### Animated agents

Skinning and behaviour evaluation are CPU work competing with the render
thread, so characters are budgeted separately from triangles. Three fields on
`PerformanceProfile` carry the policy into `scene.json`:

| Field | Standard Quest tier | Meaning |
|---|---|---|
| `max_animated_agents` | 12 | Ceiling on simultaneously animated characters |
| `animation_lod_distances_m` | `[8, 15, 40]` | Full rate / half rate / stop posing |
| `ai_tick_rates_hz` | `[30, 10, 3]` | Behaviour update rate in those same bands |

Larger tiers allow fewer characters; PC VR roughly triples the count and
doubles the distances, because a desktop has CPU headroom a headset does not.

Enforcement happens where it matters rather than as a guess at compile time:
`ctx.spawn` refuses to clone a skinned entity once the cap is reached, so the
worst case is "enemies stop appearing" instead of a stuttering headset. The
scene compiler additionally warns when a scene *places* more animated
characters than the budget allows, and says to spawn the extras from a hidden
`template` entity instead.

Animation LOD banks the time it skips and repays it on the next sample, so a
distant character walks at the correct speed — only the sampling gets coarser,
which is invisible at that range. The per-second `perf:` telemetry line
reports `rigs N (M animating, budget B)` next to the draw-call and triangle
counts.

## Level of detail

Each optimized asset ships an LOD chain (`_lod0` .. `_lod3`, roughly 100% /
40% / 15% / 5% of the LOD0 budget) and the simplification error of every level
(`mesh.lod_errors_rel`, meshoptimizer's error relative to the mesh extent). The
renderer picks the **coarsest level whose error projects to fewer than
`lod_screen_error_px` pixels** (1 px by default) at the object's distance, so
detail follows what the eye can actually resolve rather than fixed distances:

- **hysteresis** — at most one level coarser per frame, and back to a finer
  level only once the current one is `lod_hysteresis` (20%) over the limit;
- a global **`lod_bias`** the adaptive governor raises under load scales the
  pixel limit (`limit = lod_screen_error_px x (1 + lod_bias)`);
- each level's error is taken as the running maximum over the finer levels,
  so a coarser level never counts as more accurate than a finer one;
- **speed-aware prefetch** — the level the player will need 1.5 s ahead (from
  the smoothed camera motion) is requested early; only when the camera moves
  faster than 0.5 m/s, never for rigged meshes, and off under any memory
  pressure;
- first-person entities (held / worn) always use LOD0, and at `critical`
  memory pressure every other visible mesh drops to its coarsest level;
- assets compiled before LOD errors existed fall back to the scene's
  `lod_switch_distances_m`.

**Rigged assets keep an identical skeleton at every level.** `ptv_glb_optimize`
copies the joint hierarchy, bind pose, inverse bind matrices **and animation
clips** verbatim into each LOD, so one palette — built once per frame from the
entity's `CSkeleton` — is valid for whichever level the distance check picks.
Without that invariant a LOD switch mid-animation would snap the character into
a garbage pose.

Clips ride through for free: a glTF animation targets NODES, and the node tree
is copied unchanged, so decimating the mesh cannot invalidate a keyframe. The
writer does have to actually emit them, though — it did not at first, and the
result was that every clip Tripo produced was silently discarded before it
reached the headset, because the runtime loads LOD0.

Two consequences worth knowing:

- `--sloppy` is **refused** for rigged input. `meshopt_simplifySloppy`
  synthesizes vertex positions that belong to no source vertex, so there is
  nothing to carry `JOINTS_0`/`WEIGHTS_0` and the mesh detaches from its rig.
  Rigged LODs use `meshopt_simplifyWithAttributes` with
  `meshopt_SimplifyRegularize` instead, fed normals and colors but never bone
  data. (Bone indices are categorical and weights are a partition of unity;
  neither behaves like a value the quadric metric can interpolate.)
- Rigged meshes bypass the streaming worker entirely — see below.

## Culling

Built each frame in `Renderer::build_items_from_world`:

- **Frustum culling** against both eye frusta (12 planes, OR'd), from the
  views located at `xrWaitFrame` time — the same poses the frame renders with.
  The padded 140° single frustum remains only as a fallback when no views are
  available.
- **Distance culling** beyond `cull_distance_m`. Effectively infinite for
  `compact`/`standard`, so a small game still renders its whole world; the
  governor may shorten it under CPU load.
- **Small-object culling** (rendering only): objects whose bounding sphere
  projects to fewer than `small_object_cull_px` (1.5 px) are skipped. Never
  characters, highlighted (interactive) objects or the player's body.
- **Room culling**: `scene_compile` marks nav regions that walls close on all
  four sides (each side at least half covered, leaving room for doorways).
  Static geometry inside such a room draws only while the player stands in a
  region within two links of it (doorways can line up through a room). Off for
  open worlds and single rooms, and whenever the player is outside every region.

All use a bounding sphere: the pipeline-computed `bounds_radius_m` when
available, otherwise a conservative estimate.

## Static batching and instancing

Identical (mesh, material) draws collapse into one instanced draw. On top of
that, entities `scene_compile` marks `static_batch` — world geometry nothing
moves, holds, binds or reads per frame — are merged at scene load into one mesh
per grid cell (`static_batch_cell_m`, 8 or 16 m), room and material, with world
positions and colours baked in. Colliders stay per entity, so physics, AI and
navigation are unchanged. A member that ever moves dissolves its batch (logged
once) and its entities draw individually again. Measured: where the merged
geometry is primitives that instancing had already collapsed, batching splits
them into per-cell draws (Glock Laser Deck 11 → 34 draw calls, inside budget,
no GPU / CPU change); it pays off for many distinct inline meshes.

## Draw-time transform offsets

`build_items_from_world` resolves each entity's model matrix through
`entity_visual_matrix`, which composes `CTransform` with any active
`CTransformOffset` — the engine's visual "juice" layer for recoil kicks, hit
flinches and impact shoves. The offset is applied to all four DrawItem paths
(GLB mesh, procedural primitive, inline mesh, cube fallback).

This lives at draw time on purpose. `CTransform` is an output of the physics
step for anything with a rigid body, so a mechanic cannot write a kick there
without it being overwritten or desyncing the drawn pose from the physical
one. Because the offset is only ever composed into the model matrix, physics,
collision and raycasts never see it, and there is no ownership conflict with
`builtin.grab`'s kinematic follow.

The cost is one hash lookup per entity per frame and, only for entities with
an *active* offset, one extra matrix multiply. Settled offsets clear their
`active` flag and are skipped entirely. Mechanics drive this through
`ctx.fx->impulse(...)`, never by touching the component.

## Asset streaming

GLBs are **never** loaded inside the draw loop. A worker thread does the
expensive part (asset read + glTF parse + CPU vertex build) and the main thread
performs at most a couple of GPU uploads per frame (~4 ms cap), mirroring
Meta's own open-world sample which throttles to ~4 async operations per frame.

This is what fixed the `isn't responding` ANR: previously the first frame
loaded all 13 assets synchronously (~5.5 s), which starved the
`native_app_glue` event pump and blocked the Android UI thread past its ~5 s
watchdog. Meshes now pop in over a frame or two, and an entity draws a coarser
already-resident LOD while its preferred one streams in.

**Residency** is on for every tier and acts only when it must: when resident
meshes + textures exceed `resident_memory_budget_mb` (set by the memory model,
see `docs/quest-optimization.md`), when a `massive` scene exceeds its triangle
budget, or when `MemoryGuard` raises its level. Every asset's **lowest LOD is
queued at scene load and never evicted** (the always-resident floor), and LODs
share their textures by content hash, so a late load shows a coarser version —
never nothing and never a white surface. Victims are the least recently drawn
streamable meshes; they are retired and freed once no frame in flight can draw
them. Only GPU buffers are evicted: entities, colliders, AI and game state are
never touched.

**Rigged meshes are the exception, and are uploaded up front.** The streaming
worker only knows how to build static vertices; letting it touch a
rigged path would cache a static slot under that path and the entity would draw
through the wrong pipeline forever. `Application::attach_skeletons()` therefore
loads every rigged asset (and its whole LOD chain) synchronously at scene load
and marks the slots non-evictable, since a skeleton outliving its geometry would
leave the entity permanently invisible with no way to recover. Whether an asset
is rigged comes from `mesh.skinned` in `scene.json`, so the common all-static
scene never opens a GLB to find out. The player's two hand entities take the
same synchronous path, for the same reason.

## Fixed foveated rendering (FFR)

Fixed, not eye-tracked (the Quest 3 has no eye tracking): the full-detail
region stays at the centre of each eye's display and only its strength changes.
The engine enables `XR_FB_foveation`, `XR_FB_foveation_configuration`,
`XR_FB_foveation_vulkan`, `XR_FB_swapchain_update_state` and
`VK_EXT_fragment_density_map`, creates the multiview swapchain with
`XR_SWAPCHAIN_CREATE_FOVEATION_FRAGMENT_DENSITY_MAP_BIT_FB`, fetches each
image's density map through `XrSwapchainImageFoveationVulkanFB` and attaches
it to the render pass. Without the density map a Vulkan app's foveation level
does nothing — the engine's earlier path set levels with no measurable effect.

The level (none / low / medium / high) is set by the engine's governor through
`xrUpdateSwapchainFB`; the runtime's own dynamic flag is off, so two
controllers never fight. Measured on Quest 3 (Glock Laser Deck, GPU time):
level 0 10.2 ms, 1 5.95 ms, 2 5.4 ms, 3 4.6 ms. The density map costs about
1 ms at level 0, so automatic FFR starts at level 1 (2 when the scene estimate
is heavy); a user's "FFR off" drops the density map entirely.

## Dynamic resolution and refresh rate

The swapchain is allocated at `render_scale_ceiling` x the recommended eye
resolution (1.0 by default, up to 1.25 with the `high` override, within
`maxImageRect`). The governor may submit a centred sub-rectangle down to
`render_scale_floor` (0.85 by default; the governor clamps it to 0.5-1.0,
and resolution `off` / `high` lock the scale) through `imageRect`; centred so
the density map's centre stays on the lens centre. `XR_FB_display_refresh_rate`
enumerates the supported rates and requests the closest one not above
`refresh_hz` (72 by default; 90 / 120 on request), logging the result
(`XR: display refresh -> N Hz`). The governor's frame budget is
`1000 / refresh_hz` from the scene (falling back to `target_fps`, then 72),
not the rate the runtime actually granted.

## Adaptive quality governor

Build-time budgets are only an estimate; `QualityGovernor` closes the loop on
the real device. It classifies each window as GPU- or CPU-bound from the
measured GPU timestamp and CPU time, and climbs the matching ladder, least
visible first:

- **GPU-bound:** FFR +1 up to medium → render scale −0.05 down to the floor →
  FFR up to the maximum → `lod_bias` +0.5 → cull distance x0.8.
- **CPU-bound:** cull distance x0.8 → `lod_bias` +0.5.

A frame is bad when its wall time or its worse of GPU / CPU time exceeds 110%
of the budget, good when both are under 75%. It escalates after
`escalate_after_bad_frames` (30) consecutive bad frames, trying the other
ladder if the bottleneck's ladder is exhausted, and relaxes one step after
`relax_after_good_frames` (180) good ones. After every step it waits out a
cooldown (`max(20, refresh / 2)` frames, about half a second) and probes the
next window: a step that bought less than 3% of the bottleneck's time is
marked ineffective for the scene and skipped from then on. Relaxation undoes
the recorded steps in reverse order. Bounds come from `performance.governor`
(`max_lod_bias` 2, `min_cull_distance_m` at least 25 m, `max_foveation_level`
3 on Quest / 0 on PC VR). A knob the user locked ("Custom Quest
Optimizations") is never moved; when no unlocked step remains it logs
`governor: over budget, blocked by user settings (...)` (or `... with every
quality step already taken` when nothing is locked), which verification
reports as a warning, not a failure. Each step logs `governor: GPU-bound ->
<knob> (step N: ffr F scale S lod_bias B cull Cm)`. Memory is not the governor's job — see
`MemoryGuard` in `docs/quest-optimization.md`.

## On-device telemetry

About once a second (only while the XR runtime asks for frames) the runtime
logs four lines, plus a hands line:

- `perf:` FPS, frame work ms, CPU ms, GPU ms (timestamps; the start stamp is
  written at bottom-of-pipe so a queued second frame does not inflate it), draw
  calls / pipeline / descriptor binds, drawn vs considered entities with
  frustum / distance / small / room culled counts and merged batches, visible
  triangles, residency, rigs, and the current `lod_bias`, cull distance, FFR
  level and render scale:
  `perf: F fps | work A ms avg, W ms worst | cpu C ms | gpu G ms avg, X ms worst
  | calls N pipes P sets S | draw D of E (cull frustum a dist b small c room d,
  batches e) | tris T | meshes M resident (R tris), Q streaming | rigs K
  (J animating, budget B) | lod_bias L cull Um ffr V scale Z`;
- `render_body:` `placeholders=` (non-floor placeholder boxes drawn),
  `meshless_hidden=`, `player_draws=` (draws of anything but the player's
  hands, avatar and held items with its origin within 35 cm of a tracked hand
  or an avatar foot) and the first `names=`, maxima over the second;
- `perf_fx:` live projectiles vs cap, FX quads drawn vs cap, casts last tick;
- `tex:` resident textures and MB, KTX2 vs decoded, missing (and missing
  draws), ASTC support, mesh and target MB, instances;
- `mem:` total (smaps PSS + unmapped GPU memory) against target / ceiling /
  limit, the projection MemoryGuard acts on, the smaps and KGSL parts,
  per-category engine accounting, queued loads, system `MemAvailable`, level
  and evictions:
  `mem: pss P MB (target T, ceiling C, limit L) | projected J | smaps S |
  gpu_unmapped G | tex X | mesh M | targets R | reserved Q | sys_avail A |
  level green|yellow|red|critical | evict N` (`gpu_unmapped` is -1 when KGSL
  is unreadable).

`debug.ptv.perf_override` (an engineering property, never set by the pipeline)
switches multiview, the density map, frames in flight, a fixed FFR level, the
governor or batching for on-device A/B measurements, e.g.
`adb shell setprop debug.ptv.perf_override "mv=0,fdm=0,fif=1"`.

Watch it with:

```
adb logcat -s ptv
```
