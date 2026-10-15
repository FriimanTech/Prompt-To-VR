"""One OpenAI Agents SDK agent that returns a single Pydantic model.

The same shape as ``npc_plan_agent.NpcDirector`` (lazy SDK import, capturing
output schema, retry that restates the brief with the real field errors),
generalised over the output model and able to send images with the brief.
Used by the character-routing and Uthana motion-plan stages.

Tests inject ``runner`` so the SDK and the network are never touched.
"""

from __future__ import annotations

import base64
import io
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Generic, Optional, Type, TypeVar

from pydantic import BaseModel

from .agent_output_capture import (
    MAX_LOGGED_OUTPUT_CHARS,
    diagnose_output_failure,
    make_capturing_schema,
)

T = TypeVar("T", bound=BaseModel)

LogFn = Callable[[str], None]

#: Longest side of an image sent to the model. Enough to judge a body shape,
#: small enough that a dozen characters stay cheap.
MAX_IMAGE_SIDE = 768


class StructuredAgentError(RuntimeError):
    """The agent run failed or returned an unusable result."""


class StructuredAgentOutputError(StructuredAgentError):
    def __init__(self, message: str, *, issues: list[str]) -> None:
        super().__init__(message)
        self.issues = issues


@dataclass
class AgentImage:
    """A labelled image attached to the brief."""

    label: str
    path: Path


@dataclass
class AgentBrief:
    text: str
    images: list[AgentImage] = field(default_factory=list)


RunnerFn = Callable[[AgentBrief], BaseModel]


def image_data_url(path: Path, max_side: int = MAX_IMAGE_SIDE) -> Optional[str]:
    """A JPEG data URL of ``path`` scaled to ``max_side``, or None."""
    try:
        from PIL import Image
    except ImportError:  # pragma: no cover - pillow is a provider dependency
        return None
    try:
        with Image.open(path) as img:
            img = img.convert("RGB")
            img.thumbnail((max_side, max_side))
            buf = io.BytesIO()
            img.save(buf, format="JPEG", quality=85)
    except (OSError, ValueError):
        return None
    return "data:image/jpeg;base64," + base64.b64encode(buf.getvalue()).decode("ascii")


class StructuredAgent(Generic[T]):
    def __init__(
        self,
        *,
        name: str,
        instructions: str,
        output_model: Type[T],
        model: str = "gpt-5.6-sol",
        reasoning_effort: str = "high",
        verbosity: str = "medium",
        max_turns: int = 30,
        max_retries: int = 1,
        runner: Optional[RunnerFn] = None,
        log: Optional[LogFn] = None,
    ) -> None:
        self.name = name
        self.instructions = instructions
        self.output_model = output_model
        self.model = model
        self.reasoning_effort = reasoning_effort
        self.verbosity = verbosity
        self.max_turns = max_turns
        self.max_retries = max_retries
        self._runner = runner
        self.log = log

    def _log(self, text: str) -> None:
        if self.log is not None:
            self.log(text)

    def run(self, brief: AgentBrief) -> T:
        if self._runner is None:
            self._runner = self._build_default_runner()
        attempts = max(0, int(self.max_retries)) + 1
        last: Optional[StructuredAgentError] = None
        for attempt in range(attempts):
            current = brief
            if attempt > 0 and isinstance(last, StructuredAgentOutputError):
                bullets = "\n".join(f"- {i}" for i in last.issues) or "- (unspecified)"
                current = AgentBrief(
                    text=(
                        "Your previous answer did not match the schema. Fix ALL "
                        f"of these and return the COMPLETE answer:\n{bullets}\n\n"
                        + brief.text
                    ),
                    images=brief.images,
                )
                self._log(f"retry {attempt}/{attempts - 1} after: {last}")
            elif attempt > 0:
                self._log(f"retry {attempt}/{attempts - 1} (re-sending brief)")
            try:
                result = self._runner(current)
            except StructuredAgentError as exc:
                last = exc
                continue
            if not isinstance(result, self.output_model):
                raise StructuredAgentError(
                    f"runner returned {type(result).__name__}, expected "
                    f"{self.output_model.__name__}"
                )
            return result
        assert last is not None
        raise last

    # ------------------------------------------------------------------
    def _build_default_runner(self) -> RunnerFn:
        try:
            import agents as agents_mod  # type: ignore
            from agents import Agent, ModelSettings, Runner  # type: ignore
            from openai.types.shared import Reasoning  # type: ignore
        except ImportError as exc:  # pragma: no cover - real runs only
            raise StructuredAgentError(
                "openai-agents is not installed (pip install -e apps/orchestrator)."
            ) from exc

        output_schema = make_capturing_schema(agents_mod, self.output_model, strict=False)
        agent = Agent(
            name=self.name,
            instructions=self.instructions,
            model=self.model,
            model_settings=ModelSettings(
                reasoning=Reasoning(effort=self.reasoning_effort),
                verbosity=self.verbosity,
            ),
            output_type=output_schema,
        )
        model_cls = self.output_model

        def _run(brief: AgentBrief) -> BaseModel:
            output_schema.reset()
            content: list[dict] = [{"type": "input_text", "text": brief.text}]
            attached = 0
            for img in brief.images:
                url = image_data_url(img.path)
                if url is None:
                    continue
                content.append({"type": "input_text", "text": f"Image: {img.label}"})
                content.append({"type": "input_image", "image_url": url, "detail": "low"})
                attached += 1
            payload = (
                [{"role": "user", "content": content}] if attached else brief.text
            )
            self._log(
                f"turn: model={self.model} effort={self.reasoning_effort} "
                f"brief={len(brief.text)} chars images={attached}"
            )
            try:
                result = Runner.run_sync(agent, payload, max_turns=self.max_turns)
                out = result.final_output_as(model_cls)
            except Exception as exc:  # pragma: no cover - live SDK
                raw = output_schema.last_raw
                if raw is not None:
                    self._log(
                        f"FAILED; raw output ({len(raw)} chars):\n"
                        f"{raw[:MAX_LOGGED_OUTPUT_CHARS]}"
                    )
                diagnosis = diagnose_output_failure(raw, exc, model_cls)
                self._log(f"FAILED: {diagnosis.message}")
                if diagnosis.repairable:
                    raise StructuredAgentOutputError(
                        diagnosis.message, issues=diagnosis.issues
                    ) from exc
                raise StructuredAgentError(diagnosis.message) from exc
            return out

        return _run
