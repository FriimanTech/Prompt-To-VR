# Project layout

```
Prompt-To-VR/
├── AGENTS.md
├── README.md
├── .env.example
├── .gitignore
├── pytest.ini
│
├── .cursor/
│   ├── rules/                # 00-core, 10-schemas, 20-engine-core,
│   │                         # 30-generated-code, 40-providers, 50-docs
│   └── agents/               # planner, asset-planner, mechanic-planner,
│                             # scene-compiler, cpp-coder, verifier
│
├── apps/
│   ├── web/                  # React + Vite + TS control panel
│   │   ├── src/pages/        # ProjectList, NewProject, ProjectDetail
│   │   ├── src/components/   # StageTimeline, ArtifactBrowser, LogsViewer
│   │   └── src/api/          # fetch + SSE client
│   │
│   └── orchestrator/         # FastAPI backend
│       ├── ptv_orchestrator/
│       │   ├── main.py       # app entrypoint
│       │   ├── settings.py   # .env loader
│       │   ├── storage.py    # filesystem project store
│       │   ├── events.py     # in-memory SSE bus
│       │   ├── engine_catalog.py
│       │   ├── runs.py       # one-run-per-project registry, abort
│       │   ├── fsutil.py     # lock-tolerant replace / move (Windows)
│       │   ├── quest_ids.py  # Android package names
│       │   ├── api/          # FastAPI routes + launcher
│       │   ├── build/        # CMake + Quest Gradle runners
│       │   └── pipeline/     # stage_<name>.py per stage + orchestrator.py,
│       │                     # run_reset.py, sockets/, uthana/
│       └── tests/
│
├── packages/
│   ├── schemas/              # Pydantic v2 + exported JSON Schemas
│   │   ├── ptv_schemas/      # models (BuildSpec, AssetManifest, NpcPlan, …)
│   │   ├── json/             # exported schema files (generated)
│   │   └── tests/
│   │
│   └── provider-clients/     # Anthropic, OpenAI, LlmClient, Google image +
│                             # Gemini vision, Tripo, Uthana wrappers
│       ├── ptv_providers/
│       └── tests/
│
├── engine/
│   ├── CMakeLists.txt
│   ├── CMakePresets.json
│   ├── vcpkg.json
│   ├── runtime/              # hand-authored C++ VR engine
│   │   ├── include/ptv/      # public headers
│   │   ├── src/              # subsystems: core, platform, render, xr, ecs,
│   │   │                     # scene, physics, nav, input, interaction,
│   │   │                     # save, debug, mechanics
│   │   │   └── mechanics/builtin/<mechanic>/   # one module per built-in
│   │   ├── shaders/          # .vert/.frag, compiled to SPIR-V via glslc
│   │   └── tests/            # CTest smoke targets
│   │
│   ├── worldgen/             # offline ptv_worldgen host (no Vulkan/XR/ECS)
│   ├── android/              # Quest APK Gradle template (@PTV_*@ tokens)
│   ├── promoted/             # registry.json (engine_promotion stage)
│   ├── tools/                # glb_validate, ptv_glb_sockets, ptv_glb_views,
│   │                         # ptv_glb_optimize, ptv_texture_compress,
│   │                         # check_generated_bounds.py, prepare_default_hands.py
│   └── generated/            # THE ONLY PLACE LLM C++ LIVES
│       ├── mechanics/        # IMechanic modules
│       ├── ai/               # IBehaviour modules
│       ├── dialogue/         # IDialogueHook modules
│       └── world/            # IWorldBuilder modules (offline worldgen only)
│
├── samples/
│   └── starter-room/         # hand-authored scene + .exe
│       ├── CMakeLists.txt
│       ├── main.cpp
│       └── scene.json
│
├── Default_Models/           # default rigged hand GLB (Hands Only games)
│
├── projects/                 # per-project local workspaces
│   ├── memory_calibration.json           # shared: measured/estimated memory ratios
│   └── <project_id>/
│       ├── prompt.txt
│       ├── project_state.json
│       ├── build_spec.json
│       ├── optimization_preferences.json
│       ├── asset_manifest.json
│       ├── interaction_design.json
│       ├── npc_plan.json
│       ├── game_design.json
│       ├── mechanic_plan.json
│       ├── scene_spec.<scene_id>.json
│       ├── scene_spec.json
│       ├── character_routing.json
│       ├── tripo_rig_check.json
│       ├── uthana_rig.json
│       ├── asset_rig.json
│       ├── asset_animation.json
│       ├── uthana_motion_plan.json
│       ├── uthana_animation.json         # Uthana motion ledger
│       ├── asset_normalization.json
│       ├── socket_report.json
│       ├── asset_contract.json
│       ├── world.scene.json              # world_generation (+ world.<scene_id>.scene.json)
│       ├── codegen_board.json
│       ├── scene.json                    # + scene.<scene_id>.json per scene
│       ├── scene_complexity.json
│       ├── memory_status.json
│       ├── runtime_verification.json     # Quest only: on-headset checks
│       ├── memory_feedback.json          # Quest only: drives the memory correction loop
│       ├── images/<asset_id>/*.png
│       ├── assets/<asset_id>/
│       │   ├── model.glb                 # raw Tripo source (kept, not shipped)
│       │   ├── model_uthana_rigged.glb   # after uthana_rigging
│       │   ├── model_rigged.glb          # after asset_rigging
│       │   ├── model_animated.glb        # after asset_animation / uthana_animation
│       │   ├── tripo_task.json
│       │   ├── sockets/                  # socket_generation views, masks, result.json
│       │   ├── <id>_lod0..3.glb          # decimated LOD chain (shipped)
│       │   └── <id>_lod.json             # decimation stamp (idempotency)
│       ├── assets/ptv_textures/          # KTX2 / source textures by content hash
│       ├── logs/*.log                    # e.g. cmake.log, gradle.log, world.log, codegen_verify.log
│       ├── logs/runtime/verify_*.log     # headset logcat from quest_runtime_verify
│       ├── previous_runs/<UTC stamp>/    # archived by a full run (see run-lifecycle.md)
│       ├── builds/cmake-current/         # PC VR exe (symlink-like stamp)
│       └── builds/quest/                 # Quest Gradle tree + APK output
│
└── docs/
    ├── All_Stages_Docs/      # per-stage docs, PC_VR_Stages/ + Quest_Stages/
    │                         # (01_build_spec … 17_engine_promotion)
    ├── overview.md
    ├── architecture.md
    ├── local-setup.md
    ├── build-and-run.md
    ├── build-and-run-pcvr.md
    ├── run-lifecycle.md
    ├── quest-build.md
    ├── quest-optimization.md
    ├── world-generation.md
    ├── npc-and-ai.md
    ├── navigation.md
    ├── locomotion.md
    ├── input-and-bindings.md
    ├── game-flow.md
    ├── weapons.md
    ├── sockets.md
    ├── uthana.md
    ├── rendering.md
    ├── codegen-agents.md
    ├── engine-growth.md
    ├── provider-setup.md
    ├── prompt-schema.md
    ├── runtime-subsystems.md
    ├── project-layout.md       (this file)
    └── troubleshooting.md
```

Stage-by-stage documentation: [`All_Stages_Docs/README.md`](All_Stages_Docs/README.md).
