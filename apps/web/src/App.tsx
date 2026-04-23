import { Link, Outlet, NavLink } from "react-router-dom";
import { useQuery } from "@tanstack/react-query";
import { api } from "./api/client";

export default function App() {
  const health = useQuery({
    queryKey: ["health"],
    queryFn: api.health,
    refetchInterval: 10_000,
  });
  const keys = health.data
    ? {
        anthropic: health.data.has_anthropic_key,
        openai: health.data.has_openai_key,
        google: health.data.has_google_key,
        tripo: health.data.has_tripo_key,
      }
    : { anthropic: false, openai: false, google: false, tripo: false };

  return (
    <div className="min-h-full">
      <header className="border-b border-white/5 bg-panel">
        <div className="mx-auto max-w-6xl px-6 py-3 flex items-center justify-between">
          <Link to="/" className="text-lg font-semibold tracking-tight">
            <span className="text-accent">Prompt</span>
            <span className="text-muted">›</span>
            <span className="text-accent2">Native VR</span>
          </Link>
          <nav className="flex items-center gap-3 text-sm">
            <NavLink
              to="/projects"
              className={({ isActive }) =>
                "px-3 py-1 rounded-md " +
                (isActive ? "bg-accent/20 text-accent" : "text-muted hover:text-white")
              }
            >
              Projects
            </NavLink>
            <NavLink
              to="/projects/new"
              className={({ isActive }) =>
                "px-3 py-1 rounded-md " +
                (isActive ? "bg-accent/20 text-accent" : "text-muted hover:text-white")
              }
            >
              New
            </NavLink>
            <button
              onClick={() => void api.launchSample().catch(() => void 0)}
              className="ptv-button"
              title="Launch the hand-authored starter-room sample"
            >
              Launch starter sample
            </button>
            <KeyPill label="ANTHROPIC" ok={keys.anthropic} />
            <KeyPill label="OPENAI" ok={keys.openai} optional />
            <KeyPill label="GOOGLE" ok={keys.google} />
            <KeyPill label="TRIPO" ok={keys.tripo} />
          </nav>
        </div>
      </header>
      <main className="mx-auto max-w-6xl px-6 py-6">
        <Outlet />
      </main>
      <footer className="border-t border-white/5 mt-10">
        <div className="mx-auto max-w-6xl px-6 py-4 text-xs text-muted flex justify-between">
          <span>
            Native C++ Vulkan + OpenXR · Local-first · Claude / Imagen / Tripo
          </span>
          <span>
            {health.data?.cmake_generator ?? "cmake generator: unknown"}
          </span>
        </div>
      </footer>
    </div>
  );
}

function KeyPill({
  label,
  ok,
  optional = false,
}: {
  label: string;
  ok: boolean;
  optional?: boolean;
}) {
  // Optional keys (e.g. OPENAI fallback) show neutral styling when missing.
  const missingClass = optional
    ? "border-white/10 text-muted bg-panelmuted"
    : "border-err/40 text-err bg-err/10";
  const title = optional
    ? ok
      ? `${label} key set in .env (used as a fallback)`
      : `${label} key is not set (optional fallback disabled)`
    : ok
      ? `${label} key is set in .env`
      : `${label} key is missing from .env`;
  return (
    <span
      className={
        "ptv-pill " +
        (ok ? "border-ok/40 text-ok bg-ok/10" : missingClass)
      }
      title={title}
    >
      {label}
      {ok ? " ✓" : optional ? " –" : " ✗"}
    </span>
  );
}
