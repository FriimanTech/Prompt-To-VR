#pragma once

#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

namespace ptv {

class World;

/** Dialogue hook interface; invoked by builtin.dialogue_trigger. */
class IDialogueHook {
public:
    virtual ~IDialogueHook() = default;
    virtual void on_enter(World& world, std::uint32_t entity, const nlohmann::json& params) = 0;
    virtual void on_line(World& world, std::uint32_t entity, const std::string& line, const nlohmann::json& meta) = 0;
    virtual void on_exit(World& world, std::uint32_t entity) = 0;
    virtual std::string id() const = 0;
};

using DialogueHookFactory = std::function<std::unique_ptr<IDialogueHook>()>;

bool register_dialogue_hook(const std::string& id, DialogueHookFactory factory);

#define PTV_REGISTER_DIALOGUE_HOOK(id_literal, TypeName)                                           \
    namespace {                                                                                    \
        const bool _ptv_regd_##TypeName = ::ptv::register_dialogue_hook(                           \
            id_literal, [] { return std::unique_ptr<::ptv::IDialogueHook>(new TypeName()); });     \
    }

}  // namespace ptv
