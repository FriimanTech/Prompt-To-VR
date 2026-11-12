#pragma once

// The game loop, as an engine service: ``ctx.game`` and ``ctx.app``.
//
// ``builtin.game_flow`` runs the project's GameDesign (game_design.json ->
// scene.json) as data: it notices the player's health reaching zero, spends
// lives, respawns, checks objectives, shows the end screens and acts on their
// buttons. Generated code never re-implements any of that -- every game that
// did invented it differently, and one of them left the player standing in a
// frozen arena at zero health with no screen and no way out. What generated
// code DOES do is report game-specific progress (``report_progress``,
// ``complete``, ``fail``) and gate its own input on ``gameplay_active()`` so a
// weapon cannot fire through the Game Over screen.
//
// A scene with no ``builtin.game_flow`` binding leaves ``configured()`` false;
// ``gameplay_active()`` is then always true and nothing else does anything.

#include <cstdint>
#include <string>
#include <vector>

#include "ptv/math/Math.h"

namespace ptv {

enum class GameFlowState : int {
    /** No game flow in this scene. */
    Inactive = 0,
    Playing = 1,
    Paused = 2,
    /** Health just reached zero; fading out. */
    LifeLost = 3,
    /** Back at the respawn point, fading in, briefly invulnerable. */
    Respawning = 4,
    GameOver = 5,
    Victory = 6,
    /** The non-combat ending (the book is finished). */
    Completed = 7,
    /** Leave Game was chosen; the app is shutting down. */
    Exiting = 8,
};

inline const char* game_flow_state_name(GameFlowState s) {
    switch (s) {
        case GameFlowState::Playing: return "playing";
        case GameFlowState::Paused: return "paused";
        case GameFlowState::LifeLost: return "life_lost";
        case GameFlowState::Respawning: return "respawning";
        case GameFlowState::GameOver: return "game_over";
        case GameFlowState::Victory: return "victory";
        case GameFlowState::Completed: return "completed";
        case GameFlowState::Exiting: return "exiting";
        case GameFlowState::Inactive:
        default: return "inactive";
    }
}

/** One objective as the flow tracks it. */
struct GameObjectiveState {
    std::string id;
    std::string kind;
    std::string description;
    float progress{0.0f};
    float target{1.0f};
    bool complete{false};
};

/** Shared state between ``builtin.game_flow`` (the writer) and ``ctx.game``
 *  (everyone else). Engine-internal: generated code reads it through
 *  ``IGameFlow``. */
struct GameFlowShared {
    bool configured{false};
    GameFlowState state{GameFlowState::Inactive};
    /** -1 = unlimited / the game has no lives. */
    int lives{-1};
    int lives_max{-1};
    float elapsed_s{0.0f};
    /** Seconds the survive_time objective needs, or 0. */
    float time_goal_s{0.0f};
    int score{0};
    int defeated{0};
    int attempts{1};
    std::uint64_t state_seq{0};
    std::vector<GameObjectiveState> objectives;
    // Requests from generated code, applied by the flow on its next tick.
    bool fail_requested{false};
    std::string fail_reason;
};

class IGameFlow {
public:
    virtual ~IGameFlow() = default;

    /** True when this scene runs a game flow at all. */
    virtual bool configured() const = 0;
    virtual GameFlowState state() const = 0;
    /** Changes on every state transition: compare with your last value to
     *  see each one once (start a fanfare on Victory). */
    virtual std::uint64_t state_seq() const = 0;

    /** True while the player is actually playing -- always true without a
     *  game flow. Weapons, abilities and timers of your own check this, so
     *  nothing fires or counts down under a Game Over or pause screen. */
    virtual bool gameplay_active() const = 0;

    /** Lives left, or -1 when the game has none (or unlimited). */
    virtual int lives() const = 0;
    /** Seconds of play this attempt (paused and end-screen time excluded). */
    virtual float elapsed_s() const = 0;
    /** Seconds left on a survive-for objective, or -1 when there is none. */
    virtual float time_remaining_s() const = 0;

    virtual int score() const = 0;
    virtual void add_score(int points) = 0;

    /** Progress on a ``custom`` (or any counted) objective. ``set`` replaces,
     *  ``add`` accumulates; the objective completes when it reaches its
     *  target. Unknown ids are ignored with one warning. */
    virtual void set_progress(const char* objective_id, float value) = 0;
    virtual void add_progress(const char* objective_id, float delta) = 0;
    virtual void complete(const char* objective_id) = 0;
    virtual float progress(const char* objective_id) const = 0;
    virtual bool objective_complete(const char* objective_id) const = 0;

    /** Lose now, for a reason only this game knows (the reactor melted
     *  down). Ends on the game_over screen if the design has one. */
    virtual void fail(const char* reason) = 0;

    /** Engine-internal: the state ``builtin.game_flow`` writes. */
    virtual GameFlowShared* flow_state() { return nullptr; }
};

/** The engine's IGameFlow: a view over one GameFlowShared. */
class GameFlowBoard final : public IGameFlow {
public:
    bool configured() const override { return s_.configured; }
    GameFlowState state() const override { return s_.state; }
    std::uint64_t state_seq() const override { return s_.state_seq; }
    bool gameplay_active() const override {
        return !s_.configured || s_.state == GameFlowState::Playing ||
               s_.state == GameFlowState::Respawning;
    }
    int lives() const override { return s_.lives; }
    float elapsed_s() const override { return s_.elapsed_s; }
    float time_remaining_s() const override {
        if (s_.time_goal_s <= 0.0f) return -1.0f;
        const float left = s_.time_goal_s - s_.elapsed_s;
        return left > 0.0f ? left : 0.0f;
    }
    int score() const override { return s_.score; }
    void add_score(int points) override { s_.score += points; }
    void set_progress(const char* id, float value) override {
        if (auto* o = find(id)) o->progress = value;
    }
    void add_progress(const char* id, float delta) override {
        if (auto* o = find(id)) o->progress += delta;
    }
    void complete(const char* id) override {
        if (auto* o = find(id)) o->progress = o->target;
    }
    float progress(const char* id) const override {
        const auto* o = find(id);
        return o ? o->progress : 0.0f;
    }
    bool objective_complete(const char* id) const override {
        const auto* o = find(id);
        return o != nullptr && o->complete;
    }
    void fail(const char* reason) override {
        s_.fail_requested = true;
        s_.fail_reason = reason ? reason : "";
    }
    GameFlowShared* flow_state() override { return &s_; }
    void clear() { s_ = GameFlowShared{}; }

private:
    GameObjectiveState* find(const char* id) {
        if (id == nullptr) return nullptr;
        for (auto& o : s_.objectives) {
            if (o.id == id) return &o;
        }
        return nullptr;
    }
    const GameObjectiveState* find(const char* id) const {
        return const_cast<GameFlowBoard*>(this)->find(id);
    }

    GameFlowShared s_{};
};

/**
 * The application itself.
 *
 * ``request_exit`` is how Leave Game quits: it asks the OpenXR runtime to end
 * the session (``xrRequestExitSession``), the frame loop stops when the
 * session reaches STOPPING/EXITING, and on Quest the activity is then
 * finished properly (``ANativeActivity_finish``) so the app can be launched
 * again. Returning from the frame loop by any other route leaves a Quest app
 * in a state where it does not relaunch.
 */
class IApp {
public:
    virtual ~IApp() = default;
    virtual void request_exit() = 0;
    virtual bool exit_requested() const = 0;
    /** "quest" on the standalone headset, "pcvr" otherwise. */
    virtual const char* platform() const = 0;
    /** Engine verification runs only (``debug.ptv.verify_flow``): the game
     *  flow scripts its own end-screen clicks so a headset test can walk
     *  every path. Generated code must not read this. */
    virtual bool verify_flow() const { return false; }
    /** Any engine verification run (``debug.ptv.verify*``): the player is a
     *  script that cannot pick things up. Generated code must not read this. */
    virtual bool verify_run() const { return false; }
    /** Seconds since the app started. */
    virtual float uptime_s() const = 0;
    /** Fade the whole view toward black THIS frame (0 clear .. 1 black),
     *  regardless of the player's comfort settings. Respawns and restarts
     *  use it so the world never jumps under the player's eyes. Request it
     *  every frame the fade should hold; the strongest request wins. */
    virtual void request_fade(float amount) { (void)amount; }
    /** Flood the whole view with ``color`` THIS frame (0 clear .. 1 solid):
     *  a flashbang's white-out. Like request_fade, request it every frame it
     *  should hold, decaying it to fade back in; the strongest wins. */
    virtual void request_flash(float amount, const Vec3& color) {
        (void)amount;
        (void)color;
    }
    /** Stand the player at ``feet`` facing ``yaw`` exactly as the scene's
     *  start does -- unlike ``ILocomotion::teleport_to`` this is not capped
     *  to a teleport distance. For respawns; ``builtin.game_flow`` calls it.
     *  Returns false when there is no player yet. */
    virtual bool place_player(const Vec3& feet, float yaw) {
        (void)feet;
        (void)yaw;
        return false;
    }
    /** Where the scene starts the player (its default spawn), and facing. */
    virtual Vec3 scene_spawn_position() const { return Vec3(0.0f); }
    virtual float scene_spawn_yaw() const { return 0.0f; }
};

}  // namespace ptv
