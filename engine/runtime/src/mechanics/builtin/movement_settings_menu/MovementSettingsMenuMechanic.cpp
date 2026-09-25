// builtin.movement_settings_menu -- the player's movement and comfort
// settings, as a panel opened from a controller button.
//
// Replaces the locomotion picker that used to be hard-coded into
// Application and opened at the start of every game, whether the game had
// anything to choose or not -- and alongside a generated wrist menu when a
// game made its own, which is how one game ended up with two menus.
//
// The panel lists ONLY what this game actually offers, read from what the
// bound locomotion and turn mechanics declared when they attached:
//
//   * locomotion modes, when two or more are bound;
//   * snap vs smooth turning, when both are bound;
//   * vignette ("tunnel vision") switches, when the game enabled one as a
//     user option -- and each switch appears only while the motion it
//     applies to is the active one (sliding for the movement vignette,
//     smooth turning for the turning vignette). Snap turning and teleport
//     never get a vignette.
//
// A stationary bow game that offers snap or smooth turning therefore shows
// two rows; a game with nothing to choose never opens at all.
//
// Rows are icons, not text, because the engine has no in-world text: a ring
// for teleport, a headset for head-relative sliding, a controller for
// controller-relative sliding, a stepped arrow for snap turning, a smooth arc
// for smooth turning, and concentric rings for a vignette switch.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "../BuiltinMechanicBase.h"
#include "../ui_panel/UiPanelLayout.h"
#include "ptv/core/Logger.h"

namespace ptv {
namespace {

using namespace ui_panel;

constexpr const char* kLockOwner = "builtin.movement_settings_menu";
constexpr float kPi = 3.14159265358979323846f;

enum class Button { X, Y, A, B };

Button button_from(const std::string& s) {
    if (s == "y") return Button::Y;
    if (s == "a") return Button::A;
    if (s == "b") return Button::B;
    return Button::X;
}

bool pressed(const InputSnapshot& in, Button b) {
    switch (b) {
        case Button::Y: return in.button_y && !in.prev_button_y;
        case Button::A: return in.button_a && !in.prev_button_a;
        case Button::B: return in.button_b && !in.prev_button_b;
        case Button::X:
        default: return in.button_x && !in.prev_button_x;
    }
}

class MovementSettingsMenuMechanic final
    : public BuiltinMechanicBase<MovementSettingsMenuMechanic> {
public:
    static constexpr const char* kId = "builtin.movement_settings_menu";

    MechanicPhase phase() const override { return MechanicPhase::Input; }
    MechanicScope scope() const override { return MechanicScope::Player; }

    void configure(const nlohmann::json& p) {
        button_ = button_from(p.value("open_button", std::string("x")));
        open_on_start_ = p.value("open_on_start", false);
        panel_ = CUiPanel{};
        panel_.id = "movement_settings";
        panel_.width_m = 0.34f;
        panel_.row_height_m = 0.11f;
        panel_.hit_radius_m = 0.08f;
        panel_.distance_m = std::clamp(p.value("distance_m", 1.1f), 0.5f, 3.0f);
        warned_empty_ = false;
    }

    void tick(World&, std::uint32_t, float dt, const InputSnapshot& input,
              MechanicContext& ctx) {
        if (ctx.locomotion == nullptr) return;
        rebuild_rows(ctx);

        if (panel_.rows.empty()) {
            // Nothing this game lets the player choose. Never open, never
            // hold the sticks.
            if (!warned_empty_) {
                PTV_INFO(
                    "movement settings menu: nothing to choose in this game; "
                    "the menu will not open");
                warned_empty_ = true;
            }
            set_open(false, ctx);
            return;
        }

        if (open_on_start_) {
            set_open(true, ctx);
            open_on_start_ = false;
        }
        if (pressed(input, button_)) set_open(!panel_.open, ctx);
        if (!panel_.open) return;

        panel_.click_cooldown_s = std::max(0.0f, panel_.click_cooldown_s - dt);
        if (!panel_.anchor_set) {
            if (!input.head_valid) return;
            anchor_to_head(panel_, input.head_position,
                           input.head_rotation * Vec3(0.0f, 0.0f, -1.0f));
        }

        panel_.highlighted = -1;
        panel_.active_hand = -1;
        float best = panel_.hit_radius_m;
        for (int h = 0; h < 2; ++h) {
            const auto& hand = input.hands[h];
            if (!hand.aim_valid) continue;
            const Vec3 dir =
                glm::normalize(hand.aim_rotation * Vec3(0.0f, 0.0f, -1.0f));
            const int row = pick_row(panel_, hand.aim_position, dir);
            if (row < 0) continue;
            const float d = ray_point_distance(
                hand.aim_position, dir, row_world_position(panel_, row));
            if (d < best) {
                best = d;
                panel_.highlighted = row;
                panel_.active_hand = h;
            }
        }

        for (int h = 0; h < 2; ++h) {
            const auto& hand = input.hands[h];
            const bool edge = hand.prev_trigger < kTriggerPress &&
                              hand.trigger >= kTriggerPress;
            if (!edge || panel_.highlighted < 0 || panel_.active_hand != h) {
                continue;
            }
            if (panel_.click_cooldown_s > 0.0f) continue;
            apply(panel_.rows[static_cast<size_t>(panel_.highlighted)].id,
                  ctx);
            panel_.click_cooldown_s = 0.25f;
            if (ctx.haptics != nullptr) ctx.haptics->pulse(h, 0.03f, 0.4f);
            rebuild_rows(ctx);
            break;
        }

        draw(input, ctx);
    }

    void on_detach(World&, std::uint32_t) override { open_ = false; }

private:
    void set_open(bool open, MechanicContext& ctx) {
        if (open == panel_.open) return;
        panel_.open = open;
        panel_.anchor_set = false;
        panel_.highlighted = -1;
        // The pull that opened the panel must not also pick a row.
        panel_.click_cooldown_s = 0.35f;
        ctx.locomotion->set_input_lock(kLockOwner, open);
    }

    void add(const std::string& id, bool selected, bool marked) {
        UiRow r;
        r.id = id;
        r.label = id;
        r.selected = selected;
        r.marked = marked;
        panel_.rows.push_back(std::move(r));
    }

    void rebuild_rows(MechanicContext& ctx) {
        const int keep = panel_.highlighted;
        panel_.rows.clear();
        ILocomotion& loco = *ctx.locomotion;

        const bool tele = loco.mode_available(::ptv::LocomotionMode::Teleport);
        const bool head =
            loco.mode_available(::ptv::LocomotionMode::SlideHeadRelative);
        const bool ctrl =
            loco.mode_available(::ptv::LocomotionMode::SlideControllerRelative);
        const int modes = int(tele) + int(head) + int(ctrl);
        const auto active = loco.active_mode();
        if (modes >= 2) {
            if (tele) add("mode:teleport",
                          active == ::ptv::LocomotionMode::Teleport, false);
            if (head) add("mode:slide_head",
                          active == ::ptv::LocomotionMode::SlideHeadRelative,
                          false);
            if (ctrl) add("mode:slide_controller",
                          active ==
                              ::ptv::LocomotionMode::SlideControllerRelative,
                          false);
        }

        const bool snap = loco.turn_available(::ptv::TurnMode::Snap);
        const bool smooth = loco.turn_available(::ptv::TurnMode::Smooth);
        if (snap && smooth) {
            add("turn:snap", loco.turn_mode() == ::ptv::TurnMode::Snap, false);
            add("turn:smooth", loco.turn_mode() == ::ptv::TurnMode::Smooth,
                false);
        }

        if (ctx.comfort != nullptr) {
            const bool sliding =
                active == ::ptv::LocomotionMode::SlideHeadRelative ||
                active == ::ptv::LocomotionMode::SlideControllerRelative;
            if (sliding &&
                ctx.comfort->vignette_toggleable(ComfortChannel::Movement)) {
                add("vignette:movement", false,
                    ctx.comfort->vignette_enabled(ComfortChannel::Movement));
            }
            if (loco.turn_mode() == ::ptv::TurnMode::Smooth &&
                ctx.comfort->vignette_toggleable(ComfortChannel::Turning)) {
                add("vignette:turning", false,
                    ctx.comfort->vignette_enabled(ComfortChannel::Turning));
            }
        }
        panel_.highlighted =
            (keep >= 0 && keep < static_cast<int>(panel_.rows.size())) ? keep
                                                                         : -1;
    }

    void apply(const std::string& id, MechanicContext& ctx) {
        ILocomotion& loco = *ctx.locomotion;
        if (id == "mode:teleport") {
            loco.set_active_mode(::ptv::LocomotionMode::Teleport);
        } else if (id == "mode:slide_head") {
            loco.set_active_mode(::ptv::LocomotionMode::SlideHeadRelative);
        } else if (id == "mode:slide_controller") {
            loco.set_active_mode(::ptv::LocomotionMode::SlideControllerRelative);
        } else if (id == "turn:snap") {
            loco.set_turn_mode(::ptv::TurnMode::Snap);
        } else if (id == "turn:smooth") {
            loco.set_turn_mode(::ptv::TurnMode::Smooth);
        } else if (id == "vignette:movement" && ctx.comfort != nullptr) {
            ctx.comfort->set_vignette_enabled(
                ComfortChannel::Movement,
                !ctx.comfort->vignette_enabled(ComfortChannel::Movement));
        } else if (id == "vignette:turning" && ctx.comfort != nullptr) {
            ctx.comfort->set_vignette_enabled(
                ComfortChannel::Turning,
                !ctx.comfort->vignette_enabled(ComfortChannel::Turning));
        }
        PTV_INFO("movement settings: {}", id);
    }

    // --- Drawing -------------------------------------------------------

    void draw(const InputSnapshot& input, MechanicContext& ctx) const {
        if (ctx.draw == nullptr) return;
        const int count = static_cast<int>(panel_.rows.size());
        const Mat4 m = panel_matrix(panel_);
        const float hw = panel_.width_m * 0.5f;
        const float hh = panel_.row_height_m * 0.42f;

        for (int i = 0; i < count; ++i) {
            const UiRow& row = panel_.rows[static_cast<size_t>(i)];
            const Vec3 c = row_local(panel_, i, count);
            auto P = [&](float x, float y) {
                return Vec3(m * Vec4(c.x + x, c.y + y, c.z, 1.0f));
            };
            const Vec3 frame[5] = {P(-hw, -hh), P(hw, -hh), P(hw, hh),
                                   P(-hw, hh), P(-hw, -hh)};
            Vec4 color{0.60f, 0.64f, 0.70f, 1.0f};
            if (i == panel_.highlighted) {
                color = Vec4{1.0f, 0.95f, 0.55f, 1.0f};
            } else if (row.selected || row.marked) {
                color = Vec4{0.45f, 0.95f, 0.60f, 1.0f};
            }
            ctx.draw->polyline(frame, 5, color, 0.006f);
            draw_icon(row.id, P, hh, color, ctx);
            // A filled dot on the right says "this is on / chosen".
            if (row.selected || row.marked) {
                ctx.draw->marker(P(hw - 0.035f, 0.0f), 0.015f, color);
            }
        }

        if (panel_.active_hand >= 0 && panel_.highlighted >= 0) {
            const auto& hand = input.hands[panel_.active_hand];
            ctx.draw->line(hand.aim_position,
                           row_world(panel_, m, panel_.highlighted, count),
                           Vec4{1.0f, 0.95f, 0.55f, 1.0f}, 0.004f);
        }
    }

    template <typename Fn>
    void draw_icon(const std::string& id, Fn P, float hh, const Vec4& color,
                   MechanicContext& ctx) const {
        const float r = hh * 0.6f;
        auto circle = [&](float cx, float cy, float rad, int seg) {
            std::vector<Vec3> pts;
            for (int k = 0; k <= seg; ++k) {
                const float a = 2.0f * kPi * static_cast<float>(k) /
                                static_cast<float>(seg);
                pts.push_back(P(cx + rad * std::cos(a), cy + rad * std::sin(a)));
            }
            ctx.draw->polyline(pts.data(), pts.size(), color, 0.005f);
        };
        auto line = [&](float x0, float y0, float x1, float y1) {
            ctx.draw->line(P(x0, y0), P(x1, y1), color, 0.005f);
        };
        if (id == "mode:teleport") {
            circle(0.0f, 0.0f, r, 20);
        } else if (id == "mode:slide_head") {
            // Headset: a wide visor with two lenses.
            line(-r * 1.4f, -r * 0.5f, r * 1.4f, -r * 0.5f);
            line(-r * 1.4f, r * 0.5f, r * 1.4f, r * 0.5f);
            line(-r * 1.4f, -r * 0.5f, -r * 1.4f, r * 0.5f);
            line(r * 1.4f, -r * 0.5f, r * 1.4f, r * 0.5f);
            circle(-r * 0.6f, 0.0f, r * 0.3f, 10);
            circle(r * 0.6f, 0.0f, r * 0.3f, 10);
        } else if (id == "mode:slide_controller") {
            // Controller: a stick with a ring on top.
            line(0.0f, -r, 0.0f, r * 0.4f);
            circle(0.0f, r * 0.7f, r * 0.35f, 12);
        } else if (id == "turn:snap") {
            // Stepped arrow: turning in discrete jumps.
            line(-r * 1.2f, -r * 0.5f, -r * 0.4f, -r * 0.5f);
            line(-r * 0.4f, -r * 0.5f, -r * 0.4f, r * 0.5f);
            line(-r * 0.4f, r * 0.5f, r * 1.2f, r * 0.5f);
            line(r * 1.2f, r * 0.5f, r * 0.8f, r * 0.9f);
            line(r * 1.2f, r * 0.5f, r * 0.8f, r * 0.1f);
        } else if (id == "turn:smooth") {
            // Smooth arc with an arrowhead.
            std::vector<Vec3> pts;
            for (int k = 0; k <= 12; ++k) {
                const float a = kPi * (0.15f + 0.7f * k / 12.0f);
                pts.push_back(P(r * 1.2f * std::cos(a) , r * std::sin(a) - r * 0.4f));
            }
            ctx.draw->polyline(pts.data(), pts.size(), color, 0.005f);
            const Vec3 tip = pts.front();
            ctx.draw->line(tip, P(r * 1.2f * std::cos(kPi * 0.15f) - r * 0.4f,
                                  r * std::sin(kPi * 0.15f) - r * 0.4f),
                           color, 0.005f);
        } else if (id.rfind("vignette:", 0) == 0) {
            // Concentric rings: a view narrowing to its centre.
            circle(0.0f, 0.0f, r, 20);
            circle(0.0f, 0.0f, r * 0.45f, 14);
            // A small tick marks which motion it applies to.
            if (id == "vignette:turning") {
                line(r * 1.2f, -r * 0.3f, r * 1.2f, r * 0.3f);
            } else {
                line(-r * 0.3f, -r * 1.2f, r * 0.3f, -r * 1.2f);
            }
        }
    }

    CUiPanel panel_{};
    Button button_{Button::X};
    bool open_on_start_{false};
    bool open_{false};
    bool warned_empty_{false};
};

}  // namespace

extern "C" int ptv_force_link_builtin_movement_settings_menu = 1;

PTV_REGISTER_MECHANIC("builtin.movement_settings_menu",
                      MovementSettingsMenuMechanic)

}  // namespace ptv
