#pragma once

#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <entt/entt.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <optional>

namespace ptv {

// Lightweight per-entity tutorial progress tracked by this mechanic.
struct CTutorialLocalFlags {
    bool intro_done{false};
    bool fired_bolt{false};
    bool raised_shield{false};
    bool placed_platform{false};
    bool all_done() const { return intro_done && fired_bolt && raised_shield && placed_platform; }
};

// Internal state kept by the mechanic per attached entity.
struct TG_RuntimeState {
    // Visuals
    float t{0.0f};
    float pulse{0.0f};
    float success_flash{0.0f};
    bool is_projector{false};

    // Flow
    bool enabled{true};
    bool allow_skip{true};
    bool force_replay{false};

    // Logical flags snapshot (mirrored into CTag for visibility to other systems)
    CTutorialLocalFlags flags{};

    // Current tip text (exported via CTag tag string for UI layer to read)
    std::string tip;
};

// Simple helper: add or replace a tag on an entity to carry tutorial tip / flags.
void tg_add_or_replace_tag(World& world, std::uint32_t e, std::string_view key, std::string_view value);

// Helper: set flag tags like tutorial.completed.fire_bolt etc.
void tg_sync_flag_tags(World& world, std::uint32_t e, const CTutorialLocalFlags& f);

// Helper: try read events from params ("events": ["fired_bolt"]).
std::vector<std::string> tg_read_events_from_params(const nlohmann::json& params);

// Find one player-avatar entity (first with CTag containing "player_avatar").
std::optional<std::uint32_t> tg_find_player_avatar(World& world);

// Compose a concise tip string for the current gate.
std::string tg_compose_tip(const CTutorialLocalFlags& f, bool is_projector);

class TutorialGuidance : public IMechanic {
public:
    void on_attach(World& world, std::uint32_t entity, const nlohmann::json& params) override;
    void on_update(World& world, std::uint32_t entity, float dt) override;
    void on_detach(World& world, std::uint32_t entity) override;
    std::string id() const override { return "generated.tutorial_guidance"; }

private:
    std::unordered_map<std::uint32_t, TG_RuntimeState> by_entity_{};  // per-attachment state
};

} // namespace ptv
