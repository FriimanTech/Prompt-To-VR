"""Child processes that an Abort can actually kill.

``subprocess.run`` blocks until the child exits, so a stage sitting in one
is deaf to a cancel flag: the cooperative checks in the surrounding loops
only fire between invocations. For short tools that is fine. For anything
that can run for minutes -- the worldgen host, cmake, Gradle -- the run
handle needs the child itself so it can kill the tree.

:func:`run_tracked` is a drop-in for ``subprocess.run(capture_output=True,
text=True)`` that registers the child with the context's run handle first.
:func:`proc_hooks` supplies the same registration to the cmake / Gradle
runners, which own their own ``Popen`` calls because they stream output.
"""

from __future__ import annotations

import subprocess
from typing import Any, Optional, Sequence

from .context import PipelineContext


def proc_hooks(ctx: PipelineContext) -> dict[str, Any]:
    """``on_proc`` / ``on_proc_done`` kwargs for the build runners."""
    return {
        "on_proc": ctx.register_process,
        "on_proc_done": ctx.forget_process,
    }


def run_tracked(
    ctx: PipelineContext,
    argv: Sequence[str],
    *,
    cwd: Optional[str] = None,
    timeout: Optional[float] = None,
    env: Optional[dict[str, str]] = None,
) -> subprocess.CompletedProcess:
    """Run a child to completion, tracked so an Abort can kill it."""
    proc = subprocess.Popen(
        list(argv),
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=env,
    )
    ctx.register_process(proc)
    try:
        stdout, stderr = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        stdout, stderr = proc.communicate()
        raise
    finally:
        ctx.forget_process(proc)
    return subprocess.CompletedProcess(
        args=list(argv),
        returncode=proc.returncode,
        stdout=stdout,
        stderr=stderr,
    )
