#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <utility>
#include <cmath>
#include <cstdint>

namespace ptv {

namespace {
struct Persist {
    int energy = 0;
    int max_energy = 100;
    std::string last_outcome = "idle";
};
static std::unordered_map<std::string, Persist> g_persist;

static std::string to_lower_alnum_space(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        char c = ch;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ')
            out.push_back(c);
        else if (c == '\t' || c == '\n' || c == '\r')
            out.push_back(' ');
    }
    // collapse multiple spaces
    std::string out2;
    out2.reserve(out.size());
    bool prev_sp = false;
    for (char c : out) {
        bool sp = (c == ' ');
        if (sp && prev_sp) continue;
        out2.push_back(sp ? ' ' : c);
        prev_sp = sp;
    }
    if (!out2.empty() && out2.back() == ' ') out2.pop_back();
    return out2;
}

static float levenshtein_similarity(const std::string& a_in, const std::string& b_in) {
    std::string a = to_lower_alnum_space(a_in);
    std::string b = to_lower_alnum_space(b_in);
    const size_t n = a.size();
    const size_t m = b.size();
    if (n == 0 && m == 0) return 1.0f;
    if (n == 0 || m == 0) return 0.0f;
    std::vector<int> prev(m + 1), curr(m + 1);
    for (size_t j = 0; j <= m; ++j) prev[j] = static_cast<int>(j);
    for (size_t i = 1; i <= n; ++i) {
        curr[0] = static_cast<int>(i);
        for (size_t j = 1; j <= m; ++j) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            curr[j] = std::min({ prev[j] + 1, curr[j - 1] + 1, prev[j - 1] + cost });
        }
        std::swap(prev, curr);
    }
    int dist = prev[m];
    float denom = static_cast<float>(std::max(n, m));
    float sim = 1.0f - (static_cast<float>(dist) / denom);
    if (sim < 0.0f) sim = 0.0f;
    return sim;
}

static void set_tag_kv(World& w, std::uint32_t e, const std::string& key, const std::string& value) {
    if (!w.has<CTag>(e)) {
        w.emplace_or_replace<CTag>(e, CTag{});
    }
    auto& tags = w.get<CTag>(e).tags;
    const std::string prefix = key + "=";
    bool replaced = false;
    for (auto& t : tags) {
        if (t.rfind(prefix, 0) == 0) {
            t = prefix + value;
            replaced = true;
            break;
        }
    }
    if (!replaced) tags.push_back(prefix + value);
}

} // anonymous

class OathRechargeMechanic : public IMechanic {
public:
    void on_attach(World&, std::uint32_t, const nlohmann::json& params) override {
        cfg_ = params;
        stage_ = Stage::Idle;
        cooldown_t_ = 0.0f;
        prev_interact_ = false;
        persist_key_ = cfg_.value("persist_key", std::string("ring_energy_global"));
        auto it = g_persist.find(persist_key_);
        int maxE = cfg_.value("max_energy", 100);
        if (it == g_persist.end()) {
            Persist p;
            p.max_energy = maxE;
            int init_pct = cfg_.value("initial_energy_percent", cfg_.value("refill_to_percent", 100));
            if (init_pct < 0) init_pct = 0;
            if (init_pct > 100) init_pct = 100;
            p.energy = (p.max_energy * init_pct) / 100;
            g_persist.emplace(persist_key_, p);
        } else {
            // ensure max is at least config
            it->second.max_energy = std::max(it->second.max_energy, maxE);
        }
        last_prompt_ = "start";
        recognized_text_.clear();
    }

    void on_update(World& w, std::uint32_t e, float dt) override {
        if (dt <= 0.0f) dt = std::max(0.0f, w.delta_time());
        const auto bind = binding_params(w, e);
        auto getb = [&](const char* k, bool defv) { return bind.contains(k) ? bind.value(k, defv) : cfg_.value(k, defv); };
        auto geti = [&](const char* k, int defv) { return bind.contains(k) ? bind.value(k, defv) : cfg_.value(k, defv); };
        auto getf = [&](const char* k, float defv) { return bind.contains(k) ? bind.value(k, defv) : cfg_.value(k, defv); };
        auto gets = [&](const char* k, const std::string& defv) { return bind.contains(k) ? bind.value(k, defv) : cfg_.value(k, defv); };

        const bool in_range = bind.value("socket_in_range", cfg_.value("socket_in_range", false));
        const bool interact_pressed = bind.value("interact_pressed", cfg_.value("interact_pressed", false));
        const bool interact_rising = (interact_pressed && !prev_interact_);
        prev_interact_ = interact_pressed;

        const float mic_max_sec = getf("mic_max_record_sec", 6.0f);
        const float cooldown_sec = getf("cooldown_sec", 5.0f);
        const float cine_sec = getf("cinematic_sec", 1.2f);
        const int refill_to_percent = std::clamp(geti("refill_to_percent", 100), 0, 100);
        const std::string oath_text = gets("oath_text", std::string{});
        const float threshold = std::clamp(getf("match_threshold", 0.82f), 0.0f, 1.0f);
        const int stt_timeout_ms = std::max(1, geti("stt_timeout_ms", 2000));
        const bool offline_ok = bind.value("offline_ok", cfg_.value("allow_offline_override", false));

        // Update readouts on tags for observability
        {
            auto pit = g_persist.find(persist_key_);
            int energy_pct = 0;
            int maxE = 100;
            if (pit != g_persist.end() && pit->second.max_energy > 0) {
                energy_pct = static_cast<int>(std::round(100.0f * (static_cast<float>(pit->second.energy) / pit->second.max_energy)));
                maxE = pit->second.max_energy;
            }
            set_tag_kv(w, e, "ring_energy", std::to_string(energy_pct));
            set_tag_kv(w, e, "ring_energy_max", std::to_string(maxE));
            set_tag_kv(w, e, "oath_outcome", pit != g_persist.end() ? pit->second.last_outcome : "idle");
        }

        switch (stage_) {
            case Stage::Idle: {
                if (cooldown_t_ > 0.0f) {
                    stage_ = Stage::Cooldown;
                    break;
                }
                if (in_range && interact_rising) {
                    begin_listening();
                    last_prompt_ = gets("ui_prompt_ids.listening", std::string("listening"));
                }
                break;
            }
            case Stage::Listening: {
                listen_t_ += dt;
                // Cancel path
                if (bind.value("cancel", false)) {
                    start_cooldown(cooldown_sec, w, e, "canceled");
                    break;
                }
                // If recognized text arrives early, progress
                if (recognized_text_.empty()) {
                    if (bind.contains("recognized_text")) {
                        recognized_text_ = bind.value("recognized_text", std::string{});
                    } else if (bind.contains("speech_text")) {
                        recognized_text_ = bind.value("speech_text", std::string{});
                    }
                }
                if (!recognized_text_.empty() || listen_t_ >= mic_max_sec) {
                    stage_ = Stage::Validating;
                    validate_t_ = 0.0f;
                    last_prompt_ = gets("ui_prompt_ids.validating", std::string("validating"));
                    // If offline override, force recognized as oath now
                    if (offline_ok) recognized_text_ = oath_text;
                }
                break;
            }
            case Stage::Validating: {
                validate_t_ += dt;
                // Allow late arrival of text within timeout
                if (recognized_text_.empty()) {
                    if (bind.contains("recognized_text")) {
                        recognized_text_ = bind.value("recognized_text", std::string{});
                    } else if (bind.contains("speech_text")) {
                        recognized_text_ = bind.value("speech_text", std::string{});
                    }
                }
                bool decided = false;
                bool passed = false;
                if (!recognized_text_.empty()) {
                    float sim = levenshtein_similarity(recognized_text_, oath_text);
                    passed = (sim >= threshold);
                    decided = true;
                } else if (static_cast<int>(validate_t_ * 1000.0f) >= stt_timeout_ms) {
                    passed = false;
                    decided = true;
                }
                if (decided) {
                    if (passed) {
                        // Success: refill immediately and play short cinematic window
                        do_refill(refill_to_percent);
                        last_prompt_ = gets("ui_prompt_ids.success", std::string("success"));
                        set_outcome("success");
                        stage_ = Stage::SuccessCine;
                        cine_t_ = 0.0f;
                    } else {
                        last_prompt_ = gets("ui_prompt_ids.fail", std::string("fail"));
                        start_cooldown(cooldown_sec, w, e, "fail");
                    }
                }
                break;
            }
            case Stage::SuccessCine: {
                cine_t_ += dt;
                if (cine_t_ >= cine_sec) {
                    stage_ = Stage::Idle;
                    recognized_text_.clear();
                    last_prompt_ = gets("ui_prompt_ids.start", std::string("start"));
                }
                break;
            }
            case Stage::Cooldown: {
                cooldown_t_ -= dt;
                if (cooldown_t_ <= 0.0f) {
                    cooldown_t_ = 0.0f;
                    stage_ = Stage::Idle;
                    recognized_text_.clear();
                    last_prompt_ = gets("ui_prompt_ids.start", std::string("start"));
                }
                break;
            }
        }

        // Expose prompt and timers for debugging/QA through tags
        set_tag_kv(w, e, "oath_prompt", last_prompt_);
        if (stage_ == Stage::Cooldown) {
            int ms = static_cast<int>(std::round(cooldown_t_ * 1000.0f));
            set_tag_kv(w, e, "cooldown_ms", std::to_string(std::max(0, ms)));
        } else {
            set_tag_kv(w, e, "cooldown_ms", "0");
        }
        set_tag_kv(w, e, "oath_stage", stage_name());
    }

    void on_detach(World&, std::uint32_t) override {}

    std::string id() const override { return "generated.oath_recharge_mechanic"; }

private:
    enum class Stage { Idle, Listening, Validating, SuccessCine, Cooldown };

    nlohmann::json cfg_;
    Stage stage_{Stage::Idle};
    float cooldown_t_{0.0f};
    float listen_t_{0.0f};
    float validate_t_{0.0f};
    float cine_t_{0.0f};
    bool prev_interact_{false};
    std::string persist_key_;
    std::string last_prompt_;
    std::string recognized_text_;

    nlohmann::json binding_params(World& w, std::uint32_t e) const {
        if (!w.has<CMechanicBinding>(e)) return nlohmann::json::object();
        const auto& mb = w.get<CMechanicBinding>(e);
        for (const auto& entry : mb.entries) {
            if (entry.id == id()) {
                // params_json is a serialized string; parse lazily.
                auto j = nlohmann::json::parse(
                    entry.params_json, nullptr, /*allow_exceptions=*/false);
                return j.is_object() ? j : nlohmann::json::object();
            }
        }
        return nlohmann::json::object();
    }

    void begin_listening() {
        stage_ = Stage::Listening;
        listen_t_ = 0.0f;
        validate_t_ = 0.0f;
        cine_t_ = 0.0f;
        recognized_text_.clear();
    }

    void start_cooldown(float sec, World& w, std::uint32_t e, const std::string& outcome) {
        cooldown_t_ = std::max(0.0f, sec);
        stage_ = Stage::Cooldown;
        set_outcome(outcome);
        set_tag_kv(w, e, "haptic", "fail"); // stub marker, no real haptics API
    }

    void do_refill(int target_percent) {
        auto it = g_persist.find(persist_key_);
        if (it == g_persist.end()) return;
        const int tgt = std::clamp(target_percent, 0, 100);
        it->second.energy = (it->second.max_energy * tgt) / 100;
        set_outcome("success");
    }

    void set_outcome(const std::string& outcome) {
        auto it = g_persist.find(persist_key_);
        if (it != g_persist.end()) it->second.last_outcome = outcome;
    }

    const char* stage_name() const {
        switch (stage_) {
            case Stage::Idle: return "idle";
            case Stage::Listening: return "listening";
            case Stage::Validating: return "validating";
            case Stage::SuccessCine: return "success_cinematic";
            case Stage::Cooldown: return "cooldown";
        }
        return "unknown";
    }
};

} // namespace ptv

PTV_REGISTER_MECHANIC("generated.oath_recharge_mechanic", ptv::OathRechargeMechanic)