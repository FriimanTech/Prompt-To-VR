#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <entt/entt.hpp>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace ptv {

static inline bool starts_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && std::equal(p.begin(), p.end(), s.begin());
}
static inline std::string kv(const std::string& k, const std::string& v) {
    return k + ":" + v;
}
static inline void set_kv_tag(CTag& tag, const std::string& key, const std::string& val) {
    const std::string pref = key + ":";
    for (auto& s : tag.tags) {
        if (starts_with(s, pref)) { s = kv(key, val); return; }
    }
    tag.tags.emplace_back(kv(key, val));
}
static inline void set_flag_tag(CTag& tag, const std::string& key) {
    for (auto& s : tag.tags) {
        if (s == key) return;
    }
    tag.tags.emplace_back(key);
}
static inline void remove_prefix_tags(CTag& tag, const std::string& pref) {
    tag.tags.erase(std::remove_if(tag.tags.begin(), tag.tags.end(),
        [&](const std::string& s){ return starts_with(s, pref); }),
        tag.tags.end());
}
static inline void remove_exact_tag(CTag& tag, const std::string& exact) {
    tag.tags.erase(std::remove(tag.tags.begin(), tag.tags.end(), exact), tag.tags.end());
}
static inline float clamp01(float v){ return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
static inline float lerp(float a, float b, float t){ return a + (b - a) * t; }
static inline float sample_curve(const std::vector<float>& c, float t) {
    if (c.empty()) return t;
    if (c.size() == 1) return c[0];
    t = clamp01(t);
    float segf = t * float(c.size() - 1);
    int i = int(std::floor(segf));
    int j = std::min<int>(i + 1, int(c.size() - 1));
    float lt = segf - float(i);
    return lerp(c[size_t(i)], c[size_t(j)], lt);
}
static inline float dist3(const Vec3& a, const Vec3& b) {
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx*dx + dy*dy + dz*dz);
}
static inline std::string f2s(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    return std::string(buf);
}

class RingChargeAndRecharge : public IMechanic {
public:
    void on_attach(World& w, std::uint32_t e, const nlohmann::json& p) override {
        max_energy_ = std::max(0.1f, p.value("max_energy", 100.0f));
        energy_ = std::clamp(p.value("start_energy", max_energy_), 0.0f, max_energy_);
        low_threshold_ = p.value("low_energy_threshold", 0.2f * max_energy_);
        recharge_duration_ = std::max(0.1f, p.value("recharge_duration_s", 2.0f));
        pedestal_tag_ = p.value("pedestal_tag", std::string("recharge_pedestal"));
        pedestal_radius_ = std::max(0.05f, p.value("pedestal_radius_m", 0.30f));
        passive_regen_per_s_ = std::max(0.0f, p.value("passive_regen_per_s", 0.0f));
        abort_cooldown_s_ = std::max(0.0f, p.value("abort_cooldown_s", 1.0f));
        willpower_window_s_ = std::max(1.0f, p.value("willpower_window_s", 30.0f));
        willpower_max_bonus_pct_ = std::clamp(p.value("willpower_max_bonus_pct", 0.10f), 0.0f, 0.5f);

        if (p.contains("haptic_curve") && p.at("haptic_curve").is_array()) {
            for (auto& v : p.at("haptic_curve")) { haptic_curve_.push_back(v.get<float>()); }
        } else {
            haptic_curve_ = {0.0f, 0.2f, 0.5f, 0.8f, 1.0f};
        }
        if (p.contains("willpower_stability") && p.at("willpower_stability").is_array()) {
            for (auto& v : p.at("willpower_stability")) { willpower_curve_.push_back(std::clamp(v.get<float>(), 0.0f, 1.0f)); }
        } else {
            willpower_curve_ = {0.0f, 0.5f, 1.0f};
        }
        if (p.contains("ui_bindings") && p.at("ui_bindings").is_array()) {
            for (auto& s : p.at("ui_bindings")) {
                if (!s.is_string()) continue;
                auto ee = w.find(s.get<std::string>());
                if (ee != entt::null) ui_bars_.push_back(ee);
            }
        }
        if (p.contains("costs") && p.at("costs").is_object()) {
            for (auto it = p.at("costs").begin(); it != p.at("costs").end(); ++it) {
                costs_[it.key()] = it.value().get<float>();
            }
        }
        if (p.contains("drain_overrides") && p.at("drain_overrides").is_object()) {
            for (auto it = p.at("drain_overrides").begin(); it != p.at("drain_overrides").end(); ++it) {
                drains_[it.key()] = it.value().get<float>();
            }
        }

        // Ensure tag component exists on the ring entity.
        if (!w.has<CTag>(e)) w.emplace_or_replace<CTag>(e, CTag{});
        auto& ringTag = w.get<CTag>(e);
        // Publish static API hints
        set_kv_tag(ringTag, "ring.max", f2s(max_energy_));
        set_kv_tag(ringTag, "ring.low_threshold", f2s(low_threshold_));
        for (auto& kvp : costs_) set_kv_tag(ringTag, "ring.cost." + kvp.first, f2s(kvp.second));
        for (auto& kvp : drains_) set_kv_tag(ringTag, "ring.drain.override." + kvp.first, f2s(kvp.second));
        // Initial HUD fill
        publish_energy_(w, e, energy_ / max_energy_);
    }

    void on_update(World& w, std::uint32_t e, float dt) override {
        if (dt <= 0.0f) dt = w.delta_time();

        if (!w.valid(e)) return;
        if (!w.has<CTag>(e)) w.emplace_or_replace<CTag>(e, CTag{});
        auto& ringTag = w.get<CTag>(e);

        // Clear ephemeral events/prompts we own
        remove_prefix_tags(ringTag, "event.ring.");
        remove_prefix_tags(ringTag, "event.audio.voice.");
        remove_prefix_tags(ringTag, "hud.prompt.");

        // Determine pedestal entity if needed
        auto& reg = w.registry();
        if (pedestal_cached_ == entt::null || !reg.valid(pedestal_cached_)) {
            pedestal_cached_ = find_pedestal_(w);
        }

        // Process incoming requests on the ring entity
        float frame_drain = 0.0f;
        std::vector<std::string> keep;
        keep.reserve(ringTag.tags.size());
        for (const auto& s : ringTag.tags) {
            if (starts_with(s, "ring.consume:")) {
                float v = 0.0f;
                try { v = std::stof(s.substr(std::string("ring.consume:").size())); } catch (...) {}
                if (v > 0.0f) energy_ -= v;
                continue;
            }
            if (starts_with(s, "ring.drain.per_s:")) {
                float v = 0.0f;
                try { v = std::stof(s.substr(std::string("ring.drain.per_s:").size())); } catch (...) {}
                if (v > 0.0f) frame_drain += v;
                continue;
            }
            // keep everything else (including other systems' tags)
            keep.push_back(s);
        }
        ringTag.tags.swap(keep);

        // Willpower stability: reduce effective drain up to configured pct over time window
        if (frame_drain > 0.0f) {
            willpower_t_ = std::min(willpower_window_s_, willpower_t_ + dt);
        } else {
            willpower_t_ = std::max(0.0f, willpower_t_ - 2.0f * dt);
        }
        float wpf = sample_curve(willpower_curve_, willpower_t_ / willpower_window_s_);
        float drain_factor = 1.0f - (willpower_max_bonus_pct_ * wpf);
        frame_drain *= std::max(0.0f, drain_factor);

        // Apply drains / regen unless recharging
        if (!is_recharging_) {
            if (frame_drain > 0.0f) energy_ -= frame_drain * dt;
            if (passive_regen_per_s_ > 0.0f && energy_ < max_energy_) {
                energy_ = std::min(max_energy_, energy_ + passive_regen_per_s_ * dt);
            }
        }

        energy_ = std::clamp(energy_, 0.0f, max_energy_);

        // Proximity check to pedestal socket
        bool in_socket = false;
        if (pedestal_cached_ != entt::null && reg.valid(pedestal_cached_)
            && w.has<CTransform>(e) && reg.any_of<CTransform>(pedestal_cached_)) {
            auto& ringTr = w.get<CTransform>(e).t;
            auto& pedTr = reg.get<CTransform>(pedestal_cached_).t;
            in_socket = dist3(ringTr.position, pedTr.position) <= pedestal_radius_;
        }

        // Recharge state machine
        if (cooldown_t_ > 0.0f) cooldown_t_ -= dt;

        if (!is_recharging_) {
            // Prompt when near pedestal
            if (in_socket && cooldown_t_ <= 0.0f) {
                if (energy_ >= max_energy_ - 1e-3f) {
                    set_flag_tag(ringTag, "hud.prompt:ring_full");
                } else {
                    set_flag_tag(ringTag, "hud.prompt:recharge_ready");
                }
            }
            if (in_socket && cooldown_t_ <= 0.0f && energy_ < max_energy_ - 1e-3f) {
                // Start recharge
                is_recharging_ = true;
                recharge_t_ = 0.0f;
                start_energy_ = energy_;
                set_flag_tag(ringTag, "event.ring.recharge_start");
                set_flag_tag(ringTag, "event.audio.voice:ring_recharge_start");
            }
        }

        if (is_recharging_) {
            recharge_t_ += dt;
            float r = clamp01(recharge_t_ / recharge_duration_);
            float h = sample_curve(haptic_curve_, r);
            set_kv_tag(ringTag, "haptic.level", f2s(h));

            // Beam VFX on pedestal
            if (pedestal_cached_ != entt::null && reg.valid(pedestal_cached_)) {
                auto* pedTag = reg.try_get<CTag>(pedestal_cached_);
                if (!pedTag) pedTag = &reg.emplace<CTag>(pedestal_cached_, CTag{});
                set_kv_tag(*pedTag, "vfx.beam.level", f2s(r));
            }

            // HUD fill animation toward full regardless of current energy application
            float hud_fill = clamp01( (start_energy_ / max_energy_) + (1.0f - (start_energy_ / max_energy_)) * r );
            publish_energy_(w, e, hud_fill);

            if (!in_socket) {
                // Abort
                is_recharging_ = false;
                cooldown_t_ = abort_cooldown_s_;
                set_flag_tag(ringTag, "event.ring.recharge_abort");
                set_kv_tag(ringTag, "haptic.level", "0.0");
                // Clear beam
                if (pedestal_cached_ != entt::null && reg.valid(pedestal_cached_)) {
                    if (auto* pedTag = reg.try_get<CTag>(pedestal_cached_)) {
                        set_kv_tag(*pedTag, "vfx.beam.level", "0.0");
                    }
                }
            } else if (recharge_t_ >= recharge_duration_) {
                // Complete
                energy_ = max_energy_;
                is_recharging_ = false;
                cooldown_t_ = 0.25f;
                set_flag_tag(ringTag, "event.ring.recharge_complete");
                set_flag_tag(ringTag, "event.audio.voice:ring_recharge_complete");
                set_kv_tag(ringTag, "haptic.level", "0.0");
                if (pedestal_cached_ != entt::null && reg.valid(pedestal_cached_)) {
                    if (auto* pedTag = reg.try_get<CTag>(pedestal_cached_)) {
                        set_kv_tag(*pedTag, "vfx.beam.level", "0.0");
                    }
                }
            }
        } else {
            // Regular HUD energy fill to current value
            publish_energy_(w, e, energy_ / max_energy_);
        }

        // Update public energy/status tags for dependent systems
        set_kv_tag(ringTag, "ring.energy", f2s(energy_));
        set_kv_tag(ringTag, "ring.energy_pct", f2s(max_energy_ > 0.0f ? (energy_ / max_energy_) : 0.0f));
        if (energy_ <= low_threshold_) {
            set_flag_tag(ringTag, "hud.prompt:low_energy");
            set_kv_tag(ringTag, "ring.status", "low");
        } else {
            set_kv_tag(ringTag, "ring.status", "ok");
        }
    }

    void on_detach(World&, std::uint32_t) override {}

    std::string id() const override { return "generated.ring_charge_and_recharge"; }

private:
    // Config/state
    float max_energy_{100.0f};
    float energy_{100.0f};
    float low_threshold_{20.0f};
    float passive_regen_per_s_{0.0f};

    std::unordered_map<std::string, float> costs_;
    std::unordered_map<std::string, float> drains_;

    std::vector<float> haptic_curve_;
    std::vector<float> willpower_curve_;
    float willpower_window_s_{30.0f};
    float willpower_max_bonus_pct_{0.10f};
    float willpower_t_{0.0f};

    std::string pedestal_tag_{"recharge_pedestal"};
    float pedestal_radius_{0.30f};
    entt::entity pedestal_cached_{entt::null};

    bool is_recharging_{false};
    float recharge_duration_{2.0f};
    float recharge_t_{0.0f};
    float start_energy_{0.0f};
    float abort_cooldown_s_{1.0f};
    float cooldown_t_{0.0f};

    std::vector<entt::entity> ui_bars_;

    entt::entity find_pedestal_(World& w) {
        auto& reg = w.registry();
        entt::entity out = entt::null;
        auto view = reg.view<CTag, CTransform>();
        for (auto ent : view) {
            auto& tag = view.get<CTag>(ent);
            if (std::find(tag.tags.begin(), tag.tags.end(), pedestal_tag_) != tag.tags.end()) {
                out = ent;
                break;
            }
        }
        return out;
    }
    void publish_energy_(World& w, std::uint32_t ring_e, float fill01) {
        // Update bound UI bars (by entity ids) and ring HUD tag
        fill01 = clamp01(fill01);
        if (w.has<CTag>(ring_e)) {
            auto& ringTag = w.get<CTag>(ring_e);
            set_kv_tag(ringTag, "hud.fill", f2s(fill01));
        }
        auto& reg = w.registry();
        for (auto ee : ui_bars_) {
            if (!reg.valid(ee)) continue;
            auto* tag = reg.try_get<CTag>(ee);
            if (!tag) tag = &reg.emplace<CTag>(ee, CTag{});
            set_kv_tag(*tag, "ui.bar.fill", f2s(fill01));
        }
    }
};

} // namespace ptv

PTV_REGISTER_MECHANIC("generated.ring_charge_and_recharge", ptv::RingChargeAndRecharge)