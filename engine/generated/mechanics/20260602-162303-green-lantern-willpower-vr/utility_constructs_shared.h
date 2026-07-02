#pragma once

#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <entt/entt.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <cmath>

namespace ptv {

// Helper: add a tag-string to an entity (creates CTag if missing)
inline void add_tag(World& w, entt::entity e, const std::string& s) {
    const std::uint32_t ue = static_cast<std::uint32_t>(e);
    if (!w.valid(ue)) return;
    if (!w.has<CTag>(ue)) {
        w.emplace_or_replace<CTag>(ue, CTag{});
    }
    auto& tags = w.get<CTag>(ue).tags;
    tags.emplace_back(s);
}

// Helper: fetch Transform pointer for entt::entity (nullptr if missing)
inline Transform* get_tr(World& w, entt::entity e) {
    const std::uint32_t ue = static_cast<std::uint32_t>(e);
    if (!w.valid(ue) || !w.has<CTransform>(ue)) return nullptr;
    return &w.get<CTransform>(ue).t;
}

// Read the last params_json blob for this mechanic on the bound entity.
inline nlohmann::json read_self_params(World& w, std::uint32_t entity, const std::string& self_id) {
    if (!w.has<CMechanicBinding>(entity)) return nlohmann::json::object();
    const auto& mb = w.get<CMechanicBinding>(entity);
    nlohmann::json merged = nlohmann::json::object();
    for (const auto& e : mb.entries) {
        if (e.id == self_id) {
            auto j = nlohmann::json::parse(e.params_json, nullptr, false);
            if (j.is_object()) {
                for (auto it = j.begin(); it != j.end(); ++it) merged[it.key()] = it.value();
            }
        }
    }
    return merged;
}

struct ScannerState {
    entt::entity e{entt::null};
    bool active{false};
    bool scanning{true};
    float pulse_timer{0.0f};
    float interval_s{2.0f};
    float range_m{18.0f};
};

struct GrabberState {
    entt::entity e{entt::null};
    bool active{false};
    float pull_force{35.0f};
};

struct RepairState {
    bool channel{false};
    entt::entity target{entt::null};
    float progress01{0.0f};
    float rate_per_s{0.25f};
};

struct BeaconState {
    entt::entity e{entt::null};
    bool active{false};
    float yaw_rad{0.0f};
    float intensity{1.0f};
};

struct BubbleState {
    entt::entity e{entt::null};
    bool active{false};
    entt::entity protected_target{entt::null};
    float hp{50.0f};
    float ttl_s{8.0f};
};

inline entt::entity make_child(World& w, const char* tag_label, const Transform& t) {
    auto& reg = w.registry();
    entt::entity c = reg.create();
    const std::uint32_t uc = static_cast<std::uint32_t>(c);
    w.emplace_or_replace<CTransform>(uc, CTransform{t});
    w.emplace_or_replace<CTag>(uc, CTag{});
    add_tag(w, c, std::string(tag_label));
    return c;
}

class UtilityConstructs : public IMechanic {
public:
    void on_attach(World& w, std::uint32_t e, const nlohmann::json& p) override;
    void on_update(World& w, std::uint32_t e, float dt) override;
    void on_detach(World& w, std::uint32_t e) override;
    std::string id() const override { return "generated.utility_constructs"; }

    // Public state for helper .cpp modules (kept simple for multi-file split)
    ScannerState scanner_{};
    GrabberState grabber_{};
    RepairState  repair_{};
    BeaconState  beacon_{};
    BubbleState  bubble_{};

private:
    // Spawn/dismiss helpers shared across files
    void dismiss_all(World& w);
};

// Feature helpers implemented per-file
namespace utility {
    void update_scanner(World& w, std::uint32_t ring_e, UtilityConstructs& uc, const nlohmann::json& p, float dt);
    void update_grabber(World& w, std::uint32_t ring_e, UtilityConstructs& uc, const nlohmann::json& p, float dt);
    void update_repair(World& w, std::uint32_t ring_e, UtilityConstructs& uc, const nlohmann::json& p, float dt);
    void update_beacon(World& w, std::uint32_t ring_e, UtilityConstructs& uc, const nlohmann::json& p, float dt);
    void update_bubble(World& w, std::uint32_t ring_e, UtilityConstructs& uc, const nlohmann::json& p, float dt);
}

} // namespace ptv
