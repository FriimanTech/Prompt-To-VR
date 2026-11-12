// builtin.target_challenge -- a timed (or score) challenge that spawns
// targets at world-declared points and counts what the player hits.
//
// "Press the red button to start a 20-second challenge that spawns targets
// and counts hits" is a whole category of VR game (shooting galleries, time
// trials, whack-a-mole, archery rounds), and every generated version of it
// guessed differently where its targets go. In the shooting range the guess
// put them on the floor outside their lane. This runs the loop on the
// engine's terms:
//
//   * starts when ``start_signal`` is raised (a builtin.press_button), after
//     an optional countdown, and can be restarted when finished;
//   * spawns clones of its target template(s) at the points of the world's
//     anchor set (``ctx.anchors``), each placed by the set's relation --
//     hanging from a pole, standing on a pad -- from the mesh's measured
//     size, never on an occupied point;
//   * counts a target as hit when it dies (any damage through ctx.combat:
//     bullets, beams, blasts) and replaces it after ``spawn_interval_s``;
//   * ends after ``duration_s``, clears what is left, records the best score;
//   * publishes ``<id>.state`` (ready / countdown / running / finished),
//     ``<id>.time_left``, ``<id>.score``, ``<id>.best`` to ctx.vars, and
//     raises ``<id>.started`` / ``<id>.finished``, so a builtin.world_display
//     shows it and generated code can react.
//
// Params: id (var prefix; default the entity id), start_signal, duration_s
// (20), countdown_s (3), templates (entity ids; default every template tagged
// "challenge_target"), anchor_set, max_active (3), spawn_interval_s (0.6),
// target_health (1: one hit), score_per_hit (1).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "../BuiltinMechanicBase.h"
#include "ptv/core/Logger.h"

namespace ptv {
namespace {

constexpr std::uint32_t kNone = 0xFFFFFFFFu;

bool has_tag(const World& world, std::uint32_t e, const char* tag) {
    if (!world.valid(e) || !world.has<CTag>(e)) return false;
    const auto& tags = world.get<CTag>(e).tags;
    return std::find(tags.begin(), tags.end(), tag) != tags.end();
}

class TargetChallengeMechanic final : public BuiltinMechanicBase<TargetChallengeMechanic> {
public:
    static constexpr const char* kId = "builtin.target_challenge";
    MechanicPhase phase() const override { return MechanicPhase::Simulate; }
    MechanicScope scope() const override { return MechanicScope::Entity; }

    void configure(const nlohmann::json&) {}

    void on_attach(World& world, std::uint32_t self, const nlohmann::json& p,
                   MechanicContext& ctx) override {
        const std::string* eid = world.id_of(static_cast<entt::entity>(self));
        id_ = p.value("id", eid != nullptr ? *eid : std::string("challenge"));
        start_signal_ = p.value("start_signal", std::string());
        duration_ = std::clamp(p.value("duration_s", 20.0f), 1.0f, 3600.0f);
        countdown_ = std::clamp(p.value("countdown_s", 3.0f), 0.0f, 10.0f);
        anchor_set_ = p.value("anchor_set", std::string());
        max_active_ = std::clamp(p.value("max_active", 3), 1, 32);
        interval_ = std::clamp(p.value("spawn_interval_s", 0.6f), 0.0f, 30.0f);
        target_health_ = std::clamp(p.value("target_health", 1.0f), 0.01f, 100000.0f);
        score_per_hit_ = p.value("score_per_hit", 1.0f);
        template_ids_.clear();
        for (const auto& t : p.value("templates", nlohmann::json::array())) {
            if (t.is_string()) template_ids_.push_back(t.get<std::string>());
        }
        resolved_ = false;
        if (ctx.signals != nullptr && !start_signal_.empty()) {
            seen_start_ = ctx.signals->count(start_signal_.c_str());
        }
        rng_.seed(static_cast<unsigned>(std::hash<std::string>{}(id_)));
        publish(ctx, "ready");
    }

    void tick(World& world, std::uint32_t, float dt, const InputSnapshot&, MechanicContext& ctx) {
        if (!resolved_) resolve(world);
        const bool start = ctx.signals != nullptr && !start_signal_.empty() &&
                           ctx.signals->fired_since(start_signal_.c_str(), seen_start_);
        switch (state_) {
            case State::Ready:
            case State::Finished:
                if (start) begin(ctx);
                break;
            case State::Countdown:
                timer_ -= dt;
                set(ctx, "time_left", duration_);
                set(ctx, "countdown", std::max(0.0f, std::ceil(timer_)));
                if (timer_ <= 0.0f) {
                    state_ = State::Running;
                    timer_ = duration_;
                    spawn_timer_ = 0.0f;
                    publish(ctx, "running");
                    if (ctx.signals != nullptr) ctx.signals->emit((id_ + ".started").c_str());
                    PTV_INFO("challenge: '{}' started ({:.0f} s)", id_, duration_);
                }
                break;
            case State::Running:
                run(world, ctx, dt);
                break;
        }
    }

    void on_detach(World&, std::uint32_t) override {}

private:
    enum class State { Ready, Countdown, Running, Finished };
    struct Live {
        std::uint32_t entity{kNone};
        int point{-1};
    };

    void resolve(World& world) {
        resolved_ = true;
        templates_.clear();
        auto view = world.registry().view<CTag>();
        for (auto e : view) {
            const auto id = static_cast<std::uint32_t>(e);
            const std::string* name = world.id_of(e);
            const bool named = name != nullptr &&
                               std::find(template_ids_.begin(), template_ids_.end(), *name) !=
                                   template_ids_.end();
            if (named || (template_ids_.empty() && has_tag(world, id, "challenge_target") &&
                          has_tag(world, id, "template"))) {
                templates_.push_back(id);
            }
        }
        if (templates_.empty()) {
            PTV_WARN("challenge: '{}' has no target template (templates {} / tag "
                     "'challenge_target')", id_, template_ids_.size());
        }
    }

    void begin(MechanicContext& ctx) {
        clear_targets(ctx);
        score_ = 0.0f;
        set(ctx, "score", 0.0f);
        set(ctx, "time_left", duration_);
        if (countdown_ > 0.0f) {
            state_ = State::Countdown;
            timer_ = countdown_;
            publish(ctx, "countdown");
        } else {
            state_ = State::Running;
            timer_ = duration_;
            spawn_timer_ = 0.0f;
            publish(ctx, "running");
            if (ctx.signals != nullptr) ctx.signals->emit((id_ + ".started").c_str());
        }
        PTV_INFO("challenge: '{}' begins", id_);
    }

    void run(World& world, MechanicContext& ctx, float dt) {
        timer_ -= dt;
        set(ctx, "time_left", std::max(0.0f, timer_));
        // Hits: a live target that died (or was removed by its death).
        for (auto it = live_.begin(); it != live_.end();) {
            const std::uint32_t e = it->entity;
            const bool gone = !world.valid(e) ||
                              (world.has<CHealth>(e) && !world.get<CHealth>(e).alive);
            if (!gone) {
                ++it;
                continue;
            }
            score_ += score_per_hit_;
            set(ctx, "score", score_);
            if (ctx.spawn != nullptr && world.valid(e)) dying_.push_back({e, 0.6f});
            it = live_.erase(it);
            spawn_timer_ = std::max(spawn_timer_, interval_);
        }
        for (auto it = dying_.begin(); it != dying_.end();) {
            it->second -= dt;
            if (it->second <= 0.0f) {
                if (ctx.spawn != nullptr) ctx.spawn->despawn(it->first);
                it = dying_.erase(it);
            } else {
                ++it;
            }
        }
        if (timer_ <= 0.0f) {
            finish(ctx);
            return;
        }
        spawn_timer_ = std::max(0.0f, spawn_timer_ - dt);
        if (spawn_timer_ <= 0.0f && static_cast<int>(live_.size()) < max_active_) {
            spawn(world, ctx);
            spawn_timer_ = interval_;
        }
    }

    void spawn(World& world, MechanicContext& ctx) {
        if (ctx.spawn == nullptr || templates_.empty()) return;
        const int n = ctx.anchors != nullptr ? ctx.anchors->count(anchor_set_.c_str()) : 0;
        std::vector<int> free_points;
        for (int i = 0; i < n; ++i) {
            const bool used = std::any_of(live_.begin(), live_.end(),
                                          [i](const Live& l) { return l.point == i; });
            if (!used) free_points.push_back(i);
        }
        if (n > 0 && free_points.empty()) return;
        const std::uint32_t tmpl =
            templates_[std::uniform_int_distribution<std::size_t>(0, templates_.size() - 1)(rng_)];
        Vec3 pos(0.0f);
        Quat rot(1.0f, 0.0f, 0.0f, 0.0f);
        int point = -1;
        if (n > 0) {
            point = free_points[std::uniform_int_distribution<std::size_t>(
                0, free_points.size() - 1)(rng_)];
            ctx.anchors->pose_at(tmpl, anchor_set_.c_str(), point, &pos, &rot);
        } else {
            PTV_WARN("challenge: '{}' has no anchor set '{}'; spawning at the template",
                     id_, anchor_set_);
            if (world.has<CTransform>(tmpl)) pos = world.get<CTransform>(tmpl).t.position;
        }
        const std::uint32_t e = ctx.spawn->instance(tmpl, pos, rot);
        if (e == kNone) return;
        // One hit (by default) and it counts: whatever the template authored.
        auto& hp = world.registry().get_or_emplace<CHealth>(static_cast<entt::entity>(e));
        hp.max = hp.current = target_health_;
        hp.alive = true;
        hp.invuln_s = 0.0f;
        live_.push_back({e, point});
    }

    void finish(MechanicContext& ctx) {
        clear_targets(ctx);
        state_ = State::Finished;
        best_ = std::max(best_, score_);
        set(ctx, "time_left", 0.0f);
        set(ctx, "best", best_);
        publish(ctx, "finished");
        if (ctx.signals != nullptr) ctx.signals->emit((id_ + ".finished").c_str());
        PTV_INFO("challenge: '{}' finished: score {:.0f} best {:.0f}", id_, score_, best_);
    }

    void clear_targets(MechanicContext& ctx) {
        if (ctx.spawn != nullptr) {
            for (const auto& l : live_) ctx.spawn->despawn(l.entity);
            for (const auto& d : dying_) ctx.spawn->despawn(d.first);
        }
        live_.clear();
        dying_.clear();
    }

    void set(MechanicContext& ctx, const char* key, float v) {
        if (ctx.vars != nullptr) ctx.vars->set_number((id_ + "." + key).c_str(), v);
    }

    void publish(MechanicContext& ctx, const char* state) {
        if (ctx.vars != nullptr) {
            ctx.vars->set_text((id_ + ".state").c_str(), state);
            ctx.vars->set_number((id_ + ".best").c_str(), best_);
        }
    }

    std::string id_;
    std::string start_signal_;
    std::string anchor_set_;
    std::vector<std::string> template_ids_;
    std::vector<std::uint32_t> templates_;
    std::vector<Live> live_;
    std::vector<std::pair<std::uint32_t, float>> dying_;
    std::mt19937 rng_{1};
    State state_{State::Ready};
    std::uint32_t seen_start_{0};
    float duration_{20.0f};
    float countdown_{3.0f};
    float timer_{0.0f};
    float spawn_timer_{0.0f};
    float interval_{0.6f};
    float target_health_{1.0f};
    float score_per_hit_{1.0f};
    float score_{0.0f};
    float best_{0.0f};
    int max_active_{3};
    bool resolved_{false};
};

}  // namespace

extern "C" int ptv_force_link_builtin_target_challenge = 1;

PTV_REGISTER_MECHANIC("builtin.target_challenge", TargetChallengeMechanic)

}  // namespace ptv
