// Link anchor for the built-in mechanic modules.
//
// Every built-in now lives in its own module directory under
// ``builtin/<mechanic>/``. Nothing but registration glue is left here.
//
// Why this file still exists: MSVC's linker drops an entire .obj from a
// static library when nothing references it, and our built-ins announce
// themselves purely through static constructors (PTV_REGISTER_MECHANIC). With
// no reference, the registrations silently never run and every scene logs
// "no mechanic registered for builtin.grab".
//
// So there is a chain of references: MechanicRegistry.cpp names
// ``ptv_force_link_builtin_mechanics`` (below), and this file in turn names
// one anchor per module. Adding a new built-in module means adding its anchor
// to both lists here.

#include "ptv/api/IMechanic.h"

namespace ptv {

extern "C" int ptv_force_link_builtin_mechanics = 1;

extern "C" int ptv_force_link_builtin_snap_turn;
extern "C" int ptv_force_link_builtin_teleport;
extern "C" int ptv_force_link_builtin_slide;
extern "C" int ptv_force_link_builtin_grab;
extern "C" int ptv_force_link_builtin_throw;
extern "C" int ptv_force_link_builtin_recoil;
extern "C" int ptv_force_link_builtin_weapon;
extern "C" int ptv_force_link_builtin_melee;
extern "C" int ptv_force_link_builtin_bow;
extern "C" int ptv_force_link_builtin_item_source;
extern "C" int ptv_force_link_builtin_npc_ranged;
extern "C" int ptv_force_link_builtin_hand_pose;
extern "C" int ptv_force_link_builtin_character_locomotion;
extern "C" int ptv_force_link_builtin_avatar_body;
extern "C" int ptv_force_link_builtin_mirror_surface;
extern "C" int ptv_force_link_builtin_equip;
extern "C" int ptv_force_link_builtin_ui_panel;
extern "C" int ptv_force_link_builtin_head_interior;
extern "C" int ptv_force_link_builtin_motion_affordances;
extern "C" int ptv_force_link_builtin_state_affordances;
extern "C" int ptv_force_link_builtin_smooth_turn;
extern "C" int ptv_force_link_builtin_movement_settings_menu;
extern "C" int ptv_force_link_builtin_encounter_director;
extern "C" int ptv_force_link_builtin_npc_follow;
extern "C" int ptv_force_link_builtin_npc_melee_chase;
extern "C" int ptv_force_link_builtin_game_flow;
extern "C" int ptv_force_link_builtin_game_hud;

namespace {
int sum_builtin_module_anchors() {
    return ptv_force_link_builtin_snap_turn + ptv_force_link_builtin_teleport +
           ptv_force_link_builtin_slide + ptv_force_link_builtin_grab +
           ptv_force_link_builtin_throw + ptv_force_link_builtin_recoil +
           ptv_force_link_builtin_weapon + ptv_force_link_builtin_melee +
           ptv_force_link_builtin_bow + ptv_force_link_builtin_item_source +
           ptv_force_link_builtin_npc_ranged +
           ptv_force_link_builtin_hand_pose +
           ptv_force_link_builtin_character_locomotion +
           ptv_force_link_builtin_avatar_body +
           ptv_force_link_builtin_mirror_surface +
           ptv_force_link_builtin_equip +
           ptv_force_link_builtin_ui_panel +
           ptv_force_link_builtin_head_interior +
           ptv_force_link_builtin_motion_affordances +
           ptv_force_link_builtin_state_affordances +
           ptv_force_link_builtin_smooth_turn +
           ptv_force_link_builtin_movement_settings_menu +
           ptv_force_link_builtin_encounter_director +
           ptv_force_link_builtin_npc_follow +
           ptv_force_link_builtin_npc_melee_chase +
           ptv_force_link_builtin_game_flow + ptv_force_link_builtin_game_hud;
}
// Deliberately ``volatile``: a const, unused aggregate of the same values
// gets constant-folded away and the modules go missing from the link again.
volatile int g_builtin_module_anchor = sum_builtin_module_anchors();
}  // namespace

}  // namespace ptv
