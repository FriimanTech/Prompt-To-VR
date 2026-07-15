
---

### 3. Part 3 — 3D objects must be brought into the VR game and used per their prompt-defined purpose

1. After the Tripo image-to-3D stage produces `.glb` files, every generated asset must end up *inside the running VR game* in a role that matches the purpose described in the original prompt. No Tripo-produced model should be silently dropped on the way from `AssetManifest` → `SceneSpec` → `scene.json` → engine.

2. Concrete role mapping (driven by `AssetItem.category` plus tags / `style_notes`):

   1. **`environment`** → the *world the player spawns in*.
      Example: a generated city glb becomes the static world environment placed at the origin with mesh collision, large `target_scale_meters`, and tags such as `["environment", "static"]`. The `PlayerSpawn.transform` must sit on/inside this environment.
   2. **`character`** with `style_notes` indicating an NPC → an NPC entity placed where the prompt describes (e.g., a shopkeeper near a counter) with a capsule physics shape and `tags=["npc"]`.
   3. **`character`** that the prompt names as the *player body* (e.g., "Superman", "Iron Man") → attached to the player rig with `tags=["player_avatar"]`. First-person culling rules apply.
   4. **`character`** with `style_notes` indicating a hostile / enemy → dynamic capsule with `tags=["enemy", "target"]`, placed at engagement distance from the spawn.
   5. **`prop`** that the prompt describes as grabbable / wearable / usable → dynamic AABB (or sphere) with `tags=["interactable", "grabbable"]` (or `"wearable"`) placed within arm's reach of the spawn.
   6. **`prop`** that is environmental dressing → static AABB with `tags=["static"]` placed where the prompt describes.
   7. **`fx`** → no physics; tags `["vfx"]`; bound to a relevant entity if the prompt describes a target.
   8. **`ui`** → no physics; world-anchored or rig-anchored HUD panel with `tags=["hud"]`.

3. The pipeline is already responsible for this, but it must be enforced in two layers so no asset is ever lost:

   1. **LLM layer (planner enforcement)** — `SCENE_SPEC_SYSTEM` must require: *every* `AssetItem` from the `AssetManifest` is instantiated as at least one entity in `SceneSpec.entities` with a category-appropriate `mesh`, `physics`, and `tags` block, **and** the prompt's stated purpose for that asset is honoured (e.g., "the city is the world the player spawns in" must produce a `world environment` entity at the origin with the player spawn placed inside it). `ASSET_MANIFEST_SYSTEM` must in turn require that each `AssetItem.category` is chosen so the downstream scene composer can place it correctly.
   2. **Deterministic safety net (engine-side)** — `compile_scene_json` must check the final list of entities against the `AssetManifest`. If any asset is *not* referenced by an entity's `mesh.asset_id`, the compiler must auto-instantiate a category-appropriate default entity for it (environment at origin, character at engagement distance, prop within reach, etc.) so the GLB still appears in the running game. This makes the system robust to LLM forgetfulness.

4. Verification:

   1. The starter room's runtime (`engine/runtime/src/render/Renderer.cpp` via `GltfMeshLoad`) already renders any entity that has a `CMesh` component pointing at a project-relative `glb_path`, so once the entity is in `scene.json` it shows up in the headset. No engine-side renderer change is required for this — the work lives in `stage_asset_manifest.py`, `stage_scene_spec.py`, `stage_scene_compile.py`, and `prompts.py`.
   2. Add a regression test that builds a synthetic `AssetManifest` with one asset per category, hands the scene compiler a `SceneSpec` that references *none* of them, and asserts that the compiled `scene.json` contains a category-appropriate entity for each.