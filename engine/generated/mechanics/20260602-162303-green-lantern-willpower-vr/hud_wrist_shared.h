#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <optional>
#include <nlohmann/json.hpp>

namespace hudwm {
struct Page {
    std::string id;
    std::string title;
    std::string section; // e.g., "Locomotion", "Accessibility", etc.
    nlohmann::json payload; // free-form extra data from registrant
};

struct Settings {
    float ui_opacity{0.85f};
    bool high_contrast{false};
    int hud_mode{0};
    bool wrist_menu_open{false};
};

struct HudGlobal {
    Settings current;
    // page_id -> Page
    std::unordered_map<std::string, Page> pages;
    bool dirty_settings{false};
    bool show_spawn_toast_once{true};
};

inline HudGlobal& global() {
    static HudGlobal g; return g;
}

// Parse helper: safe get bool/float/int from json
inline bool j_get_bool(const nlohmann::json& j, std::string_view k, bool defv=false){ if(j.contains(std::string(k))){ try{ return j.at(std::string(k)).get<bool>(); }catch(...){} } return defv; }
inline float j_get_float(const nlohmann::json& j, std::string_view k, float defv=0.0f){ if(j.contains(std::string(k))){ try{ return j.at(std::string(k)).get<float>(); }catch(...){} } return defv; }
inline int j_get_int(const nlohmann::json& j, std::string_view k, int defv=0){ if(j.contains(std::string(k))){ try{ return j.at(std::string(k)).get<int>(); }catch(...){} } return defv; }

inline void register_page_from_json(const nlohmann::json& j){
    if(!j.is_object()) return;
    if(!j.contains("id") || !j.contains("title")) return;
    Page p;
    p.id = j.value("id", std::string("page_"));
    p.title = j.value("title", std::string("Untitled"));
    p.section = j.value("section", std::string("Misc"));
    if(j.contains("payload")) p.payload = j.at("payload");
    global().pages[p.id] = std::move(p);
}

inline std::string pages_digest_tag(){
    // tiny digest for tags: count + mode
    return std::string("menu_pages:") + std::to_string(global().pages.size());
}

inline std::string opacity_tag(){ return std::string("hud_opacity:") + std::to_string(global().current.ui_opacity); }
inline std::string contrast_tag(){ return std::string("hud_contrast:") + (global().current.high_contrast?"1":"0"); }
inline std::string mode_tag(){ return std::string("hud_mode:") + std::to_string(global().current.hud_mode); }
inline std::string menu_open_tag(){ return std::string("wrist_menu:") + (global().current.wrist_menu_open?"open":"closed"); }

} // namespace hudwm
