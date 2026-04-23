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
│       │   ├── api/          # FastAPI routes + launcher
│       │   ├── build/        # CMake runner
│       │   └── pipeline/     # one module per stage
│       └── tests/
│
├── packages/
│   ├── schemas/              # Pydantic v2 + exported JSON Schemas
│   │   ├── ptv_schemas/      # models (BuildSpec, AssetManifest, …)
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
│   │   │                     # scene, physics, input, interaction, audio,
│   │   │                     # save, debug, mechanics
│   │   ├── shaders/          # .vert/.frag, compiled to SPIR-V via glslc
│   │   └── tests/            # CTest smoke targets
│   │
│   ├── tools/                # glb_validate, check_generated_bounds.py
│   └── generated/            # THE ONLY PLACE LLM C++ LIVES
│       ├── mechanics/        # IMechanic modules
│       ├── ai/               # IBehaviour modules
│       └── dialogue/         # IDialogueHook modules
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
│       ├── mechanic_plan.json
│       ├── scene_spec.<scene_id>.json
│       ├── scene_spec.json
│       ├── asset_normalization.json
│       ├── scene.json
│       ├── images/<asset_id>/*.png
│       ├── assets/<asset_id>/*.glb
│       ├── logs/*.log
│       └── builds/cmake-<timestamp>/
│
└── docs/
    ├── overview.md
    ├── architecture.md
    ├── local-setup.md
    ├── build-and-run.md
    ├── provider-setup.md
    ├── prompt-schema.md
    ├── runtime-subsystems.md
    ├── project-layout.md       (this file)
    └── troubleshooting.md
```
