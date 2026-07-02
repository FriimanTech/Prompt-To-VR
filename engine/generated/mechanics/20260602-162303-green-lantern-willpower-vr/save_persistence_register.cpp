#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <entt/entt.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <chrono>
#include <algorithm>

#include "save_persistence_shared.h"

namespace ptv {

class SavePersistence : public IMechanic {
public:
    void on_attach(World& w, std::uint32_t e, const nlohmann::json& p) override {
        self_ = e;
        collect_window_s_ = p.value("collect_window_s", 0.15f);
        auto_slot_ = p.value("auto_slot", std::string("auto"));
        offline_ok_ = p.value("offline_ok", true);
        // Advertise readiness
        tags::append(w, self_, std::string("save.ready:{\"collect_window_s\":") + std::to_string(collect_window_s_) + "}");
    }

    void on_update(World& w, std::uint32_t /*entity*/, float dt) override {
        // Section registrations
        for (auto [src, m] : tags::consume_all_with_prefix(w, "save.register:")) {
            if (m.payload.is_object() && m.payload.contains("section")) {
                auto id = m.payload.at("section").get<std::string>();
                registered_.insert(id);
            }
        }

        // Section pushes (providers send their current data)
        for (auto [src, m] : tags::consume_all_with_prefix(w, "save.section.push:")) {
            if (!m.payload.is_object()) continue;
            std::string id = m.payload.value("section", std::string());
            if (id.empty()) continue;
            if (m.payload.contains("data")) {
                sections_[id] = m.payload.at("data");
                registered_.insert(id); // implicit register on first push
            }
        }

        // Incoming save requests
        bool start_auto_save = false;
        // Triggers from other systems
        if (!tags::consume_all_with_prefix(w, "mission.complete:").empty()) start_auto_save = true;
        if (!tags::consume_all_with_prefix(w, "recharge.complete:").empty()) start_auto_save = true;
        for (auto [src, m] : tags::consume_all_with_prefix(w, "save.request:")) {
            std::string slot = m.payload.value("slot", std::string("manual"));
            begin_collect(w, slot);
        }
        if (start_auto_save) begin_collect(w, auto_slot_);

        // Incoming load requests
        for (auto [src, m] : tags::consume_all_with_prefix(w, "load.request:")) {
            std::string slot = m.payload.value("slot", auto_slot_);
            perform_load(w, slot);
        }

        // Advance batch if active
        if (batch_.active) {
            batch_.elapsed_s += dt;
            if (batch_.elapsed_s >= batch_.window_s) finalize_save(w);
        }
    }

    void on_detach(World&, std::uint32_t) override {}

    std::string id() const override { return "generated.save_persistence"; }

private:
    void begin_collect(World& w, const std::string& slot) {
        if (batch_.active) return; // simple: one at a time
        batch_.active = true;
        batch_.slot = slot;
        batch_.window_s = collect_window_s_;
        batch_.elapsed_s = 0.0f;
        // Notify providers to push within window
        nlohmann::json j; j["slot"] = slot; j["window_s"] = collect_window_s_;
        tags::append(w, self_, std::string("save.collect.start:") + j.dump());
    }

    void finalize_save(World& w) {
        // Assemble payload
        nlohmann::json root;
        nlohmann::json meta;
        meta["version"] = 1;
        meta["slot"] = batch_.slot;
        meta["time_ticks"] = static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        root["meta"] = meta;
        nlohmann::json secs = nlohmann::json::object();
        for (const auto& id : registered_) {
            if (auto it = sections_.find(id); it != sections_.end()) {
                secs[id] = it->second;
            } else {
                secs[id] = nullptr; // known section without data in this window
            }
        }
        root["sections"] = std::move(secs);

        // Store (local, in-memory only in this stage)
        bool ok = offline_ok_ ? SaveStore::write(batch_.slot, root) : SaveStore::write(batch_.slot, root);
        (void)ok;
        const auto bytes = static_cast<int>(root.dump().size());
        nlohmann::json done; done["slot"] = batch_.slot; done["bytes"] = bytes;
        tags::append(w, self_, std::string("save.completed:") + done.dump());

        batch_ = PendingBatch{}; // reset
    }

    void perform_load(World& w, const std::string& slot) {
        auto jopt = SaveStore::read(slot);
        if (!jopt.has_value() || !jopt->is_object()) {
            nlohmann::json done; done["slot"] = slot; done["sections"] = 0; done["status"] = "empty";
            tags::append(w, self_, std::string("load.completed:") + done.dump());
            return;
        }
        const auto& j = *jopt;
        if (!j.contains("sections") || !j.at("sections").is_object()) {
            nlohmann::json done; done["slot"] = slot; done["sections"] = 0; done["status"] = "malformed";
            tags::append(w, self_, std::string("load.completed:") + done.dump());
            return;
        }
        const auto& secs = j.at("sections");
        int count = 0;
        for (auto it = secs.begin(); it != secs.end(); ++it) {
            nlohmann::json one; one["section"] = it.key(); one["data"] = it.value();
            tags::append(w, self_, std::string("load.section:") + one.dump());
            ++count;
        }
        nlohmann::json done; done["slot"] = slot; done["sections"] = count; done["status"] = "ok";
        tags::append(w, self_, std::string("load.completed:") + done.dump());
    }

private:
    std::uint32_t self_{0};
    float collect_window_s_{0.15f};
    std::string auto_slot_{"auto"};
    bool offline_ok_{true};

    PendingBatch batch_{};
    std::unordered_set<std::string> registered_;
    std::unordered_map<std::string, nlohmann::json> sections_;
};

} // namespace ptv

PTV_REGISTER_MECHANIC("generated.save_persistence", ptv::SavePersistence)
