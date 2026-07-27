"""Shared helpers for Quest (Android) identifiers.

Used by both the quest_build stage (to instantiate the manifest/gradle) and the
launch route (to ``am start`` the right component), so the package name is
computed identically in both places.
"""

from __future__ import annotations

import re


def sanitize_package_segment(project_id: str) -> str:
    """Turn a project id into a legal Java package segment.

    Java segments cannot contain '-' or start with a digit; the alien-shooter
    id ``20260717-074449-alien-shooter`` becomes
    ``p20260717_074449_alien_shooter``.
    """
    seg = re.sub(r"[^a-z0-9_]", "_", project_id.lower())
    if not seg or seg[0].isdigit():
        seg = "p" + seg
    return seg


def quest_package_name(prefix: str, project_id: str) -> str:
    """Full Android applicationId, e.g. ``com.prompttovr.p2026..._alien_shooter``."""
    return f"{prefix}.{sanitize_package_segment(project_id)}"
