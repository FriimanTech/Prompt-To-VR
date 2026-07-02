#include "boss_internal.h"

#include <string>

namespace ptv {
using namespace behemoth;

void FearBehemothBossBehaviour::update_attacks(World& world, std::uint32_t entity, float dt, const InputParams& in, BossData& b) {
    if (b.phase == Phase::Dead) return;
    tick_beam_sweep(world, entity, dt, b);
    if (b.phase == Phase::P3) {
        tick_surge(world, entity, dt, in, b);
    }
    tick_add_spawns(world, entity, dt, b);
}

void FearBehemothBossBehaviour::tick_beam_sweep(World& world, std::uint32_t entity, float dt, BossData& b) {
    // P1 frequent, P2 occasional, P3 minimal
    float cd = b.atk.beam_cooldown_s;
    if (b.phase == Phase::P2) cd *= 1.25f;
    if (b.phase == Phase::P3) cd *= 1.5f;

    if (!b.atk.beam_active) {
        b.atk.beam_timer_s += dt;
        if (b.atk.beam_timer_s >= cd) {
            b.atk.beam_timer_s = 0.0f;
            b.atk.beam_active = true;
            b.atk.beam_active_s = 0.0f;
            if (world.has<ptv::CTag>(entity)) world.get<ptv::CTag>(entity).tags.push_back("attack:beam_sweep:start");
        }
    } else {
        b.atk.beam_active_s += dt;
        if (b.atk.beam_active_s >= b.atk.beam_active_duration_s) {
            b.atk.beam_active = false;
            if (world.has<ptv::CTag>(entity)) world.get<ptv::CTag>(entity).tags.push_back("attack:beam_sweep:end");
        } else {
            // While active, briefly expose tiny weak flickers in P1 for fairness
            if (b.phase == Phase::P1 && !b.vuln.active) {
                if (world.has<ptv::CTag>(entity)) world.get<ptv::CTag>(entity).tags.push_back("hint:minor_weak_spot_flicker");
            }
        }
    }
}

void FearBehemothBossBehaviour::tick_surge(World& world, std::uint32_t entity, float dt, const InputParams& in, BossData& b) {
    if (!b.atk.surge_active) {
        b.atk.surge_timer_s += dt;
        if (b.atk.surge_timer_s >= b.atk.surge_period_s) {
            b.atk.surge_timer_s = 0.0f;
            b.atk.surge_active = true;
            b.atk.surge_active_s = 0.0f;
            if (world.has<ptv::CTag>(entity)) world.get<ptv::CTag>(entity).tags.push_back("hazard:yellow_surge:start");
        }
    } else {
        b.atk.surge_active_s += dt;
        // Apply drain effect; tests can compare inside dome vs outside via tag note
        if (world.has<ptv::CTag>(entity)) {
            auto& tg = world.get<ptv::CTag>(entity);
            if (in.dome_shield_active) tg.tags.push_back("effect:ring_drain:mitigated");
            else tg.tags.push_back("effect:ring_drain:full");
        }
        if (b.atk.surge_active_s >= b.atk.surge_active_duration_s) {
            b.atk.surge_active = false;
            if (world.has<ptv::CTag>(entity)) world.get<ptv::CTag>(entity).tags.push_back("hazard:yellow_surge:end");
            // Enrage speeds up future surges a bit each cycle
            b.atk.surge_period_s = std::max(4.0f, b.atk.surge_period_s * 0.95f);
        }
    }
}

void FearBehemothBossBehaviour::tick_add_spawns(World& world, std::uint32_t entity, float dt, BossData& b) {
    b.atk.add_timer_s += dt;
    float per = b.atk.add_period_s;
    if (b.phase == Phase::P1) per = 9999.0f; // no spawns P1
    if (b.phase == Phase::P2) per = b.atk.add_period_s;
    if (b.phase == Phase::P3) per = std::max(6.0f, b.atk.add_period_s * 0.8f);

    if (b.atk.add_timer_s >= per) {
        b.atk.add_timer_s = 0.0f;
        // Emit a spawn command via tag; a spawner system can consume this
        int count = (b.phase == Phase::P3 ? 4 : 2);
        if (world.has<ptv::CTag>(entity)) {
            auto& tg = world.get<ptv::CTag>(entity);
            tg.tags.push_back(std::string("cmd:spawn:adds:") + (count==4?"4":"2"));
        }
    }
}

} // namespace ptv
