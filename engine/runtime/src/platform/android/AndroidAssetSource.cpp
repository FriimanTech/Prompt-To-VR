#if defined(__ANDROID__)

#include "AndroidAssetSource.h"

#include <android/asset_manager.h>

#include <algorithm>

namespace ptv {

std::string AndroidAssetSource::to_asset_key(const std::string& path) const {
    // Normalise separators and strip any leading "./" or absolute prefix so
    // the engine's logical paths resolve against the APK assets/ root.
    std::string p = path;
    std::replace(p.begin(), p.end(), '\\', '/');
    // If the path contains an "assets/" segment, keep everything after it.
    const std::string marker = "assets/";
    const auto pos = p.rfind(marker);
    if (pos != std::string::npos) {
        p = p.substr(pos + marker.size());
    }
    while (p.rfind("./", 0) == 0) p = p.substr(2);
    while (!p.empty() && p.front() == '/') p = p.substr(1);
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
