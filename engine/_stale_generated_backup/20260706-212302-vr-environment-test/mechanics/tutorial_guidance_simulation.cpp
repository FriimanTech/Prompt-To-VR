#include "tutorial_guidance_shared.h"

#include <algorithm>
#include <cmath>

namespace ptv {

// Implementation helpers -----------------------------------------------------

void tg_add_or_replace_tag(World& world, std::uint32_t e, std::string_view key, std::string_view value) {
    // We encode as key:value in a flat string for simplicity.
    std::string kv = std::string(key) + ":" + std::string(value);
    if (!world.has<CTag>(e)) { world.emplace_or_replace<CTag>(e, CTag{}); }
    auto& tg = world.get<CTag>(e);
    auto it = std::find_if(tg.tags.begin(), tg.tags.end(), [&](const std::string& s){ return s.rfind(std::string(key) + ":", 0) == 0; });
    if (it == tg.tags.end()) tg.tags.push_back(std::move(kv)); else *it = std::move(kv);
}

void tg_sync_flag_tags(World& world, std::uint32_t e, const CTutorialLocalFlags& f) {
    if (!world.has<CTag>(e)) world.emplace_or_replace<CTag>(e, CTag{});
    auto& tg = world.get<CTag>(e);
    auto set_tag = [&](const char* s, bool v){
        auto it = std::find(tg.tags.begin(), tg.tags.end(), std::string(s));
        if (v) {
            if (it == tg.tags.end()) tg.tags.push_back(s);
        } else {
            if (it != tg.tags.end()) tg.tags.erase(it);
        }
    };
    set_tag("tutorial.completed.intro", f.intro_done);
    set_tag("tutorial.completed.fire_bolt", f.fired_bolt);
    set_tag("tutorial.completed.raise_shield", f.raised_shield);
    set_tag("tutorial.completed.place_platform", f.placed_platform);
}

std::vector<std::string> tg_read_events_from_params(const nlohmann::json& p) {
    std::vector<std::string> out;
    if (p.contains("events") && p.at("events").is_array()) {
        for (auto& v : p.at("events")) if (v.is_string()) out.push_back(v.get<std::string>());
    }
    if (p.contains("last_event") && p.at("last_event").is_string()) {
        out.push_back(p.at("last_event").get<std::string>());
    }
    if (p.contains("event") && p.at("event").is_string()) {
        out.push_back(p.at("event").get<std::string>());
    }
    return out;
}

std::optional<std::uint32_t> tg_find_player_avatar(World& world) {
    auto& reg = world.registry();
    auto view = reg.view<CTag>();
    for (auto e : view) {
        auto& tg = view.get<CTag>(e);
        for (auto& s : tg.tags) {
            if (s == "player_avatar") {
                return static_cast<std::uint32_t>(e);
            }
        }
    }
    return std::nullopt;
}

static const char* next_needed_gate(const CTutorialLocalFlags& f) {
    if (!f.intro_done) return "intro";
    if (!f.fired_bolt) return "fire_bolt";
    if (!f.raised_shield) return "raise_shield";
    if (!f.placed_platform) return "place_platform";
    return "done";
}

std::string tg_compose_tip(const CTutorialLocalFlags& f, bool is_projector) {
    const char* gate = next_needed_gate(f);
    std::string pfx = is_projector ? "[Projector] " : "[Mentor] ";
    if (gate == std::string("intro"))        return pfx + "Welcome, recruit. Raise your hand to view the wheel, then select Basics.";
    if (gate == std::string("fire_bolt"))    return pfx + "Tap Right Trigger to fire a bolt at a target.";
    if (gate == std::string("raise_shield")) return pfx + "Hold trigger to raise a shield. Time blocks for best effect.";
    if (gate == std::string("place_platform")) return pfx + "Grip to enter shaping. Place a small platform near you.";
    return pfx + "Basics complete. Tutorials can be toggled in Settings.";
}

// Per-frame update ------------------------------------------------------------

void TutorialGuidance::on_update(World& world, std::uint32_t entity, float dt) {
    if (!world.valid(entity)) return;
    auto it = by_entity_.find(entity);
    if (it == by_entity_.end()) return;
    auto& st = it->second;

    // Respect enable/disable every frame via player setting tag.
    if (auto player = tg_find_player_avatar(world)) {
        if (world.has<CTag>(*player)) {
            auto& pt = world.get<CTag>(*player);
            bool disabled = std::find(pt.tags.begin(), pt.tags.end(), std::string("settings.tutorials.disabled")) != pt.tags.end();
            st.enabled = !disabled && st.enabled; // once disabled globally, keep disabled; can be re-enabled via params
        }
    }

    // Pull transient commands from mechanic binding params via CMechanicBinding on this entity (if present).
    // The engine passes us params in on_attach; runtime changes are emulated by re-binding the entry or by feeding
    // a synthetic param stream via the harness. Here we just treat 'enabled', 'skip', 'replay', and 'events'.
    // Note: We don't have direct access to the binding entry here; rely on a world-level override tag instead if needed.

    // Visual pulse and idle rotation for hologram / projector hinting.
    st.t += dt;
    st.pulse = 0.1f * std::sin(6.2831853f * st.t * 0.5f); // slow 0.5 Hz pulse
    if (st.success_flash > 0.0f) {
        st.success_flash = std::max(0.0f, st.success_flash - dt * 2.0f);
    }

    if (world.has<CTransform>(entity)) {
        auto& tr = world.get<CTransform>(entity).t;
        float base = 1.0f;
        float flash = (st.success_flash > 0.0f) ? (0.25f * st.success_flash) : 0.0f;
        tr.scale = base + st.pulse + flash; // uniform scale pulse
        // Subtle idle rotate around up axis for hologram feel.
        // We don't have direct Quat utilities here; keep scale-only to avoid conflicts.
    }

    if (!st.enabled) {
        // Hide tip when disabled but keep completion flags for later sessions.
        st.tip.clear();
        tg_add_or_replace_tag(world, entity, "tutorial.tip", "");
        return;
    }

    // Derive events from a lightweight channel: we let external systems drop a key via entity tag "tutorial.event:<name>".
    std::vector<std::string> events;
    if (world.has<CTag>(entity)) {
        auto& tg = world.get<CTag>(entity);
        for (auto itg = tg.tags.begin(); itg != tg.tags.end(); ) {
            if (itg->rfind("tutorial.event:", 0) == 0) {
                events.push_back(itg->substr(std::strlen("tutorial.event:")));
                itg = tg.tags.erase(itg); // consume once
            } else { ++itg; }
        }
    }

    // Also allow a world-global event on the player avatar (one-shot consumption here).
    if (auto player = tg_find_player_avatar(world)) {
        if (world.has<CTag>(*player)) {
            auto& tg = world.get<CTag>(*player);
            for (auto itg = tg.tags.begin(); itg != tg.tags.end(); ) {
                if (itg->rfind("tutorial.event:", 0) == 0) {
                    events.push_back(itg->substr(std::strlen("tutorial.event:")));
                    itg = tg.tags.erase(itg);
                } else { ++itg; }
            }
        }
    }

    // Gate progression based on events.
    const char* need = next_needed_gate(st.flags);
    for (auto& ev : events) {
        if (ev == "skip" && st.allow_skip) {
            // Skip the current gate.
            if (need == std::string("intro")) st.flags.intro_done = true;
            else if (need == std::string("fire_bolt")) st.flags.fired_bolt = true;
            else if (need == std::string("raise_shield")) st.flags.raised_shield = true;
            else if (need == std::string("place_platform")) st.flags.placed_platform = true;
            st.success_flash = 1.0f;
        } else if (ev == "replay") {
            st.flags = CTutorialLocalFlags{};
            st.success_flash = 0.0f;
        } else if (ev == "fired_bolt") {
            st.flags.intro_done = true; // intro auto-completes when first action happens
            st.flags.fired_bolt = true;
            st.success_flash = 1.0f;
        } else if (ev == "raised_shield") {
            st.flags.raised_shield = true;
            st.success_flash = 1.0f;
        } else if (ev == "placed_platform") {
            st.flags.placed_platform = true;
            st.success_flash = 1.0f;
        }
    }

    // Mirror flags to tags so other systems (HUD) can read and persist later.
    tg_sync_flag_tags(world, entity, st.flags);

    // Compose and publish the current tip line.
    st.tip = tg_compose_tip(st.flags, st.is_projector);
    tg_add_or_replace_tag(world, entity, "tutorial.tip", st.tip);

    // If all done, add a mastery hint tag once (idempotent add handled in sync).
    if (st.flags.all_done()) {
        tg_add_or_replace_tag(world, entity, "tutorial.status", "complete");
    } else {
        tg_add_or_replace_tag(world, entity, "tutorial.status", "active");
    }
}

} // namespace ptv
