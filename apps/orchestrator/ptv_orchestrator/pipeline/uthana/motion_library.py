"""Turn planned motions into Uthana motion ids, then download them.

Motion sources are pluggable. Each :class:`MotionSource` has one resolver
with the same contract::

    resolve(spec, character_id) -> ResolvedMotion(motion_id, name, source, ...)

Download (Part 4) and merge (Part 5) only ever see the resolved
``motion_id``, so adding a source never touches them.

* ``uthana_locomotion`` -- the List your motions -> Motion Verification ->
  (no match) Generate Locomotion -> Rename flow. A motion whose canonical
  name already exists anywhere in the organisation is reused for free.
* ``uthana_library`` -- an existing motion referenced by id (the shared idle
  ``msf9nhQ4yyTJ``). No list, verification, generation or rename.

Every download is ``output_format="glb", no_mesh=True, in_place=True``,
retargeted onto the given character on Uthana's side.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Optional, Protocol

from ptv_schemas import MotionSource, UthanaMotionSpec

from .motion_names import (
    is_locomotion_name,
    parse_locomotion_name,
    parse_uthana_default_name,
    same_params,
)

#: Rough generated length of one stride pair, used only for the pre-call cost
#: estimate (the real length is measured from the downloaded clip).
SECONDS_PER_STRIDE_PAIR_ESTIMATE = 1.2

LogFn = Callable[[str], None]


@dataclass
class ResolvedMotion:
    motion_id: str
    name: str
    #: ``created`` (generated now), ``reused`` (name match in the
    #: organisation) or ``library`` (fixed id).
    source: str
    estimated_seconds: float = 0.0


class MotionResolver(Protocol):
    def resolve(self, spec: UthanaMotionSpec, character_id: str) -> ResolvedMotion: ...


class CostGuardExceeded(RuntimeError):
    """Generating this motion would exceed the per-run generated-seconds cap."""


# ---------------------------------------------------------------------------
# List your motions + Motion Verification
# ---------------------------------------------------------------------------


@dataclass
class MotionCatalogue:
    """The organisation's motions, listed once per run and kept current."""

    client: Any
    limit: Optional[int] = 500
    log: Optional[LogFn] = None
    motions: list = field(default_factory=list)
    total: int = 0

    def refresh(self) -> None:
        everything = self.client.list_motions(None)
        self.total = len(everything)
        self.motions = everything if self.limit is None else everything[: self.limit]
        if self.log:
            self.log(f"List your motions: showing {len(self.motions)} of {self.total}")
            if self.limit is not None and self.total > self.limit:
                self.log(
                    f"WARNING: the organisation has {self.total} motions, more than the "
                    f"{self.limit} listed; raise uthana_motion_list_limit so no match is missed"
                )

    def verify(self, name: str):
        """Motion Verification: the exact-name match, or None."""
        return motion_verification(name, self.motions)[1]

    def add(self, record) -> None:
        self.motions.insert(0, record)
        self.total += 1

    def find_unrenamed(self, params):
        """A motion still carrying Uthana's default name for ``params``."""
        for m in self.motions:
            got = parse_uthana_default_name(getattr(m, "name", "") or "")
            if got is not None and same_params(got, params):
                return m
        return None

    def renamed(self, motion_id: str, name: str) -> None:
        for i, m in enumerate(self.motions):
            if m.motion_id == motion_id:
                self.motions[i] = type(m)(motion_id, name)


def motion_verification(name: str, motions: list) -> tuple[str, Any]:
    """Search ``motions`` for ``name`` exactly.

    Returns ``(message, record_or_None)`` where the message is either
    ``"match found: <name> <id>"`` or
    ``"no match is found in the Uthana Organization for that motion requested"``.
    """
    for m in motions:
        if getattr(m, "name", None) == name:
            return f"match found: {m.name} {m.motion_id}", m
    return "no match is found in the Uthana Organization for that motion requested", None


# ---------------------------------------------------------------------------
# Resolvers
# ---------------------------------------------------------------------------


@dataclass
class LocomotionResolver:
    client: Any
    catalogue: MotionCatalogue
    max_generated_seconds: float = 600.0
    generated_seconds: float = 0.0
    log: Optional[LogFn] = None

    def resolve(self, spec: UthanaMotionSpec, character_id: str) -> ResolvedMotion:
        name = spec.motion_name
        if not is_locomotion_name(name):
            raise ValueError(f"{name!r} is not a locomotion motion name")
        found = self.catalogue.verify(name)
        if found is not None:
            if self.log:
                self.log(f"match found: {found.name} {found.motion_id}")
            return ResolvedMotion(found.motion_id, found.name, "reused")
        if self.log:
            self.log(f"no match is found in the Uthana Organization for {name}")

        params = parse_locomotion_name(name)
        # A motion with exactly these parameters may exist under Uthana's own
        # default name -- generated but never renamed (an interrupted run).
        # Renaming it is free; generating it again is not.
        orphan = self.catalogue.find_unrenamed(params)
        if orphan is not None:
            self.client.rename_motion(orphan.motion_id, name)
            if self.log:
                self.log(f"recovered un-renamed motion {orphan.motion_id} "
                         f"({orphan.name!r}) as {name}")
            self.catalogue.renamed(orphan.motion_id, name)
            return ResolvedMotion(orphan.motion_id, name, "reused")
        estimate = params.strides * SECONDS_PER_STRIDE_PAIR_ESTIMATE
        if self.generated_seconds + estimate > self.max_generated_seconds:
            raise CostGuardExceeded(
                f"generating {name} (~{estimate:.0f}s) would exceed the "
                f"{self.max_generated_seconds:.0f}s generated-motion cap for this run"
            )
        kwargs = params.as_api_kwargs(character_id)
        motion_id = self.client.create_locomotion(
            kwargs.pop("character_id"),
            strides=kwargs["strides"],
            move_speed=kwargs["move_speed"],
            style_id=kwargs["style_id"],
            travel_angle=kwargs["travel_angle"],
        )
        # Rename only after create_locomotion has returned the new id.
        self.client.rename_motion(motion_id, name)
        check = self.client.get_motion(motion_id)
        if check is None or check.name != name:
            raise RuntimeError(
                f"renamed {motion_id} to {name!r} but Uthana reports "
                f"{getattr(check, 'name', None)!r}"
            )
        self.generated_seconds += estimate
        self.catalogue.add(check)
        if self.log:
            self.log(f"created motion {motion_id} named {name}")
        return ResolvedMotion(motion_id, name, "created", estimate)


@dataclass
class LibraryResolver:
    """A motion that already exists in the organisation, by id."""

    def resolve(self, spec: UthanaMotionSpec, character_id: str) -> ResolvedMotion:
        if not spec.motion_id:
            raise ValueError(f"library motion {spec.key!r} has no motion_id")
        return ResolvedMotion(spec.motion_id, spec.motion_name, "library")


def build_resolvers(
    client: Any,
    catalogue: MotionCatalogue,
    *,
    max_generated_seconds: float,
    log: Optional[LogFn] = None,
) -> dict[MotionSource, MotionResolver]:
    """One resolver per supported source. New sources register here."""
    return {
        MotionSource.uthana_locomotion: LocomotionResolver(
            client, catalogue, max_generated_seconds=max_generated_seconds, log=log
        ),
        MotionSource.uthana_library: LibraryResolver(),
    }


# ---------------------------------------------------------------------------
# Download (Part 4)
# ---------------------------------------------------------------------------


def motion_file_name(name: str) -> str:
    slug = re.sub(r"[^A-Za-z0-9_.-]+", "_", name).strip("_") or "motion"
    return f"{slug}.glb"


def download_motion(
    client: Any, character_id: str, motion_id: str, dest: Path, *, in_place: bool = True
) -> Path:
    """Download ``motion_id`` retargeted onto ``character_id`` (glb, no mesh).

    ``in_place=True`` is the clip the game plays. ``in_place=False`` fetches
    the same motion WITH root motion, only to measure its true ground speed.
    """
    if not client.is_download_allowed(character_id, motion_id):
        raise RuntimeError(
            f"Uthana says downloading {motion_id} for {character_id} is not allowed "
            "(download quota)"
        )
    data = client.download_motion(character_id, motion_id, no_mesh=True, in_place=in_place)
    if not data or data[:4] != b"glTF":
        raise RuntimeError(f"download of {motion_id} did not return a GLB")
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_suffix(".part")
    tmp.write_bytes(data)
    tmp.replace(dest)
    return dest
