#include "haptics_shared.h"

#include <unordered_map>
#include <string>

namespace ptv_haptics {

static std::unordered_map<std::string, PatternDef> build_default_patterns() {
    // Tuned intuitively for VR controllers; amplitudes are relative and will be
    // multiplied by user/global/caller scales.
    std::unordered_map<std::string, PatternDef> m;
    m["hover_light"]             = PatternDef{0.25f, 0.040f, 0.002f, 0.020f};
    m["bolt_fire_sharp"]        = PatternDef{0.85f, 0.055f, 0.001f, 0.030f};
    m["construct_form_heavy"]   = PatternDef{1.00f, 0.120f, 0.004f, 0.060f};
    m["recharge_rhythm"]        = PatternDef{0.35f, 0.300f, 0.010f, 0.120f}; // single beat; callers can re-publish to sustain
    m["block_impact_strong"]    = PatternDef{0.95f, 0.090f, 0.002f, 0.050f};
    m["sprint_start"]           = PatternDef{0.70f, 0.080f, 0.003f, 0.040f};
    m["sprint_end"]             = PatternDef{0.55f, 0.070f, 0.003f, 0.035f};
    m["flight_boost"]           = PatternDef{0.80f, 0.100f, 0.004f, 0.050f};
    m["evade_burst"]            = PatternDef{0.90f, 0.070f, 0.002f, 0.030f};
    return m;
}

// Accessor for a single static instance
const std::unordered_map<std::string, PatternDef>& pattern_map() {
    static std::unordered_map<std::string, PatternDef> k = build_default_patterns();
    return k;
}

} // namespace ptv_haptics
