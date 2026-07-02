#include "boss_internal.h"

#include <algorithm>

namespace ptv {
using namespace behemoth;

void FearBehemothBossBehaviour::on_attach(World& world, std::uint32_t entity, const nlohmann::json& params) {
    BossData b{};
    // Initial tuning from params (optional)
    int mission_tier = params.value("mission_tier", 1);
    mission_tier = std::clamp(mission_tier, 1, 3);
    b.enrage_mult = 1.0f + 0.15f * (mission_tier - 1);

    // Plates
    for (auto& p : b.plates) {
        p.max_counter = 100.0f;
        p.counter = p.max_counter;
        p.open = false;
        p.broken = false;
    }

    // Timers baseline
    b.atk.beam_cooldown_s = 4.0f;
    b.atk.beam_active_duration_s = 2.0f;
    b.atk.surge_period_s = 8.0f;
    b.atk.surge_active_duration_s = 1.25f;
    b.atk.add_period_s = 10.0f;

    state_[entity] = b;

    // Tag boss entity for debug/visibility
    if (world.has<ptv::CTag>(entity)) {
        auto& tg = world.get<ptv::CTag>(entity);
        tg.tags.push_back("boss:fear_behemoth:attached");
        tg.tags.push_back("phase:P1");
    } else {
        ptv::CTag tg; tg.tags = {"boss:fear_behemoth:attached", "phase:P1"};
        world.emplace_or_replace<ptv::CTag>(entity, tg);
    }
}

void FearBehemothBossBehaviour::on_detach(World& world, std::uint32_t entity) {
    // Clear transient tags we might have added
    if (world.has<ptv::CTag>(entity)) {
        auto& tg = world.get<ptv::CTag>(entity);
        tg.tags.push_back("boss:fear_behemoth:detached");
    }
    state_.erase(entity);
}

behemoth::InputParams FearBehemothBossBehaviour::read_runtime_params(World& world, std::uint32_t entity) {
    // Pull our binding's latest params_json so test harness/scripts can drive events at runtime
    if (!world.has<ptv::CMechanicBinding>(entity)) return {};
    const auto& mb = world.get<ptv::CMechanicBinding>(entity);
    for (const auto& e : mb.entries) {
        if (e.id == "generated.boss_ai_fear_behemoth" || e.builtin_id == "builtin.enemy_ai_hook") {
            auto j = nlohmann::json::parse(e.params_json, nullptr, false);
            return behemoth::parse_params(j);
        }
    }
    return {};
}

void FearBehemothBossBehaviour::handle_health_gates(World& world, std::uint32_t entity, BossData& b) {
    if (!b.gate_66_fired && b.health01 <= 0.66f) {
        b.gate_66_fired = true;
        b.phase = Phase::P2;
        fire_arena_event_tag(world, entity, "arena_shift:phase2_cover");
        if (world.has<ptv::CTag>(entity)) world.get<ptv::CTag>(entity).tags.push_back("phase:P2");
        // Open at least one plate to seed the phase goal
        open_vulnerability_plate(world, entity, b);
    }
    if (!b.gate_33_fired && b.health01 <= 0.33f) {
        b.gate_33_fired = true;
        b.phase = Phase::P3;
        b.enrage_mult = std::max(b.enrage_mult, 1.5f);
        fire_arena_event_tag(world, entity, "arena_shift:phase3_corruption");
        if (world.has<ptv::CTag>(entity)) world.get<ptv::CTag>(entity).tags.push_back("phase:P3");
    }
    if (b.health01 <= 0.0f && b.phase != Phase::Dead) {
        b.phase = Phase::Dead;
        fire_arena_event_tag(world, entity, "boss:defeated");
        if (world.has<ptv::CTag>(entity)) world.get<ptv::CTag>(entity).tags.push_back("phase:Dead");
    }
}

void FearBehemothBossBehaviour::update_state_machine(World& world, std::uint32_t entity, float dt, const behemoth::InputParams& in) {
    auto it = state_.find(entity);
    if (it == state_.end()) return;
    BossData& b = it->second;

    // Minor passive drain to move fight forward when plates are all broken
    bool all_broken = true;
    for (const auto& p : b.plates) { if (!p.broken) { all_broken = false; break; } }
    if (all_broken) {
        b.health01 -= 0.05f * dt; // soft fail-safe to conclude fight
    }

    // Phase triggers from inputs
    if (in.ram_hit && !b.vuln.active && b.phase != Phase::Dead) {
        open_vulnerability_plate(world, entity, b);
    }

    handle_health_gates(world, entity, b);
}

void FearBehemothBossBehaviour::on_tick(World& world, std::uint32_t entity, float dt) {
    if (!world.valid(entity)) return;
    auto it = state_.find(entity);
    if (it == state_.end()) return;

    // Read external inputs (may be mutated by harness in CMechanicBinding)
    behemoth::InputParams in = read_runtime_params(world, entity);

    // Slight pacing scale from mission tier
    it->second.enrage_mult = std::max(it->second.enrage_mult, 1.0f + 0.15f * (in.mission_tier - 1));
    float pace = it->second.enrage_mult;
    dt *= pace;

    update_state_machine(world, entity, dt, in);
    update_attacks(world, entity, dt, in, it->second);
    update_vulnerability(world, entity, dt, in, it->second);

    // Subtle surge visual hint: pulse scale during active surge
    if (world.has<ptv::CTransform>(entity)) {
        auto& tr = world.get<ptv::CTransform>(entity).t;
        if (it->second.atk.surge_active) {
            tr.scale = 2.2f * (1.0f + 0.03f * std::sin(it->second.atk.surge_active_s * 12.56637f));
        } else {
            tr.scale = 2.2f;
        }
    }
}

} // namespace ptv
