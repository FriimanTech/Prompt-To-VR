import { Link } from "react-router-dom";
import { useQuery } from "@tanstack/react-query";
import { api } from "../api/client";
import {
  STAGE_META,
  stageLabel,
  stagesFor,
  statusColor,
  statusTone,
} from "../lib/stages";

/** How far along a project is, from its current stage alone.
 *
 *  The list endpoint returns a summary without the per-stage records, so this
 *  is a position in the run rather than a count of completed stages. Close
 *  enough for a glance, and it costs no extra request per card. */
function progressFraction(currentStage: string): number {
  const meta = STAGE_META[currentStage];
  if (!meta) return currentStage === "created" ? 0 : 1;
  // Device is unknown here; the Quest tail is one stage longer, so clamp.
  return Math.min(1, meta.step / stagesFor("pcvr").length);
}

export default function ProjectListPage() {
  const { data, isLoading, error, refetch } = useQuery({
    queryKey: ["projects"],
    queryFn: api.listProjects,
    refetchInterval: 5_000,
  });

  return (
    <div className="space-y-4">
      <div className="flex items-center justify-between">
        <h1 className="text-xl font-semibold">Projects</h1>
        <div className="flex gap-2">
          <button className="ptv-button" onClick={() => refetch()}>
            Refresh
          </button>
          <Link to="/projects/new" className="ptv-button-primary">
            + New project
          </Link>
        </div>
      </div>
      {isLoading && <div className="text-muted">Loading…</div>}
      {error && (
        <div className="ptv-card p-4 text-err">
          Orchestrator unreachable. Start it with
          {" "}
          <code className="mono">uvicorn ptv_orchestrator.main:app</code>.
        </div>
      )}
      {data && data.length === 0 && (
        <div className="ptv-card p-6 text-muted">
          No projects yet. Create one to generate a native VR build from a prompt.
        </div>
      )}
      <div className="grid gap-3">
        {data?.map((p) => (
          <Link
            to={`/projects/${p.project_id}`}
            key={p.project_id}
            className="ptv-card p-4 transition hover:border-accent/40"
          >
            <div className="flex justify-between gap-4">
              <div className="min-w-0">
                <div className="truncate font-medium">{p.title}</div>
                <div className="mono mt-1 text-xs text-muted">
                  {p.project_id}
                </div>
              </div>
              <div className="shrink-0 text-right text-xs">
                <div className={statusColor(p.status)}>{p.status}</div>
                <div className="text-muted">{stageLabel(p.current_stage)}</div>
                <div className="mt-1 text-muted">
                  {new Date(p.updated_at).toLocaleString()}
                </div>
              </div>
            </div>
            <div className="mt-3 h-1 w-full overflow-hidden rounded-full bg-white/5">
              <div
                className={
                  "h-full rounded-full transition-all " +
                  (statusTone(p.status) === "err"
                    ? "bg-err"
                    : statusTone(p.status) === "busy"
                      ? "bg-accent"
                      : "bg-ok")
                }
                style={{
                  width: `${Math.round(progressFraction(p.current_stage) * 100)}%`,
                }}
              />
            </div>
          </Link>
        ))}
      </div>
    </div>
  );
}
