You are building a LOCAL-FIRST “Prompt to Native VR Game” system. The system must accept a detailed text prompt for a VR game idea, generate the required plans/assets/code, compile a native C++ VR executable, and let the user launch and test it locally on Windows PC VR. The testing path is Meta Quest 3 through Quest Link acting as a PC VR headset. The runtime must be a custom native VR engine using Vulkan + OpenXR. Do NOT use Unity, Unreal Engine, Godot, WebXR, browser-based rendering, Three.js, Babylon.js, or any engine export as the primary runtime.

## Mission

Create a full working local application that takes a detailed VR game prompt, such as an idea for an Iron Man VR game, and turns it into a playable local native VR game by generating:

1. structured game specifications
2. 2D concept/reference images
3. textured 3D assets
4. C++ gameplay modules
5. scene configuration files
6. a local build pipeline that compiles and launches the VR game

The user wants the first version to focus on LOCAL prompt-to-native-VR only. Do NOT implement future roadmap items in this version.

## Hard constraints

- Do NOT use Unity, Unreal Engine, Godot, or WebXR.
- The playable runtime must be a native C++ executable.
- Use Vulkan for rendering.
- Use OpenXR for headset/controller/runtime integration.
- Use CMake as the build system.
- Use vcpkg manifest mode for C++ dependencies.
- Use a local web frontend as the control plane.
- Use a local backend/orchestrator.
- Use local filesystem storage only for projects, artifacts, logs, and build outputs.
- Do NOT add Supabase, S3, Temporal, multiplayer, speech-to-text, future app packaging, Steam packaging, Quest standalone packaging, marketplace publishing helpers, or cloud auth/storage in this version.
- Prefer deterministic systems over free-form agent improvisation.
- Prefer reusable built-in VR mechanics over generating every mechanic from scratch.
- Use Claude for the hardest gameplay planning and the hardest C++/gameplay code generation.
- For Tripo image-to-3D, explicitly enable textured output so the 3D models are colored/game-ready.

## Important architectural rule

Do NOT build a brand new engine every time from the user prompt.

Instead, build:
1. one reusable native VR runtime / engine core
2. one local orchestrator that turns prompts into structured build artifacts
3. one pipeline that generates assets, scene data, and bounded gameplay modules against the stable runtime

The prompt pipeline should generate:
- BuildSpec
- AssetManifest
- MechanicPlan
- SceneSpec
- generated C++ gameplay modules
- local build outputs

The engine core itself must remain mostly hand-authored and reusable.

## Cursor execution behavior

Use Cursor’s capabilities aggressively:
- use multiple agents / sub-agents / parallel workstreams where available
- use project rules in `.cursor/rules`
- use `AGENTS.md`
- use web search before making assumptions about current SDK versions, provider APIs, model names, or setup details
- build as much working code as possible, not just placeholders
- create docs that tell the user exactly what still must be done manually

If the current Cursor mode supports Background Agents, use them for larger isolated streams of work. If not, still split the task into clean scoped workstreams conceptually and implement the repo in phases.

## Primary workstreams / sub-agents

Create and coordinate the following workstreams:

### 1. Frontend workstream
Build the local website UI.
Responsibilities:
- project list page
- create project page
- prompt input UI
- generation progress timeline
- artifact viewer
- logs viewer
- launch latest build button
- local API integration

### 2. Backend orchestrator workstream
Build the local Python backend that coordinates generation.
Responsibilities:
- FastAPI app
- project folder creation
- schema validation
- pipeline stage execution
- status tracking
- local filesystem artifact management
- provider client orchestration
- build invocation and log capture

### 3. Engine core workstream
Build the reusable native C++ VR runtime.
Responsibilities:
- platform bootstrap
- Vulkan renderer
- OpenXR session loop
- ECS/entity management
- scene loading
- physics integration
- interaction framework
- debug UI/tools
- save/load hooks

### 4. Asset pipeline workstream
Build provider clients and asset normalization.
Responsibilities:
- Gemini image generation client
- Tripo image-to-3D client
- polling + download logic
- asset normalization
- scale/material validation
- engine import preparation

### 5. Gameplay generation workstream
Build the planning + code generation path.
Responsibilities:
- BuildSpec generation
- AssetManifest generation
- MechanicPlan generation
- SceneSpec generation
- generated C++ gameplay modules
- test generation for new modules

### 6. Docs + rules + tests workstream
Build all docs, `.env.example`, rules, tests, and troubleshooting notes.

## Model / provider strategy

### Claude usage
Use Claude for:
- highest-level gameplay planning
- mechanic decomposition
- generated C++ gameplay code
- ECS component design
- AI/NPC behavior logic
- scene compiler logic where reasoning is needed

Prefer:
- highest-available Claude Opus 4.x model for hardest gameplay planning and hardest C++ work
- Claude Sonnet 4.x for lighter planning, routine transformations, and supportive code generation

### Gemini usage
Use Google Gemini / Imagen for:
- concept/reference image generation
- UI concept generation if needed
- visual iteration for asset references

Prefer:
- Gemini image generation for fast iterative concept/reference passes
- Imagen when the docs indicate it is the better path for highest-fidelity final reference images

### Tripo usage
Use Tripo for image-to-3D and optionally text-to-3D fallback.

CRITICAL REQUIREMENT:
For Tripo image-to-3D requests, explicitly set textured output so models are colored and game-ready.

Implementation requirements:
- set `texture: true`
- enable `pbr: true` when supported
- prefer `texture_alignment: "original_image"` when appropriate for fidelity to the source concept image
- support async polling of task status
- download final GLB outputs
- if the provider’s current official docs differ, Cursor must verify live and adapt the request schema accordingly, while preserving the intent of textured/color outputs

## Local-first stack

### Frontend
- React
- TypeScript
- Vite

### Backend
- Python
- FastAPI
- Pydantic
- local filesystem project storage
- optional SQLite only if absolutely needed for local job state; prefer filesystem + JSON first

### Native runtime
- C++20
- CMake
- Vulkan
- OpenXR
- GLFW or SDL for desktop bootstrap and Vulkan surface creation
- EnTT for ECS
- Jolt Physics or Bullet for physics
- GLM for math
- TinyGLTF for glTF/GLB loading
- Volk if chosen for Vulkan loading
- Vulkan Memory Allocator if chosen
- shader compilation support
- Dear ImGui only for debug/developer tooling

### Dependency management
- vcpkg manifest mode

## Repo structure

Create a monorepo with this structure:

/apps
  /web
  /orchestrator

/engine
  /runtime
  /tools
  /generated
    /mechanics
    /ai
    /dialogue

/packages
  /schemas
  /provider-clients

/docs
/.cursor/rules
/AGENTS.md
/.env.example
/projects

## Local project storage model

Store every generated project locally on disk, for example:

/projects/<project_id>/
  prompt.txt
  project_state.json
  build_spec.json
  asset_manifest.json
  mechanic_plan.json
  scene_spec.json
  acceptance_test_plan.json
  /images
  /assets
  /generated_code
  /logs
  /builds

Do NOT add cloud storage or remote databases in this version.

## Required schemas / contracts

Create strict schemas and validation for:
- BuildSpec
- AssetManifest
- MechanicPlan
- SceneSpec
- CodeTask
- ProjectState
- ArtifactMetadata
- AcceptanceTestPlan

These schemas must be first-class and enforced. Once BuildSpec exists, downstream stages must use structured artifacts rather than repeatedly reasoning from the raw prompt.

## Generation pipeline

Implement the pipeline as:

Prompt
-> BuildSpec
-> AssetManifest
-> MechanicPlan
-> SceneSpec
-> reference image generation
-> Tripo image-to-3D
-> asset normalization/import prep
-> generated C++ gameplay modules
-> scene compilation
-> CMake configure/build
-> local launch

Do not let downstream stages skip schemas.

## Engine requirements

Build a real reusable VR runtime skeleton with these subsystems:

1. platform bootstrap
2. Vulkan renderer
3. OpenXR instance/session/swapchain/frame loop
4. ECS/entity/component system
5. scene graph or scene-to-ECS bridge
6. physics integration
7. input abstraction
8. VR interaction framework
9. audio placeholder
10. local save/load hooks
11. developer debug panels and diagnostics

## Built-in reusable VR mechanics

Build these as reusable configurable modules so the AI does not regenerate basic VR behavior every time:

- teleport locomotion
- smooth locomotion
- snap turn
- hand/controller interaction
- grab
- throw
- use/interact
- push/pull
- trigger zones
- object spawners
- basic inventory slots
- dialogue triggers
- health and damage hooks
- basic enemy AI hooks
- basic projectile hooks
- simple menu / HUD anchor support

These mechanics must be data-driven.

## Generated code boundaries

Generated C++ must never rewrite or replace:
- low-level renderer
- OpenXR frame/session loop
- core platform layer
- memory management core
- base ECS engine core

Generated code is only allowed in bounded areas like:
- /engine/generated/mechanics
- /engine/generated/ai
- /engine/generated/dialogue

Define stable interfaces the generated modules must implement.

## Asset pipeline details

### Reference image generation
Build a provider abstraction for reference image generation. Support Gemini and/or Imagen-based reference creation depending on current official docs and availability.

### Tripo image-to-3D client
Build a provider client that:
- accepts image input
- sends image-to-3D requests
- explicitly requests textured output
- supports polling
- downloads final model files
- stores outputs and metadata locally
- validates that outputs are usable in the engine

### Asset normalization
Build deterministic code to:
- validate readable model files
- inspect material/texture presence
- normalize scale
- generate or mark collision placeholders
- prepare import metadata
- reject obviously broken outputs early

## Gameplay planning and code generation

### Master planner
Create a planning stage that converts the raw user prompt into a BuildSpec including:
- game type
- scenes
- mechanics
- assets
- NPC/dialogue needs
- UX/HUD needs
- required tests
- constraints and acceptance criteria

### Asset planner
Convert BuildSpec into AssetManifest including:
- asset categories
- style notes
- rigging needs
- reference image strategy
- whether each asset uses image-to-3D or text-to-3D fallback

### Mechanic planner
Decide whether each mechanic:
- maps to an existing reusable runtime module
- needs only configuration
- needs generated C++ code

### C++ code generation
Use Claude for:
- gameplay modules
- ECS components
- state machines
- AI behavior logic
- dialogue hooks
- integration tests for new modules

## Scene compiler

Build a deterministic scene compiler that converts:
- BuildSpec
- AssetManifest
- MechanicPlan
- SceneSpec

into engine-readable outputs such as:
- scene files
- entity/component configuration
- transforms/placements
- mechanic bindings
- build metadata

## Web app requirements

Create a local web app with:
- project list
- new project form
- prompt editor
- generate/build button
- stage-by-stage progress
- artifact browser
- build logs
- launch latest build button

The website is a local control plane only.

## Orchestrator requirements

Create a local FastAPI service that:
- creates project folders
- persists stage outputs locally
- validates schemas
- runs the planning pipeline
- calls provider APIs
- triggers CMake configure/build
- returns progress/logs to the frontend
- can resume partially completed local runs where possible

## Docs that must be generated

Generate these markdown docs:

- /docs/overview.md
- /docs/architecture.md
- /docs/local-setup.md
- /docs/build-and-run.md
- /docs/provider-setup.md
- /docs/prompt-schema.md
- /docs/runtime-subsystems.md
- /docs/project-layout.md
- /docs/troubleshooting.md

The docs must explicitly tell the user:
- what software to install locally
- what SDKs are needed
- which APIs require accounts/keys
- where to place API keys
- what environment variables to set
- how to run frontend/backend
- how to configure/build the engine
- how to test with Meta Quest 3 via Quest Link for PC VR

## Environment files

Create `.env.example` with placeholders for:
- ANTHROPIC_API_KEY=
- GOOGLE_API_KEY=
- TRIPO_API_KEY=
- OPTIONAL_VULKAN_SDK_PATH=
- OPTIONAL_OPENXR_RUNTIME_HINT=

If separate env files are needed for frontend/backend, create them and document them.

## Cursor rules / AGENTS instructions

Create `.cursor/rules` and/or `AGENTS.md` enforcing:
- native-only runtime
- no Unity / Unreal / Godot / WebXR
- schema-first development
- generated code boundaries
- reusable mechanics first
- Claude for hardest gameplay/C++ tasks
- verify provider APIs with web search before implementation
- keep docs updated when code structure changes

## Testing requirements

Add tests for:
- schema validation
- provider client request/response parsing
- scene compilation
- generated code boundaries
- build invocation
- asset normalization checks

Add smoke tests for:
- desktop boot
- OpenXR runtime detection
- sample scene loading
- GLB import
- one reusable mechanic working

## Delivery requirements

Build as much real working code as possible, not just stubs.

The repo must end with:
- working local frontend
- working local FastAPI backend
- working native C++ runtime skeleton
- provider client scaffolding for Anthropic, Gemini/Imagen, and Tripo
- strict schemas
- local project storage pipeline
- docs
- env examples
- tests
- one end-to-end sample prompt flow
- one sample generated VR room demo that launches locally

## Implementation order

Phase 1:
- repo scaffolding
- docs
- env examples
- rules
- schemas
- frontend skeleton
- backend skeleton

Phase 2:
- runtime bootstrap
- Vulkan + OpenXR initialization
- sample scene loading
- reusable VR mechanics framework

Phase 3:
- planner pipeline
- BuildSpec / AssetManifest / MechanicPlan / SceneSpec generation
- local artifact persistence

Phase 4:
- Gemini/Imagen reference image integration
- Tripo image-to-3D integration
- asset normalization pipeline

Phase 5:
- gameplay code generation path
- scene compiler
- CMake build orchestration
- local launch flow

Phase 6:
- sample end-to-end prompt
- tests
- troubleshooting notes
- cleanup and polish

## What not to build in this version

Do NOT implement:
- speech-to-text
- multiplayer
- cloud auth
- Supabase
- S3
- Temporal
- Quest standalone packaging
- Steam packaging
- marketplace publishing helpers
- Unity / Unreal / Godot export
- browser runtime rendering

## Final definition of done

A user can:
1. open the local website
2. create a project
3. paste a detailed VR game prompt
4. run generation
5. watch stage progress
6. have schemas/assets/code generated
7. compile a native VR executable
8. launch and test a playable local VR scene on PC VR through OpenXR

## Final mandatory output from Cursor

After implementation, generate a final markdown section named:

# Manual Steps I Still Need To Do

That section must explicitly list:
- software the user still must install locally
- APIs/accounts the user still must create
- exact env vars the user must fill in
- any scaffolding that cannot be completed without user secrets or manual setup
- the exact commands to run the first example project locally                    