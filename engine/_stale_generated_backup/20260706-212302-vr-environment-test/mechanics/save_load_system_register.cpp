#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <nlohmann/json.hpp>
#include <string>
#include <algorithm>

#include "save_load_system_shared.h"

namespace ptv {

class SaveLoadSystem : public IMechanic {
public:
    void on_attach(World& world, std::uint32_t entity, const nlohmann::json& params) override {
        // Boot-time load: prefer any mirrored blob on entity, else keep global, else defaults.
        nlohmann::json base;
        if (auto j = read_from_tag(world, entity_ = entity)) base = *j;
        else if (!ptv_profile_current().is_null() && ptv_profile_current().is_object()) base = ptv_profile_current();
        else base = default_profile();

        // Optionally apply incoming defaults/overrides from params
        if (params.is_object()) {
            if (params.contains("defaults") && params.at("defaults").is_object()) {
                base.merge_patch(params.at("defaults"));
            }
        }

        // Ensure presence on the entity tag for other systems/tests to read.
        mirror_into_tag(world, entity, base);

        // Optionally auto-save on boot to stamp reason for traceability in tests.
        ptv_save_now(world, entity, "boot_load");
    }

    void on_update(World& world, std::uint32_t entity, float /*dt*/) override {
        if (entity_ == 0 || !world.valid(entity_)) return;
        // Poll our binding params for triggers and deltas
        if (!world.has<CMechanicBinding>(entity_)) return;
        auto& bind = world.get<CMechanicBinding>(entity_);
        for (const auto& entry : bind.entries) {
            if (entry.id == id() || entry.builtin_id == id()) {
                auto j = nlohmann::json::parse(entry.params_json, nullptr, false);
                if (!j.is_object()) continue;

                // Settings change delta: {"settings_delta": {"locomotion": {"mode": "slide"}}}
                if (j.contains("settings_delta") && j.at("settings_delta").is_object()) {
                    auto delta = j.at("settings_delta");
                    // Apply shallow object merge under settings
                    auto base = ptv_profile_get(world, entity_, "");
                    if (!base.is_object()) base = nlohmann::json::object();
                    // merge_patch() returns void (mutates in place), so it
                    // cannot be used inside a ?: expression.
                    if (base.contains("settings") && base["settings"].is_object()) {
                        base["settings"].merge_patch(delta);
                    } else {
                        base["settings"] = delta;
                    }
                    // Mirror back
                    ptv_profile_set(world, entity_, "settings", base["settings"]);
                    ptv_save_now(world, entity_, "settings_change");
                }

                // Mission / unlock / recharge autosave flags
                if (j.value("mission_accept", false)) {
                    ptv_save_now(world, entity_, "mission_accept");
                }
                if (j.value("mission_complete", false)) {
                    // Optionally update last_completed if provided
                    if (j.contains("mission_id") && j.at("mission_id").is_string()) {
                        ptv_profile_set(world, entity_, "mission.last_completed", j.at("mission_id"));
                    }
                    ptv_save_now(world, entity_, "mission_complete");
                }
                if (j.value("unlock_purchase", false)) {
                    if (j.contains("unlock_id") && j.at("unlock_id").is_string()) {
                        auto cur = ptv_profile_get(world, entity_, "constructs.unlocked");
                        if (!cur.is_array()) cur = nlohmann::json::array();
                        cur.push_back(j.at("unlock_id"));
                        ptv_profile_set(world, entity_, "constructs.unlocked", cur);
                    }
                    ptv_save_now(world, entity_, "unlock_purchase");
                }
                if (j.value("recharge_complete", false)) {
                    ptv_save_now(world, entity_, "recharge_complete");
                }

                // Manual save request (e.g. from wrist HUD in hub scenes)
                if (j.value("manual_save", false)) {
                    ptv_save_now(world, entity_, "manual_save");
                }

                // Difficulty change
                if (j.contains("difficulty") && j.at("difficulty").is_string()) {
                    ptv_profile_set(world, entity_, "difficulty", j.at("difficulty"));
                    ptv_save_now(world, entity_, "difficulty_change");
                }

                // Accessibility flags
                if (j.contains("accessibility") && j.at("accessibility").is_object()) {
                    ptv_profile_set(world, entity_, "settings.options.accessibility", j.at("accessibility"));
                    ptv_save_now(world, entity_, "accessibility_change");
                }
            }
        }
    }

    void on_detach(World& world, std::uint32_t entity) override {
        // Final autosave on detach for safety.
        if (world.valid(entity)) ptv_save_now(world, entity, "detach_autosave");
        entity_ = 0;
    }

    std::string id() const override { return "generated.save_load_system"; }

private:
    // Local helpers reused from API TU via declarations below
    static std::optional<nlohmann::json> read_from_tag(World& world, std::uint32_t entity);
    static void mirror_into_tag(World& world, std::uint32_t entity, const nlohmann::json& blob);
    static nlohmann::json default_profile();

    std::uint32_t entity_{0};
};

// Forward declarations match those defined in save_load_system_api.cpp
static std::optional<nlohmann::json> read_from_tag(World& world, std::uint32_t entity);
static void mirror_into_tag(World& world, std::uint32_t entity, const nlohmann::json& blob);
static nlohmann::json default_profile();

PTV_REGISTER_MECHANIC("generated.save_load_system", SaveLoadSystem)

} // namespace ptv
