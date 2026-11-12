import { useCallback, useEffect, useRef, useState, type ReactNode } from "react";
import { useParams } from "react-router-dom";
import { useQuery, useQueryClient } from "@tanstack/react-query";
import { api } from "../api/client";
import { eventStream } from "../api/client";
import { StageTimeline, type PipelineEvent } from "../components/StageTimeline";
import { ArtifactBrowser } from "../components/ArtifactBrowser";
import { LogsViewer } from "../components/LogsViewer";
import {
  COST_HINTS,
  STAGE_GROUPS,
  stageName,
  stagesFor,
} from "../lib/stages";

type MemoryStatusFile = {
  status?: string;
  source?: string;
  reasons?: string[];
  compromises?: string[];
  measured_peak_mb?: number | null;
  estimated_peak_mb?: number | null;
  estimates?: Record<string, { steady_mb: number; peak_mb: number }>;
};

const MEMORY_LABEL: Record<string, string> = {
  verified_safe: "Memory: verified on a Quest 3",
  estimated_safe_not_device_tested: "Memory: not tested on a Quest 3",
  at_risk: "Memory: at risk (estimate over target)",
  failed_on_device: "Memory: failed on the Quest 3",
};

/** Download APK (always, once built), the memory badge, and the user's
 *  optimization overrides with their scene-specific risk. */
function QuestStatusPanel({ id, hasApk }: { id: string; hasApk: boolean }) {
  const memory = useQuery({
    queryKey: ["memory_status", id],
    queryFn: () => api.projectJson<MemoryStatusFile>(id, "memory_status.json"),
    refetchInterval: 10_000,
  });
  const prefs = useQuery({
    queryKey: ["optimization_preferences", id],
    queryFn: () =>
      api.projectJson<Record<string, unknown>>(id, "optimization_preferences.json"),
  });
  const complexity = useQuery({
    queryKey: ["scene_complexity", id],
    queryFn: () =>
      api.projectJson<{
        scenes?: Record<string, { complexity?: { estimate?: { gpu_ms_p95?: number } } }>;
      }>(id, "scene_complexity.json"),
  });
  const verify = useQuery({
    queryKey: ["runtime_verification", id],
    queryFn: () =>
      api.projectJson<{ performance?: { gpu?: number; fps?: number } }>(
        id,
        "runtime_verification.json",
      ),
  });
  const m = memory.data;
  const status = m?.status ?? (hasApk ? "estimated_safe_not_device_tested" : "");
  const tone =
    status === "verified_safe"
      ? "border-ok/40 bg-ok/10 text-ok"
      : status === "failed_on_device" || status === "at_risk"
        ? "border-err/40 bg-err/10 text-err"
        : "border-warn/40 bg-warn/10 text-warn";
  const overrides = Object.entries(prefs.data ?? {}).filter(
    ([k, v]) =>
      v && typeof v === "object" && !Array.isArray(v) && "value" in (v as object) && k,
  ) as [string, { value: string; source: string; quote: string }][];
  const scenes = Object.values(complexity.data?.scenes ?? {});
  const gpuEst = scenes[0]?.complexity?.estimate?.gpu_ms_p95;
  const measured = verify.data?.performance;
  return (
    <div className="ptv-card space-y-2 p-3 text-sm" data-testid="quest-status">
      <div className="flex flex-wrap items-center gap-2">
        <a
          className={hasApk ? "ptv-button" : "ptv-button cursor-not-allowed opacity-40"}
          href={hasApk ? api.apkUrl(id) : undefined}
          aria-disabled={!hasApk}
          download
        >
          Download APK
        </a>
        {status && (
          <span className={"ptv-pill " + tone} title={(m?.reasons ?? []).join("\n")}>
            {MEMORY_LABEL[status] ?? status}
          </span>
        )}
        {(m?.reasons ?? []).slice(0, 2).map((r) => (
          <span key={r} className="text-xs text-muted">
            {r}
          </span>
        ))}
      </div>
      {(m?.compromises ?? []).length > 0 && (
        <div className="text-xs text-muted">
          Memory compromises: {(m?.compromises ?? []).join("; ")}
        </div>
      )}
      {overrides.length > 0 && (
        <div className="text-xs">
          <div className="text-warn">
            Custom Quest optimizations:{" "}
            {overrides.map(([k, v]) => `${k.replace("_", " ")} ${v.value}`).join(", ")}
          </div>
          {gpuEst !== undefined && (
            <div className="text-muted">
              Estimated p95 GPU {gpuEst.toFixed(1)} ms of the 13.9 ms budget
              {measured?.gpu !== undefined
                ? `; measured ${measured.gpu.toFixed(1)} ms at ${measured.fps ?? "?"} fps`
                : ""}
            </div>
          )}
        </div>
      )}
    </div>
  );
}

interface RequirementRow {
  id: string;
  kind: string;
  priority: string;
  statement: string;
  quote: string;
  outcome: "pass" | "fail" | "unverified";
  evidence: string[];
  source: string;
  claimed_by: { stage: string; item: string }[];
}

const OUTCOME_TONE: Record<string, string> = {
  pass: "border-ok/40 bg-ok/10 text-ok",
  fail: "border-err/40 bg-err/10 text-err",
  unverified: "border-white/10 bg-white/5 text-muted",
};

/** Every requirement the prompt states, with what the scene review and the
 *  headset actually saw (requirements_report.json). */
function RequirementsPanel({ id }: { id: string }) {
  const report = useQuery({
    queryKey: ["requirements_report", id],
    queryFn: () =>
      api.projectJson<{
        summary?: Record<string, number>;
        requirements?: RequirementRow[];
      }>(id, "requirements_report.json"),
    refetchInterval: 15_000,
  });
  const rows = report.data?.requirements ?? [];
  if (rows.length === 0) return null;
  const s = report.data?.summary ?? {};
  return (
    <details className="ptv-card p-3 text-sm" data-testid="requirements-report">
      <summary className="cursor-pointer">
        Prompt requirements: <span className="text-ok">{s.pass ?? 0} met</span>,{" "}
        <span className="text-err">{s.fail ?? 0} unmet</span>,{" "}
        <span className="text-muted">{s.unverified ?? 0} unverified</span>
        {(s.unclaimed ?? 0) > 0 && (
          <span className="text-warn">, {s.unclaimed} unclaimed by any stage</span>
        )}
      </summary>
      <ul className="mt-2 space-y-1">
        {rows.map((r) => (
          <li key={r.id} className="flex items-start gap-2 text-xs">
            <span className={"ptv-pill shrink-0 " + (OUTCOME_TONE[r.outcome] ?? "")}>
              {r.outcome}
            </span>
            <span className="min-w-0 flex-1">
              <span className="mono text-muted">{r.kind}</span> {r.statement}
              {r.evidence.length > 0 && (
                <span className="block text-muted" title={r.evidence.join("\n")}>
                  {r.source}: {r.evidence[0]}
                </span>
              )}
            </span>
          </li>
        ))}
      </ul>
    </details>
  );
}

/** Pick a stage: either resume from it (it and everything after re-runs) or
 *  run only it.
 *
 *  Grouped and cost-marked because the choice is a spending decision as much
 *  as a scheduling one -- resuming from reference images re-bills every image,
 *  every mesh, every rig and every clip downstream of it. */
function StageMenu({
  label,
  help,
  targetDevice,
  disabled,
  onPick,
}: {
  label: string;
  help: ReactNode;
  targetDevice: string;
  disabled: boolean;
  onPick: (stage: string) => void;
}) {
  const [open, setOpen] = useState(false);
  const ref = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (!open) return;
    const onDown = (e: MouseEvent) => {
      if (ref.current && !ref.current.contains(e.target as Node)) setOpen(false);
    };
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape") setOpen(false);
    };
    document.addEventListener("mousedown", onDown);
    document.addEventListener("keydown", onKey);
    return () => {
      document.removeEventListener("mousedown", onDown);
      document.removeEventListener("keydown", onKey);
    };
  }, [open]);

  const stages = stagesFor(targetDevice);

  return (
    <div className="relative" ref={ref}>
      <button
        className="ptv-button"
        aria-haspopup="menu"
        aria-expanded={open}
        disabled={disabled}
        onClick={() => setOpen((o) => !o)}
      >
        {label}
        <span aria-hidden className="text-xs">
          ▾
        </span>
      </button>
      {open && (
        <div
          role="menu"
          className="absolute right-0 z-30 mt-1 max-h-[70vh] w-[26rem] overflow-auto ptv-card p-1"
        >
          <div className="px-3 py-2 text-xs text-muted">{help}</div>
          {STAGE_GROUPS.map((group) => {
            const groupStages = stages.filter((s) => s.group === group.id);
            if (groupStages.length === 0) return null;
            return (
              <div key={group.id} className="mb-1">
                <div className="px-3 pt-2 pb-1 text-[0.65rem] font-medium uppercase tracking-wider text-muted">
                  {group.label}
                </div>
                {groupStages.map((s) => (
                  <button
                    key={s.id}
                    role="menuitem"
                    className="flex w-full items-center gap-2 rounded px-3 py-1.5 text-left text-sm hover:bg-white/5"
                    onClick={() => {
                      setOpen(false);
                      onPick(s.id);
                    }}
                  >
                    <span className="mono w-5 shrink-0 text-right text-xs text-muted">
                      {s.step}
                    </span>
                    <span className="min-w-0 flex-1 truncate">
                      {s.label}
                      <span className="mono ml-2 text-[0.65rem] text-muted">
                        {s.id}
                      </span>
                    </span>
                    {s.cost === "credits" && (
                      <span
                        className="ptv-pill shrink-0 border-warn/40 bg-warn/10 text-warn"
                        title={COST_HINTS.credits}
                      >
                        credits
                      </span>
                    )}
                  </button>
                ))}
              </div>
            );
          })}
        </div>
      )}
    </div>
  );
}

function ProviderHealth() {
  const health = useQuery({ queryKey: ["health"], queryFn: api.health });
  if (!health.data) return null;
  const h = health.data;
  const keys: [string, boolean][] = [
    ["OpenAI", h.has_openai_key],
    ["Google", h.has_google_key],
    ["Tripo", h.has_tripo_key],
    ["Uthana", Boolean(h.has_uthana_key)],
  ];
  const missing = keys.filter(([, ok]) => !ok);
  if (missing.length === 0) return null;
  // Surfaced on the project page, not just a settings screen: a missing Tripo
  // key does not fail until stage 7, by which point four LLM stages have
  // already been paid for.
  return (
    <div className="ptv-card border-warn/30 bg-warn/5 p-3 text-xs">
      <span className="text-warn">Missing provider keys: </span>
      <span className="text-muted">
        {missing.map(([n]) => n).join(", ")}. Stages that need them will fail or
        fall back.
      </span>
    </div>
  );
}

/** Events that mean a stage started or finished, i.e. its artifacts moved. */
const STAGE_STATUS_EVENTS = new Set(["running", "succeeded", "failed", "aborted", "skipped"]);

type Tab = "pipeline" | "prompt" | "artifacts" | "logs";

const TAB_LABELS: Record<Tab, string> = {
  pipeline: "Pipeline",
  prompt: "Prompt",
  artifacts: "Artifacts",
  logs: "Logs",
};

export default function ProjectDetailPage() {
  const { id = "" } = useParams();
  const qc = useQueryClient();
  const project = useQuery({
    queryKey: ["project", id],
    queryFn: () => api.getProject(id),
    refetchInterval: 4000,
  });
  const artifacts = useQuery({
    queryKey: ["artifacts", id],
    queryFn: () => api.listArtifacts(id),
    refetchInterval: 8000,
  });
  const [tab, setTab] = useState<Tab>("pipeline");
  const [events, setEvents] = useState<PipelineEvent[]>([]);
  const [notice, setNotice] = useState<string>("");

  useEffect(() => {
    if (!id) return;
    const src = eventStream(id);
    // The artifact list walks and hashes the whole project on the server.
    // Refreshing it on every log line kept files open while stages were
    // replacing them, so it refreshes only when a stage changes state, and
    // at most once per burst.
    let artifactsTimer: ReturnType<typeof setTimeout> | undefined;
    const refreshArtifactsSoon = () => {
      if (artifactsTimer) clearTimeout(artifactsTimer);
      artifactsTimer = setTimeout(() => {
        void qc.invalidateQueries({ queryKey: ["artifacts", id] });
      }, 1500);
    };
    const push = (e: MessageEvent) => {
      try {
        const data = JSON.parse(e.data) as PipelineEvent;
        setEvents((prev) => [...prev.slice(-399), data]);
        return data;
      } catch {
        /* a malformed frame must not break the stream */
        return undefined;
      }
    };
    src.onmessage = (e) => {
      const data = push(e);
      void qc.invalidateQueries({ queryKey: ["project", id] });
      if (data && STAGE_STATUS_EVENTS.has(data.status)) refreshArtifactsSoon();
    };
    src.addEventListener("pipeline", push as EventListener);
    return () => {
      if (artifactsTimer) clearTimeout(artifactsTimer);
      src.close();
    };
  }, [id, qc]);

  /** Every run button funnels through here so a failure is reported once,
   *  visibly, instead of being swallowed by a bare .catch. */
  const start = useCallback(
    (opts: { stage?: string; resume?: boolean; resume_from?: string }, what: string) => {
      setNotice(`Starting ${what}…`);
      api
        .run(id, opts)
        .then(() => {
          setNotice(`Started ${what}.`);
          void qc.invalidateQueries({ queryKey: ["project", id] });
        })
        .catch((e: unknown) => setNotice(`Could not start ${what}: ${String(e)}`));
    },
    [id, qc],
  );

  /** Stop whatever is running. Also the escape hatch for a project that
   *  only LOOKS busy because the server was killed mid-stage. */
  const abort = useCallback(() => {
    setNotice("Aborting…");
    api
      .abort(id)
      .then((r) => {
        setNotice(
          r.aborted
            ? "Abort requested. The stage stops at its next checkpoint."
            : r.reconciled
              ? "No run was in flight; cleared the stale running state."
              : "Nothing was running.",
        );
        void qc.invalidateQueries({ queryKey: ["project", id] });
      })
      .catch((e: unknown) => setNotice(`Could not abort: ${String(e)}`));
  }, [id, qc]);

  useEffect(() => {
    if (!notice) return;
    const t = setTimeout(() => setNotice(""), 6000);
    return () => clearTimeout(t);
  }, [notice]);

  if (project.isLoading) return <div className="text-muted">Loading…</div>;
  if (project.error)
    return <div className="text-err">{String(project.error)}</div>;
  if (!project.data) return null;
  const p = project.data;

  const isQuest = p.target_device === "quest";
  const canLaunch = isQuest
    ? Boolean(p.latest_build_apk)
    : Boolean(p.latest_build_exe);
  const launchLabel = isQuest ? "Install & launch on Quest" : "Launch build";
  const busy = p.status === "running";

  return (
    <div className="space-y-4">
      <div className="flex flex-wrap items-start justify-between gap-4">
        <div className="min-w-0">
          <h1 className="truncate text-xl font-semibold">{p.title}</h1>
          <div className="mt-1 flex flex-wrap items-center gap-2">
            <span className="mono text-xs text-muted">{p.project_id}</span>
            <span className="ptv-pill border-white/10 bg-white/5 text-muted">
              {isQuest ? "Quest (standalone APK)" : "PC VR (Quest Link)"}
            </span>
            <span className="ptv-pill border-white/10 bg-white/5 text-muted">
              {p.player_experience === "hands_only"
                ? "Hands Only"
                : "Full Player Avatar"}
            </span>
            <span
              className={
                "ptv-pill " +
                (busy
                  ? "border-accent/40 bg-accent/10 text-accent"
                  : p.status === "failed"
                    ? "border-err/40 bg-err/10 text-err"
                    : p.status === "aborted"
                      ? "border-warn/40 bg-warn/10 text-warn"
                      : "border-white/10 bg-white/5 text-muted")
              }
            >
              {busy ? `running · ${stageName(p.current_stage)}` : p.status}
            </span>
          </div>
        </div>

        <div className="flex flex-wrap items-center gap-2">
          <button
            className="ptv-button"
            disabled={busy}
            title={
              busy
                ? "A run is already in progress"
                : "Start this game over from the prompt. The previous run is moved to previous_runs/ and every stage runs fresh, so all asset generation is billed again."
            }
            onClick={() => start({}, "a full run")}
          >
            Run full pipeline
          </button>
          <button
            className="ptv-button"
            disabled={busy}
            title={
              busy
                ? "A run is already in progress"
                : "Continue from the first stage that has not succeeded."
            }
            onClick={() => start({ resume: true }, "a resume")}
          >
            Resume
          </button>
          <StageMenu
            label="Resume from…"
            help={
              <>
                Runs the chosen stage <strong className="text-white">and every
                stage after it</strong>. Anything before is skipped.
              </>
            }
            targetDevice={p.target_device}
            disabled={busy}
            onPick={(s) => start({ resume_from: s }, `resume from ${stageName(s)}`)}
          />
          <StageMenu
            label="Run only…"
            help={
              <>
                Runs <strong className="text-white">only the chosen stage</strong>,
                reusing everything else. Useful to repair one step (e.g. a
                single failed rig); follow it with Resume to rebuild what
                depends on it.
              </>
            }
            targetDevice={p.target_device}
            disabled={busy}
            onPick={(s) => start({ stage: s }, `${stageName(s)} only`)}
          />
          <button
            className={
              busy
                ? "ptv-button border-warn/40 text-warn"
                : "ptv-button cursor-not-allowed opacity-40"
            }
            disabled={!busy}
            title={
              busy
                ? "Stop the running stage now. Requests already sent to a provider cannot be recalled, but their artifacts are kept and a Resume will reuse them."
                : "Nothing is running"
            }
            onClick={abort}
          >
            Abort
          </button>
          <button
            className={
              canLaunch
                ? "ptv-button-primary"
                : "ptv-button-primary cursor-not-allowed opacity-40"
            }
            disabled={!canLaunch}
            title={
              canLaunch
                ? launchLabel
                : "No build yet. Run the pipeline through the build stage first."
            }
            onClick={() => api.launch(id).catch((e) => setNotice(String(e)))}
          >
            {launchLabel}
          </button>
          {isQuest && (
            <button
              className={
                canLaunch && !busy
                  ? "ptv-button"
                  : "ptv-button cursor-not-allowed opacity-40"
              }
              disabled={!canLaunch || busy}
              title="Run the APK on the connected headset and check the NPCs this game asked for appear, move and attack. Writes runtime_verification.json. Skipped if no headset is connected."
              onClick={() =>
                api
                  .verify(id)
                  .then(() => setNotice("Verifying on the headset; see the events log."))
                  .catch((e) => setNotice(String(e)))
              }
            >
              Verify on headset
            </button>
          )}
        </div>
      </div>

      {notice && (
        <div className="ptv-card border-accent/30 bg-accent/5 p-3 text-xs text-muted">
          {notice}
        </div>
      )}
      {isQuest && <QuestStatusPanel id={id} hasApk={Boolean(p.latest_build_apk)} />}
      <RequirementsPanel id={id} />
      <ProviderHealth />

      <div className="flex gap-1 border-b border-white/5">
        {(Object.keys(TAB_LABELS) as Tab[]).map((t) => (
          <button
            key={t}
            onClick={() => setTab(t)}
            className={
              "-mb-px border-b-2 px-3 py-2 text-sm transition-colors " +
              (tab === t
                ? "border-accent text-accent"
                : "border-transparent text-muted hover:text-white")
            }
          >
            {TAB_LABELS[t]}
            {t === "artifacts" && artifacts.data && artifacts.data.length > 0 && (
              <span className="mono ml-1.5 text-xs text-muted">
                {artifacts.data.length}
              </span>
            )}
          </button>
        ))}
      </div>

      {tab === "pipeline" && (
        <StageTimeline
          state={p}
          events={events}
          onRunStage={(s) => start({ stage: s }, `${stageName(s)} on its own`)}
        />
      )}
      {tab === "prompt" && (
        <pre className="ptv-card whitespace-pre-wrap p-4 mono text-sm">
          {p.prompt}
        </pre>
      )}
      {tab === "artifacts" && (
        <ArtifactBrowser projectId={id} artifacts={artifacts.data ?? []} />
      )}
      {tab === "logs" && <LogsViewer projectId={id} />}
    </div>
  );
}
