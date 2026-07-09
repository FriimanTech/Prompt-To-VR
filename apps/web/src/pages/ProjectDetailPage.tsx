import { useEffect, useState } from "react";
import { useParams } from "react-router-dom";
import { useQuery, useQueryClient } from "@tanstack/react-query";
import { api, eventStream } from "../api/client";
import { StageTimeline } from "../components/StageTimeline";
import { ArtifactBrowser } from "../components/ArtifactBrowser";
import { LogsViewer } from "../components/LogsViewer";

type Tab = "pipeline" | "prompt" | "artifacts" | "logs";

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
  const [events, setEvents] = useState<{ stage: string; status: string; message: string; at: string }[]>([]);

  useEffect(() => {
    if (!id) return;
    const src = eventStream(id);
    src.onmessage = (e) => {
      try {
        const data = JSON.parse(e.data);
        setEvents((prev) => [...prev.slice(-199), data]);
        void qc.invalidateQueries({ queryKey: ["project", id] });
        void qc.invalidateQueries({ queryKey: ["artifacts", id] });
      } catch {
        /* ignore */
      }
    };
    src.addEventListener("pipeline", (e: MessageEvent) => {
      try {
        const data = JSON.parse(e.data);
        setEvents((prev) => [...prev.slice(-199), data]);
      } catch { /* */ }
    });
    return () => src.close();
  }, [id, qc]);

  if (project.isLoading) return <div className="text-muted">Loading…</div>;
  if (project.error) return <div className="text-err">{String(project.error)}</div>;
  if (!project.data) return null;
  const p = project.data;

  const canLaunch = Boolean(p.latest_build_exe);

  return (
    <div className="space-y-4">
      <div className="flex items-start justify-between">
        <div>
          <h1 className="text-xl font-semibold">{p.title}</h1>
          <div className="text-xs text-muted mono">{p.project_id}</div>
        </div>
        <div className="flex flex-wrap gap-2">
          <button className="ptv-button" onClick={() => api.run(id).catch(() => void 0)}>
            Run full pipeline
          </button>
          <button className="ptv-button" onClick={() => api.run(id, { resume: true }).catch(() => void 0)}>
            Resume
          </button>
          <button
            className={canLaunch ? "ptv-button-primary" : "ptv-button-primary opacity-40 cursor-not-allowed"}
            disabled={!canLaunch}
            onClick={() => api.launch(id).catch(() => void 0)}
          >
            Launch latest build
          </button>
        </div>
      </div>

      <div className="flex gap-2 border-b border-white/5">
        {(["pipeline", "prompt", "artifacts", "logs"] as Tab[]).map((t) => (
          <button
            key={t}
            onClick={() => setTab(t)}
            className={
              "px-3 py-2 text-sm border-b-2 -mb-px " +
              (tab === t
                ? "border-accent text-accent"
                : "border-transparent text-muted hover:text-white")
            }
          >
            {t[0].toUpperCase() + t.slice(1)}
          </button>
        ))}
      </div>

      {tab === "pipeline" && (
        <StageTimeline state={p} events={events} />
      )}
      {tab === "prompt" && (
        <pre className="ptv-card p-4 mono text-sm whitespace-pre-wrap">
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
