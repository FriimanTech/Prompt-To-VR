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

## Frame flow (per eye)

```
begin render pass (MSAA color + depth, cleared to the horizon color)
  → sky pass      : fullscreen triangle, gradient + sun, depth test/write off
  → geometry pass : UBO-lit, per-primitive mesh, fog + contact-AO + tonemap
end render pass  → MSAA resolve into the swapchain image
```

Both eyes are recorded sequentially into one command buffer and submitted with a
single fence wait per frame (the existing single-frame-in-flight model).

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
| `camera_pos` (`.w`) | Camera world position (renderer-computed) and point-light count. |
| `points[8]` | Up to 8 point lights (position + range, color + intensity). |

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
non-uniformly scaled primitives light correctly — done in-shader to avoid growing
the push-constant block (kept at 160 bytes).

## Skinned geometry (`scene_skinned.vert`)

Rigged meshes go through a **second graphics pipeline**. It shares the render
pass, the pipeline layout, the push-constant block and `scene.frag` with the
static one; only the vertex shader and the vertex input differ, so switching
between them costs a single `vkCmdBindPipeline`.

| | Static | Skinned |
|---|---|---|
| Vertex struct | `LitVertex`, 40 B | `SkinnedVertex`, 48 B |
| Attributes | pos, normal, color | + `JOINTS_0` (`R8G8B8A8_UINT`), `WEIGHTS_0` (`R8G8B8A8_UNORM`) |
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
carried in `pc.params.y`, an already-unused push-constant component: the block
is at 160 bytes against Vulkan's guaranteed 128, so it must not grow. The
alternative — a descriptor set per skinned mesh — would put every character on
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
ACES-tonemaps. It is drawn first each eye so it fills the background; geometry
fog reuses the same horizon color so the world melts into the sky at distance.

## Primitive shapes

`spawn_primitive` kinds now render as real meshes instead of always being cubes.
Unit meshes (sphere, cylinder, plane, ramp) are generated once at init at unit
extent — identical to the cube — so `build_items_from_world` keeps scaling by the
primitive's `size` and driving color through the per-item tint. `box`/unknown
still uses the cube. These match the collision shapes built in `PhysicsWorld`, so
what you see is what you can stand on.

## Performance budget

Targeted at a **smooth 72–90 Hz**, on Quest 3 standalone as well as over Quest
Link. There is no shadow pass, no bloom, and no fullscreen post; the costs are
MSAA, one extra fullscreen sky pass, and a slightly heavier fragment shader.

Meta's published guidance for Quest 3 is **1.3M–1.8M triangles per frame** and
**200–300 draw calls** for a busy simulation. Since the renderer is still
two-pass stereo, draw calls are `2 x visible entities`, so the visible entity
count is the practical ceiling.

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
- `large` (<= 48 assets, <= 250 entities): 18k ceiling, culling on at 120 m,
  MSAA 2x, foveation medium.
- `massive` (> 250 mesh entities): 10k ceiling, culling at 80 m, foveation
  high, LRU mesh eviction on.
- `pcvr` multiplies the geometry budgets ~5x and disables the mobile
  compromises.

Per-asset budgets are weighted by importance (`hero` / `standard` /
`background`, derived from category, scale and interactivity) with a hard
floor so small props never become blobs, and are never raised above the source
mesh — decimation only.

## Level of detail

Each optimized asset ships an LOD chain (`_lod0` .. `_lod3`, roughly 100% /
40% / 15% / 5% of the LOD0 budget). The renderer selects a level from camera
distance against the scene's `lod_switch_distances_m`, with:

- **hysteresis** — at most one level change per frame and a 15% margin before
  moving back up, so an object sitting on a switch boundary cannot flicker;
- a global **`lod_bias`** the adaptive governor raises under load.

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

- **Frustum culling** against a single conservative frustum that covers both
  eyes (HMD centre pose, FOV padded to 140°). Culling once is cheaper than per
  eye and, being conservative, can only ever draw a few extra edge objects —
  never wrongly remove a visible one.
- **Distance culling** beyond `cull_distance_m`. Deliberately set effectively
  infinite for `compact`/`standard`, so a small game still renders its whole
  world. This is a budget, not a global rule.

Both use a bounding sphere: the pipeline-computed `bounds_radius_m` when
available, otherwise a conservative estimate.

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

When a scene exceeds the resident triangle budget (only enabled in `massive`),
least-recently-used meshes are evicted one per frame. This is safe because
`draw_frame` waits on its fence before returning, so no command buffer is still
referencing the freed buffers.

**Rigged meshes are the exception, and are uploaded up front.** The streaming
worker only knows how to build 40-byte static vertices; letting it touch a
rigged path would cache a static slot under that path and the entity would draw
through the wrong pipeline forever. `Application::attach_skeletons()` therefore
loads every rigged asset (and its whole LOD chain) synchronously at scene load
and marks the slots non-evictable, since a skeleton outliving its geometry would
leave the entity permanently invisible with no way to recover. Whether an asset
is rigged comes from `mesh.skinned` in `scene.json`, so the common all-static
scene never opens a GLB to find out. The player's two hand entities take the
same synchronous path, for the same reason.

## Fixed foveated rendering

When the runtime exposes `XR_FB_foveation` + `XR_FB_foveation_configuration` +
`XR_FB_swapchain_update_state` (Quest does; PC VR generally does not), the
swapchains are created foveation-capable and the level is applied through
`xrUpdateSwapchainFB`. The periphery renders at lower resolution — one of the
cheapest large GPU savings on mobile. The level starts from the scene tier and
can change per frame.

## Adaptive quality governor

Build-time budgets are only an estimate; `QualityGovernor` closes the loop on
the real device. It watches measured frame time and, with hysteresis to avoid
oscillation, walks an escalation ladder (cheapest perceptual cost first):

1. raise `lod_bias`, 2. shrink the cull distance, 3. raise the foveation level

...then relaxes back in reverse once there is headroom. Bounds come from the
scene's tier, so the governor can never destroy image quality outright. This is
what lets an arbitrarily large prompted game self-tune instead of the pipeline
refusing content.

## On-device telemetry

About once a second the runtime logs FPS, average/worst frame ms, drawn vs
considered entities, frustum/distance culled counts, visible triangles,
resident meshes and triangles, pending streams, and the current
`lod_bias`/cull distance/foveation level. Watch it with:

```
adb logcat -s ptv
```
