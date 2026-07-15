#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <entt/entt.hpp>

#include <string>
#include <unordered_map>
#include <vector>
#include <optional>
#include <utility>
#include <algorithm>
#include <cmath>

#include "haptics_audio_feedback_shared.h"

namespace ptv {

// Alias for brevity
static constexpr const char* kMechanicId = "generated.haptics_audio_feedback";

class HapticsAudioFeedback : public IMechanic {
public:
    void on_attach(World& world, std::uint32_t entity, const nlohmann::json& params) override {
        (void)world;
        // Per-entity state is created lazily in update when first seen.
        // Record optional gains from params for future extension (not persisted across frames).
        float h_gain = params.value("gain_haptics", 1.0f);
        float a_gain = params.value("gain_audio", 1.0f);
        gains_[entity] = {haf_clamp01(h_gain), haf_clamp01(a_gain)};

        // Tag entity so test harness can find the router
        if (world.has<CTag>(entity)) {
            auto& t = world.get<CTag>(entity).tags;
            haf_append_tag_limited(t, std::string("router=") + kMechanicId);
        } else {
            CTag t{};
            t.tags.push_back(std::string("router=") + kMechanicId);
            world.emplace_or_replace<CTag>(entity, std::move(t));
        }
    }

    void on_update(World& world, std::uint32_t entity, float dt) override {
        if (!world.valid(entity)) return;

        auto& st = state_[entity];
        st.t += dt;

        const auto gains = gains_.count(entity) ? gains_[entity] : std::pair<float,float>{1.0f,1.0f};
        const float h_gain = gains.first;
        const float a_gain = gains.second;

        // Gather current-frame parameter events from our binding entry (if present)
        bool e_preview=false, e_finalize=false, e_dismiss=false;
        bool e_beam_start=false, e_beam_stop=false;
        bool e_block_hit=false, e_recharge_complete=false;
        bool e_flight_start=false, e_boost_on=false, e_boost_off=false;
        bool e_impact_strong=false;
        float recharge_progress = st.recharge_progress; // default to prior if not sent this frame
        std::string source_entity_id;

        if (world.has<CMechanicBinding>(entity)) {
            const auto& mb = world.get<CMechanicBinding>(entity);
            for (const auto& entry : mb.entries) {
                if (entry.id == kMechanicId || entry.builtin_id == kMechanicId) {
                    auto pj = haf_try_parse_params(entry.params_json);
                    if (pj) {
                        const auto& j = *pj;
                        auto try_b = [&](const char* k, bool& dst){ if (j.contains(k)) { auto v=j.at(k); if (v.is_boolean()) dst=v.get<bool>(); } };
                        try_b("construct.preview", e_preview);
                        try_b("construct.finalize", e_finalize);
                        try_b("construct.dismiss", e_dismiss);
                        try_b("beam.start", e_beam_start);
                        try_b("beam.stop", e_beam_stop);
                        try_b("block.hit", e_block_hit);
                        try_b("recharge.complete", e_recharge_complete);
                        try_b("flight.start", e_flight_start);
                        try_b("flight.boost_on", e_boost_on);
                        try_b("flight.boost_off", e_boost_off);
                        try_b("impact.strong", e_impact_strong);
                        if (j.contains("recharge.progress")) {
                            auto v = j.at("recharge.progress");
                            if (v.is_number_float() || v.is_number_integer()) {
                                recharge_progress = static_cast<float>(v.get<double>());
                                recharge_progress = haf_clamp01(recharge_progress);
                            }
                        }
                        if (j.contains("source_entity_id") && j.at("source_entity_id").is_string()) {
                            source_entity_id = j.at("source_entity_id").get<std::string>();
                        }
                    }
                }
            }
        }

        // Collect command tags to emit this frame
        std::vector<std::string> frame_tags;
        frame_tags.reserve(12);

        auto emit_haptic = [&](const char* side, const char* pattern, float amp, int dur_ms, float freq_hz = 0.0f){
            amp = haf_clamp01(amp * h_gain);
            std::vector<std::pair<std::string,std::string>> kv{
                {"side", side}, {"pattern", pattern}, {"amp", to_str(amp)}, {"dur_ms", to_str(dur_ms)}
            };
            if (freq_hz > 0.0f) kv.emplace_back("freq_hz", to_str(freq_hz));
            haf_append_tag_limited(frame_tags, haf_cmd("haptic", kv));
        };
        auto emit_haptic_both = [&](const char* pattern, float amp, int dur_ms, float freq_hz = 0.0f){
            emit_haptic("left", pattern, amp, dur_ms, freq_hz);
            emit_haptic("right", pattern, amp, dur_ms, freq_hz);
        };
        auto emit_audio = [&](const char* action, const char* event_id){
            std::vector<std::pair<std::string,std::string>> kv{
                {"action", action}, {"event_id", event_id}
            };
            if (!source_entity_id.empty()) kv.emplace_back("entity", source_entity_id);
            haf_append_tag_limited(frame_tags, haf_cmd("audio", kv));
        };

        // Rising edges and instant pulses
        if (e_preview && !st.prev_construct_preview) {
            emit_haptic_both("light_pulse", 0.18f, 40);
            emit_audio("play", "sfx.hover");
        }
        if (e_finalize && !st.prev_construct_finalize) {
            emit_haptic_both("heavy_burst", 0.9f, 120);
            emit_audio("play", "sfx.construct_finalize");
        }
        if (e_dismiss && !st.prev_construct_dismiss) {
            emit_haptic_both("sharp_pulse", 0.25f, 50);
            emit_audio("play", "sfx.construct_dismiss");
        }
        if (e_block_hit && !st.prev_block_hit) {
            emit_haptic_both("strong_impact", 0.85f, 90);
            emit_audio("play", "sfx.block_hit");
        }
        if (e_impact_strong && !st.prev_impact_strong) {
            emit_haptic_both("strong_impact", 1.0f, 140);
            emit_audio("play", "sfx.impact_strong");
        }
        if (e_flight_start && !st.prev_flight_start) {
            st.flight_hum_active = true;
            st.flight_hum_accum = 0.0f;
            emit_audio("play", "sfx.flight_start");
        }
        if (e_boost_on && !st.prev_boost_on) {
            st.boost_active = true;
            emit_haptic_both("sharp_pulse", 0.6f, 70);
            emit_audio("play", "sfx.boost_on");
        }
        if (e_boost_off && !st.prev_boost_off) {
            st.boost_active = false;
            emit_haptic_both("light_pulse", 0.22f, 50);
            emit_audio("play", "sfx.boost_off");
        }

        // Beam sustain loop control
        if (e_beam_start && !st.prev_beam_start) {
            st.beam_active = true;
            st.beam_pulse_accum = 0.0f;
            emit_audio("loop_start", "sfx.beam_loop");
        }
        if (e_beam_stop) {
            st.beam_active = false;
            emit_audio("loop_stop", "sfx.beam_loop");
        }

        // Recharge progress handling (steady escalating)
        st.recharge_progress = recharge_progress;
        // Tick every ~250ms with amplitude scaled by progress
        st.recharge_tick_accum += dt;
        if (st.recharge_tick_accum >= 0.25f) {
            st.recharge_tick_accum = 0.0f;
            float amp = 0.15f + 0.7f * haf_clamp01(st.recharge_progress);
            emit_haptic_both("steady_step", amp, 35);
            if (st.recharge_progress > 0.01f) emit_audio("play", "sfx.recharge_tick");
        }
        if (e_recharge_complete && !st.prev_recharge_complete) {
            st.rc_pulse_count_remaining = 3; // 3 celebratory blips over ~0.45s
            st.rc_sequence_accum = 0.0f;
            emit_audio("play", "sfx.recharge_complete");
        }

        // Sequence scheduler for recharge-complete triple pulse (every 0.15s)
        if (st.rc_pulse_count_remaining > 0) {
            st.rc_sequence_accum += dt;
            const float step = 0.15f;
            while (st.rc_sequence_accum >= step && st.rc_pulse_count_remaining > 0) {
                st.rc_sequence_accum -= step;
                float amp = 0.6f + 0.15f * static_cast<float>(3 - st.rc_pulse_count_remaining);
                emit_haptic_both("celebrate", amp, 55);
                --st.rc_pulse_count_remaining;
            }
        }

        // Continuous patterns
        if (st.beam_active) {
            st.beam_pulse_accum += dt;
            const float period = 0.18f; // rhythmic resonance
            if (st.beam_pulse_accum >= period) {
                st.beam_pulse_accum -= period;
                emit_haptic_both("resonance", 0.28f, 30, 90.0f);
            }
        }
        if (st.flight_hum_active) {
            st.flight_hum_accum += dt;
            const float period = st.boost_active ? 0.22f : 0.5f;
            if (st.flight_hum_accum >= period) {
                st.flight_hum_accum -= period;
                float base = st.boost_active ? 0.35f : 0.14f;
                emit_haptic_both("hum", base, 28, st.boost_active ? 120.0f : 70.0f);
            }
        }

        // Push tags into entity CTag for this frame
        if (!frame_tags.empty()) {
            if (world.has<CTag>(entity)) {
                auto& t = world.get<CTag>(entity).tags;
                for (const auto& s : frame_tags) haf_append_tag_limited(t, s);
            } else {
                CTag t{};
                t.tags = std::move(frame_tags);
                world.emplace_or_replace<CTag>(entity, std::move(t));
            }
        }

        // Store edges for next frame
        st.prev_construct_preview = e_preview;
        st.prev_construct_finalize = e_finalize;
        st.prev_construct_dismiss = e_dismiss;
        st.prev_beam_start = e_beam_start;
        st.prev_block_hit = e_block_hit;
        st.prev_recharge_complete = e_recharge_complete;
        st.prev_flight_start = e_flight_start;
        st.prev_boost_on = e_boost_on;
        st.prev_boost_off = e_boost_off;
        st.prev_impact_strong = e_impact_strong;
    }

    void on_detach(World& world, std::uint32_t entity) override {
        // Leave CTag intact (other systems may use it). Just drop local caches.
        state_.erase(entity);
        gains_.erase(entity);
        (void)world;
    }

    std::string id() const override { return kMechanicId; }

private:
    // Per-entity caches
    std::unordered_map<std::uint32_t, HAF_State> state_{};
    // haptics/audio gains per entity (0..1)
    std::unordered_map<std::uint32_t, std::pair<float,float>> gains_{};

    // Stringify helpers
    static std::string to_str(float v) {
        char buf[32];
        // compact formatting
        std::snprintf(buf, sizeof(buf), "%.3f", v);
        return std::string(buf);
    }
    static std::string to_str(int v) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", v);
        return std::string(buf);
    }
};

PTV_REGISTER_MECHANIC("generated.haptics_audio_feedback", HapticsAudioFeedback)

} // namespace ptv
