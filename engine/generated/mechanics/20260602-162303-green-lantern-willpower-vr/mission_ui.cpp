#include "mission_shared.h"

namespace ptv { namespace mission_util {

void ensure_wrist_page_registered(World& w, std::uint32_t e, const std::string& uiPageRegisterPrefix) {
    // Idempotent: the HUD module deduplicates by id; we can push once on attach.
    nlohmann::json j; j["id"] = "mission_log"; j["title"] = "Missions"; j["order"] = 5;
    push_tag(w, e, make_tag(uiPageRegisterPrefix, j));
}

void update_wrist_status(World& w, std::uint32_t e, const MissionDef* def, const MissionRuntimeState& rt, const std::string& uiToastPrefix, float toastCooldownReset) {
    (void)toastCooldownReset; // reserved for future throttling
    if (!def) return;
    // Minimal HUD pulse on state transitions is driven elsewhere; here we can expose quick hints when selection happens.
    if (rt.has_selection && !rt.in_progress && !rt.finished) {
        nlohmann::json j; j["text"] = std::string("Selected: ") + def->id + " (" + rt.selected_difficulty + ") — Approach portal to deploy."; j["style"] = "info";
        push_tag(w, e, make_tag(uiToastPrefix, j));
    }
}

}} // namespace ptv::mission_util
