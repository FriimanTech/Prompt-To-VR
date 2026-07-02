#include "ptv/api/IDialogueHook.h"
#include "ptv/ecs/World.h"
#include <nlohmann/json.hpp>

#include <unordered_map>
#include <utility>
#include <string>

#include "mentor_dialogue_shared.h"

namespace ptv {

// Forward from mentor_lines.cpp
std::vector<MentorLine> build_lines_for_topic(const std::string& topic,
                                              const std::string& scene,
                                              const std::string& failure_kind);
// Forward from mentor_triggers.cpp
struct ParsedTrigger { std::string topic; std::string scene; std::string failure_kind; bool mute_vo; };
ParsedTrigger parse_trigger_params(const nlohmann::json& j);
void emit_line_tags(World& world, std::uint32_t entity, const MentorLine& ml, bool mute_vo);

class MentorGuidanceDialogue final : public IDialogueHook {
public:
    void on_enter(World& world, std::uint32_t entity, const nlohmann::json& params) override {
        auto parsed = parse_trigger_params(params);
        MentorSessionState st;
        st.lines = build_lines_for_topic(parsed.topic, parsed.scene, parsed.failure_kind);
        st.cursor = 0;
        st.mute_vo = parsed.mute_vo;
        sessions_[entity] = std::move(st);
        // Emit first line immediately
        auto& s = sessions_[entity];
        if (!s.lines.empty()) {
            emit_line_tags(world, entity, s.lines.front(), s.mute_vo);
            s.cursor = 1;
        }
        // Register a UI toast so the player knows how to advance (if the trigger supports it)
        {
            std::string toast = "ui.toast:{\"text\":\"Mentor guidance active — proceed at your pace.\",\"ttl\":2.0}";
            if (world.has<CTag>(entity)) {
                auto tags = world.get<CTag>(entity).tags;
                tags.push_back(toast);
                world.emplace_or_replace<CTag>(entity, CTag{ std::move(tags) });
            } else {
                world.emplace_or_replace<CTag>(entity, CTag{ std::vector<std::string>{ toast } });
            }
        }
    }

    // on_line is called when the driving system requests progression.
    // Expect params.next(bool) or params.ack(bool) to advance.
    void on_line(World& world, std::uint32_t entity, const std::string& line, const nlohmann::json& meta) override {
        auto it = sessions_.find(entity);
        if (it == sessions_.end()) return;
        bool next = meta.value("next", meta.value("ack", true));
        if (!next) return;
        auto& s = it->second;
        if (s.cursor < s.lines.size()) {
            emit_line_tags(world, entity, s.lines[s.cursor], s.mute_vo);
            s.cursor++;
        } else {
            // No more lines: publish a gentle close signal the UI/HUD can consume.
            const char* k = "ui.toast:{\"text\":\"Guidance complete.\",\"ttl\":1.5}";
            if (world.has<CTag>(entity)) {
                auto tags = world.get<CTag>(entity).tags;
                tags.push_back(k);
                world.emplace_or_replace<CTag>(entity, CTag{ std::move(tags) });
            } else {
                world.emplace_or_replace<CTag>(entity, CTag{ std::vector<std::string>{ k } });
            }
        }
    }

    void on_exit(World& world, std::uint32_t entity) override {
        (void)world;
        sessions_.erase(entity);
    }

    std::string id() const override { return "generated.mentor_guidance_dialogue"; }

private:
    std::unordered_map<std::uint32_t, MentorSessionState> sessions_{};
};

} // namespace ptv

PTV_REGISTER_DIALOGUE_HOOK("generated.mentor_guidance_dialogue", ptv::MentorGuidanceDialogue)
