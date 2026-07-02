#pragma once

#include "ptv/api/IBehaviour.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <unordered_map>
#include <string>
#include <cstdint>

#include "boss_shared.h"

namespace ptv {

class FearBehemothBossBehaviour : public IBehaviour {
public:
    // IBehaviour
    void on_attach(World& world, std::uint32_t entity, const nlohmann::json& params) override;
    void on_tick(World& world, std::uint32_t entity, float dt) override;
    void on_detach(World& world, std::uint32_t entity) override;
    std::string id() const override { return "generated.boss_ai_fear_behemoth"; }

private:
    // Helpers split across cpp files
    behemoth::InputParams read_runtime_params(World& world, std::uint32_t entity);

    void update_state_machine(World& world, std::uint32_t entity, float dt, const behemoth::InputParams& in);
    void handle_health_gates(World& world, std::uint32_t entity, behemoth::BossData& b);

    void update_attacks(World& world, std::uint32_t entity, float dt, const behemoth::InputParams& in, behemoth::BossData& b);
    void tick_beam_sweep(World& world, std::uint32_t entity, float dt, behemoth::BossData& b);
    void tick_surge(World& world, std::uint32_t entity, float dt, const behemoth::InputParams& in, behemoth::BossData& b);
    void tick_add_spawns(World& world, std::uint32_t entity, float dt, behemoth::BossData& b);

    void update_vulnerability(World& world, std::uint32_t entity, float dt, const behemoth::InputParams& in, behemoth::BossData& b);
    void open_vulnerability_plate(World& world, std::uint32_t entity, behemoth::BossData& b);

    void fire_arena_event_tag(World& world, std::uint32_t entity, const std::string& tag);

private:
    std::unordered_map<std::uint32_t, behemoth::BossData> state_;
};

} // namespace ptv
