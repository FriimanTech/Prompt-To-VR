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
│       │   ├── api/          # FastAPI routes + launcher
│       │   ├── build/        # CMake + Quest Gradle runners
│       │   └── pipeline/     # one module per stage
│       └── tests/
│
├── packages/
│   ├── schemas/              # Pydantic v2 + exported JSON Schemas
│   │   ├── ptv_schemas/      # models (BuildSpec, AssetManifest, NpcPlan, …)
│   │   ├── json/             # exported schema files (generated)
│   │   └── tests/
│   │
│   └── provider-clients/     # Anthropic, Google GenAI, Tripo wrappers
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
│   ├── tools/                # scene compiler, glb_validate, ptv_glb_sockets,
│   │                         # ptv_glb_optimize, check_generated_bounds.py
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
├── projects/                 # per-project local workspaces
│   └── <project_id>/
│       ├── prompt.txt
│       ├── project_state.json
│       ├── build_spec.json
│       ├── asset_manifest.json
│       ├── npc_plan.json
│       ├── mechanic_plan.json
│       ├── scene_spec.<scene_id>.json
│       ├── scene_spec.json
│       ├── asset_rig.json
│       ├── asset_animation.json
│       ├── asset_normalization.json
│       ├── asset_contract.json
│       ├── codegen_board.json
│       ├── world.scene.json
│       ├── scene.json
│       ├── images/<asset_id>/*.png
│       ├── assets/<asset_id>/
│       │   ├── model.glb                 # raw Tripo source (kept, not shipped)
│       │   ├── model_rigged.glb          # after asset_rigging
│       │   ├── model_animated.glb        # after asset_animation
│       │   ├── tripo_task.json
│       │   ├── <id>_lod0..3.glb          # decimated LOD chain (shipped)
│       │   └── <id>_lod.json             # decimation stamp (idempotency)
│       ├── logs/*.log
│       ├── builds/cmake-current/         # PC VR exe (symlink-like stamp)
│       └── builds/quest/                 # Quest Gradle tree + APK output
│
└── docs/
    ├── All_Stages_Docs/      # per-stage relearning (17 stages × PC + Quest)
    ├── overview.md
    ├── architecture.md
    ├── local-setup.md
    ├── build-and-run.md
    ├── quest-build.md
    ├── world-generation.md
    ├── npc-and-ai.md
    ├── navigation.md
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
