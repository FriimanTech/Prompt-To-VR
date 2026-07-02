#include "mission_shared.h"

namespace ptv { namespace mission_util {

static std::optional<std::string> eval_medal(const std::string& missionId, float elapsedS, int score, const MedalThresholds& t) {
    auto it = t.by_mission.find(missionId);
    if (it == t.by_mission.end()) return std::nullopt;
    const auto& trio = it->second;
    // Heuristic: if thresholds are all >0, treat as time trial (lower is better). Otherwise treat as score (higher is better).
    const bool isTime = (trio.gold > 0.0f && trio.silver > 0.0f && trio.bronze > 0.0f);
    if (isTime) {
        if (elapsedS <= trio.gold)   return std::optional<std::string>{"gold"};
        if (elapsedS <= trio.silver) return std::optional<std::string>{"silver"};
        if (elapsedS <= trio.bronze) return std::optional<std::string>{"bronze"};
    } else {
        if (score >= (int)trio.gold)   return std::optional<std::string>{"gold"};
        if (score >= (int)trio.silver) return std::optional<std::string>{"silver"};
        if (score >= (int)trio.bronze) return std::optional<std::string>{"bronze"};
    }
    return std::nullopt;
}

void maybe_award_medal_and_persist(World& w, std::uint32_t e, const MissionDef& def, MissionRuntimeState& rt, const MedalThresholds& thresholds,
                                   const std::string& saveSectionPushPrefix, const std::string& saveRequestPrefix,
                                   const std::string& uiToastPrefix, const std::string& missionCompletePrefix) {
    if (!rt.finished || rt.medal.has_value()) return;

    auto medal = eval_medal(def.id, rt.elapsed_s, rt.score, thresholds);
    if (medal.has_value()) rt.medal = medal;

    // Persist completion + medal to Save system via tag protocol from dependency
    nlohmann::json savePayload;
    savePayload["section"] = "mission";
    savePayload["data"] = {
        {"mission_id", def.id},
        {"difficulty", rt.selected_difficulty},
        {"elapsed_s", rt.elapsed_s},
        {"score", rt.score},
        {"medal", rt.medal.value_or("")}
    };
    push_tag(w, e, make_tag(saveSectionPushPrefix, savePayload));

    // Also publish mission.complete for consumers (progression)
    nlohmann::json mc; mc["id"] = def.id; mc["difficulty"] = rt.selected_difficulty; mc["medal"] = rt.medal.value_or("");
    push_tag(w, e, make_tag(missionCompletePrefix, mc));

    // Request save
    nlohmann::json req; req["reason"] = "mission_complete"; req["immediate"] = true;
    push_tag(w, e, make_tag(saveRequestPrefix, req));

    // Toast
    nlohmann::json toast; toast["text"] = std::string("Mission Complete: ") + def.id + (rt.medal ? std::string(" — ") + *rt.medal + " medal" : std::string("")); toast["style"] = "success";
    push_tag(w, e, make_tag(uiToastPrefix, toast));
}

}} // namespace ptv::mission_util
