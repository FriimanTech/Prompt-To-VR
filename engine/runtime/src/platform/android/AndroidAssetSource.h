#pragma once

// Android-only asset source backed by the APK's AAssetManager. Guarded so the
// file is inert if it is ever pulled into a non-Android build.
#if defined(__ANDROID__)

#include "ptv/platform/AssetSource.h"

struct AAssetManager;

namespace ptv {

/**
 * Reads engine assets (scene.json, GLBs, compiled .spv shaders) from the APK
 * via the Android ``AAssetManager`` handed to us by the NativeActivity.
 *
 * Paths are interpreted relative to the APK ``assets/`` root. The engine
 * passes the same logical paths it uses on desktop; the Quest host is
 * responsible for stripping any absolute prefix before it reaches here (see
 * AndroidMain.cpp), so e.g. ``shaders/scene.vert.spv`` and ``scene.json``
 * resolve directly.
 */
class AndroidAssetSource final : public IAssetSource {
public:
    explicit AndroidAssetSource(AAssetManager* mgr) : mgr_(mgr) {}

    std::optional<std::vector<std::uint8_t>> read_bytes(
        const std::string& path) const override;
    bool exists(const std::string& path) const override;

private:
    // Normalise a possibly-prefixed path down to an APK-relative asset key.
    std::string to_asset_key(const std::string& path) const;

    AAssetManager* mgr_{nullptr};
};

}  // namespace ptv

#endif  // __ANDROID__
