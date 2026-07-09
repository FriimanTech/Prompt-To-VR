import type { Artifact } from "../api/client";
import { fileUrl } from "../api/client";

/** Order kinds most-interesting-first so reference images and 3D models are at
 *  the top and build noise (binaries) sinks to the bottom. Unlisted kinds keep
 *  their natural order after these. */
const KIND_ORDER = [
  "image",
  "glb",
  "json",
  "cpp",
  "header",
  "log",
  "binary",
  "other",
];

const KIND_LABELS: Record<string, string> = {
  image: "Reference images",
  glb: "3D models (GLB)",
  json: "Structured artifacts (JSON)",
  cpp: "Generated C++ sources",
  header: "Generated C++ headers",
  log: "Logs",
  binary: "Binaries",
  other: "Other files",
};

export function ArtifactBrowser({
  projectId,
  artifacts,
}: {
  projectId: string;
  artifacts: Artifact[];
}) {
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

  const kinds = Object.keys(byKind).sort((a, b) => {
    const ia = KIND_ORDER.indexOf(a);
    const ib = KIND_ORDER.indexOf(b);
    if (ia === -1 && ib === -1) return a.localeCompare(b);
    if (ia === -1) return 1;
    if (ib === -1) return -1;
    return ia - ib;
  });

  return (
    <div className="space-y-4">
      {kinds.map((kind) => {
        const items = byKind[kind]
          .slice()
          .sort((a, b) => a.path.localeCompare(b.path));
        return (
          <div key={kind} className="ptv-card p-4">
            <div className="text-sm font-medium text-muted uppercase tracking-wide mb-3">
              {KIND_LABELS[kind] ?? kind} · {items.length}
            </div>
            {kind === "image" ? (
              <ImageGrid projectId={projectId} items={items} />
            ) : (
              <FileList projectId={projectId} items={items} />
            )}
          </div>
        );
      })}
    </div>
  );
}

function ImageGrid({
  projectId,
  items,
}: {
  projectId: string;
  items: Artifact[];
}) {
  return (
    <div className="grid grid-cols-2 sm:grid-cols-3 md:grid-cols-4 gap-3">
      {items.map((a) => {
        const url = fileUrl(projectId, a.path);
        return (
          <a
            key={a.path}
            href={url}
            target="_blank"
            rel="noreferrer"
            className="group block rounded-md overflow-hidden border border-white/10 hover:border-accent/60 transition-colors"
            title={a.path}
          >
            <div className="aspect-square bg-black/30 flex items-center justify-center overflow-hidden">
              <img
                src={url}
                alt={a.path}
                loading="lazy"
                className="w-full h-full object-cover group-hover:scale-105 transition-transform"
              />
            </div>
            <div className="p-2 text-xs">
              <div className="truncate mono">{assetLabel(a.path)}</div>
              <div className="text-muted">{humanSize(a.size_bytes)}</div>
              {a.description && (
                <div className="text-muted truncate" title={a.description}>
                  {a.description}
                </div>
              )}
            </div>
          </a>
        );
      })}
    </div>
  );
}

function FileList({
  projectId,
  items,
}: {
  projectId: string;
  items: Artifact[];
}) {
  return (
    <ul className="space-y-1 text-sm mono">
      {items.map((a) => (
        <li
          key={a.path}
          className="grid grid-cols-[1fr_auto_auto] gap-3 items-center"
        >
          <a
            href={fileUrl(projectId, a.path)}
            target="_blank"
            rel="noreferrer"
            className="truncate text-accent hover:underline"
            title={a.path}
          >
            {a.path}
          </a>
          <span className="text-muted">{humanSize(a.size_bytes)}</span>
          <span className="text-xs text-muted" title={a.sha256}>
            {a.sha256.slice(0, 10)}
          </span>
        </li>
      ))}
    </ul>
  );
}

/** Turn ``projects/<id>/images/<asset>/<file>.png`` into ``<asset>/<file>`` so
 *  the grid shows which asset each reference image belongs to. */
function assetLabel(path: string): string {
  const parts = path.split("/");
  const idx = parts.indexOf("images");
  if (idx >= 0 && idx < parts.length - 1) {
    return parts.slice(idx + 1).join("/");
  }
  return parts[parts.length - 1] ?? path;
}

function humanSize(n: number): string {
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
  if (n < 1024 * 1024 * 1024) return `${(n / 1024 / 1024).toFixed(1)} MB`;
  return `${(n / 1024 / 1024 / 1024).toFixed(2)} GB`;
}
