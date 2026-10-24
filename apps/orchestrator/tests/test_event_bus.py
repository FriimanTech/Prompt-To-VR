"""The event bus is safe from stage threads and can never fail a stage.

A burst of Gradle output used to fill a browser's 256-slot queue from a
worker thread, raise ``QueueFull`` out of ``ctx.emit`` and fail quest_build.
"""

from __future__ import annotations

import asyncio
import threading

from ptv_orchestrator.events import EventBus, PipelineEvent


def _ev(i: int, status: str = "log") -> PipelineEvent:
    return PipelineEvent(project_id="p", stage="quest_build", status=status, message=str(i))


def test_thread_burst_to_slow_subscriber_never_raises_and_keeps_statuses():
    bus = EventBus(queue_size=64)
    received: list[PipelineEvent] = []
    errors: list[BaseException] = []

    async def main() -> None:
        agen = bus.subscribe("p")
        first = asyncio.ensure_future(agen.__anext__())
        await asyncio.sleep(0)  # subscriber registered

        def stage_thread() -> None:
            try:
                bus.publish(_ev(-1, "running"))
                for i in range(10_000):
                    bus.publish(_ev(i))
                bus.publish(_ev(10_000, "failed"))
            except BaseException as exc:  # noqa: BLE001
                errors.append(exc)

        t = threading.Thread(target=stage_thread)
        t.start()
        while t.is_alive():
            await asyncio.sleep(0.01)
        t.join()
        received.append(await first)
        # Drain what the queue held, slowly reading never mattered.
        while True:
            try:
                received.append(await asyncio.wait_for(agen.__anext__(), 0.2))
            except asyncio.TimeoutError:
                break
        await agen.aclose()

    asyncio.run(main())
    assert not errors
    statuses = [e.status for e in received]
    assert "running" in statuses and "failed" in statuses
    assert len(received) <= 64 + 1


def test_publish_without_subscribers_and_after_loop_closes():
    bus = EventBus()
    bus.publish(_ev(0))  # nobody listening: fine

    async def subscribe_once() -> None:
        agen = bus.subscribe("p")
        task = asyncio.ensure_future(agen.__anext__())
        await asyncio.sleep(0)
        task.cancel()

    asyncio.run(subscribe_once())
    # The subscriber's loop is gone; publishing must not raise and must
    # forget it.
    bus.publish(_ev(1))
    bus.publish(_ev(2))
    assert bus.subscriber_count("p") == 0


def test_global_subscribers_receive_every_project():
    bus = EventBus()
    got: list[str] = []

    async def main() -> None:
        agen = bus.subscribe("*")
        nxt = asyncio.ensure_future(agen.__anext__())
        await asyncio.sleep(0)
        bus.publish(_ev(7, "succeeded"))
        got.append((await nxt).status)
        await agen.aclose()

    asyncio.run(main())
    assert got == ["succeeded"]
