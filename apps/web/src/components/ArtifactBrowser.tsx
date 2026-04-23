import type { Artifact } from "../api/client";

export function ArtifactBrowser({ artifacts }: { artifacts: Artifact[] }) {
  if (artifacts.length === 0) {
    return (
      <div className="ptv-card p-4 text-muted">
        No artifacts yet. Run the pipeline.
      </div>
    );
  }

  const byKind = artifacts.reduce<Record<string, Artifact[]>>((acc, a) => {
    (acc[a.kind] ??= []).push(a);
    return acc;
  }, {});

  return (
    <div className="space-y-4">
      {Object.entries(byKind).map(([kind, items]) => (
        <div key={kind} className="ptv-card p-4">
          <div className="text-sm font-medium text-muted uppercase tracking-wide mb-2">
            {kind} · {items.length}
          </div>
          <ul className="space-y-1 text-sm mono">
            {items.map((a) => (
              <li
                key={a.path}
                className="grid grid-cols-[1fr_auto_auto] gap-3 items-center"
              >
                <span className="truncate">{a.path}</span>
                <span className="text-muted">{humanSize(a.size_bytes)}</span>
                <span className="text-xs text-muted" title={a.sha256}>
                  {a.sha256.slice(0, 10)}
                </span>
              </li>
            ))}
          </ul>
        </div>
      ))}
    </div>
  );
}

function humanSize(n: number): string {
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
  if (n < 1024 * 1024 * 1024) return `${(n / 1024 / 1024).toFixed(1)} MB`;
  return `${(n / 1024 / 1024 / 1024).toFixed(2)} GB`;
}
