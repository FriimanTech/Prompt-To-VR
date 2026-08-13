#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ptv/math/Math.h"

namespace ptv::nav {

/** A convex patch of walkable floor, as an axis-aligned footprint.
 *
 * Rectangles rather than arbitrary polygons because of where this data comes
 * from: the world-architect agent DECLARES its walkable space while building
 * the level, and rooms, corridors, courtyards and streets are rectangles.
 * Anything a rectangle cannot express is expressed as several of them. */
struct NavRegion {
    std::string id;
    /** Footprint on the XZ plane, in metres. */
    Vec3 min{0.0f};
    Vec3 max{0.0f};
    /** Floor height this region sits at. */
    float floor_y{0.0f};

    Vec3 center() const {
        return Vec3(0.5f * (min.x + max.x), floor_y, 0.5f * (min.z + max.z));
    }
    bool contains_xz(const Vec3& p) const {
        return p.x >= min.x && p.x <= max.x && p.z >= min.z && p.z <= max.z;
    }
    /** Nearest point inside the footprint, at floor height. */
    Vec3 clamp_xz(const Vec3& p) const;
};

/** A traversable connection between two regions -- a doorway, a gap in a
 *  wall, the top of a ramp. */
struct NavLink {
    int from{-1};
    int to{-1};
    /** Where to cross. Path smoothing routes through this point, which is
     *  what makes an NPC aim at the doorway rather than at the wall beside
     *  it. */
    Vec3 point{0.0f};
    /** Extra cost, so a ladder or a long detour can be made less attractive
     *  than walking round. 1.0 is neutral. */
    float cost_multiplier{1.0f};
    bool bidirectional{true};
};

/**
 * Walkable topology of a level: which patches of floor exist and how they
 * connect.
 *
 * The point of having this at all is that local steering cannot get an NPC
 * through a door. Steering is reactive -- it knows only what its whiskers hit
 * this frame -- so it walks into the wall beside a doorway and jitters there
 * forever. Routing needs a model of the whole space.
 *
 * The usual way to build that model is to voxelise finished geometry
 * (Recast). That is not the cheapest option here, because this engine's
 * levels are not scanned: they are authored in C++ by an agent that already
 * knows it is placing a room here and a doorway there. Reconstructing
 * topology that somebody just wrote down is wasted work and a source of
 * error, so the world builder declares it instead and this graph is a direct
 * reading of that declaration -- validated offline before it ships.
 */
class NavGraph {
public:
    void clear();

    int add_region(const NavRegion& region);
    void add_link(const NavLink& link);

    bool empty() const { return regions_.empty(); }
    std::size_t region_count() const { return regions_.size(); }
    std::size_t link_count() const { return links_.size(); }
    const NavRegion& region(int index) const { return regions_[static_cast<std::size_t>(index)]; }

    /** Region containing this point, or the nearest one within
     *  ``snap_radius_m``. -1 when the point is nowhere near walkable floor. */
    int region_at(const Vec3& p, float snap_radius_m = 2.0f) const;

    /** Route between two points as a list of world-space corners, starting
     *  at ``from`` and ending at ``to``.
     *
     *  Returns false when the two are in disconnected parts of the level.
     *  Within a single region the answer is the straight line, which is both
     *  correct and the common case. */
    bool find_path(const Vec3& from, const Vec3& to,
                   std::vector<Vec3>& out_corners) const;

    /** Walking distance along that route. */
    bool path_distance(const Vec3& from, const Vec3& to,
                       float& out_meters) const;

    bool is_reachable(const Vec3& from, const Vec3& to) const;

private:
    /** A* over regions. Returns region indices from start to goal. */
    bool search(int start, int goal, std::vector<int>& out_regions) const;

    std::vector<NavRegion> regions_;
    std::vector<NavLink> links_;
    /** Per-region outgoing link indices, so the search does not scan every
     *  link for every node it expands. */
    std::vector<std::vector<int>> adjacency_;
};

}  // namespace ptv::nav
