15. I want you to use Subagents that can communicate with each other to work on each Part 1–2 in parallel without conflicts. That being said, this is what I need them to do: review each Part below, and after analyzing what I want, complete the following:
16. Think about what I want and determine the best way to achieve it. You may create a new method or use an existing one.
17. Search the web for the latest documentation, examples, and best practices to understand how to build this with the highest quality.
18. Implement the code required to create the additions I want.
19. Validate all additions to ensure they function correctly. Review the final result to confirm that all required code has been properly implemented and meets the objective. This applies across all Parts below.

---

### 1. Part 1

1. I want you to fix both walking locomotion options that were previously added. Specifically, correct the naming so that “head-based” and “controller-based” locomotion are properly defined as **Head-Relative** and **Controller-Relative**. Additionally, what I referred to as walking locomotion is called **sliding locomotion** by Meta. I want you to review Meta’s sliding locomotion documentation and correctly implement both locomotion types so they behave as follows:

   1. **Head-Relative Locomotion**
      When the joystick on the left controller is moved (forward, backward, diagonal, etc.), movement direction is determined by where the user’s head (camera) is facing.

      1. For example, if the user is looking at a wall and pushes the joystick forward, they will move directly toward that wall. If the user turns their head while moving, the movement direction should dynamically update to match the new view direction.

   2. **Controller-Relative Locomotion**
      When the joystick on the left controller is moved, movement direction is determined by the direction the controller is pointing, independent of the user’s head orientation.

      1. For example, if the user is looking straight ahead at a wall but pushes the joystick to the right, they will move to the right—even if their head remains facing forward.

   3. Resources (Use the documentation provided below and also find additional relevant sources to achieve the goal)

      1. (Meta Overview of Locomotion Types) - [https://developers.meta.com/horizon/design/locomotion-types/](https://developers.meta.com/horizon/design/locomotion-types/)
      2. (Meta Sliding Locomotion Documentation) - [https://developers.meta.com/horizon/design/locomotion-types/#artificial-locomotion-types](https://developers.meta.com/horizon/design/locomotion-types/#artificial-locomotion-types)
      3. (Meta Locomotion Input Mapping) - [https://developers.meta.com/horizon/design/locomotion-input-maps/](https://developers.meta.com/horizon/design/locomotion-input-maps/)

---

### 2. Part 2

1. I want you to refine the VR engine and Backend AI system to ensure they coordinate correctly and are capable of fully building a VR game. Review the details below for how the VR engine and Backend AI Agent system should operate. Also evaluate how prompt-to-VR game generation should function on top of this engine.

   1. Full walkthrough of VR Engine / Backend AI Agent system

      1. The VR engine should provide a foundational system that all prompt-to-VR games can leverage so they do not need to start from scratch. However, not all generated VR games must be fully dependent on the engine. Instead, each generated game should:

         * Use any relevant prebuilt functions from the engine
         * Modify those functions when necessary using AI
         * Generate new functionality when required by the prompt

         Below is an example scenario demonstrating how a prompt should be processed and executed:

         1. Example prompt input:
            “I want you to create a game called Superman Flight Sim, where users spawn in a city and can walk around. At any time, they can press a button to transition from standing on the ground to floating in the air. The same joystick used for walking should also allow them to fly forward, backward, left, and right. Additionally, when the user is hovering and looks down, they should see Superman’s legs positioned similarly to how he appears in films, with one knee slightly higher than the other.”

         2. The application should analyze the prompt, generate all required assets, and perform the following steps:

            1. Use Gemini (or a similar model) to generate images that will later be converted into 3D assets. Specifically:

               * 1a. A detailed city environment image
               * 1b. A detailed Superman character image
               * 1c. Any additional required assets based on the prompt

            2. Use Tripo (or a similar system) to convert these images into high-quality 3D models in `.glb` format

            3. Construct the VR game using backend AI agents:

               1. Import all generated 3D assets

               2. Set the city as the main environment

               3. Configure first-person embodiment so the player exists inside the Superman character model

               4. Add sliding locomotion. Since the prompt does not specify locomotion type, implement both Head-Relative and Controller-Relative movement. Add a settings menu that allows the user to select their preferred locomotion type before moving.

                  * The AI agent must check whether these locomotion systems already exist in the VR engine
                  * If they exist, reuse and apply them
                  * If they do not exist, create them according to Part 1

               5. If required mechanics are missing (e.g., flight or hover):

                  1. Research proper implementations using Meta and other VR documentation
                  2. Create a flight locomotion system and add it to the VR engine
                  3. Apply the system to the current game
                  4. Repeat the process for hover functionality
                  5. Store these systems in the engine for future reuse and iteration

               6. After core mechanics are implemented, generate additional code to:

                  1. Add collision detection so users cannot pass through the environment
                  2. Ensure full-body embodiment (arms and legs visible and correctly positioned)
                  3. Apply the selected locomotion type when chosen in settings
                  4. Define input mappings for hover activation
                  5. Define input mappings for exiting hover and transitioning into flight
                  6. Implement smooth transitions between walking, hovering, and flying
                  7. Add animations for walking, hovering, and flying to improve realism

            4. Finalize the game by ensuring:

               * A functional start menu exists
               * A settings menu is implemented and operational
               * All UI elements respond correctly
               * Controller input mappings are accurate
               * All systems are fully integrated and stable

         3. Once all systems, assets, input mappings, physics, and gameplay mechanics are complete, the user should be able to play the fully functional VR game.

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
