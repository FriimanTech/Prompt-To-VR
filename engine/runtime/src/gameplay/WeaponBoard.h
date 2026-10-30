#pragma once

// The engine side of ctx.weapons (ptv/api/Weapons.h): a board the weapon
// built-ins write their state and hits to, and that NPC AI pulls triggers
// through. Plain data; no ticking of its own.

#include <array>
#include <unordered_map>
#include <unordered_set>

#include "ptv/api/MechanicContext.h"

namespace ptv {

class WeaponBoard final : public IWeapons {
public:
    explicit WeaponBoard(IHands** hands) : hands_(hands) {}

    bool state(std::uint32_t weapon, WeaponState* out) const override {
        const auto it = states_.find(weapon);
        if (it == states_.end()) return false;
        if (out != nullptr) *out = it->second;
        return true;
    }

    std::uint32_t held_weapon(int hand) const override {
        if (hands_ == nullptr || *hands_ == nullptr || hand < 0 || hand > 1) return kNone;
        const std::uint32_t e = (*hands_)->held_entity(hand);
        return states_.count(e) != 0 ? e : kNone;
    }

    std::uint64_t hit_seq() const override { return seq_; }

    int hits_since(std::uint64_t after_seq, WeaponHit* out, int max_out) const override {
        if (out == nullptr || max_out <= 0 || seq_ <= after_seq) return 0;
        const std::uint64_t cap = hits_.size();
        const std::uint64_t first =
            std::max<std::uint64_t>(after_seq + 1, seq_ > cap ? seq_ - cap + 1 : 1);
        int n = 0;
        for (std::uint64_t s = first; s <= seq_ && n < max_out; ++s) {
            out[n++] = hits_[static_cast<std::size_t>((s - 1) % cap)];
        }
        return n;
    }

    void trigger(std::uint32_t weapon, bool pressed) override {
        if (pressed) {
            npc_triggers_.insert(weapon);
        } else {
            npc_triggers_.erase(weapon);
        }
    }

    bool fire_once(std::uint32_t weapon) override {
        if (states_.count(weapon) == 0) return false;
        fire_requests_.insert(weapon);
        return true;
    }

    void aim_at(std::uint32_t weapon, const Vec3& point, bool enabled) override {
        if (enabled) {
            aims_[weapon] = point;
        } else {
            aims_.erase(weapon);
        }
    }

    bool aim_point(std::uint32_t weapon, Vec3* out) const override {
        const auto it = aims_.find(weapon);
        if (it == aims_.end()) return false;
        if (out != nullptr) *out = it->second;
        return true;
    }

    void publish_state(std::uint32_t weapon, const WeaponState& s) override {
        states_[weapon] = s;
    }

    void publish_hit(const WeaponHit& hit) override {
        WeaponHit h = hit;
        h.seq = ++seq_;
        hits_[static_cast<std::size_t>((h.seq - 1) % hits_.size())] = h;
    }

    bool npc_trigger(std::uint32_t weapon) const override {
        return npc_triggers_.count(weapon) != 0;
    }

    bool take_fire_request(std::uint32_t weapon) override {
        return fire_requests_.erase(weapon) != 0;
    }

    void forget(std::uint32_t weapon) {
        states_.erase(weapon);
        npc_triggers_.erase(weapon);
        fire_requests_.erase(weapon);
    }

    void clear() {
        states_.clear();
        npc_triggers_.clear();
        fire_requests_.clear();
        aims_.clear();
        seq_ = 0;
    }

private:
    static constexpr std::uint32_t kNone = 0xFFFFFFFFu;
    IHands** hands_{nullptr};
    std::unordered_map<std::uint32_t, WeaponState> states_;
    std::unordered_set<std::uint32_t> npc_triggers_;
    std::unordered_set<std::uint32_t> fire_requests_;
    std::unordered_map<std::uint32_t, Vec3> aims_;
    std::array<WeaponHit, 64> hits_{};
    std::uint64_t seq_{0};
};

}  // namespace ptv
