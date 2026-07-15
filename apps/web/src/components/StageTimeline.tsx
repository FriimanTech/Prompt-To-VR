import type { ProjectState, StageRecord } from "../api/client";
import { STAGE_ORDER, stageLabel, statusColor } from "../lib/stages";

export function StageTimeline({
  state,
  events,
}: {
  state: ProjectState;
  events: { stage: string; status: string; message: string; at: string }[];
}) {
  const byStage = new Map<string, StageRecord[]>();
  for (const s of state.stages) {
    const arr = byStage.get(s.stage) ?? [];
    arr.push(s);
    byStage.set(s.stage, arr);
  }

  const visible: string[] = STAGE_ORDER.filter(
    (s) => s !== "created" && s !== "failed",
  );

  // While a run is in progress the pipeline always proceeds through to the end,
  // so any stage AFTER the currently-running one will run in this pass. Show
  // those as "queued" instead of the stale status left over from a previous run
  // (e.g. a resume-from-world-generation shouldn't show scene_compile as
  // "succeeded" until it has actually re-run).
  const runActive = state.status === "running";
  const runningIndex = runActive ? visible.indexOf(state.current_stage) : -1;

  return (
    <div className="grid md:grid-cols-[1fr_1fr] gap-4">
      <div className="ptv-card p-4">
        <h2 className="text-sm font-medium text-muted mb-3 uppercase tracking-wide">
          Pipeline stages
        </h2>
        <ol className="space-y-0">
          {visible.map((id, idx) => {
            const records = byStage.get(id) ?? [];
            const latest = records[records.length - 1];
            const queued = runActive && runningIndex >= 0 && idx > runningIndex;
            const status = queued
              ? "queued"
              : latest?.status ?? (state.current_stage === id ? "running" : "pending");
            return (
              <li key={id} className="stage-row">
                <div>
                  <div className="text-sm">{stageLabel(id)}</div>
                  {/* Hide the stale prior-run message on queued stages. */}
                  {!queued && latest?.message && (
                    <div className="text-xs text-muted">{latest.message}</div>
                  )}
                </div>
                <span className={"text-xs mono " + statusColor(status)}>
                  {status}
                </span>
              </li>
            );
          })}
        </ol>
      </div>
      <div className="ptv-card p-4">
        <h2 className="text-sm font-medium text-muted mb-3 uppercase tracking-wide">
          Live events
        </h2>
        <div className="max-h-[520px] overflow-y-auto space-y-1 font-mono text-xs">
          {events.length === 0 && (
            <div className="text-muted">
              Waiting for pipeline events… press <em>Run full pipeline</em>.
            </div>
          )}
          {events.map((e, i) => (
            <div key={i} className="flex gap-3 items-start">
              <span className="text-muted shrink-0">
                {new Date(e.at).toLocaleTimeString()}
              </span>
              <span className={"shrink-0 w-16 " + statusColor(e.status)}>
                {e.status}
              </span>
              <span className="shrink-0 w-36 text-accent2">{e.stage}</span>
              <span className="opacity-90">{e.message}</span>
            </div>
          ))}
        </div>
      </div>
    </div>
  );
}
