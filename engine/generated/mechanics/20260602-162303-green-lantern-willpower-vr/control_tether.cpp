#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include "control_constructs_shared.h"
#include <algorithm>
#include <string>

namespace ptv {

std::uint32_t find_entity(World& w, const std::string& id_str) {
    auto e = w.find(id_str);
    // World::find returns entt::null for not found; ABI passes as std::uint32_t
    return static_cast<std::uint32_t>(e);
}

void ensure_tag(World& w, std::uint32_t e, const std::string& tag) {
    if (!w.valid(e)) return;
    CTag tagc{};
    if (w.has<CTag>(e)) {
        tagc = w.get<CTag>(e);
    }
    auto it = std::find(tagc.tags.begin(), tagc.tags.end(), tag);
    if (it == tagc.tags.end()) tagc.tags.push_back(tag);
    w.emplace_or_replace<CTag>(e, std::move(tagc));
}

void remove_tag_prefix(World& w, std::uint32_t e, const std::string& prefix) {
    if (!w.has<CTag>(e)) return;
    auto tagc = w.get<CTag>(e);
    std::vector<std::string> kept;
    kept.reserve(tagc.tags.size());
    for (auto& s : tagc.tags) {
        if (s.rfind(prefix, 0) != 0) kept.push_back(std::move(s));
    }
    tagc.tags = std::move(kept);
    w.emplace_or_replace<CTag>(e, std::move(tagc));
}

bool has_tag_or_prefix(World& w, std::uint32_t e, const std::string& key) {
    if (!w.has<CTag>(e)) return false;
    auto& tagc = w.get<CTag>(e);
    for (auto& s : tagc.tags) {
        if (s == key) return true;
        if (s.rfind(key + ":", 0) == 0) return true;
    }
    return false;
}

void publish_haptics(World& w, std::uint32_t e, const std::string& pattern_id,
                     const std::string& hand, float intensity_scale, float duration_s) {
    // Dependency contract from haptics_manager exports
    nlohmann::json j;
    j["pattern_id"] = pattern_id;
    j["hand"] = hand;
    j["intensity_scale"] = intensity_scale;
    j["duration_s"] = duration_s;
    ensure_tag(w, e, std::string("haptics.publish:") + j.dump());
}

void spawn_vfx_line(World& w, std::uint32_t e_context,
                    const ptv::Vec3& from, const ptv::Vec3& to,
                    const std::string& asset_id,
                    float ttl_s) {
    nlohmann::json j;
    j["asset"] = asset_id;               // e.g., construct_chain_tether
    j["kind"] = "line";                  // a generic line beam
    j["from"] = {from.x, from.y, from.z};
    j["to"] = {to.x, to.y, to.z};
    j["ttl_s"] = ttl_s;
    ensure_tag(w, e_context, std::string("vfx.spawn:") + j.dump());
}

void request_cage_tighten(World& w, std::uint32_t target,
                          float delta_radius_m, float duration_add_s, float drain_mul) {
    if (!w.valid(target)) return;
    nlohmann::json j;
    j["delta_radius_m"] = delta_radius_m;
    j["duration_add_s"] = duration_add_s;
    j["drain_mul"] = drain_mul;
    ensure_tag(w, target, std::string("construct.cage:tighten:") + j.dump());
}

bool is_in_yellow_zone(World& w, std::uint32_t target) {
    if (!w.has<CTag>(target)) return false;
    auto& tagc = w.get<CTag>(target);
    for (auto& s : tagc.tags) {
        if (s == "yellow_zone" || s == "yellow_zone_warning") return true;
        if (s.rfind("zone:", 0) == 0 && s.find("yellow") != std::string::npos) return true;
    }
    return false;
}

static ptv::Vec3 get_position(World& w, std::uint32_t e) {
    if (w.has<CTransform>(e)) return w.get<CTransform>(e).t.position;
    return {0.0f,0.0f,0.0f};
}

static void set_position(World& w, std::uint32_t e, const ptv::Vec3& p) {
    if (!w.has<CTransform>(e)) return;
    auto comp = w.get<CTransform>(e);
    comp.t.position = p;
    w.emplace_or_replace<CTransform>(e, comp);
}

ParsedControlInput parse_control_binding_params(World& w, std::uint32_t binding_entity, const std::string& self_mechanic_id) {
    ParsedControlInput out;
    if (!w.has<CMechanicBinding>(binding_entity)) return out;
    auto& mb = w.get<CMechanicBinding>(binding_entity);
    for (auto& e : mb.entries) {
        if (e.id == self_mechanic_id) {
            auto j = nlohmann::json::parse(e.params_json, nullptr, false);
            if (!j.is_object()) break;
            out.left_trigger = j.value("left_trigger_held", false);
            out.right_trigger = j.value("right_trigger_held", false);
            out.left_grip = j.value("left_grip_held", false);
            out.active_control = j.value("active_control", out.active_control);
            out.refined_target_id = j.value("current_target_id", std::string());
            out.tether_max_length_m = j.value("tether_max_length_m", out.tether_max_length_m);
            out.tether_tension_break_threshold = j.value("tether_tension_break_threshold", out.tether_tension_break_threshold);
            out.reel_in_speed_mps = j.value("reel_in_speed_mps", out.reel_in_speed_mps);
            out.bands_slow_per_stack = j.value("bands_slow_per_stack", out.bands_slow_per_stack);
            out.bands_max_stacks = j.value("bands_max_stacks", out.bands_max_stacks);
            out.fear_zone_resistance_multiplier = j.value("fear_zone_resistance_multiplier", out.fear_zone_resistance_multiplier);
            break;
        }
    }
    return out;
}

bool step_tether(World& w, std::uint32_t hand_e, float dt,
                 const ParsedControlInput& in,
                 TetherRuntime& tr) {
    if (!tr.active) return false;
    if (!w.valid(tr.target_entity)) { tr.active = false; return false; }

    // Comfort: visualize line each frame
    const auto hand_p = get_position(w, hand_e);
    const auto target_p = get_position(w, tr.target_entity);
    spawn_vfx_line(w, hand_e, hand_p, target_p, "construct_chain_tether", 0.05f);

    // Compute distance and tension
    ptv::Vec3 diff{target_p.x - hand_p.x, target_p.y - hand_p.y, target_p.z - hand_p.z};
    float dist = std::sqrt(diff.x*diff.x + diff.y*diff.y + diff.z*diff.z);
    float stretch = std::max(0.0f, dist - in.tether_max_length_m);

    // Simple relative-velocity proxy: how fast closing/opening this frame (comfort-safe)
    float rel_speed = 0.0f; // unavailable without velocity comps; keep 0 for stability

    tr.tension_norm = std::clamp((stretch / std::max(0.001f, in.tether_max_length_m)) + 0.25f*rel_speed*0.0f, 0.0f, 4.0f);
    float break_thresh = in.tether_tension_break_threshold;
    if (is_in_yellow_zone(w, tr.target_entity)) break_thresh /= std::max(0.01f, in.fear_zone_resistance_multiplier);

    // Reel/anchor mode on Left Trigger: gently pull target toward hand
    if (in.left_trigger) {
        if (dist > 0.001f) {
            float step = in.reel_in_speed_mps * dt;
            float t = std::min(step / dist, 0.25f); // clamp to avoid yanks
            ptv::Vec3 new_p{ target_p.x + (hand_p.x - target_p.x)*t,
                             target_p.y + (hand_p.y - target_p.y)*t,
                             target_p.z + (hand_p.z - target_p.z)*t };
            set_position(w, tr.target_entity, new_p);
        }
    }

    // Break check
    if (tr.tension_norm > break_thresh) {
        // Snap VFX + haptics
        spawn_vfx_line(w, hand_e, hand_p, target_p, "construct_chain_tether", 0.15f);
        publish_haptics(w, hand_e, "bolt_fire_sharp", "right", 0.8f, 0.08f);
        remove_tag_prefix(w, tr.target_entity, "status.tethered");
        tr.active = false;
        return false;
    }

    // Optional: if target already inside an energy cage and player is pressing LT, tighten slightly
    if (in.left_trigger && has_tag_or_prefix(w, tr.target_entity, "construct.cage:active")) {
        request_cage_tighten(w, tr.target_entity, -0.1f, 1.0f, 1.05f);
    }

    return true;
}

} // namespace ptv
