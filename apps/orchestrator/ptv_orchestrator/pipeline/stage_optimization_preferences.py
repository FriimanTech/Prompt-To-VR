"""Stage: optimization_preferences -- the user's own Quest optimization choices.

Runs right after build_spec. Reads the main prompt and the New Project page's
"Custom Quest Optimizations" box and maps any optimization wording onto the
fixed set of user-controllable settings (``OptimizationPreferences``). Each
recognised setting becomes a lock in ``PerformanceProfile.locks`` (applied in
asset_optimization), which the runtime governor never moves.

Deterministic around one light model call:

* a keyword gate: no optimization wording in either text -> empty
  preferences, no model call;
* one structured-output call maps the wording onto the schema;
* a validator keeps only settings whose quote appears VERBATIM in its source,
  lets the custom box win over the main prompt, clamps requests beyond the
  hard limits and records unsupported ones (eye tracking, no compression,
  turning memory safety off) -- those three are detected without the model
  too, so they are reported even when it is unavailable.

Optimization wording never shapes gameplay: the BuildSpec prompt is told to
ignore it.
"""

from __future__ import annotations

import hashlib
import json
import re
from typing import Any, Optional

from pydantic import Field

from ptv_schemas import (
    OPTIMIZATION_ALLOWED_VALUES,
    ClampedRequest,
    OptimizationPreferences,
    PrefSetting,
    ProjectStage,
    ProjectStatus,
    UnsupportedRequest,
)
from ptv_schemas.common import StrictModel

from .context import PipelineContext, StageResult

_STAGE = ProjectStage.optimization_preferences
ARTIFACT = "optimization_preferences.json"

#: Words that mean the text may contain an optimization instruction.
KEYWORDS = re.compile(
    r"foveat|\bffr\b|resolution|render scale|supersampl|super-sampl|\blods?\b|"
    r"level of detail|model detail|texture|draw distance|render distance|view distance|"
    r"\bcull|\bfps\b|\bhz\b|frame ?rate|refresh rate|anti-?alias|\bmsaa\b|"
    r"graphics|visual quality|image quality|max(imum)? quality|"
    r"performance|optimi[sz]",
    re.IGNORECASE,
)

#: The preview endpoint and the stage share answers for identical text.
_CACHE: dict[str, OptimizationPreferences] = {}


class _RawItem(StrictModel):
    setting: str = Field(..., description="|".join(OPTIMIZATION_ALLOWED_VALUES))
    value: str
    level: Optional[str] = Field(default=None, description="low|medium|high (FFR only)")
    source: str = Field(..., description="custom_box|main_prompt")
    quote: str = Field(..., description="The user's exact words, copied verbatim.")


class _RawUnsupported(StrictModel):
    request: str
    reason: str
    closest: str = ""
    source: str = "custom_box"
    quote: str = ""


class _RawPrefs(StrictModel):
    items: list[_RawItem] = Field(default_factory=list)
    unsupported: list[_RawUnsupported] = Field(default_factory=list)


SYSTEM = """You map a VR game creator's OPTIMIZATION / GRAPHICS-QUALITY instructions onto
fixed engine settings for a Meta Quest 3 build. Everything is automatic unless the
user explicitly asks for something. Only explicit instructions about rendering
quality or performance count -- NEVER gameplay ("detailed missions", "high
quality enemies AI", "fast-paced") and never asset descriptions ("a detailed
rifle model").

Settings and allowed values:
- ffr (Fixed Foveated Rendering; the Quest 3 has no eye tracking):
  off | fixed (pinned level) | max (dynamic but never above level); level low|medium|high
- resolution (dynamic render resolution): off (always native, never reduced) | high (up to 1.25x, never reduced)
- lod (model detail at distance): detail | performance
- texture_quality: high | memory_saver
- draw_distance: far | performance
- room_culling: off
- anti_aliasing: high (always 4x MSAA)
- frame_rate: 90 | 120

Rules:
- One item per setting per source. source is "custom_box" for text from the
  Custom Quest Optimizations box and "main_prompt" for the main prompt.
- quote MUST be copied verbatim (same words, same order) from that source.
- "no FFR" / "disable foveated rendering" -> ffr off. "no resolution
  reduction" / "never lower resolution" -> resolution off. "high textures" ->
  texture_quality high. "detailed models far away" -> lod detail.
- Requests the engine cannot honour go in unsupported with the reason and the
  closest supported setting: eye-tracked foveation (no eye tracking on Quest 3;
  closest: fixed foveated rendering), disabling texture compression (memory
  safety requires it; closest: texture_quality high), disabling memory
  protection (never allowed).
- Return empty lists when there is no optimization instruction."""


def _norm(s: str) -> str:
    return re.sub(r"\s+", " ", s or "").strip().lower()


def _cache_key(prompt: str, box: str) -> str:
    return hashlib.sha256(f"{prompt}\x00{box}".encode("utf-8")).hexdigest()


def keyword_gate(prompt: str, box: str) -> bool:
    return bool(KEYWORDS.search(box or "")) or bool(KEYWORDS.search(prompt or ""))


_DETERMINISTIC_UNSUPPORTED = [
    (re.compile(r"eye[- ]?track", re.IGNORECASE), "eye-tracked foveated rendering",
     "the Quest 3 has no eye tracking", "fixed foveated rendering (ffr)"),
    (re.compile(r"(no|disable|without|turn off)\s+(texture\s+)?compression", re.IGNORECASE),
     "disable texture compression",
     "memory safety requires compressed (ASTC) textures", "texture quality high (ASTC 4x4)"),
    (re.compile(r"(disable|turn off|no)\s+(the\s+)?memory\s+(guard|protection|safety|limit)",
                re.IGNORECASE),
     "disable memory protection", "memory safety is never overridable", ""),
]


def _deterministic_unsupported(text: str, source: str) -> list[UnsupportedRequest]:
    out = []
    for pat, req, reason, closest in _DETERMINISTIC_UNSUPPORTED:
        m = pat.search(text or "")
        if m:
            out.append(UnsupportedRequest(request=req, reason=reason, closest=closest,
                                          source=source, quote=m.group(0)))
    return out


def _clamps(setting: str, value: str, quote: str) -> tuple[str, Optional[ClampedRequest]]:
    """Requests beyond a hard safety limit are clamped and reported."""
    q = quote.lower()
    if setting == "resolution":
        m = re.search(r"(\d+(?:\.\d+)?)\s*x", q)
        if m and float(m.group(1)) > 1.25:
            return "high", ClampedRequest(setting="resolution", requested=f"{m.group(1)}x",
                                          applied="1.25x (high)",
                                          reason="render scale is capped at 1.25x and maxImageRect")
    if setting == "frame_rate":
        m = re.search(r"(\d{2,3})\s*(hz|fps)", q)
        if m and int(m.group(1)) > 120:
            return "120", ClampedRequest(setting="frame_rate", requested=f"{m.group(1)} Hz",
                                         applied="120 Hz",
                                         reason="the Quest 3 refreshes at most 120 Hz")
    if setting == "texture_quality" and re.search(r"4096|4k|8k", q):
        return "high", ClampedRequest(setting="texture_quality", requested="4096 px",
                                      applied="2048 px (high)",
                                      reason="the maximum texture size is 2048")
    if setting == "anti_aliasing" and re.search(r"8x|16x", q):
        return "high", ClampedRequest(setting="anti_aliasing", requested="8x+",
                                      applied="4x MSAA",
                                      reason="MSAA is capped at 4x")
    return value, None


def validate(raw: _RawPrefs, prompt: str, box: str) -> OptimizationPreferences:
    prefs = OptimizationPreferences(model_called=True)
    sources = {"custom_box": _norm(box), "main_prompt": _norm(prompt)}
    chosen: dict[str, PrefSetting] = {}
    for it in raw.items:
        setting = it.setting.strip().lower()
        source = it.source if it.source in sources else "custom_box"
        allowed = OPTIMIZATION_ALLOWED_VALUES.get(setting)
        if not allowed:
            continue
        if not it.quote or _norm(it.quote) not in sources[source]:
            continue  # not the user's words -> not the user's choice
        value = it.value.strip().lower()
        if setting == "frame_rate":
            value = re.sub(r"\D", "", value) or value
        if value not in allowed or value == "auto":
            continue
        value, clamp = _clamps(setting, value, it.quote)
        if clamp:
            prefs.clamped.append(clamp)
        level = None
        if setting == "ffr" and value in ("fixed", "max"):
            level = {"low": 1, "medium": 2, "high": 3}.get((it.level or "").lower(),
                                                             2 if value == "fixed" else 1)
        current = chosen.get(setting)
        if current is not None and current.source == "custom_box" and source == "main_prompt":
            continue  # the box wins
        chosen[setting] = PrefSetting(value=value, level=level, source=source,  # type: ignore[arg-type]
                                      quote=it.quote.strip())
    for name, s in chosen.items():
        setattr(prefs, name, s)
    seen = set()
    for u in raw.unsupported:
        key = _norm(u.request)
        if key in seen:
            continue
        seen.add(key)
        src = u.source if u.source in sources else "custom_box"
        prefs.unsupported.append(UnsupportedRequest(
            request=u.request, reason=u.reason, closest=u.closest, source=src,  # type: ignore[arg-type]
            quote=u.quote if _norm(u.quote) in sources[src] else ""))
    return prefs


def _merge_unsupported(prefs: OptimizationPreferences, prompt: str, box: str) -> None:
    have = {_norm(u.request) for u in prefs.unsupported}
    for src, text in (("custom_box", box), ("main_prompt", prompt)):
        for u in _deterministic_unsupported(text, src):
            if not any(w in h for h in have for w in _norm(u.request).split()[:2]):
                prefs.unsupported.append(u)
                have.add(_norm(u.request))


def parse_preferences(prompt: str, box: str, client: Any = None,
                      model: Optional[str] = None) -> OptimizationPreferences:
    """Map the two texts onto OptimizationPreferences (cached by text)."""
    key = _cache_key(prompt or "", box or "")
    if key in _CACHE:
        return _CACHE[key].model_copy(deep=True)
    if not keyword_gate(prompt, box):
        prefs = OptimizationPreferences(model_called=False)
    elif client is None:
        prefs = OptimizationPreferences(model_called=False)
    else:
        user = ("Custom Quest Optimizations box (source custom_box):\n"
                + (box.strip() or "(empty)")
                + "\n\nMain prompt (source main_prompt):\n" + (prompt.strip() or "(empty)"))
        resp = client.generate_json(prompt=user, schema=_RawPrefs.model_json_schema(),
                                    system=SYSTEM, model=model)
        raw = _RawPrefs.model_validate(resp.data)
        prefs = validate(raw, prompt, box)
    _merge_unsupported(prefs, prompt, box)
    _CACHE[key] = prefs.model_copy(deep=True)
    return prefs


WARNING_TEXT = (
    "Warning: Your custom Quest optimization settings differ from the VR engine's "
    "recommended settings. Disabling or modifying certain optimizations may reduce "
    "performance, cause unstable frame rates, or prevent your game from running smoothly "
    "on the Quest 3.")


def preview_payload(prefs: OptimizationPreferences) -> dict[str, Any]:
    differs = prefs.any_override() or bool(prefs.unsupported) or bool(prefs.clamped)
    return {"preferences": json.loads(prefs.model_dump_json()),
            "lines": prefs.summary_lines(),
            "differs_from_auto": differs,
            "warning": WARNING_TEXT if differs else ""}


def load_preferences(project_dir) -> Optional[OptimizationPreferences]:
    p = project_dir / ARTIFACT
    if not p.exists():
        return None
    try:
        return OptimizationPreferences.model_validate_json(p.read_text(encoding="utf-8"))
    except Exception:  # noqa: BLE001 - a broken file means "automatic"
        return None


def run(ctx: PipelineContext) -> StageResult:
    from .llm import make_llm

    state = ctx.store.load_state(ctx.project_id)
    ctx.store.record_stage(state, _STAGE, ProjectStatus.running)
    box = getattr(state, "custom_quest_optimizations", "") or ""
    client = make_llm(ctx.settings) if keyword_gate(state.prompt, box) else None
    try:
        prefs = parse_preferences(state.prompt, box, client=client,
                                  model=ctx.settings.claude_light_model)
    except Exception as exc:  # noqa: BLE001 - never block the pipeline on this
        ctx.emit(_STAGE, "warning",
                 f"could not interpret optimization wording ({exc}); every optimization "
                 "stays automatic")
        prefs = OptimizationPreferences(model_called=False)
        _merge_unsupported(prefs, state.prompt, box)
    path = ctx.project_dir / ARTIFACT
    path.write_text(prefs.model_dump_json(indent=2), encoding="utf-8")
    rel = str(path.relative_to(ctx.settings.repo_root)).replace("\\", "/")
    lines = prefs.summary_lines()
    for line in lines:
        ctx.emit(_STAGE, "info", line)
    if prefs.any_override():
        ctx.emit(_STAGE, "warning", WARNING_TEXT)
    msg = (f"{len(prefs.settings())} override(s)" if prefs.settings()
           else "all optimizations automatic") + (
        f", {len(prefs.unsupported)} unsupported" if prefs.unsupported else "")
    ctx.store.record_stage(state, _STAGE, ProjectStatus.succeeded, message=msg, artifacts=[rel])
    ctx.emit(_STAGE, "succeeded", msg)
    return StageResult(stage=_STAGE, artifacts=[rel], data=prefs)
