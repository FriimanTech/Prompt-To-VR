#include "HandRig.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <glm/gtc/quaternion.hpp>

#include "ptv/api/BodySlots.h"

namespace ptv::hand_rig {
namespace {

const std::array<const char*, kFingers> kFingerNames = {
    "thumb", "index", "middle", "ring", "pinky",
};

Vec3 bind_pos(const ISkeleton& sk, std::uint32_t e, int j) {
    return Vec3(sk.joint_bind_model(e, j)[3]);
}

Quat bind_rot(const ISkeleton& sk, std::uint32_t e, int j) {
    return glm::normalize(glm::quat_cast(Mat3(sk.joint_bind_model(e, j))));
}

Vec3 safe_normalize(const Vec3& v, const Vec3& fallback) {
    const float l = glm::length(v);
    return (l > 1e-6f) ? v / l : fallback;
}

std::vector<int> children_of(const ISkeleton& sk, std::uint32_t e, int joint) {
    std::vector<int> out;
    const int n = sk.joint_count(e);
    for (int j = 0; j < n; ++j) {
        if (sk.joint_parent(e, j) == joint) out.push_back(j);
    }
    return out;
}

/** Follow first children down from ``start``: the whole finger chain,
 *  including any end/tip bone. */
std::vector<int> chain_from(const ISkeleton& sk, std::uint32_t e, int start) {
    std::vector<int> chain;
    int j = start;
    while (j >= 0 && chain.size() < 8) {
        chain.push_back(j);
        const auto kids = children_of(sk, e, j);
        j = kids.empty() ? -1 : kids.front();
    }
    return chain;
}

/** Resolve fingers through BodySlots names. Returns how many segments hit. */
int resolve_by_name(const ISkeleton& sk, std::uint32_t e, int hand, Rig& rig) {
    int found = 0;
    for (int f = 0; f < kFingers; ++f) {
        for (int s = 0; s < kSegments; ++s) {
            const std::string slot = body_finger_slot(
                kFingerNames[static_cast<size_t>(f)], s + 1, hand);
            const int j = sk.joint_for_slot(e, slot.c_str());
            rig.fingers[static_cast<size_t>(f)]
                .segments[static_cast<size_t>(s)].joint = j;
            if (j >= 0) ++found;
        }
    }
    return found;
}

/** A joint with at least four multi-joint chains hanging off it: a wrist. */
int find_wrist_by_topology(const ISkeleton& sk, std::uint32_t e) {
    const int n = sk.joint_count(e);
    int best = -1;
    int best_chains = 0;
    for (int j = 0; j < n; ++j) {
        int chains = 0;
        for (const int c : children_of(sk, e, j)) {
            if (chain_from(sk, e, c).size() >= 3) ++chains;
        }
        if (chains >= 4 && chains > best_chains) {
            best = j;
            best_chains = chains;
        }
    }
    return best;
}

/** Name-free fallback: five chains off the wrist, thumb rooted nearest the
 *  wrist, the rest ordered outward from the thumb. */
bool resolve_by_topology(const ISkeleton& sk, std::uint32_t e, Rig& rig) {
    if (rig.wrist < 0) return false;
    struct Chain {
        std::vector<int> joints;
        Vec3 base;
    };
    std::vector<Chain> chains;
    const Vec3 wrist = bind_pos(sk, e, rig.wrist);
    for (const int c : children_of(sk, e, rig.wrist)) {
        auto joints = chain_from(sk, e, c);
        if (joints.size() < 3) continue;
        // A metacarpal-first rig (Unreal style) has five or more joints per
        // chain; the three that bend are the ones after the metacarpal.
        if (joints.size() >= 5) joints.erase(joints.begin());
        chains.push_back({joints, bind_pos(sk, e, joints.front())});
    }
    if (chains.size() < 4) return false;

    const auto thumb_it = std::min_element(
        chains.begin(), chains.end(), [&](const Chain& a, const Chain& b) {
            return glm::length(a.base - wrist) < glm::length(b.base - wrist);
        });
    const Chain thumb = *thumb_it;
    chains.erase(thumb_it);
    std::sort(chains.begin(), chains.end(),
              [&](const Chain& a, const Chain& b) {
                  return glm::length(a.base - thumb.base) <
                         glm::length(b.base - thumb.base);
              });

    auto assign = [&](int finger, const Chain& ch) {
        for (int s = 0; s < kSegments; ++s) {
            rig.fingers[static_cast<size_t>(finger)]
                .segments[static_cast<size_t>(s)]
                .joint = (static_cast<size_t>(s) < ch.joints.size())
                             ? ch.joints[static_cast<size_t>(s)]
                             : -1;
        }
    };
    assign(Thumb, thumb);
    for (size_t i = 0; i < chains.size() && i < 4; ++i) {
        assign(static_cast<int>(i) + 1, chains[i]);
    }
    return true;
}

/** Where a segment's bone points: toward the next segment, or its own
 *  child, or (for a distal joint with no child) the previous direction. */
Vec3 bone_dir(const ISkeleton& sk, std::uint32_t e, const FingerChain& ch,
              int s, const Vec3& fallback) {
    const int j = ch.segments[static_cast<size_t>(s)].joint;
    int child = (s + 1 < kSegments)
                    ? ch.segments[static_cast<size_t>(s + 1)].joint
                    : -1;
    if (child < 0) {
        const auto kids = children_of(sk, e, j);
        if (!kids.empty()) child = kids.front();
    }
    if (child < 0) return fallback;
    return safe_normalize(bind_pos(sk, e, child) - bind_pos(sk, e, j),
                          fallback);
}

}  // namespace

int Rig::joint_count() const {
    int n = 0;
    for (const auto& f : fingers) {
        for (const auto& s : f.segments) {
            if (s.joint >= 0) ++n;
        }
    }
    return n;
}

Rig analyze(const ISkeleton& sk, std::uint32_t e, int hand, Resolution* how) {
    Rig rig;
    rig.hand = (hand == 0) ? 0 : 1;
    if (how) *how = Resolution::None;
    if (!sk.has_skeleton(e)) return rig;

    rig.wrist = sk.joint_for_slot(
        e, (std::string("hand") + body_side_suffix(rig.hand)).c_str());

    const int named = resolve_by_name(sk, e, rig.hand, rig);
    const bool named_ok = rig.fingers[Index].present() &&
                          (rig.fingers[Pinky].present() ||
                           rig.fingers[Ring].present());
    if (named_ok) {
        if (how) *how = Resolution::Names;
        if (rig.wrist < 0) {
            rig.wrist = sk.joint_parent(
                e, rig.fingers[Index].segments[0].joint);
        }
    } else {
        // Names failed. Only trust topology when this looks like a single
        // hand -- on a full body an unnamed "wrist" could be either side.
        const int other_side = sk.joint_for_slot(
            e, (std::string("hand") + body_side_suffix(1 - rig.hand)).c_str());
        if (rig.wrist < 0 && other_side >= 0) return rig;
        if (rig.wrist < 0) rig.wrist = find_wrist_by_topology(sk, e);
        for (auto& f : rig.fingers) f = FingerChain{};
        if (!resolve_by_topology(sk, e, rig)) return rig;
        if (how) *how = Resolution::Topology;
    }
    (void)named;
    if (rig.wrist < 0) return rig;

    // --- Palm frame -------------------------------------------------------
    const Vec3 w = bind_pos(sk, e, rig.wrist);
    const Vec3 i = bind_pos(sk, e, rig.fingers[Index].segments[0].joint);
    const int outer_joint = rig.fingers[Pinky].present()
                                ? rig.fingers[Pinky].segments[0].joint
                                : rig.fingers[Ring].segments[0].joint;
    const Vec3 p = bind_pos(sk, e, outer_joint);
    const Vec3 m = rig.fingers[Middle].present()
                       ? bind_pos(sk, e, rig.fingers[Middle].segments[0].joint)
                       : 0.5f * (i + p);

    const Vec3 f = safe_normalize(m - w, Vec3(0.0f, 0.0f, -1.0f));
    Vec3 s = p - i;
    s -= f * glm::dot(s, f);
    s = safe_normalize(s, Vec3(1.0f, 0.0f, 0.0f));
    // Right hand, palm down, fingers forward: the thumb is on the left, so
    // cross(fingers, index->pinky) points out of the palm. The left hand is
    // the mirror image.
    Vec3 n = (rig.hand == 1) ? glm::cross(f, s) : glm::cross(s, f);

    // A rig whose rest pose is curled tells us for certain which side the
    // palm is on. If its fingers curl AWAY from our normal the rig is the
    // other hand's geometry (or its bones are mirrored); believe the
    // geometry, because that is what the fingers will actually bend toward.
    {
        float agree = 0.0f;
        float total = 0.0f;
        for (int fi = Index; fi <= Pinky; ++fi) {
            const auto& ch = rig.fingers[static_cast<size_t>(fi)];
            if (!ch.present() || ch.segments[2].joint < 0) continue;
            Vec3 c = bind_pos(sk, e, ch.segments[2].joint) -
                     bind_pos(sk, e, ch.segments[0].joint);
            c -= f * glm::dot(c, f);
            agree += glm::dot(c, n);
            total += glm::length(c);
        }
        if (total > 1e-4f && agree < -0.3f * total) n = -n;
    }

    // --- Curl axes ----------------------------------------------------------
    for (int fi = 0; fi < kFingers; ++fi) {
        auto& ch = rig.fingers[static_cast<size_t>(fi)];
        if (!ch.present()) continue;
        Vec3 prev = f;
        for (int si = 0; si < kSegments; ++si) {
            auto& seg = ch.segments[static_cast<size_t>(si)];
            if (seg.joint < 0) continue;
            const Vec3 d = bone_dir(sk, e, ch, si, prev);
            prev = d;
            // cross(d, n) rotates d toward n for a positive angle.
            const Vec3 axis_model =
                safe_normalize(glm::cross(d, n), glm::normalize(glm::cross(f, n)));
            seg.curl_axis_local =
                glm::normalize(glm::inverse(bind_rot(sk, e, seg.joint)) *
                               axis_model);
        }
    }
    if (rig.fingers[Thumb].present()) {
        const int t1 = rig.fingers[Thumb].segments[0].joint;
        const Vec3 dt = bone_dir(sk, e, rig.fingers[Thumb], 0, f);
        const Vec3 toward_index = safe_normalize(i - bind_pos(sk, e, t1), f);
        const Vec3 axis_model = safe_normalize(glm::cross(dt, toward_index),
                                               glm::normalize(glm::cross(f, n)));
        rig.thumb_adduct_axis_local =
            glm::normalize(glm::inverse(bind_rot(sk, e, t1)) * axis_model);
    }

    // --- Grip alignment -----------------------------------------------------
    // OpenXR grip: +X normal to the palm (away from the palm on the left
    // hand, INTO it on the right), -Z along the closed fingers' tube from
    // little finger to thumb, +Y completing a right-handed frame -- which
    // makes +Y point back along the hand, away from the fingers.
    const Vec3 gx = (rig.hand == 0) ? n : -n;
    const Vec3 gy = -f;
    const Vec3 gz = glm::normalize(glm::cross(gx, gy));
    const Vec3 knuckles = 0.5f * (i + p);
    const float palm_len = glm::length(knuckles - w);
    // The grip origin is the palm centroid at the palm's surface; the bones
    // run through the middle of the hand, so step toward the palm a little.
    const Vec3 origin = glm::mix(w, knuckles, 0.5f) + n * (0.12f * palm_len);

    Mat4 H(1.0f);
    H[0] = Vec4(gx, 0.0f);
    H[1] = Vec4(gy, 0.0f);
    H[2] = Vec4(gz, 0.0f);
    H[3] = Vec4(origin, 1.0f);
    rig.hand_frame_model = H;
    rig.grip_to_entity = glm::inverse(H);

    rig.wrist_bind_model = sk.joint_bind_model(e, rig.wrist);
    const Mat4 hand_in_wrist = glm::inverse(rig.wrist_bind_model) * H;
    rig.grip_to_wrist_rot =
        glm::normalize(glm::inverse(glm::quat_cast(Mat3(hand_in_wrist))));
    rig.palm_offset_wrist_local = Vec3(hand_in_wrist[3]);

    rig.valid = rig.joint_count() > 0;
    return rig;
}

}  // namespace ptv::hand_rig
