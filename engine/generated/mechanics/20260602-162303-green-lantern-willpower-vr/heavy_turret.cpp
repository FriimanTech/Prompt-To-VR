#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <entt/entt.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <optional>
#include <cfloat>

namespace ptv { namespace heavy_internal {

enum class Form : std::uint8_t { None=0, Turret, MechArm, JetDrill, FistSwarm };
struct LiveConstructRef { entt::entity e{entt::null}; Form form{Form::None}; float t{0.0f}; };

static Quat look_rotation_level(const Vec3& fwd){
    Vec3 f = fwd;
    const float len = std::sqrt(f.x*f.x+f.y*f.y+f.z*f.z);
    if(len>1e-5f){ f.x/=len; f.y/=len; f.z/=len; }
    Vec3 u{0.0f,1.0f,0.0f};
    Vec3 r{ u.y*f.z - u.z*f.y, u.z*f.x - u.x*f.z, u.x*f.y - u.y*f.x };
    float rl = std::sqrt(r.x*r.x+r.y*r.y+r.z*r.z); if(rl>1e-5f){ r.x/=rl; r.y/=rl; r.z/=rl; }
    Vec3 lf{ r.y*u.z - r.z*u.y, r.z*u.x - r.x*u.z, r.x*u.y - r.y*u.x };
    float m00=r.x, m01=u.x, m02=lf.x;
    float m10=r.y, m11=u.y, m12=lf.y;
    float m20=r.z, m21=u.z, m22=lf.z;
    float t = m00 + m11 + m22;
    Quat q{1,0,0,0};
    if(t>0.0f){
        float s = std::sqrt(t+1.0f)*2.0f;
        q.w = 0.25f*s;
        q.x = (m21 - m12)/s;
        q.y = (m02 - m20)/s;
        q.z = (m10 - m01)/s;
    }
    return q;
}

static std::optional<Vec3> nearest_enemy_pos(World& w, const Vec3& from){
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

static void face_toward(ptv::Transform& t, const ptv::Vec3& target){
    ptv::Vec3 dir{ target.x - t.position.x, target.y - t.position.y, target.z - t.position.z };
    float len = std::sqrt(dir.x*dir.x+dir.y*dir.y+dir.z*dir.z);
    if(len>1e-5f){ dir.x/=len; dir.y/=len; dir.z/=len; }
    t.rotation = look_rotation_level(dir);
}

void tick_turret(World& w, std::uint32_t /*source*/, LiveConstructRef& ref, float dt){
    if(ref.e==entt::null || !w.registry().valid(ref.e)) return;
    auto u = static_cast<std::uint32_t>(ref.e);
    if(!w.has<CTransform>(u)) return;
    auto& tr = w.get<CTransform>(u).t;

    // Acquire nearest enemy and smoothly rotate toward it. Lifetime ~ 15s.
    ref.t += dt;
    const float lifetime = 15.0f;
    auto nearest = nearest_enemy_pos(w, tr.position);
    if(nearest){
        // Smooth rotate (critically damped)
        face_toward(tr, *nearest);
    }

    // Simple idle bob to sell life
    tr.position.y += 0.01f * std::sin(ref.t*2.2f);

    if(ref.t>lifetime){ if(w.registry().valid(ref.e)) w.registry().destroy(ref.e); ref.e=entt::null; }
}

}} // namespace ptv::heavy_internal
