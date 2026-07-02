#include "ptv/ecs/World.h"
#include "hover_mode_shared.h"

#include <entt/entt.hpp>
#include <string>

using nlohmann::json;

namespace hover_internal {

// World helpers declared in shared
inline void upsert_tag(ptv::World& w, std::uint32_t e, const std::string& key, const std::string& value_json) {
    if (!w.valid(e)) return;
    if (!w.has<ptv::CTag>(e)) {
        w.emplace_or_replace<ptv::CTag>(e, ptv::CTag{});
    }
    auto& vec = w.get<ptv::CTag>(e).tags;
    // replace if same prefix exists, else push
    auto prefix = key;
    bool replaced = false;
    for (auto& s : vec) {
        if (s.rfind(prefix, 0) == 0) { // starts with
            s = key + value_json;
            replaced = true;
            break;
        }
    }
    if (!replaced) vec.push_back(key + value_json);
}

inline void remove_tag_with_prefix(ptv::World& w, std::uint32_t e, std::string_view prefix) {
    if (!w.valid(e) || !w.has<ptv::CTag>(e)) return;
    auto& vec = w.get<ptv::CTag>(e).tags;
    vec.erase(std::remove_if(vec.begin(), vec.end(), [&](const std::string& s){ return s.rfind(prefix.data(), 0) == 0; }), vec.end());
}

FrameInput read_frame_input(ptv::World& w, std::uint32_t e, const char* mechanic_id) {
    FrameInput fi;
    // Default scene id empty; if the entity has a tag "scene_id:<id>", consume it
    if (w.has<ptv::CTag>(e)) {
        const auto& tags = w.get<ptv::CTag>(e).tags;
        for (const auto& s : tags) {
            constexpr const char* kScenePrefix = "scene.id:";
            if (s.rfind(kScenePrefix, 0) == 0) {
                fi.scene_id = s.substr(std::char_traits<char>::length(kScenePrefix));
                break;
            }
        }
    }

    // Scan CMechanicBinding to find our entry and parse params_json (serialised each frame by the harness)
    auto& reg = w.registry();
    auto view = reg.view<ptv::CMechanicBinding>();
    for (auto ent : view) {
        auto& bind = view.get<ptv::CMechanicBinding>(ent);
        for (auto& entry : bind.entries) {
            if (entry.id == mechanic_id) {
                auto j = json::parse(entry.params_json, nullptr, /*allow_exceptions=*/false);
                if (j.is_object()) {
                    fi.a_pressed = j.value("a_button_press", false);
                    if (j.contains("left_stick") && j["left_stick"].is_array() && j["left_stick"].size() == 2) {
                        fi.left_x = j["left_stick"][0].get<float>();
                        fi.left_y = j["left_stick"][1].get<float>();
                    } else {
                        fi.left_x = j.value("left_x", 0.0f);
                        fi.left_y = j.value("left_y", 0.0f);
                    }
                    fi.right_y = j.value("right_y", j.value("right_stick_y", 0.0f));
                    fi.altitude_assist = j.value("altitude_assist", true);
                    fi.scene_id = j.value("scene_id", fi.scene_id);
                }
                return fi; // first match is enough
            }
        }
    }
    return fi;
}

inline void publish_haptics(ptv::World& w, std::uint32_t e, const std::string& pattern_id, const char* hand, float amp, float dur_s) {
    // Emit a publish tag on this entity; the haptics manager listens globally
    nlohmann::json j;
    j["pattern_id"] = pattern_id;
    j["hand"] = hand;
    j["intensity_scale"] = amp;
    j["duration_s"] = dur_s;
    upsert_tag(w, e, kHapticsPublishPrefix, j.dump());
}

inline void write_hover_mode_tag(ptv::World& w, std::uint32_t e, bool active) {
    nlohmann::json j; j["state"] = active ? "hover" : "off";
    upsert_tag(w, e, std::string("locomotion.mode:"), j.dump());
}

inline void write_velocity_tag(ptv::World& w, std::uint32_t e, float vx, float vy, float vz) {
    nlohmann::json j; j["vx"] = vx; j["vy"] = vy; j["vz"] = vz;
    upsert_tag(w, e, std::string("locomotion.velocity:"), j.dump());
}

inline void write_aim_stabilizer_tag(ptv::World& w, std::uint32_t e, bool enabled, float strength) {
    nlohmann::json j; j["enabled"] = enabled; j["strength"] = strength;
    upsert_tag(w, e, std::string("aim.stabilizer:"), j.dump());
}

} // namespace hover_internal
