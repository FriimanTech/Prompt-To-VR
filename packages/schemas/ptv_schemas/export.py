"""Export Pydantic models as JSON Schema files.

Usage::

    ptv-export-schemas packages/schemas/json
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Type

from pydantic import BaseModel

from . import (
    AcceptanceTestPlan,
    ArtifactMetadata,
    AssetManifest,
    BuildSpec,
    CodeTask,
    MechanicPlan,
    ProjectState,
    SceneSpec,
)


_EXPORTS: dict[str, Type[BaseModel]] = {
    "build_spec": BuildSpec,
    "asset_manifest": AssetManifest,
    "mechanic_plan": MechanicPlan,
    "scene_spec": SceneSpec,
    "code_task": CodeTask,
    "project_state": ProjectState,
    "artifact_metadata": ArtifactMetadata,
    "acceptance_test_plan": AcceptanceTestPlan,
}


def export_all(out_dir: Path) -> dict[str, Path]:
    out_dir.mkdir(parents=True, exist_ok=True)
    written: dict[str, Path] = {}
    for name, model in _EXPORTS.items():
        schema = model.model_json_schema(by_alias=True)
        # Sort keys for deterministic diffs.
        path = out_dir / f"{name}.schema.json"
        path.write_text(
            json.dumps(schema, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        written[name] = path
    return written


def main() -> int:
    if len(sys.argv) != 2:
        print("Usage: ptv-export-schemas <out_dir>", file=sys.stderr)
        return 2
    written = export_all(Path(sys.argv[1]))
    for name, path in written.items():
        print(f"wrote {name:24s} -> {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
