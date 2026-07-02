#include "mentor_dialogue_shared.h"
#include "ptv/ecs/World.h"

#include <nlohmann/json.hpp>
#include <string>

namespace ptv {

struct ParsedTrigger {
    std::string topic;
    std::string scene;
    std::string failure_kind;
    bool mute_vo{false};
};

ParsedTrigger parse_trigger_params(const nlohmann::json& j) {
    ParsedTrigger out;
    if (j.is_object()) {
        out.topic = j.value("topic", std::string{});
        out.scene = j.value("scene", std::string{});
        out.failure_kind = j.value("failure_kind", std::string{});
        out.mute_vo = j.value("mute_vo", false);
        // Accessibility hint: if subtitles_only true, also mute VO
        if (j.value("subtitles_only", false)) out.mute_vo = true;
    }
    if (out.topic.empty()) out.topic = "onboarding"; // sensible default
    return out;
}

// Emit lightweight tag protocol via CTag strings on the mentor entity.
// Other systems (HUD, VFX, audio) may consume these.
//  - subtitle.show:{"speaker":"..","text":"..","ttl":3.0}
//  - dialogue.play:{"speaker":"..","text":".."}
//  - vfx.spawn:{"asset_id":"..","ttl_s":2.0}
void emit_line_tags(World& world, std::uint32_t entity,
                    const MentorLine& line,
                    bool mute_vo) {
    if (!world.valid(entity)) return;
    // Push subtitles
    {
        std::string payload = std::string("subtitle.show:{\"speaker\":\"") + line.speaker +
                              "\",\"text\":\"" + line.text +
                              "\",\"ttl\":" + std::to_string(line.ttl_s) + "}";
        world.emplace_or_replace<CTag>(entity, CTag{ std::vector<std::string>{ payload } });
    }
    // Optionally push VO request
    if (!mute_vo) {
        std::string payload = std::string("dialogue.play:{\"speaker\":\"") + line.speaker +
                              "\",\"text\":\"" + line.text + "\"}";
        if (world.has<CTag>(entity)) {
            auto tags = world.get<CTag>(entity).tags; // copy then replace with appended
            tags.push_back(payload);
            world.emplace_or_replace<CTag>(entity, CTag{ std::move(tags) });
        } else {
            world.emplace_or_replace<CTag>(entity, CTag{ std::vector<std::string>{ payload } });
        }
    }
    // Optional VFX hint
    if (line.vfx_asset_id.has_value()) {
        std::string payload = std::string("vfx.spawn:{\"asset_id\":\"") + *line.vfx_asset_id +
                              "\",\"ttl_s\":2.0}";
        if (world.has<CTag>(entity)) {
            auto tags = world.get<CTag>(entity).tags;
            tags.push_back(payload);
            world.emplace_or_replace<CTag>(entity, CTag{ std::move(tags) });
        } else {
            world.emplace_or_replace<CTag>(entity, CTag{ std::vector<std::string>{ payload } });
        }
    }
}

} // namespace ptv
