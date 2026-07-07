// Always-available deterministic fallback world. Used by stage_world_generation
// when the agent's builder cannot be produced/compiled/validated, so a project
// always boots into *something* walkable.

#include "ptv/api/IWorldBuilder.h"

#include "WorldGen.h"

namespace ptv::world {

class FallbackWorld final : public IWorldBuilder {
public:
    void build(WorldContext& w) override {
        SkyDesc sky{};
        sky.top = Color{0.05f, 0.07f, 0.16f};
        sky.horizon = Color{0.42f, 0.5f, 0.68f};
        w.sky(sky);

        GroundDesc ground{};
        ground.size_m = 60.0f;
        ground.color = Color{0.18f, 0.19f, 0.22f};
        ground.grid = true;
        w.ground(ground);

        LightDesc sun{};
        sun.kind = "directional";
        sun.direction = Vec3{0.3f, -0.8f, 0.3f};
        sun.color = Color{1.0f, 0.96f, 0.9f};
        sun.intensity = 1.3f;
        w.light(sun);

        // A few reference blocks so the space is not empty.
        helpers::WallShellDesc shell{};
        shell.width = 24.0f;
        shell.depth = 24.0f;
        shell.height = 4.0f;
        helpers::wall_shell(w, shell);

        helpers::PlatformSetDesc steps{};
        steps.origin = Vec3{4.0f, 0.0f, 4.0f};
        steps.count = 3;
        helpers::platform_set(w, steps);

        w.player_spawn(Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f});
    }

    std::string id() const override { return kFallbackBuilderId; }
};

PTV_REGISTER_WORLD_BUILDER(kFallbackBuilderId, FallbackWorld)

}  // namespace ptv::world
