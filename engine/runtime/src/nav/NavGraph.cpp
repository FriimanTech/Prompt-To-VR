#include "ptv/nav/NavGraph.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace ptv::nav {
namespace {

float distance_xz(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

}  // namespace

Vec3 NavRegion::clamp_xz(const Vec3& p) const {
    return Vec3(std::clamp(p.x, min.x, max.x), floor_y,
                std::clamp(p.z, min.z, max.z));
}

void NavGraph::clear() {
    regions_.clear();
    links_.clear();
    adjacency_.clear();
}

int NavGraph::add_region(const NavRegion& region) {
    NavRegion r = region;
    // Tolerate a footprint given corner-first in either order rather than
    // silently producing a region that contains nothing.
    if (r.min.x > r.max.x) std::swap(r.min.x, r.max.x);
    if (r.min.z > r.max.z) std::swap(r.min.z, r.max.z);
    regions_.push_back(r);
    adjacency_.emplace_back();
    return static_cast<int>(regions_.size()) - 1;
}

void NavGraph::add_link(const NavLink& link) {
    const int n = static_cast<int>(regions_.size());
    if (link.from < 0 || link.from >= n) return;
    if (link.to < 0 || link.to >= n) return;
    if (link.from == link.to) return;

    links_.push_back(link);
    const int index = static_cast<int>(links_.size()) - 1;
    adjacency_[static_cast<std::size_t>(link.from)].push_back(index);
    if (link.bidirectional) {
        adjacency_[static_cast<std::size_t>(link.to)].push_back(index);
    }
}

int NavGraph::region_at(const Vec3& p, float snap_radius_m) const {
    int best = -1;
    float best_d = 0.0f;
    for (std::size_t i = 0; i < regions_.size(); ++i) {
        const auto& r = regions_[i];
        if (r.contains_xz(p)) {
            // Several regions can overlap at a doorway; prefer the one whose
            // floor is closest, so a point on a bridge does not resolve to
            // the ground underneath it.
            const float d = std::fabs(p.y - r.floor_y);
            if (best < 0 || d < best_d) {
                best = static_cast<int>(i);
                best_d = d;
            }
        }
    }
    if (best >= 0) return best;

    // Nothing contains it: fall back to the closest region within reach, so
    // an NPC standing a few centimetres outside a room still routes.
    const float snap = std::max(0.0f, snap_radius_m);
    for (std::size_t i = 0; i < regions_.size(); ++i) {
        const float d = distance_xz(p, regions_[i].clamp_xz(p));
        if (d > snap) continue;
        if (best < 0 || d < best_d) {
            best = static_cast<int>(i);
            best_d = d;
        }
    }
    return best;
}

bool NavGraph::search(int start, int goal, std::vector<int>& out_regions) const {
    out_regions.clear();
    if (start < 0 || goal < 0) return false;
    if (start == goal) {
        out_regions.push_back(start);
        return true;
    }

    const std::size_t n = regions_.size();
    const float inf = std::numeric_limits<float>::infinity();
    std::vector<float> g(n, inf);
    std::vector<int> came_from(n, -1);
    std::vector<std::uint8_t> closed(n, 0u);

    struct Node {
        float f;
        int region;
        bool operator>(const Node& o) const { return f > o.f; }
    };
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;

    const Vec3 goal_c = regions_[static_cast<std::size_t>(goal)].center();
    auto heuristic = [&](int r) {
        return distance_xz(regions_[static_cast<std::size_t>(r)].center(),
                           goal_c);
    };

    g[static_cast<std::size_t>(start)] = 0.0f;
    open.push({heuristic(start), start});

    while (!open.empty()) {
        const int current = open.top().region;
        open.pop();
        const auto ci = static_cast<std::size_t>(current);
        if (closed[ci]) continue;
        closed[ci] = 1u;

        if (current == goal) {
            for (int at = goal; at != -1;
                 at = came_from[static_cast<std::size_t>(at)]) {
                out_regions.push_back(at);
            }
            std::reverse(out_regions.begin(), out_regions.end());
            return true;
        }

        for (int li : adjacency_[ci]) {
            const auto& link = links_[static_cast<std::size_t>(li)];
            const int next = (link.from == current) ? link.to : link.from;
            if (next == current) continue;
            const auto ni = static_cast<std::size_t>(next);
            if (closed[ni]) continue;

            // Cost is measured through the crossing point, not centre to
            // centre: two rooms joined by a door at one far end are much
            // further apart than their centres suggest.
            const float step =
                (distance_xz(regions_[ci].center(), link.point) +
                 distance_xz(link.point, regions_[ni].center())) *
                std::max(0.01f, link.cost_multiplier);
            const float tentative = g[ci] + step;
            if (tentative >= g[ni]) continue;
            g[ni] = tentative;
            came_from[ni] = current;
            open.push({tentative + heuristic(next), next});
        }
    }
    return false;
}

bool NavGraph::find_path(const Vec3& from, const Vec3& to,
                         std::vector<Vec3>& out_corners) const {
    out_corners.clear();
    if (regions_.empty()) return false;

    const int start = region_at(from);
    const int goal = region_at(to);
    if (start < 0 || goal < 0) return false;

    std::vector<int> route;
    if (!search(start, goal, route)) return false;

    out_corners.push_back(from);
    // One corner per crossing. Inside a single region the straight line is
    // already correct, so a same-region path is just from -> to.
    for (std::size_t i = 0; i + 1 < route.size(); ++i) {
        const int a = route[i];
        const int b = route[i + 1];
        for (int li : adjacency_[static_cast<std::size_t>(a)]) {
            const auto& link = links_[static_cast<std::size_t>(li)];
            const bool joins = (link.from == a && link.to == b) ||
                               (link.bidirectional && link.to == a &&
                                link.from == b);
            if (!joins) continue;
            out_corners.push_back(link.point);
            break;
        }
    }
    out_corners.push_back(to);
    return true;
}

bool NavGraph::path_distance(const Vec3& from, const Vec3& to,
                             float& out_meters) const {
    std::vector<Vec3> corners;
    if (!find_path(from, to, corners)) return false;
    float total = 0.0f;
    for (std::size_t i = 0; i + 1 < corners.size(); ++i) {
        total += distance_xz(corners[i], corners[i + 1]);
    }
    out_meters = total;
    return true;
}

bool NavGraph::is_reachable(const Vec3& from, const Vec3& to) const {
    // No declared topology means no opinion, and refusing every destination
    // would freeze every NPC in a level whose builder did not declare any.
    if (regions_.empty()) return true;
    const int start = region_at(from);
    const int goal = region_at(to);
    if (start < 0 || goal < 0) return false;
    std::vector<int> route;
    return search(start, goal, route);
}

}  // namespace ptv::nav
