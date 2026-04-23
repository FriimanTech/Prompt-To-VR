import { useEffect, useState } from "react";
import { api } from "../api/client";

const NAMES = ["cmake", "tripo_models", "reference_images"];

export function LogsViewer({ projectId }: { projectId: string }) {
  const [name, setName] = useState<string>("cmake");
  const [text, setText] = useState<string>("");
  useEffect(() => {
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

  return (
    <div className="space-y-3">
      <div className="flex gap-2 text-sm">
        {NAMES.map((n) => (
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
            {n}
          </button>
        ))}
      </div>
      <pre className="ptv-card p-4 mono text-xs whitespace-pre-wrap max-h-[640px] overflow-y-auto">
        {text || "(log file empty or not created yet)"}
      </pre>
    </div>
  );
}
