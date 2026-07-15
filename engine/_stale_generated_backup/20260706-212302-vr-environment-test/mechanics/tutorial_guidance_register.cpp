#include "tutorial_guidance_shared.h"

#include <string>

namespace ptv {

static bool tag_list_contains(const std::vector<std::string>& tags, std::string_view q) {
    for (auto& t : tags) if (t == q) return true; return false;
}

void TutorialGuidance::on_attach(World& world, std::uint32_t entity, const nlohmann::json& params) {
    TG_RuntimeState st{};
    st.enabled = params.value("enabled", true);
    st.allow_skip = params.value("allow_skip", true);
    st.force_replay = params.value("replay", false);

    // Determine if this attachment is on a projector vs mentor hologram by tag hints.
    if (world.has<CTag>(entity)) {
        auto& tg = world.get<CTag>(entity);
        st.is_projector = tag_list_contains(tg.tags, "projector") || tag_list_contains(tg.tags, "tutorial_projector") || tag_list_contains(tg.tags, "display");
    }

    // Respect a global disable via player settings tag if present.
    if (auto player = tg_find_player_avatar(world)) {
        if (world.has<CTag>(*player)) {
            auto& pt = world.get<CTag>(*player);
            if (tag_list_contains(pt.tags, "settings.tutorials.disabled")) {
                st.enabled = false;
            }
        }
    }

    // If replay forced, clear local flags; otherwise mirror any pre-existing completion on this entity (from tags).
    if (!st.force_replay && world.has<CTag>(entity)) {
        auto& tg = world.get<CTag>(entity);
        st.flags.intro_done      = tag_list_contains(tg.tags, "tutorial.completed.intro");
        st.flags.fired_bolt      = tag_list_contains(tg.tags, "tutorial.completed.fire_bolt");
        st.flags.raised_shield   = tag_list_contains(tg.tags, "tutorial.completed.raise_shield");
        st.flags.placed_platform = tag_list_contains(tg.tags, "tutorial.completed.place_platform");
    }

    st.tip = tg_compose_tip(st.flags, st.is_projector);
    by_entity_.emplace(entity, std::move(st));

    // Tag presence to help UI discover this attachment.
    tg_add_or_replace_tag(world, entity, "tutorial.attached", "true");
    tg_add_or_replace_tag(world, entity, "tutorial.tip", by_entity_[entity].tip);
}

void TutorialGuidance::on_detach(World& world, std::uint32_t entity) {
    // Best-effort: leave completion tags but remove volatile tip tag.
    if (world.has<CTag>(entity)) {
        auto& tg = world.get<CTag>(entity);
        tg.tags.erase(std::remove(tg.tags.begin(), tg.tags.end(), std::string("tutorial.tip:")), tg.tags.end());
        tg.tags.erase(std::remove(tg.tags.begin(), tg.tags.end(), std::string("tutorial.attached:true")), tg.tags.end());
    }
    by_entity_.erase(entity);
}

PTV_REGISTER_MECHANIC("generated.tutorial_guidance", TutorialGuidance)

} // namespace ptv
