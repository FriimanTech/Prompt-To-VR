// Hand-authored IWorldBuilder sample for the placement contract (no LLM).
// Compiled only with -DPTV_WORLDGEN_SAMPLES=ON; the orchestrator's
// test_worldgen_anchors builds and runs it.
//
// A small range: a table built from primitives with an on_surface anchor on
// its top, a GLB crate placed by its BASE (asset_size gives its real height,
// so a lamp goes on top of it), a lane with a rod and an anchor set of hang
// points under it, and tagged NPC spawn zones -- every call the
// world-architect agent is asked to make, so a host regression shows up
// without spending a run.

#include "ptv/api/IWorldBuilder.h"

namespace ptv::world {

class AnchorRangeWorld final : public IWorldBuilder {
public:
    void build(WorldContext& w) override {
        GroundDesc ground{};
        ground.size_m = 60.0f;
        w.ground(ground);
        w.player_spawn(Vec3{0.0f, 0.0f, 6.0f}, Vec3{0.0f, 0.0f, -1.0f});

        NavRegionDesc hall{};
        hall.id = "hall";
        hall.min = Vec3{-10.0f, 0.0f, -20.0f};
        hall.max = Vec3{10.0f, 0.0f, 10.0f};
        w.nav_region(hall);

        // A table: a 4 cm top at 0.78 m on four legs.
        PrimitiveDesc top{};
        top.name = "firearms_table_top";
        top.size = Vec3{1.6f, 0.04f, 0.8f};
        top.transform.position = Vec3{-3.0f, 0.76f, 2.0f};
        top.color = Color{0.35f, 0.3f, 0.25f};
        w.spawn_primitive(top);
        for (int i = 0; i < 4; ++i) {
            PrimitiveDesc leg{};
            leg.name = "firearms_table_leg";
            leg.size = Vec3{0.06f, 0.74f, 0.06f};
            leg.transform.position = Vec3{-3.0f + ((i & 1) ? 0.7f : -0.7f), 0.37f,
                                          2.0f + ((i & 2) ? 0.32f : -0.32f)};
            w.spawn_primitive(leg);
        }
        GameplayAnchorDesc pistol{};
        pistol.id = "glock_on_table";
        pistol.position = Vec3{-3.4f, 0.78f, 2.0f};
        pistol.relation = "on_surface";
        pistol.support = "firearms_table_top";
        w.gameplay_anchor(pistol);
        w.satisfies("req_tables", "a 1.6 m table at the staging area, top at 0.78 m");

        // A crate placed by its base, and a lamp on it at its measured top.
        ObjectPlacement crate{};
        crate.asset_id = "crate";
        crate.name = "supply_crate";
        crate.transform.position = Vec3{3.0f, 0.0f, 2.0f};
        w.place_object(crate);
        const AssetSize cs = w.asset_size("crate");
        GameplayAnchorDesc lamp{};
        lamp.id = "lamp_on_crate";
        lamp.position = Vec3{3.0f, cs.known ? cs.size.y : 0.5f, 2.0f};
        lamp.relation = "on_surface";
        lamp.support = "supply_crate";
        w.gameplay_anchor(lamp);

        // Lane 2: a rod 5 m up, hang points 2 cm under it.
        PrimitiveDesc rod{};
        rod.name = "lane2_drop_rod";
        rod.size = Vec3{8.0f, 0.06f, 0.06f};
        rod.transform.position = Vec3{0.0f, 5.0f, -15.0f};
        rod.color = Color{0.8f, 0.1f, 0.1f};
        w.spawn_primitive(rod);
        AnchorSetDesc hang{};
        hang.id = "lane2_hang_points";
        hang.relation = "hang_from";
        hang.facing = Vec3{0.0f, 0.0f, 1.0f};
        for (int i = 0; i < 4; ++i) hang.points.push_back(Vec3{-3.0f + 2.0f * i, 4.97f, -15.0f});
        w.anchor_set(hang);

        NpcSpawnZoneDesc front{};
        front.id = "lane3_front";
        front.position = Vec3{6.0f, 0.0f, -8.0f};
        front.tags = {"front_band"};
        w.npc_spawn_zone(front);
        NpcSpawnZoneDesc back{};
        back.id = "lane3_back";
        back.position = Vec3{6.0f, 0.0f, -18.0f};
        back.tags = {"back_band"};
        w.npc_spawn_zone(back);
    }

    std::string id() const override { return "sample.anchor_range"; }
};

PTV_REGISTER_WORLD_BUILDER("sample.anchor_range", AnchorRangeWorld)

}  // namespace ptv::world
