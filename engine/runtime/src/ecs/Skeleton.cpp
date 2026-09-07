#include "ptv/ecs/Skeleton.h"

#include <algorithm>
#include <cmath>

namespace ptv {
namespace {

using gltf_mesh_load::AnimChannel;
using gltf_mesh_load::AnimInterp;
using gltf_mesh_load::AnimPath;
using gltf_mesh_load::JointPose;

/** Locate ``t`` in a sorted key list.
 *
 * Returns the index of the key at or before ``t`` and the 0..1 fraction to
 * the next one. Clamps at both ends, which is what glTF requires outside the
 * sampler's range. */
struct KeyLookup {
    size_t i0{0};
    size_t i1{0};
    float frac{0.0f};
    float dt{0.0f};
};

KeyLookup find_keys(const std::vector<float>& times, float t) {
    KeyLookup k;
    if (times.size() < 2) return k;
    if (t <= times.front()) return k;
    if (t >= times.back()) {
        k.i0 = k.i1 = times.size() - 1;
        return k;
    }
    // upper_bound is safe here: the loader keeps sampler inputs in file order
    // and glTF requires them to be strictly increasing.
    const auto it = std::upper_bound(times.begin(), times.end(), t);
    k.i1 = static_cast<size_t>(it - times.begin());
    k.i0 = k.i1 - 1;
    k.dt = times[k.i1] - times[k.i0];
    k.frac = (k.dt > 1e-8f) ? (t - times[k.i0]) / k.dt : 0.0f;
    return k;
}

/** Hermite basis for CUBICSPLINE, straight from the glTF spec's appendix.
 *
 * Tangents are stored scaled by the key delta, so they must be multiplied by
 * it again here; skipping that is the classic cubic-spline glTF bug where
 * animation looks right at the keys and overshoots between them. */
float cubic_component(float v0, float out_tan0, float in_tan1, float v1,
                      float frac, float dt) {
    const float t = frac;
    const float t2 = t * t;
    const float t3 = t2 * t;
    return (2.0f * t3 - 3.0f * t2 + 1.0f) * v0 +
           dt * (t3 - 2.0f * t2 + t) * out_tan0 +
           (-2.0f * t3 + 3.0f * t2) * v1 + dt * (t3 - t2) * in_tan1;
}

void sample_channel(const AnimChannel& ch, float t, JointPose& pose) {
    const size_t comps = (ch.path == AnimPath::Rotation) ? 4u : 3u;
    if (ch.times.empty()) return;

    auto value_at = [&](size_t key, size_t component) -> float {
        // CUBICSPLINE packs [in-tangent, value, out-tangent] per key, so the
        // value sits one stride into the triple.
        const size_t stride =
            (ch.interp == AnimInterp::CubicSpline) ? comps * 3u : comps;
        const size_t offset =
            (ch.interp == AnimInterp::CubicSpline) ? comps : 0u;
        const size_t idx = key * stride + offset + component;
        return (idx < ch.values.size()) ? ch.values[idx] : 0.0f;
    };
    auto tangent_at = [&](size_t key, size_t component, bool out) -> float {
        const size_t stride = comps * 3u;
        const size_t offset = out ? comps * 2u : 0u;
        const size_t idx = key * stride + offset + component;
        return (idx < ch.values.size()) ? ch.values[idx] : 0.0f;
    };

    const KeyLookup k = find_keys(ch.times, t);

    float v[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (ch.interp == AnimInterp::Step || k.i0 == k.i1) {
        for (size_t c = 0; c < comps; ++c) v[c] = value_at(k.i0, c);
    } else if (ch.interp == AnimInterp::CubicSpline) {
        for (size_t c = 0; c < comps; ++c) {
            v[c] = cubic_component(value_at(k.i0, c), tangent_at(k.i0, c, true),
                                   tangent_at(k.i1, c, false),
                                   value_at(k.i1, c), k.frac, k.dt);
        }
    } else {
        // LINEAR. Rotations must slerp, not lerp: componentwise blending of
        // quaternions changes angular speed through the arc and denormalizes
        // the result.
        if (ch.path == AnimPath::Rotation) {
            const Quat q0(value_at(k.i0, 3), value_at(k.i0, 0),
                          value_at(k.i0, 1), value_at(k.i0, 2));
            const Quat q1(value_at(k.i1, 3), value_at(k.i1, 0),
                          value_at(k.i1, 1), value_at(k.i1, 2));
            const Quat q = glm::normalize(glm::slerp(glm::normalize(q0),
                                                     glm::normalize(q1),
                                                     k.frac));
            pose.rotation = q;
            return;
        }
        for (size_t c = 0; c < comps; ++c) {
            const float a = value_at(k.i0, c);
            const float b = value_at(k.i1, c);
            v[c] = a + (b - a) * k.frac;
        }
    }

    switch (ch.path) {
        case AnimPath::Translation:
            pose.translation = Vec3(v[0], v[1], v[2]);
            break;
        case AnimPath::Scale:
            pose.scale = Vec3(v[0], v[1], v[2]);
            break;
        case AnimPath::Rotation:
            // glTF orders quaternion components x, y, z, w.
            pose.rotation = glm::normalize(Quat(v[3], v[0], v[1], v[2]));
            break;
    }
}

}  // namespace

void sample_animation_clip(const gltf_mesh_load::AnimationClip& clip,
                           float time_s,
                           const std::vector<std::uint8_t>& overridden,
                           std::vector<JointPose>& out_pose) {
    for (const auto& ch : clip.channels) {
        if (ch.joint < 0 ||
            static_cast<size_t>(ch.joint) >= out_pose.size()) {
            continue;
        }
        const size_t j = static_cast<size_t>(ch.joint);
        if (j < overridden.size() && overridden[j]) continue;
        sample_channel(ch, time_s, out_pose[j]);
    }
}

void tick_animators(World& world, float dt) {
    AnimationLod off{};
    off.enabled = false;
    tick_animators(world, dt, Vec3(0.0f), off);
}

void tick_animators(World& world, float dt, const Vec3& viewer,
                    const AnimationLod& lod) {
    // Scratch pose for the outgoing clip during a cross-fade. Function-static
    // so a scene full of characters reuses one buffer instead of allocating a
    // joint array per animator per frame.
    static std::vector<JointPose> fade_pose;
    // Advances once per call, so the "every other frame" and "every fourth
    // frame" bands stay in step across the whole scene rather than each
    // animator keeping its own counter.
    static std::uint64_t frame_index = 0;
    ++frame_index;

    auto view = world.registry().view<CSkeleton>();
    for (auto entity : view) {
        auto& sk = view.get<CSkeleton>(entity);
        const size_t n = sk.skeleton.joint_count();
        if (n == 0) continue;
        if (sk.local_pose.size() != n) sk.local_pose = sk.skeleton.bind_pose;

        CAnimator* anim = world.registry().try_get<CAnimator>(entity);

        // --- Distance throttling --------------------------------------
        //
        // Skipped frames bank their dt as debt and hand it back the next
        // time this animator is sampled, so a distant character covers the
        // same ground per second as a near one. Only the smoothness drops.
        float step = dt;
        if (lod.enabled && anim != nullptr) {
            const CTransform* ct = world.registry().try_get<CTransform>(entity);
            if (ct != nullptr) {
                const Vec3 d = ct->t.position - viewer;
                const float dist = std::sqrt(glm::dot(d, d));
                int stride = 1;
                if (dist > lod.stop_m) {
                    stride = 0;  // out of range entirely
                } else if (dist > lod.half_rate_m) {
                    stride = 4;
                } else if (dist > lod.full_rate_m) {
                    stride = 2;
                }

                if (stride == 0) {
                    // Hold the last pose and keep the palette as it is. The
                    // debt still accrues, so walking back into range does not
                    // rewind the animation.
                    anim->lod_debt_s += dt;
                    continue;
                }
                if (stride > 1 &&
                    (frame_index % static_cast<std::uint64_t>(stride)) != 0) {
                    anim->lod_debt_s += dt;
                    continue;
                }
                step = dt + anim->lod_debt_s;
                anim->lod_debt_s = 0.0f;
            }
        }
        // Size the flags before anything reads them, or the first frame after
        // an animator is attached would treat a freshly written override as
        // absent and stamp the bind pose over it.
        if (anim && anim->joint_overridden.size() != n) {
            anim->joint_overridden.resize(n, 0u);
        }

        // Start from bind every frame. Without this an override that stops
        // being written would freeze at its last value instead of releasing,
        // and a clip that animates only some joints would inherit whatever
        // the previous clip left behind.
        for (size_t j = 0; j < n; ++j) {
            const bool overridden =
                anim && j < anim->joint_overridden.size() &&
                anim->joint_overridden[j];
            if (!overridden) sk.local_pose[j] = sk.skeleton.bind_pose[j];
        }

        if (anim) {
            // --- Outgoing clip -------------------------------------------
            //
            // Sampled BEFORE the incoming one, into scratch, and advanced on
            // its own clock: blending a moving pose against a frozen one
            // still reads as a hitch, just a softer one.
            const bool fading =
                anim->prev_clip >= 0 &&
                static_cast<size_t>(anim->prev_clip) < anim->clips.size() &&
                anim->blend_duration > 1e-6f && anim->blend_t > 0.0f;
            if (fading) {
                const auto& pclip =
                    anim->clips[static_cast<size_t>(anim->prev_clip)];
                anim->prev_time_s += step * anim->speed;
                if (pclip.duration > 1e-6f) {
                    anim->prev_time_s =
                        std::fmod(anim->prev_time_s, pclip.duration);
                    if (anim->prev_time_s < 0.0f)
                        anim->prev_time_s += pclip.duration;
                }
                fade_pose.assign(sk.skeleton.bind_pose.begin(),
                                 sk.skeleton.bind_pose.end());
                sample_animation_clip(pclip, anim->prev_time_s,
                                      anim->joint_overridden, fade_pose);
            }

            // --- Incoming clip -------------------------------------------
            if (anim->active_clip >= 0 &&
                static_cast<size_t>(anim->active_clip) < anim->clips.size()) {
                const auto& clip =
                    anim->clips[static_cast<size_t>(anim->active_clip)];
                if (anim->playing) {
                    anim->time_s += step * anim->speed;
                    if (clip.duration > 1e-6f) {
                        if (anim->looping) {
                            anim->time_s = std::fmod(anim->time_s,
                                                     clip.duration);
                            if (anim->time_s < 0.0f)
                                anim->time_s += clip.duration;
                        } else if (anim->time_s >= clip.duration) {
                            anim->time_s = clip.duration;
                            anim->playing = false;
                        }
                    }
                }
                sample_animation_clip(clip, anim->time_s,
                                      anim->joint_overridden, sk.local_pose);
            }

            // --- Cross-fade ----------------------------------------------
            //
            // ``w`` is the weight still owed to the OUTGOING clip, so it runs
            // 1 -> 0 across the fade. With no incoming clip the target is the
            // bind pose, which makes a faded stop_clip fall back to rest
            // instead of snapping there.
            if (fading && fade_pose.size() == n) {
                const float w = std::clamp(
                    anim->blend_t / anim->blend_duration, 0.0f, 1.0f);
                for (size_t j = 0; j < n; ++j) {
                    if (j < anim->joint_overridden.size() &&
                        anim->joint_overridden[j]) {
                        continue;
                    }
                    JointPose& out = sk.local_pose[j];
                    const JointPose& prev = fade_pose[j];
                    out.translation =
                        glm::mix(out.translation, prev.translation, w);
                    out.scale = glm::mix(out.scale, prev.scale, w);
                    // glm::slerp picks the shortest arc, so opposite-sign
                    // quaternions do not blend the long way round.
                    out.rotation = glm::normalize(
                        glm::slerp(out.rotation, prev.rotation, w));
                }
                anim->blend_t = std::max(0.0f, anim->blend_t - step);
                if (anim->blend_t <= 0.0f) {
                    anim->prev_clip = -1;
                    anim->blend_duration = 0.0f;
                }
            } else if (anim->prev_clip >= 0) {
                anim->prev_clip = -1;
                anim->blend_t = 0.0f;
                anim->blend_duration = 0.0f;
            }
        }

        gltf_mesh_load::build_joint_palette(sk.skeleton, sk.local_pose,
                                            sk.palette);

        // --- Second palette for the player's own view -------------------
        //
        // Scaling the joints in the LOCAL pose and rebuilding is deliberate:
        // the collapse then happens about each joint's own origin and the
        // children follow automatically, which is what makes a hidden head
        // disappear at the neck instead of leaving a sliver at the feet.
        // Doing it here rather than in the renderer also keeps all pose math
        // in the pose system.
        if (auto* hide = world.registry().try_get<CSelfViewHide>(entity);
            hide != nullptr && !hide->joints.empty() && hide->scale < 1.0f) {
            static std::vector<JointPose> self_view_pose;
            self_view_pose = sk.local_pose;
            const float s = std::max(0.0f, hide->scale);
            for (const int j : hide->joints) {
                if (j < 0 || static_cast<size_t>(j) >= n) continue;
                self_view_pose[static_cast<size_t>(j)].scale *= s;
            }
            gltf_mesh_load::build_joint_palette(sk.skeleton, self_view_pose,
                                                sk.palette_self_view);
        } else if (!sk.palette_self_view.empty()) {
            // Toggled back on: drop the variant so the renderer falls back to
            // the one palette rather than keeping a stale collapsed copy.
            sk.palette_self_view.clear();
        }

        // Overrides are per-frame statements, not sticky state: a mechanic
        // that stops driving a joint hands it straight back to the clip.
        if (anim) {
            std::fill(anim->joint_overridden.begin(),
                      anim->joint_overridden.end(), std::uint8_t{0});
        }
    }
}

}  // namespace ptv
