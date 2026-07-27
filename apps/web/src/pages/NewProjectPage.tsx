import { useState } from "react";
import { useNavigate } from "react-router-dom";
import { api, type TargetDevice } from "../api/client";

const SAMPLE = `A small Iron Man-style training arcade (local PC VR, Quest 3 via Quest Link).

Scene: a single clean room with polished floor, a pedestal on the left with a
glowing repulsor gauntlet, three floating cylindrical targets at head height,
and a charge station on the right wall.

Mechanics:
 - teleport around the room,
 - grab the repulsor gauntlet from the pedestal,
 - fire a short-range repulsor blast by squeezing the trigger (custom
   mechanic: projectile, 3 charges, 0.25s cooldown),
 - reload by touching the charge station with the gauntlet.

Acceptance tests:
 - player can teleport and see hands,
 - grabbing the gauntlet attaches it to the hand and enables firing,
 - destroying all three targets prints "Training complete" on a HUD,
 - reload works by bringing the gauntlet to the charge station.
`;

export default function NewProjectPage() {
  const nav = useNavigate();
  const [title, setTitle] = useState("Iron Man training arcade");
  const [prompt, setPrompt] = useState(SAMPLE);
  const [device, setDevice] = useState<TargetDevice>("pcvr");
  const [busy, setBusy] = useState(false);
  const [err, setErr] = useState<string | null>(null);

  async function submit(runNow: boolean) {
    setBusy(true);
    setErr(null);
    try {
      const { project_id } = await api.createProject(title, prompt, device);
      if (runNow) {
        await api.run(project_id).catch((e) => {
          // Surface the error but still navigate so the user can see state.
          setErr(String(e));
        });
      }
      nav(`/projects/${project_id}`);
    } catch (e) {
      setErr(String(e));
    } finally {
      setBusy(false);
    }
  }

  return (
    <div className="space-y-4">
      <div>
        <h1 className="text-xl font-semibold">New project</h1>
        <p className="text-sm text-muted">
          Describe the VR game. Be concrete about mechanics, assets, and
          acceptance tests. Reuse of built-in mechanics is preferred.
        </p>
      </div>
      <div className="ptv-card p-5 space-y-3">
        <label className="block">
          <div className="text-sm text-muted mb-1">Title</div>
          <input
            className="ptv-input"
            value={title}
            onChange={(e) => setTitle(e.target.value)}
            maxLength={128}
          />
        </label>
        <label className="block">
          <div className="text-sm text-muted mb-1">Target device</div>
          <div className="flex gap-2">
            <button
              type="button"
              className={device === "pcvr" ? "ptv-button-primary" : "ptv-button"}
              onClick={() => setDevice("pcvr")}
            >
              PC VR (Quest Link)
            </button>
            <button
              type="button"
              className={device === "quest" ? "ptv-button-primary" : "ptv-button"}
              onClick={() => setDevice("quest")}
            >
              Quest (standalone APK)
            </button>
          </div>
          <div className="text-xs text-muted mt-1">
            {device === "pcvr"
              ? "Builds a Windows .exe you run on the PC and view over Quest Link."
              : "Builds a standalone Android APK installed and run locally on the Quest 3."}
          </div>
        </label>
        <label className="block">
          <div className="text-sm text-muted mb-1">Prompt</div>
          <textarea
            className="ptv-textarea mono text-sm"
            value={prompt}
            onChange={(e) => setPrompt(e.target.value)}
            rows={22}
            maxLength={16_000}
          />
        </label>
        {err && (
          <div className="text-err text-sm whitespace-pre-wrap">{err}</div>
        )}
        <div className="flex items-center gap-2">
          <button
            className="ptv-button-primary"
            onClick={() => submit(true)}
            disabled={busy}
          >
            Create &amp; run pipeline
          </button>
          <button
            className="ptv-button"
            onClick={() => submit(false)}
            disabled={busy}
          >
            Create without running
          </button>
        </div>
      </div>
    </div>
  );
}
