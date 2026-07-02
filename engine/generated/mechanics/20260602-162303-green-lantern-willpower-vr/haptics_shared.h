#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <optional>
#include <algorithm>
#include <cstdint>
#include <nlohmann/json.hpp>

namespace ptv_haptics {

// Hand selector used by the tag protocol
enum class Hand : std::uint8_t { Left=0, Right=1, Both=2 };

inline std::string_view to_string(Hand h) {
    switch (h) {
        case Hand::Left: return "left";
        case Hand::Right: return "right";
        case Hand::Both: return "both";
    }
    return "right";
}

inline std::optional<Hand> parse_hand(std::string_view s) {
    if (s == "left") return Hand::Left;
    if (s == "right") return Hand::Right;
    if (s == "both") return Hand::Both;
    return std::nullopt;
}

// Known pattern ids (string keys used in the publish protocol)
// hover_light, bolt_fire_sharp, construct_form_heavy, recharge_rhythm,
// block_impact_strong, sprint_start, sprint_end, flight_boost, evade_burst

struct PatternDef {
    float base_amp{0.6f};        // 0..1 baseline amplitude
    float default_duration_s{0.08f};
    // Optional attack/decay shaping hint (0..1). Not used by engine directly, kept for future expansion.
    float attack_s{0.005f};
    float decay_s{0.03f};
};

struct PublishReq {
    std::string pattern_id;            // required
    Hand hand{Hand::Right};            // default right
    float intensity_scale{1.0f};       // additional scalar from caller (e.g., construct weight)
    std::optional<float> duration_s;   // optional override
};

// Tag protocol (cross-mechanic, string-only, no shared headers needed):
//   To publish (from any entity): add a CTag entry string starting with
//     "haptics.publish:{json}"
//   JSON shape: { "pattern_id":"bolt_fire_sharp", "hand":"right|left|both",
//                 "intensity_scale":1.0, "duration_s":0.12 }
//   The manager consumes and removes that tag, then emits on its own attached entity
//     "haptics.play:{json}"
//   JSON shape: { "pattern":"bolt_fire_sharp", "hand":"right",
//                 "amp":0.85, "dur_s":0.12 }

inline constexpr std::string_view kPublishPrefix = "haptics.publish:";
inline constexpr std::string_view kPlayPrefix    = "haptics.play:";

// Safe parse of a publish JSON blob into PublishReq
inline std::optional<PublishReq> parse_publish_json(const std::string& json_text) {
    auto j = nlohmann::json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return std::nullopt;
    PublishReq r{};
    if (!j.contains("pattern_id")) return std::nullopt;
    r.pattern_id = j.value("pattern_id", std::string{});
    auto h_str = j.value("hand", std::string("right"));
    if (auto h = parse_hand(h_str)) r.hand = *h; else r.hand = Hand::Right;
    r.intensity_scale = j.value("intensity_scale", 1.0f);
    if (j.contains("duration_s")) r.duration_s = j.value("duration_s", 0.0f);
    return r;
}

// Compose a play tag JSON payload
inline std::string make_play_json(const std::string& pattern, Hand hand, float amp, float dur_s) {
    nlohmann::json j;
    j["pattern"] = pattern;
    j["hand"] = std::string(to_string(hand));
    j["amp"] = amp;
    j["dur_s"] = dur_s;
    return j.dump();
}

// Clamp helper
inline float clamp01(float v) { return std::max(0.0f, std::min(1.0f, v)); }

// Lightweight ring buffer for recent events (debug/validation)
struct RecentEvents {
    struct Item { std::string pattern; std::string hand; float amp; float dur; };
    std::vector<Item> items;
    std::size_t max_items{32};
    void push(Item i) {
        items.emplace_back(std::move(i));
        if (items.size() > max_items) items.erase(items.begin());
    }
};

} // namespace ptv_haptics
