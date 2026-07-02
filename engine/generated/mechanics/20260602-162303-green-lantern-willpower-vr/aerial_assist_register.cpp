#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"

#include <nlohmann/json.hpp>
#include <string>

#include "aerial_assist_shared.h"

namespace ptv {

// Forward decl from ui file
void aerial_assist_show_toggle_toast(World& w, std::uint32_t e, bool enabled);

extern const char* kTagHudLead;     // from compute TU (for exports only we don't need linkage)
extern const char* kTagAimAssist;   // from compute TU

class AerialCombatAssist; // from compute TU

// Thin shim that owns the mechanic instance while also surfacing quick param edge detection for toasts.
class AerialCombatAssistRegister final : public IMechanic {
public:
    void on_attach(World& w, std::uint32_t e, const nlohmann::json& params) override {
        last_enable_[e] = params.value("enable", true);
        // Immediately surface current state as a toast to confirm
        aerial_assist_show_toggle_toast(w, e, last_enable_[e]);
        // Defer behavior to the implementation class by delegating via world binding system — engine will call all mechanics bound.
        (void)w; (void)e; (void)params;
    }
    void on_update(World& w, std::uint32_t e, float /*dt*/) override {
        // Watch for toggle change and toast
        if (w.has<CMechanicBinding>(e)) {
            const auto& mb = w.get<CMechanicBinding>(e);
            for (const auto& entry : mb.entries) {
                if (entry.id == id()) {
                    auto j = nlohmann::json::parse(entry.params_json, nullptr, false);
                    if (j.is_object()) {
                        bool en = j.value("enable", true);
                        auto it = last_enable_.find(e);
                        bool prev = (it!=last_enable_.end()) ? it->second : en;
                        if (en != prev) {
                            aerial_assist_show_toggle_toast(w, e, en);
                            last_enable_[e] = en;
                        }
                    }
                    break;
                }
            }
        }
    }
    void on_detach(World&, std::uint32_t e) override { last_enable_.erase(e); }
    std::string id() const override { return "generated.aerial_combat_assist"; }
private:
    std::unordered_map<std::uint32_t,bool> last_enable_{};
};

} // namespace ptv

PTV_REGISTER_MECHANIC("generated.aerial_combat_assist", ptv::AerialCombatAssistRegister)
