#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <optional>
#include <array>
#include <algorithm>

namespace ptv {
namespace haptics {

// Forward decls from haptics_service.cpp
enum class Pattern : std::uint8_t;
enum class HandMask : std::uint8_t;
struct BusEvent { Pattern pattern; HandMask hand; float intensity_scale; std::optional<float> duration_s; };
void drain(std::vector<BusEvent>& out);
bool has_pattern(const std::string& id);
void publish(const std::string& pattern_id, const std::string& hand, float intensity_scale, std::optional<float> duration_s);
void set_user_intensity(float v);
float get_user_intensity();
const char* pattern_to_string(Pattern p);
const char* hand_to_string(HandMask h);

// Pattern envelope sampling (debug/meta only)
struct SampledPulse { float ttl; float amp; const char* pattern; const char* hand; };
void synthesize_envelope(const BusEvent& e, float user_scale, std::vector<SampledPulse>& out_samples);

} // namespace haptics

class GlobalHapticsManager : public IMechanic {
public:
    void on_attach(World& world, std::uint32_t entity, const nlohmann::json& params) override {
        (void)world; (void)entity;
        user_scale_ = std::clamp(params.value("user_intensity_01", 1.0f), 0.0f, 1.0f);
        ptv::haptics::set_user_intensity(user_scale_);
        debug_buffer_.reserve(32);
        // Optional test publish path via binding params
        if (params.contains("publish") && params.at("publish").is_object()) {
            const auto& p = params.at("publish");
            std::string pat = p.value("pattern_id", std::string("hover_light"));
            std::string hand = p.value("hand", std::string("both"));
            float scale = p.value("intensity_scale", 1.0f);
            std::optional<float> dur;
            if (p.contains("duration_s")) dur = p.at("duration_s").get<float>();
            ptv::haptics::publish(pat, hand, scale, dur);
        }
    }

    void on_update(World& world, std::uint32_t entity, float dt) override {
        (void)world;
        // Drain bus -> synthesize lightweight debug pulses
        drain_and_synthesize_();
        // Tick-down debug samples and emit to a debug tag for test visibility
        tick_debug_(dt);
        emit_debug_tags_(world, entity);
    }

    void on_detach(World& world, std::uint32_t entity) override {
        // Clear debug tag on detach
        if (world.valid(entity) && world.has<CTag>(entity)) {
            world.remove<CTag>(entity);
        }
    }

    std::string id() const override { return "generated.haptics_manager"; }

private:
    void drain_and_synthesize_() {
        std::vector<ptv::haptics::BusEvent> bus;
        ptv::haptics::drain(bus);
        if (bus.empty()) return;
        const float u = ptv::haptics::get_user_intensity();
        for (const auto& e : bus) {
            ptv::haptics::synthesize_envelope(e, u, active_);
        }
    }

    void tick_debug_(float dt) {
        // Decay ttl; compact
        std::size_t w = 0;
        for (std::size_t i = 0; i < active_.size(); ++i) {
            auto s = active_[i];
            s.ttl -= dt;
            if (s.ttl > 0.0f) {
                active_[w++] = s;
            }
        }
        active_.resize(w);
    }

    void emit_debug_tags_(World& world, std::uint32_t entity) {
        // Compose compact strings like: hfx=bolt_fire_sharp:right:0.85:0.06|construct_form_heavy:both:1.00:0.20
        debug_buffer_.clear();
        if (!active_.empty()) {
            for (std::size_t i = 0; i < active_.size(); ++i) {
                const auto& s = active_[i];
                // reserve minimal room
                char buf[96];
                int n = std::snprintf(buf, sizeof(buf), "hfx=%s:%s:%.2f:%.2f", s.pattern, s.hand, s.amp, s.ttl);
                if (n > 0) debug_buffer_.emplace_back(buf);
            }
        } else {
            debug_buffer_.emplace_back("hfx=idle");
        }
        CTag tag;
        tag.tags = debug_buffer_;
        world.emplace_or_replace<CTag>(entity, std::move(tag));
    }

private:
    float user_scale_{1.0f};
    std::vector<ptv::haptics::SampledPulse> active_;
    std::vector<std::string> debug_buffer_;
};

} // namespace ptv

PTV_REGISTER_MECHANIC("generated.haptics_manager", ptv::GlobalHapticsManager)
