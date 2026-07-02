#include "boss_internal.h"

#include <string>

namespace ptv {

void FearBehemothBossBehaviour::fire_arena_event_tag(World& world, std::uint32_t entity, const std::string& tag) {
    if (world.has<ptv::CTag>(entity)) {
        world.get<ptv::CTag>(entity).tags.push_back(tag);
    } else {
        ptv::CTag tg; tg.tags = {tag};
        world.emplace_or_replace<ptv::CTag>(entity, tg);
    }
}

} // namespace ptv
