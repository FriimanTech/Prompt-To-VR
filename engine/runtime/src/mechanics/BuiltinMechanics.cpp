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

namespace {
int sum_builtin_module_anchors() {
    return ptv_force_link_builtin_snap_turn + ptv_force_link_builtin_teleport +
           ptv_force_link_builtin_slide + ptv_force_link_builtin_grab +
           ptv_force_link_builtin_throw;
}
// Deliberately ``volatile``: a const, unused aggregate of the same values
// gets constant-folded away and the modules go missing from the link again.
volatile int g_builtin_module_anchor = sum_builtin_module_anchors();
}  // namespace

}  // namespace ptv
