#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <entt/entt.hpp>
#include <string>
#include <unordered_map>
#include <optional>
#include <vector>
#include <cmath>
#include <algorithm>

// Forward-declare haptics surface provided by dependency task.
namespace ptv { namespace haptics {
    void publish(const std::string& pattern, const std::string& hand_mask, float intensity, std::optional<float> duration_s);
    bool has_pattern(const std::string&);
    void set_user_intensity(float);
    float get_user_intensity();
    const char* pattern_to_string(int); // not used here
    const char* hand_to_string(int);    // not used here
}} // namespace ptv::haptics

namespace ptv {

namespace heavy_internal {
    enum class Form : std::uint8_t { None=0, Turret, MechArm, JetDrill, FistSwarm };

    struct Cooldowns {
        float turret{0.0f};
        float mech_arm{0.0f};
        float jet_drill{0.0f};
        float fist_swarm{0.0f};
    };

    struct Costs { float turret{8.0f}; float mech_arm{10.0f}; float jet_drill{12.0f}; float fist_swarm{10.0f}; };

    struct LiveConstructRef { entt::entity e{entt::null}; Form form{Form::None}; float t{0.0f}; };

    struct PerEntityState {
        // Config
        float will_focus_req{0.6f};
        float cooldown_s{20.0f};
        Costs costs{};
        // Runtime
        float charge_t{0.0f};
        bool charging{false};
        bool two_hand_stabilize{false};
        Cooldowns cds{};
        std::vector<LiveConstructRef> lives; // active spawned constructs bound to this source entity
        // Soft resources seen from params each frame (driven by external system)
        float last_focus{0.0f};
        float last_ring_energy{100.0f};
        // Preview orientation cache (for comfort – avoids roll)
        Vec3 cached_forward{0.0f,0.0f,1.0f};
    };

    // Simple math helpers
    inline Vec3 forward_of(const Quat& q){
        // q * (0,0,1)
        const float x = 2.0f*(q.x*q.z + q.w*q.y);
        const float y = 2.0f*(q.y*q.z - q.w*q.x);
        const float z = 1.0f - 2.0f*(q.x*q.x + q.y*q.y);
        return Vec3{x,y,z};
    }

    inline Quat look_rotation_level(const Vec3& fwd){
        // Level (no roll). Up = +Y. Build basis then convert to quaternion.
        Vec3 f = fwd; // assume normalized-ish
        const float len = std::sqrt(f.x*f.x+f.y*f.y+f.z*f.z);
        if(len>1e-5f){ f.x/=len; f.y/=len; f.z/=len; }
        // project to XZ to remove roll/pitch coupling; keep some pitch but clamp roll by rebuilding up
        Vec3 u{0.0f,1.0f,0.0f};
        // Right = normalize(cross(up, f))
        Vec3 r{ u.y*f.z - u.z*f.y, u.z*f.x - u.x*f.z, u.x*f.y - u.y*f.x };
        float rl = std::sqrt(r.x*r.x+r.y*r.y+r.z*r.z); if(rl>1e-5f){ r.x/=rl; r.y/=rl; r.z/=rl; }
        // Recompute leveled forward = cross(right, up)
        Vec3 lf{ r.y*u.z - r.z*u.y, r.z*u.x - r.x*u.z, r.x*u.y - r.y*u.x };
        // Build rotation matrix columns [r u lf]
        float m00=r.x, m01=u.x, m02=lf.x;
        float m10=r.y, m11=u.y, m12=lf.y;
        float m20=r.z, m21=u.z, m22=lf.z;
        float t = m00 + m11 + m22;
        Quat q{1,0,0,0};
        if(t>0.0f){
            float s = std::sqrt(t+1.0f)*2.0f; // s=4*qw
            q.w = 0.25f*s;
            q.x = (m21 - m12)/s;
            q.y = (m02 - m20)/s;
            q.z = (m10 - m01)/s;
        }
        return q;
    }

    inline bool has_tag(const CTag& tag, const std::string& key){
        return std::find(tag.tags.begin(), tag.tags.end(), key)!=tag.tags.end();
    }

    inline entt::entity spawn_basic(World& w, const std::string& id_hint, const std::string& asset_id, const std::string& glb,
                                    const Transform& t, float target_scale=1.0f){
        auto& reg = w.registry();
        entt::entity e = reg.create();
        std::uint32_t u = static_cast<std::uint32_t>(e);
        w.emplace_or_replace<CTransform>(u, CTransform{t});
        CMesh m{}; m.asset_id=asset_id; m.glb_path=glb; m.target_scale_meters=target_scale; w.emplace_or_replace<CMesh>(u, std::move(m));
        CPhysics p{}; p.kind="dynamic"; p.mass_kg=1.0f; p.restitution=0.1f; p.friction=0.6f; p.collision_shape="convex"; w.emplace_or_replace<CPhysics>(u, std::move(p));
        CTag tag{}; tag.tags = {"construct", id_hint}; w.emplace_or_replace<CTag>(u, std::move(tag));
        return e;
    }

    // Enemy query: find nearest enemy with CTransform and tag "enemy"
    inline std::optional<Vec3> nearest_enemy_pos(World& w, const Vec3& from){
        auto& reg = w.registry();
        float best=FLT_MAX; Vec3 pos{}; bool found=false;
        auto view = reg.view<CTransform, CTag>();
        for(auto ent : view){
            auto& tr = view.get<CTransform>(ent).t;
            const CTag& tg = view.get<CTag>(ent);
            if(std::find(tg.tags.begin(), tg.tags.end(), std::string("enemy"))==tg.tags.end()) continue;
            Vec3 d{tr.position.x-from.x, tr.position.y-from.y, tr.position.z-from.z};
            float ds = d.x*d.x+d.y*d.y+d.z*d.z;
            if(ds < best){ best=ds; pos = tr.position; found=true; }
        }
        if(found) return pos; else return std::nullopt;
    }

    // Forward declarations for per-form ticks implemented in split files
    void tick_turret(World& w, std::uint32_t source, LiveConstructRef& ref, float dt);
    void tick_mech_arm(World& w, std::uint32_t source, LiveConstructRef& ref, float dt);
    void tick_jet_drill(World& w, std::uint32_t source, LiveConstructRef& ref, float dt);
    void tick_fist_swarm(World& w, std::uint32_t source, LiveConstructRef& ref, float dt);
}

class HeavySignatureConstructs : public IMechanic {
public:
    void on_attach(World& w, std::uint32_t e, const nlohmann::json& p) override {
        auto& S = state_[e];
        S.will_focus_req = p.value("willpower_focus_required", 0.6f);
        S.cooldown_s     = p.value("cooldown_s", 20.0f);
        if(p.contains("per_construct_costs")){
            auto pc = p.at("per_construct_costs");
            S.costs.turret     = pc.value("turret", 8.0f);
            S.costs.mech_arm   = pc.value("mech_arm", 10.0f);
            S.costs.jet_drill  = pc.value("jet_drill", 12.0f);
            S.costs.fist_swarm = pc.value("fist_swarm", 10.0f);
        }
        // Tag to help HUDs discover cooldowns for this source entity
        CTag hud{}; hud.tags = {"heavy_constructs","cooldowns"}; w.emplace_or_replace<CTag>(e, std::move(hud));
    }

    void on_update(World& w, std::uint32_t e, float dt) override {
        if(!w.valid(e)) return;
        auto it = state_.find(e); if(it==state_.end()) return; auto& S = it->second;
        // Read pose
        if(!w.has<CTransform>(e)) return; auto& tr = w.get<CTransform>(e).t;

        // Pull dynamic control params from this entity's binding entry for our mechanic id.
        bool right_trigger=false, right_trigger_released=false, left_grip=false; 
        heavy_internal::Form selected = heavy_internal::Form::None;
        float ext_focus=S.last_focus, ext_ring=S.last_ring_energy;
        if(w.has<CMechanicBinding>(e)){
            const auto& mb = w.get<CMechanicBinding>(e);
            for(const auto& entry : mb.entries){
                if(entry.id==id()){
                    auto j = nlohmann::json::parse(entry.params_json, nullptr, false);
                    if(j.is_object()){
                        right_trigger = j.value("right_trigger", false);
                        right_trigger_released = j.value("right_trigger_released", false);
                        left_grip = j.value("left_grip", false);
                        std::string sel = j.value("selected","none");
                        if(sel=="construct_turret") selected = heavy_internal::Form::Turret;
                        else if(sel=="construct_mech_arm") selected = heavy_internal::Form::MechArm;
                        else if(sel=="construct_energy_jet_drill") selected = heavy_internal::Form::JetDrill;
                        else if(sel=="construct_fist_swarm") selected = heavy_internal::Form::FistSwarm;
                        ext_focus = j.value("willpower_focus", S.last_focus);
                        ext_ring  = j.value("ring_energy", S.last_ring_energy);
                    }
                    break;
                }
            }
        }
        S.last_focus = ext_focus; S.last_ring_energy = ext_ring; S.two_hand_stabilize = left_grip;

        // Cooldowns tick down
        S.cds.turret     = std::max(0.0f, S.cds.turret - dt);
        S.cds.mech_arm   = std::max(0.0f, S.cds.mech_arm - dt);
        S.cds.jet_drill  = std::max(0.0f, S.cds.jet_drill - dt);
        S.cds.fist_swarm = std::max(0.0f, S.cds.fist_swarm - dt);

        // Simple two-hand stabilization: cache leveled forward, blend when left grip held
        Vec3 ring_fwd = heavy_internal::forward_of(tr.rotation);
        if(S.two_hand_stabilize){
            // Low-pass toward cached_forward
            float a = 1.0f - std::exp(-dt*10.0f);
            S.cached_forward.x = (1.0f-a)*S.cached_forward.x + a*ring_fwd.x;
            S.cached_forward.y = (1.0f-a)*S.cached_forward.y + a*ring_fwd.y;
            S.cached_forward.z = (1.0f-a)*S.cached_forward.z + a*ring_fwd.z;
        } else {
            S.cached_forward = ring_fwd;
        }

        // Charge-up gating by focus
        const bool focus_ok = (S.last_focus >= S.will_focus_req);
        if(right_trigger && focus_ok){
            S.charging = true; S.charge_t += dt;
            // Resonance haptics while charging
            if(ptv::haptics::has_pattern("resonance")){
                ptv::haptics::publish("resonance","right", std::clamp(0.4f + 0.6f*std::min(S.charge_t/1.5f,1.0f), 0.0f, 1.0f), std::nullopt);
            }
        }
        if(right_trigger_released){
            if(S.charging && focus_ok){
                try_finalize_spawn(w, e, S, selected, tr);
            }
            S.charging=false; S.charge_t=0.0f;
        }

        // Drive live constructs simulation
        for(auto& live : S.lives){
            switch(live.form){
                case heavy_internal::Form::Turret:    heavy_internal::tick_turret(w, e, live, dt); break;
                case heavy_internal::Form::MechArm:   heavy_internal::tick_mech_arm(w, e, live, dt); break;
                case heavy_internal::Form::JetDrill:  heavy_internal::tick_jet_drill(w, e, live, dt); break;
                case heavy_internal::Form::FistSwarm: heavy_internal::tick_fist_swarm(w, e, live, dt); break;
                default: break;
            }
        }
        // Cull dead
        S.lives.erase(std::remove_if(S.lives.begin(), S.lives.end(), [&](const heavy_internal::LiveConstructRef& r){ return r.e==entt::null || !w.registry().valid(r.e); }), S.lives.end());

        // Expose cooldowns via CTag update for HUD queries (namespaced keys)
        CTag tag; tag.tags = {
            std::string("heavy_cd_turret:")+std::to_string(S.cds.turret),
            std::string("heavy_cd_mech_arm:")+std::to_string(S.cds.mech_arm),
            std::string("heavy_cd_jet_drill:")+std::to_string(S.cds.jet_drill),
            std::string("heavy_cd_fist_swarm:")+std::to_string(S.cds.fist_swarm)
        }; w.emplace_or_replace<CTag>(e, std::move(tag));
    }

    void on_detach(World& w, std::uint32_t e) override {
        auto it = state_.find(e); if(it==state_.end()) return;
        // Best-effort: despawn any constructs we created that still exist
        for(auto& live : it->second.lives){ if(w.registry().valid(live.e)) w.registry().destroy(live.e); }
        state_.erase(it);
        if(w.valid(e)) w.remove<CTag>(e);
    }

    std::string id() const override { return "generated.heavy_signature_constructs"; }

private:
    void try_finalize_spawn(World& w, std::uint32_t e, heavy_internal::PerEntityState& S, heavy_internal::Form sel, const Transform& hand_tr){
        if(sel==heavy_internal::Form::None) return;
        // Check per-form cooldown & mock cost vs available ring energy (if provided)
        auto ok_and_consume = [&](float& cd_slot, float cost)->bool{
            if(cd_slot>0.0f) return false;
            if(S.last_ring_energy < cost) return false; // soft gate; external system would actually deduct
            cd_slot = S.cooldown_s; // set cooldown immediately on deploy
            return true;
        };

        Transform spawn = hand_tr; // place slightly forward
        Vec3 fwd = heavy_internal::forward_of(hand_tr.rotation);
        spawn.position.x += fwd.x * 0.8f; spawn.position.y += fwd.y * 0.2f; spawn.position.z += fwd.z * 0.8f;
        spawn.rotation = heavy_internal::look_rotation_level(fwd);

        using Form = heavy_internal::Form;
        heavy_internal::LiveConstructRef ref; ref.t=0.0f;
        switch(sel){
            case Form::Turret:
                if(!ok_and_consume(S.cds.turret, S.costs.turret)) return;
                ref.form = Form::Turret;
                ref.e = heavy_internal::spawn_basic(w, "construct_turret", "construct_turret", "assets/construct_turret.glb", spawn, 1.0f);
                pulse_heavy_confirm();
                break;
            case Form::MechArm:
                if(!ok_and_consume(S.cds.mech_arm, S.costs.mech_arm)) return;
                ref.form = Form::MechArm;
                ref.e = heavy_internal::spawn_basic(w, "construct_mech_arm", "construct_mech_arm", "assets/construct_mech_arm.glb", spawn, 1.2f);
                pulse_heavy_confirm();
                break;
            case Form::JetDrill:
                if(!ok_and_consume(S.cds.jet_drill, S.costs.jet_drill)) return;
                ref.form = Form::JetDrill;
                ref.e = heavy_internal::spawn_basic(w, "construct_energy_jet_drill", "construct_energy_jet_drill", "assets/construct_energy_jet_drill.glb", spawn, 1.0f);
                pulse_heavy_confirm();
                break;
            case Form::FistSwarm:
                if(!ok_and_consume(S.cds.fist_swarm, S.costs.fist_swarm)) return;
                ref.form = Form::FistSwarm;
                ref.e = heavy_internal::spawn_basic(w, "construct_fist_swarm_anchor", "construct_fist_swarm", "assets/construct_fist_swarm.glb", spawn, 0.6f);
                pulse_heavy_confirm();
                break;
            default: return;
        }
        S.lives.push_back(ref);
    }

    void pulse_heavy_confirm(){
        if(ptv::haptics::has_pattern("heavy")){
            ptv::haptics::publish("heavy","right", 1.0f, 0.12f);
        }
    }

    std::unordered_map<std::uint32_t, heavy_internal::PerEntityState> state_{};
};

} // namespace ptv

PTV_REGISTER_MECHANIC("generated.heavy_signature_constructs", ptv::HeavySignatureConstructs)
