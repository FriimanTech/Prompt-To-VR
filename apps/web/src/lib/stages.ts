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
  scene_compile: "9. Scene compile",
  cmake_configure: "10. CMake configure",
  cmake_build: "11. CMake build",
  ready_to_launch: "Ready to launch",
  failed: "Failed",
};

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
      return "text-accent";
    case "warning":
    case "skipped":
      return "text-warn";
    case "failed":
      return "text-err";
    default:
      return "text-muted";
  }
}
