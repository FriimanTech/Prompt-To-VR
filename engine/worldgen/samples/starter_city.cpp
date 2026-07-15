// Hand-authored IWorldBuilder sample (no LLM). Compiled only when the host is
// configured with -DPTV_WORLDGEN_SAMPLES=ON. It is the environment analog of
// samples/starter-room: a deterministic reference world that exercises the full
// WorldContext surface (sky, ground, lights, primitives, convenience builders,
// an arbitrary add_mesh, and player_spawn) so the host + runtime path can be
// validated without spending any API credits.
//
// It intentionally includes ONLY the public interface header -- proof that the
// authoring surface is self-sufficient and never needs engine internals.

#include "ptv/api/IWorldBuilder.h"

namespace ptv::world {

namespace {

// A simple procedural pyramid emitted via add_mesh() to exercise arbitrary
// geometry + triangle-mesh collision.
MeshDesc make_pyramid(const Vec3& center, float base, float height,
                      const Color& color) {
    const float h = base * 0.5f;
    // 4 base corners + apex.
    const float bx0 = center.x - h, bx1 = center.x + h;
    const float bz0 = center.z - h, bz1 = center.z + h;
    const float by = center.y;
    const float ay = center.y + height;
    MeshDesc m{};
    m.color = color;
    m.collision = Collision::Mesh;
    m.tags = {"landmark"};
    m.name = "pyramid";
    m.positions = {
        bx0, by, bz0,  // 0
        bx1, by, bz0,  // 1
        bx1, by, bz1,  // 2
        bx0, by, bz1,  // 3
        center.x, ay, center.z,  // 4 apex
    };
    m.indices = {
        0, 1, 2, 0, 2, 3,  // base
        0, 4, 1,           // sides
        1, 4, 2,
        2, 4, 3,
        3, 4, 0,
    };
    return m;
}

}  // namespace

class StarterCityWorld final : public IWorldBuilder {
public:
    void build(WorldContext& w) override {
        // Dusk sky. Also exercises the author-controlled environment knobs
        // (ambient fill, fog, exposure) so this sample validates the full
        // lighting surface, not just the sky gradient.
        SkyDesc sky{};
        sky.top = Color{0.04f, 0.06f, 0.14f};
        sky.horizon = Color{0.85f, 0.45f, 0.30f};
        sky.sun_direction = Vec3{0.35f, -0.55f, 0.30f};
        sky.sun_color = Color{1.0f, 0.85f, 0.65f};
        sky.sun_intensity = 1.2f;
        sky.ambient_color = Color{0.22f, 0.20f, 0.26f};  // cool ground bounce
        sky.ambient_intensity = 0.35f;                   // readable baseline
        sky.fog_density = 0.010f;                         // subtle depth haze
        sky.exposure = 1.05f;                             // slight lift
        w.sky(sky);

        // Ground plaza with collision.
        GroundDesc ground{};
        ground.size_m = 200.0f;
        ground.color = Color{0.16f, 0.17f, 0.2f};
        ground.grid = true;
        w.ground(ground);

        // Key + fill lights.
        LightDesc sun{};
        sun.kind = "directional";
        sun.direction = Vec3{0.35f, -0.55f, 0.30f};
        sun.color = Color{1.0f, 0.9f, 0.78f};
        sun.intensity = 1.4f;
        w.light(sun);

        LightDesc fill{};
        fill.kind = "point";
        fill.position = Vec3{0.0f, 6.0f, 0.0f};
        fill.color = Color{0.4f, 0.55f, 1.0f};
        fill.intensity = 1.0f;
        fill.range = 30.0f;  // author-controlled falloff radius
        w.light(fill);

        // A grid of buildings around a central plaza.
        helpers::BuildingGridDesc grid{};
        grid.origin = Vec3{-70.0f, 0.0f, -70.0f};
        grid.cols = 7;
        grid.rows = 7;
        grid.spacing = 22.0f;
        grid.footprint = 11.0f;
        grid.min_height = 8.0f;
        grid.max_height = 52.0f;
        grid.color = Color{0.3f, 0.33f, 0.42f};
        grid.seed = w.seed();
        helpers::building_grid(w, grid);

        // A boundary wall shell around the play area.
        helpers::WallShellDesc shell{};
        shell.width = 40.0f;
        shell.depth = 40.0f;
        shell.height = 3.0f;
        shell.color = Color{0.35f, 0.37f, 0.45f};
        helpers::wall_shell(w, shell);

        // Two roads crossing the plaza.
        helpers::PathDesc road_ns{};
        road_ns.start = Vec3{0.0f, 0.02f, -70.0f};
        road_ns.end = Vec3{0.0f, 0.02f, 70.0f};
        road_ns.width = 6.0f;
        helpers::path(w, road_ns);

        helpers::PathDesc road_ew{};
        road_ew.start = Vec3{-70.0f, 0.02f, 0.0f};
        road_ew.end = Vec3{70.0f, 0.02f, 0.0f};
        road_ew.width = 6.0f;
        helpers::path(w, road_ew);

        // Steps up to a viewing platform.
        helpers::PlatformSetDesc steps{};
        steps.origin = Vec3{10.0f, 0.0f, 10.0f};
        steps.count = 5;
        steps.step_up = 0.9f;
        steps.step_forward = 2.6f;
        helpers::platform_set(w, steps);

        // A hero landmark built from an arbitrary procedural mesh.
        w.add_mesh(make_pyramid(Vec3{0.0f, 0.0f, -14.0f}, 10.0f, 16.0f,
                                Color{0.8f, 0.7f, 0.25f}));

        // A couple of colored pillar primitives near spawn.
        for (int i = -1; i <= 1; i += 2) {
            PrimitiveDesc pillar{};
            pillar.kind = PrimitiveKind::Cylinder;
            pillar.size = Vec3{1.0f, 5.0f, 1.0f};
            pillar.transform.position = Vec3{static_cast<float>(i) * 3.0f, 2.5f, -4.0f};
            pillar.color = Color{0.6f, 0.62f, 0.7f};
            pillar.collision = Collision::Box;
            pillar.tags = {"pillar"};
            w.spawn_primitive(pillar);
        }

        // Spawn on the plaza, facing the hero landmark.
        w.player_spawn(Vec3{0.0f, 0.0f, 2.0f}, Vec3{0.0f, 0.0f, -1.0f});
    }

    std::string id() const override { return "sample.starter_city"; }
};

PTV_REGISTER_WORLD_BUILDER("sample.starter_city", StarterCityWorld)

}  // namespace ptv::world
