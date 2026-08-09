"""Strict Pydantic v2 models for the Prompt-To-Native-VR pipeline.

Every artifact that flows through the pipeline has exactly one model defined
here. The same JSON shape is consumed by the C++ engine via
``engine/runtime/src/scene/SceneJson.cpp``.
"""

from __future__ import annotations

from .build_spec import (
    BuildSpec,
    GameType,
    TargetDevice,
    SceneEntry,
    MechanicEntry,
    MechanicType,
    AssetEntry,
    TestEntry,
)
from .performance import (
    PerformanceProfile,
    PerformanceTier,
    AssetImportance,
    AssetBudgetRequest,
    GovernorBounds,
    classify_tier,
    derive_profile,
    allocate_asset_budgets,
    asset_triangle_ceiling,
    importance_weight,
)
from .asset_manifest import (
    AnimationIntent,
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
    ControllerBinding,
    ControllerControl,
    ControllerHand,
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
    "GameType",
    "TargetDevice",
    "SceneEntry",
    "MechanicEntry",
    "MechanicType",
    "AssetEntry",
    "TestEntry",
    "AnimationIntent",
    "AssetManifest",
    "AssetItem",
    "AssetCategory",
    "GenerationStrategy",
    "TripoRequest",
    "ReferenceImageRequest",
    "PerformanceProfile",
    "PerformanceTier",
    "AssetImportance",
    "AssetBudgetRequest",
    "GovernorBounds",
    "classify_tier",
    "derive_profile",
    "allocate_asset_budgets",
    "asset_triangle_ceiling",
    "importance_weight",
    "MechanicPlan",
    "MechanicPlanItem",
    "MechanicClassification",
    "ControllerBinding",
    "ControllerControl",
    "ControllerHand",
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
