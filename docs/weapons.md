# Weapons: shooting, swinging, blocking and drawing, for any weapon

Every weapon a game describes -- a Glock, an M16, a laser pistol, an RPG, a
bow, a sword, a hammer, a shield -- is run by an engine built-in, tuned from
the project's `interaction_design.json`. Generated code never fires, keeps
ammo, casts a ray from a muzzle or applies weapon damage; it reacts to what
the weapons did through `ctx.weapons`.

This replaced per-game generated "shooting hubs". The humans-vs-aliens pistol
showed why: its generated ray started at the muzzle, hit the pistol's own
collider first, and no shot ever reached an alien. A weapon that every game
shares is tested once and hits the same way everywhere.

Sockets (where hands go, where shots leave) are described in
[sockets.md](sockets.md); this page is what happens after the item is in the
hand.

## Pipeline

```
interaction_design -> npc_plan -> game_design -> mechanic_plan -> ... ->
socket_generation -> asset_contract -> code_generation -> asset_optimization
-> scene_compile -> build -> quest_runtime_verify (weapons pass)
```

1. **`interaction_design`** decides, per item, its `weapon` (fire system,
   fire mode, rpm, spread, pellets, range, damage, ammo / reload / overheat,
   visual, impact, recoil, haptics), `melee`, `shield` or `bow` spec, and the
   `projectiles` the game's weapons fire. Every decision is `explicit` (with
   the prompt's verbatim quote) or `inferred` (with a rationale, researched
   with web search for real weapons). Fire systems:
   * `hitscan` -- an instant ray, no visible round;
   * `hybrid` -- the ray decides the hit, a tracer / bolt is drawn flying to
     it (the default for real firearms: a 9 mm round crosses five metres per
     frame and is never seen);
   * `projectile` -- a simulated object with travel time and gravity
     (rockets, grenades, slow energy bolts);
   * `beam` -- a continuous ray tested every frame while the trigger is held;
   * `drawn_launcher` -- a bow.

   A projectile gets a mesh (`needs_mesh`) only when it is actually seen
   (a rocket, an arrow); `reconcile_manifest` then appends its asset at its
   real size. It also DROPS manifest assets that are meshes of effects the
   engine draws itself -- `continuous_red_laser_fx`, `rocket_smoke_trail`,
   `muzzle_flash_fx` -- before any image or Tripo credit is spent on them
   (`engine_drawn_fx_assets`); `scene_compile` strips entities showing such
   an asset in older projects, where a mesh of a laser beam stood in front
   of the gun and took every shot.
2. **`mechanic_plan`** -- `weapon_plan.reconcile_weapon_items` maps the
   design onto the built-ins, deterministically and with every change
   named: one `builtin+config` item per weapon / bow / melee item / shield
   (`builtin.item_source` for a quiver, `builtin.npc.ranged_attack` for NPCs
   whose plan has `ranged_attack`), removes generated shooting hubs, fire
   profiles, ammo / reload, recoil and projectile tasks plus `builtin.recoil`,
   and tells every task that depended on a removed one to read
   `ctx.weapons` instead. `code_generation` runs the same reconcile, so an
   older plan is fixed on resume.
3. **`asset_optimization`** caps projectile meshes at the profile's
   `projectile_max_triangles` (Quest 300).
4. **`scene_compile`** (`weapon_scene.apply_weapons`):
   * binds each weapon entity's built-in with the design's numbers
     (`weapon_plan.weapon_params` & co., socket names from the design) and
     makes player weapons `grabbable`;
   * strips stale bindings (`builtin.recoil`, `generated.*` ids the plan no
     longer has) and auto-placed projectile props;
   * emits one hidden **projectile template** per projectile with a mesh
     (`ptv_projectile_<id>`, tag `template`), an **arrow template**
     (`ptv_arrow_<asset>`, tags `template arrow grabbable`) and a
     **source** entity (`ptv_source_<asset>`, `builtin.item_source`) that
     draws arrows / magazines from a body slot;
   * arms ranged NPCs: the design's NPC weapon is attached to the NPC's right
     hand by its own grip socket (`attached_to.socket`); a character modelled
     holding its gun gets `builtin.weapon` on itself; otherwise it warns;
   * writes the scene-level **`projectiles` table** (id, kind, template,
     flight, size) the runtime exposes as `ctx.projectiles->preset(id)`;
   * **sizes projectiles to the bore** (`projectile_scale.py`): a round /
     rocket must be 0.6-1.3x the muzzle's measured `bore_diameter` (an
     energy bolt 1-3x), else its scale is corrected to 1.0x (1.5x); with no
     bore, the design's calibre is the reference; an arrow must be longer
     than the bow's draw. A size the prompt states explicitly is reported,
     never changed;
   * **validates** (`weapon_violations`): every gun has its muzzle socket,
     every bow its limb tips and grip, every melee weapon a strike point,
     every template and source exists, every seen projectile has a mesh --
     failing the stage with all of them listed -- and warns when sustained
     fire would outgrow the projectile pool;
   * gives the wrist HUD an ammo row when a player weapon counts ammo.
5. **`quest_runtime_verify`** runs a third launch (see below).

## Runtime

| Built-in | Bound to | Does |
| --- | --- | --- |
| `builtin.weapon` | the gun | fire modes, rate, spread, pellets, ammo / reload / overheat, recoil, haptics, flash, tracer, hit + damage |
| `builtin.bow` | the bow | nock, draw (string drawn from the limb tips), release through `ctx.projectiles` |
| `builtin.melee_weapon` | sword / hammer / axe | swept strike segment, speed-scaled damage, cooldown |
| `builtin.shield` | the shield | registers as a blocker; hits from its front are absorbed |
| `builtin.item_source` | a mesh-less source entity | squeeze at a body slot to draw a copy of its template into the hand |
| `builtin.npc.ranged_attack` | an NPC | keeps firing distance, faces, fires its weapon in bursts via `ctx.weapons` |

A shot: the holding hand's trigger (or an NPC's `ctx.weapons->trigger`)
reaches `builtin.weapon`; it reads the `muzzle` socket (+Z forward); for
`hitscan` / `hybrid` / `beam` it casts `raycast_ignoring` the weapon, its
wielder, the player's hands and whatever they hold; for `projectile` it
launches into `ctx.projectiles`, which sweeps each step (continuous
collision), sticks / bounces / explodes, and applies splash with a line of
sight check. Damage goes through `ICombat::apply_weapon_damage` (faction
checked by `CombatRules.h`, bypassing the 0.4 s contact invulnerability so
rapid fire counts, blocked by a shield facing the shot), and every hit is
published on `ctx.weapons->hits_since`.

Engine services (all in `ptv/api/`):

* **`ctx.weapons`** (`Weapons.h`): `state(weapon, &s)` (ammo, magazine,
  reserve, reloading, heat, `fire_seq`, `held_by_hand`, `wielder`, last shot
  origin / direction), `held_weapon(hand)`, `hit_seq()` /
  `hits_since(seq, out, max)`, and for AI `trigger`, `fire_once`, `aim_at`.
* **`ctx.projectiles`** (`Projectiles.h`): `launch(ProjectileDesc)`,
  `hits_since`, `live_count`, `preset(id, &desc)` from the scene table.
* **`ctx.vfx`** (`Effects.h`): `muzzle_flash`, `tracer`, `beam`, `impact`,
  `decal`, `explosion`, `light_flash`, `glow`, `streak` -- procedural quads
  drawn by `FxRenderer` in one additive and one alpha pass (see
  [rendering.md](rendering.md)).

## Quest 3 performance

Fixed-size pools only, recycled oldest-first; nothing allocates per shot.
From the scene's `performance` block (`PerformanceProfile`):

| Cap | Quest 3 | PC VR |
| --- | --- | --- |
| `max_live_projectiles` | 64 | 256 |
| `max_projectile_meshes` | 24 | 96 |
| `max_fx_quads` | 1024 | 4096 |
| `max_beams` | 4 | 16 |
| `max_decals` | 48 | 192 |
| `max_flash_lights` | 2 | 4 |
| `projectile_max_triangles` | 300 | 1500 |

Automatic fire draws a tracer every third round unless the design says
otherwise. A projectile makes one swept cast per tick (plus line-of-sight
rays when it explodes). The 1 Hz `perf_fx:` telemetry line reports live
projectiles, FX quads and casts next to frame time.

## Generated code

API contract v5 (`codegen_contracts.py`) rejects, with the reason fed back
to the agent: a `ctx.physics->raycast` from a muzzle socket (always), and in
a project whose design hands weapons to the engine, firing from a muzzle on
the trigger, a private ammo / magazine count, or a private projectile /
bullet / tracer type. Game-specific reactions (a target that scores, a
custom HUD) read `ctx.weapons->hits_since` and `state`.

## Verification on the headset

`quest_runtime_verify` launches a third time with
`debug.ptv.verify_weapons=1` when the scene has player weapons. The engine's
`WeaponVerifier` (`engine/runtime/src/core/WeaponVerifier.h`) supplies a
scripted hand (the headset is lying on a desk), puts each weapon in it via
`IHands::request_hold`, puts the other hand on the support socket of a
two-handed weapon (an RPG does not fire one-handed), aims at the centre of
the nearest hostile's collider (`combat_rules::aim_point`; else a clone of
an NPC template 8 m ahead), and fires: five single pulls then three seconds
held, two seconds for a beam, a scripted draw and release for a bow. It logs
`weapons:` lines; `evaluate_weapons` checks per weapon that it was held, it
fired (five shots, or the whole magazine when it holds fewer), every shot
left within 2 cm of the muzzle socket, it hit the target (hits on level
geometry do not count), the target's health dropped through `ctx.combat`, a
beam ticked continuously, the projectile pool stayed inside its cap, and the
frame rate held: p95 within 15% of the frame budget (vsync jitter) and at
most 2% of frames over 1.5 display periods. Enemies are not auto-defeated
for hitting the player in this pass, so the target survives to be shot.
Report-only, skipped without a headset, like the other passes.

The first runs of this pass found real bugs, all fixed:

* a generated copy of `builtin.grab` picking up the pistol never took a
  scripted hold (designed weapons are always held by `builtin.grab` now);
* every ranged aim used "origin + 1.2 m", which shoots over a character
  whose origin is at half its height (`combat_rules::aim_point` aims at the
  collider's centre, `IPhysicsAccess::body_center`);
* an NPC's hitscan ignored the player's body like the player's own shots
  do, so ranged enemies could never hit the player;
* a drone with `ranged_attack` but no designed weapon and no muzzle never
  fired, so its wave never ended (it now fires from the front of its body);
* a mesh of a laser beam (an effect asset the manifest bought) stood in
  front of the laser pistol and took every shot.
