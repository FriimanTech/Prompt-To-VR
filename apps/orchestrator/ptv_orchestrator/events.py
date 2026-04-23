"""Per-project in-memory event bus for SSE progress streaming."""

from __future__ import annotations

import asyncio
import json
from collections import defaultdict
from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
from typing import AsyncIterator


def _now_iso() -> str:
    return datetime.now(tz=timezone.utc).isoformat()


@dataclass
class PipelineEvent:
    project_id: str
    stage: str
    status: str
    message: str = ""
    at: str = field(default_factory=_now_iso)

    def to_json(self) -> str:
        return json.dumps(asdict(self))


class EventBus:
    def __init__(self) -> None:
        self._queues: dict[str, list[asyncio.Queue[PipelineEvent]]] = defaultdict(list)

    def publish(self, event: PipelineEvent) -> None:
        for q in list(self._queues.get(event.project_id, [])):
            q.put_nowait(event)
        for q in list(self._queues.get("*", [])):
            q.put_nowait(event)

    async def subscribe(
        self, project_id: str
    ) -> AsyncIterator[PipelineEvent]:
        q: asyncio.Queue[PipelineEvent] = asyncio.Queue(maxsize=256)
        self._queues[project_id].append(q)
        try:
            while True:
                ev = await q.get()
                yield ev
        finally:
            self._queues[project_id].remove(q)


_bus = EventBus()


def get_bus() -> EventBus:
    return _bus
