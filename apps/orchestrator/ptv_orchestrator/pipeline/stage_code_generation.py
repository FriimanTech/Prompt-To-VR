"""Stage 8: Claude generates C++ modules for every generate-class mechanic.

Generated files are written under engine/generated/<kind>/<project_id>/<name>.cpp
so that multiple projects can coexist and a single check-bounds script can
reject any file that lands outside these directories.
"""

from __future__ import annotations

import re
from pathlib import Path

from ptv_schemas import CodeTask, CodeTaskKind, ProjectStage, ProjectStatus

from .bounds import assert_path_allowed, filter_cpp_only
from .context import PipelineContext, StageResult
from .llm import make_llm
from .prompts import CODE_TASK_SYSTEM


_KIND_DIR = {
    CodeTaskKind.mechanic: "mechanics",
    CodeTaskKind.ai: "ai",
    CodeTaskKind.dialogue: "dialogue",
}


def run(ctx: PipelineContext) -> StageResult:
    ctx.emit(ProjectStage.code_generation, "running", "Generating C++ modules")
    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(
        state, ProjectStage.code_generation, ProjectStatus.running
    )

    plan = ctx.mechanic_plan or ctx.store.load_mechanic_plan(ctx.project_id)
    to_generate = [it for it in plan.items if it.code_task is not None]

    produced: list[str] = []
    client = make_llm(ctx.settings)

    repo_root: Path = ctx.settings.repo_root
    gen_root = repo_root / "engine" / "generated"
    for it in to_generate:
        task: CodeTask = it.code_task  # type: ignore[assignment]
        rel_dir = Path("engine/generated") / _KIND_DIR[task.kind] / ctx.project_id
        out_dir = repo_root / rel_dir
        out_dir.mkdir(parents=True, exist_ok=True)
        fname = _safe_filename(task.id) + ".cpp"
        out_path = out_dir / fname

        prompt = (
            f"CodeTask:\n{task.model_dump_json(indent=2)}\n\n"
            "Produce ONLY the C++ file contents (no prose, no fences). "
            f"The file will be saved at {rel_dir.as_posix()}/{fname}."
        )
        resp = client.generate_code(
            prompt=prompt,
            system=CODE_TASK_SYSTEM,
        )
        code = _strip_code_fences(resp.raw_text)
        code = filter_cpp_only(code)

        # Boundary guard: path must be under engine/generated/{mechanics,ai,dialogue}/
        assert_path_allowed(out_path, gen_root)
        out_path.write_text(code, encoding="utf-8")
        produced.append(str(out_path.relative_to(repo_root)).replace("\\", "/"))

        # Write a CodeTask side-car for traceability.
        side = out_path.with_suffix(".codetask.json")
        side.write_text(task.model_dump_json(indent=2), encoding="utf-8")
        produced.append(str(side.relative_to(repo_root)).replace("\\", "/"))

        ctx.emit(
            ProjectStage.code_generation,
            "info",
            f"{task.id} -> {out_path.name}",
        )

    ctx.store.record_stage(
        state,
        ProjectStage.code_generation,
        ProjectStatus.succeeded,
        message=f"Generated {len(produced)} files",
        artifacts=produced,
    )
    ctx.emit(
        ProjectStage.code_generation,
        "succeeded",
        f"{len(produced)} files",
    )
    return StageResult(stage=ProjectStage.code_generation, artifacts=produced)


def _safe_filename(raw: str) -> str:
    s = re.sub(r"[^a-zA-Z0-9_-]+", "_", raw)
    return s[:48] or "module"


def _strip_code_fences(text: str) -> str:
    t = text.strip()
    if t.startswith("```"):
        # Remove fenced block
        t = re.sub(r"^```[a-zA-Z0-9]*\n", "", t)
        if t.endswith("```"):
            t = t[: -3]
    return t.strip() + "\n"
