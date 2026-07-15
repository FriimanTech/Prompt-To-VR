#include "save_load_system_shared.h"

#include <string>
#include <vector>
#include <algorithm>

namespace ptv {

static nlohmann::json& saved_profile_storage() {
    static nlohmann::json g = nlohmann::json::object();
    return g;
}

static const char* kProfileTagPrefix = "__profile_json__:"; // stored inside CTag.tags as one long string

static nlohmann::json default_profile() {
    nlohmann::json j = nlohmann::json::object();
    // settings namespace
    j["settings"] = {
        {"locomotion", {
            {"mode", "teleport"},
            {"snap_deg", 45.0f},
            {"smooth_turn_rate", 120.0f},
            {"vignette", true},
            {"horizon_lock", false}
        }},
        {"options", {
            {"accessibility", {
                {"handedness", "right"},
                {"captions", false}
            }}
        }}
    };
    // gameplay namespace
    j["difficulty"] = "normal";
    j["constructs"] = {
        {"unlocked", nlohmann::json::array()},
        {"loadout", nlohmann::json::array()}
    };
    j["mastery"] = {
        {"points", 0}
    };
    j["mission"] = {
        {"last_completed", ""}
    };
    j["meta"] = {
        {"version", 1},
        {"last_save_reason", "boot_default"}
    };
    return j;
}

static void mirror_into_tag(World& world, std::uint32_t entity, const nlohmann::json& blob) {
    if (!world.valid(entity)) return;
    std::string packed = std::string(kProfileTagPrefix) + blob.dump();
    if (!world.has<CTag>(entity)) {
        world.emplace_or_replace<CTag>(entity, CTag{});
    }
    auto& tag = world.get<CTag>(entity);
    // Remove any previous profile tag then push the new one
    auto& v = tag.tags;
    v.erase(std::remove_if(v.begin(), v.end(), [](const std::string& s){
        return s.rfind(kProfileTagPrefix, 0) == 0; // starts_with
    }), v.end());
    v.emplace_back(std::move(packed));
}

static std::optional<nlohmann::json> read_from_tag(World& world, std::uint32_t entity) {
    if (!world.valid(entity) || !world.has<CTag>(entity)) return std::nullopt;
    auto& tag = world.get<CTag>(entity);
    for (const auto& s : tag.tags) {
        if (s.rfind(kProfileTagPrefix, 0) == 0) {
            auto slice = s.substr(std::char_traits<char>::length(kProfileTagPrefix));
            auto j = nlohmann::json::parse(slice, nullptr, false);
            if (j.is_object()) return j;
        }
    }
    return std::nullopt;
}

static std::vector<std::string> split_dotted(const std::string& path) {
    std::vector<std::string> out; out.reserve(8);
    std::size_t i = 0; std::size_t n = path.size();
    while (i < n) {
        auto p = path.find('.', i);
        if (p == std::string::npos) p = n;
        out.emplace_back(path.substr(i, p - i));
        i = p + (p < n ? 1 : 0);
    }
    return out;
}

static nlohmann::json* ensure_path(nlohmann::json& root, const std::vector<std::string>& keys) {
    nlohmann::json* cur = &root;
    for (const auto& k : keys) {
        if (!cur->is_object()) *cur = nlohmann::json::object();
        if (!cur->contains(k)) (*cur)[k] = nlohmann::json::object();
        cur = &((*cur)[k]);
    }
    return cur;
}

static const nlohmann::json* find_path(const nlohmann::json& root, const std::vector<std::string>& keys) {
    auto* cur = &root;
    for (const auto& k : keys) {
        if (!cur->is_object() || !cur->contains(k)) return nullptr;
        cur = &cur->at(k);
    }
    return cur;
}

// Public API -------------------------------------------------------------

nlohmann::json ptv_profile_get(World& world, std::uint32_t entity, const std::string& dotted_path) {
    // Prefer the entity tag, fallback to global storage, then default.
    nlohmann::json base;
    if (auto j = read_from_tag(world, entity)) base = *j;
    else if (!saved_profile_storage().is_null() && saved_profile_storage().is_object()) base = saved_profile_storage();
    else base = default_profile();

    auto keys = split_dotted(dotted_path);
    if (keys.empty()) return base; // whole blob
    if (auto* v = find_path(base, keys)) return *v;
    return nlohmann::json();
}

void ptv_profile_set(World& world, std::uint32_t entity, const std::string& dotted_path,
                     const nlohmann::json& value) {
    nlohmann::json base;
    if (auto j = read_from_tag(world, entity)) base = *j; else base = saved_profile_storage();
    if (!base.is_object()) base = default_profile();

    auto keys = split_dotted(dotted_path);
    if (keys.empty()) return;
    auto last = keys.back();
    std::vector<std::string> parents(keys.begin(), keys.end()-1);
    auto* obj = ensure_path(base, parents);
    (*obj)[last] = value;

    // mirror to global + tag
    saved_profile_storage() = base;
    mirror_into_tag(world, entity, base);
}

nlohmann::json ptv_settings_get(World& world, std::uint32_t entity,
                                const std::string& name_space,
                                const std::string& key,
                                const nlohmann::json& default_value) {
    auto v = ptv_profile_get(world, entity, std::string("settings.") + name_space + "." + key);
    return v.is_null() ? default_value : v;
}

void ptv_settings_set(World& world, std::uint32_t entity,
                      const std::string& name_space,
                      const std::string& key,
                      const nlohmann::json& value) {
    ptv_profile_set(world, entity, std::string("settings.") + name_space + "." + key, value);
}

void ptv_save_now(World& world, std::uint32_t entity, const std::string& reason) {
    // Increment version and stamp reason, then mirror.
    nlohmann::json base;
    if (auto j = read_from_tag(world, entity)) base = *j; else base = saved_profile_storage();
    if (!base.is_object()) base = default_profile();

    // bump version
    int ver = 0;
    if (base.contains("meta") && base["meta"].is_object() && base["meta"].contains("version")) {
        ver = base["meta"]["version"].get<int>();
    }
    base["meta"]["version"] = ver + 1;
    base["meta"]["last_save_reason"] = reason;

    saved_profile_storage() = base;
    mirror_into_tag(world, entity, base);
}

const nlohmann::json& ptv_profile_current() { return saved_profile_storage(); }

} // namespace ptv
