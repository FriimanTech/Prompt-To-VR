#include "VisualTagTable.h"

#include <algorithm>

namespace ptv::render_tags {
namespace {

struct Row {
    const char* tag;
    Role roles;
    Vec4 mesh_tint;
    float placeholder_xz;
    float placeholder_y;
    Vec4 placeholder_color;
    float placeholder_material;
};

// Order IS priority: the first row that sets a field wins. ``ground`` sits
// above the affordances so a floor that is also tagged grabbable still draws
// as a floor rather than a small orange cube.
const std::vector<Row>& rows() {
    static const std::vector<Row> table = {
        // --- visibility policy ------------------------------------------
        {"template", Role::kAlwaysHidden, {}, 0, 0, {}, 0},
        {"player_system", Role::kHiddenWhenMeshless, {}, 0, 0, {}, 0},
        {"tutorial", Role::kHiddenWhenMeshless, {}, 0, 0, {}, 0},
        {"checkpoint", Role::kHiddenWhenMeshless, {}, 0, 0, {}, 0},
        {"spawn_point", Role::kHiddenWhenMeshless, {}, 0, 0, {}, 0},

        // --- the floor ---------------------------------------------------
        {"ground",
         Role::kGround | Role::kPlaceholder,
         {},
         30.0f,
         0.05f,
         {0.22f, 0.24f, 0.27f, 1.0f},
         1.0f},
        {"teleport_target",
         Role::kGround | Role::kPlaceholder,
         {},
         30.0f,
         0.05f,
         {0.22f, 0.24f, 0.27f, 1.0f},
         1.0f},

        // --- resting cues on real meshes ---------------------------------
        // Subtle on purpose. A resting tint has to survive being looked at
        // for an hour; the readable "you can act on this" signal is the hover
        // highlight the affordance built-ins publish, not this.
        //
        // Environment sits above the gameplay cues: a wall that is also
        // tagged grabbable should still read as a wall.
        {"environment",
         Role::kMeshTint,
         {0.96f, 0.98f, 1.05f, 1.0f},
         0,
         0,
         {},
         0},
        {"hover_allowed",
         Role::kMeshTint,
         {0.96f, 0.98f, 1.05f, 1.0f},
         0,
         0,
         {},
         0},
        {"enemy",
         Role::kMeshTint,
         {1.05f, 0.95f, 0.92f, 1.0f},
         0,
         0,
         {},
         0},
        {"grabbable",
         Role::kMeshTint | Role::kPlaceholder,
         {1.05f, 1.02f, 0.98f, 1.0f},
         0.18f,
         0.18f,
         {0.95f, 0.55f, 0.22f, 1.0f},
         0.0f},

        // --- declared affordances ----------------------------------------
        // One row each, so a world architect declaring a door gets a door
        // rather than falling through to the generic grey cube. These are the
        // rows that used to be missing.
        {"door",
         Role::kPlaceholder,
         {},
         0.9f,
         2.0f,
         {0.55f, 0.40f, 0.28f, 1.0f},
         0.0f},
        {"container",
         Role::kPlaceholder,
         {},
         0.5f,
         0.4f,
         {0.48f, 0.38f, 0.30f, 1.0f},
         0.0f},
        {"switchable",
         Role::kPlaceholder,
         {},
         0.12f,
         0.12f,
         {0.85f, 0.82f, 0.35f, 1.0f},
         0.0f},
        {"mover",
         Role::kPlaceholder,
         {},
         0.8f,
         0.3f,
         {0.42f, 0.46f, 0.52f, 1.0f},
         0.0f},
        {"breakable",
         Role::kPlaceholder,
         {},
         0.5f,
         0.6f,
         {0.62f, 0.78f, 0.86f, 1.0f},
         0.0f},
        {"harvestable",
         Role::kPlaceholder,
         {},
         0.6f,
         0.25f,
         {0.35f, 0.62f, 0.32f, 1.0f},
         0.0f},

        // --- gameplay volumes --------------------------------------------
        {"trigger",
         Role::kPlaceholder,
         {},
         0.20f,
         0.20f,
         {0.30f, 0.75f, 0.98f, 1.0f},
         0.0f},
    };
    return table;
}

}  // namespace

TagVisuals resolve(const std::vector<std::string>& tags) {
    TagVisuals out{};
    if (tags.empty()) return out;

    for (const Row& row : rows()) {
        const bool present =
            std::find(tags.begin(), tags.end(), row.tag) != tags.end();
        if (!present) continue;

        if (has_role(row.roles, Role::kAlwaysHidden)) {
            // Nothing else can matter once the entity never draws.
            out.always_hidden = true;
            return out;
        }
        if (has_role(row.roles, Role::kHiddenWhenMeshless)) {
            out.hidden_when_meshless = true;
        }
        if (has_role(row.roles, Role::kGround)) {
            out.is_ground = true;
        }
        if (has_role(row.roles, Role::kMeshTint) && !out.has_mesh_tint) {
            out.has_mesh_tint = true;
            out.mesh_tint = row.mesh_tint;
        }
        if (has_role(row.roles, Role::kPlaceholder) && !out.has_placeholder) {
            out.has_placeholder = true;
            out.placeholder_xz_scale = row.placeholder_xz;
            out.placeholder_y_scale = row.placeholder_y;
            out.placeholder_color = row.placeholder_color;
            out.placeholder_material_id = row.placeholder_material;
        }
    }
    return out;
}

const std::vector<std::string>& known_tags() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> out;
        out.reserve(rows().size());
        for (const Row& row : rows()) out.emplace_back(row.tag);
        return out;
    }();
    return names;
}

}  // namespace ptv::render_tags
