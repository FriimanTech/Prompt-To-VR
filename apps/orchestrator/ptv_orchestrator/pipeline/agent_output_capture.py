"""Diagnosable structured output for the Agents SDK stages.

The OpenAI Agents SDK validates a model's structured output against the
``output_type`` and, when that fails, raises ``ModelBehaviorError`` with the
payload **redacted** by default (``OPENAI_AGENTS_DONT_LOG_MODEL_DATA``). The
message is then literally ``"Invalid JSON when parsing model output"``: no
field, no path, no snippet.

That is fine for a one-shot call and useless for a repair loop. A stage that
feeds the failure back to the agent is telling it "something was wrong,
guess what" -- so it guesses the same thing again and burns the whole repair
budget. The scene_spec stage did exactly that for three attempts and ten
minutes.

``make_capturing_schema`` wraps the SDK's own schema object and keeps the
exact string the model produced, so a failure can be written to the
project's log in full and turned into field-level repair instructions.
Redaction stays on: with the payload captured here, there is no reason to
let the SDK inline it into an exception message that ends up in the UI.

Used by the scene_spec, mechanic_plan, npc_plan and world agents, which all
hit the same wall.
"""

from __future__ import annotations

import re
from typing import Any, Optional

from pydantic import BaseModel, ValidationError

MAX_ISSUES = 12
"""Enough for the agent to act on. The same mistake repeated ninety times
across ninety entities is one lesson, not ninety."""

MAX_LOGGED_OUTPUT_CHARS = 200_000

_INDEX = re.compile(r"\.\d+(?=\.|$)")


def make_capturing_schema(
    agents_mod: Any, model_type: type[BaseModel], *, strict: bool
) -> Any:
    """An ``AgentOutputSchemaBase`` that remembers the model's raw output.

    Built by a factory because ``AgentOutputSchemaBase`` can only be
    subclassed after the lazy SDK import each agent does.
    """

    class _CapturingOutputSchema(agents_mod.AgentOutputSchemaBase):  # type: ignore[misc]
        """Delegates to the real schema; records what it was handed."""

        def __init__(self) -> None:
            self._inner = agents_mod.AgentOutputSchema(
                model_type, strict_json_schema=strict
            )
            self.last_raw: Optional[str] = None

        def reset(self) -> None:
            self.last_raw = None

        def is_plain_text(self) -> bool:
            return False

        def name(self) -> str:
            return self._inner.name()

        def json_schema(self) -> dict[str, Any]:
            return self._inner.json_schema()

        def is_strict_json_schema(self) -> bool:
            return self._inner.is_strict_json_schema()

        def validate_json(self, json_str: str) -> Any:
            self.last_raw = json_str
            return self._inner.validate_json(json_str)

    return _CapturingOutputSchema()


def issues_from_validation_error(err: ValidationError) -> list[str]:
    """Field-level repair instructions, deduped by shape.

    ``entities.0.id`` and ``entities.37.id`` failing the same way collapse
    into one issue, while the reported location keeps a real index so the
    agent can go look at it.
    """
    issues: list[str] = []
    seen: set[tuple[str, str]] = set()
    for e in err.errors():
        loc = ".".join(str(p) for p in e.get("loc", ()))
        msg = str(e.get("msg", "invalid"))
        key = (_INDEX.sub(".<i>", loc), msg)
        if key in seen:
            continue
        seen.add(key)
        issues.append(f"{loc or '<root>'}: {msg}")
        if len(issues) >= MAX_ISSUES:
            issues.append("(further errors of the same kinds were omitted)")
            break
    return issues


def looks_truncated(raw: str) -> bool:
    """A complete JSON object ends with '}'. Anything else was cut off."""
    return not raw.rstrip().endswith("}")


class OutputDiagnosis:
    """What went wrong with one structured-output turn.

    ``kind`` is one of ``truncated``, ``validation``, ``not_json`` or
    ``other``. Only the first three are worth repairing in-context; ``other``
    means the SDK failed before it ever saw model text (transport error,
    refusal, turn budget, a schema the API rejected).
    """

    def __init__(self, kind: str, message: str, issues: list[str]) -> None:
        self.kind = kind
        self.message = message
        self.issues = issues

    @property
    def repairable(self) -> bool:
        return self.kind != "other"

    @property
    def truncated(self) -> bool:
        return self.kind == "truncated"

    def __str__(self) -> str:  # pragma: no cover - trivial
        return self.message


def diagnose_output_failure(
    raw: Optional[str], exc: BaseException, model_type: type[BaseModel]
) -> OutputDiagnosis:
    """Classify an Agents SDK failure using the captured raw output."""
    text = str(exc)

    if raw is None:
        return OutputDiagnosis(
            "other", f"Agents SDK run failed: {text}", []
        )

    if looks_truncated(raw) or "max_output_tokens" in text or "incomplete" in text:
        return OutputDiagnosis(
            "truncated",
            f"model output was cut off after {len(raw)} chars "
            "(hit the output token cap)",
            [],
        )

    try:
        model_type.model_validate_json(raw)
    except ValidationError as verr:
        issues = issues_from_validation_error(verr)
        return OutputDiagnosis(
            "validation",
            f"model output failed schema validation ({len(issues)} problem(s))",
            issues,
        )
    except ValueError:
        return OutputDiagnosis(
            "not_json",
            f"model output was not valid JSON ({len(raw)} chars)",
            ["the output was not parseable JSON; emit one JSON object only"],
        )

    # Re-validating succeeded, so the failure was elsewhere in the run.
    return OutputDiagnosis("other", f"Agents SDK run failed: {text}", [])
