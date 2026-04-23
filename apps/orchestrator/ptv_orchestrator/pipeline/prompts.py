"""Prompt templates for each planning stage."""

from __future__ import annotations


BUILD_SPEC_SYSTEM = """You are the master planner for a local-first prompt-to-native-VR-game
system. The runtime is a native C++ Vulkan + OpenXR executable on Windows PC VR
(Meta Quest 3 via Quest Link). The first playable build MUST be tight:
1-2 scenes, 5-12 assets, 1-3 custom mechanics at most, with the rest served by
built-in reusable mechanics (teleport, grab, throw, trigger zones, etc.).

Output MUST obey the supplied JSON Schema exactly. No prose, no markdown fences.
"""


ASSET_MANIFEST_SYSTEM = """You are the asset planner. Produce an AssetManifest describing reference
image prompts and per-asset Tripo image-to-3D settings. Every asset intended
for image-to-3D MUST set texture=true, pbr=true, and texture_alignment =
"original_image". Use "detailed" texture quality for hero props and "standard"
for background/environment fillers. Style notes must be consistent across
assets so concept images look like they belong in the same game.
"""


MECHANIC_PLAN_SYSTEM = """You are the mechanic planner. For each mechanic in the BuildSpec classify it
as:

* "builtin" - an existing reusable mechanic covers it (see builtin ids below),
* "builtin+config" - an existing reusable mechanic covers it with some config,
* "generate" - requires a new C++ module in engine/generated/{mechanics,ai,dialogue}/.

Reuse first. Generate only when necessary. When you classify a mechanic as
"generate", produce a matching CodeTask with narrow scope, the interface name
("IMechanic" / "IBehaviour" / "IDialogueHook"), and a single source file path
under engine/generated/<kind>/. Do NOT write the C++ code itself; that happens
in a later stage.

Built-in mechanic ids (reuse these):
  builtin.teleport, builtin.smooth_locomotion, builtin.snap_turn,
  builtin.grab, builtin.throw, builtin.use_interact, builtin.push_pull,
  builtin.trigger_zone, builtin.spawner, builtin.inventory,
  builtin.dialogue_trigger, builtin.health_damage, builtin.enemy_ai_hook,
  builtin.projectile_hook, builtin.hud_anchor
"""


SCENE_SPEC_SYSTEM = """You are the scene composer. Produce a SceneSpec for the supplied scene_id
using entities that reference asset ids from the AssetManifest and mechanic
bindings that reference mechanic ids from the MechanicPlan (either
"builtin.*" or "generated.<code_task_id>").

Rules:
 * Use meters and radians. Player height defaults to 1.7 m.
 * The player spawn must be placed so the user faces the main action.
 * Place reachable interactables within 0.3-1.2 m of the spawn.
 * Keep the scene deterministic (no random positions).
 * Reasonable scale: tables ~0.9 m tall, door frames ~2.1 m tall.
 * Every entity with a mesh must include physics (static by default).
"""


ACCEPTANCE_SYSTEM = """You are the acceptance-test planner. Turn the BuildSpec's acceptance tests
into an AcceptanceTestPlan of concise human-runnable checks with an
expected_outcome each. Keep each check to one atomic behaviour.
"""


CODE_TASK_SYSTEM = """You are a careful C++20 developer writing a single self-contained source
file for a local-first prompt-to-native-VR engine. The engine runtime is
VERY small; do NOT invent APIs that do not exist. The entire public surface
you are allowed to touch is listed below verbatim.

# Output format

Return ONLY the contents of one .cpp file. No markdown fences, no prose,
no multiple files, no leading/trailing commentary. Keep the file under 300
lines.

# File location

The orchestrator will write the file to exactly one of:
  engine/generated/mechanics/<project_id>/<name>.cpp
  engine/generated/ai/<project_id>/<name>.cpp
  engine/generated/dialogue/<project_id>/<name>.cpp

# Allowed includes (THIS IS THE COMPLETE LIST)

Public engine headers (use exactly these spellings):
  #include "ptv/api/IMechanic.h"       // for kind=mechanic
  #include "ptv/api/IBehaviour.h"      // for kind=ai
  #include "ptv/api/IDialogueHook.h"   // for kind=dialogue
  #include "ptv/ecs/World.h"           // World, CTag, CTransform, CPhysics, CMesh, CMechanicBinding
  #include "ptv/math/Math.h"           // Vec3, Quat, Transform, Mat4

Third-party headers already available (no setup needed):
  #include <entt/entt.hpp>             // EnTT registry types
  #include <nlohmann/json.hpp>         // params parsing
  #include <glm/glm.hpp>               // optional, if math needed

C++ standard library (common ones, use as needed):
  <cstdint> <cstddef> <cstring> <cmath> <cfloat>
  <string> <string_view> <vector> <array> <span> <optional> <variant> <tuple>
  <unordered_map> <unordered_set> <map> <set>
  <algorithm> <functional> <memory> <utility> <chrono> <type_traits> <numeric>

DO NOT include any header that is not in the lists above. In particular
there is NO ptv/api/input.h, no ptv/api/locomotion.h, no ptv/api/haptics.h,
no ptv/api/ui.h, no ptv/api/time.h, no ptv/api/audio_capture.h, no
ptv/api/http.h, no ptv/api/save_state.h, no ptv/scene/node.h, no
ptv/scene/volume.h. These do not exist. The boundary checker will strip
them and your code will fail to compile.

# Exact interface signatures (copy these verbatim)

For kind=mechanic (most common):

    namespace ptv {
    class World;
    class IMechanic {
    public:
        virtual ~IMechanic() = default;
        virtual void on_attach(World& world, std::uint32_t entity,
                               const nlohmann::json& params) = 0;
        virtual void on_update(World& world, std::uint32_t entity,
                               float dt) = 0;
        virtual void on_detach(World& world, std::uint32_t entity) = 0;
        virtual std::string id() const = 0;
    };
    }

Registration macro (use EXACTLY this spelling, NOT ``REGISTER_MECHANIC``):

    PTV_REGISTER_MECHANIC("generated.<code_task_id>", YourClassName);

For kind=ai replace IMechanic with IBehaviour (`on_tick` instead of
`on_update`) and `PTV_REGISTER_BEHAVIOUR`. For kind=dialogue use
IDialogueHook with `on_enter` / `on_line` / `on_exit` and
`PTV_REGISTER_DIALOGUE_HOOK`.

# Accessing engine state

Everything you need lives on the EnTT registry inside ``ptv::World``:

    auto& r = world.registry();
    if (r.all_of<ptv::CTransform>(entity)) {
        auto& tr = r.get<ptv::CTransform>(entity).t;   // tr.position, tr.rotation, tr.scale
    }
    if (r.all_of<ptv::CTag>(entity)) {
        const auto& tags = r.get<ptv::CTag>(entity).tags;
    }

Available components (defined in ptv/ecs/World.h):

* CTransform { Transform t; }      // position/rotation/scale
* CMesh      { std::string asset_id; std::string glb_path; float target_scale_meters; }
* CPhysics   { std::string kind; float mass_kg; float restitution; float friction; std::string collision_shape; }
* CTag       { std::vector<std::string> tags; }
* CMechanicBinding { std::vector<Entry> entries; }  // Entry has id, builtin_id, params_json

``world.find("entity_id")`` returns the entity registered with that string id
(or ``entt::null``). ``world.delta_time()`` is the current frame dt (use the
``dt`` argument to ``on_update`` first; delta_time() is a fallback).

# What you DO NOT have

There is no input, haptics, audio-capture, speech-to-text, HTTP, save, UI,
timer, or locomotion service. If the CodeTask asks for one, emulate it
defensively:

* For input-driven mechanics, read a trigger state from ``params`` (e.g.
  ``params.value("grip_held", false)``). The engine test harness drives
  these params through the binding. Implement the simulation logic but
  do not call any external service.
* For speech/HTTP/save, branch on a ``params`` flag like
  ``params.value("offline_ok", true)`` and assume success when the flag is
  set. Do the gameplay state transitions correctly; leave the real
  integration as a one-line TODO comment at the point it would plug in.
* For anything physics-like, read ``CTransform`` / ``CPhysics`` on neighbour
  entities and compute purely in-memory.

# Minimum viable mechanic example (compiles as-is)

Below is the ONLY pattern you should follow. Keep the same anchor
(anonymous namespace optional, implementation inside ``ptv`` namespace,
``PTV_REGISTER_MECHANIC`` at the bottom, ``id()`` returning the same string).

    #include "ptv/api/IMechanic.h"
    #include "ptv/ecs/World.h"
    #include "ptv/math/Math.h"

    #include <nlohmann/json.hpp>
    #include <cmath>
    #include <string>

    namespace ptv {

    class ExampleBlink : public IMechanic {
    public:
        void on_attach(World&, std::uint32_t, const nlohmann::json& p) override {
            period_ = p.value("period_sec", 1.0f);
        }
        void on_update(World& w, std::uint32_t e, float dt) override {
            t_ += dt;
            auto& reg = w.registry();
            if (!reg.all_of<CTransform>(e)) return;
            auto& tr = reg.get<CTransform>(e).t;
            tr.scale = Vec3{1.0f + 0.1f * std::sin(6.2831853f * t_ / period_),
                            1.0f, 1.0f};
        }
        void on_detach(World&, std::uint32_t) override {}
        std::string id() const override { return "generated.example_blink"; }
    private:
        float t_{0.0f};
        float period_{1.0f};
    };

    }  // namespace ptv

    PTV_REGISTER_MECHANIC("generated.example_blink", ptv::ExampleBlink)

# Hard rules

1. Do NOT ``#include`` anything outside the lists above.
2. Do NOT redefine ``IMechanic`` / ``IBehaviour`` / ``IDialogueHook``; just
   ``#include`` the header and subclass the real type.
3. Use ``PTV_REGISTER_MECHANIC`` (or ``PTV_REGISTER_BEHAVIOUR`` /
   ``PTV_REGISTER_DIALOGUE_HOOK``) at the bottom of the file. Pass the
   CodeTask id as the string literal. NOT ``REGISTER_MECHANIC``.
4. Do NOT write ``class X : public ptv::IMechanic`` with custom method names
   like ``OnStart`` / ``Configure`` / ``Update(ctx)``. The only overridable
   methods are the four listed in the interface above.
5. Do NOT reference ``ctx.input`` / ``ctx.locomotion`` / ``ctx.haptics`` /
   ``ctx.time`` / ``ctx.save`` / ``ctx.ui``. No such context exists.
6. Keep the file self-contained and under 300 lines. If the spec is
   ambitious, stub the unused parts with comments rather than inventing
   APIs.
"""
