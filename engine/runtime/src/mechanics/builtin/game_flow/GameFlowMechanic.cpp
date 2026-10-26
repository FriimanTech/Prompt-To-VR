// ---------------------------------------------------------------------------
// builtin.game_flow -- the game loop, run as data.
//
// The project's GameDesign (game_design.json, compiled into this binding's
// params by scene_compile) says what can kill the player, how many lives
// they have, where they come back, what they are trying to do, how the game
// ends and what each end-screen button does. This mechanic runs exactly that:
//
//   Playing -> (health 0) LifeLost -> Respawning -> Playing
//                                  \-> GameOver (no lives left)
//   Playing -> (objectives met) Victory | Completed
//   Playing <-> Paused (left menu button)
//   GameOver / Victory / Completed -> Try Again / Play Again (fresh reload)
//                                   -> Leave Game (the app exits)
//
// It exists because every generated game used to invent its own run flow,
// and the humans-vs-aliens APK showed what that produced: health reached
// zero, the director paused, and the player stood in a frozen arena with no
// screen and no way out. Generated code now reports only game-specific
// progress through ``ctx.game`` and gates its input on gameplay_active().
//
// VR comfort: every relocation of the player happens under a fade to black,
// end screens are world-locked panels at a comfortable distance (Meta's
// "Head -> Comfort" placement, via builtin.ui_panel), and the player is
// invulnerable and the sticks locked while a screen is up.
//
// Every state change is logged as a ``flow:`` line; quest_runtime_verify
// reads them. In ``debug.ptv.verify_flow`` runs the mechanic also scripts its
// own clicks so one unattended run walks death, Game Over, Try Again,
// victory, Play Again and Leave Game.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../BuiltinMechanicBase.h"
#include "ptv/api/Facing.h"
#include "ptv/core/Logger.h"

namespace ptv {
namespace {

constexpr const char* kLockOwner = "builtin.game_flow";
constexpr float kFadeS = 0.6f;

struct ObjectiveDef {
    std::string id;
    std::string kind;
    std::string description;
    float duration_s{0.0f};
    int count{0};
    std::vector<std::string> targets;
    float radius_m{2.0f};
};

struct ScreenDef {
    bool present{false};
    std::string title;
    std::string subtitle;
    std::vector<std::string> buttons;
    bool show_stats{true};
};

const char* button_label(const std::string& id) {
    if (id == "try_again") return "Try Again";
    if (id == "play_again") return "Play Again";
    if (id == "keep_playing") return "Keep Playing";
    if (id == "leave_game") return "Leave Game";
    if (id == "resume") return "Resume";
    if (id == "restart") return "Restart";
    return id.c_str();
}

std::vector<std::string> strings(const nlohmann::json& j, const char* key) {
    std::vector<std::string> out;
    if (j.contains(key) && j[key].is_array()) {
        for (const auto& v : j[key]) {
            if (v.is_string()) out.push_back(v.get<std::string>());
        }
    }
    return out;
}

std::string clock_text(float s) {
    const int total = std::max(0, static_cast<int>(std::floor(s)));
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d:%02d", total / 60, total % 60);
    return buf;
}

class GameFlowMechanic final : public BuiltinMechanicBase<GameFlowMechanic> {
public:
    static constexpr const char* kId = "builtin.game_flow";

    // Simulate: after combat has applied this frame's hits and before the
    // pose system, so a death is noticed on the frame it happens.
    MechanicPhase phase() const override { return MechanicPhase::Simulate; }

    void configure(const nlohmann::json& p) {
        panel_id_ = p.value("panel_id", std::string("ptv_game_flow"));
        has_health_ = p.value("has_health", false);
        invulnerable_ = p.value("invulnerable", false);
        max_health_ = std::clamp(p.value("max_health", 100.0f), 1.0f, 100000.0f);
        regen_ = std::max(0.0f, p.value("regen_per_s", 0.0f));
        lives_max_ = p.value("lives", -1);
        free_play_ = p.value("free_play", false);
        const auto& r = p.value("respawn", nlohmann::json::object());
        respawn_at_ = r.value("at", std::string("spawn"));
        respawn_delay_ = std::clamp(r.value("delay_s", 2.0f), 0.5f, 30.0f);
        restore_health_ = r.value("restore_health", true);
        respawn_invuln_ = std::clamp(r.value("invulnerable_s", 3.0f), 0.0f, 30.0f);
        enemy_policy_ = r.value("enemy_policy", std::string("clear_nearby"));
        clear_radius_ = std::clamp(r.value("clear_radius_m", 6.0f), 0.0f, 200.0f);
        if (p.contains("spawn") && p["spawn"].is_object()) {
            const auto& s = p["spawn"];
            const auto pos = s.value("position", std::vector<float>{});
            if (pos.size() == 3) {
                spawn_pos_ = Vec3(pos[0], pos[1], pos[2]);
                has_spawn_param_ = true;
            }
            spawn_yaw_ = glm::radians(s.value("yaw_deg", 0.0f));
        }
        if (p.contains("markers") && p["markers"].is_object()) {
            for (auto it = p["markers"].begin(); it != p["markers"].end(); ++it) {
                const auto v = it.value().get<std::vector<float>>();
                if (v.size() == 3) markers_[it.key()] = Vec3(v[0], v[1], v[2]);
            }
        }
        objectives_.clear();
        for (const auto& o : p.value("objectives", nlohmann::json::array())) {
            if (!o.is_object()) continue;
            ObjectiveDef d{};
            d.id = o.value("id", "");
            d.kind = o.value("kind", "custom");
            d.description = o.value("description", "");
            d.duration_s = o.value("duration_s", 0.0f);
            d.count = o.value("count", 0);
            d.targets = strings(o, "target_ids");
            d.radius_m = o.value("radius_m", 2.0f);
            if (!(d.radius_m > 0.0f)) d.radius_m = 2.0f;
            if (!d.id.empty()) objectives_.push_back(std::move(d));
        }
        const auto& v = p.value("victory", nlohmann::json::object());
        victory_kind_ = v.value("kind", std::string("none"));
        victory_ids_ = strings(v, "objective_ids");
        failure_kinds_.clear();
        for (const auto& f : p.value("failure", nlohmann::json::array())) {
            if (f.is_object()) failure_kinds_.push_back(f.value("kind", ""));
        }
        for (const char* kind : {"game_over", "victory", "completed"}) {
            ScreenDef& s = screens_[kind];
            s = ScreenDef{};
            const auto& sj = p.value("screens", nlohmann::json::object());
            if (!sj.contains(kind) || !sj[kind].is_object()) continue;
            s.present = true;
            s.title = sj[kind].value("title", std::string(kind));
            s.subtitle = sj[kind].value("subtitle", "");
            s.buttons = strings(sj[kind], "buttons");
            s.show_stats = sj[kind].value("show_stats", true);
        }
        const auto& pause = p.value("pause", nlohmann::json::object());
        pause_enabled_ = pause.value("enabled", true);
        pause_buttons_ = strings(pause, "buttons");
        if (pause_buttons_.empty()) pause_buttons_ = {"resume", "restart", "leave_game"};
        fresh_restart_ = p.value("reset", nlohmann::json::object()).value("world_state", true);
        experience_ = p.value("experience", std::string("combat"));
    }

    void tick(World& world, std::uint32_t self, float dt, const InputSnapshot& input,
              MechanicContext& ctx) {
        if (dt <= 0.0f || ctx.game == nullptr) return;
        GameFlowShared* s = ctx.game->flow_state();
        if (s == nullptr) return;
        if (!started_) start(world, *s, ctx);
        (void)self;

        player_ = ctx.hands != nullptr ? ctx.hands->player_entity() : player_;
        state_t_ += dt;

        switch (s->state) {
            case GameFlowState::Playing:
            case GameFlowState::Respawning:
                tick_playing(world, *s, dt, input, ctx);
                break;
            case GameFlowState::LifeLost:
                tick_life_lost(world, *s, ctx);
                break;
            case GameFlowState::Paused:
            case GameFlowState::GameOver:
            case GameFlowState::Victory:
            case GameFlowState::Completed:
                tick_screen(world, *s, input, ctx);
                break;
            case GameFlowState::Exiting:
                if (ctx.app != nullptr) ctx.app->request_fade(1.0f);
                break;
            default:
                break;
        }
        draw_toast(*s, input, ctx);
    }

    void on_detach(World&, std::uint32_t) override {}

private:
    // ------------------------------------------------------------------
    void start(World& world, GameFlowShared& s, MechanicContext& ctx) {
        started_ = true;
        verify_ = ctx.app != nullptr && ctx.app->verify_flow();
        s.configured = true;
        s.lives_max = lives_max_;
        s.lives = lives_max_;
        s.elapsed_s = 0.0f;
        s.score = 0;
        s.defeated = 0;
        if (s.attempts <= 0) s.attempts = 1;
        s.time_goal_s = 0.0f;
        s.objectives.clear();
        for (const auto& d : objectives_) {
            GameObjectiveState o{};
            o.id = d.id;
            o.kind = d.kind;
            o.description = d.description;
            o.target = d.kind == "survive_time" ? std::max(1.0f, d.duration_s)
                       : d.count > 0           ? static_cast<float>(d.count)
                                               : 1.0f;
            if (d.kind == "survive_time") s.time_goal_s = std::max(s.time_goal_s, o.target);
            s.objectives.push_back(o);
        }
        if (ctx.combat != nullptr) damage_cursor_ = ctx.combat->damage_seq(0xFFFFFFFFu);
        if (ctx.encounters != nullptr) victory_seen_ = ctx.encounters->victory_seq();
        player_ = ctx.hands != nullptr ? ctx.hands->player_entity() : 0xFFFFFFFFu;
        if (has_health_ && world.valid(player_)) {
            auto& hp = world.registry().get_or_emplace<CHealth>(
                static_cast<entt::entity>(player_));
            hp.max = max_health_;
            hp.current = max_health_;
            hp.alive = true;
            hp.death_timer_s = -1.0f;
        }
        if (!has_spawn_param_ && ctx.app != nullptr) {
            spawn_pos_ = ctx.app->scene_spawn_position();
            spawn_yaw_ = ctx.app->scene_spawn_yaw();
        }
        last_safe_ = spawn_pos_;
        enter(s, GameFlowState::Playing, "start", ctx);
    }

    void enter(GameFlowShared& s, GameFlowState next, const std::string& why,
               MechanicContext& ctx) {
        const GameFlowState prev = s.state;
        s.state = next;
        ++s.state_seq;
        state_t_ = 0.0f;
        verify_clicked_ = false;
        PTV_INFO(
            "flow: state={} from={} why={} attempt={} lives={} elapsed={:.1f} "
            "score={} defeated={}",
            game_flow_state_name(next), game_flow_state_name(prev), why, s.attempts,
            s.lives, s.elapsed_s, s.score, s.defeated);
        const bool screen = next == GameFlowState::Paused ||
                            next == GameFlowState::GameOver ||
                            next == GameFlowState::Victory ||
                            next == GameFlowState::Completed;
        if (ctx.locomotion != nullptr) ctx.locomotion->set_input_lock(kLockOwner, screen);
        if (ctx.encounters != nullptr && ctx.encounters->configured()) {
            ctx.encounters->pause(screen || next == GameFlowState::LifeLost);
        }
        if (ctx.ui != nullptr) {
            if (screen) {
                open_screen(s, next, ctx);
            } else {
                ctx.ui->close(panel_id_.c_str());
            }
        }
    }

    // ------------------------------------------------------------------
    void tick_playing(World& world, GameFlowShared& s, float dt,
                      const InputSnapshot& input, MechanicContext& ctx) {
        if (s.state == GameFlowState::Respawning) {
            if (ctx.app != nullptr) {
                ctx.app->request_fade(std::max(0.0f, 1.0f - state_t_ / kFadeS));
            }
            if (state_t_ >= kFadeS) enter(s, GameFlowState::Playing, "respawned", ctx);
        }

        // Verification attempt 2 must reach the ending, so the player is kept
        // alive and time runs faster.
        const bool verify_win_run = verify_ && verify_phase(s) == 2;
        const float time_scale = verify_win_run ? 10.0f : 1.0f;
        s.elapsed_s += dt * time_scale;

        if (has_health_ && world.valid(player_) && world.has<CHealth>(player_)) {
            auto& hp = world.get<CHealth>(player_);
            if (invulnerable_ || verify_win_run) hp.invuln_s = std::max(hp.invuln_s, 0.5f);
            if (regen_ > 0.0f && hp.alive && ctx.combat != nullptr) {
                ctx.combat->heal(player_, regen_ * dt);
            }
        }
        remember_safe_spot(world, ctx);
        count_kills(world, s, ctx);
        update_objectives(world, s, ctx);

        if (pause_enabled_ && input.menu_pressed()) {
            enter(s, GameFlowState::Paused, "menu_button", ctx);
            return;
        }

        // Death.
        if (has_health_ && !invulnerable_ && world.valid(player_) &&
            ctx.combat != nullptr && !ctx.combat->is_alive(player_)) {
            if (s.lives > 0) --s.lives;
            enter(s, GameFlowState::LifeLost, "health_zero", ctx);
            return;
        }
        // Verification attempt 1: the player has to die, whatever the
        // enemies manage; the death PATH is what is being checked.
        if (verify_ && verify_phase(s) == 1 && can_die() && state_t_ > 12.0f &&
            s.state == GameFlowState::Playing && ctx.combat != nullptr &&
            world.valid(player_)) {
            PTV_INFO("flow: verify kills the player (attempt {})", s.attempts);
            ctx.combat->kill(player_);
            return;
        }
        if (verify_win_run && state_t_ > 15.0f) force_objectives(s);

        // Endings.
        if (s.fail_requested) {
            s.fail_requested = false;
            PTV_INFO("flow: failed by game code: {}", s.fail_reason);
            finish(s, false, "fail_requested", ctx);
            return;
        }
        if (timer_failure(s)) {
            finish(s, false, "timer_expired", ctx);
            return;
        }
        if (victory_met(s)) {
            finish(s, true, "objectives_met", ctx);
            return;
        }
        // Verification's last pass: leave the game. Also reached by a game
        // with no ending (free play, or endless with nothing to win).
        const int phase = verify_ ? verify_phase(s) : 0;
        const bool leave_pass =
            verify_ && (phase >= 3 || free_play_ || (phase == 2 && victory_kind_ == "none"));
        if (leave_pass && state_t_ > 10.0f && s.state == GameFlowState::Playing) {
            if (pause_enabled_) {
                enter(s, GameFlowState::Paused, "verify_pause", ctx);
            } else {
                PTV_INFO("flow: verify leaves (no pause menu)");
                act(s, "leave_game", ctx);
            }
        }
    }

    void tick_life_lost(World& world, GameFlowShared& s, MechanicContext& ctx) {
        if (ctx.app != nullptr) ctx.app->request_fade(std::min(1.0f, state_t_ / kFadeS));
        if (s.lives == 0) {
            if (state_t_ >= kFadeS + 0.4f) {
                if (ctx.app != nullptr) ctx.app->request_fade(0.0f);
                finish(s, false, "lives_exhausted", ctx);
            }
            return;
        }
        if (state_t_ < respawn_delay_) return;
        respawn(world, s, ctx);
        enter(s, GameFlowState::Respawning, "respawn", ctx);
    }

    void respawn(World& world, GameFlowShared& s, MechanicContext& ctx) {
        Vec3 where = spawn_pos_;
        float yaw = spawn_yaw_;
        if (respawn_at_ == "last_safe") {
            where = last_safe_;
        } else if (respawn_at_ == "checkpoint" && !checkpoint_.empty()) {
            where = markers_[checkpoint_];
        }
        if (ctx.app != nullptr) ctx.app->place_player(where, yaw);
        if (ctx.combat != nullptr && world.valid(player_)) {
            const float hp = restore_health_ ? 0.0f : 0.25f * max_health_;
            ctx.combat->revive(player_, hp, respawn_invuln_);
        }
        apply_enemy_policy(world, where, ctx);
        PTV_INFO("flow: respawned at {:.2f},{:.2f},{:.2f} ({}) lives={}", where.x, where.y,
                 where.z, respawn_at_, s.lives);
    }

    void apply_enemy_policy(World& world, const Vec3& where, MechanicContext& ctx) {
        if (enemy_policy_ == "reset_wave" || enemy_policy_ == "reset_all") {
            if (ctx.encounters != nullptr && ctx.encounters->configured()) {
                ctx.encounters->reset();
                ctx.encounters->start();
            }
            return;
        }
        if (enemy_policy_ != "clear_nearby" || ctx.combat == nullptr) return;
        // Nothing hostile left standing on the spawn: being killed again the
        // moment the fade lifts is the worst respawn there is.
        std::vector<std::uint32_t> doomed;
        auto view = world.registry().view<CHealth, CTransform>();
        for (auto e : view) {
            const auto id = static_cast<std::uint32_t>(e);
            if (id == player_ || !ctx.combat->is_alive(id)) continue;
            if (!ctx.combat->is_hostile(id, player_)) continue;
            const Vec3 d = view.get<CTransform>(e).t.position - where;
            if (glm::length(Vec3(d.x, 0.0f, d.z)) <= clear_radius_) doomed.push_back(id);
        }
        for (auto id : doomed) {
            if (ctx.spawn != nullptr) {
                ctx.spawn->despawn(id);
            } else {
                ctx.combat->kill(id);
            }
        }
    }

    // ------------------------------------------------------------------
    void open_screen(GameFlowShared& s, GameFlowState state, MechanicContext& ctx) {
        std::string title;
        std::string subtitle;
        std::vector<std::string> buttons;
        if (state == GameFlowState::Paused) {
            title = "Paused";
            buttons = pause_buttons_;
        } else {
            const char* kind = state == GameFlowState::GameOver ? "game_over"
                               : state == GameFlowState::Victory ? "victory"
                                                                 : "completed";
            ScreenDef& sd = screens_[kind];
            title = sd.present ? sd.title
                               : (state == GameFlowState::GameOver ? "Game Over"
                                  : state == GameFlowState::Victory ? "You Win!"
                                                                    : "The End");
            subtitle = sd.subtitle;
            buttons = sd.buttons;
            if (buttons.empty()) {
                buttons = {state == GameFlowState::GameOver ? "try_again" : "play_again",
                           "leave_game"};
            }
            if (!sd.present || sd.show_stats) {
                std::string stats;
                if (s.time_goal_s > 0.0f || experience_ == "combat") {
                    stats = "Time " + clock_text(s.elapsed_s);
                }
                if (s.defeated > 0) {
                    stats += (stats.empty() ? "" : "   ") + std::string("Defeated ") +
                             std::to_string(s.defeated);
                }
                if (s.score > 0) {
                    stats += (stats.empty() ? "" : "   ") + std::string("Score ") +
                             std::to_string(s.score);
                }
                if (!stats.empty()) subtitle = subtitle.empty() ? stats : subtitle + "\n" + stats;
            }
        }
        screen_buttons_ = buttons;
        std::vector<UiRowDesc> rows;
        for (const auto& b : screen_buttons_) {
            UiRowDesc r{};
            r.id = b.c_str();
            r.label = button_label(b);
            rows.push_back(r);
        }
        ctx.ui->set_title(panel_id_.c_str(), title.c_str(), subtitle.c_str());
        ctx.ui->set_rows(panel_id_.c_str(), rows.data(), static_cast<int>(rows.size()));
        ctx.ui->open(panel_id_.c_str());
        if (!ctx.ui->panel_exists(panel_id_.c_str())) {
            PTV_WARN("flow: no ui panel '{}' in the scene; the {} screen cannot be shown",
                     panel_id_, game_flow_state_name(state));
        }
    }

    void tick_screen(World& world, GameFlowShared& s, const InputSnapshot& input,
                     MechanicContext& ctx) {
        // Nothing can hurt the player while a screen is up.
        if (world.valid(player_) && world.has<CHealth>(player_)) {
            world.get<CHealth>(player_).invuln_s =
                std::max(world.get<CHealth>(player_).invuln_s, 0.5f);
        }
        if (s.state == GameFlowState::Paused && input.menu_pressed()) {
            enter(s, GameFlowState::Playing, "resume_menu_button", ctx);
            return;
        }
        std::string chosen;
        if (ctx.ui != nullptr) {
            UiClick click{};
            while (ctx.ui->poll_click(panel_id_.c_str(), &click)) {
                if (click.row_id != nullptr) chosen = click.row_id;
            }
        }
        if (chosen.empty() && verify_ && state_t_ > 3.0f && !verify_clicked_) {
            chosen = verify_choice(s);
            verify_clicked_ = true;
            if (!chosen.empty()) PTV_INFO("flow: verify clicks {}", chosen);
        }
        if (!chosen.empty()) act(s, chosen, ctx);
    }

    void act(GameFlowShared& s, const std::string& button, MechanicContext& ctx) {
        PTV_INFO("flow: button={} state={}", button, game_flow_state_name(s.state));
        if (button == "resume" || button == "keep_playing") {
            enter(s, GameFlowState::Playing, button, ctx);
        } else if (button == "try_again" || button == "play_again" || button == "restart") {
            if (ctx.ui != nullptr) ctx.ui->close(panel_id_.c_str());
            if (ctx.app != nullptr) ctx.app->request_fade(1.0f);
            if (ctx.transition == nullptr || !ctx.transition->restart_current(fresh_restart_)) {
                PTV_WARN("flow: restart unavailable; resetting the flow in place");
                started_ = false;
            }
        } else if (button == "leave_game") {
            enter(s, GameFlowState::Exiting, "leave_game", ctx);
            if (ctx.ui != nullptr) ctx.ui->close(panel_id_.c_str());
            if (ctx.app != nullptr) ctx.app->request_exit();
        }
    }

    // ------------------------------------------------------------------
    void count_kills(World& world, GameFlowShared& s, MechanicContext& ctx) {
        if (ctx.combat == nullptr) return;
        DamageEvent ev[32];
        const int n = ctx.combat->damage_events_since(0xFFFFFFFFu, damage_cursor_, ev, 32);
        for (int i = 0; i < n; ++i) {
            damage_cursor_ = std::max(damage_cursor_, ev[i].seq);
            if (!ev[i].killed || ev[i].target == player_) continue;
            if (player_ != 0xFFFFFFFFu && !ctx.combat->is_hostile(ev[i].target, player_)) continue;
            ++s.defeated;
            std::string asset;
            if (world.valid(ev[i].target) && world.has<CMesh>(ev[i].target)) {
                asset = world.get<CMesh>(ev[i].target).asset_id;
            }
            for (auto& o : s.objectives) {
                const ObjectiveDef* d = def(o.id);
                if (d == nullptr || d->kind != "defeat_count") continue;
                if (!d->targets.empty() &&
                    std::find(d->targets.begin(), d->targets.end(), asset) == d->targets.end()) {
                    continue;
                }
                o.progress += 1.0f;
            }
        }
    }

    void update_objectives(World& world, GameFlowShared& s, MechanicContext& ctx) {
        const Vec3 me = player_position(world, ctx);
        for (auto& o : s.objectives) {
            if (o.complete) continue;
            const ObjectiveDef* d = def(o.id);
            if (d == nullptr) continue;
            if (d->kind == "survive_time") {
                o.progress = s.elapsed_s;
            } else if (d->kind == "clear_waves") {
                if (ctx.encounters != nullptr && ctx.encounters->victory_seq() > victory_seen_) {
                    o.progress = o.target;
                }
            } else if (d->kind == "reach_location") {
                for (const auto& t : d->targets) {
                    const auto it = markers_.find(t);
                    if (it == markers_.end()) continue;
                    const Vec3 dv = it->second - me;
                    if (std::sqrt(dv.x * dv.x + dv.z * dv.z) <= d->radius_m) {
                        o.progress = o.target;
                        checkpoint_ = t;
                    }
                }
            } else if (d->kind == "collect" && ctx.hands != nullptr) {
                for (int h = 0; h < 2; ++h) {
                    const std::uint32_t held = ctx.hands->held_entity(h);
                    if (!world.valid(held) || !world.has<CMesh>(held)) continue;
                    const std::string& asset = world.get<CMesh>(held).asset_id;
                    if (std::find(d->targets.begin(), d->targets.end(), asset) == d->targets.end())
                        continue;
                    if (collected_[o.id].insert(held).second) o.progress += 1.0f;
                }
            }
            if (o.progress >= o.target - 1e-4f) {
                o.complete = true;
                PTV_INFO("flow: objective={} complete ({:.0f}/{:.0f})", o.id, o.progress,
                         o.target);
            }
        }
    }

    bool victory_met(const GameFlowShared& s) const {
        if (victory_kind_ == "none" || s.objectives.empty()) return false;
        auto complete = [&](const std::string& id) {
            for (const auto& o : s.objectives) {
                if (o.id == id) return o.complete;
            }
            return false;
        };
        std::vector<std::string> ids = victory_ids_;
        if (ids.empty()) {
            for (const auto& o : s.objectives) ids.push_back(o.id);
        }
        if (victory_kind_ == "any_objective") {
            return std::any_of(ids.begin(), ids.end(), complete);
        }
        return std::all_of(ids.begin(), ids.end(), complete);
    }

    bool timer_failure(const GameFlowShared& s) const {
        if (std::find(failure_kinds_.begin(), failure_kinds_.end(), "timer_expired") ==
            failure_kinds_.end()) {
            return false;
        }
        // The clock ran out before the (non-timer) objectives were done.
        return s.time_goal_s > 0.0f && s.elapsed_s >= s.time_goal_s && !victory_met(s);
    }

    void finish(GameFlowShared& s, bool won, const std::string& why, MechanicContext& ctx) {
        GameFlowState next = GameFlowState::GameOver;
        if (won) {
            const bool challenge = experience_ == "combat" || experience_ == "score_attack" ||
                                   experience_ == "puzzle";
            next = (challenge || !screens_["completed"].present) && screens_["victory"].present
                       ? GameFlowState::Victory
                   : screens_["completed"].present ? GameFlowState::Completed
                                                   : GameFlowState::Victory;
        }
        enter(s, next, why, ctx);
    }

    void force_objectives(GameFlowShared& s) {
        for (auto& o : s.objectives) {
            const ObjectiveDef* d = def(o.id);
            if (o.complete || d == nullptr || d->kind == "survive_time") continue;
            o.progress = o.target;
            PTV_INFO("flow: verify forces objective={} (kind {})", o.id, d->kind);
        }
    }

    // ------------------------------------------------------------------
    /** Which scripted pass this attempt is in a verification run:
     *  1 = die until Game Over, 2 = win, 3 = leave. A game that cannot die
     *  starts at the win pass. */
    int verify_phase(const GameFlowShared& s) const {
        const int offset = can_die() ? 0 : 1;
        return std::min(3, s.attempts + offset);
    }

    std::string verify_choice(const GameFlowShared& s) const {
        auto has = [&](const char* b) {
            return std::find(screen_buttons_.begin(), screen_buttons_.end(), b) !=
                   screen_buttons_.end();
        };
        if (verify_phase(s) >= 3 || s.state == GameFlowState::Paused) {
            if (has("leave_game")) return "leave_game";
        }
        for (const char* b : {"try_again", "play_again", "restart", "leave_game"}) {
            if (has(b)) return b;
        }
        return {};
    }

    bool can_die() const { return has_health_ && !invulnerable_ && lives_max_ != 0; }

    const ObjectiveDef* def(const std::string& id) const {
        for (const auto& d : objectives_) {
            if (d.id == id) return &d;
        }
        return nullptr;
    }

    Vec3 player_position(World& world, MechanicContext& ctx) const {
        if (ctx.locomotion != nullptr) return ctx.locomotion->body_position();
        if (world.valid(player_) && world.has<CTransform>(player_)) {
            return world.get<CTransform>(player_).t.position;
        }
        return Vec3(0.0f);
    }

    void remember_safe_spot(World& world, MechanicContext& ctx) {
        if (respawn_at_ != "last_safe" || ctx.perception == nullptr) return;
        if (++safe_counter_ % 60 != 0) return;
        if (ctx.locomotion != nullptr && !ctx.locomotion->is_grounded()) return;
        if (ctx.perception->nearest_hostile(player_, 8.0f) != 0xFFFFFFFFu) return;
        last_safe_ = player_position(world, ctx);
    }

    /** "Life lost -- 2 left" in front of the player while they are down. */
    void draw_toast(const GameFlowShared& s, const InputSnapshot& input,
                    MechanicContext& ctx) const {
        if (ctx.text == nullptr || !input.head_valid) return;
        if (s.state != GameFlowState::LifeLost || s.lives == 0) return;
        const Vec3 fwd = facing_of(input.head_rotation);
        const Vec3 at = input.head_position + fwd * 1.3f + Vec3(0.0f, -0.05f, 0.0f);
        const Quat face = rotation_facing(fwd);
        std::string msg = "Life lost";
        if (s.lives > 0) {
            msg += "\n" + std::to_string(s.lives) + (s.lives == 1 ? " life left" : " lives left");
        }
        ctx.text->text(msg.c_str(), at, face, 0.05f, Vec4(1.0f, 0.85f, 0.8f, 1.0f));
    }

    // --- Config ---
    std::string panel_id_{"ptv_game_flow"};
    std::string experience_{"combat"};
    bool has_health_{false};
    bool invulnerable_{false};
    float max_health_{100.0f};
    float regen_{0.0f};
    int lives_max_{-1};
    bool free_play_{false};
    std::string respawn_at_{"spawn"};
    float respawn_delay_{2.0f};
    bool restore_health_{true};
    float respawn_invuln_{3.0f};
    std::string enemy_policy_{"clear_nearby"};
    float clear_radius_{6.0f};
    Vec3 spawn_pos_{0.0f};
    float spawn_yaw_{0.0f};
    bool has_spawn_param_{false};
    std::unordered_map<std::string, Vec3> markers_;
    std::vector<ObjectiveDef> objectives_;
    std::string victory_kind_{"none"};
    std::vector<std::string> victory_ids_;
    std::vector<std::string> failure_kinds_;
    std::unordered_map<std::string, ScreenDef> screens_;
    bool pause_enabled_{true};
    std::vector<std::string> pause_buttons_;
    bool fresh_restart_{true};

    // --- Runtime ---
    bool started_{false};
    bool verify_{false};
    bool verify_clicked_{false};
    float state_t_{0.0f};
    std::uint32_t player_{0xFFFFFFFFu};
    std::uint64_t damage_cursor_{0};
    std::uint64_t victory_seen_{0};
    Vec3 last_safe_{0.0f};
    std::string checkpoint_;
    std::vector<std::string> screen_buttons_;
    std::unordered_map<std::string, std::unordered_set<std::uint32_t>> collected_;
    int safe_counter_{0};
};

}  // namespace

PTV_REGISTER_MECHANIC("builtin.game_flow", GameFlowMechanic)

extern "C" int ptv_force_link_builtin_game_flow = 1;

}  // namespace ptv
