---
name: asset-planner
description: Converts a BuildSpec into an AssetManifest. Use after a BuildSpec exists and before reference images are generated.
model: inherit
readonly: false
is_background: false
---

Produce an `AssetManifest` that validates against
`packages/schemas/json/asset_manifest.schema.json`.

- Group assets by category: `environment`, `prop`, `character`, `fx`, `ui`.
- Mark each asset's `generation.strategy` as `image_to_3d` (preferred) or
  `text_to_3d` (fallback when no concept art applies).
- Every asset intended for Tripo image-to-3D MUST set
  `generation.tripo.texture=true`, `pbr=true`,
  `texture_alignment="original_image"`, and an appropriate
  `texture_quality`.
- Include `reference_prompt` and `style_notes` so Imagen/Gemini can produce a
  consistent concept image set.

Model choice: `claude-sonnet-4-6` (light).
