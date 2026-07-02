#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include "control_constructs_shared.h"
#include <algorithm>
#include <string>

namespace ptv {

static void write_bands_tag(World& w, std::uint32_t target, int stacks, float slow_per_stack) {
    nlohmann::json j;
    j["stacks"] = stacks;
    j["slow_per_stack"] = slow_per_stack;
    ensure_tag(w, target, std::string("status.slow_bands:") + j.dump());
}

void apply_bands_now(World& w, std::uint32_t target, const ParsedControlInput& in,
                     BandsRuntime& br) {
    if (!w.valid(target)) return;
    if (br.stacks < in.bands_max_stacks) br.stacks += 1;
    br.time_since_last_apply_s = 0.0f;
    write_bands_tag(w, target, br.stacks, in.bands_slow_per_stack);
}

void decay_bands(World& w, std::uint32_t target, float dt,
                 const ParsedControlInput& in, BandsRuntime& br) {
    if (!w.valid(target)) return;
    br.time_since_last_apply_s += dt;
    // Decay after 2.0s of no refresh: drop one stack per second
    if (br.time_since_last_apply_s > 2.0f && br.stacks > 0) {
        float excess = br.time_since_last_apply_s - 2.0f;
        int drop = static_cast<int>(excess);
        if (drop > 0) {
            br.stacks = std::max(0, br.stacks - drop);
            // keep timer but clamp to fractional remainder to avoid large jumps
            br.time_since_last_apply_s = 2.0f + (excess - drop);
            if (br.stacks == 0) {
                remove_tag_prefix(w, target, "status.slow_bands");
            } else {
                write_bands_tag(w, target, br.stacks, in.bands_slow_per_stack);
            }
        }
    }
}

} // namespace ptv
