import { Link } from "react-router-dom";
import { useQuery } from "@tanstack/react-query";
import { api } from "../api/client";
import { stageLabel, statusColor } from "../lib/stages";

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
            className="ptv-card p-4 flex justify-between hover:border-accent/40 transition"
          >
            <div>
              <div className="font-medium">{p.title}</div>
              <div className="text-xs text-muted mono mt-1">{p.project_id}</div>
            </div>
            <div className="text-right text-xs">
              <div className={statusColor(p.status)}>{p.status}</div>
              <div className="text-muted">{stageLabel(p.current_stage)}</div>
              <div className="text-muted mt-1">
                {new Date(p.updated_at).toLocaleString()}
              </div>
            </div>
          </Link>
        ))}
      </div>
    </div>
  );
}
