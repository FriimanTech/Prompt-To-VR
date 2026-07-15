#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <nlohmann/json.hpp>

namespace ptv {

// Lightweight per-entity runtime state for routing and pattern synthesis
struct HAF_State {
    // Edge-detect booleans from last frame
    bool prev_construct_preview{false};
    bool prev_construct_finalize{false};
    bool prev_construct_dismiss{false};
    bool prev_beam_start{false};
    bool beam_active{false};
    bool prev_block_hit{false};
    bool prev_recharge_complete{false};
    bool prev_flight_start{false};
    bool flight_hum_active{false};
    bool prev_boost_on{false};
    bool boost_active{false};
    bool prev_boost_off{false};
    bool prev_impact_strong{false};

    // Analog progress for recharge escalation (0..1)
    float recharge_progress{0.0f};

    // Timers for rhythmic patterns
    float t{0.0f};
    float beam_pulse_accum{0.0f};
    float recharge_tick_accum{0.0f};
    float flight_hum_accum{0.0f};

    // Short celebratory sequence after recharge complete
    int rc_pulse_count_remaining{0};
    float rc_sequence_accum{0.0f};
};

// Helper: safely parse the params JSON for our entry
inline std::optional<nlohmann::json> haf_try_parse_params(const std::string& s) {
    auto j = nlohmann::json::parse(s, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return std::nullopt;
    return j;
}

// Build a compact command tag string we can stuff into CTag.tags
// Kind examples: "haptic", "audio"
// For haptics fields: side(=both|left|right), pattern, amp(0..1), dur_ms, freq_hz(optional)
// For audio fields: action(play|loop_start|loop_stop), event_id, entity(optional)
inline std::string haf_cmd(const std::string& kind,
                           const std::vector<std::pair<std::string, std::string>>& kv) {
    std::string out = kind;
    out.reserve(128);
    out += ":";
    bool first = true;
    for (const auto& p : kv) {
        if (!first) out += ";"; else first = false;
        out += p.first;
        out += "=";
        out += p.second;
    }
    return out;
}

// Clamp helper
inline float haf_clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

// Append a command tag into existing tag list with a simple cap
inline void haf_append_tag_limited(std::vector<std::string>& tags, const std::string& s, std::size_t cap = 16) {
    tags.emplace_back(s);
    if (tags.size() > cap) {
        // Drop oldest to keep size bounded
        tags.erase(tags.begin(), tags.begin() + (tags.size() - cap));
    }
}

} // namespace ptv
