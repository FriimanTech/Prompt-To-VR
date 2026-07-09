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

Targeted at a **smooth 72–90 Hz over Quest Link on a normal/mid-range gaming
PC**. The costs are MSAA 4x, one extra fullscreen sky pass, and a slightly
heavier fragment shader; there is no shadow pass, no bloom, and no fullscreen
post. If a device reports no 4x support the renderer steps down automatically.
