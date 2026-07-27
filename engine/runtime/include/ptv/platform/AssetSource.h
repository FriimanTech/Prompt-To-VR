#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ptv {

/**
 * Abstraction over "where do engine assets come from".
 *
 * On desktop (PC VR) assets live on the filesystem and paths are ordinary
 * absolute/relative filesystem paths -- ``FilesystemAssetSource`` reproduces
 * the engine's historical behaviour byte-for-byte.
 *
 * On Quest (standalone Android) the ``scene.json``, GLBs, and compiled
 * ``.spv`` shaders are packaged INSIDE the APK and must be read through the
 * Android ``AAssetManager`` (there is no accessible filesystem path).
 * ``AndroidAssetSource`` (see ``platform/android/``) implements this against
 * the asset manager handed to us by the ``NativeActivity``.
 *
 * The engine resolves every asset read through the process-global source set
 * by ``set_asset_source``; if none is set it falls back to a filesystem
 * source, so existing desktop code paths keep working unchanged.
 */
class IAssetSource {
public:
    virtual ~IAssetSource() = default;

    /** Read the whole asset at ``path`` into memory. Returns ``nullopt`` if
     *  the asset does not exist or cannot be read. */
    virtual std::optional<std::vector<std::uint8_t>> read_bytes(
        const std::string& path) const = 0;

    /** True when ``path`` resolves to a readable asset. */
    virtual bool exists(const std::string& path) const = 0;

    /** Convenience: read an asset as UTF-8 text. */
    std::optional<std::string> read_text(const std::string& path) const {
        auto bytes = read_bytes(path);
        if (!bytes) return std::nullopt;
        return std::string(bytes->begin(), bytes->end());
    }
};

/** Default source: ordinary filesystem I/O (absolute or CWD-relative paths).
 *  This is what desktop / PC VR uses. */
class FilesystemAssetSource final : public IAssetSource {
public:
    std::optional<std::vector<std::uint8_t>> read_bytes(
        const std::string& path) const override;
    bool exists(const std::string& path) const override;
};

/** Install the process-global asset source (takes ownership). Passing
 *  ``nullptr`` restores the default filesystem source on the next access. */
void set_asset_source(std::unique_ptr<IAssetSource> source);

/** The process-global asset source. Never null: lazily initialises a
 *  ``FilesystemAssetSource`` when nothing has been installed. */
const IAssetSource& asset_source();

}  // namespace ptv
