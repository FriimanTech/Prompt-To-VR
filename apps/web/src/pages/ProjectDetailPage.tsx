import { useCallback, useEffect, useRef, useState } from "react";
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

/** Pick a stage to resume from: that stage and everything after it re-runs.
 *
 *  Grouped and cost-marked because the choice is a spending decision as much
 *  as a scheduling one -- resuming from reference images re-bills every image,
 *  every mesh, every rig and every clip downstream of it. */
function ResumeFromMenu({
  targetDevice,
  disabled,
  onPick,
}: {
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
        Resume from…
        <span aria-hidden className="text-xs">
          ▾
        </span>
      </button>
      {open && (
        <div
          role="menu"
          className="absolute right-0 z-30 mt-1 max-h-[70vh] w-[26rem] overflow-auto ptv-card p-1"
        >
          <div className="px-3 py-2 text-xs text-muted">
            Runs the chosen stage <strong className="text-white">and every
            stage after it</strong>. Anything before is skipped.
          </div>
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
                    <span className="min-w-0 flex-1 truncate">{s.label}</span>
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
    ["Anthropic", h.has_anthropic_key],
    ["OpenAI", h.has_openai_key],
    ["Google", h.has_google_key],
    ["Tripo", h.has_tripo_key],
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
    const push = (e: MessageEvent) => {
      try {
        const data = JSON.parse(e.data) as PipelineEvent;
        setEvents((prev) => [...prev.slice(-399), data]);
      } catch {
        /* a malformed frame must not break the stream */
      }
    };
    src.onmessage = (e) => {
      push(e);
      void qc.invalidateQueries({ queryKey: ["project", id] });
      void qc.invalidateQueries({ queryKey: ["artifacts", id] });
    };
    src.addEventListener("pipeline", push as EventListener);
    return () => src.close();
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
            <span
              className={
                "ptv-pill " +
                (busy
                  ? "border-accent/40 bg-accent/10 text-accent"
                  : p.status === "failed"
                    ? "border-err/40 bg-err/10 text-err"
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
                : "Run every stage from the beginning. Re-bills all asset generation."
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
          <ResumeFromMenu
            targetDevice={p.target_device}
            disabled={busy}
            onPick={(s) => start({ resume_from: s }, `resume from ${stageName(s)}`)}
          />
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
        </div>
      </div>

      {notice && (
        <div className="ptv-card border-accent/30 bg-accent/5 p-3 text-xs text-muted">
          {notice}
        </div>
      )}
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
