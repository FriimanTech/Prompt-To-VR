#pragma once

// Sizing the player's avatar to the player, from their standing eye height.
//
// The first estimate used to be the median of the first 0.6 s of head
// poses, half a second after launch -- often while the headset was still
// being put on, or the player was seated. One run measured 1.11 m and shrank
// the avatar to 75 %, so its shoulders sat 30 cm below the real ones and
// every reach stretched the arm to its limit. Now:
//
//   * the first calibration waits for the head to be STILL (spread under
//     2 cm over 0.6 s) and for a standing height (1.25 m or more); after 10 s
//     without one it takes what it has, so a seated player still gets a body;
//   * standing up later re-calibrates UPWARD once the taller height has
//     held for 1.5 s, blended in over about half a second;
//   * crouching never shrinks the avatar -- the body sinks with the player
//     instead (see plant_under_head).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace ptv::avatar_body {

struct HeightCalibrationConfig {
    float settle_s{0.5f};            // ignore the first moments of tracking
    float stable_window_s{0.6f};
    float stable_spread_m{0.02f};
    float min_standing_eye_m{1.25f};
    float first_timeout_s{10.0f};
    float rise_ratio{1.05f};         // re-calibrate when this much taller...
    float rise_hold_s{1.5f};         // ...for this long
    float blend_s{0.5f};
    float min_scale{0.75f};
    float max_scale{1.3f};
};

class HeightCalibration {
public:
    enum class Event { None, First, Fallback, StoodTaller };

    explicit HeightCalibration(HeightCalibrationConfig cfg = {}) : cfg_(cfg) {}

    void reset() {
        count_ = 0;
        head_ = 0;
        elapsed_ = 0.0f;
        rise_timer_ = 0.0f;
        calibrated_ = false;
        eye_ = 0.0f;
        target_ = 1.0f;
        scale_ = 1.0f;
    }

    /** One head sample: ``user_eye_m`` is the headset's height above the
     *  player's feet, ``avatar_eye_m`` the avatar's eye height above its
     *  soles at its unscaled size. Returns what changed, if anything. */
    Event update(float user_eye_m, float avatar_eye_m, float dt) {
        Event ev = Event::None;
        dt = std::isfinite(dt) ? std::max(0.0f, dt) : 0.0f;
        elapsed_ += dt;
        const bool usable = std::isfinite(user_eye_m) && user_eye_m > 0.3f &&
                            std::isfinite(avatar_eye_m) && avatar_eye_m > 0.3f;
        if (usable && elapsed_ >= cfg_.settle_s) push(elapsed_, user_eye_m);

        float median = 0.0f;
        const bool stable = usable && stable_median(median);
        if (!calibrated_) {
            if (stable && median >= cfg_.min_standing_eye_m) {
                set_target(median, avatar_eye_m, /*snap=*/true);
                ev = Event::First;
            } else if (elapsed_ >= cfg_.first_timeout_s && count_ > 0) {
                set_target(window_median(), avatar_eye_m, /*snap=*/true);
                ev = Event::Fallback;
            }
        } else if (stable && median > eye_ * cfg_.rise_ratio) {
            rise_timer_ += dt;
            if (rise_timer_ >= cfg_.rise_hold_s) {
                set_target(median, avatar_eye_m, /*snap=*/false);
                rise_timer_ = 0.0f;
                ev = Event::StoodTaller;
            }
        } else {
            rise_timer_ = 0.0f;
        }

        if (calibrated_ && cfg_.blend_s > 1e-4f) {
            const float tau = cfg_.blend_s / 3.0f;  // ~95 % within blend_s
            scale_ += (target_ - scale_) * (1.0f - std::exp(-dt / tau));
        } else if (calibrated_) {
            scale_ = target_;
        }
        return ev;
    }

    bool calibrated() const { return calibrated_; }
    /** The eye height the current target was taken from (m). */
    float calibrated_eye() const { return eye_; }
    float target_scale() const { return target_; }
    /** The scale to apply this frame (blending towards the target). */
    float scale() const { return scale_; }

private:
    static constexpr std::size_t kCap = 256;

    void push(float t, float v) {
        times_[head_] = t;
        values_[head_] = v;
        head_ = (head_ + 1) % kCap;
        count_ = std::min(count_ + 1, kCap);
    }

    /** Median of the samples within the stability window, if they span the
     *  whole window and agree within the spread. */
    bool stable_median(float& out) const {
        if (count_ < 3) return false;
        const float now = times_[(head_ + kCap - 1) % kCap];
        float lo = 1e9f, hi = -1e9f, oldest = now;
        std::array<float, kCap> w{};
        std::size_t n = 0;
        for (std::size_t i = 0; i < count_; ++i) {
            const std::size_t k = (head_ + kCap - 1 - i) % kCap;
            if (now - times_[k] > cfg_.stable_window_s) break;
            oldest = times_[k];
            lo = std::min(lo, values_[k]);
            hi = std::max(hi, values_[k]);
            w[n++] = values_[k];
        }
        // Allow one frame of slack: samples land on frame boundaries.
        if (n < 3 || now - oldest < cfg_.stable_window_s * 0.9f) return false;
        if (hi - lo > cfg_.stable_spread_m) return false;
        std::nth_element(w.begin(), w.begin() + n / 2, w.begin() + n);
        out = w[n / 2];
        return true;
    }

    float window_median() const {
        std::array<float, kCap> w{};
        const std::size_t n = count_;
        for (std::size_t i = 0; i < n; ++i) w[i] = values_[i];
        std::nth_element(w.begin(), w.begin() + n / 2, w.begin() + n);
        return w[n / 2];
    }

    void set_target(float eye, float avatar_eye, bool snap) {
        eye_ = eye;
        target_ = std::clamp(eye / avatar_eye, cfg_.min_scale, cfg_.max_scale);
        if (snap) scale_ = target_;
        calibrated_ = true;
    }

    HeightCalibrationConfig cfg_;
    std::array<float, kCap> times_{};
    std::array<float, kCap> values_{};
    std::size_t head_{0};
    std::size_t count_{0};
    float elapsed_{0.0f};
    float rise_timer_{0.0f};
    bool calibrated_{false};
    float eye_{0.0f};
    float target_{1.0f};
    float scale_{1.0f};
};

}  // namespace ptv::avatar_body
