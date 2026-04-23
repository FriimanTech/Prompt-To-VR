// Minimal CLI that opens a GLB and prints a short report (without depending
// on the full tinygltf loader); mirrors the Python `stage_asset_normalize`.

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: ptv_glb_validate <file.glb>\n");
        return 2;
    }
    std::ifstream f(argv[1], std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "cannot open: %s\n", argv[1]);
        return 1;
    }
    std::vector<char> data((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
    if (data.size() < 20) {
        std::fprintf(stderr, "file too small\n");
        return 1;
    }
    std::uint32_t magic = *reinterpret_cast<std::uint32_t*>(&data[0]);
    if (magic != 0x46546C67u) {
        std::fprintf(stderr, "bad GLB magic\n");
        return 1;
    }
    std::uint32_t chunk_len = *reinterpret_cast<std::uint32_t*>(&data[12]);
    std::uint32_t chunk_type = *reinterpret_cast<std::uint32_t*>(&data[16]);
    if (chunk_type != 0x4E4F534Au) {
        std::fprintf(stderr, "first chunk is not JSON\n");
        return 1;
    }
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(std::string(data.data() + 20, chunk_len));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "json parse: %s\n", e.what());
        return 1;
    }
    std::printf("GLB ok  meshes=%zu materials=%zu textures=%zu\n",
                doc.value("meshes", nlohmann::json::array()).size(),
                doc.value("materials", nlohmann::json::array()).size(),
                doc.value("textures", nlohmann::json::array()).size());
    return 0;
}
