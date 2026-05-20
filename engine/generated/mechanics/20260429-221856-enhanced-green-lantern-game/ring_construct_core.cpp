#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <entt/entt.hpp>
#include <nlohmann/json.hpp>

#include <string>
#include <unordered_map>
#include <vector>
#include <optional>
#include <algorithm>
#include <cmath>

namespace ptv {

struct ConstructDef {
    float spawn_cost{2.0f};
    float drain_per_s{0.0f};
    float lifetime_s{5.0f}; // 0 => no auto-expire
    int   max_concurrent{4};
};

struct ConstructRec {
    entt::entity e{entt::null};
    std::string type;
    bool sustained{false};
    float t{0.0f};
};

class RingConstructCore : public IMechanic {
public:
    void on_attach(World&, std::uint32_t, const nlohmann::json& params) override {
        energy_ = params.value("energy_initial", 100.0f);
        preview_tint_ = params.value("preview_tint", std::string("#40FF40AA"));
        // Defaults
        set_default_defs();
        // Override from config
        if (params.contains("construct_costs") && params.at("construct_costs").is_object()) {
            for (auto& [k, v] : params.at("construct_costs").items()) {
                auto& d = defs_[k];
                d.spawn_cost     = v.value("spawn", d.spawn_cost);
                d.drain_per_s    = v.value("drain_per_s", d.drain_per_s);
                d.lifetime_s     = v.value("lifetime_s", d.lifetime_s);
                d.max_concurrent = v.value("max", d.max_concurrent);
            }
        }
        grid_size_ = params.value("freeform_grid_m", 0.25f);
        id_str_ = "generated.ring_construct_core";
    }

    void on_update(World& w, std::uint32_t owner, float dt) override {
        const nlohmann::json in = read_binding_params(w, owner).value_or(nlohmann::json::object());
        // Owner transform for spawn origin fallback
        Transform ownerT{};
        if (w.has<CTransform>(owner)) ownerT = w.get<CTransform>(owner).t;

        // Radial/menu selection
        if (in.value("y_hold", false)) radial_open_ = true;
        if (in.value("y_released", false)) {
            radial_open_ = false;
            active_category_ = in.value("radial_selection", active_category_);
        }
        selected_kind_ = in.value("sub_selection", selected_kind_);

        // Preview/guided shaping
        const bool grip = in.value("right_grip", false) || (in.value("grip_value", 0.0f) > 0.01f);
        const bool want_freeform = (selected_kind_ == "freeform_wall" || selected_kind_ == "freeform_platform");
        ptv::Transform aimT = aim_from_input(ownerT, in);
        const bool assist = in.value("left_trigger", false);
        const bool placement_ok = in.value("placement_valid", true);

        if (want_freeform && (grip || in.value("preview_force", false))) {
            ensure_preview(w, aimT, assist);
            float gv = std::clamp(in.value("grip_value", 0.5f), 0.05f, 1.5f);
            if (snap_grid_) gv = std::round(gv / grid_size_) * grid_size_;
            set_preview_scale(w, gv);
            set_preview_validity(w, placement_ok);
        } else {
            destroy_preview(w);
        }

        // Trigger actions
        const bool rt_down = in.value("right_trigger_pressed", false);
        const bool rt_held = in.value("right_trigger_held", false);
        const bool rt_up   = in.value("right_trigger_released", false);

        if (rt_down) {
            // Finalize preview if active
            if (preview_ != entt::null) {
                if (placement_ok && try_consume(defs_[selected_kind_].spawn_cost)) {
                    finalize_preview(w, selected_kind_);
                    hud_update(w, owner);
                } else {
                    // invalid -> refund if reserved? we did not consume yet; provide feedback via tag
                    tag_feedback(w, owner, "invalid_placement");
                    destroy_preview(w);
                }
            } else {
                // Spawn/activate based on selection/category
                if (is_sustained(selected_kind_)) {
                    if (!sustained_active_) {
                        if (try_consume(defs_[selected_kind_].spawn_cost) && under_limit(selected_kind_)) {
                            start_sustained(w, aimT, selected_kind_);
                            hud_update(w, owner);
                        } else {
                            tag_feedback(w, owner, "deny:energy_or_limit");
                        }
                    }
                } else {
                    // Instant
                    if (under_limit(selected_kind_) && try_consume(defs_[selected_kind_].spawn_cost)) {
                        spawn_construct(w, aimT, selected_kind_, false, false);
                        hud_update(w, owner);
                    } else {
                        tag_feedback(w, owner, "deny:energy_or_limit");
                    }
                }
            }
        }

        // Sustained maintenance
        if (sustained_active_) {
            auto it = std::find_if(recs_.begin(), recs_.end(), [&](const ConstructRec& r){return r.sustained;});
            if (it == recs_.end() || !w.registry().valid(it->e)) {
                sustained_active_ = false;
            } else {
                it->t += dt;
                const auto& def = defs_[it->type];
                if (!rt_held || !try_consume(def.drain_per_s * dt)) {
                    // stop sustained
                    safe_destroy(w, it->e);
                    recs_.erase(it);
                    sustained_active_ = false;
                    hud_update(w, owner);
                }
            }
        }
        if (rt_up && preview_ != entt::null) {
            // If player releases trigger without finalizing, cancel preview
            destroy_preview(w);
        }

        // Update instant lifetimes
        for (auto it = recs_.begin(); it != recs_.end();) {
            if (!it->sustained) {
                it->t += dt;
                float life = defs_[it->type].lifetime_s;
                if ((life > 0.0f && it->t >= life) || !w.registry().valid(it->e)) {
                    safe_destroy(w, it->e);
                    it = recs_.erase(it);
                    continue;
                }
            }
            ++it;
        }

        // Scene cleanup safety
        cleanup_zombies(w);

        // Optional target assist tag on preview
        if (preview_ != entt::null) {
            set_tag_flag(w, preview_, "assist", assist);
        }

        // Push a simple HUD energy tag on owner for diegetic HUDs
        hud_update(w, owner);
    }

    void on_detach(World& w, std::uint32_t) override {
        // Destroy all constructs and preview
        for (auto& r : recs_) safe_destroy(w, r.e);
        recs_.clear();
        destroy_preview(w);
    }

    std::string id() const override { return id_str_.empty() ? "generated.ring_construct_core" : id_str_; }

private:
    // Config/state
    std::unordered_map<std::string, ConstructDef> defs_;
    std::vector<ConstructRec> recs_;
    entt::entity preview_{entt::null};
    float energy_{100.0f};
    float grid_size_{0.25f};
    bool snap_grid_{false};
    bool sustained_active_{false};
    bool radial_open_{false};
    std::string active_category_{"attack"};
    std::string selected_kind_{"bolt"};
    std::string preview_tint_{"#40FF40AA"};
    std::string id_str_;

    void set_default_defs() {
        defs_["bolt"] = {1.0f, 0.0f, 2.0f, 12};
        defs_["fist"] = {3.0f, 0.0f, 6.0f, 4};
        defs_["sword"] = {4.0f, 0.0f, 8.0f, 2};
        defs_["round_shield"] = {3.0f, 0.0f, 8.0f, 3};
        defs_["barrier_wall"] = {5.0f, 0.0f, 12.0f, 2};
        defs_["dome"] = {8.0f, 2.0f, 0.0f, 1};
        defs_["cage"] = {6.0f, 1.5f, 0.0f, 2};
        defs_["beam"] = {2.0f, 5.0f, 0.0f, 1};
        defs_["freeform_wall"] = {4.0f, 0.0f, 20.0f, 4};
        defs_["freeform_platform"] = {4.0f, 0.0f, 20.0f, 4};
    }

    bool is_sustained(const std::string& kind) const {
        return kind == "dome" || kind == "cage" || kind == "beam";
    }

    bool try_consume(float v) {
        if (v <= 0.0f) return true;
        if (energy_ + 1e-4f < v) return false;
        energy_ -= v;
        return true;
    }

    int count_active_of(const std::string& kind) const {
        int c = 0;
        for (auto& r : recs_) if (r.type == kind) ++c;
        return c;
    }

    bool under_limit(const std::string& kind) {
        auto it = defs_.find(kind);
        int maxc = (it != defs_.end()) ? it->second.max_concurrent : 4;
        return count_active_of(kind) < maxc;
    }

    nlohmann::json::object_t::value_type::second_type dummy_{};

    std::optional<nlohmann::json> read_binding_params(World& w, std::uint32_t e) {
        if (!w.has<CMechanicBinding>(e)) return std::nullopt;
        const auto& bind = w.get<CMechanicBinding>(e);
        for (const auto& en : bind.entries) {
            if (en.id == id() || en.builtin_id == id()) {
                auto j = nlohmann::json::parse(en.params_json, nullptr, false);
                if (j.is_object()) return j;
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    Transform aim_from_input(const Transform& base, const nlohmann::json& in) {
        Transform t = base;
        if (in.contains("aim_position") && in.at("aim_position").is_array() && in.at("aim_position").size() == 3) {
            t.position.x = in.at("aim_position")[0].get<float>();
            t.position.y = in.at("aim_position")[1].get<float>();
            t.position.z = in.at("aim_position")[2].get<float>();
        }
        // rotation not driven; keep base
        float s = in.value("preview_scale", 1.0f);
        t.scale = s;
        return t;
    }

    void ensure_preview(World& w, const Transform& t, bool assist) {
        if (preview_ != entt::null && w.registry().valid(preview_)) {
            auto& tr = w.registry().get<CTransform>(preview_).t;
            tr = t;
            set_tag_flag(w, preview_, "assist", assist);
            return;
        }
        auto& reg = w.registry();
        preview_ = reg.create();
        reg.emplace<CTransform>(preview_, CTransform{t});
        CTag tag{};
        tag.tags = {"construct","preview","tint:"+preview_tint_};
        if (assist) tag.tags.push_back("assist");
        reg.emplace<CTag>(preview_, std::move(tag));
        CMesh mesh{};
        mesh.asset_id = "vfx.construct.preview";
        mesh.glb_path = "";
        mesh.target_scale_meters = t.scale;
        reg.emplace<CMesh>(preview_, std::move(mesh));
    }

    void set_preview_scale(World& w, float s) {
        if (preview_ == entt::null || !w.registry().valid(preview_)) return;
        auto& tr = w.registry().get<CTransform>(preview_).t;
        tr.scale = std::clamp(s, 0.05f, 10.0f);
        if (w.registry().all_of<CMesh>(preview_)) {
            w.registry().get<CMesh>(preview_).target_scale_meters = tr.scale;
        }
    }

    void set_preview_validity(World& w, bool ok) {
        if (preview_ == entt::null || !w.registry().valid(preview_)) return;
        auto& tag = w.registry().get<CTag>(preview_);
        auto has = [&](const std::string& s){ return std::find(tag.tags.begin(), tag.tags.end(), s) != tag.tags.end(); };
        auto add = [&](const std::string& s){ if (!has(s)) tag.tags.push_back(s); };
        auto remove = [&](const std::string& s){ tag.tags.erase(std::remove(tag.tags.begin(), tag.tags.end(), s), tag.tags.end()); };
        if (ok) { remove("invalid"); add("valid"); }
        else { remove("valid"); add("invalid"); }
    }

    void finalize_preview(World& w, const std::string& kind) {
        if (preview_ == entt::null || !w.registry().valid(preview_)) return;
        // Turn preview into final construct
        auto& tag = w.registry().get<CTag>(preview_);
        tag.tags.erase(std::remove(tag.tags.begin(), tag.tags.end(), "preview"), tag.tags.end());
        tag.tags.push_back("type:"+kind);
        if (w.registry().all_of<CMesh>(preview_)) {
            auto& m = w.registry().get<CMesh>(preview_);
            m.asset_id = "vfx.construct."+kind;
        } else {
            CMesh m{}; m.asset_id = "vfx.construct."+kind; m.glb_path = ""; m.target_scale_meters = w.registry().get<CTransform>(preview_).t.scale;
            w.registry().emplace<CMesh>(preview_, m);
        }
        recs_.push_back(ConstructRec{preview_, kind, false, 0.0f});
        preview_ = entt::null;
    }

    void destroy_preview(World& w) {
        if (preview_ != entt::null && w.registry().valid(preview_)) {
            w.registry().destroy(preview_);
        }
        preview_ = entt::null;
    }

    void start_sustained(World& w, const Transform& t, const std::string& kind) {
        auto e = spawn_construct(w, t, kind, false, true);
        (void)e;
        sustained_active_ = true;
    }

    entt::entity spawn_construct(World& w, const Transform& t, const std::string& kind, bool preview, bool sustained) {
        auto& reg = w.registry();
        entt::entity e = reg.create();
        reg.emplace<CTransform>(e, CTransform{t});
        CMesh mesh{}; mesh.asset_id = preview ? "vfx.construct.preview" : ("vfx.construct."+kind); mesh.glb_path = ""; mesh.target_scale_meters = t.scale;
        reg.emplace<CMesh>(e, std::move(mesh));
        CTag tag{}; tag.tags = {"construct", preview?"preview":"final", "type:"+kind};
        reg.emplace<CTag>(e, std::move(tag));
        recs_.push_back(ConstructRec{e, kind, sustained, 0.0f});
        return e;
    }

    void set_tag_flag(World& w, entt::entity e, const std::string& flag, bool on) {
        if (!w.registry().valid(e) || !w.registry().all_of<CTag>(e)) return;
        auto& tag = w.registry().get<CTag>(e);
        auto it = std::find(tag.tags.begin(), tag.tags.end(), flag);
        if (on) { if (it == tag.tags.end()) tag.tags.push_back(flag); }
        else { if (it != tag.tags.end()) tag.tags.erase(it); }
    }

    void safe_destroy(World& w, entt::entity e) {
        if (w.registry().valid(e)) w.registry().destroy(e);
    }

    void cleanup_zombies(World& w) {
        for (auto it = recs_.begin(); it != recs_.end();) {
            if (!w.registry().valid(it->e)) it = recs_.erase(it);
            else ++it;
        }
        if (preview_ != entt::null && !w.registry().valid(preview_)) preview_ = entt::null;
    }

    void hud_update(World& w, std::uint32_t owner) {
        if (!w.has<CTag>(owner)) { w.emplace_or_replace<CTag>(owner, CTag{}); }
        auto& tag = w.get<CTag>(owner);
        // remove old energy tag
        for (auto it = tag.tags.begin(); it != tag.tags.end();) {
            if (it->rfind("hud:energy=", 0) == 0) it = tag.tags.erase(it);
            else ++it;
        }
        tag.tags.push_back("hud:energy="+std::to_string(static_cast<int>(std::round(energy_))));
    }

    void tag_feedback(World& w, std::uint32_t owner, const std::string& code) {
        if (!w.has<CTag>(owner)) { w.emplace_or_replace<CTag>(owner, CTag{}); }
        auto& tag = w.get<CTag>(owner);
        tag.tags.push_back("feedback:"+code);
        // Optional: could prune later; harmless
    }
};

} // namespace ptv

PTV_REGISTER_MECHANIC("generated.ring_construct_core", ptv::RingConstructCore)