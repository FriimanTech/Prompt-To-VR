// ---------------------------------------------------------------------------
// builtin.game_hud -- the game loop's numbers, on the left wrist.
//
// Shows exactly what the GameDesign enabled and nothing else: a health bar
// when the player can be hurt, lives when there are lives, the clock when an
// objective is timed, score, and the current objective. Wrist-mounted rather
// than head-locked because a HUD glued to the face is the classic VR comfort
// mistake: it cannot be looked away from and fights the eyes' focus. Turning
// the wrist up to read it is the Quest-native gesture players already know.
//
// Reads ctx.game and ctx.combat only, so it shows the same truth the flow
// acts on -- never a private copy that could disagree with it.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cmath>
#include <string>

#include "../BuiltinMechanicBase.h"
#include "ptv/api/Facing.h"

namespace ptv {
namespace {

class GameHudMechanic final : public BuiltinMechanicBase<GameHudMechanic> {
public:
    static constexpr const char* kId = "builtin.game_hud";
    MechanicPhase phase() const override { return MechanicPhase::Simulate; }

    void configure(const nlohmann::json& p) {
        health_ = p.value("health", false);
        lives_ = p.value("lives", false);
        timer_ = p.value("timer", false);
        score_ = p.value("score", false);
        objective_ = p.value("objective", false);
        hand_ = std::clamp(p.value("hand", 0), 0, 1);
        text_h_ = std::clamp(p.value("text_height_m", 0.011f), 0.004f, 0.05f);
    }

    void tick(World&, std::uint32_t, float, const InputSnapshot& input,
              MechanicContext& ctx) {
        if (ctx.text == nullptr || ctx.game == nullptr || !input.head_valid) return;
        const auto& hand = input.hands[hand_];
        if (!hand.hand_valid) return;
        const GameFlowState st = ctx.game->state();
        if (st == GameFlowState::GameOver || st == GameFlowState::Victory ||
            st == GameFlowState::Completed || st == GameFlowState::Exiting) {
            return;
        }

        // Just above the back of the wrist, facing the eyes.
        const Vec3 up(0.0f, 1.0f, 0.0f);
        Vec3 base = hand.hand_position + hand.hand_rotation * Vec3(0.0f, 0.035f, 0.07f);
        Vec3 to_eye = input.head_position - base;
        to_eye.y = 0.0f;
        if (glm::length(to_eye) < 1e-3f) return;
        const Quat face = rotation_facing(-to_eye);
        const Vec3 right = face * Vec3(1.0f, 0.0f, 0.0f);
        const float line = text_h_ * 1.8f;
        float y = 0.0f;
        const Vec4 white(0.95f, 0.97f, 1.0f, 1.0f);

        if (objective_) {
            const std::string text = objective_text(ctx);
            if (!text.empty()) {
                ctx.text->text(text.c_str(), base + up * y, face, text_h_ * 0.85f,
                               Vec4(0.85f, 0.9f, 1.0f, 1.0f));
                y += line;
            }
        }
        std::string row;
        if (timer_) {
            const float left = ctx.game->time_remaining_s();
            row += clock(left >= 0.0f ? left : ctx.game->elapsed_s());
        }
        if (score_) {
            if (!row.empty()) row += "   ";
            row += std::to_string(ctx.game->score());
        }
        if (lives_ && ctx.game->lives() >= 0) {
            if (!row.empty()) row += "   ";
            row += "Lives " + std::to_string(ctx.game->lives());
        }
        if (!row.empty()) {
            ctx.text->text(row.c_str(), base + up * y, face, text_h_, white);
            y += line;
        }
        if (health_ && ctx.combat != nullptr && ctx.hands != nullptr) {
            const std::uint32_t player = ctx.hands->player_entity();
            const float maxh = ctx.combat->max_health(player);
            if (maxh > 0.0f) {
                const float frac =
                    std::clamp(ctx.combat->health(player) / maxh, 0.0f, 1.0f);
                const float width = 0.08f;
                const Vec3 l = base + up * (y + 0.3f * text_h_) - right * (0.5f * width);
                const Vec4 bar = frac > 0.5f   ? Vec4(0.3f, 0.95f, 0.45f, 1.0f)
                                 : frac > 0.25f ? Vec4(1.0f, 0.8f, 0.2f, 1.0f)
                                                : Vec4(1.0f, 0.25f, 0.2f, 1.0f);
                if (ctx.draw != nullptr) {
                    ctx.draw->line(l, l + right * width, Vec4(0.15f, 0.15f, 0.18f, 1.0f),
                                   0.006f);
                    if (frac > 0.0f) ctx.draw->line(l, l + right * (width * frac), bar, 0.007f);
                }
                const std::string hp =
                    std::to_string(static_cast<int>(std::ceil(ctx.combat->health(player))));
                ctx.text->text(hp.c_str(), l + right * (width + 0.006f) - up * (0.5f * text_h_),
                               face, text_h_ * 0.9f, white, TextAlign::Left);
            }
        }
        if (st == GameFlowState::Paused) {
            ctx.text->text("Paused", base + up * (y + line), face, text_h_, white);
        }
    }

private:
    static std::string clock(float s) {
        const int t = std::max(0, static_cast<int>(std::ceil(s)));
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d:%02d", t / 60, t % 60);
        return buf;
    }

    static std::string objective_text(MechanicContext& ctx) {
        GameFlowShared* s = ctx.game->flow_state();
        if (s == nullptr) return {};
        for (const auto& o : s->objectives) {
            if (o.complete) continue;
            std::string text = o.description.empty() ? o.id : o.description;
            if (text.size() > 32) text = text.substr(0, 31) + ".";
            if (o.kind != "survive_time" && o.target > 1.0f) {
                text += " " + std::to_string(static_cast<int>(o.progress)) + "/" +
                        std::to_string(static_cast<int>(o.target));
            }
            return text;
        }
        return {};
    }

    bool health_{false};
    bool lives_{false};
    bool timer_{false};
    bool score_{false};
    bool objective_{false};
    int hand_{0};
    float text_h_{0.011f};
};

}  // namespace

PTV_REGISTER_MECHANIC("builtin.game_hud", GameHudMechanic)

extern "C" int ptv_force_link_builtin_game_hud = 1;

}  // namespace ptv
