// builtin.mirror_surface -- see yourself.
//
// Needed for more than vanity: the whole point of suppressing the avatar's
// head in the player's own view is that it stays visible everywhere else, and
// without a second view of the avatar that distinction is untestable and
// invisible. Avatar customization, wardrobe menus and dress-up games all
// depend on it too.
//
// This mechanic owns POLICY; the renderer owns the reflection itself:
//
//   * which mirror is eligible (the nearest one within range, one at a time),
//   * the plane it reflects about, derived from the entity's own orientation
//     so a scene only has to place and rotate a quad,
//   * what appears in the reflection, and the cap on how much.
//
// The reflection is drawn as mirrored geometry straight into the output
// buffer rather than through a render-to-texture pass. On the tile-based GPU
// in a Quest an offscreen target forces a GMEM round-trip and bandwidth is
// the bottleneck; Meta's own mobile-VR guidance is that the simplest mirror
// is a reflected copy of the geometry. Published Quest 3 numbers put a
// full-scene stereo mirror at roughly 2.5-3.5 ms, which is why the reflected
// set here is the avatar and what it is wearing rather than the whole world.

#include <algorithm>
#include <string>
#include <vector>

#include "../BuiltinMechanicBase.h"

#include <glm/gtc/quaternion.hpp>

namespace ptv {
namespace {

class MirrorSurfaceMechanic
    : public BuiltinMechanicBase<MirrorSurfaceMechanic> {
public:
    static constexpr const char* kId = "builtin.mirror_surface";

    // Simulate: the avatar has been posed by builtin.avatar_body already this
    // frame, and the renderer reads the component immediately after, so the
    // reflection is never a frame behind the body it reflects.
    MechanicPhase phase() const override { return MechanicPhase::Simulate; }
    MechanicScope scope() const override { return MechanicScope::Entity; }

    void configure(const nlohmann::json& p) {
        // Local-space normal of the mirror plane. Default +Z, matching a quad
        // authored facing the viewer.
        if (p.contains("plane_normal") && p["plane_normal"].is_array() &&
            p["plane_normal"].size() == 3) {
            local_normal_ = Vec3(p["plane_normal"][0].get<float>(),
                                 p["plane_normal"][1].get<float>(),
                                 p["plane_normal"][2].get<float>());
        }
        max_distance_ =
            std::clamp(p.value("max_active_distance_m", 6.0f), 0.5f, 40.0f);
        max_reflected_ = std::clamp(p.value("max_reflected", 24), 1, 128);
        reflect_tags_.clear();
        if (p.contains("reflect_tags") && p["reflect_tags"].is_array()) {
            for (const auto& t : p["reflect_tags"]) {
                if (t.is_string()) reflect_tags_.push_back(t.get<std::string>());
            }
        }
    }

    void tick(World& world, std::uint32_t self, float, const InputSnapshot&,
              MechanicContext& ctx) {
        if (!world.has<CTransform>(self)) return;
        const auto& t = world.get<CTransform>(self).t;

        auto& m = world.registry().get_or_emplace<CMirrorSurface>(
            static_cast<entt::entity>(self));

        // Rotate the authored normal into world space, so a scene places and
        // turns a quad and the plane follows without extra configuration.
        const Vec3 n = t.rotation * local_normal_;
        m.normal = glm::length(n) > 1.0e-4f ? n : Vec3(0.0f, 0.0f, 1.0f);
        m.max_distance_m = max_distance_;
        m.max_reflected = max_reflected_;
        if (m.reflect_tags != reflect_tags_) m.reflect_tags = reflect_tags_;

        // Only reflect while the player is close enough to see it, and only
        // from the front: standing behind a mirror should cost nothing.
        bool eligible = false;
        if (ctx.locomotion != nullptr) {
            const Vec3 viewer = ctx.locomotion->position();
            const Vec3 to_viewer = viewer - t.position;
            const float distance = glm::length(to_viewer);
            eligible = distance <= max_distance_ &&
                       glm::dot(to_viewer, m.normal) > 0.0f;
        }
        m.active = eligible;
    }

    void on_detach(World& world, std::uint32_t self) override {
        // Leaving the component behind with active=true would keep the
        // renderer paying for a reflection nothing is driving any more.
        const auto e = static_cast<entt::entity>(self);
        if (world.valid(self) && world.registry().all_of<CMirrorSurface>(e)) {
            world.registry().get<CMirrorSurface>(e).active = false;
        }
    }

private:
    Vec3 local_normal_{0.0f, 0.0f, 1.0f};
    float max_distance_{6.0f};
    int max_reflected_{24};
    std::vector<std::string> reflect_tags_;
};

}  // namespace

// Anchor so MSVC keeps this TU (and its registration) when linking
// ptv_runtime as a static library. Referenced by BuiltinMechanics.cpp.
extern "C" int ptv_force_link_builtin_mirror_surface = 1;

PTV_REGISTER_MECHANIC("builtin.mirror_surface", MirrorSurfaceMechanic)

}  // namespace ptv
