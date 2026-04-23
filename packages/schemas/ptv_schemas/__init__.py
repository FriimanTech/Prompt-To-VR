"""Strict Pydantic v2 models for the Prompt-To-Native-VR pipeline.

Every artifact that flows through the pipeline has exactly one model defined
here. The same JSON shape is consumed by the C++ engine via
``engine/runtime/src/scene/SceneJson.cpp``.
"""

from __future__ import annotations

from .build_spec import (
    BuildSpec,
    SceneEntry,
    MechanicEntry,
    AssetEntry,
    TestEntry,
)
from .asset_manifest import (
    AssetManifest,
    AssetItem,
    AssetCategory,
    GenerationStrategy,
    TripoRequest,
    ReferenceImageRequest,
)
from .mechanic_plan import (
    MechanicPlan,
    MechanicPlanItem,
    MechanicClassification,
)
from .scene_spec import (
    SceneSpec,
    EntitySpec,
    Transform,
    MeshRef,
    MechanicBinding,
    LightSpec,
    PlayerSpawn,
)
from .code_task import (
    CodeTask,
    CodeTaskKind,
    CodeTaskFile,
)
from .project_state import (
    ProjectState,
    ProjectStage,
    ProjectStatus,
    StageRecord,
)
from .artifact import ArtifactMetadata, ArtifactKind
from .acceptance import AcceptanceTestPlan, AcceptanceCheck

__all__ = [
    "BuildSpec",
    "SceneEntry",
    "MechanicEntry",
    "AssetEntry",
    "TestEntry",
    "AssetManifest",
    "AssetItem",
    "AssetCategory",
    "GenerationStrategy",
    "TripoRequest",
    "ReferenceImageRequest",
    "MechanicPlan",
    "MechanicPlanItem",
    "MechanicClassification",
    "SceneSpec",
    "EntitySpec",
    "Transform",
    "MeshRef",
    "MechanicBinding",
    "LightSpec",
    "PlayerSpawn",
    "CodeTask",
    "CodeTaskKind",
    "CodeTaskFile",
    "ProjectState",
    "ProjectStage",
    "ProjectStatus",
    "StageRecord",
    "ArtifactMetadata",
    "ArtifactKind",
    "AcceptanceTestPlan",
    "AcceptanceCheck",
]
