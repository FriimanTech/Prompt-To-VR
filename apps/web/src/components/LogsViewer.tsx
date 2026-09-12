import { useEffect, useMemo, useState } from "react";
import { api } from "../api/client";

/** Human labels for the known log files. Unknown logs fall back to their raw
 *  name so nothing is ever hidden from the operator. */
const LOG_LABELS: Record<string, string> = {
  scene_spec: "SceneSpec",
  reference_images: "Reference images",
  tripo_models: "Tripo 3D",
  world: "World generation",
  codegen_verify: "Codegen verify",
  cmake: "CMake (configure + build)",
  gradle: "Quest Gradle / NDK",
};

/** Order we prefer to surface logs in; anything not listed is appended. */
const PREFERRED_ORDER = [
  "scene_spec",
  "reference_images",
  "tripo_models",
  "world",
  "codegen_verify",
  "cmake",
  "gradle",
];

function logLabel(name: string): string {
  return LOG_LABELS[name] ?? name;
}

function sortLogs(names: string[]): string[] {
  return [...names].sort((a, b) => {
    const ia = PREFERRED_ORDER.indexOf(a);
    const ib = PREFERRED_ORDER.indexOf(b);
    if (ia === -1 && ib === -1) return a.localeCompare(b);
    if (ia === -1) return 1;
    if (ib === -1) return -1;
    return ia - ib;
  });
}

export function LogsViewer({ projectId }: { projectId: string }) {
  const [available, setAvailable] = useState<string[]>([]);
  const [name, setName] = useState<string>("");
  const [text, setText] = useState<string>("");

  // Discover which log files actually exist for this project (refreshed while
  // the pipeline runs so new logs appear without a manual reload).
  useEffect(() => {
    let alive = true;
    const tick = async () => {
      const names = await api.listLogs(projectId).catch(() => []);
      if (alive) setAvailable(sortLogs(names));
    };
    void tick();
    const id = setInterval(tick, 5_000);
    return () => {
      alive = false;
      clearInterval(id);
    };
  }, [projectId]);

  // Pick a sensible default selection once logs are known.
  useEffect(() => {
    if (available.length === 0) return;
    if (!name || !available.includes(name)) setName(available[0]);
  }, [available, name]);

  // Poll the selected log's contents.
  useEffect(() => {
    if (!name) {
      setText("");
      return;
    }
    let alive = true;
    const tick = async () => {
      const t = await api.getLog(projectId, name).catch(() => "");
      if (alive) setText(t);
    };
    void tick();
    const id = setInterval(tick, 3_000);
    return () => {
      alive = false;
      clearInterval(id);
    };
  }, [projectId, name]);

  const lineCount = useMemo(
    () => (text ? text.replace(/\n$/, "").split("\n").length : 0),
    [text],
  );

  if (available.length === 0) {
    return (
      <div className="ptv-card p-4 text-muted">
        No logs yet. Logs appear as stages run (SceneSpec, reference images,
        Tripo, world generation, codegen verify, CMake / Gradle).
      </div>
    );
  }

  return (
    <div className="space-y-3">
      <div className="flex flex-wrap gap-2 text-sm">
        {available.map((n) => (
          <button
            key={n}
            onClick={() => setName(n)}
            className={
              "px-3 py-1 rounded-md border " +
              (n === name
                ? "border-accent/60 text-accent bg-accent/10"
                : "border-white/10 text-muted hover:text-white")
            }
          >
            {logLabel(n)}
          </button>
        ))}
      </div>
      {name && (
        <div className="flex items-center justify-between text-xs text-muted">
          <span className="mono">{name}.log</span>
          <span>{lineCount ? `${lineCount} line(s)` : "empty"}</span>
        </div>
      )}
      <pre className="ptv-card p-4 mono text-xs whitespace-pre-wrap max-h-[640px] overflow-y-auto">
        {text || "(log file empty or not created yet)"}
      </pre>
    </div>
  );
}
