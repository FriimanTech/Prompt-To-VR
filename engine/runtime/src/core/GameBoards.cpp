#include "GameBoards.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "ptv/api/Facing.h"
#include "ptv/ecs/World.h"

namespace ptv {

// --- Signals ---------------------------------------------------------------

void SignalBoard::emit(const char* name) {
    if (name == nullptr || *name == '\0') return;
    ++counts_[name];
}

std::uint32_t SignalBoard::count(const char* name) const {
    if (name == nullptr) return 0;
    const auto it = counts_.find(name);
    return it == counts_.end() ? 0u : it->second;
}

// --- Vars --------------------------------------------------------------------

void VarBoard::set_number(const char* name, float value) {
    if (name == nullptr || *name == '\0' || !std::isfinite(value)) return;
    Var& v = vars_[name];
    if (v.is_number && v.number == value && v.seq != 0) return;
    v.is_number = true;
    v.number = value;
    v.text.clear();
    ++v.seq;
}

void VarBoard::set_text(const char* name, const char* value) {
    if (name == nullptr || *name == '\0') return;
    Var& v = vars_[name];
    const std::string s = value != nullptr ? value : "";
    if (!v.is_number && v.text == s && v.seq != 0) return;
    v.is_number = false;
    v.text = s;
    ++v.seq;
}

float VarBoard::number(const char* name, float fallback) const {
    if (name == nullptr) return fallback;
    const auto it = vars_.find(name);
    if (it == vars_.end() || !it->second.is_number) return fallback;
    return it->second.number;
}

const char* VarBoard::text(const char* name) const {
    scratch_.clear();
    if (name == nullptr) return scratch_.c_str();
    const auto it = vars_.find(name);
    if (it == vars_.end()) return scratch_.c_str();
    if (!it->second.is_number) {
        scratch_ = it->second.text;
        return scratch_.c_str();
    }
    char buf[48];
    const float n = it->second.number;
    if (std::abs(n - std::round(n)) < 1e-4f) {
        std::snprintf(buf, sizeof(buf), "%.0f", n);
    } else {
        std::snprintf(buf, sizeof(buf), "%.2f", n);
    }
    scratch_ = buf;
    return scratch_.c_str();
}

bool VarBoard::has(const char* name) const {
    return name != nullptr && vars_.count(name) != 0;
}

std::uint32_t VarBoard::seq(const char* name) const {
    if (name == nullptr) return 0;
    const auto it = vars_.find(name);
    return it == vars_.end() ? 0u : it->second.seq;
}

// --- Anchors -------------------------------------------------------------

const AnchorSetSpec* AnchorBoard::find_set(const char* set_id) const {
    if (sets_ == nullptr || set_id == nullptr) return nullptr;
    for (const auto& s : *sets_) {
        if (s.id == set_id) return &s;
    }
    for (const auto& s : *sets_) {
        if (std::find(s.tags.begin(), s.tags.end(), set_id) != s.tags.end()) return &s;
    }
    return nullptr;
}

int AnchorBoard::count(const char* set_id) const {
    const AnchorSetSpec* s = find_set(set_id);
    return s == nullptr ? 0 : static_cast<int>(s->points.size());
}

bool AnchorBoard::point(const char* set_id, int index, Vec3* position, Vec3* facing) const {
    const AnchorSetSpec* s = find_set(set_id);
    if (s == nullptr || index < 0 || index >= static_cast<int>(s->points.size())) return false;
    if (position != nullptr) *position = s->points[static_cast<std::size_t>(index)];
    if (facing != nullptr) *facing = s->facing;
    return true;
}

const char* AnchorBoard::relation(const char* set_id) const {
    const AnchorSetSpec* s = find_set(set_id);
    return s == nullptr ? "" : s->relation.c_str();
}

bool AnchorBoard::anchor(const char* id, Vec3* position, Vec3* facing) const {
    if (anchors_ == nullptr || id == nullptr) return false;
    for (const auto& a : *anchors_) {
        if (a.id != id) continue;
        if (position != nullptr) *position = a.position;
        if (facing != nullptr) *facing = a.facing;
        return true;
    }
    return false;
}

bool AnchorBoard::pose_at(std::uint32_t entity, const char* set_id, int index,
                          Vec3* position, Quat* rotation) const {
    Vec3 p(0.0f), f(0.0f, 0.0f, -1.0f);
    if (!point(set_id, index, &p, &f)) return false;
    const std::string rel = relation(set_id);
    Quat q(1.0f, 0.0f, 0.0f, 0.0f);
    if (glm::length(Vec3(f.x, 0.0f, f.z)) > 1e-4f) q = rotation_facing(Vec3(f.x, 0.0f, f.z));
    Vec3 out = p;
    if (world_ != nullptr && world_->valid(entity) && world_->has<CCollisionGeom>(entity)) {
        const auto& g = world_->get<CCollisionGeom>(entity);
        const float s = world_->has<CTransform>(entity) ? world_->get<CTransform>(entity).t.scale
                                                         : 1.0f;
        Vec3 lo(1e30f), hi(-1e30f);
        for (int i = 0; i < 8; ++i) {
            const Vec3 c((i & 1) ? g.aabb_max.x : g.aabb_min.x,
                         (i & 2) ? g.aabb_max.y : g.aabb_min.y,
                         (i & 4) ? g.aabb_max.z : g.aabb_min.z);
            const Vec3 r = q * (c * s);
            lo = glm::min(lo, r);
            hi = glm::max(hi, r);
        }
        const Vec3 c = 0.5f * (lo + hi);
        if (rel == "on_floor" || rel == "on_surface") {
            out = Vec3(p.x - c.x, p.y - lo.y + 0.002f, p.z - c.z);
        } else if (rel == "hang_from") {
            out = Vec3(p.x - c.x, p.y - hi.y, p.z - c.z);
        }
    }
    if (position != nullptr) *position = out;
    if (rotation != nullptr) *rotation = q;
    return true;
}

}  // namespace ptv
