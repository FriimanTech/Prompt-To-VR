#if defined(__ANDROID__)

#include "AndroidAssetSource.h"

#include <android/asset_manager.h>

#include <algorithm>

namespace ptv {

std::string AndroidAssetSource::to_asset_key(const std::string& path) const {
    // Map an engine "logical" path onto the key layout inside the APK's assets/
    // root (the directory AAssetManager treats as its root).
    //
    // The quest_build stage (stage_quest_build._package_assets) packages a
    // project's files like this, relative to the assets root:
    //   scene.json                         <- app/src/main/assets/scene.json
    //   assets/<asset_id>/<uuid>_pbr.glb   <- from projects/<id>/assets/<...>
    //   shaders/<name>.spv                 <- compiled engine shaders
    //
    // The compiled scene.json refers to meshes as
    //   projects/<project_id>/assets/<asset_id>/<uuid>_pbr.glb
    // so we strip the leading "projects/<project_id>/" segment, leaving
    // "assets/<asset_id>/<uuid>_pbr.glb" -- which matches the packaged layout.
    // "scene.json" and "shaders/*.spv" carry no such prefix and pass through.
    std::string p = path;
    std::replace(p.begin(), p.end(), '\\', '/');
    while (p.rfind("./", 0) == 0) p = p.substr(2);
    while (!p.empty() && p.front() == '/') p = p.substr(1);
    const std::string projects = "projects/";
    if (p.rfind(projects, 0) == 0) {
        const auto slash = p.find('/', projects.size());
        if (slash != std::string::npos) {
            p = p.substr(slash + 1);
        }
    }
    return p;
}

std::optional<std::vector<std::uint8_t>> AndroidAssetSource::read_bytes(
    const std::string& path) const {
    if (!mgr_) return std::nullopt;
    const std::string key = to_asset_key(path);
    AAsset* asset = AAssetManager_open(mgr_, key.c_str(), AASSET_MODE_BUFFER);
    if (!asset) return std::nullopt;
    const off_t len = AAsset_getLength(asset);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(len));
    int read = 0;
    if (len > 0) {
        read = AAsset_read(asset, bytes.data(), static_cast<size_t>(len));
    }
    AAsset_close(asset);
    if (len > 0 && read != static_cast<int>(len)) return std::nullopt;
    return bytes;
}

bool AndroidAssetSource::exists(const std::string& path) const {
    if (!mgr_) return false;
    const std::string key = to_asset_key(path);
    AAsset* asset = AAssetManager_open(mgr_, key.c_str(), AASSET_MODE_UNKNOWN);
    if (!asset) return false;
    AAsset_close(asset);
    return true;
}

}  // namespace ptv

#endif  // __ANDROID__
