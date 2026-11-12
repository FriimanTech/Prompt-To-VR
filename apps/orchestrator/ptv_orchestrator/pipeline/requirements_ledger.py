"""The requirements ledger: extraction, validation and coverage gates.

See ``ptv_schemas.requirements`` for why it exists. This module owns:

* :func:`extract` -- one hard-tier structured call that splits the prompt
  into atomic requirements, then :func:`validate` keeps only those whose
  quote is the prompt's own words (the same check every ``explicit``
  decision in the pipeline gets);
* the coverage gates -- each planning stage owns some requirement kinds
  (:data:`STAGE_OWNS`); after it runs, :func:`gate` records what it claimed
  and returns what it left unclaimed, so the stage can ask its model once
  more and fail with the list when the second answer still leaves gaps;
* :func:`brief` -- the ledger as text for a stage's prompt.

A project without ``requirements.json`` (generated before the ledger
existed) has no gates: :func:`load` returns ``None`` and every helper is a
no-op.
"""

from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Any, Iterable, Optional

from pydantic import Field

from ptv_schemas import (
    Requirement,
    RequirementClaim,
    RequirementKind,
    RequirementOutcome,
    RequirementPriority,
    RequirementResult,
    RequirementsLedger,
)
from ptv_schemas.common import StrictModel

ARTIFACT = "requirements.json"
REPORT = "requirements_report.json"

#: Which stage must claim which requirement kinds.
STAGE_OWNS: dict[str, set[RequirementKind]] = {
    "asset_manifest": {RequirementKind.asset, RequirementKind.visual},
    "mechanic_plan": {RequirementKind.mechanic, RequirementKind.interaction,
                      RequirementKind.rule, RequirementKind.npc},
    "scene_spec": {RequirementKind.placement},
    "world_generation": {RequirementKind.layout},
}


class _RawRequirement(StrictModel):
    id: str = Field(..., description="snake_case id, e.g. req_lane2_timer")
    kind: str = Field(..., description="|".join(k.value for k in RequirementKind))
    priority: str = Field(default="must", description="must|should")
    statement: str
    quote: str = Field(..., description="The prompt's exact words, copied verbatim.")
    subjects: list[str] = Field(default_factory=list)
    acceptance: list[str] = Field(default_factory=list)


class _RawLedger(StrictModel):
    requirements: list[_RawRequirement] = Field(default_factory=list)


SYSTEM = """You turn a VR game prompt into a ledger of ATOMIC requirements -- every
single thing the creator asked for, one per item, so that each can be planned,
built and checked separately. Missing one means the shipped game silently lacks it.

Rules:
- One requirement per checkable fact. "Two tables with a Glock, an M4 and a grenade
  on them" is several: the tables exist, each item exists, each item rests ON a table.
- kind is one of:
  asset (an object or character must exist), visual (how something looks),
  placement (where something is: on a table, hanging from the ceiling, in lane 2,
  in front / middle / back), layout (the environment's structure: three lanes,
  lane 3 is wider, a ceiling), interaction (how the player uses an object: grab,
  throw, press, activate), mechanic (a gameplay system: a timed challenge, scoring,
  respawn), rule (a constraint: no manual reload, 20 seconds), npc (which NPCs
  exist, where they stand, what they do, whether they die and respawn).
- quote MUST be copied verbatim from the prompt (same words, same order); keep it
  short -- the clause that states this requirement.
- statement: one plain sentence a tester could check.
- acceptance: 1-3 things an observer would see when it is met.
- subjects: the objects / characters / areas named, in the prompt's words.
- priority "should" only for things the prompt states as optional or nice to have.
- Do not invent requirements the prompt does not state; do not include
  optimization / graphics-quality wording (another stage handles it).
- ids: short snake_case, unique, prefixed req_."""


def _norm(s: str) -> str:
    return re.sub(r"[^a-z0-9]+", " ", (s or "").lower()).strip()


def _slug(s: str, taken: set[str]) -> str:
    base = re.sub(r"[^a-z0-9_]+", "_", (s or "").lower()).strip("_")[:56] or "req"
    if not base.startswith("req"):
        base = f"req_{base}"[:60]
    out, k = base, 2
    while out in taken:
        out = f"{base[:58]}_{k}"
        k += 1
    taken.add(out)
    return out


def validate(raw: _RawLedger, prompt: str, prompt_id: str) -> RequirementsLedger:
    """Keep requirements whose quote is the prompt's own words."""
    text = _norm(prompt)
    taken: set[str] = set()
    out: list[Requirement] = []
    notes: list[str] = []
    seen_statements: set[str] = set()
    for r in raw.requirements:
        try:
            kind = RequirementKind(r.kind.strip().lower())
        except ValueError:
            notes.append(f"dropped '{r.id}': unknown kind '{r.kind}'")
            continue
        quote = (r.quote or "").strip()
        if not quote or _norm(quote) not in text:
            notes.append(f"dropped '{r.id}': its quote is not in the prompt")
            continue
        key = _norm(r.statement)
        if key in seen_statements:
            continue
        seen_statements.add(key)
        priority = (RequirementPriority.should if r.priority.strip().lower() == "should"
                    else RequirementPriority.must)
        out.append(Requirement(
            id=_slug(r.id, taken), kind=kind, priority=priority,
            statement=r.statement.strip()[:400], quote=quote[:800],
            subjects=[s.strip()[:80] for s in r.subjects if s.strip()][:12],
            acceptance=[a.strip()[:200] for a in r.acceptance if a.strip()][:6],
        ))
    return RequirementsLedger(prompt_id=prompt_id, requirements=out, notes=notes)


def extract(prompt: str, prompt_id: str, client: Any, model: Optional[str] = None,
            build_spec_json: str = "") -> RequirementsLedger:
    user = "VR game prompt:\n" + prompt.strip()
    if build_spec_json:
        user += ("\n\nThe BuildSpec already planned from it (for vocabulary only; the "
                 "PROMPT is the source of truth):\n" + build_spec_json[:20000])
    resp = client.generate_json(prompt=user, schema=_RawLedger.model_json_schema(),
                                system=SYSTEM, model=model)
    return validate(_RawLedger.model_validate(resp.data), prompt, prompt_id)


# ---------------------------------------------------------------------------
# Storage
# ---------------------------------------------------------------------------

def load(project_dir: Path) -> Optional[RequirementsLedger]:
    p = Path(project_dir) / ARTIFACT
    if not p.exists():
        return None
    try:
        return RequirementsLedger.model_validate_json(p.read_text(encoding="utf-8"))
    except Exception:  # noqa: BLE001 - a broken ledger disables the gates
        return None


def save(project_dir: Path, ledger: RequirementsLedger) -> Path:
    p = Path(project_dir) / ARTIFACT
    p.write_text(ledger.model_dump_json(indent=2), encoding="utf-8")
    return p


# ---------------------------------------------------------------------------
# Prompts
# ---------------------------------------------------------------------------

def brief(ledger: Optional[RequirementsLedger], stage: str, *, all_kinds: bool = False) -> str:
    """The requirements a stage must satisfy, as prompt text. Empty without a
    ledger."""
    if ledger is None or not ledger.requirements:
        return ""
    owns = STAGE_OWNS.get(stage, set())
    rows = []
    for r in ledger.requirements:
        mine = r.kind in owns
        if not (mine or all_kinds):
            continue
        tag = "YOURS" if mine else "context"
        rows.append(f"- {r.id} [{r.kind.value}, {r.priority.value}, {tag}]: {r.statement}"
                    f"  (prompt: \"{r.quote[:160]}\")")
    if not rows:
        return ""
    return ("REQUIREMENTS LEDGER (requirements.json). Every requirement marked YOURS must "
            "be claimed by at least one of your items: list its id in that item's "
            "`satisfies`. Claim only what an item really delivers.\n" + "\n".join(rows))


# ---------------------------------------------------------------------------
# Claims + gates
# ---------------------------------------------------------------------------

def _claims(stage: str, items: Iterable[tuple[str, Iterable[str]]],
            known: set[str]) -> dict[str, list[RequirementClaim]]:
    out: dict[str, list[RequirementClaim]] = {}
    for item_id, sats in items:
        for rid in sats or []:
            rid = str(rid).strip()
            if rid in known:
                out.setdefault(rid, []).append(RequirementClaim(stage=stage, item=str(item_id)))
    return out


def claims_from_manifest(manifest: Any, known: set[str]) -> dict[str, list[RequirementClaim]]:
    return _claims("asset_manifest",
                   ((i.id, getattr(i, "satisfies", [])) for i in manifest.items), known)


def claims_from_mechanic_plan(plan: Any, known: set[str]) -> dict[str, list[RequirementClaim]]:
    return _claims("mechanic_plan",
                   ((i.id, getattr(i, "satisfies", [])) for i in plan.items), known)


def claims_from_scene_specs(specs: Iterable[Any], known: set[str]) -> dict[str, list[RequirementClaim]]:
    pairs = []
    for spec in specs:
        for e in spec.entities:
            pairs.append((e.id, getattr(e, "satisfies", [])))
    return _claims("scene_spec", pairs, known)


def claims_from_world(world_docs: Iterable[dict], known: set[str]) -> dict[str, list[RequirementClaim]]:
    """``ctx.satisfies(id, how)`` calls a world builder made, written by
    ptv_worldgen under ``requirements``."""
    out: dict[str, list[RequirementClaim]] = {}
    for doc in world_docs:
        for c in (doc or {}).get("requirements") or []:
            rid = str((c or {}).get("id") or "").strip()
            if rid in known:
                out.setdefault(rid, []).append(RequirementClaim(
                    stage="world_generation", item=str(doc.get("builder_id") or "world"),
                    note=str(c.get("how") or "")[:300]))
    return out


_STOP = {"a", "an", "the", "and", "or", "of", "with", "to", "in", "on", "for", "vr",
          "player", "players", "model", "asset", "prop", "item", "object", "3d"}


def _tokens(text: str) -> set[str]:
    out = set()
    for t in _norm(text).split():
        if t in _STOP or len(t) < 3:
            continue
        out.add(t[:-1] if t.endswith("s") and len(t) > 4 else t)
    return out


def name_claims(ledger: Optional[RequirementsLedger], stage: str,
                items: Iterable[tuple[str, str]]) -> dict[str, list[RequirementClaim]]:
    """Asset and visual requirements claimed by name: a requirement one of
    whose subjects' HEAD noun is in an asset's id / name ("Glock Pistol" ->
    ``glock_pistol``). Any shared word was too loose: "all-black tactical
    combat outfit" claimed every asset whose notes said black. Only for the
    kinds where existence IS the requirement -- a mechanic that merely
    mentions a grenade does not make it explode."""
    if ledger is None:
        return {}
    out: dict[str, list[RequirementClaim]] = {}
    items = list(items)
    for r in ledger.requirements:
        if r.kind not in (RequirementKind.asset, RequirementKind.visual):
            continue
        heads = {w[-1] for w in (_ordered(s) for s in (r.subjects or [r.statement])) if w}
        if not heads:
            continue
        for item_id, text in items:
            if heads & _tokens(f"{item_id.replace('_', ' ')} {text}"):
                out.setdefault(r.id, []).append(RequirementClaim(
                    stage=stage, item=item_id, note="matched by name"))
    return out


def _ordered(text: str) -> list[str]:
    out: list[str] = []
    for t in _norm(text).split():
        if t in _STOP or len(t) < 3:
            continue
        t = t[:-1] if t.endswith("s") and len(t) > 4 else t
        if t not in out:
            out.append(t)
    return out


def _names_subject(subject: str, name: str) -> bool:
    """``name`` (an entity / asset id) is the thing ``subject`` describes:
    its head noun ("pistol" of "Glock Pistol"), or two of its words ("red dot"
    of "Red Dot Scope" in ``m4a1_red_dot``)."""
    words = _ordered(subject)
    have = _tokens(name.replace("_", " "))
    if not words or not have:
        return False
    return words[-1] in have or len(set(words) & have) >= 2


_BANDS = ("front", "middle", "back", "rear", "left", "right", "north", "south", "east", "west",
          "near", "far", "inner", "outer", "upper", "lower")


def placed_subject(r: Requirement) -> str:
    """The subject a placement requirement PLACES: the one its statement
    names first ("The regular-sized Goblins are positioned at the front of
    the third lane" places the goblins, not the lane), whatever order the
    subjects were listed in."""
    if not r.subjects:
        return r.statement
    words = _ordered(r.statement)

    def first_mention(subject: str) -> int:
        head = _ordered(subject)
        idx = [words.index(w) for w in head if w in words]
        return min(idx) if idx else len(words) + 1

    return min(r.subjects, key=first_mention)


def placement_claims(ledger: Optional[RequirementsLedger], scenes: Iterable[Any],
                     world_owned: Iterable[str] = (), npc_plan: Any = None
                     ) -> dict[str, list[RequirementClaim]]:
    """Placement requirements a stage's artifacts deliver whether or not the
    agent listed them in ``satisfies``:

    * an entity named for the subject the requirement places
      (:func:`placed_subject`) that HAS a placement intent -- a pistol
      ``on_surface`` on a table anchor, a challenge template with an
      ``anchor_set``;
    * world-owned furniture (the world builds and places it);
    * an encounter group whose ``zone_tags`` / ``zone_quote`` say where it
      appears (the director spawns it there).

    The shooting range's composer placed every item with an anchor and never
    filled ``satisfies``; every placement requirement read as unmet."""
    if ledger is None:
        return {}
    out: dict[str, list[RequirementClaim]] = {}
    owned = set(world_owned)
    placed: list[tuple[str, str]] = []
    for spec in scenes:
        for e in getattr(spec, "entities", []) or []:
            p = getattr(e, "placement", None)
            rel = getattr(getattr(p, "relation", None), "value", "free") if p else "free"
            if rel == "free" and not (p and (p.anchor or p.anchor_set)):
                continue
            mesh = getattr(e, "mesh", None)
            asset = getattr(mesh, "asset_id", "") if mesh is not None else ""
            placed.append((e.id, f"{e.id} {asset}"))
    groups = []
    enc = getattr(npc_plan, "encounters", None) if npc_plan is not None else None
    for w in (enc.waves if enc is not None else []):
        for g in w.groups:
            if g.zone_tags:
                groups.append(g)
    for r in ledger.requirements:
        if r.kind != RequirementKind.placement:
            continue
        subject = placed_subject(r)
        for eid, name in placed:
            if _names_subject(subject, name):
                out.setdefault(r.id, []).append(RequirementClaim(
                    stage="scene_spec", item=eid, note="placed with an anchor"))
        for asset in owned:
            if _names_subject(subject, asset):
                out.setdefault(r.id, []).append(RequirementClaim(
                    stage="world_generation", item=asset, note="world-owned; the world places it"))
        text = _norm(f"{r.statement} {r.quote}")
        for g in groups:
            band = any(b in text.split() and any(b in t for t in g.zone_tags) for b in _BANDS)
            quoted = bool(g.zone_quote) and _norm(g.zone_quote) in _norm(r.quote + " " + r.statement)
            if (band or quoted) and _names_subject(subject, g.asset_id):
                out.setdefault(r.id, []).append(RequirementClaim(
                    stage="npc_plan", item=g.asset_id,
                    note=f"spawns in zones tagged {', '.join(g.zone_tags)}"))
    return out


def merge_claims(*parts: dict[str, list[RequirementClaim]]) -> dict[str, list[RequirementClaim]]:
    out: dict[str, list[RequirementClaim]] = {}
    for part in parts:
        for rid, cs in part.items():
            have = {(c.stage, c.item) for c in out.get(rid, [])}
            for c in cs:
                if (c.stage, c.item) not in have:
                    out.setdefault(rid, []).append(c)
                    have.add((c.stage, c.item))
    return out


def gate(ledger: Optional[RequirementsLedger], stage: str,
         claims: dict[str, list[RequirementClaim]]) -> list[Requirement]:
    """Record ``stage``'s claims (replacing its previous ones) and return the
    must-requirements it owns that nobody claimed."""
    if ledger is None:
        return []
    for rid in list(ledger.claims):
        ledger.claims[rid] = [c for c in ledger.claims[rid] if c.stage != stage]
        if not ledger.claims[rid]:
            del ledger.claims[rid]
    for rid, cs in claims.items():
        ledger.claims.setdefault(rid, []).extend(cs)
    owns = STAGE_OWNS.get(stage, set())
    return [r for r in ledger.requirements
            if r.kind in owns and r.priority == RequirementPriority.must
            and not ledger.claims.get(r.id)]


def uncovered_text(missing: list[Requirement]) -> str:
    return "\n".join(f"- {r.id} [{r.kind.value}]: {r.statement} (prompt: \"{r.quote[:160]}\")"
                     for r in missing)


class RequirementGateError(RuntimeError):
    """A planning stage left requirements it owns unclaimed after its repair round."""


# ---------------------------------------------------------------------------
# Results + report
# ---------------------------------------------------------------------------

def clear_results(ledger: RequirementsLedger, source: str) -> None:
    """Forget what ``source`` recorded last time: a re-run replaces its own
    verdicts, or a failure the fixed stage no longer finds outlives the fix."""
    ledger.results = {k: v for k, v in ledger.results.items() if v.source != source}


def record_result(ledger: RequirementsLedger, req_id: str, outcome: RequirementOutcome,
                  evidence: list[str], source: str) -> None:
    if ledger.get(req_id) is None:
        return
    prev = ledger.results.get(req_id)
    # A verified failure is never overwritten by a weaker "unverified".
    if prev is not None and outcome == RequirementOutcome.unverified and \
            prev.outcome != RequirementOutcome.unverified:
        return
    ledger.results[req_id] = RequirementResult(
        id=req_id, outcome=outcome, evidence=[e[:300] for e in evidence][:20], source=source)


def write_report(project_dir: Path, ledger: RequirementsLedger) -> Path:
    rows = []
    for r in ledger.requirements:
        res = ledger.results.get(r.id)
        rows.append({
            "id": r.id, "kind": r.kind.value, "priority": r.priority.value,
            "statement": r.statement, "quote": r.quote,
            "claimed_by": [c.model_dump() for c in ledger.claims.get(r.id, [])],
            "outcome": (res.outcome.value if res else RequirementOutcome.unverified.value),
            "evidence": list(res.evidence) if res else [],
            "source": res.source if res else "",
        })
    summary = {k: sum(1 for x in rows if x["outcome"] == k) for k in ("pass", "fail", "unverified")}
    summary["unclaimed"] = sum(1 for x in rows if not x["claimed_by"])
    p = Path(project_dir) / REPORT
    p.write_text(json.dumps({"summary": summary, "requirements": rows}, indent=2),
                 encoding="utf-8")
    return p
