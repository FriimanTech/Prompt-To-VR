#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <optional>
#include <utility>

namespace ptv {

namespace persistence {

// In-memory local-first storage for this runtime session.
struct Store {
    // slot -> full save payload (object of sections)
    std::unordered_map<std::string, nlohmann::json> slots;

    static Store& instance() {
        static Store s; return s;
    }
};

static nlohmann::json safe_parse(const std::string& s) {
    if (s.empty()) return nlohmann::json{};
    return nlohmann::json::parse(s, nullptr, /*allow_exceptions=*/false);
}

static void tag_report(World& w, std::uint32_t e, const std::string& k, const std::string& v) {
    CTag t;
    if (w.has<CTag>(e)) t = w.get<CTag>(e);
    // Replace or insert key=value pair compactly.
    const std::string kv = k + ":" + v;
    bool found = false;
    for (auto& s : t.tags) {
        if (s.rfind(k + ":", 0) == 0) { s = kv; found = true; break; }
    }
    if (!found) t.tags.push_back(kv);
    w.emplace_or_replace<CTag>(e, std::move(t));
}

class SavePersistence : public IMechanic {
public:
    void on_attach(World& world, std::uint32_t entity, const nlohmann::json& params) override {
        current_slot_ = params.value("default_slot", std::string("auto"));
        emit_tags_ = params.value("emit_summary_tag", true);
        // Optionally autoload on attach.
        if (params.value("autoload_on_attach", false)) {
            const std::string slot = params.value("autoload_slot", current_slot_);
            if (auto loaded = load_from_slot(slot)) {
                sections_ = std::move(*loaded);
                if (emit_tags_) tag_report(world, entity, "load", slot + "=ok");
            } else if (emit_tags_) {
                tag_report(world, entity, "load", slot + "=miss");
            }
        }
    }

    void on_update(World& world, std::uint32_t entity, float /*dt*/) override {
        // Read our binding entry and treat its params as the command surface.
        if (world.has<CMechanicBinding>(entity)) {
            const auto& mb = world.get<CMechanicBinding>(entity);
            for (const auto& entry : mb.entries) {
                if (entry.id == id()) {
                    const auto p = safe_parse(entry.params_json);
                    if (p.is_object()) process_commands(world, entity, p);
                }
            }
        }
    }

    void on_detach(World&, std::uint32_t) override {
        // No-op.
    }

    std::string id() const override { return "generated.save_persistence"; }

private:
    // Section registry held as JSON object of id -> value.
    nlohmann::json sections_ = nlohmann::json::object();
    std::string current_slot_ = "auto";
    bool emit_tags_ = true;

    static bool save_to_slot(const std::string& slot, const nlohmann::json& payload) {
        auto& s = persistence::Store::instance();
        s.slots[slot] = payload;
        return true;
    }

    static std::optional<nlohmann::json> load_from_slot(const std::string& slot) {
        auto& s = persistence::Store::instance();
        auto it = s.slots.find(slot);
        if (it == s.slots.end()) return std::nullopt;
        return it->second;
    }

    void process_commands(World& world, std::uint32_t entity, const nlohmann::json& p) {
        // Section updates
        if (p.contains("set_section") && p.at("set_section").is_object()) {
            const auto& one = p.at("set_section");
            if (one.contains("id") && one.contains("data") && one.at("id").is_string()) {
                const std::string id = one.at("id").get<std::string>();
                sections_[id] = one.at("data");
                if (emit_tags_) tag_report(world, entity, "section", id + "=set");
            }
        }
        if (p.contains("set_sections") && p.at("set_sections").is_object()) {
            for (auto it = p.at("set_sections").begin(); it != p.at("set_sections").end(); ++it) {
                sections_[it.key()] = it.value();
            }
            if (emit_tags_) tag_report(world, entity, "sections", "bulk_set");
        }

        // Slot management
        if (p.contains("slot") && p.at("slot").is_string()) {
            current_slot_ = p.at("slot").get<std::string>();
        }

        // Triggers
        const bool mission_done = p.value("mission_complete", false);
        const bool recharge_done = p.value("recharge_complete", false);
        const bool manual_save = p.value("manual_save", false);
        const bool do_save = p.value("save", false) || mission_done || recharge_done || manual_save;
        const bool do_load = p.value("load", false);

        // Optional explicit save/load objects with slot override
        if (p.contains("save") && p.at("save").is_object()) {
            const auto& so = p.at("save");
            if (so.contains("slot") && so.at("slot").is_string()) current_slot_ = so.at("slot").get<std::string>();
        }
        if (p.contains("load") && p.at("load").is_object()) {
            const auto& lo = p.at("load");
            if (lo.contains("slot") && lo.at("slot").is_string()) current_slot_ = lo.at("slot").get<std::string>();
        }

        // Execute save
        if (do_save) {
            const std::string slot = current_slot_;
            const bool ok = save_to_slot(slot, sections_);
            if (emit_tags_) tag_report(world, entity, "save", slot + (ok ? "=ok" : "=fail"));
        }
        // Execute load
        if (do_load) {
            const std::string slot = current_slot_;
            if (auto loaded = load_from_slot(slot)) {
                sections_ = std::move(*loaded);
                if (emit_tags_) tag_report(world, entity, "load", slot + "=ok");
            } else if (emit_tags_) {
                tag_report(world, entity, "load", slot + "=miss");
            }
        }

        // Optional clear request: wipe in-memory sections (not slots)
        if (p.value("clear_sections", false)) {
            sections_ = nlohmann::json::object();
            if (emit_tags_) tag_report(world, entity, "sections", "cleared");
        }

        // Expose a compact hash-ish count for tests via tag
        if (p.value("emit_state_digest", false)) {
            const std::size_t count = sections_.is_object() ? sections_.size() : 0;
            if (emit_tags_) tag_report(world, entity, "digest", std::to_string(count));
        }
    }
};

} // namespace persistence

using persistence::SavePersistence;

} // namespace ptv

PTV_REGISTER_MECHANIC("generated.save_persistence", ptv::persistence::SavePersistence)
