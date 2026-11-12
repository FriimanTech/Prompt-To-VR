#pragma once

// The scene's shared boards: named signals, live values, and the anchors the
// world declared. Owned by Application, reset on every scene load, exposed to
// mechanics as ctx.signals / ctx.vars / ctx.anchors.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "ptv/api/MechanicContext.h"
#include "ptv/scene/Scene.h"

namespace ptv {

class World;

class SignalBoard final : public ISignals {
public:
    void emit(const char* name) override;
    std::uint32_t count(const char* name) const override;
    void reset() { counts_.clear(); }

private:
    std::unordered_map<std::string, std::uint32_t> counts_;
};

class VarBoard final : public IVars {
public:
    void set_number(const char* name, float value) override;
    void set_text(const char* name, const char* value) override;
    float number(const char* name, float fallback = 0.0f) const override;
    const char* text(const char* name) const override;
    bool has(const char* name) const override;
    std::uint32_t seq(const char* name) const override;
    void reset() { vars_.clear(); }

private:
    struct Var {
        bool is_number{false};
        float number{0.0f};
        std::string text;
        std::uint32_t seq{0};
    };
    std::unordered_map<std::string, Var> vars_;
    mutable std::string scratch_;
};

class AnchorBoard final : public IAnchors {
public:
    void bind(const std::vector<AnchorSpec>* anchors,
              const std::vector<AnchorSetSpec>* sets, const World* world) {
        anchors_ = anchors;
        sets_ = sets;
        world_ = world;
    }
    int count(const char* set_id) const override;
    bool point(const char* set_id, int index, Vec3* position, Vec3* facing) const override;
    const char* relation(const char* set_id) const override;
    bool anchor(const char* id, Vec3* position, Vec3* facing) const override;
    bool pose_at(std::uint32_t entity, const char* set_id, int index, Vec3* position,
                 Quat* rotation) const override;

private:
    const AnchorSetSpec* find_set(const char* set_id) const;

    const std::vector<AnchorSpec>* anchors_{nullptr};
    const std::vector<AnchorSetSpec>* sets_{nullptr};
    const World* world_{nullptr};
};

}  // namespace ptv
