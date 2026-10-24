"""File replacement that survives other processes reading the file.

On Windows, a file another handle has open cannot be renamed over, renamed
away or deleted: ``os.replace`` fails with ``PermissionError`` (WinError 5,
access denied, or WinError 32, sharing violation). Readers are everywhere
here -- the web UI's artifact listing hashes every project file, antivirus
scans new downloads, the editor's file watcher peeks at JSON -- and each holds
the file for only milliseconds. Retrying for a few seconds turns a crash into
a short wait.
"""

from __future__ import annotations

import os
import shutil
import sys
import threading
import time
from pathlib import Path
from typing import Union

PathLike = Union[str, "os.PathLike[str]"]

#: Total time spent retrying one operation before giving up.
RETRY_BUDGET_S = 3.0
_FIRST_DELAY_S = 0.02
_MAX_DELAY_S = 0.25


def _is_transient(exc: OSError) -> bool:
    if isinstance(exc, PermissionError):
        return True
    return getattr(exc, "winerror", None) in (5, 32, 33)


def _retry(fn, *, budget_s: float = RETRY_BUDGET_S):
    deadline = time.monotonic() + budget_s
    delay = _FIRST_DELAY_S
    while True:
        try:
            return fn()
        except OSError as exc:
            if not _is_transient(exc) or time.monotonic() + delay > deadline:
                raise
            time.sleep(delay)
            delay = min(delay * 2.0, _MAX_DELAY_S)


def replace_with_retry(src: PathLike, dst: PathLike, *, budget_s: float = RETRY_BUDGET_S) -> None:
    """``os.replace(src, dst)``, retried while another handle holds either."""
    _retry(lambda: os.replace(src, dst), budget_s=budget_s)


def unlink_with_retry(path: PathLike, *, budget_s: float = RETRY_BUDGET_S) -> bool:
    """Delete ``path`` if it exists. Returns False (never raises) when it
    stays locked: a leftover file is always better than a lost result."""
    try:
        _retry(lambda: Path(path).unlink(missing_ok=True), budget_s=budget_s)
        return True
    except OSError as exc:
        print(f"[fsutil] could not delete {path}: {exc}", file=sys.stderr)
        return False


def rmtree_with_retry(path: PathLike, *, budget_s: float = RETRY_BUDGET_S) -> bool:
    """Remove a directory tree; False when part of it stays locked."""
    p = Path(path)
    if not p.exists():
        return True
    try:
        _retry(lambda: shutil.rmtree(p) if p.exists() else None, budget_s=budget_s)
        return True
    except OSError as exc:
        print(f"[fsutil] could not remove {path}: {exc}", file=sys.stderr)
        return False


def move_with_retry(src: PathLike, dst: PathLike, *, budget_s: float = RETRY_BUDGET_S) -> None:
    """Move ``src`` to ``dst``, replacing ``dst``.

    A rename first (same volume, atomic). If that keeps failing, copy and
    then delete the source; once the copy has landed the move has succeeded,
    so a source that cannot be deleted is logged and left behind rather than
    raised -- raising there is how a paid download used to be recorded as a
    failure."""
    src_p, dst_p = Path(src), Path(dst)
    dst_p.parent.mkdir(parents=True, exist_ok=True)
    try:
        replace_with_retry(src_p, dst_p, budget_s=budget_s)
        return
    except OSError:
        pass  # locked or cross-volume: copy instead
    tmp = dst_p.with_name(f"{dst_p.name}.{os.getpid()}.{threading.get_ident()}.tmp")
    _retry(lambda: shutil.copy2(src_p, tmp), budget_s=budget_s)
    replace_with_retry(tmp, dst_p, budget_s=budget_s)
    unlink_with_retry(src_p, budget_s=budget_s)


def atomic_write_text(path: PathLike, text: str, *, encoding: str = "utf-8",
                      budget_s: float = RETRY_BUDGET_S) -> None:
    """Write ``text`` so readers see the old file or the new one, never half.

    The temp name is unique per process and thread, so two writers can never
    collide on it."""
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    tmp = p.with_name(f"{p.name}.{os.getpid()}.{threading.get_ident()}.tmp")
    try:
        tmp.write_text(text, encoding=encoding)
        replace_with_retry(tmp, p, budget_s=budget_s)
    finally:
        if tmp.exists():
            unlink_with_retry(tmp, budget_s=0.2)
