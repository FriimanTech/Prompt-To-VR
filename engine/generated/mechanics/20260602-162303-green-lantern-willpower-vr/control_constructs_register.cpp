#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <optional>

#include "control_constructs_shared.h"

namespace ptv {

// Forward from integration shim
void construct_core_advertise_if_needed(World& w, std::uint32_t hand_e);

class ControlConstructsExtension : public IMechanic {
public:
    void on_attach(World& w, std::uint32_t entity, const nlohmann::json& params) override {
        (void)params;
        self_entity_ = entity;
        // Advertise our entries to construct core once
        construct_core_advertise_if_needed(w, entity);
        // Defaults are taken from parse each frame; nothing else to cache.
    }

    void on_update(World& w, std::uint32_t entity, float dt) override {
        // Parse per-frame inputs and config derived from our binding entry
        auto in = parse_control_binding_params(w, entity, id());

        // Resolve refined/current target if any
        std::uint32_t target = 0;
        if (!in.refined_target_id.empty()) target = find_entity(w, in.refined_target_id);

        // Selection routing
        if (in.active_control == "chain_tether") {
            update_tether(w, entity, target, in, dt);
        } else if (in.active_control == "restraining_bands") {
            update_bands(w, entity, target, in, dt);
        }
    }

    void on_detach(World& w, std::uint32_t entity) override {
        (void)w; (void)entity;
        tether_.active = false;
        band_targets_.clear();
    }

    std::string id() const override { return "generated.control_constructs_extension"; }

private:
    void update_tether(World& w, std::uint32_t hand_e, std::uint32_t target,
                       const ParsedControlInput& in, float dt) {
        // Fire to latch if RT pressed and we have a target and no active tether
        if (in.right_trigger && !tether_.active && w.valid(target)) {
            tether_.active = true;
            tether_.target_entity = target;
            tether_.anchor_ws = (w.has<CTransform>(hand_e) ? w.get<CTransform>(hand_e).t.position : Vec3{0,0,0});
            tether_.tension_norm = 0.0f;
            // Mark target with a transient tethered tag (consumed by other systems if any)
            nlohmann::json j; j["by"] = "ring"; j["from_entity"] = hand_e; j["max_len_m"] = in.tether_max_length_m;
            ensure_tag(w, target, std::string("status.tethered:") + j.dump());
            publish_haptics(w, hand_e, "construct_form_heavy", "right", 0.7f, 0.12f);
        }

        // Step simulation if active
        if (tether_.active) {
            if (!step_tether(w, hand_e, dt, in, tether_)) {
                // broke this frame
                tether_.active = false;
            }
        }

        // If no target or we switched selection, ensure cleanup when inactive
        if (!tether_.active && w.valid(target)) {
            remove_tag_prefix(w, target, "status.tethered");
        }
    }

    void update_bands(World& w, std::uint32_t hand_e, std::uint32_t target,
                      const ParsedControlInput& in, float dt) {
        // Apply on activation
        if (in.right_trigger && w.valid(target)) {
            auto& br = band_targets_[target];
            apply_bands_now(w, target, in, br);
            publish_haptics(w, hand_e, "bolt_fire_sharp", "right", 0.6f, 0.06f);
        }
        // Decay all tracked band targets
        std::vector<std::uint32_t> to_erase;
        for (auto& kv : band_targets_) {
            auto te = kv.first; auto& br = kv.second;
            if (!w.valid(te)) { to_erase.push_back(te); continue; }
            decay_bands(w, te, dt, in, br);
            if (br.stacks == 0) to_erase.push_back(te);
        }
        for (auto e : to_erase) band_targets_.erase(e);
    }

private:
    std::uint32_t self_entity_{0};
    TetherRuntime tether_{};
    std::unordered_map<std::uint32_t, BandsRuntime> band_targets_{};
};

} // namespace ptv

PTV_REGISTER_MECHANIC("generated.control_constructs_extension", ptv::ControlConstructsExtension)
