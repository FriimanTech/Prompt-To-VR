#include "mission_shared.h"

#include <entt/entt.hpp>

namespace ptv { namespace mission_util {

// Consumes objective progress tags from any entity, shaped as:
//   objective.progress:{"id":"<objective_id>", "delta":1, "state":"update|complete|fail"}
// After consuming, the string is erased from the source entity's CTag.
void tick_objectives_from_tags(World& w, MissionRuntimeState& rt, const MissionDef& def, const std::string& objectiveProgressPrefix, std::uint32_t selfEntity) {
    auto& reg = w.registry();
    auto view = reg.view<CTag>();
    for (auto ent : view) {
        auto& tags = view.get<CTag>(ent).tags;
        for (auto it = tags.begin(); it != tags.end(); ) {
            const std::string& s = *it;
            if (s.rfind(objectiveProgressPrefix, 0) == 0) {
                auto payload = nlohmann::json::parse(s.substr(objectiveProgressPrefix.size()), nullptr, false);
                if (payload.is_object()) {
                    const std::string oid = payload.value("id", std::string{});
                    const int delta = payload.value("delta", 0);
                    const std::string state = payload.value("state", std::string{"update"});
                    auto itp = rt.objective_progress.find(oid);
                    if (itp != rt.objective_progress.end()) {
                        auto& pr = itp->second;
                        if (state == "fail") {
                            rt.failed = true;
                        } else {
                            if (delta != 0) pr.current = std::max(0, pr.current + delta);
                            if (state == "complete" || pr.current >= pr.target) {
                                pr.current = pr.target;
                                pr.completed = true;
                            }
                        }
                        // Advance if current objective matches and is completed
                        if (!rt.failed && rt.objective_index < def.objectives.size()) {
                            const auto& cur = def.objectives[rt.objective_index];
                            if (cur.id == oid && pr.completed) {
                                rt.objective_index++;
                                // lightweight score increment per objective
                                rt.score += 100;
                                // Inform HUD via toast on our own entity
                                nlohmann::json j; j["text"] = std::string("Objective Complete: ") + oid; j["style"] = "success";
                                push_tag(w, selfEntity, make_tag("ui.toast:", j));
                            }
                            if (rt.objective_index >= def.objectives.size()) {
                                rt.finished = true;
                                rt.in_progress = false;
                            }
                        }
                    }
                }
                it = tags.erase(it);
            } else {
                ++it;
            }
        }
    }
}

}} // namespace ptv::mission_util
