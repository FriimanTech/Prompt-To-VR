// ---------------------------------------------------------------------------
// The pipeline, as the UI understands it.
//
// This file MIRRORS ``_SHARED_STAGES`` / ``_PCVR_TAIL`` / ``_QUEST_TAIL`` in
// apps/orchestrator/ptv_orchestrator/pipeline/orchestrator.py. The backend is
// authoritative; if the two disagree, the resume menu offers stages in an order
// the pipeline will not actually run them in, and the timeline marks the wrong
// stages as queued.
//
// Ordering constraints worth knowing before editing, because they are real
// dependencies and not preferences:
//
//   * ``npc_plan`` precedes rigging. Whether a thing needs animating at all is
//     the most expensive question in the pipeline (~30 credits per rig, ~20
//     per clip) and only the prompt can answer it.
//   * ``character_routing`` follows ``tripo_models`` (Tripo's rig-check needs
//     the mesh) and precedes every rigging stage, which all read its answer.
//     ``uthana_rigging`` runs before Tripo's ``asset_rigging`` so a character
//     whose Uthana rig fails falls back to Tripo in the same run.
//   * All rigging and animation (Tripo and Uthana) precede normalization,
//     because they REPLACE the primary GLB and everything downstream has to
//     validate, socket, decimate and compile the rigged, animated mesh.
//   * ``asset_contract`` precedes codegen: the agent needs real socket names.
//   * ``world_generation`` precedes ``code_generation``. Gameplay code acts on
//     the worlds, and a mechanic that opens a door needs the ids the world
//     architect actually declared. This pair was the other way round until the
//     multi-environment work; a stale copy here is what makes the timeline
//     look like it is running backwards.
// ---------------------------------------------------------------------------

/** What a stage spends when it runs. Drives the cost badges, because the
 *  difference between "re-run this freely" and "this bills real money per
 *  asset" is the single most useful thing to know before clicking. */
export type StageCost =
  /** Deterministic local work: tools, compilers, file writing. Free. */
  | "free"
  /** One or more LLM calls. Costs tokens, not per-asset credits. */
  | "llm"
  /** Third-party generation billed per asset or per clip. The expensive one. */
  | "credits";

export type StageGroup =
  | "plan"
  | "assets"
  | "build"
  | "compile"
  | "ship";

export type StageMeta = {
  id: string;
  /** Position in the run, 1-based. Shown as the step number. */
  step: number;
  label: string;
  group: StageGroup;
  cost: StageCost;
  /** One line, in plain terms: what comes out of this stage. */
  blurb: string;
  /** Only present for the device-specific tail. */
  device?: "pcvr" | "quest";
};

export const STAGE_GROUPS: { id: StageGroup; label: string; blurb: string }[] = [
  {
    id: "plan",
    label: "Plan the game",
    blurb: "Turn the prompt into structured specs. Cheap and fast to re-run.",
  },
  {
    id: "assets",
    label: "Make the assets",
    blurb:
      "Images, 3D models, rigs, animations, then normalization and sockets. Credits are spent here.",
  },
  {
    id: "build",
    label: "Build the world and the gameplay",
    blurb:
      "Agents write the C++: environments first, then the mechanics that act on them.",
  },
  {
    id: "compile",
    label: "Optimize and compose",
    blurb:
      "Deterministic. Decimate to LODs, then merge environments and gameplay into scene.json.",
  },
  {
    id: "ship",
    label: "Compile and ship",
    blurb: "Produce the runnable build for the target device.",
  },
];

/** Every stage the pipeline can run, in backend order. */
const STAGES: StageMeta[] = [
  {
    id: "build_spec",
    step: 1,
    label: "BuildSpec",
    group: "plan",
    cost: "llm",
    blurb: "The prompt becomes a strict spec: scenes, mechanics, assets.",
  },
  {
    id: "asset_manifest",
    step: 2,
    label: "AssetManifest",
    group: "plan",
    cost: "llm",
    blurb: "Which objects to generate. The environment is not one of them.",
  },
  {
    id: "npc_plan",
    step: 3,
    label: "NPC plan",
    group: "plan",
    cost: "llm",
    blurb:
      "Who fights whom, and which characters need rigs at all. Runs early because that answer is what costs credits later.",
  },
  {
    id: "game_design",
    step: 4,
    label: "Game design",
    group: "plan",
    cost: "llm",
    blurb:
      "The rules of the loop: health, lives, respawn, objectives, how it ends, and the end screens. Explicit prompt rules win; nothing a game does not need is added.",
  },
  {
    id: "mechanic_plan",
    step: 5,
    label: "MechanicPlan",
    group: "plan",
    cost: "llm",
    blurb:
      "Maps each mechanic to a built-in, a built-in with config, or generated C++.",
  },
  {
    id: "scene_spec",
    step: 6,
    label: "SceneSpec",
    group: "plan",
    cost: "llm",
    blurb: "One scene layout per scene, composed in parallel and validated.",
  },
  {
    id: "reference_images",
    step: 7,
    label: "Reference images",
    group: "assets",
    cost: "credits",
    blurb: "Gemini draws each object. Characters and avatars come out T-posed.",
  },
  {
    id: "tripo_models",
    step: 8,
    label: "Tripo 3D models",
    group: "assets",
    cost: "credits",
    blurb: "Image to textured, PBR 3D mesh.",
  },
  {
    id: "character_routing",
    step: 9,
    label: "Character routing",
    group: "assets",
    cost: "llm",
    blurb:
      "Decides who rigs and animates each character: Uthana (humanoids, finger joints, styled gaits) or Tripo (creatures, preset clips). Tripo's rig-check is free.",
  },
  {
    id: "uthana_rigging",
    step: 10,
    label: "Uthana auto-rig",
    group: "assets",
    cost: "credits",
    blurb:
      "Rigs the player's avatar and routed humanoids WITH finger joints. A low-confidence rig falls back to Tripo.",
  },
  {
    id: "asset_rigging",
    step: 11,
    label: "Auto-rigging",
    group: "assets",
    cost: "credits",
    blurb:
      "Tripo-routed creatures and humanoids: ~30 credits per rig. The free rig-check from routing is reused.",
  },
  {
    id: "asset_animation",
    step: 12,
    label: "Animation retarget",
    group: "assets",
    cost: "credits",
    blurb: "~20 credits per clip. Idle, walk, run, attack and the rest.",
  },
  {
    id: "uthana_motion_plan",
    step: 13,
    label: "Uthana motion plan",
    group: "assets",
    cost: "llm",
    blurb:
      "Chooses each character's gait style, stride and speed (0.78 x height to walk, 2.2 x height to run). Generates nothing.",
  },
  {
    id: "uthana_animation",
    step: 14,
    label: "Uthana animation",
    group: "assets",
    cost: "credits",
    blurb:
      "Reuses motions already in your Uthana organisation by name, generates the rest, adds the shared idle, and merges all clips into one animated GLB.",
  },
  {
    id: "asset_normalization",
    step: 15,
    label: "Asset normalization",
    group: "assets",
    cost: "free",
    blurb:
      "Validates every GLB (header, scale, collision, skinned flag) and writes asset_normalization.json. Changes no geometry.",
  },
  {
    id: "asset_contract",
    step: 16,
    label: "Asset sockets",
    group: "assets",
    cost: "free",
    blurb:
      "Derives named attachment points (muzzle, grip) from geometry, so gameplay code can ask for them by name.",
  },
  {
    id: "world_generation",
    step: 17,
    label: "VR environments",
    group: "build",
    cost: "llm",
    blurb:
      "One world architect per scene, in parallel. Each writes C++, compiles offline, and declares its doors, spawns and walkable space.",
  },
  {
    id: "code_generation",
    step: 18,
    label: "Gameplay C++",
    group: "build",
    cost: "llm",
    blurb:
      "Mechanics, AI and dialogue, with a compile-and-repair loop. Runs after the worlds so it can address real object ids.",
  },
  {
    id: "asset_optimization",
    step: 19,
    label: "Decimation and LODs",
    group: "compile",
    cost: "free",
    blurb:
      "Raw Tripo output is ~500k triangles per asset. This is what makes a scene runnable on a Quest 3.",
  },
  {
    id: "scene_compile",
    step: 20,
    label: "Scene compile",
    group: "compile",
    cost: "free",
    blurb:
      "Merges each environment with its gameplay entities into scene.json. Fails loudly if a mechanic needs a socket its asset lacks.",
  },
  {
    id: "cmake_build",
    step: 21,
    label: "CMake build (.exe)",
    group: "ship",
    cost: "free",
    blurb: "Visual Studio + Vulkan + OpenXR. Produces the PC VR executable.",
    device: "pcvr",
  },
  {
    id: "quest_build",
    step: 21,
    label: "Quest APK build",
    group: "ship",
    cost: "free",
    blurb: "Gradle + NDK. Produces a sideloadable standalone APK.",
    device: "quest",
  },
  {
    id: "quest_runtime_verify",
    step: 22,
    label: "Verify on headset",
    group: "ship",
    cost: "free",
    blurb:
      "Runs the APK on the connected Quest and checks the NPCs this game asked for appear, move and attack. Report-only; skipped when no headset is connected.",
    device: "quest",
  },
  // One entry per device: the Quest tail has one more stage before it, and
  // the step number is what the timeline shows.
  {
    id: "engine_promotion",
    step: 22,
    label: "Engine promotion",
    group: "ship",
    cost: "free",
    blurb: "Promotes anything reusable this project produced back into the engine.",
    device: "pcvr",
  },
  {
    id: "engine_promotion",
    step: 23,
    label: "Engine promotion",
    group: "ship",
    cost: "free",
    blurb: "Promotes anything reusable this project produced back into the engine.",
    device: "quest",
  },
];

export const STAGE_META: Record<string, StageMeta> = Object.fromEntries(
  STAGES.map((s) => [s.id, s]),
);

/** Stages for one target device, in run order. Mirrors the backend's
 *  ``_stages_for(target_device)``: shared stages then the device tail. */
export function stagesFor(targetDevice: string = "pcvr"): StageMeta[] {
  const device = targetDevice === "quest" ? "quest" : "pcvr";
  return STAGES.filter((s) => !s.device || s.device === device);
}

/** Every stage that can be resumed from, in order. */
export function resumableStages(targetDevice: string = "pcvr"): string[] {
  return stagesFor(targetDevice).map((s) => s.id);
}

/** Back-compat default (PC VR) for callers that do not know the device yet. */
export const RESUMABLE_STAGES = resumableStages("pcvr");

/** Ordering used by anything that needs the full vocabulary including the
 *  non-stage lifecycle markers. */
export const STAGE_ORDER = [
  "created",
  ...Array.from(new Set(STAGES.map((s) => s.id))),
  "ready_to_launch",
  "failed",
] as const;

/** Terminal and lifecycle labels that are not pipeline stages. */
const LIFECYCLE_LABELS: Record<string, string> = {
  created: "Created",
  ready_to_launch: "Ready to launch",
  failed: "Failed",
  // Emitted by older projects; kept so their history still reads.
  cmake_configure: "CMake configure",
  quest_install: "Install on Quest",
};

export function stageLabel(id: string): string {
  const meta = STAGE_META[id];
  if (meta) return `${meta.step}. ${meta.label}`;
  return LIFECYCLE_LABELS[id] ?? id;
}

/** Label without the step number, for headings and badges. */
export function stageName(id: string): string {
  return STAGE_META[id]?.label ?? LIFECYCLE_LABELS[id] ?? id;
}

export function stageCost(id: string): StageCost {
  return STAGE_META[id]?.cost ?? "free";
}

export const COST_LABELS: Record<StageCost, string> = {
  free: "local",
  llm: "tokens",
  credits: "credits",
};

export const COST_HINTS: Record<StageCost, string> = {
  free: "Deterministic local work. Re-run freely.",
  llm: "Makes model calls. Costs tokens.",
  credits: "Bills third-party credits per asset. Re-run deliberately.",
};

/** Status vocabulary, normalized into the four things a reader cares about.
 *
 *  The backend emits many more event kinds than statuses (``bounds_reject``,
 *  ``agent_turn``, ``compile_error``, ``fallback`` ...). Mapping them here
 *  rather than at each call site is what keeps a new event kind from silently
 *  rendering as untinted grey. */
export type StatusTone = "ok" | "busy" | "warn" | "err" | "idle";

export function statusTone(status: string): StatusTone {
  switch (status) {
    case "succeeded":
    case "ready":
    case "ok":
      return "ok";
    case "running":
    case "info":
    case "log":
    case "agent_turn":
      return "busy";
    case "warning":
    case "warn":
    case "skipped":
    case "fallback":
    case "bounds_reject":
    case "invalid_scene":
    // Aborting is not a failure: nothing is broken and the project is
    // immediately re-runnable, so it reads as a caution, not an error.
    case "aborted":
      return "warn";
    case "failed":
    case "compile_error":
    case "run_error":
      return "err";
    default:
      return "idle";
  }
}

const TONE_TEXT: Record<StatusTone, string> = {
  ok: "text-ok",
  busy: "text-accent",
  warn: "text-warn",
  err: "text-err",
  idle: "text-muted",
};

export function statusColor(status: string): string {
  return TONE_TEXT[statusTone(status)];
}

/** Human duration for a stage record, or "" when it never ran. */
export function formatDuration(
  startedAt: string | null,
  endedAt: string | null,
): string {
  if (!startedAt) return "";
  const start = Date.parse(startedAt);
  const end = endedAt ? Date.parse(endedAt) : Date.now();
  if (!Number.isFinite(start) || !Number.isFinite(end)) return "";
  const secs = Math.max(0, Math.round((end - start) / 1000));
  if (secs < 60) return `${secs}s`;
  const mins = Math.floor(secs / 60);
  if (mins < 60) return `${mins}m ${secs % 60}s`;
  return `${Math.floor(mins / 60)}h ${mins % 60}m`;
}
