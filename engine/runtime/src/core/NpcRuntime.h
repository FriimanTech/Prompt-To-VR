#pragma once

// Pieces of the Application's NPC / combat plumbing that are pure data, kept
// here so they can be tested without a Vulkan + OpenXR Application.

#include <cstddef>
#include <cstdint>
#include <deque>

#include "ptv/api/MechanicContext.h"
#include "ptv/ecs/World.h"

namespace ptv {

/** The recent hits ICombat publishes through ``damage_seq`` and
 *  ``damage_events_since``. */
class DamageLog {
public:
    static constexpr std::size_t kMaxEvents = 64;

    /** Append a landed hit and return its sequence number. */
    std::uint64_t record(DamageEvent ev) {
        ev.seq = ++next_seq_;
        events_.push_back(ev);
        if (events_.size() > kMaxEvents) events_.pop_front();
        return ev.seq;
    }

    std::uint64_t last_seq(std::uint32_t target) const {
        for (auto it = events_.rbegin(); it != events_.rend(); ++it) {
            if (it->target == target) return it->seq;
        }
        return 0;
    }

    /** ``target`` 0xFFFFFFFF = any target. Oldest first. */
    int since(std::uint32_t target, std::uint64_t after_seq, DamageEvent* out,
              int max_out) const {
        if (out == nullptr || max_out <= 0) return 0;
        int n = 0;
        for (const auto& ev : events_) {
            if (ev.seq <= after_seq) continue;
            if (target != 0xFFFFFFFFu && ev.target != target) continue;
            out[n++] = ev;
            if (n >= max_out) break;
        }
        return n;
    }

    void clear() { events_.clear(); }

private:
    std::deque<DamageEvent> events_{};
    // Never reset, so a cursor a mechanic held across a clear() cannot make
    // a new hit look old.
    std::uint64_t next_seq_{0};
};

/** The scene entity that should BE the player: the one tagged
 *  ``player_rig`` that carries no drawable mesh. ``entt::null`` when the
 *  scene has none, in which case the engine makes its own. */
inline entt::entity find_adoptable_player_rig(World& world) {
    for (const auto cand : world.registry().view<CTag>()) {
        const auto id = static_cast<std::uint32_t>(cand);
        if (!has_tag(world, id, "player_rig")) continue;
        const auto* cm = world.registry().try_get<CMesh>(cand);
        if (cm != nullptr && !cm->glb_path.empty()) continue;
        return cand;
    }
    return entt::null;
}

}  // namespace ptv
