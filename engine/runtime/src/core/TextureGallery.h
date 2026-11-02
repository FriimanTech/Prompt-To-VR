#pragma once

// On-headset texture verification (debug.ptv.verify_textures).
//
// Shows every textured mesh asset the scene places, one at a time, 1.1 m in
// front of the player's eyes on the scene's own sky, lit by the scene's own
// lights and drawn through the real Quest pipeline (ASTC from KTX2, the
// shipped shaders, MSAA, the compositor). quest_runtime_verify takes a Quest
// screenshot during each slot and compares it with an offline render of the
// same asset, so "the texture is bound" and "the texture is visible and
// mapped right on the device" are both checked.
//
// Timeline: kSettleS of nothing, then kSlotS per asset. Each switch logs
//   texgallery: show <i>/<n> <path> textured=<t> missing=<m> tris=<k>
// bracketed by "texgallery: begin <n>" and "texgallery: done".

#include <string>
#include <vector>

#include "ptv/math/Math.h"
#include "ptv/render/Renderer.h"

namespace ptv {

class World;

/** Bounds of a rigged mesh as its palette draws it. A rig's raw vertices can
 *  be in a different scale or place than the skinned result (the default
 *  hands' raw vertices span 2 mm, their bind pose 30 cm), so framing must use
 *  the skinned positions. False for an empty mesh. */
bool skinned_bounds(const std::vector<gltf_mesh_load::SkinnedVertex>& verts,
                    const std::vector<Mat4>& palette, Vec3& centre, float& radius);

class TextureGallery {
public:
    static constexpr float kSettleS = 6.0f;
    static constexpr float kSlotS = 4.0f;

    /** Collect every mesh asset in ``world`` (one entry per LOD0 path). */
    void start(World& world, Renderer& renderer);
    void update(float dt);
    /** Replace the frame's draw list with the current asset. */
    void build_items(std::vector<Renderer::DrawItem>& items, Renderer& renderer,
                     const Vec3& head_pos, const Vec3& head_forward_xz) const;
    bool started() const { return started_; }
    bool done() const { return done_; }

private:
    struct Entry {
        std::string path;
        int slot{-1};
        bool skinned{false};
        std::vector<Mat4> palette;
        Renderer::SlotInfo info{};
    };
    std::vector<Entry> entries_;
    float t_{0.0f};
    int shown_{-2};
    bool started_{false};
    bool done_{false};
};

}  // namespace ptv
