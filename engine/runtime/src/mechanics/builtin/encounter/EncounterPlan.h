#pragma once

// The data an encounter director runs: which NPC types arrive, in what
// waves, from where, and under what caps. Compiled by scene_compile from the
// NPC plan into the director's binding params. Parsing and the per-wave spawn
// order are pure functions so they can be tested without a world.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ptv/math/Math.h"

namespace ptv::encounter {

enum class Mode { Waves, Continuous, Fixed };
enum class Start { OnLoad, Delay, PlayerGrabsTag, Signal };

struct Group {
    std::string template_id;  // scene entity id of the hidden template
    int count{0};
};

struct Wave {
    std::vector<Group> groups;
};

struct Zone {
    std::string id;
    Vec3 position{0.0f};
    float radius{2.0f};
    std::vector<std::string> tags;
};

struct Plan {
    Mode mode{Mode::Waves};
    Start start{Start::OnLoad};
    float start_delay_s{0.0f};
    std::string start_tag{"weapon"};
    int max_concurrent{6};
    float spawn_interval_s{1.5f};
    float inter_wave_delay_s{6.0f};
    float min_player_distance_m{10.0f};
    /** Zones this far away are ignored unless nothing closer is eligible. */
    float max_player_distance_m{60.0f};
    std::vector<Wave> waves;
    std::vector<Zone> zones;

    int wave_total(std::size_t wave) const {
        if (wave >= waves.size()) return 0;
        int n = 0;
        for (const auto& g : waves[wave].groups) n += std::max(0, g.count);
        return n;
    }
};

inline Vec3 read_vec3(const nlohmann::json& j, Vec3 fallback) {
    if (!j.is_array() || j.size() < 3) return fallback;
    for (int i = 0; i < 3; ++i) {
        if (!j[static_cast<std::size_t>(i)].is_number()) return fallback;
    }
    return Vec3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
}

inline Plan parse(const nlohmann::json& p) {
    Plan plan;
    if (!p.is_object()) return plan;
    const std::string mode = p.value("mode", std::string("waves"));
    plan.mode = mode == "continuous" ? Mode::Continuous
                : mode == "fixed"    ? Mode::Fixed
                                     : Mode::Waves;
    if (p.contains("start") && p["start"].is_object()) {
        const auto& s = p["start"];
        const std::string kind = s.value("kind", std::string("on_load"));
        plan.start = kind == "delay"              ? Start::Delay
                     : kind == "player_grabs_tag" ? Start::PlayerGrabsTag
                     : kind == "signal"           ? Start::Signal
                                                  : Start::OnLoad;
        plan.start_delay_s = std::max(0.0f, s.value("delay_s", 0.0f));
        plan.start_tag = s.value("tag", plan.start_tag);
    }
    plan.max_concurrent = std::clamp(p.value("max_concurrent", 6), 1, 64);
    plan.spawn_interval_s =
        std::clamp(p.value("spawn_interval_s", 1.5f), 0.0f, 60.0f);
    plan.inter_wave_delay_s =
        std::clamp(p.value("inter_wave_delay_s", 6.0f), 0.0f, 600.0f);
    plan.min_player_distance_m =
        std::clamp(p.value("min_player_distance_m", 10.0f), 0.0f, 500.0f);
    plan.max_player_distance_m = std::clamp(
        p.value("max_player_distance_m", 60.0f), plan.min_player_distance_m,
        2000.0f);
    if (p.contains("waves") && p["waves"].is_array()) {
        for (const auto& w : p["waves"]) {
            Wave wave;
            if (w.contains("groups") && w["groups"].is_array()) {
                for (const auto& g : w["groups"]) {
                    Group group;
                    group.template_id = g.value("template", std::string());
                    group.count = std::clamp(g.value("count", 0), 0, 200);
                    if (!group.template_id.empty() && group.count > 0) {
                        wave.groups.push_back(group);
                    }
                }
            }
            if (!wave.groups.empty()) plan.waves.push_back(std::move(wave));
        }
    }
    if (p.contains("zones") && p["zones"].is_array()) {
        for (const auto& z : p["zones"]) {
            Zone zone;
            zone.id = z.value("id", std::string());
            zone.position = read_vec3(z.value("position", nlohmann::json()),
                                      Vec3(0.0f));
            zone.radius = std::clamp(z.value("radius", 2.0f), 0.0f, 50.0f);
            if (z.contains("tags") && z["tags"].is_array()) {
                for (const auto& t : z["tags"]) {
                    if (t.is_string()) zone.tags.push_back(t.get<std::string>());
                }
            }
            plan.zones.push_back(std::move(zone));
        }
    }
    return plan;
}

/** The order a wave's NPCs arrive in: groups interleaved, so a wave of
 *  3 standard + 2 tall arrives standard, tall, standard, tall, standard
 *  rather than as two blocks. */
inline std::vector<std::string> spawn_order(const Wave& wave) {
    std::vector<std::string> out;
    std::vector<int> left;
    for (const auto& g : wave.groups) left.push_back(std::max(0, g.count));
    bool any = true;
    while (any) {
        any = false;
        for (std::size_t i = 0; i < wave.groups.size(); ++i) {
            if (left[i] <= 0) continue;
            out.push_back(wave.groups[i].template_id);
            --left[i];
            any = true;
        }
    }
    return out;
}

/** A deterministic point inside a zone for the k-th spawn there (golden
 *  angle spiral, so consecutive spawns do not stack). */
inline Vec3 point_in_zone(const Zone& zone, int k) {
    if (zone.radius <= 1e-3f) return zone.position;
    const float golden = 2.39996323f;
    const float a = golden * static_cast<float>(k);
    const float frac = std::fmod(0.37f + 0.61803399f * static_cast<float>(k), 1.0f);
    const float r = zone.radius * std::sqrt(frac);
    return zone.position + Vec3(r * std::cos(a), 0.0f, r * std::sin(a));
}

}  // namespace ptv::encounter
