#pragma once

// ---------------------------------------------------------------------------
// ptv::InputSnapshot -- the read-only, per-frame controller + headset input
// handed to every mechanic's ``on_update``.
//
// This is the ONLY way generated gameplay code sees live VR input. It contains
// NO OpenXR types (only glm + std), so it stays inside the generated-code
// include whitelist (``ptv/api/*`` + glm + std). The engine fills it every
// frame in Application.cpp from ``XrRuntime::sync_input`` AFTER the rig
// transform is applied, so every pose below is expressed in the SAME
// world/stage space as entity ``CTransform`` components -- generated code can
// compare them directly.
//
// Meta Touch layout note: the A and B face buttons live on the RIGHT
// controller; X and Y live on the LEFT controller. Triggers, grips, and
// thumbsticks exist on both hands and are indexed by ``Hand``.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <string_view>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace ptv {

struct InputSnapshot {
    enum Hand : std::uint32_t { Left = 0, Right = 1 };

    // Sentinel for "this hand is not holding anything" (see HandState::held).
    static constexpr std::uint32_t kInvalidEntity = 0xFFFFFFFFu;

    struct HandState {
        // Analog controls, current frame. ``trigger`` / ``grip`` are 0..1.
        float trigger{0.0f};
        float grip{0.0f};
        glm::vec2 thumbstick{0.0f};

        // Same analog controls from the PREVIOUS frame, for edge detection.
        float prev_trigger{0.0f};
        float prev_grip{0.0f};

        // Capacitive touch ("capsense"): the finger is RESTING on the control
        // without pressing it. This is the difference between an index finger
        // curled in mid-air and one laid along the trigger, and between a
        // thumb up and a thumb down on the rest -- the two poses that make a
        // virtual hand read as alive rather than as two canned states.
        //
        // ``trigger_touched`` is independent of ``trigger``: a resting finger
        // reports touched with a value of 0. False on hardware or runtimes
        // that do not report capsense, which reads as "finger not there".
        bool trigger_touched{false};
        bool thumbrest_touched{false};
        bool prev_trigger_touched{false};
        bool prev_thumbrest_touched{false};
        // Thumb resting on the thumbstick. This is where a Touch player's
        // thumb sits nearly all the time, so it is the default signal for
        // lowering the virtual thumb.
        bool thumbstick_touched{false};
        bool prev_thumbstick_touched{false};
        // Thumbstick pressed IN (L3 on the left, R3 on the right) -- the
        // conventional sprint control.
        bool thumbstick_click{false};
        bool prev_thumbstick_click{false};
        // Thumb resting on either face button of THIS controller (A/B on the
        // right, X/Y on the left).
        bool face_button_touched{false};
        bool prev_face_button_touched{false};

        // Grip ("hand") pose: palm-centric. Use for "where is the hand".
        // Valid only when ``hand_valid`` is true.
        glm::vec3 hand_position{0.0f};
        glm::quat hand_rotation{1.0f, 0.0f, 0.0f, 0.0f};
        bool hand_valid{false};

        // Aim pose: controller-body forward (-Z points where the controller
        // points). Use for rays, muzzle direction, and "which way am I
        // pointing". Valid only when ``aim_valid`` is true.
        glm::vec3 aim_position{0.0f};
        glm::quat aim_rotation{1.0f, 0.0f, 0.0f, 0.0f};
        bool aim_valid{false};

        // Smoothed world-space hand velocity in m/s. Exponentially averaged
        // by the engine, because the raw per-frame difference of Quest 3
        // inside-out tracking is far too noisy to drive a throw. Zero while
        // the hand is untracked.
        glm::vec3 velocity{0.0f};

        // Entity currently held by this hand via the built-in grab system, or
        // ``kInvalidEntity`` when the hand is empty. Lets a weapon/tool
        // mechanic detect "am I being held, and by which hand?".
        std::uint32_t held_entity{kInvalidEntity};
    };

    HandState hands[2]{};

    // Face buttons -- current frame + previous frame (for edge detection).
    bool button_a{false};
    bool button_b{false};
    bool button_x{false};
    bool button_y{false};
    bool prev_button_a{false};
    bool prev_button_b{false};
    bool prev_button_x{false};
    bool prev_button_y{false};
    /** Left controller menu button: the game's pause control, owned by
     *  ``builtin.game_flow`` when the scene has one. */
    bool button_menu{false};
    bool prev_button_menu{false};
    bool menu_pressed() const { return button_menu && !prev_button_menu; }

    // Head (HMD) pose in world/stage space.
    glm::vec3 head_position{0.0f};
    glm::quat head_rotation{1.0f, 0.0f, 0.0f, 0.0f};
    bool head_valid{false};

    // Frame delta in seconds (same value as the ``dt`` arg to on_update).
    float dt{0.0f};

    // ---- Convenience accessors -------------------------------------------

    const HandState& hand(Hand h) const { return hands[h]; }

    // Trigger edge / hold helpers (threshold defaults to 0.5).
    bool trigger_pressed(Hand h, float threshold = 0.5f) const {
        return hands[h].trigger >= threshold && hands[h].prev_trigger < threshold;
    }
    bool trigger_released(Hand h, float threshold = 0.5f) const {
        return hands[h].trigger < threshold && hands[h].prev_trigger >= threshold;
    }
    bool trigger_down(Hand h, float threshold = 0.5f) const {
        return hands[h].trigger >= threshold;
    }

    // Grip edge / hold helpers (threshold defaults to 0.5).
    bool grip_pressed(Hand h, float threshold = 0.5f) const {
        return hands[h].grip >= threshold && hands[h].prev_grip < threshold;
    }
    bool grip_released(Hand h, float threshold = 0.5f) const {
        return hands[h].grip < threshold && hands[h].prev_grip >= threshold;
    }
    bool grip_down(Hand h, float threshold = 0.5f) const {
        return hands[h].grip >= threshold;
    }

    // Capsense helpers. ``trigger_resting`` is the pose case a hand mechanic
    // actually wants: finger ON the trigger but not pulling it.
    bool trigger_touching(Hand h) const { return hands[h].trigger_touched; }
    bool trigger_resting(Hand h, float threshold = 0.5f) const {
        return hands[h].trigger_touched && hands[h].trigger < threshold;
    }
    bool trigger_touch_started(Hand h) const {
        return hands[h].trigger_touched && !hands[h].prev_trigger_touched;
    }
    bool thumbrest_touching(Hand h) const { return hands[h].thumbrest_touched; }
    bool thumbrest_touch_started(Hand h) const {
        return hands[h].thumbrest_touched && !hands[h].prev_thumbrest_touched;
    }
    bool thumbstick_touching(Hand h) const {
        return hands[h].thumbstick_touched;
    }
    bool thumbstick_touch_started(Hand h) const {
        return hands[h].thumbstick_touched && !hands[h].prev_thumbstick_touched;
    }
    bool face_button_touching(Hand h) const {
        return hands[h].face_button_touched;
    }

    // Thumbstick press (L3 / R3): held state and rising edge.
    bool thumbstick_down(Hand h) const { return hands[h].thumbstick_click; }
    bool thumbstick_clicked(Hand h) const {
        return hands[h].thumbstick_click && !hands[h].prev_thumbstick_click;
    }

    // Face-button current-state helpers.
    bool button_a_down() const { return button_a; }
    bool button_b_down() const { return button_b; }
    bool button_x_down() const { return button_x; }
    bool button_y_down() const { return button_y; }

    // Face-button rising-edge helpers (true only on the frame it goes down).
    bool button_a_pressed() const { return button_a && !prev_button_a; }
    bool button_b_pressed() const { return button_b && !prev_button_b; }
    bool button_x_pressed() const { return button_x && !prev_button_x; }
    bool button_y_pressed() const { return button_y && !prev_button_y; }

    // Named-button lookup so a bound mechanic can read the control named in
    // its params (e.g. ``params.value("button","a")``). Accepts "a"/"b"/"x"/
    // "y" and the stick presses "left_thumbstick"/"l3"/"right_thumbstick"/
    // "r3" (case-sensitive lowercase). Unknown names return false.
    bool button_down(std::string_view name) const {
        if (name == "a") return button_a;
        if (name == "b") return button_b;
        if (name == "x") return button_x;
        if (name == "y") return button_y;
        if (name == "left_thumbstick" || name == "l3") {
            return hands[Left].thumbstick_click;
        }
        if (name == "right_thumbstick" || name == "r3") {
            return hands[Right].thumbstick_click;
        }
        return false;
    }
    bool button_pressed(std::string_view name) const {
        if (name == "a") return button_a_pressed();
        if (name == "b") return button_b_pressed();
        if (name == "x") return button_x_pressed();
        if (name == "y") return button_y_pressed();
        if (name == "left_thumbstick" || name == "l3") {
            return thumbstick_clicked(Left);
        }
        if (name == "right_thumbstick" || name == "r3") {
            return thumbstick_clicked(Right);
        }
        return false;
    }

    // Returns the hand (0=Left, 1=Right) currently holding ``entity`` via the
    // grab system, or -1 when no hand holds it. Handy for weapon mechanics:
    // ``int h = input.hand_holding(entity);`` then read ``input.hands[h]``.
    int hand_holding(std::uint32_t entity) const {
        if (entity == kInvalidEntity) return -1;
        if (hands[0].held_entity == entity) return 0;
        if (hands[1].held_entity == entity) return 1;
        return -1;
    }
};

}  // namespace ptv
