#pragma once

// ctx.weapons: the state of every weapon builtin.weapon / builtin.bow /
// builtin.melee_weapon runs, and every hit they land.
//
// Generated code READS this (an ammo counter, a hit marker, a custom on-hit
// effect -- freeze the target, open a portal where the shot landed) and may
// pull a weapon's trigger for an NPC that wields it. It never fires a weapon
// itself, keeps its ammo or applies its damage: the built-in does, through
// ICombat, so an enemy hurt by one game's pistol and another's sword is
// hurt the same way.

#include <cstdint>

#include "ptv/math/Math.h"

namespace ptv {

enum class WeaponDelivery : int {
    Hitscan = 0,
    Projectile = 1,
    Hybrid = 2,
    Beam = 3,
    Melee = 4,
    Drawn = 5,
};

struct WeaponState {
    bool configured{false};
    WeaponDelivery delivery{WeaponDelivery::Hitscan};
    int ammo_in_magazine{0};
    int magazine_size{0};
    int reserve{0};
    bool infinite{false};
    bool reloading{false};
    /** 0..1 while reloading. */
    float reload_progress{0.0f};
    /** 0..1 for weapons that overheat; 1 = overheated until it vents. */
    float heat{0.0f};
    bool overheated{false};
    /** Increments every shot / swing. */
    std::uint64_t fire_seq{0};
    /** Hand holding it (0 left, 1 right), -1 when not held by the player. */
    int held_by_hand{-1};
    /** The entity firing it (the player, or the NPC wielding it). */
    std::uint32_t wielder{0xFFFFFFFFu};
    /** Where and which way the last shot left (world space). */
    Vec3 last_origin{0.0f};
    Vec3 last_direction{0.0f, 0.0f, -1.0f};
};

struct WeaponHit {
    std::uint64_t seq{0};
    std::uint32_t weapon{0xFFFFFFFFu};
    std::uint32_t shooter{0xFFFFFFFFu};
    /** The entity struck (its health owner), or invalid for geometry. */
    std::uint32_t target{0xFFFFFFFFu};
    Vec3 point{0.0f};
    Vec3 normal{0.0f, 1.0f, 0.0f};
    float damage{0.0f};
    bool killed{false};
    WeaponDelivery delivery{WeaponDelivery::Hitscan};
};

class IWeapons {
public:
    virtual ~IWeapons() = default;

    /** State of ``weapon``; false when no weapon built-in runs it. */
    virtual bool state(std::uint32_t weapon, WeaponState* out) const = 0;
    /** The weapon the player holds in ``hand``, or invalid. */
    virtual std::uint32_t held_weapon(int hand) const = 0;
    /** Monotonic hit counter; compare with your last seen value. */
    virtual std::uint64_t hit_seq() const = 0;
    /** Up to ``max_out`` hits after ``after_seq``, oldest first. */
    virtual int hits_since(std::uint64_t after_seq, WeaponHit* out, int max_out) const = 0;

    /** Hold / release the trigger of a weapon an NPC wields (generated AI,
     *  builtin.npc.ranged_attack). Ignored for a weapon the player holds. */
    virtual void trigger(std::uint32_t weapon, bool pressed) = 0;
    /** One shot (or swing) now, if the weapon can fire. */
    virtual bool fire_once(std::uint32_t weapon) = 0;
    /** Aim an NPC-wielded weapon at a world point: shots leave the muzzle
     *  toward it instead of along a muzzle the animation swings around.
     *  ``enabled`` false returns it to the muzzle's own direction. */
    virtual void aim_at(std::uint32_t weapon, const Vec3& point, bool enabled) = 0;
    virtual bool aim_point(std::uint32_t weapon, Vec3* out) const = 0;

    // --- Engine-internal: the weapon built-ins report through these. ------
    virtual void publish_state(std::uint32_t weapon, const WeaponState& state) = 0;
    virtual void publish_hit(const WeaponHit& hit) = 0;
    /** Pending NPC trigger state and one-shot requests for ``weapon``. */
    virtual bool npc_trigger(std::uint32_t weapon) const = 0;
    virtual bool take_fire_request(std::uint32_t weapon) = 0;
};

}  // namespace ptv
