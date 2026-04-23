#pragma once

#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

namespace ptv {

class World;

/** AI / NPC behaviour interface. */
class IBehaviour {
public:
    virtual ~IBehaviour() = default;
    virtual void on_attach(World& world, std::uint32_t entity, const nlohmann::json& params) = 0;
    virtual void on_tick(World& world, std::uint32_t entity, float dt) = 0;
    virtual void on_detach(World& world, std::uint32_t entity) = 0;
    virtual std::string id() const = 0;
};

using BehaviourFactory = std::function<std::unique_ptr<IBehaviour>()>;

bool register_behaviour(const std::string& id, BehaviourFactory factory);

#define PTV_REGISTER_BEHAVIOUR(id_literal, TypeName)                                           \
    namespace {                                                                                \
        const bool _ptv_regb_##TypeName = ::ptv::register_behaviour(                           \
            id_literal, [] { return std::unique_ptr<::ptv::IBehaviour>(new TypeName()); });    \
    }

}  // namespace ptv
