#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <entt/entt.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <utility>
#include <cstdint>
#include <algorithm>

#include "haptics_shared.h"

// Forward decl from patterns unit
namespace ptv_haptics { const std::unordered_map<std::string, PatternDef>& pattern_map(); }

// Service helpers
namespace ptv_haptics {
    bool try_consume_publish_tag(const std::string& tag_str, PublishReq& out);
    void resolve_play_params(const PublishReq& req,
                             float user_intensity_scale,
                             const std::unordered_map<std::string, PatternDef>& defs,
                             float& out_amp, float& out_dur_s);
    void emit_play_tags(ptv::World& w, std::uint32_t target_entity,
                        const std::string& pattern, Hand hand,
                        float amp, float dur_s, RecentEvents& recent);
}

namespace ptv {

class HapticsManager final : public IMechanic {
public:
    void on_attach(World& world, std::uint32_t entity, const nlohmann::json& params) override {
        self_ = entity;
        // Global/user intensity scale (fallback until Accessibility Settings task exists)
        user_intensity_ = params.value("default_intensity", 1.0f);
        user_intensity_ = std::clamp(user_intensity_, 0.0f, 2.0f);
        // Optional debug: keep more recent events
        recent_.max_items = static_cast<std::size_t>(std::clamp(params.value("recent_events", 32), 1, 256));
    }

    void on_update(World& world, std::uint32_t /*entity*/, float /*dt*/) override {
        // Scan all entities for publish tags, consume, and emit resolved play tags onto self_
        auto& defs = ptv_haptics::pattern_map();
        auto& reg = world.registry();
        auto view = reg.view<CTag>();
        for (auto e : view) {
            std::uint32_t ent = static_cast<std::uint32_t>(e);
            auto& ctag = view.get<CTag>(e);
            if (ctag.tags.empty()) continue;

            // Walk and collect indices to erase after processing to avoid iterator invalidation
            std::vector<std::size_t> to_erase;
            for (std::size_t i = 0; i < ctag.tags.size(); ++i) {
                const std::string& t = ctag.tags[i];
                ptv_haptics::PublishReq req;
                if (!ptv_haptics::try_consume_publish_tag(t, req)) continue;

                // Resolve parameters
                float amp = 0.6f, dur = 0.08f;
                ptv_haptics::resolve_play_params(req, user_intensity_, defs, amp, dur);

                // Emit onto manager's entity so device/plugin can read a single place
                if (world.valid(self_)) {
                    ptv_haptics::emit_play_tags(world, self_, req.pattern_id, req.hand, amp, dur, recent_);
                }

                to_erase.push_back(i);
            }
            if (!to_erase.empty()) {
                // Erase from back to front
                std::sort(to_erase.begin(), to_erase.end());
                for (std::size_t k = 0; k < to_erase.size(); ++k) {
                    std::size_t idx = to_erase[to_erase.size() - 1 - k];
                    if (idx < ctag.tags.size()) ctag.tags.erase(ctag.tags.begin() + static_cast<std::ptrdiff_t>(idx));
                }
            }
        }

        // Optional: self-test hook to guarantee pattern presence for acceptance
        // Add a one-time map-export tag listing supported patterns if asked via params flag
        if (!announced_patterns_ && announce_supported_) {
            announced_patterns_ = true;
            if (!world.has<CTag>(self_)) world.emplace_or_replace<CTag>(self_, CTag{});
            auto& t = world.get<CTag>(self_);
            nlohmann::json j;
            j["patterns"] = nlohmann::json::array();
            for (const auto& kv : ptv_haptics::pattern_map()) j["patterns"].push_back(kv.first);
            t.tags.emplace_back(std::string(ptv_haptics::kPlayPrefix) + j.dump());
        }
    }

    void on_detach(World& world, std::uint32_t entity) override {
        // Best-effort: drop any lingering play announcements on detach
        if (world.has<CTag>(entity)) {
            auto& t = world.get<CTag>(entity);
            auto& vec = t.tags;
            vec.erase(std::remove_if(vec.begin(), vec.end(), [](const std::string& s){
                return s.rfind(std::string(ptv_haptics::kPlayPrefix), 0) == 0;
            }), vec.end());
        }
    }

    std::string id() const override { return "generated.haptics_manager"; }

private:
    std::uint32_t self_{0};
    float user_intensity_{1.0f};
    ptv_haptics::RecentEvents recent_{};
    bool announce_supported_{true};
    bool announced_patterns_{false};
};

} // namespace ptv

PTV_REGISTER_MECHANIC("generated.haptics_manager", ptv::HapticsManager)
