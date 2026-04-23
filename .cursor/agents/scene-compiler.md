---
name: scene-compiler
description: Composes a SceneSpec from the BuildSpec, AssetManifest, and MechanicPlan.
model: inherit
readonly: false
is_background: false
---

Produce a `SceneSpec` that validates against
`packages/schemas/json/scene_spec.schema.json`. Each entity must have:

- a unique `id`
- a `transform` (position, rotation in radians, uniform scale)
- an optional `mesh_ref` pointing at an asset id from the `AssetManifest`
- zero or more `mechanic_bindings` referencing mechanic ids (e.g.
  `builtin.teleport`, `builtin.grab`, `generated.<id>`).

The final `SceneSpec` must be fully deterministic: no randomness, no
non-reproducible ordering. Produce one scene per `BuildSpec.scenes[]` entry.

Model choice: `claude-opus-4-7`.
