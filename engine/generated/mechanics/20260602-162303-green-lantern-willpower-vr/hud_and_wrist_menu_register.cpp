#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <entt/entt.hpp>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>

namespace hudwm {

struct Page {
    std::string id;
    std::string title;
    std::string section;
    nlohmann::json payload;
};

struct Settings {
    float ui_opacity{0.85f};
    bool high_contrast{false};
    int hud_mode{0};
    bool wrist_menu_open{false};
};

struct HudGlobal {
    Settings current;
    std::unordered_map<std::string, Page> pages;
    bool dirty_settings{false};
    bool show_spawn_toast_once{true};
};

inline HudGlobal& global() {
    static HudGlobal g; return g;
}

inline bool j_get_bool(const nlohmann::json& j, std::string_view k, bool defv=false){
    if(j.contains(std::string(k))){ try{ return j.at(std::string(k)).get<bool>(); }catch(...){} } return defv;
}
inline float j_get_float(const nlohmann::json& j, std::string_view k, float defv=0.0f){
    if(j.contains(std::string(k))){ try{ return j.at(std::string(k)).get<float>(); }catch(...){} } return defv;
}
inline int j_get_int(const nlohmann::json& j, std::string_view k, int defv=0){
    if(j.contains(std::string(k))){ try{ return j.at(std::string(k)).get<int>(); }catch(...){} } return defv;
}

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
    return std::string("menu_pages:") + std::to_string(global().pages.size());
}
inline std::string opacity_tag(){ return std::string("hud_opacity:") + std::to_string(global().current.ui_opacity); }
inline std::string contrast_tag(){ return std::string("hud_contrast:") + (global().current.high_contrast?"1":"0"); }
inline std::string mode_tag(){ return std::string("hud_mode:") + std::to_string(global().current.hud_mode); }
inline std::string menu_open_tag(){ return std::string("wrist_menu:") + (global().current.wrist_menu_open?"open":"closed"); }

} // namespace hudwm

namespace ptv {

namespace hud_helpers {
extern "C" void hud_get_settings_json(std::string& out_json){
    nlohmann::json j;
    j["ui_opacity"] = hudwm::global().current.ui_opacity;
    j["high_contrast"] = hudwm::global().current.high_contrast;
    j["hud_mode"] = hudwm::global().current.hud_mode;
    j["wrist_menu_open"] = hudwm::global().current.wrist_menu_open;
    nlohmann::json pages = nlohmann::json::array();
    for (auto& kv : hudwm::global().pages){
        nlohmann::json pj; pj["id"]=kv.second.id; pj["title"]=kv.second.title; pj["section"]=kv.second.section; pages.push_back(std::move(pj));
    }
    j["pages"] = std::move(pages);
    out_json = j.dump();
}
} // namespace hud_helpers

class HudAndWristMenu : public IMechanic {
public:
    void on_attach(World& w, std::uint32_t e, const nlohmann::json& params) override {
        using namespace hudwm;
        // Seed settings from params (harness can override for tests)
        global().current.ui_opacity = params.value("ui_opacity", global().current.ui_opacity);
        global().current.high_contrast = params.value("high_contrast", global().current.high_contrast);
        // Tag entity as participating in HUD system
        world_emplace_hud_tags(w, e, /*on=*/true);
        // Optional immediate pages provided via params
        if (params.contains("register_pages") && params["register_pages"].is_array()) {
            for (auto& pj : params["register_pages"]) hudwm::register_page_from_json(pj);
        }
        // Spawn toast once on first attach anywhere
        if (hudwm::global().show_spawn_toast_once) {
            push_toast_tag(w, e, "toast:open_locomotion_menu");
            hudwm::global().show_spawn_toast_once = false;
        }
    }

    void on_update(World& w, std::uint32_t e, float dt) override {
        (void)dt;
        // Role-detect: anchors (tagged hud or mesh id pack) vs rig controller (tagged player/rig)
        const bool is_hud_anchor = is_hud_entity(w, e);
        if (is_hud_anchor) {
            apply_hud_state_to_entity(w, e);
        }
        // Always listen for page registrations and input-like params from mechanic bindings on this entity
        consume_binding_messages(w, e);
        // If this looks like the rig, propagate current HUD tags to all HUD entities for consistency
        if (is_player_rig_like(w, e)) propagate_to_all_hud_entities(w);
        // Persist settings if flagged
        maybe_emit_persistence_request(w, e);
    }

    void on_detach(World& w, std::uint32_t e) override {
        world_emplace_hud_tags(w, e, /*on=*/false);
    }

    std::string id() const override { return "generated.hud_and_wrist_menu"; }

private:
    static bool has_tag(const ptv::World& w, std::uint32_t e, const char* needle){
        if(!w.has<ptv::CTag>(e)) return false;
        const auto& tags = w.get<ptv::CTag>(e).tags;
        for (auto& t : tags) if (t == needle) return true;
        return false;
    }

    static bool is_hud_entity(ptv::World& w, std::uint32_t e){
        if (has_tag(w,e,"hud")) return true;
        if (w.has<ptv::CMesh>(e)) {
            const auto& m = w.get<ptv::CMesh>(e);
            if (m.asset_id == std::string("ring_hud_ui_pack")) return true;
        }
        return false;
    }

    static bool is_player_rig_like(ptv::World& w, std::uint32_t e){
        return has_tag(w,e,"player") || has_tag(w,e,"rig") || has_tag(w,e,"player_rig");
    }

    static void world_emplace_hud_tags(ptv::World& w, std::uint32_t e, bool on){
        using namespace hudwm;
        ptv::CTag tag;
        if (w.has<ptv::CTag>(e)) tag = w.get<ptv::CTag>(e);
        auto& v = tag.tags;
        auto ensure = [&](const std::string& t){ if(std::find(v.begin(), v.end(), t)==v.end()) v.push_back(t); };
        auto drop = [&](const std::string& t){ v.erase(std::remove(v.begin(), v.end(), t), v.end()); };
        if (on) {
            ensure("hud_system");
            ensure(hudwm::pages_digest_tag());
            ensure(hudwm::opacity_tag());
            ensure(hudwm::contrast_tag());
            ensure(hudwm::mode_tag());
            ensure(hudwm::menu_open_tag());
        } else {
            drop("hud_system");
        }
        w.emplace_or_replace<ptv::CTag>(e, std::move(tag));
    }

    static void apply_hud_state_to_entity(ptv::World& w, std::uint32_t e){
        using namespace hudwm;
        // Write readable tags; actual rendering handled by builtin.hud_anchor + content meshes
        if (!w.has<ptv::CTag>(e)) w.emplace_or_replace<ptv::CTag>(e, ptv::CTag{});
        auto& tag = w.get<ptv::CTag>(e);
        auto& v = tag.tags;
        auto setkv = [&](const std::string& key_prefix, const std::string& val){
            // remove existing with same prefix
            for (auto it=v.begin(); it!=v.end(); ) {
                if (it->rfind(key_prefix,0)==0) it = v.erase(it); else ++it;
            }
            v.push_back(key_prefix+val);
        };
        setkv("hud_opacity:", std::to_string(global().current.ui_opacity));
        setkv("hud_contrast:", global().current.high_contrast?"1":"0");
        setkv("hud_mode:", std::to_string(global().current.hud_mode));
        setkv("wrist_menu:", global().current.wrist_menu_open?"open":"closed");
        setkv("menu_pages:", std::to_string(global().pages.size()));
        // Optional: subtle scale pulse for focus meter readability (comfort-safe)
        if (w.has<ptv::CTransform>(e)) {
            auto& tr = w.get<ptv::CTransform>(e).t;
            tr.scale = 1.0f; // keep neutral; real VFX handled by content
        }
    }

    static void propagate_to_all_hud_entities(ptv::World& w){
        using namespace hudwm;
        auto& reg = w.registry();
        auto view = reg.view<ptv::CTag>();
        for (auto ent : view) {
            auto& t = view.get<ptv::CTag>(ent);
            bool is_hud=false; for (auto& s: t.tags) if (s=="hud") { is_hud=true; break; }
            if (!is_hud) continue;
            // Convert entt::entity to std::uint32_t safely via cast
            std::uint32_t eid = static_cast<std::uint32_t>(ent);
            if (!w.valid(eid)) continue;
            apply_hud_state_to_entity(w, eid);
        }
    }

    static void parse_and_apply_input_like(const nlohmann::json& j){
        using namespace hudwm;
        // Harness supplies these booleans in params for tests
        if (j.contains("x_button_tap")){
            if (j_get_bool(j, "x_button_tap", false) && !global().current.wrist_menu_open){
                global().current.hud_mode = (global().current.hud_mode + 1) % 3; // cycle 3 modes
                global().dirty_settings = true;
            }
        }
        if (j.contains("x_button_hold")){
            bool hold = j_get_bool(j, "x_button_hold", false);
            if (hold != global().current.wrist_menu_open){
                global().current.wrist_menu_open = hold;
                global().dirty_settings = true;
            }
        }
        if (j.contains("set_opacity")){
            float o = j_get_float(j, "set_opacity", global().current.ui_opacity);
            global().current.ui_opacity = std::clamp(o, 0.2f, 1.0f);
            global().dirty_settings = true;
        }
        if (j.contains("toggle_high_contrast")){
            if (j_get_bool(j, "toggle_high_contrast", false)){
                global().current.high_contrast = !global().current.high_contrast;
                global().dirty_settings = true;
            }
        }
    }

    static void consume_binding_messages(ptv::World& w, std::uint32_t e){
        if (!w.has<ptv::CMechanicBinding>(e)) return;
        auto& mb = w.get<ptv::CMechanicBinding>(e);
        for (auto& entry : mb.entries){
            // params_json is a serialized JSON blob
            auto j = nlohmann::json::parse(entry.params_json, nullptr, /*allow_exceptions=*/false);
            if (!j.is_object()) continue;
            // Page registration channel: any entry that carries hud_wrist_menu.register_page
            if (j.contains("hud_wrist_menu") && j["hud_wrist_menu"].is_object()){
                auto& hw = j["hud_wrist_menu"];
                if (hw.contains("register_page")) hudwm::register_page_from_json(hw["register_page"]);
                if (hw.contains("set")) parse_and_apply_input_like(hw["set"]);
            }
            // Direct input-like shim when test harness routes here
            if (j.contains("x_button_tap") || j.contains("x_button_hold") || j.contains("toggle_high_contrast") || j.contains("set_opacity")){
                parse_and_apply_input_like(j);
            }
        }
    }

    static void maybe_emit_persistence_request(ptv::World& w, std::uint32_t e){
        using namespace hudwm;
        if (!global().dirty_settings) return;
        global().dirty_settings = false;
        // Emit a lightweight persistence apply request as a CMechanicBinding entry on this entity.
        // save_persistence listens for entries; we publish a settings section "hud_wrist_menu".
        ptv::CMechanicBinding mb;
        if (w.has<ptv::CMechanicBinding>(e)) mb = w.get<ptv::CMechanicBinding>(e);
        nlohmann::json section;
        section["hud_wrist_menu"]["ui_opacity"] = global().current.ui_opacity;
        section["hud_wrist_menu"]["high_contrast"] = global().current.high_contrast;
        section["hud_wrist_menu"]["hud_mode"] = global().current.hud_mode;
        section["hud_wrist_menu"]["wrist_menu_open"] = global().current.wrist_menu_open;
        // Also persist pages list (ids only) to allow restoring ordering if needed
        std::vector<std::string> ids; ids.reserve(global().pages.size());
        for (auto& kv : global().pages) ids.push_back(kv.first);
        section["hud_wrist_menu"]["pages"] = ids;
        ptv::CMechanicBinding::Entry eapply; // id, builtin_id, params_json
        eapply.id = "apply_settings.hud_wrist_menu";
        eapply.builtin_id = "persistence.apply_and_save"; // understood by generated.save_persistence
        eapply.params_json = section.dump();
        mb.entries.push_back(std::move(eapply));
        w.emplace_or_replace<ptv::CMechanicBinding>(e, std::move(mb));
    }

    static void push_toast_tag(ptv::World& w, std::uint32_t e, const std::string& toast_key){
        if (!w.has<ptv::CTag>(e)) w.emplace_or_replace<ptv::CTag>(e, ptv::CTag{});
        auto& t = w.get<ptv::CTag>(e).tags;
        if (std::find(t.begin(), t.end(), toast_key)==t.end()) t.push_back(toast_key);
    }
};

} // namespace ptv

PTV_REGISTER_MECHANIC("generated.hud_and_wrist_menu", ptv::HudAndWristMenu)
