#include "ptv/platform/AssetSource.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>

namespace ptv {

std::optional<std::vector<std::uint8_t>> FilesystemAssetSource::read_bytes(
    const std::string& path) const {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return std::nullopt;
    const std::streamsize size = in.tellg();
    if (size < 0) return std::nullopt;
    in.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (size > 0 && !in.read(reinterpret_cast<char*>(bytes.data()), size)) {
        return std::nullopt;
    }
    return bytes;
}

bool FilesystemAssetSource::exists(const std::string& path) const {
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(path), ec);
}

namespace {
std::mutex g_asset_mutex;
std::unique_ptr<IAssetSource> g_asset_source;
}  // namespace

void set_asset_source(std::unique_ptr<IAssetSource> source) {
    std::lock_guard<std::mutex> lock(g_asset_mutex);
    g_asset_source = std::move(source);
}

const IAssetSource& asset_source() {
    std::lock_guard<std::mutex> lock(g_asset_mutex);
    if (!g_asset_source) {
        g_asset_source = std::make_unique<FilesystemAssetSource>();
    }
    return *g_asset_source;
}

}  // namespace ptv
