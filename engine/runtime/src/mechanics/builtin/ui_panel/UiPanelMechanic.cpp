// ---------------------------------------------------------------------------
// builtin.ui_panel -- a world-anchored virtual panel with a laser pointer.
//
// Generalized out of the locomotion menu in Application.cpp. That menu worked,
// and everything it got right is preserved here: comfort-anchored placement
// from Meta's "Head -> Comfort" guidance, either hand may point, whichever
// hand is closest wins the highlight, the click is edge-triggered, and a
// cooldown stops the pull that opened the panel from also selecting a row.
//
// What it got wrong was being unreusable: two hard-coded pads, two hard-coded
// pages, and locomotion decisions written directly into the hit-test. Any
// generated menu -- a wardrobe, a mission select, a shop -- had to reimplement
// the whole thing over a MechanicContext that offered only debug draw and
// raycasts.
//
// The division of labour here is the point. This mechanic owns where the panel
// sits, which row the pointer is on, and what counts as a click. It has no
// idea what a row MEANS. A wardrobe is then generated code that lists what
// IEquip reports and calls unequip on a click, which is a few lines rather
// than a from-scratch UI.
//
// Rows are drawn as state-coloured plates with their label in SDF text
// (ctx.text), plus an optional thumbnail entity -- for a wardrobe the player
// recognises their gauntlet by looking at it. A panel title and subtitle
// (IUi::set_title) sit above the rows: "Game Over" / "Survived 2:41".
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "../BuiltinMechanicBase.h"
#include "UiPanelLayout.h"

#include <glm/gtc/matrix_transform.hpp>

namespace ptv {

using namespace ui_panel;

class UiPanelMechanic final : public BuiltinMechanicBase<UiPanelMechanic> {
public:
    static constexpr const char* kId = "builtin.ui_panel";

    /** Input phase: a click has to be visible to Simulate-phase mechanics on
     *  the SAME frame, or a wardrobe reacts a frame after the player pressed. */
    MechanicPhase phase() const override { return MechanicPhase::Input; }

    /** Unused: this mechanic overrides ``on_attach`` because it needs the world
     *  and entity to place the component. The CRTP base still instantiates a
     *  call to it. */
    void configure(const nlohmann::json&) {}

    void on_attach(World& world, std::uint32_t entity, const nlohmann::json& p,
                   MechanicContext&) override {
        auto& panel = world.emplace_or_replace<CUiPanel>(entity, CUiPanel{});
        panel.id = p.value("panel_id", std::string("panel"));
        panel.open = p.value("open_on_start", false);
        panel.distance_m = p.value("distance_m", panel.distance_m);
        panel.below_eye_m = p.value("below_eye_m", panel.below_eye_m);
        panel.row_height_m = p.value("row_height_m", panel.row_height_m);
        panel.width_m = p.value("width_m", panel.width_m);
        panel.hit_radius_m = p.value("hit_radius_m", panel.hit_radius_m);
        panel.pointer_hand = p.value("pointer_hand", -1);

        // Rows declared in the scene, for a fixed menu that needs no code at
        // all. A dynamic panel leaves this empty and calls set_rows.
        if (p.contains("rows") && p["rows"].is_array()) {
            for (const auto& r : p["rows"]) {
                if (!r.is_object()) continue;
                UiRow row{};
                row.id = r.value("id", "");
                row.label = r.value("label", row.id);
                row.enabled = r.value("enabled", true);
                if (row.id.empty()) continue;
                panel.rows.push_back(std::move(row));
            }
        }
    }

    void tick(World& world, std::uint32_t entity, float dt,
              const InputSnapshot& input, MechanicContext& ctx) {
        if (!world.has<CUiPanel>(entity)) return;
        auto& panel = world.get<CUiPanel>(entity);

        panel.click_cooldown_s = std::max(0.0f, panel.click_cooldown_s - dt);

        if (!panel.open) {
            // Forget the placement so the next open re-anchors to wherever the
            // player is then, rather than to wherever they were when they last
            // closed it -- possibly a different room.
            panel.anchor_set = false;
            panel.highlighted = -1;
            panel.active_hand = -1;
            return;
        }

        // Anchoring needs a real head pose. Placing the panel from the default
        // identity pose would put it at the world origin, which is wherever the
        // level happens to start rather than in front of the player.
        if (!panel.anchor_set) {
            if (!input.head_valid) return;
            const Vec3 fwd = input.head_rotation * Vec3(0.0f, 0.0f, -1.0f);
            anchor_to_head(panel, input.head_position, fwd);
        }

        // Hit-test from every eligible hand and let the closest win. Either
        // hand may point unless the panel pinned one, so a player does not have
        // to swap the controller they are already holding something with.
        panel.highlighted = -1;
        panel.active_hand = -1;
        float best = panel.hit_radius_m;
        for (int h = 0; h < 2; ++h) {
            if (panel.pointer_hand >= 0 && panel.pointer_hand != h) continue;
            const auto& hand = input.hands[h];
            if (!hand.hand_valid) continue;
            // Aim along the controller body, not the angled grip, so the ray
            // goes where the controller visually points.
            const Vec3 dir = glm::normalize(hand.aim_rotation *
                                            Vec3(0.0f, 0.0f, -1.0f));
            const int row = ui_panel::pick_row(panel, hand.aim_position, dir);
            if (row < 0) continue;
            const float d = ray_point_distance(
                hand.aim_position, dir,
                ui_panel::row_world_position(panel, row));
            if (d < best) {
                best = d;
                panel.highlighted = row;
                panel.active_hand = h;
            }
        }

        // Click on a rising trigger edge, and only from the hand that owns the
        // highlight -- otherwise a stray pull from the player's other hand,
        // busy doing something unrelated, fires the menu.
        for (int h = 0; h < 2; ++h) {
            const auto& hand = input.hands[h];
            // The engine already froze last frame's value, so the panel does
            // not keep its own copy -- two records of the same fact would drift
            // the moment one of them was updated on a frame the other skipped.
            const bool edge = hand.prev_trigger < kTriggerPress &&
                              hand.trigger >= kTriggerPress;
            if (!edge) continue;
            if (panel.highlighted < 0 || panel.active_hand != h) continue;
            if (panel.click_cooldown_s > 0.0f) continue;

            push_event(panel, panel.highlighted, h);
            panel.click_cooldown_s = 0.25f;
            if (ctx.haptics != nullptr) ctx.haptics->pulse(h, 0.03f, 0.4f);
        }

        draw(panel, input, ctx);
    }

private:
    /** Outline every row, fill the highlighted one, and draw the pointer.
     *
     *  Colour carries the state because there is no text: marked rows read
     *  warm, disabled rows fade back, and the highlight is the brightest thing
     *  on the panel so the player can see what a click will hit before making
     *  it. */
    void draw(const CUiPanel& panel, const InputSnapshot& input,
              MechanicContext& ctx) const {
        if (ctx.draw == nullptr) return;
        const int count = static_cast<int>(panel.rows.size());
        const Mat4 m = panel_matrix(panel);
        const float hw = panel.width_m * 0.5f;
        const float hh = panel.row_height_m * 0.45f;
        draw_text(panel, m, ctx);
        if (count == 0) return;

        for (int i = 0; i < count; ++i) {
            const UiRow& row = panel.rows[static_cast<std::size_t>(i)];
            const Vec3 c = row_local(panel, i, count);
            const Vec3 corners[5] = {
                Vec3(m * Vec4(c.x - hw, c.y - hh, c.z, 1.0f)),
                Vec3(m * Vec4(c.x + hw, c.y - hh, c.z, 1.0f)),
                Vec3(m * Vec4(c.x + hw, c.y + hh, c.z, 1.0f)),
                Vec3(m * Vec4(c.x - hw, c.y + hh, c.z, 1.0f)),
                Vec3(m * Vec4(c.x - hw, c.y - hh, c.z, 1.0f)),
            };

            Vec4 color{0.62f, 0.66f, 0.72f, 1.0f};
            if (!row.enabled) {
                color = Vec4{0.34f, 0.35f, 0.38f, 1.0f};
            } else if (i == panel.highlighted) {
                color = Vec4{1.0f, 0.95f, 0.55f, 1.0f};
            } else if (row.selected) {
                color = Vec4{0.70f, 0.90f, 1.0f, 1.0f};
            } else if (row.marked) {
                color = Vec4{1.0f, 0.72f, 0.40f, 1.0f};
            }
            ctx.draw->polyline(corners, 5, color, 0.008f);

            // A filled centre distinguishes "worn" at a glance from across the
            // room, where an outline colour is hard to judge.
            if (row.marked) {
                ctx.draw->marker(row_world(panel, m, i, count), hh * 0.5f,
                                 color);
            }
        }

        // The pointer ray, so aiming is a visible act rather than guesswork.
        if (panel.active_hand >= 0) {
            const auto& hand = input.hands[panel.active_hand];
            if (hand.hand_valid && panel.highlighted >= 0) {
                ctx.draw->line(hand.aim_position,
                               row_world(panel, m, panel.highlighted, count),
                               Vec4{1.0f, 0.95f, 0.55f, 1.0f}, 0.006f);
            }
        }
    }

    /** Title above the rows, subtitle under it, and each row's label on the
     *  row. Text sits a few millimetres in front of the plates so the two
     *  never z-fight, and is sized from the row height so a panel scaled for
     *  a far wall stays as readable as one at arm's length. */
    void draw_text(const CUiPanel& panel, const Mat4& m,
                   MechanicContext& ctx) const {
        if (ctx.text == nullptr) return;
        const int count = static_cast<int>(panel.rows.size());
        const Quat facing = glm::angleAxis(panel.yaw_rad, Vec3(0.0f, 1.0f, 0.0f));
        const float label_h = panel.row_height_m * 0.30f;
        constexpr float kLift = 0.004f;
        const float top =
            count > 0 ? row_local(panel, 0, count).y + panel.row_height_m * 0.5f
                      : 0.0f;
        float y = top + panel.row_height_m * 0.35f;
        if (!panel.subtitle.empty()) {
            ctx.text->text(panel.subtitle.c_str(),
                           Vec3(m * Vec4(0.0f, y, kLift, 1.0f)), facing,
                           label_h * 0.8f, Vec4(0.85f, 0.88f, 0.92f, 1.0f));
            y += label_h * 1.9f;
        }
        if (!panel.title.empty()) {
            ctx.text->text(panel.title.c_str(),
                           Vec3(m * Vec4(0.0f, y, kLift, 1.0f)), facing,
                           label_h * 1.7f, Vec4(1.0f, 1.0f, 1.0f, 1.0f));
        }
        for (int i = 0; i < count; ++i) {
            const UiRow& row = panel.rows[static_cast<std::size_t>(i)];
            if (row.label.empty()) continue;
            const Vec3 c = row_local(panel, i, count);
            Vec4 color{0.92f, 0.94f, 0.97f, 1.0f};
            if (!row.enabled) color = Vec4{0.55f, 0.56f, 0.60f, 1.0f};
            if (i == panel.highlighted) color = Vec4{1.0f, 0.95f, 0.55f, 1.0f};
            ctx.text->text(row.label.c_str(),
                           Vec3(m * Vec4(c.x, c.y - 0.5f * label_h, c.z + kLift, 1.0f)),
                           facing, label_h, color);
        }
    }
};

// Spelled as a literal, matching every other built-in: the catalog
// consistency check greps for these ids, and a symbolic one reads as an
// unregistered mechanic to it.
PTV_REGISTER_MECHANIC("builtin.ui_panel", UiPanelMechanic)

extern "C" int ptv_force_link_builtin_ui_panel = 1;

}  // namespace ptv
