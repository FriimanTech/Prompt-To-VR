#include "ptv/api/IDialogueHook.h"
#include "ptv/ecs/World.h"
#include <nlohmann/json.hpp>
#include <entt/entt.hpp>

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace ptv {

struct MentorState {
    bool subtitles_only{false};
    std::string last_line;
};

class MentorGuidanceDialogue final : public IDialogueHook {
public:
    void on_enter(World& world, std::uint32_t entity, const nlohmann::json& params) override {
        MentorState st;
        st.subtitles_only = params.value("mute_vo", false);
        states_[entity] = st;

        auto mentor = world.find("mentor_npc");
        if (mentor != entt::null) {
            world.emplace_or_replace<CTag>(static_cast<std::uint32_t>(mentor),
                CTag{std::vector<std::string>{"mentor_dialogue_active"}});
        }

        world.emplace_or_replace<CTag>(entity,
            CTag{std::vector<std::string>{"subtitle_speaker:Mentor"}});
    }

    void on_line(World& world, std::uint32_t entity, const std::string& line, const nlohmann::json& meta) override {
        auto it = states_.find(entity);
        if (it == states_.end()) return;
        MentorState& st = it->second;

        std::string speaker = meta.value("speaker", std::string("Mentor"));
        st.last_line = line;

        world.emplace_or_replace<CTag>(entity,
            CTag{std::vector<std::string>{std::string("subtitle:") + speaker + ": " + line}});
    }

    void on_exit(World& world, std::uint32_t entity) override {
        auto mentor = world.find("mentor_npc");
        if (mentor != entt::null) {
            world.emplace_or_replace<CTag>(static_cast<std::uint32_t>(mentor),
                CTag{std::vector<std::string>{}});
        }
        states_.erase(entity);
    }

    std::string id() const override { return "generated.mentor_guidance_dialogue"; }

private:
    std::unordered_map<std::uint32_t, MentorState> states_;
};

} // namespace ptv

PTV_REGISTER_DIALOGUE_HOOK("generated.mentor_guidance_dialogue", ptv::MentorGuidanceDialogue)
