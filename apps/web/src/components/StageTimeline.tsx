import { useMemo, useState } from "react";
import type { ProjectState, StageRecord } from "../api/client";
import {
  COST_HINTS,
  COST_LABELS,
  STAGE_GROUPS,
  formatDuration,
  stageCost,
  stagesFor,
  statusColor,
  statusTone,
  type StageMeta,
} from "../lib/stages";

export type PipelineEvent = {
  stage: string;
  status: string;
  message: string;
  at: string;
};

/** What the timeline shows for one stage, after reconciling the stored record
 *  against whether a run is currently in flight. */
type Resolved = {
  meta: StageMeta;
  status: string;
  message: string;
  duration: string;
  /** Will run in this pass, but has not yet. */
  queued: boolean;
  artifacts: number;
};

function CostBadge({ cost }: { cost: ReturnType<typeof stageCost> }) {
  const tone =
    cost === "credits"
      ? "border-warn/40 text-warn bg-warn/10"
      : cost === "llm"
        ? "border-accent2/40 text-accent2 bg-accent2/10"
        : "border-white/10 text-muted bg-white/5";
  return (
    <span className={"ptv-pill " + tone} title={COST_HINTS[cost]}>
      {COST_LABELS[cost]}
    </span>
  );
}

function StatusDot({ status }: { status: string }) {
  const tone = statusTone(status);
  const base = "inline-block h-2 w-2 rounded-full shrink-0";
  if (tone === "busy") {
    // Only the running stage animates. A timeline where everything pulses
    // tells you nothing about where the pipeline actually is.
    return <span className={base + " bg-accent animate-pulse"} />;
  }
  const color =
    tone === "ok"
      ? "bg-ok"
      : tone === "warn"
        ? "bg-warn"
        : tone === "err"
          ? "bg-err"
          : "bg-white/20";
  return <span className={base + " " + color} />;
}

function StageRow({
  row,
  onRun,
  busy,
}: {
  row: Resolved;
  onRun: (stage: string) => void;
  busy: boolean;
}) {
  const { meta, status, message, duration, queued, artifacts } = row;
  return (
    <li className="group grid grid-cols-[auto_1fr_auto] items-start gap-3 border-b border-white/5 px-3 py-2.5 last:border-b-0 hover:bg-white/[0.02]">
      <div className="flex items-center gap-2.5 pt-0.5">
        <StatusDot status={status} />
        <span className="mono w-5 text-right text-xs text-muted">
          {meta.step}
        </span>
      </div>

      <div className="min-w-0">
        <div className="flex flex-wrap items-center gap-2">
          <span className="text-sm font-medium">{meta.label}</span>
          <span className="mono text-[0.65rem] text-muted/80" title="Backend stage id">
            {meta.id}
          </span>
          <CostBadge cost={meta.cost} />
          {duration && (
            <span className="mono text-xs text-muted">{duration}</span>
          )}
          {artifacts > 0 && (
            <span className="mono text-xs text-muted">
              {artifacts} artifact{artifacts === 1 ? "" : "s"}
            </span>
          )}
        </div>
        {/* The stage's own message when it has one, otherwise what it does.
            Hidden while queued: showing the previous run's result next to a
            stage that is about to re-run reads as though it already finished. */}
        <div className="mt-0.5 text-xs text-muted">
          {!queued && message ? message : meta.blurb}
        </div>
      </div>

      <div className="flex items-center gap-2 pt-0.5">
        <span className={"mono text-xs " + statusColor(status)}>{status}</span>
        {/* Re-running one stage in isolation is for iterating on it -- the
            downstream stages are deliberately NOT run, so the build on disk
            stays stale until you resume. */}
        <button
          className="ptv-icon-button opacity-0 transition-opacity group-hover:opacity-100 focus:opacity-100 disabled:cursor-not-allowed disabled:opacity-30"
          disabled={busy}
          title={
            busy
              ? "A run is already in progress"
              : `Run only ${meta.label} (does not run the stages after it)`
          }
          onClick={() => onRun(meta.id)}
        >
          Run
        </button>
      </div>
    </li>
  );
}

export function StageTimeline({
  state,
  events,
  onRunStage,
}: {
  state: ProjectState;
  events: PipelineEvent[];
  onRunStage: (stage: string) => void;
}) {
  const [onlyProblems, setOnlyProblems] = useState(false);

  const rows = useMemo<Resolved[]>(() => {
    const latest = new Map<string, StageRecord>();
    for (const s of state.stages) latest.set(s.stage, s); // appended in order

    const ordered = stagesFor(state.target_device);
    const runActive = state.status === "running";
    const runningIndex = runActive
      ? ordered.findIndex((s) => s.id === state.current_stage)
      : -1;

    return ordered.map((meta, idx) => {
      const rec = latest.get(meta.id);
      // A run always proceeds to the end, so anything after the current stage
      // WILL run in this pass. Showing it with the status left over from a
      // previous run is actively misleading: a resume from world_generation
      // would show scene_compile as "succeeded" before it re-ran.
      const queued = runActive && runningIndex >= 0 && idx > runningIndex;
      const status = queued
        ? "queued"
        : (rec?.status ?? (state.current_stage === meta.id ? "running" : "pending"));
      return {
        meta,
        status,
        message: rec?.message ?? "",
        duration: queued ? "" : formatDuration(rec?.started_at ?? null, rec?.ended_at ?? null),
        queued,
        artifacts: rec?.artifacts?.length ?? 0,
      };
    });
  }, [state]);

  const done = rows.filter((r) => r.status === "succeeded").length;
  const failed = rows.find((r) => statusTone(r.status) === "err");
  const busy = state.status === "running";

  const shownEvents = onlyProblems
    ? events.filter((e) => {
        const tone = statusTone(e.status);
        return tone === "warn" || tone === "err";
      })
    : events;

  return (
    <div className="space-y-4">
      <div className="ptv-card p-4">
        <div className="mb-3 flex flex-wrap items-center justify-between gap-3">
          <div>
            <div className="text-sm font-medium">
              {done} of {rows.length} stages complete
            </div>
            <div className="text-xs text-muted">
              {busy
                ? `Running ${state.current_stage.replace(/_/g, " ")}…`
                : failed
                  ? `Stopped at ${failed.meta.label}. Fix the cause, then Resume.`
                  : done === rows.length
                    ? "Pipeline complete."
                    : "Idle. Run the full pipeline, or resume from a stage."}
            </div>
          </div>
          <div className="mono text-xs text-muted">
            {Math.round((done / rows.length) * 100)}%
          </div>
        </div>
        <div className="h-1.5 w-full overflow-hidden rounded-full bg-white/5">
          <div
            className={
              "h-full rounded-full transition-all duration-500 " +
              (failed ? "bg-err" : done === rows.length ? "bg-ok" : "bg-accent")
            }
            style={{ width: `${(done / rows.length) * 100}%` }}
          />
        </div>
        {/* Compact id strip so every backend stage is visible even when a
            group is scrolled off-screen. Matches orchestrator order. */}
        <ol className="mt-3 flex flex-wrap gap-1.5">
          {rows.map((r) => {
            const tone = statusTone(r.status);
            const chip =
              tone === "ok"
                ? "border-ok/40 bg-ok/10 text-ok"
                : tone === "busy"
                  ? "border-accent/40 bg-accent/10 text-accent"
                  : tone === "err"
                    ? "border-err/40 bg-err/10 text-err"
                    : tone === "warn"
                      ? "border-warn/40 bg-warn/10 text-warn"
                      : "border-white/10 bg-white/5 text-muted";
            return (
              <li
                key={r.meta.id}
                className={"ptv-pill mono text-[0.65rem] " + chip}
                title={`${r.meta.step}. ${r.meta.label} (${r.meta.id}) · ${r.status}`}
              >
                {r.meta.step}.{r.meta.id}
              </li>
            );
          })}
        </ol>
      </div>

      <div className="grid gap-4 xl:grid-cols-[minmax(0,1.15fr)_minmax(0,1fr)]">
        <div className="space-y-4">
          {STAGE_GROUPS.map((group) => {
            const groupRows = rows.filter((r) => r.meta.group === group.id);
            if (groupRows.length === 0) return null;
            const groupDone = groupRows.filter(
              (r) => r.status === "succeeded",
            ).length;
            return (
              <section key={group.id} className="ptv-card overflow-hidden">
                <header className="flex items-start justify-between gap-3 border-b border-white/5 px-3 py-2.5">
                  <div>
                    <h3 className="text-sm font-medium">{group.label}</h3>
                    <p className="text-xs text-muted">{group.blurb}</p>
                  </div>
                  <span className="mono shrink-0 text-xs text-muted">
                    {groupDone}/{groupRows.length}
                  </span>
                </header>
                <ol>
                  {groupRows.map((row) => (
                    <StageRow
                      key={row.meta.id}
                      row={row}
                      onRun={onRunStage}
                      busy={busy}
                    />
                  ))}
                </ol>
              </section>
            );
          })}
        </div>

        <div className="ptv-card flex max-h-[70vh] flex-col overflow-hidden">
          <header className="flex items-center justify-between gap-3 border-b border-white/5 px-3 py-2.5">
            <h3 className="text-sm font-medium">Live events</h3>
            <div className="flex items-center gap-3">
              <label className="flex cursor-pointer items-center gap-1.5 text-xs text-muted">
                <input
                  type="checkbox"
                  className="accent-accent"
                  checked={onlyProblems}
                  onChange={(e) => setOnlyProblems(e.target.checked)}
                />
                Problems only
              </label>
              <span className="mono text-xs text-muted">
                {shownEvents.length}
              </span>
            </div>
          </header>
          <div className="flex-1 space-y-0.5 overflow-y-auto p-3 mono text-xs">
            {shownEvents.length === 0 && (
              <div className="text-muted">
                {events.length === 0
                  ? "No events yet. Press Run full pipeline."
                  : "No warnings or errors."}
              </div>
            )}
            {/* Newest first: during a long run the interesting line is the one
                that just arrived, and scroll-chasing a growing list is worse
                than reading a stable top. */}
            {[...shownEvents].reverse().map((e, i) => (
              <div
                key={shownEvents.length - i}
                className="flex items-start gap-2.5 rounded px-1 py-0.5 hover:bg-white/[0.03]"
              >
                <span className="shrink-0 text-muted">
                  {new Date(e.at).toLocaleTimeString()}
                </span>
                <span className={"w-16 shrink-0 " + statusColor(e.status)}>
                  {e.status}
                </span>
                <span className="w-32 shrink-0 truncate text-accent2">
                  {e.stage}
                </span>
                <span className="min-w-0 break-words opacity-90">
                  {e.message}
                </span>
              </div>
            ))}
          </div>
        </div>
      </div>
    </div>
  );
}
