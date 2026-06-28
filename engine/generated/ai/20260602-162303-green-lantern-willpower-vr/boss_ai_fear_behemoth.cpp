#include "ptv/api/IBehaviour.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <entt/entt.hpp>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <unordered_map>
#include <optional>
#include <algorithm>

namespace ptv {

// Boss AI: Fear Behemoth (multi-phase, armor plates, surges, beams, vulnerability windows)
// Single translation unit to satisfy include whitelist.

class BossAIFearBehemoth : public IBehaviour {
public:
    std::string id() const override { return "generated.boss_ai_fear_behemoth"; }

    void on_attach(World& world, std::uint32_t e, const nlohmann::json& params) override {
        State s{};
        s.health_pct = std::clamp(params.value("health_start_pct", 1.0f), 0.0f, 1.0f);
        s.mission_tier = params.value("mission_tier", 1);
        s.num_plates = std::clamp(params.value("num_plates", 4), 1, 8);
        s.plate_max_hp = params.value("plate_max_hp", 100.0f);
        s.window_seconds = params.value("vuln_window_s", 6.0f);
        s.beam_warmup_s = params.value("beam_warmup_s", 2.0f);
        s.beam_fire_s = params.value("beam_fire_s", 4.0f);
        s.beam_cool_s = params.value("beam_cool_s", 3.0f);
        s.surge_interval_s = params.value("surge_interval_s", 12.0f);
        s.surge_active_s = params.value("surge_active_s", 3.5f);
        s.surge_base_drain = params.value("surge_base_drain", 8.0f); // ring drain per tick unit (abstract)
        s.dome_mitigation = params.value("dome_mitigation", 0.35f);   // 35% damage if under dome
        s.enrage_mult = params.value("enrage_cycle_mult", 1.35f);
        s.spawn_on_phase2 = params.value("spawn_adds_on_p2", true);
        s.spawn_on_phase3 = params.value("spawn_adds_on_p3", true);

        s.plates.assign(s.num_plates, s.plate_max_hp);
        s.phase = Phase::P1;
        s.beam_t = 0.0f;
        s.surge_t = 0.0f;
        s.window_t = -1.0f;
        s.last_surgedrain_encoded.clear();

        states_[e] = s;

        // Preserve base tags and add boss identity tag hints for VFX hooks
        apply_dynamic_tags(world, e, s, /*phase_change_msg=*/"enter_p1");
    }

    void on_tick(World& world, std::uint32_t e, float dt) override {
        auto it = states_.find(e);
        if (it == states_.end()) return;
        State& s = it->second;
        if (!world.valid(e)) return;

        // Pull driving flags/events from this entity's CMechanicBinding (test harness can poke them)
        DrivingInput inp = read_driving_input(world, e);

        // External direct health adjustment hook (e.g., ranged damage simulation)
        if (inp.damage_override.has_value()) {
            s.health_pct = std::clamp(inp.damage_override.value(), 0.0f, 1.0f);
        } else if (inp.damage_delta.has_value()) {
            s.health_pct = std::clamp(s.health_pct + inp.damage_delta.value(), 0.0f, 1.0f);
        }

        // Phase changes by health gates (66%, 33%)
        Phase target_phase = s.phase;
        if (s.phase == Phase::P1 && s.health_pct <= 0.66f) target_phase = Phase::P2;
        if ((s.phase == Phase::P2 || s.phase == Phase::P1) && s.health_pct <= 0.33f) target_phase = Phase::P3;
        if (s.health_pct <= 0.0f) target_phase = Phase::Defeated;
        if (target_phase != s.phase) {
            s.phase = target_phase;
            on_enter_phase(world, e, s);
        }

        // Handle vulnerability window start (ram / stagger)
        if (inp.ram_hit || inp.stagger_combo) {
            s.window_t = s.window_seconds;
            s.window_open = true;
        }

        // Turret pressure contributes during window (softens plates passively)
        if (s.window_open && inp.turret_pressure > 0.0f) {
            const float dps = 2.0f * inp.turret_pressure; // abstract scale
            damage_random_plate(s, dps * dt);
        }

        // Drill contact: heavy damage but only if window open
        if (s.window_open && inp.drill_contact) {
            const float drill_dps = 30.0f;
            damage_random_plate(s, drill_dps * dt);
        }

        // Resolve broken plates -> small health chunk damage and weak point exposure tag
        int broken_now = 0;
        for (float& hp : s.plates) {
            if (hp > 0.0f && hp <= 0.001f) {
                hp = 0.0f;
                broken_now++;
                // Boss loses a chunk of health when a plate breaks
                s.health_pct = std::clamp(s.health_pct - 0.05f, 0.0f, 1.0f);
            }
        }
        if (broken_now > 0) {
            s.last_event_msgs.push_back("plate_broken:" + std::to_string(broken_now));
        }

        // Update timers (phase-driven)
        switch (s.phase) {
            case Phase::P1: tick_beam_p1(world, e, s, dt); break;
            case Phase::P2: tick_phase2(world, e, s, dt); break;
            case Phase::P3: tick_phase3(world, e, s, dt, inp.dome_present); break;
            case Phase::Defeated: tick_defeated(world, e, s); break;
        }

        // Update vulnerability window countdown
        if (s.window_open) {
            s.window_t -= dt;
            if (s.window_t <= 0.0f) {
                s.window_open = false;
                s.window_t = -1.0f;
            }
        }

        // Emit dynamic tags for VFX/scripts and acceptance hooks
        apply_dynamic_tags(world, e, s);
    }

    void on_detach(World& world, std::uint32_t e) override {
        // Clear our dynamic tags but preserve base ones if present
        if (world.has<CTag>(e)) {
            auto base = world.get<CTag>(e).tags;
            std::vector<std::string> kept;
            for (const auto& t : base) {
                if (!starts_with(t, "boss:")) kept.push_back(t);
            }
            world.emplace_or_replace<CTag>(e, CTag{kept});
        }
        states_.erase(e);
    }

private:
    enum class Phase { P1, P2, P3, Defeated };

    struct State {
        // Tunables
        int mission_tier{1};
        int num_plates{4};
        float plate_max_hp{100.0f};
        float window_seconds{6.0f};
        float beam_warmup_s{2.0f};
        float beam_fire_s{4.0f};
        float beam_cool_s{3.0f};
        float surge_interval_s{12.0f};
        float surge_active_s{3.5f};
        float surge_base_drain{8.0f};
        float dome_mitigation{0.35f};
        float enrage_mult{1.35f};
        bool spawn_on_phase2{true};
        bool spawn_on_phase3{true};

        // Runtime
        Phase phase{Phase::P1};
        float health_pct{1.0f};
        std::vector<float> plates; // size = num_plates

        // P1 beam cycle timers
        float beam_t{0.0f};
        // P3 surge timers
        float surge_t{0.0f};

        // Vulnerability window
        bool window_open{false};
        float window_t{-1.0f};

        // Sticky ephemeral reporting
        std::vector<std::string> last_event_msgs;
        std::string last_surgedrain_encoded;
    };

    struct DrivingInput {
        bool ram_hit{false};
        bool stagger_combo{false};
        bool drill_contact{false};
        float turret_pressure{0.0f}; // 0..1 suggested
        bool dome_present{false};
        std::optional<float> damage_delta;   // add to health_pct (may be negative)
        std::optional<float> damage_override; // direct set health_pct
    };

    static bool starts_with(const std::string& s, const std::string& p) {
        return s.size() >= p.size() && std::equal(p.begin(), p.end(), s.begin());
    }

    void on_enter_phase(World& world, std::uint32_t e, State& s) {
        switch (s.phase) {
            case Phase::P2:
                // Open plates (exposed weak points available after staggers)
                s.window_open = false; s.window_t = -1.0f; // reset; windows will be granted by future staggers
                if (s.spawn_on_phase2) s.last_event_msgs.push_back("spawn_adds_request");
                s.last_event_msgs.push_back("enter_p2");
                break;
            case Phase::P3:
                // Enrage: faster cycles
                s.beam_warmup_s /= s.enrage_mult;
                s.beam_fire_s   /= s.enrage_mult;
                s.beam_cool_s   /= s.enrage_mult;
                s.surge_interval_s /= s.enrage_mult;
                s.last_event_msgs.push_back("enter_p3");
                if (s.spawn_on_phase3) s.last_event_msgs.push_back("spawn_adds_request");
                break;
            case Phase::Defeated:
                s.last_event_msgs.push_back("boss_defeated");
                break;
            case Phase::P1:
                s.last_event_msgs.push_back("enter_p1");
                break;
        }
        apply_dynamic_tags(world, e, s, /*phase_change_msg=*/nullptr);
    }

    void tick_beam_p1(World&, std::uint32_t, State& s, float dt) {
        // Cycle: warmup -> fire -> cooldown
        s.beam_t += dt;
        const float cycle = s.beam_warmup_s + s.beam_fire_s + s.beam_cool_s;
        float t = std::fmod(s.beam_t, std::max(cycle, 0.001f));
        bool warm = t < s.beam_warmup_s;
        bool fire = t >= s.beam_warmup_s && t < (s.beam_warmup_s + s.beam_fire_s);
        // Report via msgs for tag generation
        if (warm) {
            push_unique(s.last_event_msgs, "beam:warmup");
        } else if (fire) {
            push_unique(s.last_event_msgs, "beam:firing");
        } else {
            push_unique(s.last_event_msgs, "beam:cooldown");
        }
    }

    void tick_phase2(World& world, std::uint32_t e, State& s, float dt) {
        // Maintain occasional brief weak spot pings even without a window (minor openings)
        minor_weakspot_pulses(s, dt);
        // Also keep a slower beam pattern, shorter bursts
        tick_beam_p1(world, e, s, dt * 0.6f);
    }

    void tick_phase3(World& world, std::uint32_t e, State& s, float dt, bool dome_present) {
        // Corruption surges
        s.surge_t += dt;
        float cycle = s.surge_interval_s;
        float t = std::fmod(s.surge_t, std::max(cycle, 0.001f));
        bool surge_active = (t < s.surge_active_s);
        if (surge_active) {
            float drain = s.surge_base_drain * (dome_present ? s.dome_mitigation : 1.0f);
            s.last_surgedrain_encoded = std::string("surge_drain_last:") + to_fixed_2(drain) + (dome_present ? ":inside_dome" : ":outside");
            push_unique(s.last_event_msgs, "surge:active");
        } else {
            push_unique(s.last_event_msgs, "surge:idle");
        }
        tick_beam_p1(world, e, s, dt * 1.1f);
    }

    void tick_defeated(World&, std::uint32_t, State& s) {
        // No-op; tags will advertise defeated state
        (void)s;
    }

    void minor_weakspot_pulses(State& s, float dt) {
        // Lightweight sinusoidal chance to flicker a minor weak spot hint
        s.beam_t += dt; // reuse timer
        float ph = std::fmod(s.beam_t, 5.0f);
        if (ph < 0.2f) push_unique(s.last_event_msgs, "weakspot:flicker");
    }

    void damage_random_plate(State& s, float dmg) {
        if (s.plates.empty() || dmg <= 0.0f) return;
        // Pick the first non-broken plate (deterministic for tests)
        auto it = std::find_if(s.plates.begin(), s.plates.end(), [](float hp){ return hp > 0.0f; });
        if (it == s.plates.end()) return;
        *it = std::max(0.0f, *it - dmg);
    }

    static void push_unique(std::vector<std::string>& v, const std::string& s) {
        if (std::find(v.begin(), v.end(), s) == v.end()) v.push_back(s);
    }

    static std::string to_fixed_2(float x) {
        int i = static_cast<int>(std::round(x * 100.0f));
        int abs_i = std::abs(i);
        int hi = abs_i / 100;
        int lo = abs_i % 100;
        std::string sign = (i < 0 ? "-" : "");
        std::string s = sign + std::to_string(hi) + "." + (lo < 10 ? "0" : "") + std::to_string(lo);
        return s;
    }

    DrivingInput read_driving_input(World& world, std::uint32_t e) {
        DrivingInput out{};
        if (!world.has<CMechanicBinding>(e)) return out;
        auto& bindings = world.get<CMechanicBinding>(e).entries;
        for (const auto& entry : bindings) {
            // Accept either builtin enemy hook or this behaviour's own id; tests can poke either
            bool accept = (!entry.id.empty() && (entry.id == id() || entry.id == "builtin.enemy_ai_hook"));
            if (!accept) continue;
            auto j = nlohmann::json::parse(entry.params_json, nullptr, /*allow_exceptions=*/false);
            if (!j.is_object()) continue;
            if (j.contains("ram_hit")) out.ram_hit = j.at("ram_hit").get<bool>();
            if (j.contains("stagger_combo")) out.stagger_combo = j.at("stagger_combo").get<bool>();
            if (j.contains("drill_contact")) out.drill_contact = j.at("drill_contact").get<bool>();
            if (j.contains("turret_pressure")) out.turret_pressure = std::clamp(j.at("turret_pressure").get<float>(), 0.0f, 5.0f);
            if (j.contains("dome_present")) out.dome_present = j.at("dome_present").get<bool>();
            if (j.contains("damage_delta")) out.damage_delta = j.at("damage_delta").get<float>();
            if (j.contains("damage_override")) out.damage_override = j.at("damage_override").get<float>();
        }
        return out;
    }

    void apply_dynamic_tags(World& world, std::uint32_t e, const State& s, const char* phase_change_msg = nullptr) {
        std::vector<std::string> base;
        if (world.has<CTag>(e)) base = world.get<CTag>(e).tags;

        // Strip previous boss:* dynamic tags to avoid growth
        std::vector<std::string> kept;
        kept.reserve(base.size());
        for (const auto& t : base) {
            if (!starts_with(t, "boss:")) kept.push_back(t);
        }

        // Phase tag
        kept.push_back(std::string("boss:phase:") + phase_to_str(s.phase));
        // Health encoding for HUD/testing
        kept.push_back(std::string("boss:health:") + to_fixed_2(s.health_pct));

        // Window tags
        kept.push_back(std::string("boss:vuln:") + (s.window_open ? "open" : "closed"));
        if (s.window_open) {
            kept.push_back(std::string("boss:vuln_t:") + to_fixed_2(std::max(0.0f, s.window_t)));
            kept.push_back("boss:plates:exposed");
        }

        // Beam state hints (from last_event_msgs)
        if (contains_msg(s.last_event_msgs, "beam:warmup")) kept.push_back("boss:beam:warmup");
        if (contains_msg(s.last_event_msgs, "beam:firing")) kept.push_back("boss:beam:firing");
        if (contains_msg(s.last_event_msgs, "beam:cooldown")) kept.push_back("boss:beam:cooldown");

        // Surge hints
        if (contains_msg(s.last_event_msgs, "surge:active")) kept.push_back("boss:surge:active");
        if (contains_msg(s.last_event_msgs, "surge:idle")) kept.push_back("boss:surge:idle");
        if (!s.last_surgedrain_encoded.empty()) kept.push_back(std::string("boss:") + s.last_surgedrain_encoded);

        // Minor weakspot flicker tag in P2 when applicable
        if (contains_msg(s.last_event_msgs, "weakspot:flicker")) kept.push_back("boss:weakspot:flicker");

        // Plate state summary
        int plates_alive = 0;
        for (float hp : s.plates) if (hp > 0.0f) plates_alive++;
        kept.push_back(std::string("boss:plates_alive:") + std::to_string(plates_alive));
        if (contains_msg(s.last_event_msgs, "plate_broken:1")) kept.push_back("boss:plate_broken");

        // Add spawns request pulses
        if (contains_msg(s.last_event_msgs, "spawn_adds_request")) kept.push_back("boss:spawn_adds_request");

        // Phase change marker (one-shot)
        if (phase_change_msg) kept.push_back(std::string("boss:") + phase_change_msg);

        world.emplace_or_replace<CTag>(e, CTag{kept});
    }

    static const char* phase_to_str(Phase p) {
        switch (p) {
            case Phase::P1: return "p1"; case Phase::P2: return "p2"; case Phase::P3: return "p3"; default: return "defeated";
        }
    }

    static bool contains_msg(const std::vector<std::string>& v, const char* needle) {
        return std::find(v.begin(), v.end(), std::string(needle)) != v.end();
    }

    std::unordered_map<std::uint32_t, State> states_;
};

PTV_REGISTER_BEHAVIOUR("generated.boss_ai_fear_behemoth", BossAIFearBehemoth)

} // namespace ptv
