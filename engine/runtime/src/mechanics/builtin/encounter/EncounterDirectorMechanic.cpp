// builtin.encounter_director -- the scene's NPC population.
//
// Every game with enemies that "keep coming" needs the same machinery: hidden
// templates cloned into live actors, waves that start on some trigger, a cap
// on how many are alive at once, spawn points away from the player and out of
// their sight, defeat counting, and a way for the HUD to say "wave 3 of 5,
// 4 left". Written per game by a code-generation agent, it disagreed with the
// scene it ran in -- one game's spawner pooled entities tagged "pooled" that
// the scene never tagged, and not a single enemy ever appeared.
//
// So it is the engine's. The encounter plan (compiled from the NPC plan into
// this binding's params) says WHAT arrives WHEN; this module does the rest
// and publishes its state through ``ctx.encounters`` for generated code.
//
// Params (see EncounterPlan.h): mode (waves | continuous | fixed), start
// {kind: on_load | delay | player_grabs_tag | signal, delay_s, tag},
// max_concurrent, spawn_interval_s, inter_wave_delay_s,
// min_player_distance_m, max_player_distance_m,
// waves [{groups: [{template, count}]}], zones [{id, position, radius, tags}].

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../BuiltinMechanicBase.h"
#include "EncounterPlan.h"
#include "ptv/core/Logger.h"

#include <glm/gtc/quaternion.hpp>

namespace ptv {
namespace {

constexpr std::uint32_t kInvalid = 0xFFFFFFFFu;
/** After a refused spawn (budget, blocked spot), try again this soon. */
constexpr float kRetryS = 0.5f;

bool has_tag(const World& world, std::uint32_t e, const std::string& tag) {
    if (!world.valid(e) || !world.has<CTag>(e)) return false;
    for (const auto& t : world.get<CTag>(e).tags) {
        if (t == tag) return true;
    }
    return false;
}

class EncounterDirectorMechanic final
    : public BuiltinMechanicBase<EncounterDirectorMechanic> {
public:
    static constexpr const char* kId = "builtin.encounter_director";

    // One per game, however many entities carry the binding.
    MechanicScope scope() const override { return MechanicScope::Player; }

    void configure(const nlohmann::json& p) {
        plan_ = encounter::parse(p);
        resolved_ = false;
        reset_runtime();
        if (plan_.waves.empty()) {
            PTV_WARN("encounter: no waves in the plan; nothing will spawn");
        }
    }

    void on_detach(World& world, std::uint32_t) override {
        (void)world;
        live_.clear();
    }

    void tick(World& world, std::uint32_t, float dt, const InputSnapshot& input,
              MechanicContext& ctx) {
        EncounterState& s = state(ctx);
        s.configured = !plan_.waves.empty();
        s.wave_count = static_cast<int>(plan_.waves.size());
        if (!s.configured) return;
        if (!resolved_) resolve(world);

        if (s.reset_requested) {
            s.reset_requested = false;
            despawn_all(ctx);
            reset_runtime();
            s.phase = EncounterPhase::Idle;
            s.wave_index = 0;
            s.defeated_total = 0;
            PTV_INFO("encounter: reset");
        }

        prune(world, s);
        publish(s);

        const std::uint32_t player = find_player(world, ctx);
        if (player != kInvalid && world.has<CHealth>(player) &&
            !world.get<CHealth>(player).alive &&
            s.phase != EncounterPhase::Victory) {
            if (s.phase != EncounterPhase::Failed) {
                PTV_INFO("encounter: the player died; spawning stops");
            }
            s.phase = EncounterPhase::Failed;
            return;
        }
        if (s.paused) return;

        tick_phase(world, dt, input, ctx, s, player);
        publish(s);
    }

private:
    void publish(EncounterState& s) const {
        s.alive = static_cast<int>(live_.size());
        s.remaining_in_wave =
            static_cast<int>(queue_.size() - std::min(next_, queue_.size())) + s.alive;
    }

    void tick_phase(World& world, float dt, const InputSnapshot& input,
                    MechanicContext& ctx, EncounterState& s, std::uint32_t player) {
        switch (s.phase) {
            case EncounterPhase::Idle:
                tick_idle(world, dt, input, s);
                // A wave that starts spawns on the same frame.
                if (s.phase == EncounterPhase::Active) {
                    tick_active(world, 0.0f, ctx, s, player);
                }
                break;
            case EncounterPhase::Active:
                tick_active(world, dt, ctx, s, player);
                break;
            case EncounterPhase::InterWave:
                s.countdown_s = std::max(0.0f, s.countdown_s - dt);
                if (s.countdown_s <= 0.0f) {
                    begin_wave(s, s.wave_index + 1);
                    if (s.phase == EncounterPhase::Active) {
                        tick_active(world, 0.0f, ctx, s, player);
                    }
                }
                break;
            case EncounterPhase::Victory:
            case EncounterPhase::Failed:
            default:
                break;
        }
    }

    EncounterState& state(MechanicContext& ctx) {
        if (ctx.encounters != nullptr) {
            if (EncounterState* st = ctx.encounters->director_state()) return *st;
        }
        return local_state_;
    }

    void reset_runtime() {
        live_.clear();
        counted_.clear();
        queue_.clear();
        next_ = 0;
        spawn_timer_s_ = 0.0f;
        start_timer_s_ = 0.0f;
        spawn_counter_ = 0;
    }

    void resolve(World& world) {
        resolved_ = true;
        std::unordered_set<std::string> missing;
        for (const auto& w : plan_.waves) {
            for (const auto& g : w.groups) {
                if (templates_.count(g.template_id) != 0) continue;
                const auto e = world.find(g.template_id);
                if (e == entt::null) {
                    missing.insert(g.template_id);
                    continue;
                }
                templates_[g.template_id] = static_cast<std::uint32_t>(e);
            }
        }
        for (const auto& m : missing) {
            PTV_WARN("encounter: no scene entity '{}' to spawn from; that "
                     "group is skipped",
                     m);
        }
        PTV_INFO("encounter: {} wave(s), {} NPC type(s), {} spawn zone(s), "
                 "cap {} alive",
                 plan_.waves.size(), templates_.size(), plan_.zones.size(),
                 plan_.max_concurrent);
    }

    std::uint32_t find_player(World& world, MechanicContext& ctx) const {
        if (ctx.hands != nullptr) {
            const std::uint32_t p = ctx.hands->player_entity();
            if (p != kInvalid && world.valid(p)) return p;
        }
        auto view = world.registry().view<const CTag>();
        for (const auto e : view) {
            for (const auto& t : view.get<const CTag>(e).tags) {
                if (t == "player") return static_cast<std::uint32_t>(e);
            }
        }
        return kInvalid;
    }

    void tick_idle(World& world, float dt, const InputSnapshot& input,
                   EncounterState& s) {
        bool go = s.start_requested;
        switch (plan_.start) {
            case encounter::Start::OnLoad:
                go = true;
                break;
            case encounter::Start::Delay:
                start_timer_s_ += dt;
                s.countdown_s =
                    std::max(0.0f, plan_.start_delay_s - start_timer_s_);
                go = go || start_timer_s_ >= plan_.start_delay_s;
                break;
            case encounter::Start::PlayerGrabsTag:
                for (int h = 0; h < 2; ++h) {
                    const std::uint32_t held = input.hands[h].held_entity;
                    if (held != kInvalid && has_tag(world, held, plan_.start_tag)) {
                        go = true;
                    }
                }
                break;
            case encounter::Start::Signal:
            default:
                break;
        }
        if (!go) return;
        s.start_requested = false;
        begin_wave(s, 0);
    }

    void begin_wave(EncounterState& s, int wave) {
        const int n = static_cast<int>(plan_.waves.size());
        if (wave >= n) {
            if (plan_.mode == encounter::Mode::Continuous && n > 0) {
                wave = n - 1;  // endless: the last wave repeats
            } else {
                s.phase = EncounterPhase::Victory;
                ++s.victory_seq;
                PTV_INFO("encounter: every wave cleared");
                return;
            }
        }
        s.wave_index = wave;
        queue_ = encounter::spawn_order(plan_.waves[static_cast<std::size_t>(wave)]);
        next_ = 0;
        spawn_timer_s_ = 0.0f;
        s.countdown_s = 0.0f;
        s.phase = EncounterPhase::Active;
        ++s.wave_started_seq;
        PTV_INFO("encounter: wave {}/{} started ({} to spawn)", wave + 1, n,
                 queue_.size());
    }

    void tick_active(World& world, float dt, MechanicContext& ctx,
                     EncounterState& s, std::uint32_t player) {
        spawn_timer_s_ = std::max(0.0f, spawn_timer_s_ - dt);
        while (next_ < queue_.size() && spawn_timer_s_ <= 0.0f &&
               static_cast<int>(live_.size()) < plan_.max_concurrent) {
            const std::string& tid = queue_[next_];
            const auto it = templates_.find(tid);
            if (it == templates_.end()) {
                ++next_;  // unknown type: skip rather than stall the wave
                continue;
            }
            const std::uint32_t made = spawn_one(world, ctx, it->second, player);
            if (made == kInvalid) {
                spawn_timer_s_ = kRetryS;
                break;
            }
            live_.push_back(made);
            ++next_;
            spawn_timer_s_ = plan_.spawn_interval_s;
            if (plan_.spawn_interval_s > 0.0f) break;
        }

        if (next_ >= queue_.size() && live_.empty()) {
            ++s.wave_cleared_seq;
            PTV_INFO("encounter: wave {} cleared", s.wave_index + 1);
            const bool last =
                s.wave_index + 1 >= static_cast<int>(plan_.waves.size());
            if (plan_.mode == encounter::Mode::Fixed ||
                (last && plan_.mode == encounter::Mode::Waves)) {
                s.phase = EncounterPhase::Victory;
                ++s.victory_seq;
                PTV_INFO("encounter: every wave cleared");
                return;
            }
            s.phase = EncounterPhase::InterWave;
            s.countdown_s = plan_.inter_wave_delay_s;
        }
    }

    /** Living spawns stay; the dead and the destroyed are counted once. */
    void prune(World& world, EncounterState& s) {
        auto it = live_.begin();
        while (it != live_.end()) {
            const std::uint32_t e = *it;
            const bool gone = !world.valid(e) ||
                              (world.has<CHealth>(e) && !world.get<CHealth>(e).alive);
            if (!gone) {
                ++it;
                continue;
            }
            if (counted_.insert(e).second) ++s.defeated_total;
            it = live_.erase(it);
        }
    }

    std::uint32_t spawn_one(World& world, MechanicContext& ctx,
                            std::uint32_t tmpl, std::uint32_t player) {
        if (ctx.spawn == nullptr || !world.valid(tmpl)) return kInvalid;
        Vec3 player_pos(0.0f);
        if (player != kInvalid && world.has<CTransform>(player)) {
            player_pos = world.get<CTransform>(player).t.position;
        }
        Vec3 pos;
        if (!choose_spot(ctx, player_pos, pos)) return kInvalid;

        // Arrive facing the player. The spot is on the floor; the body's own
        // ground snap (character locomotion, using the measured foot offset)
        // lifts it to stand on it.
        Vec3 to = player_pos - pos;
        to.y = 0.0f;
        Quat facing(1.0f, 0.0f, 0.0f, 0.0f);
        if (glm::length(to) > 1e-3f) {
            facing = glm::angleAxis(std::atan2(-to.x, -to.z), Vec3(0, 1, 0));
        }
        const std::uint32_t e = ctx.spawn->instance(tmpl, pos, facing);
        if (e != kInvalid) ++spawn_counter_;
        return e;
    }

    /** A spawn point: a declared zone far enough from the player and,
     *  preferably, out of their sight; else a ring around the player. */
    bool choose_spot(MechanicContext& ctx, const Vec3& player, Vec3& out) {
        const Vec3 eye = player + Vec3(0.0f, 0.7f, 0.0f);
        std::vector<int> hidden, visible, far_ok;
        for (int i = 0; i < static_cast<int>(plan_.zones.size()); ++i) {
            const auto& z = plan_.zones[static_cast<std::size_t>(i)];
            const float d = glm::length(Vec3(z.position.x - player.x, 0.0f,
                                             z.position.z - player.z));
            if (d < plan_.min_player_distance_m) continue;
            if (d > plan_.max_player_distance_m) {
                far_ok.push_back(i);
                continue;
            }
            const bool seen =
                ctx.nav != nullptr &&
                ctx.nav->line_of_sight(eye, z.position + Vec3(0.0f, 1.0f, 0.0f));
            (seen ? visible : hidden).push_back(i);
        }
        const std::vector<int>& pick =
            !hidden.empty() ? hidden : (!visible.empty() ? visible : far_ok);
        if (!pick.empty()) {
            const int zi = pick[static_cast<std::size_t>(spawn_counter_) % pick.size()];
            const auto& z = plan_.zones[static_cast<std::size_t>(zi)];
            out = encounter::point_in_zone(z, spawn_counter_ / static_cast<int>(pick.size()));
            if (ctx.nav != nullptr) out = ctx.nav->project_to_ground(out);
            return true;
        }
        // No usable zone: a ring around the player at a respectful distance,
        // on reachable floor when the level declared any.
        const float r = plan_.min_player_distance_m + 2.0f;
        for (int k = 0; k < 12; ++k) {
            const float a = 2.39996323f * static_cast<float>(spawn_counter_ + k);
            Vec3 p = player + Vec3(r * std::cos(a), 0.0f, r * std::sin(a));
            if (ctx.nav != nullptr) {
                p = ctx.nav->project_to_ground(p);
                if (!ctx.nav->is_reachable(player, p)) continue;
            }
            out = p;
            return true;
        }
        return false;
    }

    void despawn_all(MechanicContext& ctx) {
        if (ctx.spawn == nullptr) return;
        for (const auto e : live_) ctx.spawn->despawn(e);
        for (const auto e : counted_) ctx.spawn->despawn(e);
    }

    encounter::Plan plan_{};
    bool resolved_{false};
    std::unordered_map<std::string, std::uint32_t> templates_;
    std::vector<std::uint32_t> live_;
    std::unordered_set<std::uint32_t> counted_;
    std::vector<std::string> queue_;
    std::size_t next_{0};
    float spawn_timer_s_{0.0f};
    float start_timer_s_{0.0f};
    int spawn_counter_{0};
    EncounterState local_state_{};
};

}  // namespace

extern "C" int ptv_force_link_builtin_encounter_director = 1;

PTV_REGISTER_MECHANIC("builtin.encounter_director", EncounterDirectorMechanic)

}  // namespace ptv
