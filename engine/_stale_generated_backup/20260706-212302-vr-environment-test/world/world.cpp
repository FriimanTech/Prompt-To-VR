#include "ptv/api/IWorldBuilder.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace ptv::world {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTau = 6.28318530717958647692f;

struct SimpleRng {
  explicit SimpleRng(std::uint64_t seed) {
    state = static_cast<std::uint32_t>(seed ^ (seed >> 32U) ^ 0xA53C9E71u);
    if (state == 0U) {
      state = 0x6D2B79F5u;
    }
  }

  std::uint32_t next_u32() {
    std::uint32_t x = state;
    x ^= x << 13U;
    x ^= x >> 17U;
    x ^= x << 5U;
    state = x;
    return x;
  }

  float unit() {
    return static_cast<float>(next_u32() & 0x00FFFFFFu) / 16777216.0f;
  }

  std::uint32_t state;
};

Quat quat_identity() {
  return Quat{0.0f, 0.0f, 0.0f, 1.0f};
}

Quat quat_yaw(float radians) {
  const float h = radians * 0.5f;
  return Quat{0.0f, std::sin(h), 0.0f, std::cos(h)};
}

Quat quat_pitch(float radians) {
  const float h = radians * 0.5f;
  return Quat{std::sin(h), 0.0f, 0.0f, std::cos(h)};
}

Vec3 v3(float x, float y, float z) {
  return Vec3{x, y, z};
}

Color color(float r, float g, float b) {
  return Color{r, g, b};
}

Transform tr(Vec3 p, Vec3 s, Quat q = quat_identity()) {
  return Transform{p, q, s};
}

float rand_range(SimpleRng& rng, float lo, float hi) {
  return lo + (hi - lo) * rng.unit();
}

void push_pos(std::vector<float>& positions, float x, float y, float z) {
  positions.push_back(x);
  positions.push_back(y);
  positions.push_back(z);
}

void push_pos(std::vector<float>& positions, Vec3 p) {
  push_pos(positions, p.x, p.y, p.z);
}

std::uint32_t vertex_count(const std::vector<float>& positions) {
  return static_cast<std::uint32_t>(positions.size() / 3U);
}

void box(WorldContext& w, Vec3 pos, Vec3 size, Color c, Collision collision,
         std::vector<std::string> tags = {}) {
  w.spawn_primitive(PrimitiveDesc{
      .kind = PrimitiveKind::Box,
      .transform = tr(pos, size),
      .size = v3(1.0f, 1.0f, 1.0f),
      .color = c,
      .collision = collision,
      .tags = tags,
  });
}

void box_rot(WorldContext& w, Vec3 pos, Vec3 size, float yaw, Color c,
             Collision collision, std::vector<std::string> tags = {}) {
  w.spawn_primitive(PrimitiveDesc{
      .kind = PrimitiveKind::Box,
      .transform = tr(pos, size, quat_yaw(yaw)),
      .size = v3(1.0f, 1.0f, 1.0f),
      .color = c,
      .collision = collision,
      .tags = tags,
  });
}

void cyl(WorldContext& w, Vec3 pos, Vec3 scale, Color c, Collision collision,
         std::vector<std::string> tags = {}) {
  w.spawn_primitive(PrimitiveDesc{
      .kind = PrimitiveKind::Cylinder,
      .transform = tr(pos, scale),
      .size = v3(1.0f, 1.0f, 1.0f),
      .color = c,
      .collision = collision,
      .tags = tags,
  });
}

void cyl_rot(WorldContext& w, Vec3 pos, Vec3 scale, Quat q, Color c,
             Collision collision, std::vector<std::string> tags = {}) {
  w.spawn_primitive(PrimitiveDesc{
      .kind = PrimitiveKind::Cylinder,
      .transform = tr(pos, scale, q),
      .size = v3(1.0f, 1.0f, 1.0f),
      .color = c,
      .collision = collision,
      .tags = tags,
  });
}

void sphere(WorldContext& w, Vec3 pos, Vec3 scale, Color c, Collision collision,
            std::vector<std::string> tags = {}) {
  w.spawn_primitive(PrimitiveDesc{
      .kind = PrimitiveKind::Sphere,
      .transform = tr(pos, scale),
      .size = v3(1.0f, 1.0f, 1.0f),
      .color = c,
      .collision = collision,
      .tags = tags,
  });
}

void ramp(WorldContext& w, Vec3 pos, Vec3 scale, float yaw, Color c,
          Collision collision, std::vector<std::string> tags = {}) {
  w.spawn_primitive(PrimitiveDesc{
      .kind = PrimitiveKind::Ramp,
      .transform = tr(pos, scale, quat_yaw(yaw)),
      .size = v3(1.0f, 1.0f, 1.0f),
      .color = c,
      .collision = collision,
      .tags = tags,
  });
}

void point_light(WorldContext& w, Vec3 pos, Color c, float intensity, float range) {
  w.light(LightDesc{
      .kind = "point",
      .position = pos,
      .direction = v3(0.0f, -1.0f, 0.0f),
      .color = c,
      .intensity = intensity,
      .range = range,
  });
}

void directional_light(WorldContext& w, Vec3 dir, Color c, float intensity) {
  w.light(LightDesc{
      .kind = "directional",
      .position = v3(0.0f, 0.0f, 0.0f),
      .direction = dir,
      .color = c,
      .intensity = intensity,
      .range = 0.0f,
  });
}

void object(WorldContext& w, const std::string& asset_id, Vec3 pos, Vec3 scale,
            float yaw, std::vector<std::string> tags = {}) {
  w.place_object(ObjectPlacement{
      .asset_id = asset_id,
      .transform = tr(pos, scale, quat_yaw(yaw)),
      .tags = tags,
  });
}

void add_cylinder_mesh(WorldContext& w, Vec3 pos, float radius, float height,
                       int segments, Color c, Collision collision,
                       std::vector<std::string> tags = {}) {
  std::vector<float> positions;
  std::vector<std::uint32_t> indices;
  positions.reserve(static_cast<size_t>(segments) * 6U + 6U);

  const float half_h = height * 0.5f;
  for (int i = 0; i < segments; ++i) {
    const float a = kTau * static_cast<float>(i) / static_cast<float>(segments);
    const float x = std::cos(a) * radius;
    const float z = std::sin(a) * radius;
    push_pos(positions, x, -half_h, z);
    push_pos(positions, x, half_h, z);
  }
  const std::uint32_t bottom_center = vertex_count(positions);
  push_pos(positions, 0.0f, -half_h, 0.0f);
  const std::uint32_t top_center = vertex_count(positions);
  push_pos(positions, 0.0f, half_h, 0.0f);

  for (int i = 0; i < segments; ++i) {
    const int n = (i + 1) % segments;
    const std::uint32_t b0 = static_cast<std::uint32_t>(i * 2);
    const std::uint32_t t0 = b0 + 1;
    const std::uint32_t b1 = static_cast<std::uint32_t>(n * 2);
    const std::uint32_t t1 = b1 + 1;

    indices.push_back(b0);
    indices.push_back(b1);
    indices.push_back(t1);
    indices.push_back(b0);
    indices.push_back(t1);
    indices.push_back(t0);

    indices.push_back(bottom_center);
    indices.push_back(b0);
    indices.push_back(b1);

    indices.push_back(top_center);
    indices.push_back(t1);
    indices.push_back(t0);
  }

  w.add_mesh(MeshDesc{
      .positions = positions,
      .normals = {},
      .indices = indices,
      .transform = tr(pos, v3(1.0f, 1.0f, 1.0f)),
      .color = c,
      .collision = collision,
      .tags = tags,
  });
}

void add_cone_mesh(WorldContext& w, Vec3 pos, float radius, float height,
                   int segments, Color c, Collision collision,
                   std::vector<std::string> tags = {}) {
  std::vector<float> positions;
  std::vector<std::uint32_t> indices;
  positions.reserve(static_cast<size_t>(segments) * 3U + 6U);

  const float half_h = height * 0.5f;
  for (int i = 0; i < segments; ++i) {
    const float a = kTau * static_cast<float>(i) / static_cast<float>(segments);
    push_pos(positions, std::cos(a) * radius, -half_h, std::sin(a) * radius);
  }
  const std::uint32_t apex = vertex_count(positions);
  push_pos(positions, 0.0f, half_h, 0.0f);
  const std::uint32_t center = vertex_count(positions);
  push_pos(positions, 0.0f, -half_h, 0.0f);

  for (int i = 0; i < segments; ++i) {
    const std::uint32_t a = static_cast<std::uint32_t>(i);
    const std::uint32_t b = static_cast<std::uint32_t>((i + 1) % segments);
    indices.push_back(a);
    indices.push_back(b);
    indices.push_back(apex);
    indices.push_back(center);
    indices.push_back(a);
    indices.push_back(b);
  }

  w.add_mesh(MeshDesc{
      .positions = positions,
      .normals = {},
      .indices = indices,
      .transform = tr(pos, v3(1.0f, 1.0f, 1.0f)),
      .color = c,
      .collision = collision,
      .tags = tags,
  });
}

void add_torus_mesh(WorldContext& w, Vec3 pos, float major_radius,
                    float minor_radius, int major_segments, int minor_segments,
                    Color c, Collision collision, float yaw,
                    std::vector<std::string> tags = {}) {
  std::vector<float> positions;
  std::vector<std::uint32_t> indices;
  positions.reserve(static_cast<size_t>(major_segments * minor_segments) * 3U);

  for (int i = 0; i < major_segments; ++i) {
    const float a = kTau * static_cast<float>(i) / static_cast<float>(major_segments);
    const float ca = std::cos(a);
    const float sa = std::sin(a);
    for (int j = 0; j < minor_segments; ++j) {
      const float b = kTau * static_cast<float>(j) / static_cast<float>(minor_segments);
      const float cb = std::cos(b);
      const float sb = std::sin(b);
      const float r = major_radius + minor_radius * cb;
      push_pos(positions, r * ca, minor_radius * sb, r * sa);
    }
  }

  for (int i = 0; i < major_segments; ++i) {
    const int in = (i + 1) % major_segments;
    for (int j = 0; j < minor_segments; ++j) {
      const int jn = (j + 1) % minor_segments;
      const std::uint32_t a = static_cast<std::uint32_t>(i * minor_segments + j);
      const std::uint32_t b = static_cast<std::uint32_t>(in * minor_segments + j);
      const std::uint32_t c0 = static_cast<std::uint32_t>(in * minor_segments + jn);
      const std::uint32_t d = static_cast<std::uint32_t>(i * minor_segments + jn);
      indices.push_back(a);
      indices.push_back(b);
      indices.push_back(c0);
      indices.push_back(a);
      indices.push_back(c0);
      indices.push_back(d);
    }
  }

  w.add_mesh(MeshDesc{
      .positions = positions,
      .normals = {},
      .indices = indices,
      .transform = tr(pos, v3(1.0f, 1.0f, 1.0f), quat_yaw(yaw)),
      .color = c,
      .collision = collision,
      .tags = tags,
  });
}

void add_beveled_prism_mesh(WorldContext& w, Vec3 pos, float radius, float height,
                            int sides, float bevel, Color c, Collision collision,
                            float yaw, std::vector<std::string> tags = {}) {
  std::vector<float> positions;
  std::vector<std::uint32_t> indices;
  const float half_h = height * 0.5f;
  const float upper_radius = radius * (1.0f - bevel);
  const float lower_radius = radius;
  positions.reserve(static_cast<size_t>(sides) * 12U + 6U);

  for (int i = 0; i < sides; ++i) {
    const float a = kTau * static_cast<float>(i) / static_cast<float>(sides);
    const float ca = std::cos(a);
    const float sa = std::sin(a);
    push_pos(positions, ca * lower_radius, -half_h, sa * lower_radius);
    push_pos(positions, ca * lower_radius, -half_h + bevel * height, sa * lower_radius);
    push_pos(positions, ca * upper_radius, half_h - bevel * height, sa * upper_radius);
    push_pos(positions, ca * upper_radius, half_h, sa * upper_radius);
  }

  auto vid = [sides](int i, int level) -> std::uint32_t {
    const int wrapped = (i + sides) % sides;
    return static_cast<std::uint32_t>(wrapped * 4 + level);
  };

  for (int i = 0; i < sides; ++i) {
    const int n = (i + 1) % sides;
    for (int level = 0; level < 3; ++level) {
      indices.push_back(vid(i, level));
      indices.push_back(vid(n, level));
      indices.push_back(vid(n, level + 1));
      indices.push_back(vid(i, level));
      indices.push_back(vid(n, level + 1));
      indices.push_back(vid(i, level + 1));
    }
  }

  const std::uint32_t bottom_center = vertex_count(positions);
  push_pos(positions, 0.0f, -half_h, 0.0f);
  const std::uint32_t top_center = vertex_count(positions);
  push_pos(positions, 0.0f, half_h, 0.0f);

  for (int i = 0; i < sides; ++i) {
    const int n = (i + 1) % sides;
    indices.push_back(bottom_center);
    indices.push_back(vid(n, 0));
    indices.push_back(vid(i, 0));

    indices.push_back(top_center);
    indices.push_back(vid(i, 3));
    indices.push_back(vid(n, 3));
  }

  w.add_mesh(MeshDesc{
      .positions = positions,
      .normals = {},
      .indices = indices,
      .transform = tr(pos, v3(1.0f, 1.0f, 1.0f), quat_yaw(yaw)),
      .color = c,
      .collision = collision,
      .tags = tags,
  });
}

void add_arch_mesh(WorldContext& w, Vec3 pos, float inner_radius, float thickness,
                   float depth, float pillar_height, int segments, Color c,
                   Collision collision, float yaw,
                   std::vector<std::string> tags = {}) {
  std::vector<float> positions;
  std::vector<std::uint32_t> indices;

  const float outer_radius = inner_radius + thickness;
  const float z0 = -depth * 0.5f;
  const float z1 = depth * 0.5f;

  auto add_vertex = [&](float x, float y, float z) -> std::uint32_t {
    const std::uint32_t idx = vertex_count(positions);
    push_pos(positions, x, y, z);
    return idx;
  };

  auto add_quad = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c0, std::uint32_t d) {
    indices.push_back(a);
    indices.push_back(b);
    indices.push_back(c0);
    indices.push_back(a);
    indices.push_back(c0);
    indices.push_back(d);
  };

  for (int i = 0; i < segments; ++i) {
    const float a0 = kPi - kPi * static_cast<float>(i) / static_cast<float>(segments);
    const float a1 = kPi - kPi * static_cast<float>(i + 1) / static_cast<float>(segments);
    const float x00 = std::cos(a0) * inner_radius;
    const float y00 = pillar_height + std::sin(a0) * inner_radius;
    const float x01 = std::cos(a1) * inner_radius;
    const float y01 = pillar_height + std::sin(a1) * inner_radius;
    const float x10 = std::cos(a0) * outer_radius;
    const float y10 = pillar_height + std::sin(a0) * outer_radius;
    const float x11 = std::cos(a1) * outer_radius;
    const float y11 = pillar_height + std::sin(a1) * outer_radius;

    const std::uint32_t a = add_vertex(x00, y00, z0);
    const std::uint32_t b = add_vertex(x01, y01, z0);
    const std::uint32_t c1 = add_vertex(x11, y11, z0);
    const std::uint32_t d = add_vertex(x10, y10, z0);
    const std::uint32_t e = add_vertex(x00, y00, z1);
    const std::uint32_t f = add_vertex(x01, y01, z1);
    const std::uint32_t g = add_vertex(x11, y11, z1);
    const std::uint32_t h = add_vertex(x10, y10, z1);

    add_quad(a, b, c1, d);
    add_quad(e, h, g, f);
    add_quad(a, e, f, b);
    add_quad(d, c1, g, h);
    add_quad(b, f, g, c1);
    add_quad(a, d, h, e);
  }

  w.add_mesh(MeshDesc{
      .positions = positions,
      .normals = {},
      .indices = indices,
      .transform = tr(pos, v3(1.0f, 1.0f, 1.0f), quat_yaw(yaw)),
      .color = c,
      .collision = collision,
      .tags = tags,
  });

  box_rot(w, v3(pos.x - std::cos(yaw) * inner_radius, pos.y + pillar_height * 0.5f,
                pos.z + std::sin(yaw) * inner_radius),
          v3(thickness, pillar_height, depth), yaw, c, collision, tags);
  box_rot(w, v3(pos.x + std::cos(yaw) * inner_radius, pos.y + pillar_height * 0.5f,
                pos.z - std::sin(yaw) * inner_radius),
          v3(thickness, pillar_height, depth), yaw, c, collision, tags);
}

void add_ring_walkway(WorldContext& w, float radius, float width, Color c,
                      const std::string& tag) {
  const int segments = 72;
  const float chord = kTau * radius / static_cast<float>(segments);
  for (int i = 0; i < segments; ++i) {
    const float a = kTau * static_cast<float>(i) / static_cast<float>(segments);
    const float x = std::cos(a) * radius;
    const float z = std::sin(a) * radius;
    box_rot(w, v3(x, 0.035f, z), v3(chord * 1.04f, 0.07f, width), -a, c,
            Collision::Box, {tag, "walkable", "plaza-ring"});
  }
}

void add_radial_path(WorldContext& w, float angle, float r0, float r1, float width,
                     Color c, const std::string& tag) {
  const float mid = (r0 + r1) * 0.5f;
  const float len = r1 - r0;
  const float x = std::cos(angle) * mid;
  const float z = std::sin(angle) * mid;
  box_rot(w, v3(x, 0.045f, z), v3(len, 0.09f, width), -angle, c,
          Collision::Box, {tag, "walkable", "radial-path"});
}

void add_low_rail_pair(WorldContext& w, float angle, float r0, float r1, float half_width,
                       Color rail_color, Color glow_color, const std::string& tag) {
  const float len = r1 - r0;
  const float mid = (r0 + r1) * 0.5f;
  const float ca = std::cos(angle);
  const float sa = std::sin(angle);
  const float px = -sa;
  const float pz = ca;
  for (int side = -1; side <= 1; side += 2) {
    const float ox = px * half_width * static_cast<float>(side);
    const float oz = pz * half_width * static_cast<float>(side);
    box_rot(w, v3(ca * mid + ox, 0.38f, sa * mid + oz), v3(len, 0.18f, 0.14f), -angle,
            rail_color, Collision::Box, {tag, "safety-rail"});
    box_rot(w, v3(ca * mid + ox, 0.57f, sa * mid + oz), v3(len, 0.05f, 0.08f), -angle,
            glow_color, Collision::None, {tag, "emerald-guide-strip"});
  }
}

void add_sector_label(WorldContext& w, Vec3 pos, float yaw, Color emerald,
                      Color amber, const std::string& tag) {
  box_rot(w, pos, v3(1.35f, 0.05f, 0.12f), yaw, emerald, Collision::None,
          {tag, "floor-glyph"});
  box_rot(w, v3(pos.x, pos.y + 0.015f, pos.z), v3(0.18f, 0.06f, 1.1f), yaw, emerald,
          Collision::None, {tag, "floor-glyph"});
  sphere(w, v3(pos.x, pos.y + 0.08f, pos.z), v3(0.18f, 0.05f, 0.18f), amber,
         Collision::None, {tag, "alert-dot"});
}

void build_central_battery(WorldContext& w) {
  const Color stone = color(0.27f, 0.31f, 0.34f);
  const Color dark_stone = color(0.16f, 0.19f, 0.22f);
  const Color metal = color(0.38f, 0.43f, 0.44f);
  const Color emerald = color(0.06f, 0.82f, 0.38f);
  const Color pale_emerald = color(0.44f, 0.95f, 0.62f);
  const Color glass = color(0.12f, 0.70f, 0.42f);

  add_beveled_prism_mesh(w, v3(0.0f, 0.38f, 0.0f), 7.4f, 0.72f, 16, 0.10f,
                         stone, Collision::Mesh, 0.0f, {"central-dais", "walkable"});
  add_beveled_prism_mesh(w, v3(0.0f, 1.02f, 0.0f), 5.65f, 0.56f, 16, 0.12f,
                         dark_stone, Collision::Mesh, 0.0f, {"central-dais", "step"});
  add_beveled_prism_mesh(w, v3(0.0f, 1.48f, 0.0f), 4.05f, 0.38f, 16, 0.10f,
                         metal, Collision::Mesh, 0.0f, {"central-dais", "battery-plinth"});

  cyl(w, v3(0.0f, 6.4f, 0.0f), v3(2.15f, 9.8f, 2.15f), glass, Collision::Box,
      {"central-power-column", "solid"});
  cyl(w, v3(0.0f, 6.4f, 0.0f), v3(2.55f, 9.95f, 2.55f), color(0.07f, 0.48f, 0.30f),
      Collision::None, {"central-power-aura"});
  add_cone_mesh(w, v3(0.0f, 12.0f, 0.0f), 2.85f, 2.65f, 32,
                color(0.40f, 0.47f, 0.46f), Collision::Mesh,
                {"central-battery-cap", "solid"});
  add_cone_mesh(w, v3(0.0f, 0.95f, 0.0f), 2.85f, 1.55f, 32,
                color(0.35f, 0.40f, 0.39f), Collision::Mesh,
                {"central-battery-base", "solid"});

  for (int i = 0; i < 8; ++i) {
    const float a = kTau * static_cast<float>(i) / 8.0f;
    const float x = std::cos(a) * 3.15f;
    const float z = std::sin(a) * 3.15f;
    cyl(w, v3(x, 6.1f, z), v3(0.23f, 8.8f, 0.23f), metal, Collision::Box,
        {"battery-spine", "solid"});
    box_rot(w, v3(std::cos(a) * 3.32f, 3.8f, std::sin(a) * 3.32f),
            v3(0.13f, 4.9f, 0.18f), -a, emerald, Collision::None,
            {"vertical-energy-conduit"});
    sphere(w, v3(std::cos(a) * 3.45f, 10.55f, std::sin(a) * 3.45f),
           v3(0.34f, 0.34f, 0.34f), pale_emerald, Collision::None,
           {"battery-node", "glow"});
  }

  add_torus_mesh(w, v3(0.0f, 3.25f, 0.0f), 3.25f, 0.08f, 64, 8, emerald,
                 Collision::None, 0.0f, {"floating-energy-ring"});
  add_torus_mesh(w, v3(0.0f, 7.0f, 0.0f), 3.55f, 0.09f, 64, 8, pale_emerald,
                 Collision::None, 0.35f, {"floating-energy-ring"});
  add_torus_mesh(w, v3(0.0f, 10.85f, 0.0f), 3.05f, 0.07f, 64, 8, emerald,
                 Collision::None, -0.28f, {"floating-energy-ring"});

  for (int i = 0; i < 16; ++i) {
    const float a = kTau * static_cast<float>(i) / 16.0f;
    box_rot(w, v3(std::cos(a) * 5.25f, 1.72f, std::sin(a) * 5.25f),
            v3(1.0f, 0.10f, 0.18f), -a, emerald, Collision::None,
            {"dais-energy-inlay"});
  }

  point_light(w, v3(0.0f, 5.0f, 0.0f), color(0.10f, 1.0f, 0.45f), 6.0f, 19.0f);
  point_light(w, v3(0.0f, 11.2f, 0.0f), color(0.35f, 1.0f, 0.62f), 4.5f, 22.0f);

  object(w, "green_energy_vfx_pack", v3(0.0f, 5.9f, 0.0f), v3(2.4f, 5.6f, 2.4f),
         0.0f, {"central-battery-vfx"});
}

void build_plaza_floor_and_paths(WorldContext& w) {
  const Color plaza = color(0.21f, 0.25f, 0.27f);
  const Color inner = color(0.29f, 0.34f, 0.35f);
  const Color path_c = color(0.30f, 0.36f, 0.37f);
  const Color emerald = color(0.06f, 0.78f, 0.34f);
  const Color rail = color(0.18f, 0.22f, 0.24f);
  const Color amber = color(0.86f, 0.62f, 0.12f);

  add_ring_walkway(w, 9.8f, 3.6f, inner, "inner-processional-ring");
  add_ring_walkway(w, 17.2f, 2.2f, plaza, "outer-orientation-ring");
  add_ring_walkway(w, 25.4f, 2.2f, color(0.18f, 0.22f, 0.24f), "skyline-patrol-ring");

  const float path_angles[6] = {0.0f, kPi / 3.0f, 2.0f * kPi / 3.0f, kPi,
                                4.0f * kPi / 3.0f, 5.0f * kPi / 3.0f};
  const char* tags[6] = {"path-command-hall", "path-launch-platform", "path-training-arena",
                         "path-construct-hall", "path-recharge-room", "path-observation"};
  for (int i = 0; i < 6; ++i) {
    add_radial_path(w, path_angles[i], 5.5f, 31.0f, 3.6f, path_c, tags[i]);
    add_low_rail_pair(w, path_angles[i], 11.2f, 30.8f, 2.05f, rail, emerald, tags[i]);
  }

  for (int i = 0; i < 32; ++i) {
    const float a = kTau * static_cast<float>(i) / 32.0f;
    const float r = (i % 2 == 0) ? 14.3f : 20.2f;
    box_rot(w, v3(std::cos(a) * r, 0.095f, std::sin(a) * r),
            v3(2.6f, 0.07f, 0.10f), -a, emerald, Collision::None,
            {"emerald-floor-circuit"});
  }

  for (int i = 0; i < 6; ++i) {
    const float a = path_angles[i];
    add_sector_label(w, v3(std::cos(a) * 13.3f, 0.13f, std::sin(a) * 13.3f), -a,
                     emerald, amber, tags[i]);
  }

  for (int i = 0; i < 12; ++i) {
    const float a = kTau * static_cast<float>(i) / 12.0f + 0.12f;
    cyl(w, v3(std::cos(a) * 21.8f, 0.65f, std::sin(a) * 21.8f),
        v3(0.25f, 1.2f, 0.25f), rail, Collision::Box, {"plaza-bollard", "solid"});
    sphere(w, v3(std::cos(a) * 21.8f, 1.34f, std::sin(a) * 21.8f),
           v3(0.33f, 0.22f, 0.33f), emerald, Collision::None,
           {"plaza-bollard-light"});
  }
}

void build_command_hall(WorldContext& w) {
  const Color stone = color(0.20f, 0.25f, 0.29f);
  const Color wall = color(0.24f, 0.30f, 0.33f);
  const Color metal = color(0.40f, 0.45f, 0.45f);
  const Color emerald = color(0.05f, 0.80f, 0.35f);
  const Color hologram = color(0.20f, 0.95f, 0.58f);
  const float z = 0.0f;
  const float x = 36.0f;

  add_beveled_prism_mesh(w, v3(x, 0.22f, z), 8.2f, 0.38f, 8, 0.08f, stone,
                         Collision::Mesh, kPi / 8.0f,
                         {"command-hall-floor", "walkable"});
  box(w, v3(x + 5.7f, 2.1f, z), v3(0.7f, 4.2f, 13.6f), wall, Collision::Box,
      {"command-hall-back-wall", "solid"});
  box(w, v3(x, 2.1f, z + 6.6f), v3(10.8f, 4.2f, 0.62f), wall, Collision::Box,
      {"command-hall-side-wall", "solid"});
  box(w, v3(x, 2.1f, z - 6.6f), v3(10.8f, 4.2f, 0.62f), wall, Collision::Box,
      {"command-hall-side-wall", "solid"});
  box(w, v3(x + 1.0f, 4.55f, z), v3(10.6f, 0.35f, 13.8f), color(0.16f, 0.20f, 0.23f),
      Collision::Box, {"command-hall-roof", "solid"});

  for (int i = 0; i < 4; ++i) {
    const float zz = -4.8f + static_cast<float>(i) * 3.2f;
    cyl(w, v3(x - 3.6f, 2.25f, zz), v3(0.45f, 4.5f, 0.45f), metal, Collision::Box,
        {"command-pillar", "solid"});
    cyl(w, v3(x + 3.7f, 2.25f, zz), v3(0.45f, 4.5f, 0.45f), metal, Collision::Box,
        {"command-pillar", "solid"});
    box(w, v3(x - 3.6f, 3.45f, zz), v3(0.16f, 1.7f, 0.10f), emerald, Collision::None,
        {"command-pillar-conduit"});
    box(w, v3(x + 3.7f, 3.45f, zz), v3(0.16f, 1.7f, 0.10f), emerald, Collision::None,
        {"command-pillar-conduit"});
  }

  add_torus_mesh(w, v3(x - 0.4f, 1.05f, z), 2.45f, 0.08f, 48, 8, emerald,
                 Collision::None, 0.0f, {"starmap-dais-ring"});
  cyl(w, v3(x - 0.4f, 0.72f, z), v3(2.0f, 0.55f, 2.0f), metal, Collision::Box,
      {"starmap-console", "solid"});
  sphere(w, v3(x - 0.4f, 2.0f, z), v3(1.15f, 1.15f, 1.15f), hologram, Collision::None,
         {"holographic-star-map"});

  for (int i = 0; i < 10; ++i) {
    const float a = kTau * static_cast<float>(i) / 10.0f;
    const float rr = 1.55f + 0.15f * static_cast<float>(i % 3);
    sphere(w, v3(x - 0.4f + std::cos(a) * rr, 2.0f + 0.28f * std::sin(a * 2.0f),
                 z + std::sin(a) * rr),
           v3(0.09f, 0.09f, 0.09f), (i % 4 == 0) ? color(0.92f, 0.62f, 0.13f) : emerald,
           Collision::None, {"sector-alert-node"});
  }

  object(w, "mission_console_hologram", v3(x - 0.4f, 1.34f, z), v3(1.65f, 1.65f, 1.65f),
         -kPi / 2.0f, {"mission-selection-console"});
  object(w, "star_map_prop", v3(x - 0.4f, 2.05f, z), v3(1.35f, 1.35f, 1.35f),
         0.0f, {"sector-map-hologram"});
  object(w, "trainer_ai_hologram_npc", v3(x - 4.1f, 0.75f, 2.7f), v3(1.0f, 1.0f, 1.0f),
         -kPi / 2.0f, {"mentor-construct-command"});

  point_light(w, v3(x - 0.4f, 2.8f, z), color(0.10f, 0.95f, 0.45f), 3.4f, 11.0f);
}

void build_training_arena(WorldContext& w) {
  const float a = 2.0f * kPi / 3.0f;
  const float cx = std::cos(a) * 39.0f;
  const float cz = std::sin(a) * 39.0f;
  const Color floor = color(0.23f, 0.26f, 0.29f);
  const Color wall = color(0.17f, 0.21f, 0.24f);
  const Color green = color(0.05f, 0.85f, 0.37f);
  const Color yellow = color(0.86f, 0.66f, 0.14f);
  const Color cover = color(0.38f, 0.44f, 0.45f);

  box_rot(w, v3(cx, 0.08f, cz), v3(18.0f, 0.16f, 14.0f), -a, floor, Collision::Box,
          {"training-arena-floor", "walkable"});
  box_rot(w, v3(cx + std::cos(a) * 7.7f, 1.55f, cz + std::sin(a) * 7.7f),
          v3(0.55f, 3.1f, 14.5f), -a, wall, Collision::Box,
          {"training-arena-back-wall", "solid"});
  box_rot(w, v3(cx - std::sin(a) * 7.25f, 1.25f, cz + std::cos(a) * 7.25f),
          v3(18.0f, 2.5f, 0.42f), -a, wall, Collision::Box,
          {"training-arena-side-wall", "solid"});
  box_rot(w, v3(cx + std::sin(a) * 7.25f, 1.25f, cz - std::cos(a) * 7.25f),
          v3(18.0f, 2.5f, 0.42f), -a, wall, Collision::Box,
          {"training-arena-side-wall", "solid"});

  for (int lane = -2; lane <= 2; ++lane) {
    const float off = static_cast<float>(lane) * 2.45f;
    box_rot(w, v3(cx - std::sin(a) * off, 0.18f, cz + std::cos(a) * off),
            v3(14.8f, 0.05f, 0.10f), -a, green, Collision::None,
            {"training-lane-line"});
  }

  for (int i = 0; i < 6; ++i) {
    const float forward = -5.6f + static_cast<float>(i % 3) * 4.1f;
    const float side = (i < 3 ? -1.0f : 1.0f) * (2.0f + static_cast<float>(i % 2) * 1.2f);
    const Vec3 pos = v3(cx + std::cos(a) * forward - std::sin(a) * side,
                        0.75f,
                        cz + std::sin(a) * forward + std::cos(a) * side);
    box_rot(w, pos, v3(1.15f, 1.5f, 2.2f), -a + (i % 2 == 0 ? 0.24f : -0.18f),
            cover, Collision::Box, {"training-cover", "solid"});
    object(w, "destructible_cover_set", v3(pos.x, pos.y + 0.05f, pos.z),
           v3(0.9f, 0.9f, 0.9f), -a, {"cover-visual"});
  }

  for (int i = 0; i < 5; ++i) {
    const float forward = -6.0f + static_cast<float>(i) * 3.0f;
    const float side = (i % 2 == 0) ? 5.1f : -5.1f;
    const Vec3 base = v3(cx + std::cos(a) * forward - std::sin(a) * side,
                         0.0f,
                         cz + std::sin(a) * forward + std::cos(a) * side);
    cyl(w, v3(base.x, 1.5f, base.z), v3(0.36f, 3.0f, 0.36f), color(0.28f, 0.33f, 0.35f),
        Collision::Box, {"hover-practice-column", "solid"});
    add_torus_mesh(w, v3(base.x, 3.15f, base.z), 0.78f, 0.05f, 32, 6, green,
                   Collision::None, 0.0f, {"hover-target-ring"});
  }

  for (int i = 0; i < 4; ++i) {
    const float side = -4.5f + static_cast<float>(i) * 3.0f;
    const Vec3 pos = v3(cx + std::cos(a) * 7.1f - std::sin(a) * side,
                        1.15f,
                        cz + std::sin(a) * 7.1f + std::cos(a) * side);
    cyl(w, pos, v3(0.55f, 0.30f, 0.55f), color(0.34f, 0.38f, 0.39f), Collision::Box,
        {"enemy-spawner-base", "solid"});
    sphere(w, v3(pos.x, pos.y + 0.75f, pos.z), v3(0.46f, 0.46f, 0.46f), yellow,
           Collision::None, {"hostile-hologram-target"});
    object(w, (i % 2 == 0) ? "robotic_drone_enemy" : "fear_construct_enemy",
           v3(pos.x, pos.y + 1.0f, pos.z), v3(0.75f, 0.75f, 0.75f), -a + kPi,
           {"stationary-training-target"});
  }

  object(w, "tutorial_hologram_projectors", v3(cx - std::cos(a) * 7.4f, 0.23f,
                                                cz - std::sin(a) * 7.4f),
         v3(1.0f, 1.0f, 1.0f), -a, {"training-projector"});
  object(w, "construct_pack_basic_attack", v3(cx - std::sin(a) * -5.9f, 1.4f,
                                                cz + std::cos(a) * -5.9f),
         v3(0.95f, 0.95f, 0.95f), -a, {"bolt-practice-preview"});
  object(w, "construct_pack_defense", v3(cx - std::sin(a) * 5.9f, 1.4f,
                                          cz + std::cos(a) * 5.9f),
         v3(0.95f, 0.95f, 0.95f), -a, {"shield-practice-preview"});

  point_light(w, v3(cx, 4.2f, cz), color(0.18f, 0.92f, 0.50f), 3.2f, 16.0f);
}

void build_construct_hall(WorldContext& w) {
  const float a = kPi;
  const float cx = -38.0f;
  const float cz = 0.0f;
  const Color floor = color(0.25f, 0.28f, 0.31f);
  const Color wall = color(0.18f, 0.22f, 0.25f);
  const Color metal = color(0.37f, 0.42f, 0.43f);
  const Color emerald = color(0.09f, 0.82f, 0.37f);

  box_rot(w, v3(cx, 0.09f, cz), v3(17.0f, 0.18f, 11.0f), -a, floor, Collision::Box,
          {"construct-hall-floor", "walkable"});
  box(w, v3(cx - 6.2f, 2.45f, cz), v3(0.65f, 4.9f, 11.5f), wall, Collision::Box,
      {"construct-hall-back-wall", "solid"});
  box(w, v3(cx, 2.45f, cz + 5.55f), v3(12.8f, 4.9f, 0.55f), wall, Collision::Box,
      {"construct-hall-side-wall", "solid"});
  box(w, v3(cx, 2.45f, cz - 5.55f), v3(12.8f, 4.9f, 0.55f), wall, Collision::Box,
      {"construct-hall-side-wall", "solid"});

  add_arch_mesh(w, v3(cx + 5.8f, 0.0f, cz), 2.65f, 0.42f, 1.0f, 3.1f, 20, metal,
                Collision::Mesh, kPi / 2.0f, {"construct-hall-entry-arch", "solid"});

  const char* assets[6] = {"construct_pack_basic_attack", "construct_pack_defense",
                           "construct_pack_control", "construct_pack_traversal",
                           "construct_pack_utility", "construct_pack_heavy"};
  for (int i = 0; i < 6; ++i) {
    const bool left = i < 3;
    const float z = (left ? -3.6f : 3.6f);
    const float x = cx + 2.8f - static_cast<float>(i % 3) * 3.6f;
    box(w, v3(x, 0.62f, z), v3(2.0f, 0.5f, 1.6f), metal, Collision::Box,
        {"construct-display-pedestal", "solid"});
    cyl(w, v3(x, 1.06f, z), v3(0.92f, 0.18f, 0.92f), color(0.20f, 0.25f, 0.26f),
        Collision::Box, {"construct-display-turntable", "solid"});
    sphere(w, v3(x, 2.0f, z), v3(0.58f, 0.58f, 0.58f), emerald, Collision::None,
           {"construct-hardlight-statue"});
    add_torus_mesh(w, v3(x, 1.48f, z), 0.95f, 0.04f, 32, 6, emerald, Collision::None,
                   0.0f, {"construct-display-ring"});
    object(w, assets[i], v3(x, 1.55f, z), v3(0.80f, 0.80f, 0.80f),
           left ? kPi / 2.0f : -kPi / 2.0f, {"construct-archetype-preview"});
    box(w, v3(x + 0.02f, 1.16f, z + (left ? 0.96f : -0.96f)),
        v3(1.35f, 0.08f, 0.18f), emerald, Collision::None, {"holographic-plaque"});
  }

  object(w, "construct_icons_radial", v3(cx + 4.1f, 1.45f, 0.0f), v3(1.4f, 1.4f, 1.4f),
         kPi / 2.0f, {"construct-wheel-practice"});
  object(w, "ring_ui_hud_assets", v3(cx + 2.2f, 1.25f, 0.0f), v3(0.9f, 0.9f, 0.9f),
         kPi / 2.0f, {"ring-ui-calibration"});

  point_light(w, v3(cx - 1.5f, 3.5f, 0.0f), color(0.16f, 0.94f, 0.50f), 2.7f, 13.0f);
}

void build_recharge_room(WorldContext& w) {
  const float a = 4.0f * kPi / 3.0f;
  const float cx = std::cos(a) * 36.5f;
  const float cz = std::sin(a) * 36.5f;
  const Color floor = color(0.20f, 0.24f, 0.26f);
  const Color wall = color(0.16f, 0.20f, 0.22f);
  const Color metal = color(0.35f, 0.40f, 0.40f);
  const Color emerald = color(0.08f, 0.90f, 0.42f);
  const Color soft = color(0.36f, 0.95f, 0.62f);

  add_beveled_prism_mesh(w, v3(cx, 0.18f, cz), 6.7f, 0.32f, 12, 0.08f, floor,
                         Collision::Mesh, 0.0f, {"recharge-room-floor", "walkable"});
  add_torus_mesh(w, v3(cx, 0.40f, cz), 4.7f, 0.06f, 56, 8, emerald, Collision::None,
                 0.0f, {"recharge-concentric-ring"});
  add_torus_mesh(w, v3(cx, 0.48f, cz), 2.9f, 0.05f, 48, 8, soft, Collision::None,
                 0.18f, {"recharge-concentric-ring"});
  cyl(w, v3(cx, 0.85f, cz), v3(1.1f, 1.0f, 1.1f), metal, Collision::Box,
      {"personal-battery-pedestal", "solid"});
  sphere(w, v3(cx, 1.65f, cz), v3(0.54f, 0.54f, 0.54f), emerald, Collision::None,
         {"recharge-core-glow"});
  object(w, "recharge_battery_prop", v3(cx, 0.9f, cz), v3(1.2f, 1.2f, 1.2f), -a,
         {"interactive-recharge-station"});
  object(w, "ring_beam_fx", v3(cx, 1.85f, cz), v3(1.25f, 1.25f, 1.25f), 0.0f,
         {"recharge-ritual-beam"});

  for (int i = 0; i < 8; ++i) {
    const float t = kTau * static_cast<float>(i) / 8.0f;
    const Vec3 p = v3(cx + std::cos(t) * 5.7f, 1.65f, cz + std::sin(t) * 5.7f);
    cyl(w, p, v3(0.28f, 3.1f, 0.28f), wall, Collision::Box,
        {"recharge-room-column", "solid"});
    box_rot(w, v3(p.x, p.y + 0.4f, p.z), v3(0.10f, 2.1f, 0.14f), -t, emerald,
            Collision::None, {"recharge-column-glyph"});
  }

  point_light(w, v3(cx, 2.35f, cz), color(0.20f, 1.0f, 0.55f), 4.2f, 12.0f);
}

void build_launch_platform(WorldContext& w) {
  const float a = kPi / 3.0f;
  const float cx = std::cos(a) * 39.0f;
  const float cz = std::sin(a) * 39.0f;
  const Color floor = color(0.25f, 0.30f, 0.32f);
  const Color metal = color(0.39f, 0.45f, 0.45f);
  const Color emerald = color(0.06f, 0.88f, 0.39f);
  const Color voidedge = color(0.12f, 0.16f, 0.19f);

  add_beveled_prism_mesh(w, v3(cx, 0.20f, cz), 7.4f, 0.38f, 10, 0.08f, floor,
                         Collision::Mesh, 0.0f, {"launch-platform-floor", "walkable"});
  add_torus_mesh(w, v3(cx, 0.45f, cz), 5.4f, 0.08f, 60, 8, emerald, Collision::None,
                 0.0f, {"launch-pad-energy-ring"});

  for (int i = 0; i < 10; ++i) {
    const float t = kTau * static_cast<float>(i) / 10.0f;
    const float r = 7.4f;
    cyl(w, v3(cx + std::cos(t) * r, 0.70f, cz + std::sin(t) * r),
        v3(0.22f, 1.2f, 0.22f), voidedge, Collision::Box, {"launch-safety-post", "solid"});
    if (i % 2 == 0) {
      box_rot(w, v3(cx + std::cos(t + 0.31f) * r, 1.12f, cz + std::sin(t + 0.31f) * r),
              v3(3.1f, 0.14f, 0.16f), -t, metal, Collision::Box,
              {"launch-safety-rail", "solid"});
    }
  }

  const Vec3 gate_pos = v3(cx + std::cos(a) * 3.0f, 2.85f, cz + std::sin(a) * 3.0f);
  add_torus_mesh(w, gate_pos, 2.55f, 0.18f, 64, 10, metal, Collision::Mesh,
                 -a, {"portal-gate-collision-ring", "solid"});
  add_torus_mesh(w, gate_pos, 2.12f, 0.08f, 64, 8, emerald, Collision::None,
                 -a, {"portal-inner-energy-ring"});
  sphere(w, v3(gate_pos.x, gate_pos.y, gate_pos.z), v3(1.65f, 1.65f, 0.16f),
         color(0.07f, 0.70f, 0.42f), Collision::None, {"portal-curtain"});
  object(w, "portal_gate_prop", gate_pos, v3(1.25f, 1.25f, 1.25f), -a,
         {"mission-portal-gate"});
  object(w, "flight_trail_fx", v3(cx - std::cos(a) * 1.6f, 1.6f, cz - std::sin(a) * 1.6f),
         v3(1.2f, 1.2f, 1.2f), -a, {"preflight-comfort-vfx"});

  box_rot(w, v3(cx - std::sin(a) * 4.0f, 0.95f, cz + std::cos(a) * 4.0f),
          v3(2.2f, 1.0f, 1.2f), -a, metal, Collision::Box,
          {"loadout-console", "solid"});
  object(w, "ui_wrist_menu_panels", v3(cx - std::sin(a) * 4.0f, 1.55f,
                                        cz + std::cos(a) * 4.0f),
         v3(1.1f, 1.1f, 1.1f), -a, {"comfort-loadout-panel"});

  point_light(w, v3(cx, 3.2f, cz), color(0.15f, 0.95f, 0.50f), 3.8f, 14.0f);
}

void build_observation_and_spawn_orientation(WorldContext& w) {
  const float a = 5.0f * kPi / 3.0f;
  const float cx = std::cos(a) * 34.0f;
  const float cz = std::sin(a) * 34.0f;
  const Color floor = color(0.22f, 0.27f, 0.29f);
  const Color metal = color(0.35f, 0.41f, 0.42f);
  const Color emerald = color(0.06f, 0.84f, 0.37f);
  const Color blue = color(0.20f, 0.45f, 0.72f);

  add_beveled_prism_mesh(w, v3(cx, 0.16f, cz), 5.2f, 0.28f, 8, 0.08f, floor,
                         Collision::Mesh, 0.0f, {"observation-balcony", "walkable"});
  box_rot(w, v3(cx + std::cos(a) * 3.8f, 0.78f, cz + std::sin(a) * 3.8f),
          v3(0.35f, 1.25f, 7.5f), -a, metal, Collision::Box,
          {"observation-outer-rail", "solid"});
  box_rot(w, v3(cx - std::sin(a) * 3.8f, 0.78f, cz + std::cos(a) * 3.8f),
          v3(7.5f, 1.25f, 0.35f), -a, metal, Collision::Box,
          {"observation-side-rail", "solid"});
  box_rot(w, v3(cx + std::sin(a) * 3.8f, 0.78f, cz - std::cos(a) * 3.8f),
          v3(7.5f, 1.25f, 0.35f), -a, metal, Collision::Box,
          {"observation-side-rail", "solid"});

  object(w, "skybox_space_nebula_fx", v3(cx + std::cos(a) * 6.0f, 2.8f,
                                          cz + std::sin(a) * 6.0f),
         v3(2.4f, 2.4f, 2.4f), -a, {"space-vista-framing"});
  sphere(w, v3(cx + std::cos(a) * 4.8f, 2.0f, cz + std::sin(a) * 4.8f),
         v3(1.0f, 0.24f, 1.0f), blue, Collision::None, {"distant-planet-hologram"});
  add_torus_mesh(w, v3(cx + std::cos(a) * 4.8f, 2.0f, cz + std::sin(a) * 4.8f),
                 1.55f, 0.035f, 40, 6, emerald, Collision::None, 0.0f,
                 {"planet-orbit-line"});

  point_light(w, v3(cx, 2.0f, cz), color(0.12f, 0.75f, 0.45f), 2.0f, 9.0f);
}

void build_skyline(WorldContext& w, SimpleRng& rng) {
  const Color stone_a = color(0.13f, 0.17f, 0.20f);
  const Color stone_b = color(0.18f, 0.22f, 0.25f);
  const Color metal = color(0.25f, 0.30f, 0.32f);
  const Color emerald = color(0.05f, 0.62f, 0.31f);
  const Color soft = color(0.30f, 0.85f, 0.55f);

  for (int ring = 0; ring < 2; ++ring) {
    const int count = (ring == 0) ? 28 : 34;
    const float radius = (ring == 0) ? 52.0f : 66.0f;
    for (int i = 0; i < count; ++i) {
      const float jitter = rand_range(rng, -0.045f, 0.045f);
      const float a = kTau * static_cast<float>(i) / static_cast<float>(count) + jitter;
      const float x = std::cos(a) * radius;
      const float z = std::sin(a) * radius;
      const float width = rand_range(rng, 2.1f, 4.2f);
      const float depth = rand_range(rng, 1.7f, 3.6f);
      const float height = rand_range(rng, ring == 0 ? 5.0f : 8.0f, ring == 0 ? 13.0f : 22.0f);
      const Color c = (i % 3 == 0) ? stone_b : stone_a;
      box_rot(w, v3(x, height * 0.5f, z), v3(width, height, depth), -a + kPi * 0.5f,
              c, Collision::Box, {"distant-oa-skyline", "solid"});
      if (i % 4 == 0) {
        add_cone_mesh(w, v3(x, height + 1.2f, z), width * 0.55f, 2.4f, 8,
                      metal, Collision::None, {"skyline-spire-cap"});
      }
      if (i % 2 == 0) {
        box_rot(w, v3(x, height * 0.68f, z), v3(0.08f, height * 0.42f, 0.10f),
                -a + kPi * 0.5f, emerald, Collision::None, {"skyline-emerald-window"});
      }
    }
  }

  for (int i = 0; i < 12; ++i) {
    const float a = kTau * static_cast<float>(i) / 12.0f + 0.09f;
    const float r = 45.0f;
    add_arch_mesh(w, v3(std::cos(a) * r, 0.0f, std::sin(a) * r), 2.1f, 0.45f, 1.2f,
                  3.0f, 18, metal, Collision::Mesh, -a + kPi * 0.5f,
                  {"distant-guardian-arch", "solid"});
    sphere(w, v3(std::cos(a) * r, 5.85f, std::sin(a) * r), v3(0.35f, 0.35f, 0.35f),
           soft, Collision::None, {"arch-beacon"});
  }
}

void build_small_details(WorldContext& w, SimpleRng& rng) {
  const Color crate_c = color(0.36f, 0.41f, 0.41f);
  const Color emerald = color(0.06f, 0.82f, 0.38f);
  const Color amber = color(0.86f, 0.60f, 0.13f);
  const Color metal = color(0.31f, 0.36f, 0.37f);

  for (int i = 0; i < 10; ++i) {
    const float a = rand_range(rng, 0.0f, kTau);
    const float r = rand_range(rng, 22.0f, 30.0f);
    if (std::abs(std::sin(a * 3.0f)) < 0.16f) {
      continue;
    }
    const float x = std::cos(a) * r;
    const float z = std::sin(a) * r;
    box_rot(w, v3(x, 0.42f, z), v3(0.9f, 0.8f, 0.9f), rand_range(rng, 0.0f, kTau),
            crate_c, Collision::Box, {"cargo-crate", "solid", "grab-prop-area"});
    if (i % 2 == 0) {
      object(w, "interactive_cargo_crates", v3(x, 0.45f, z), v3(0.75f, 0.75f, 0.75f),
             rand_range(rng, 0.0f, kTau), {"interactive-cargo-visual"});
    }
  }

  for (int i = 0; i < 9; ++i) {
    const float a = kTau * static_cast<float>(i) / 9.0f + 0.21f;
    const float r = 14.8f;
    cyl(w, v3(std::cos(a) * r, 0.50f, std::sin(a) * r), v3(0.22f, 0.85f, 0.22f),
        metal, Collision::Box, {"tutorial-projector-base", "solid"});
    object(w, "tutorial_hologram_projectors",
           v3(std::cos(a) * r, 0.78f, std::sin(a) * r), v3(0.55f, 0.55f, 0.55f),
           -a + kPi, {"locomotion-selector-projector"});
    box_rot(w, v3(std::cos(a) * (r + 0.55f), 1.35f, std::sin(a) * (r + 0.55f)),
            v3(1.0f, 0.05f, 0.42f), -a, (i % 3 == 0) ? amber : emerald,
            Collision::None, {"floating-tutorial-panel"});
  }

  object(w, "ring_model", v3(-2.9f, 1.45f, -5.4f), v3(1.35f, 1.35f, 1.35f),
         0.25f, {"first-ring-activation-display"});
  object(w, "lantern_suit_hands_arms", v3(-4.0f, 1.1f, -5.2f), v3(1.0f, 1.0f, 1.0f),
         0.2f, {"avatar-calibration-preview"});
  object(w, "drone_scanner_prop", v3(3.6f, 1.25f, -4.8f), v3(0.9f, 0.9f, 0.9f),
         -0.4f, {"scanner-drone-preview"});

  for (int i = 0; i < 6; ++i) {
    const float a = kTau * static_cast<float>(i) / 6.0f + 0.5f;
    const float r = 18.6f;
    sphere(w, v3(std::cos(a) * r, 2.1f + 0.35f * static_cast<float>(i % 2),
                 std::sin(a) * r),
           v3(0.42f, 0.42f, 0.42f), color(0.18f, 0.86f, 0.50f), Collision::None,
           {"floating-corps-traffic-light"});
  }
}

}  // namespace

class GeneratedWorld20260706212302 : public IWorldBuilder {
public:
  void build(WorldContext& w) override {
    const std::uint64_t seed_value = static_cast<std::uint64_t>(w.seed());
    SimpleRng rng(seed_value ^ 0x9E3779B97F4A7C15ull);

    w.sky(SkyDesc{
        .top = color(0.018f, 0.032f, 0.075f),
        .horizon = color(0.075f, 0.145f, 0.155f),
        .sun_direction = v3(-0.36f, -0.58f, -0.28f),
        .sun_color = color(0.72f, 0.94f, 0.86f),
        .sun_intensity = 2.25f,
        .ambient_color = color(0.18f, 0.38f, 0.30f),
        .ambient_intensity = 0.47f,
        .fog_density = 0.0065f,
        .exposure = 1.18f,
    });

    directional_light(w, v3(-0.36f, -0.58f, -0.28f), color(0.72f, 0.94f, 0.86f), 1.85f);

    w.ground(GroundDesc{
        .size_m = 150.0f,
        .y = 0.0f,
        .color = color(0.115f, 0.145f, 0.165f),
        .grid = true,
        .collision = Collision::Box,
    });

    build_plaza_floor_and_paths(w);
    build_central_battery(w);
    build_command_hall(w);
    build_training_arena(w);
    build_construct_hall(w);
    build_recharge_room(w);
    build_launch_platform(w);
    build_observation_and_spawn_orientation(w);
    build_skyline(w, rng);
    build_small_details(w, rng);

    point_light(w, v3(-5.0f, 2.8f, -9.0f), color(0.12f, 0.95f, 0.48f), 2.4f, 10.0f);
    point_light(w, v3(7.5f, 2.8f, -4.0f), color(0.12f, 0.90f, 0.44f), 2.2f, 9.0f);

    w.player_spawn(v3(0.0f, 0.05f, -15.0f), v3(0.0f, 0.0f, 1.0f));
  }

  std::string id() const override {
    return "generated.world.20260706-212302-vr-environment-test";
  }
};

PTV_REGISTER_WORLD_BUILDER("generated.world.20260706-212302-vr-environment-test", GeneratedWorld20260706212302)

}  // namespace ptv::world
