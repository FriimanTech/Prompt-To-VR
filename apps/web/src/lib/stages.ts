export const STAGE_ORDER = [
  "created",
  "build_spec",
  "asset_manifest",
  "mechanic_plan",
  "scene_spec",
  "reference_images",
  "tripo_models",
  "asset_normalization",
  "code_generation",
  "world_generation",
  "scene_compile",
  "cmake_configure",
  "cmake_build",
  "ready_to_launch",
  "failed",
] as const;

export const STAGE_LABELS: Record<string, string> = {
  created: "Created",
  build_spec: "1. BuildSpec",
  asset_manifest: "2. AssetManifest",
  mechanic_plan: "3. MechanicPlan",
  scene_spec: "4. SceneSpec",
  reference_images: "5. Reference images",
  tripo_models: "6. Tripo 3D",
  asset_normalization: "7. Asset normalization",
  code_generation: "8. C++ gameplay codegen",
  world_generation: "9. VR world generation",
  scene_compile: "10. Scene compile",
  cmake_configure: "11. CMake configure",
  cmake_build: "11. CMake configure + build",
  engine_promotion: "12. Engine promotion",
  ready_to_launch: "Ready to launch",
  failed: "Failed",
};

// The stages the pipeline can actually run, in order. Mirrors the backend
// ``_STAGES`` list in apps/orchestrator/.../pipeline/orchestrator.py: CMake
// configure is folded into ``cmake_build``, and ``engine_promotion`` is the
// final stage. Used to populate the "Resume from stage" menu.
export const RESUMABLE_STAGES = [
  "build_spec",
  "asset_manifest",
  "mechanic_plan",
  "scene_spec",
  "reference_images",
  "tripo_models",
  "asset_normalization",
  "code_generation",
  "world_generation",
  "scene_compile",
  "cmake_build",
  "engine_promotion",
] as const;

export function stageLabel(id: string): string {
  return STAGE_LABELS[id] ?? id;
}

export function statusColor(status: string): string {
  switch (status) {
    case "succeeded":
    case "ready":
    case "ok":
      return "text-ok";
    case "running":
    case "info":
    case "log":
    case "agent_turn":
      return "text-accent";
    case "warning":
    case "warn":
    case "skipped":
    case "fallback":
    case "bounds_reject":
    case "invalid_scene":
      return "text-warn";
    case "failed":
    case "compile_error":
    case "run_error":
      return "text-err";
    case "pending":
    case "queued":
      return "text-muted";
    default:
      return "text-muted";
  }
}
