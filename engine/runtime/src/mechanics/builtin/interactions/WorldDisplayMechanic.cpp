// builtin.world_display -- live text on a screen, scoreboard or sign IN the
// world.
//
// The shooting range's timed-challenge HUD floated, empty: the plan mapped it
// to builtin.game_hud, which is the player's WRIST HUD, and the game-flow
// pass then stripped that from every entity but its director. A screen in the
// world is a different thing, so it has its own built-in. It draws text lines
// on the display's face with ctx.text, from templates over ctx.vars:
//
//     "TIME {lane2.time_left:0.0}s"   ->  "TIME 14.3s"
//     "HITS {lane2.score}  BEST {lane2.best}"
//
// The face is the mesh's ``screen`` socket when it has one (+Z out of the
// screen), otherwise whichever vertical face of its collider looks toward the
// player. Text is sized to fit the face.
//
// Params: lines (array of templates), title (string, drawn larger on top),
// color ([r, g, b]), title_color, text_height_m (0 = fit), screen_socket
// ("screen"), margin (0.08 = fraction of the face kept clear).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../BuiltinMechanicBase.h"
#include "ptv/api/Facing.h"

namespace ptv {
namespace {

/** "{name}" / "{name:0.0}" / "{name:00}" -> the var's value. */
std::string expand(const std::string& tmpl, const IVars* vars) {
    std::string out;
    out.reserve(tmpl.size() + 16);
    for (std::size_t i = 0; i < tmpl.size();) {
        if (tmpl[i] != '{') {
            out += tmpl[i++];
            continue;
        }
        const std::size_t close = tmpl.find('}', i);
        if (close == std::string::npos) {
            out += tmpl.substr(i);
            break;
        }
        const std::string inner = tmpl.substr(i + 1, close - i - 1);
        const std::size_t colon = inner.find(':');
        const std::string name = inner.substr(0, colon);
        const std::string fmt = colon == std::string::npos ? "" : inner.substr(colon + 1);
        i = close + 1;
        if (vars == nullptr || !vars->has(name.c_str())) {
            out += "-";
            continue;
        }
        const float v = vars->number(name.c_str(), std::nanf(""));
        if (std::isnan(v)) {
            out += vars->text(name.c_str());
            continue;
        }
        const std::size_t dot = fmt.find('.');
        const int decimals = dot == std::string::npos ? 0 : static_cast<int>(fmt.size() - dot - 1);
        const int width = static_cast<int>(dot == std::string::npos ? fmt.size() : dot);
        char buf[48];
        if (fmt.empty()) {
            std::snprintf(buf, sizeof(buf), "%s", vars->text(name.c_str()));
        } else {
            std::snprintf(buf, sizeof(buf), "%0*.*f", std::max(0, width + (decimals ? decimals + 1 : 0)),
                          decimals, static_cast<double>(v));
        }
        out += buf;
    }
    return out;
}

Vec4 read_color(const nlohmann::json& p, const char* key, const Vec4& fallback) {
    if (!p.contains(key) || !p[key].is_array() || p[key].size() < 3) return fallback;
    return Vec4(p[key][0].get<float>(), p[key][1].get<float>(), p[key][2].get<float>(), 1.0f);
}

class WorldDisplayMechanic final : public BuiltinMechanicBase<WorldDisplayMechanic> {
public:
    static constexpr const char* kId = "builtin.world_display";
    MechanicPhase phase() const override { return MechanicPhase::Simulate; }
    MechanicScope scope() const override { return MechanicScope::Entity; }

    void configure(const nlohmann::json& p) {
        lines_.clear();
        for (const auto& l : p.value("lines", nlohmann::json::array())) {
            if (l.is_string()) lines_.push_back(l.get<std::string>());
        }
        title_ = p.value("title", std::string());
        color_ = read_color(p, "color", Vec4(0.95f, 0.97f, 1.0f, 1.0f));
        title_color_ = read_color(p, "title_color", Vec4(1.0f, 0.35f, 0.25f, 1.0f));
        text_height_ = std::max(0.0f, p.value("text_height_m", 0.0f));
        socket_ = p.value("screen_socket", std::string("screen"));
        margin_ = std::clamp(p.value("margin", 0.08f), 0.0f, 0.4f);
    }

    void tick(World& world, std::uint32_t self, float, const InputSnapshot& input,
              MechanicContext& ctx) {
        if (ctx.text == nullptr || !world.valid(self) || !world.has<CTransform>(self)) return;
        if (lines_.empty() && title_.empty()) return;
        Vec3 centre(0.0f), normal(0.0f, 0.0f, 1.0f), up(0.0f, 1.0f, 0.0f);
        float face_w = 1.0f, face_h = 0.6f;
        face(world, self, input, ctx, centre, normal, up, face_w, face_h);

        std::vector<std::string> rows;
        if (!title_.empty()) rows.push_back(expand(title_, ctx.vars));
        for (const auto& l : lines_) rows.push_back(expand(l, ctx.vars));
        const float usable_h = face_h * (1.0f - 2.0f * margin_);
        const float usable_w = face_w * (1.0f - 2.0f * margin_);
        float h = text_height_ > 0.0f ? text_height_
                                      : usable_h / (1.45f * static_cast<float>(rows.size()) + 0.3f);
        // Shrink until the widest row fits.
        for (const auto& r : rows) {
            const float w = ctx.text->measure(r.c_str(), h);
            if (w > usable_w && w > 1e-4f) h *= usable_w / w;
        }
        h = std::clamp(h, 0.01f, 0.5f);
        const Quat rot = rotation_facing(-normal);  // text faces +Z = normal
        const float step = 1.45f * h;
        Vec3 p = centre + up * (0.5f * step * static_cast<float>(rows.size() - 1)) -
                 up * (0.5f * h) + normal * 0.006f;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const bool is_title = !title_.empty() && i == 0;
            ctx.text->text(rows[i].c_str(), p, rot, is_title ? h * 1.1f : h,
                           is_title ? title_color_ : color_, TextAlign::Center);
            p -= up * step;
        }
    }

private:
    void face(World& world, std::uint32_t self, const InputSnapshot& input, MechanicContext& ctx,
              Vec3& centre, Vec3& normal, Vec3& up, float& w, float& h) const {
        const auto& t = world.get<CTransform>(self).t;
        if (ctx.sockets != nullptr && ctx.sockets->has_socket(self, socket_.c_str())) {
            const Mat4 m = ctx.sockets->socket_world(self, socket_.c_str());
            centre = Vec3(m[3]);
            normal = glm::normalize(Vec3(m[2]));
            up = glm::normalize(Vec3(m[1]));
        }
        if (!world.has<CCollisionGeom>(self)) {
            if (!(ctx.sockets != nullptr && ctx.sockets->has_socket(self, socket_.c_str()))) {
                centre = t.position;
                normal = t.rotation * Vec3(0.0f, 0.0f, 1.0f);
            }
            return;
        }
        const auto& g = world.get<CCollisionGeom>(self);
        const Vec3 lo = g.aabb_min * t.scale, hi = g.aabb_max * t.scale;
        const Vec3 ext = hi - lo;
        if (ctx.sockets != nullptr && ctx.sockets->has_socket(self, socket_.c_str())) {
            // Face size: the box extents across the screen plane.
            const Vec3 ln = glm::inverse(t.rotation) * normal;
            const bool along_x = std::abs(ln.x) > std::abs(ln.z);
            w = along_x ? ext.z : ext.x;
            h = ext.y;
            return;
        }
        // The vertical face of the box that looks most toward the viewer.
        const Vec3 viewer = input.head_valid ? input.head_position : t.position + Vec3(0, 0, 5);
        const Vec3 c_local = 0.5f * (lo + hi);
        const Vec3 to_view = glm::inverse(t.rotation) * (viewer - (t.position + t.rotation * c_local));
        Vec3 ln(0.0f), face_c = c_local;
        if (std::abs(to_view.x) * ext.z > std::abs(to_view.z) * ext.x) {
            ln = Vec3(to_view.x >= 0 ? 1.0f : -1.0f, 0.0f, 0.0f);
            face_c.x = to_view.x >= 0 ? hi.x : lo.x;
            w = ext.z;
        } else {
            ln = Vec3(0.0f, 0.0f, to_view.z >= 0 ? 1.0f : -1.0f);
            face_c.z = to_view.z >= 0 ? hi.z : lo.z;
            w = ext.x;
        }
        h = ext.y;
        centre = t.position + t.rotation * face_c;
        normal = glm::normalize(t.rotation * ln);
        up = glm::normalize(t.rotation * Vec3(0.0f, 1.0f, 0.0f));
    }

    std::vector<std::string> lines_;
    std::string title_;
    std::string socket_;
    Vec4 color_{1.0f};
    Vec4 title_color_{1.0f};
    float text_height_{0.0f};
    float margin_{0.08f};
};

}  // namespace

extern "C" int ptv_force_link_builtin_world_display = 1;

PTV_REGISTER_MECHANIC("builtin.world_display", WorldDisplayMechanic)

}  // namespace ptv
