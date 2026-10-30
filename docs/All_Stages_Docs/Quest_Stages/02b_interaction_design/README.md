# Stage 2b: Interaction Design

> **Pipeline position:** after `asset_manifest` → before `npc_plan`  
> **Target:** shared (PC VR and Quest)  
> **Primary code:** [`apps/orchestrator/ptv_orchestrator/pipeline/stage_interaction_design.py`](../../../../apps/orchestrator/ptv_orchestrator/pipeline/stage_interaction_design.py)  
> **Primary artifacts:** `projects/<project_id>/interaction_design.json`

## Main stage summary

### What this stage is

The **interaction design stage** decides, once, what the player (or an NPC)
does with every object: a pistol is gripped by its handle and steadied by the
other hand wrapped around the first; a rifle is held by its pistol grip and
supported under the handguard; a round shield is held by one strap with the
forearm through the other; a book is not held at all. For each held item it
also decides the sockets the mesh needs (with a plain-words description of
each part) and, for weapons, how the weapon fires and what it fires.

### Why it exists

Every prop used to get the same two guessed sockets (`muzzle`, `grip`), so a
target board had a muzzle and a shield was held like a pistol. And every
game generated its own shooting code -- the humans-vs-aliens pistol's ray hit
its own collider and never damaged an alien. With this stage, sockets are
found per item on the real mesh (`socket_generation`) and weapons are engine
built-ins tuned from this document.

### Inputs (what enters)

| Input | Location |
|-------|----------|
| User prompt (verbatim) -- the source of explicit decisions | `projects/<id>/prompt.txt` |
| BuildSpec | `build_spec.json` |
| AssetManifest | `asset_manifest.json` |

### Outputs (what it produces)

| Output | Location |
|--------|----------|
| InteractionDesign | `projects/<id>/interaction_design.json` |
| AssetManifest, reconciled (projectile assets appended, real-world scale, `socket_hints` mirrored) | `asset_manifest.json` |

### Where output goes next

- **`npc_plan`, `game_design`, `mechanic_plan`** -- a one-line-per-item excerpt (`design_excerpt`)
- **`mechanic_plan` / `code_generation`** -- `weapon_plan.reconcile_weapon_items` maps weapons onto `builtin.weapon` / `bow` / `melee_weapon` / `shield`
- **`socket_generation`** -- which sockets to find on each mesh, and what each part looks like
- **`asset_optimization`** -- projectile meshes get the projectile triangle cap
- **`scene_compile`** -- weapon bindings, projectile templates and table, projectile scale check

## Detailed stage deep dive

- Agent: OpenAI Agents SDK, `gpt-5.6-sol` at reasoning `high` with
  `WebSearchTool` (real weapons' dimensions, rates of fire, magazine sizes),
  prompt `INTERACTION_DESIGN_SYSTEM` (`pipeline/interaction_design_agent.py`).
- Every decision carries `provenance`: `explicit` with the prompt's verbatim
  quote (checked; unverifiable quotes are downgraded) or `inferred` with a
  rationale. Explicit always wins: "a Glock that shoots a laser beam" fires a
  beam.
- Validate-and-repair (`pipeline/interaction_rules.py`): every held item has
  a complete hold (primary socket, support socket for two-hand holds, forearm
  strap for shields), every weapon a runnable fire system and a muzzle, every
  bow its limb tips and an arrow projectile, scenery no sockets. Problems go
  back to the agent up to `interaction_design_max_repair_rounds` times, then
  are repaired deterministically. Without an agent (no key, API error) a
  deterministic `fallback_design` is used, loudly.
- `reconcile_manifest`: projectiles that are seen (`needs_mesh`: rockets,
  arrows) get an asset at their real size; `real_world_dimensions` correct an
  item's `target_scale_meters`; `socket_hints` mirror the design.

### Settings

`interaction_design_model`, `interaction_design_reasoning_effort` (high),
`interaction_design_verbosity`, `interaction_design_agent_max_turns`,
`interaction_design_max_retries`, `interaction_design_max_repair_rounds`,
`interaction_design_web_search`.

See [docs/sockets.md](../../../sockets.md) and [docs/weapons.md](../../../weapons.md).
