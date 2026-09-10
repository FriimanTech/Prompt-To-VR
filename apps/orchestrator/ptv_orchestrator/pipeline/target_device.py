"""Keep ``BuildSpec.target_device`` in step with the project's own choice.

``ProjectState.target_device`` is the single source of truth: the user picks
PC VR or Quest in the UI at create time, and the orchestrator routes the
build tail, the ``PerformanceProfile`` and the launch path off that value.
``BuildSpec.target_device`` is a MIRROR of it, not an independent decision --
the planning agent has no business choosing which headset the user owns, and
left to itself it just emits the schema default (``pcvr``).

So the build_spec stage overwrites whatever the model returned, and existing
projects are reconciled at run start rather than being forced through a full
BuildSpec regeneration.
"""

from __future__ import annotations

from ptv_schemas import BuildSpec, TargetDevice


def sync_build_spec_device(
    spec: BuildSpec, target_device: TargetDevice
) -> tuple[BuildSpec, bool]:
    """Return ``(spec, changed)`` with ``target_device`` forced to match."""
    if spec.target_device == target_device:
        return spec, False
    return spec.model_copy(update={"target_device": target_device}), True
