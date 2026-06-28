#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <optional>
#include <array>
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace ptv {
namespace haptics {

// Public enums/types
enum class Pattern : std::uint8_t {
    hover_light = 0,
    bolt_fire_sharp,
    construct_form_heavy,
    recharge_rhythm,
    block_impact_strong,
    sprint_start,
    sprint_end,
    flight_boost,
    evade_burst,
    Unknown
};

enum class HandMask : std::uint8_t { Left = 1, Right = 2, Both = 3 };

struct BusEvent {
    Pattern pattern{Pattern::Unknown};
    HandMask hand{HandMask::Both};
    float intensity_scale{1.0f};
    std::optional<float> duration_s{};
};

// Internal bus (single-threaded game loop; no mutex).
static std::vector<BusEvent> g_queue;
static float g_user_intensity{1.0f};

// String helpers
static inline bool ieq(const std::string& a, const char* b) {
    if (a.size() != std::strlen(b)) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = char(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = char(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

Pattern pattern_from_string(const std::string& id) {
    if (ieq(id, "hover_light")) return Pattern::hover_light;
    if (ieq(id, "bolt_fire_sharp")) return Pattern::bolt_fire_sharp;
    if (ieq(id, "construct_form_heavy")) return Pattern::construct_form_heavy;
    if (ieq(id, "recharge_rhythm")) return Pattern::recharge_rhythm;
    if (ieq(id, "block_impact_strong")) return Pattern::block_impact_strong;
    if (ieq(id, "sprint_start")) return Pattern::sprint_start;
    if (ieq(id, "sprint_end")) return Pattern::sprint_end;
    if (ieq(id, "flight_boost")) return Pattern::flight_boost;
    if (ieq(id, "evade_burst")) return Pattern::evade_burst;
    return Pattern::Unknown;
}

const char* pattern_to_string(Pattern p) {
    switch (p) {
        case Pattern::hover_light: return "hover_light";
        case Pattern::bolt_fire_sharp: return "bolt_fire_sharp";
        case Pattern::construct_form_heavy: return "construct_form_heavy";
        case Pattern::recharge_rhythm: return "recharge_rhythm";
        case Pattern::block_impact_strong: return "block_impact_strong";
        case Pattern::sprint_start: return "sprint_start";
        case Pattern::sprint_end: return "sprint_end";
        case Pattern::flight_boost: return "flight_boost";
        case Pattern::evade_burst: return "evade_burst";
        default: return "unknown";
    }
}

HandMask hand_from_string(const std::string& h) {
    if (ieq(h, "left")) return HandMask::Left;
    if (ieq(h, "right")) return HandMask::Right;
    return HandMask::Both;
}

const char* hand_to_string(HandMask h) {
    switch (h) { case HandMask::Left: return "left"; case HandMask::Right: return "right"; default: return "both"; }
}

bool has_pattern(const std::string& id) { return pattern_from_string(id) != Pattern::Unknown; }

void set_user_intensity(float v) { g_user_intensity = std::clamp(v, 0.0f, 1.0f); }
float get_user_intensity() { return g_user_intensity; }

void publish(const std::string& pattern_id, const std::string& hand, float intensity_scale, std::optional<float> duration_s) {
    BusEvent e; e.pattern = pattern_from_string(pattern_id); e.hand = hand_from_string(hand); e.intensity_scale = std::max(0.0f, intensity_scale); e.duration_s = duration_s;
    if (e.pattern == Pattern::Unknown) return; // ignore
    g_queue.emplace_back(e);
}

void drain(std::vector<BusEvent>& out) {
    if (g_queue.empty()) return;
    out.insert(out.end(), g_queue.begin(), g_queue.end());
    g_queue.clear();
}

// Envelope description for debug sampling (no device layer here)
struct PatternDesc { float base_duration; float base_amp; int pulses; float pulse_hz; float attack; float decay; };

static PatternDesc desc_for(Pattern p) {
    switch (p) {
        case Pattern::hover_light:          return {0.045f, 0.25f, 1, 0.0f, 0.005f, 0.020f};
        case Pattern::bolt_fire_sharp:      return {0.060f, 0.85f, 2, 22.0f, 0.002f, 0.030f};
        case Pattern::construct_form_heavy: return {0.180f, 1.00f, 3, 15.0f, 0.006f, 0.120f};
        case Pattern::recharge_rhythm:      return {0.900f, 0.35f, 5, 3.0f,  0.010f, 0.200f};
        case Pattern::block_impact_strong:  return {0.090f, 0.95f, 1, 0.0f,  0.001f, 0.070f};
        case Pattern::sprint_start:         return {0.110f, 0.55f, 2, 14.0f, 0.004f, 0.050f};
        case Pattern::sprint_end:           return {0.080f, 0.40f, 1, 9.0f,  0.004f, 0.040f};
        case Pattern::flight_boost:         return {0.150f, 0.70f, 3, 18.0f, 0.003f, 0.080f};
        case Pattern::evade_burst:          return {0.070f, 0.80f, 2, 28.0f, 0.002f, 0.040f};
        default:                            return {0.050f, 0.30f, 1, 0.0f,  0.005f, 0.020f};
    }
}

struct SampledPulse { float ttl; float amp; const char* pattern; const char* hand; };

static inline float scalar_mix(float a, float b, float t) { return a + (b - a) * t; }

// Very small synthesizer: creates a handful of pulses with amplitudes pre-scaled; used only for debug tags.
static void synthesize_one(const BusEvent& e, float user_scale, std::vector<SampledPulse>& out) {
    PatternDesc d = desc_for(e.pattern);
    float dur = e.duration_s.has_value() ? std::max(0.01f, e.duration_s.value()) : d.base_duration;
    float amp = std::clamp(d.base_amp * e.intensity_scale * user_scale, 0.0f, 1.0f);
    int pulses = std::max(1, d.pulses);
    // Distribute ttl across small pulses for readability
    float per = dur / float(pulses);
    for (int i = 0; i < pulses; ++i) {
        float local_amp = amp * scalar_mix(1.0f, 0.7f, float(i) / float(std::max(1, pulses - 1)));
        SampledPulse s{ per, local_amp, pattern_to_string(e.pattern), hand_to_string(e.hand) };
        out.emplace_back(s);
    }
}

// Expose to manager
void synthesize_envelope(const BusEvent& e, float user_scale, std::vector<SampledPulse>& out_samples) {
    synthesize_one(e, user_scale, out_samples);
}

} // namespace haptics
} // namespace ptv
