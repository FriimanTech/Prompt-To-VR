#pragma once

// ---------------------------------------------------------------------------
// Where a panel's rows are, which one a ray selects, and how clicks queue.
//
// Split from the mechanic so it can be tested without a headset, an OpenXR
// session, or a frame loop. All of the parts that are easy to get subtly wrong
// live here: whether a list stays centred as it grows, whether a disabled row
// can be selected, whether aiming away from the panel picks the nearest row
// anyway, and whether an unread click queue grows without bound.
//
// Pure math plus std, so it costs the Quest build nothing.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>

#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <glm/gtc/matrix_transform.hpp>

namespace ptv::ui_panel {

/** Bound on the unread click queue.
 *
 *  An unread queue means the caller stopped polling, which is a bug in the
 *  caller. Dropping the oldest keeps that bug from becoming a slow leak while
 *  keeping the newest click -- the one the player just made. */
constexpr std::size_t kMaxQueuedEvents = 16;

constexpr float kTriggerPress = 0.5f;
constexpr float kTriggerRelease = 0.4f;

/** Distance from a ray to a point, or a large number when the point is behind
 *  the ray's origin.
 *
 *  The behind-the-origin case is what stops a player facing away from the panel
 *  from still driving it: without it, the closest point on the infinite LINE
 *  would happily land on a row at their back. */
inline float ray_point_distance(const Vec3& origin, const Vec3& dir,
                                const Vec3& p) {
    const Vec3 v = p - origin;
    const float t = glm::dot(v, dir);
    if (t < 0.0f) return 1e9f;
    return glm::length((origin + dir * t) - p);
}

/** Panel-local position of row ``i``.
 *
 *  Centred vertically, so a list grows evenly in both directions instead of
 *  creeping down out of comfortable view as it gets longer. */
inline Vec3 row_local(const CUiPanel& panel, int i, int count) {
    const float total = static_cast<float>(count) * panel.row_height_m;
    const float top = total * 0.5f - panel.row_height_m * 0.5f;
    return Vec3(0.0f, top - static_cast<float>(i) * panel.row_height_m, 0.0f);
}

/** Panel-to-world, yaw only.
 *
 *  Yaw only because a panel that pitched to face the head exactly would tilt
 *  away from the horizon, and a tilted reading surface is worse to read in a
 *  headset than a slightly oblique upright one. */
inline Mat4 panel_matrix(const CUiPanel& panel) {
    const Mat4 m = glm::translate(Mat4(1.0f), panel.anchor);
    return glm::rotate(m, panel.yaw_rad, Vec3(0.0f, 1.0f, 0.0f));
}

inline Vec3 row_world(const CUiPanel& panel, const Mat4& m, int i, int count) {
    const Vec4 p = m * Vec4(row_local(panel, i, count), 1.0f);
    return Vec3(p);
}

inline Vec3 row_world_position(const CUiPanel& panel, int index) {
    return row_world(panel, panel_matrix(panel), index,
                     static_cast<int>(panel.rows.size()));
}

/** Which row a ray from ``origin`` along ``dir`` selects, or -1.
 *
 *  Disabled rows are skipped rather than highlighted-and-refused: a highlight
 *  the player cannot act on reads as the panel being broken. */
inline int pick_row(const CUiPanel& panel, const Vec3& origin,
                    const Vec3& dir) {
    const int count = static_cast<int>(panel.rows.size());
    if (count == 0) return -1;
    const Mat4 m = panel_matrix(panel);
    int best = -1;
    float best_dist = panel.hit_radius_m;
    for (int i = 0; i < count; ++i) {
        if (!panel.rows[static_cast<std::size_t>(i)].enabled) continue;
        const float d =
            ray_point_distance(origin, dir, row_world(panel, m, i, count));
        if (d < best_dist) {
            best_dist = d;
            best = i;
        }
    }
    return best;
}

/** Place the panel in front of ``head``, facing back at it. */
inline void anchor_to_head(CUiPanel& panel, const Vec3& head,
                           const Vec3& forward_xz) {
    const float fl = glm::length(Vec3(forward_xz.x, 0.0f, forward_xz.z));
    const Vec3 fwd =
        fl > 1e-5f ? glm::normalize(Vec3(forward_xz.x, 0.0f, forward_xz.z))
                   : Vec3(0.0f, 0.0f, -1.0f);
    panel.anchor = Vec3(head.x + fwd.x * panel.distance_m,
                        head.y - panel.below_eye_m,
                        head.z + fwd.z * panel.distance_m);
    // The panel's local +Z is its face, so rotate that to point back at the
    // user: R(yaw) * (0,0,1) == -fwd.
    panel.yaw_rad = std::atan2(-fwd.x, -fwd.z);
    panel.anchor_set = true;
}

/** Queue a click, dropping the oldest if the caller has stopped reading. */
inline void push_event(CUiPanel& panel, int index, int hand) {
    UiEvent ev{};
    ev.panel_id = panel.id;
    ev.row_index = index;
    ev.hand = hand;
    if (index >= 0 && index < static_cast<int>(panel.rows.size())) {
        ev.row_id = panel.rows[static_cast<std::size_t>(index)].id;
    }
    panel.events.push_back(std::move(ev));
    if (panel.events.size() > kMaxQueuedEvents) {
        panel.events.erase(panel.events.begin());
    }
}

}  // namespace ptv::ui_panel
