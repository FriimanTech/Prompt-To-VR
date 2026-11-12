# Sockets: how the engine knows where a hand goes on any object

A socket is a named local transform on an asset: "the palm goes here", "shots
leave from here, pointing that way", "the forearm passes through this strap".
Gameplay asks for sockets by name (`ctx.sockets->socket_world(e, "muzzle")`),
and `builtin.grab` holds an item by them. The asset pipeline produces them from
the actual mesh, because Tripo returns a single raw mesh with no skeleton and
no part names.

## Pipeline

```
asset_manifest -> interaction_design -> ... -> asset_normalization
               -> socket_generation -> asset_contract -> ... -> scene_compile
```

1. **`interaction_design`** (agent, `gpt-5.6-sol` high + web search) decides,
   per non-character asset, its `usage`, its `hold` profile and the `sockets`
   it needs. Each socket has a `kind` and a `part_description` in plain words
   ("the pistol's handle below the slide, where the palm wraps"). Scenery gets
   no sockets. The artifact is `interaction_design.json`; its validator and
   deterministic repair live in `interaction_rules.py`.
2. **`socket_generation`** finds those parts on the real mesh:
   * `ptv_glb_views` (C++, `engine/tools/glb_views.cpp`) renders 10 views
     (six orthographic along the mesh's principal axes, four three-quarter
     perspectives). Each view also gets a **triangle-id buffer** and a depth
     buffer, and the tool writes the merged mesh (`mesh.bin`). Geometry comes
     from `load_glb_textured`, which uses the same node traversal as the
     runtime's `load_glb_merged`, so triangle *i* is the runtime's triangle *i*.
   * Gemini (`socket_vision_model`, default `gemini-3.8-flash`, Interactions
     API) segments the socket parts plus orientation context parts (barrel,
     blade, head, the shield's front face, the bowstring) in every view.
     Polygon masks are decoded under every plausible coordinate convention.
     The reading whose bounding box matches the returned `box_2d` wins, because
     models drift between conventions.
   * **Fusion** (`sockets/fusion.py`): mask pixels become triangles through the
     id buffer. A triangle joins a part when the views that can see it mostly
     agree (score of at least 0.5). The region is then cut to its largest
     connected piece.
   * **Solvers** (`sockets/solvers.py`) turn regions into exact transforms. A
     handle gets its principal axis, palm-height cross-section centre and
     thickness. A muzzle gets the rim centre, bore diameter and barrel axis. A
     handguard is gripped palm-up. Shield straps are placed on the back, or
     synthesised there when Tripo did not model them. A blade gets its base and
     tip, a hammer its striking face. Bows get their rest, string and limb
     tips, and projectiles their nose and nock.
   * **Render-and-verify**: the solved sockets are drawn on the mesh (an
     orange fist band, a red muzzle arrow, strap bars). The views are rendered
     upright using the solved up vector, and Gemini is asked per socket whether
     a person would hold the item that way. On a failure, the parts are
     segmented again once with the critique. If it still fails, the geometry
     fallback is verified too, and whichever passes wins; otherwise the
     socket is kept, labelled `low`.
   * Results are cached by GLB hash, design spec and pipeline version
     (`assets/<id>/sockets/result.json`), so a resume never re-bills a call
     and never moves a socket. `socket_report.json` summarises the stage. A
     REQUIRED socket that cannot be produced fails the stage, listing all of
     them.
   * Without a Google key the same solvers run on geometry-only regions
     (`sockets/fallback.py`) and every socket is labelled `heuristic`.
3. **`asset_contract`** merges the sockets and the hold profile into
   `asset_contract.json`, alongside each character's model frame. With an
   interaction design, an asset it gave no sockets gets none: there are no
   more guessed muzzles on targets and racks. Projects from before the stage
   keep the legacy `ptv_glb_sockets` derivation.
4. **`scene_compile`** converts unit-space positions to metres
   (`target_scale_meters`) and emits `mesh.sockets[...]`, including `kind`,
   `confidence`, `mirror`, `bore_diameter_m`, `handle_thickness_m` and
   `lateral_offset_m`, plus `mesh.hold`. It fails when a hold names a missing
   socket and warns on implausible distances between two-hand sockets.
   `required_sockets` validation is unchanged.

## Frame conventions

**Hand sockets** (`hand_grip`, `support_grip`, `support_wrap`, `hand_strap`)
are the **OpenXR grip frame** of the hand holding that part:

* origin on the gripped part's axis at palm height;
* **-Z** along the part from the little finger to the thumb (pistol: from the
  bottom of the handle toward the slide; sword: from the pommel toward the
  guard);
* **+X** the palm normal: into the palm for the right hand, away from it for
  the left. For a handle this is the item's right for both hands;
* **+Y** completes a right-handed frame (the back of a handle).

`mirror: same`: the palm switches sides with the hand (handles), so both hands
use this frame. `mirror: flip_z180`: both palms approach from the same side
(palm-up under a handguard, a shield strap), so the left hand uses this frame
turned half a turn about Z.

**Facing sockets** (`muzzle`, `strike_tip`, `strike_base`, `strike_face`,
`projectile_nose`, `arrow_nock`, `arrow_rest`, `string_rest`, limb tips): **+Z**
is the direction the point faces, **+Y** is the item's up. Generated code reads
`Vec3(m[3])` as the origin and `Vec3(m[2])` as the direction.

**Surface sockets** (`press_surface`, `screen`): **+Z** is the surface normal,
pointing out of the object toward the person using it. `press_surface` is the
cap a fingertip pokes (`builtin.press_button` uses its normal and footprint);
`screen` is the face a `builtin.world_display` writes on. The geometry
fallback finds a press surface among the top, upward-facing triangles and a
screen among the triangles facing along the thinnest axis. See
[interactions.md](interactions.md).

## Holding (runtime)

`builtin.grab` (`HoldSolver.h`) picks the solve from `CHoldProfile`:

* **hand-frame** (swords, tools, bows): `item = grip * inverse(socket)`;
* **aim-aligned** (guns, launchers, wands): the palm socket sits at the grip
  origin, and the item turns about it so `hold.forward` / `hold.up` follow the
  controller's aim pose. The Touch handle is angled, so the grip frame alone
  would point a barrel at the floor;
* **two_hand_line** (rifle grip plus handguard, two-handed sword, RPG): the
  second hand squeezing within 16 cm of the support socket steadies the item
  instead of taking it. The span between the two sockets is laid along the
  line between the hands, rolled to keep the primary hand's up. Pulling the
  hands far apart lets go. Releasing the primary hand hands the item to the
  support hand;
* **two_hand_wrap** (pistol): the item follows the shooting hand, and the
  support hand is drawn wrapped around it, offset to its own side by
  `lateral_offset_m`;
* **forearm_strap** (shield): the hand strap sits at the palm and the
  hand-strap-to-forearm-strap line lies along the forearm, with the front face
  along the back of the forearm. The forearm comes from the avatar's own arm
  (published by `builtin.avatar_body`) or from the default hand rig's
  measured wrist (`IHands::forearm_direction`).

The visible hand is drawn on the socket it holds
(`IHands::set_hand_anchor`, valid for the frame it is set in). Both default
hands and a full-body avatar's arm IK read it. `builtin.hand_pose` reads the
hold's `finger_pose`: `trigger` keeps the index finger on the trigger and
following it, `wrap` / `strap` close all four fingers, and a support hand
cups. NPCs hold items the same way through
`IAttach::attach_by_socket(item, "grip", npc, "hand_r")`, which measures the
NPC rig's palm. Sockets on rigged meshes ride their nearest joint.

Each grab logs `hold check: entity=.. hand=.. palm_err_cm=.. aim_err_deg=..`.

## Benchmark

`apps/orchestrator/scripts/socket_benchmark.py` sets up `projects/socket-benchmark`
with 11 held items (pistol, rifle, blaster, swords, strapped shield, bow, arrow,
hammer, RPG, bullet), generates them (paid), runs `socket_generation`, and
writes a contact sheet and `metrics.json`. The `existing` subcommand runs the
same pass on shipped project weapons without touching those projects.
