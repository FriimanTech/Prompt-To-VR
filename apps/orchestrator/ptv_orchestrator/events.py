"""Per-project in-memory event bus for SSE progress streaming.

Stages run on worker threads; subscribers (one per open browser stream) live
on the server's asyncio loop. Two rules follow from that:

* An event is handed to a subscriber through ITS loop
  (``call_soon_threadsafe``). An ``asyncio.Queue`` is not thread-safe: put
  into from another thread, it does not wake the reader, so it silently
  fills up.
* Publishing never raises and never blocks. A browser that falls behind
  loses old ``log`` lines -- never a stage status -- and a build is never
  failed by the act of reporting on it (it was: a burst of Gradle output
  raised ``QueueFull`` and failed ``quest_build``).
"""

from __future__ import annotations

import asyncio
import json
import sys
import threading
from collections import defaultdict
from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
from typing import AsyncIterator

#: Events a subscriber can hold before old log lines are dropped.
QUEUE_SIZE = 1024

#: Statuses that mark a stage boundary. Never dropped for a slow reader.
STATUS_EVENTS = frozenset({"running", "succeeded", "failed", "aborted", "skipped"})


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


class _Subscriber:
    def __init__(self, loop: asyncio.AbstractEventLoop, maxsize: int) -> None:
        self.loop = loop
        self.queue: asyncio.Queue[PipelineEvent] = asyncio.Queue(maxsize=maxsize)
        self.dropped = 0

    def offer(self, event: PipelineEvent) -> None:
        """Runs on the subscriber's loop. Makes room by dropping the oldest
        log line; a status event is kept even if that means dropping one."""
        q = self.queue
        if not q.full():
            q.put_nowait(event)
            return
        items = [q.get_nowait() for _ in range(q.qsize())]
        oldest_log = next(
            (i for i, ev in enumerate(items) if ev.status not in STATUS_EVENTS), None)
        if oldest_log is not None:
            del items[oldest_log]
            items.append(event)
        elif event.status in STATUS_EVENTS:
            del items[0]
            items.append(event)
        # else: the queue holds only stage statuses and this is a log line;
        # the log line is what gets dropped.
        self.dropped += 1
        for ev in items:
            q.put_nowait(ev)


class EventBus:
    def __init__(self, queue_size: int = QUEUE_SIZE) -> None:
        self._subs: dict[str, list[_Subscriber]] = defaultdict(list)
        self._lock = threading.Lock()
        self._queue_size = queue_size

    def publish(self, event: PipelineEvent) -> None:
        """Deliver ``event`` to every subscriber of its project and of "*".

        Safe from any thread; never raises."""
        try:
            with self._lock:
                targets = list(self._subs.get(event.project_id, [])) + list(
                    self._subs.get("*", []))
            for sub in targets:
                self._deliver(sub, event)
        except Exception as exc:  # noqa: BLE001 - reporting must never fail a stage
            print(f"[events] publish failed: {type(exc).__name__}: {exc}", file=sys.stderr)

    def _deliver(self, sub: _Subscriber, event: PipelineEvent) -> None:
        if sub.loop.is_closed():
            self._remove(sub)
            return
        try:
            running = asyncio.get_running_loop()
        except RuntimeError:
            running = None
        if running is sub.loop:
            sub.offer(event)
            return
        try:
            sub.loop.call_soon_threadsafe(sub.offer, event)
        except RuntimeError:
            # The loop closed between the check and the call.
            self._remove(sub)

    def _remove(self, sub: _Subscriber) -> None:
        with self._lock:
            for subs in self._subs.values():
                if sub in subs:
                    subs.remove(sub)

    def subscriber_count(self, project_id: str) -> int:
        with self._lock:
            return len(self._subs.get(project_id, []))

    async def subscribe(
        self, project_id: str
    ) -> AsyncIterator[PipelineEvent]:
        sub = _Subscriber(asyncio.get_running_loop(), self._queue_size)
        with self._lock:
            self._subs[project_id].append(sub)
        try:
            while True:
                ev = await sub.queue.get()
                yield ev
        finally:
            self._remove(sub)


_bus = EventBus()


def get_bus() -> EventBus:
    return _bus
