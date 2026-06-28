#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <entt/entt.hpp>
#include <string>
#include <vector>

// This unit offers optional helpers for tests to query tags quickly by name.
// It does not expose any new symbols; it simply keeps mechanic-related helpers in one translation unit if future split is needed.

namespace ptv {
namespace progression_int {

static bool has_tag(World& w, entt::entity e, const std::string& tag) {
    if (e == entt::null) return false;
    auto ee = static_cast<std::uint32_t>(e);
    if (!w.has<CTag>(ee)) return false;
    const auto& t = w.get<CTag>(ee);
    return std::find(t.tags.begin(), t.tags.end(), tag) != t.tags.end();
}

static bool any_construct_family_unlocked(World& w) {
    auto& reg = w.registry();
    auto view = reg.view<CTag>();
    for (auto ent : view) {
        const auto& t = view.get<CTag>(ent);
        for (const auto& s : t.tags) {
            if (s.rfind("unlocked_family.", 0) == 0) return true;
        }
    }
    return false;
}

} // namespace progression_int
} // namespace ptv
