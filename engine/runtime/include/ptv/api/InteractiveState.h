#pragma once

// ---------------------------------------------------------------------------
// ptv::InteractiveValue -- one value of declared, changeable world state.
//
// Shared by the ECS component that holds it and the service that mutates it,
// which is why it lives in its own header rather than in either.
//
// It is a small variant, not a bool, because the things a world actually
// contains disagree about shape: a traffic light is enumerated, a door is a
// continuous angle, an elevator is a named floor, a switch is a flag. Forcing
// them all into a boolean is what pushes state into the IDENTIFIER instead --
// and an opened ``door_closed_1`` is then a name that both contradicts
// reality and is the key its saves are written under.
//
// Mirrors ptv::world::StateValue in ptv/api/IWorldBuilder.h. They are
// deliberately separate types: the world-builder surface is compiled OFFLINE
// in ptv_worldgen, which links none of the engine, so it cannot share an
// engine header. The JSON they meet in is the contract between them.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <string>

#include "ptv/math/Math.h"

namespace ptv {

struct InteractiveValue {
    enum class Type : std::uint8_t { Bool, Number, Name, Vector };

    Type type{Type::Bool};
    bool b{false};
    float number{0.0f};
    std::string name;  // "closed", "green", "floor_3"
    Vec3 vector{0.0f, 0.0f, 0.0f};

    static InteractiveValue of(bool v) {
        InteractiveValue s{};
        s.type = Type::Bool;
        s.b = v;
        return s;
    }
    static InteractiveValue of(float v) {
        InteractiveValue s{};
        s.type = Type::Number;
        s.number = v;
        return s;
    }
    static InteractiveValue named(std::string v) {
        InteractiveValue s{};
        s.type = Type::Name;
        s.name = std::move(v);
        return s;
    }
    static InteractiveValue of(const Vec3& v) {
        InteractiveValue s{};
        s.type = Type::Vector;
        s.vector = v;
        return s;
    }

    /** Value equality, used to decide whether state has DIVERGED from the
     *  pristine document value. That comparison is the whole persistence
     *  mechanism: only the difference is saved, so a world reloads from its
     *  unchanged document plus a small set of deltas. */
    bool operator==(const InteractiveValue& o) const {
        if (type != o.type) return false;
        switch (type) {
            case Type::Bool: return b == o.b;
            case Type::Number:
                // Tolerance rather than exact equality: a door driven to its
                // closed angle by a physics-ish curve lands near 0, not on
                // it, and saving that as a change would grow the save file
                // with noise on every door in the level.
                return std::fabs(number - o.number) < 1.0e-4f;
            case Type::Name: return name == o.name;
            case Type::Vector:
            default:
                return glm::length(vector - o.vector) < 1.0e-4f;
        }
    }
    bool operator!=(const InteractiveValue& o) const { return !(*this == o); }
};

}  // namespace ptv
