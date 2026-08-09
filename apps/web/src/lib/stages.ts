export const STAGE_ORDER = [
  "created",
  "build_spec",
  "asset_manifest",
  "mechanic_plan",
  "scene_spec",
  "reference_images",
  "tripo_models",
  "asset_rigging",
  "asset_animation",
  "asset_normalization",
  "asset_contract",
  "code_generation",
  "world_generation",
  "asset_optimization",
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
  asset_rigging: "7. Auto-rigging",
  asset_animation: "8. Animation retarget",
  asset_normalization: "9. Asset normalization",
  asset_contract: "10. Asset sockets",
  code_generation: "11. C++ gameplay codegen",
  world_generation: "12. VR world generation",
  asset_optimization: "13. Asset decimation + LODs",
  scene_compile: "14. Scene compile",
  cmake_configure: "15. CMake configure",
  cmake_build: "15. CMake configure + build",
  quest_build: "15. Quest APK build",
  quest_install: "16. Install on Quest",
  engine_promotion: "16. Engine promotion",
  ready_to_launch: "Ready to launch",
  failed: "Failed",
};

// The stages the pipeline can actually run, in order. Mirrors the backend
// ``_stages_for(target_device)`` in apps/orchestrator/.../pipeline/orchestrator.py.
// Stages 1-14 are shared; the build tail differs by device (cmake_build for
// PC VR, quest_build for Quest). ``engine_promotion`` is the final stage.
const SHARED_STAGES = [
  "build_spec",
  "asset_manifest",
  "mechanic_plan",
  "scene_spec",
  "reference_images",
  "tripo_models",
  "asset_rigging",
  "asset_animation",
  "asset_normalization",
  "asset_contract",
  "code_generation",
  "world_generation",
  "asset_optimization",
  "scene_compile",
] as const;

export function resumableStages(targetDevice: string = "pcvr"): string[] {
  const tail =
    targetDevice === "quest"
      ? ["quest_build", "engine_promotion"]
      : ["cmake_build", "engine_promotion"];
  return [...SHARED_STAGES, ...tail];
}

// Back-compat default (PC VR) for callers that don't know the device yet.
export const RESUMABLE_STAGES = resumableStages("pcvr");

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
