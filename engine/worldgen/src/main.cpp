// ptv_worldgen -- offline host that runs a generated IWorldBuilder and writes
// world.scene.json. Links no engine core; safe to run generated world code.
//
// Usage:
//   ptv_worldgen --out <world.scene.json> [--builder <id>] [--seed <n>]
//                [--assets <asset_map.json>] [--list]

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "WorldGen.h"

namespace {

std::string arg_value(int argc, char** argv, const std::string& flag,
                      const std::string& fallback = "") {
    for (int i = 1; i < argc - 1; ++i) {
        if (flag == argv[i]) return argv[i + 1];
    }
    return fallback;
}

bool has_flag(int argc, char** argv, const std::string& flag) {
    for (int i = 1; i < argc; ++i) {
        if (flag == argv[i]) return true;
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace ptv::world;

    if (has_flag(argc, argv, "--list")) {
        for (const auto& id : registered_builder_ids(/*include_fallback=*/true)) {
            std::cout << id << "\n";
        }
        return 0;
    }

    const std::string out_path = arg_value(argc, argv, "--out");
    if (out_path.empty()) {
        std::cerr << "ptv_worldgen: --out <path> is required\n";
        return 1;
    }

    std::uint64_t seed = 1;
    try {
        const std::string s = arg_value(argc, argv, "--seed", "1");
        seed = static_cast<std::uint64_t>(std::stoull(s));
    } catch (...) {
        seed = 1;
    }

    // Optional asset_id -> glb_path map for place_object().
    nlohmann::json asset_map = nlohmann::json::object();
    const std::string assets_path = arg_value(argc, argv, "--assets");
    if (!assets_path.empty()) {
        std::ifstream in(assets_path);
        if (in) {
            try {
                in >> asset_map;
            } catch (const std::exception& e) {
                std::cerr << "ptv_worldgen: warning: bad --assets json: "
                          << e.what() << "\n";
                asset_map = nlohmann::json::object();
            }
        }
    }

    // Choose the builder: explicit id, else first generated builder, else the
    // deterministic fallback.
    std::string builder_id = arg_value(argc, argv, "--builder");
    if (builder_id.empty()) {
        auto ids = registered_builder_ids(/*include_fallback=*/false);
        builder_id = ids.empty() ? std::string(kFallbackBuilderId) : ids.front();
    }

    auto builder = make_builder(builder_id);
    if (!builder) {
        std::cerr << "ptv_worldgen: no builder registered with id '"
                  << builder_id << "'\n";
        return 2;
    }

    nlohmann::json doc;
    try {
        doc = build_scene_json(*builder, seed, asset_map);
    } catch (const std::exception& e) {
        std::cerr << "ptv_worldgen: builder '" << builder_id
                  << "' threw: " << e.what() << "\n";
        return 3;
    }

    std::error_code ec;
    const std::filesystem::path out_fs(out_path);
    if (out_fs.has_parent_path()) {
        std::filesystem::create_directories(out_fs.parent_path(), ec);
    }
    std::ofstream out(out_fs);
    if (!out) {
        std::cerr << "ptv_worldgen: cannot open output '" << out_path << "'\n";
        return 4;
    }
    out << doc.dump(2) << "\n";

    const std::size_t n_entities =
        doc.contains("entities") ? doc["entities"].size() : 0;
    std::cerr << "ptv_worldgen: builder='" << builder_id << "' seed=" << seed
              << " entities=" << n_entities << " -> " << out_path << "\n";
    return 0;
}
