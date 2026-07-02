#include "ptv/ecs/World.h"
#include <entt/entt.hpp>
#include <string>
#include <vector>
#include <cstdint>
#include <algorithm>

#include "haptics_shared.h"

namespace ptv_haptics {

// Compute final amplitude and duration for a request
void resolve_play_params(const PublishReq& req,
                                float user_intensity_scale,
                                const std::unordered_map<std::string, PatternDef>& defs,
                                float& out_amp,
                                float& out_dur_s) {
    auto it = defs.find(req.pattern_id);
    const PatternDef* def = (it != defs.end()) ? &it->second : nullptr;
    const float base_amp = def ? def->base_amp : 0.6f;
    const float base_dur = def ? def->default_duration_s : 0.08f;
    out_dur_s = req.duration_s.has_value() ? std::max(0.01f, *req.duration_s) : base_dur;
    out_amp = ptv_haptics::clamp01(base_amp * req.intensity_scale * user_intensity_scale);
}

// Consume one publish tag string. Returns true if consumed.
bool try_consume_publish_tag(const std::string& tag_str, PublishReq& out) {
    using namespace ptv_haptics;
    if (tag_str.rfind(std::string(kPublishPrefix), 0) != 0) return false; // no prefix
    const std::string json_part = tag_str.substr(std::string(kPublishPrefix).size());
    auto parsed = parse_publish_json(json_part);
    if (!parsed) return false;
    out = *parsed;
    return true;
}

// Emit one or two play tags onto the target entity's CTag
void emit_play_tags(ptv::World& w, std::uint32_t target_entity,
                    const std::string& pattern, ptv_haptics::Hand hand,
                    float amp, float dur_s, ptv_haptics::RecentEvents& recent) {
    using namespace ptv_haptics;
    if (!w.has<ptv::CTag>(target_entity)) {
        w.emplace_or_replace<ptv::CTag>(target_entity, ptv::CTag{});
    }
    auto& tag = w.get<ptv::CTag>(target_entity);

    auto push = [&](Hand h){
        std::string payload = std::string(kPlayPrefix) + make_play_json(pattern, h, amp, dur_s);
        tag.tags.emplace_back(std::move(payload));
        recent.push({pattern, std::string(to_string(h)), amp, dur_s});
    };

    if (hand == Hand::Both) {
        push(Hand::Left);
        push(Hand::Right);
    } else {
        push(hand);
    }
}

} // namespace ptv_haptics
