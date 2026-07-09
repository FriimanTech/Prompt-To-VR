const BASE = "/api";

async function req<T>(path: string, init?: RequestInit): Promise<T> {
  const res = await fetch(BASE + path, {
    ...init,
    headers: {
      "Content-Type": "application/json",
      ...(init?.headers ?? {}),
    },
  });
  if (!res.ok) {
    const text = await res.text().catch(() => "");
    throw new Error(`${res.status} ${res.statusText}: ${text}`);
  }
  if (res.status === 204) return undefined as T;
  return (await res.json()) as T;
}

export type ProjectSummary = {
  project_id: string;
  title: string;
  current_stage: string;
  status: string;
  updated_at: string;
};

export type StageRecord = {
  stage: string;
  status: string;
  started_at: string | null;
  ended_at: string | null;
  message: string;
  artifacts: string[];
};

export type ProjectState = {
  project_id: string;
  title: string;
  prompt: string;
  created_at: string;
  updated_at: string;
  current_stage: string;
  status: string;
  stages: StageRecord[];
  latest_build_exe: string | null;
  latest_build_dir: string | null;
  latest_scene_json: string | null;
};

export type Artifact = {
  id: string;
  kind: string;
  path: string;
  size_bytes: number;
  sha256: string;
  stage: string;
  created_at: string;
  description: string | null;
};

export type Health = {
  ok: boolean;
  has_anthropic_key: boolean;
  has_openai_key: boolean;
  has_google_key: boolean;
  has_tripo_key: boolean;
  cmake_generator: string;
  vcpkg_root: string | null;
};

export const api = {
  health: () => req<Health>("/health"),
  listProjects: () => req<ProjectSummary[]>("/projects"),
  getProject: (id: string) => req<ProjectState>(`/projects/${id}`),
  createProject: (title: string, prompt: string) =>
    req<{ project_id: string }>("/projects", {
      method: "POST",
      body: JSON.stringify({ title, prompt }),
    }),
  listArtifacts: (id: string) => req<Artifact[]>(`/projects/${id}/artifacts`),
  listLogs: (id: string) => req<string[]>(`/projects/${id}/logs`),
  getLog: async (id: string, name: string) => {
    const res = await fetch(BASE + `/projects/${id}/logs/${name}`);
    if (!res.ok) return "";
    return res.text();
  },
  run: (id: string, opts?: { stage?: string; resume?: boolean }) =>
    req<{ ok: boolean; mode: string }>(`/projects/${id}/run`, {
      method: "POST",
      body: JSON.stringify(opts ?? {}),
    }),
  launch: (id: string, scene_json?: string) =>
    req<{ ok: boolean; exe: string }>(`/projects/${id}/launch`, {
      method: "POST",
      body: JSON.stringify({ scene_json: scene_json ?? null }),
    }),
  launchSample: () =>
    req<{ ok: boolean; exe: string }>("/sample/launch", { method: "POST" }),
};

export function eventStream(projectId: string): EventSource {
  return new EventSource(`${BASE}/projects/${projectId}/events`);
}

/** URL for the raw bytes of an artifact (image preview, download, ...). The
 *  `path` is the artifact's repo-relative path from the /artifacts endpoint. */
export function fileUrl(projectId: string, path: string): string {
  return `${BASE}/projects/${projectId}/file?path=${encodeURIComponent(path)}`;
}
