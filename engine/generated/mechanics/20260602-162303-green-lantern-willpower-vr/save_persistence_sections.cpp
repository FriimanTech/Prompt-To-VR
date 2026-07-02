#include "save_persistence_shared.h"

#include "ptv/ecs/World.h"

#include <entt/entt.hpp>
#include <algorithm>

namespace ptv {
namespace tags {

static nlohmann::json parse_payload(std::string_view after_prefix) {
    return nlohmann::json::parse(after_prefix, nullptr, /*allow_exceptions=*/false);
}

std::vector<TagMatch> consume_with_prefix(World& w, std::uint32_t e, std::string_view prefix) {
    std::vector<TagMatch> out;
    if (!w.has<CTag>(e)) return out;
    auto& vec = w.get<CTag>(e).tags;
    std::vector<std::string> remain;
    remain.reserve(vec.size());
    for (const auto& s : vec) {
        if (s.rfind(prefix.data(), 0) == 0) {
            auto json_part = std::string_view{s}.substr(prefix.size());
            TagMatch m; m.raw = s; m.prefix = std::string(prefix); m.payload = parse_payload(json_part);
            out.push_back(std::move(m));
        } else {
            remain.push_back(s);
        }
    }
    if (out.size() != 0) {
        w.emplace_or_replace<CTag>(e, CTag{remain});
    }
    return out;
}

std::vector<std::pair<std::uint32_t, TagMatch>> consume_all_with_prefix(World& w, std::string_view prefix) {
    std::vector<std::pair<std::uint32_t, TagMatch>> out;
    auto& reg = w.registry();
    auto view = reg.view<CTag>();
    for (auto ent : view) {
        std::uint32_t e = static_cast<std::uint32_t>(ent);
        auto matches = consume_with_prefix(w, e, prefix);
        for (auto& m : matches) out.emplace_back(e, std::move(m));
    }
    return out;
}

void append(World& w, std::uint32_t e, const std::string& tag) {
    if (!w.has<CTag>(e)) { w.emplace_or_replace<CTag>(e, CTag{}); }
    auto& vec = w.get<CTag>(e).tags;
    vec.push_back(tag);
    w.emplace_or_replace<CTag>(e, CTag{vec});
}

} // namespace tags
} // namespace ptv
