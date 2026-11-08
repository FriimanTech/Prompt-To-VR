import { useState } from "react";
import { Link, NavLink, Outlet } from "react-router-dom";
import { useQuery } from "@tanstack/react-query";
import { api } from "./api/client";

export default function App() {
  const health = useQuery({
    queryKey: ["health"],
    queryFn: api.health,
    refetchInterval: 10_000,
  });
  const [sampleNote, setSampleNote] = useState<string | null>(null);
  const [sampleBusy, setSampleBusy] = useState<"pcvr" | "quest" | null>(null);
  const keys = health.data
    ? {
        openai: health.data.has_openai_key,
        google: health.data.has_google_key,
        tripo: health.data.has_tripo_key,
        anthropic: health.data.has_anthropic_key,
      }
    : { openai: false, google: false, tripo: false, anthropic: false };

  async function launchPcvrSample() {
    setSampleBusy("pcvr");
    setSampleNote(null);
    try {
      await api.launchSample();
      setSampleNote("PC VR starter sample is starting. Put on the headset with Quest Link active.");
    } catch (e) {
      setSampleNote(String(e));
    } finally {
      setSampleBusy(null);
    }
  }

  async function launchQuestSample() {
    setSampleBusy("quest");
    setSampleNote(null);
    try {
      await api.launchQuestSample();
      setSampleNote("Glock Laser Deck is installed and starting on the Quest.");
    } catch (e) {
      setSampleNote(String(e));
    } finally {
      setSampleBusy(null);
    }
  }

  return (
    <div className="min-h-full">
      <header className="border-b border-white/10 bg-panel/80 backdrop-blur">
        <div className="mx-auto max-w-6xl px-6 py-4">
          <div className="flex flex-wrap items-center justify-between gap-4">
            <Link to="/" className="text-lg font-semibold tracking-tight">
              <span className="text-accent">Prompt</span>
              <span className="text-muted"> to </span>
              <span className="text-accent2">VR</span>
            </Link>
            <nav className="flex items-center gap-1 rounded-full border border-white/10 bg-white/5 p-1 text-sm">
              <NavLink
                to="/projects"
                className={({ isActive }) =>
                  "rounded-full px-3 py-1 " +
                  (isActive ? "bg-white/10 text-white" : "text-muted hover:text-white")
                }
              >
                Projects
              </NavLink>
              <NavLink
                to="/projects/new"
                className={({ isActive }) =>
                  "rounded-full px-3 py-1 " +
                  (isActive ? "bg-white/10 text-white" : "text-muted hover:text-white")
                }
              >
                New project
              </NavLink>
            </nav>
          </div>

          <div className="mt-4 flex flex-wrap items-end justify-between gap-4">
            <div>
              <div className="mb-2 text-[11px] uppercase tracking-wider text-muted">
                Try a sample
              </div>
              <div className="flex flex-wrap gap-2">
                <button
                  onClick={() => void launchQuestSample()}
                  className="ptv-button-primary"
                  disabled={sampleBusy !== null}
                  title="Installs samples/quest-sample/GlockLaserDeck.apk on the connected Quest and starts Glock Laser Deck."
                >
                  {sampleBusy === "quest" ? "Installing Quest sample…" : "Quest: Launch Sample Game"}
                </button>
                <button
                  onClick={() => void launchPcvrSample()}
                  className="ptv-button"
                  disabled={sampleBusy !== null}
                  title="Starts the Windows starter-room program through Quest Link. Build it first."
                >
                  {sampleBusy === "pcvr" ? "Starting PC VR sample…" : "PC VR: Launch Starter Sample"}
                </button>
              </div>
            </div>
            <div>
              <div className="mb-2 text-right text-[11px] uppercase tracking-wider text-muted">
                API keys
              </div>
              <div className="flex flex-wrap justify-end gap-2">
                <KeyPill label="OPENAI" ok={keys.openai} />
                <KeyPill label="GOOGLE" ok={keys.google} />
                <KeyPill label="TRIPO" ok={keys.tripo} />
                <KeyPill label="ANTHROPIC" ok={keys.anthropic} />
              </div>
            </div>
          </div>

          {sampleNote && (
            <p className="mt-3 max-w-3xl text-sm text-muted">{sampleNote}</p>
          )}
        </div>
      </header>
      <main className="mx-auto max-w-6xl px-6 py-8">
        <Outlet />
      </main>
      <footer className="mt-16 border-t border-white/10">
        <div className="mx-auto flex max-w-6xl flex-wrap items-center justify-between gap-2 px-6 py-5 text-xs text-muted">
          <span>Native C++ · Vulkan + OpenXR · OpenAI · Gemini · Tripo3D · Uthana</span>
          <span>Engine build: {health.data?.cmake_generator ?? "unknown"}</span>
        </div>
      </footer>
    </div>
  );
}

function KeyPill({ label, ok }: { label: string; ok: boolean }) {
  const title =
    label === "ANTHROPIC"
      ? ok
        ? "Claude key is set. The recommended setup leaves this empty so those stages use OpenAI."
        : "Claude key is not set. That is the recommended setup."
      : ok
        ? `${label} key is set in .env`
        : `${label} key is missing from .env`;
  return (
    <span
      className={
        "ptv-pill " +
        (ok ? "border-ok/40 bg-ok/10 text-ok" : "border-err/40 bg-err/10 text-err")
      }
      title={title}
    >
      {label}
      {ok ? " ✓" : " ✗"}
    </span>
  );
}
