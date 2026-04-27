#pragma once

#include <entt/entt.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ptv/math/Math.h"

namespace ptv {

class IMechanic;

/** Lightweight wrapper around an EnTT registry + stable ids. */
class World {
public:
    World() = default;

    entt::registry& registry() { return registry_; }
    const entt::registry& registry() const { return registry_; }

    entt::entity create_entity(const std::string& id = {}) {
        auto e = registry_.create();
        if (!id.empty()) id_to_entity_[id] = e;
        return e;
    }

    entt::entity find(const std::string& id) const {
        auto it = id_to_entity_.find(id);
        return it == id_to_entity_.end() ? entt::null : it->second;
    }

    float delta_time() const { return dt_; }
    void set_delta_time(float dt) { dt_ = dt; }

    // ---- uint32_t-safe component helpers for generated code ---------------
    //
    // IMechanic's interface passes entities as std::uint32_t (a stable ABI
    // for generated code). entt::registry wants ``entt::entity``. Wrapping
    // the cast once here means mechanics never have to write
    // ``static_cast<entt::entity>(e)`` and can't accidentally truncate.

    template <class C>
    bool has(std::uint32_t e) const {
        return registry_.all_of<C>(static_cast<entt::entity>(e));
    }

    template <class... Cs>
    bool has_all(std::uint32_t e) const {
        return registry_.all_of<Cs...>(static_cast<entt::entity>(e));
    }

    template <class C>
    C& get(std::uint32_t e) {
        return registry_.get<C>(static_cast<entt::entity>(e));
    }

    template <class C>
    const C& get(std::uint32_t e) const {
        return registry_.get<C>(static_cast<entt::entity>(e));
    }

    template <class C, class... Args>
    C& emplace_or_replace(std::uint32_t e, Args&&... args) {
        return registry_.emplace_or_replace<C>(
            static_cast<entt::entity>(e), std::forward<Args>(args)...);
    }

    template <class C>
    void remove(std::uint32_t e) {
        registry_.remove<C>(static_cast<entt::entity>(e));
    }

    bool valid(std::uint32_t e) const {
        return registry_.valid(static_cast<entt::entity>(e));
    }

private:
    entt::registry registry_{};
    std::unordered_map<std::string, entt::entity> id_to_entity_{};
    float dt_{0.0f};
};

// ---- Core components --------------------------------------------------

struct CTransform {
    Transform t;
};

struct CMesh {
    std::string asset_id;
    std::string glb_path;
    float target_scale_meters{1.0f};
};

struct CPhysics {
    std::string kind{"static"};
    float mass_kg{1.0f};
    float restitution{0.2f};
    float friction{0.5f};
    std::string collision_shape{"aabb"};
};

struct CMechanicBinding {
    struct Entry {
        std::string id;              // fully-qualified mechanic id
        std::string builtin_id;      // populated for builtin / builtin+config
        std::string params_json;     // serialized params for deterministic re-init
    };
    std::vector<Entry> entries;
};

struct CTag {
    std::vector<std::string> tags;
};

}  // namespace ptv
