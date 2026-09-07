#include "ptv/api/BodySlots.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <unordered_map>

namespace ptv {
namespace {

// Namespace prefixes rigs prepend to every joint. Stripped so the rest of
// the name can be compared against the table.
constexpr std::array<const char*, 3> kStrippedPrefixes = {
    "mixamorig:",
    "mixamorig",  // some exporters drop the colon
    "armature|",
};

struct SlotTable {
    std::unordered_map<std::string, std::vector<std::string>> candidates;
    std::vector<std::string> names;
};

void add(SlotTable& t, const std::string& slot,
         std::vector<std::string> raw_candidates) {
    std::vector<std::string> normalized;
    normalized.reserve(raw_candidates.size());
    for (const auto& c : raw_candidates) {
        std::string n = normalize_joint_name(c);
        if (n.empty()) continue;
        // Keep first-wins ordering but drop duplicates: several conventions
        // collapse to the same normalized string (``hand_l`` and ``Hand_L``),
        // and resolving the same name twice is wasted lookups.
        if (std::find(normalized.begin(), normalized.end(), n) ==
            normalized.end()) {
            normalized.push_back(std::move(n));
        }
    }
    t.candidates[slot] = std::move(normalized);
    t.names.push_back(slot);
}

/** Sided helper: expands one entry into ``<slot>_l`` and ``<slot>_r``.
 *
 *  ``pattern`` entries use ``{L}`` for the long side word (``Left`` /
 *  ``Right``) and ``{s}`` for the short suffix (``l`` / ``r``), which is
 *  what the two dominant conventions use respectively. */
void add_sided(SlotTable& t, const std::string& slot,
               const std::vector<std::string>& patterns) {
    static const std::array<std::pair<const char*, const char*>, 2> kSides = {
        std::pair<const char*, const char*>{"left", "l"},
        std::pair<const char*, const char*>{"right", "r"},
    };
    for (const auto& [long_word, short_word] : kSides) {
        std::vector<std::string> expanded;
        expanded.reserve(patterns.size());
        for (const auto& p : patterns) {
            std::string out;
            out.reserve(p.size() + 8);
            for (size_t i = 0; i < p.size(); ++i) {
                if (p.compare(i, 3, "{L}") == 0) {
                    out += long_word;
                    i += 2;
                } else if (p.compare(i, 3, "{s}") == 0) {
                    out += short_word;
                    i += 2;
                } else {
                    out += p[i];
                }
            }
            expanded.push_back(std::move(out));
        }
        add(t, slot + "_" + short_word, std::move(expanded));
    }
}

const SlotTable& table() {
    static const SlotTable t = [] {
        SlotTable s;

        // --- Torso, head and their landmarks ---------------------------
        // ``root`` and ``hips`` are separated because Mixamo rigs usually
        // have no root bone at the origin, so a caller that wants "the whole
        // body" should ask for hips and a caller that wants an export root
        // should ask for root and accept a miss.
        add(s, "root", {"root", "armature", "rootbone", "reference"});
        add(s, "hips", {"Hips", "pelvis", "hip", "Bip01_Pelvis", "root"});
        // Waist is the same joint as hips on every rig this pipeline sees;
        // it exists as a separate slot because "belt", "tool belt" and
        // "holster" read as waist, not as hips.
        add(s, "waist", {"Hips", "pelvis", "spine_01", "Spine", "hip"});
        add(s, "spine", {"Spine", "spine_01", "spine1", "Spine1", "abdomen"});
        add(s, "chest", {"Spine2", "chest", "spine_03", "UpperChest", "Spine1",
                         "upperchest", "ribcage"});
        // A backpack mounts on the upper back, which is the chest joint.
        add(s, "back", {"Spine2", "chest", "spine_03", "UpperChest", "Spine1"});
        add(s, "neck", {"Neck", "neck_01", "neck1", "Neck1"});
        add(s, "head", {"Head", "head_01", "headtop", "Bip01_Head"});
        // Face, nose and ears have no bones on any rig the pipeline produces.
        // They resolve to the head so that "glasses sit on the nose" still
        // lands somewhere sensible instead of failing outright; the offset
        // that puts them in the right place comes from the caller.
        add(s, "face", {"Head", "head_01", "face"});
        add(s, "nose", {"nose", "Head", "head_01"});

        // --- Sided landmarks on the head --------------------------------
        add_sided(s, "eye", {"{L}Eye", "eye_{s}", "{L}_eye", "Head"});
        add_sided(s, "ear", {"{L}Ear", "ear_{s}", "{L}_ear", "Head"});

        // --- Arms -------------------------------------------------------
        add_sided(s, "shoulder",
                  {"{L}Shoulder", "clavicle_{s}", "shoulder_{s}",
                   "{L}Clavicle"});
        add_sided(s, "upper_arm",
                  {"{L}Arm", "upperarm_{s}", "{L}UpperArm", "upper_arm_{s}",
                   "arm_{s}", "{L}_arm"});
        // Mixamo has no elbow bone: the elbow is where the forearm starts, so
        // both slots deliberately resolve to the same joint.
        const std::vector<std::string> kForearm = {
            "{L}ForeArm", "lowerarm_{s}", "{L}LowerArm", "forearm_{s}",
            "lower_arm_{s}", "elbow_{s}", "{L}_forearm"};
        add_sided(s, "elbow", kForearm);
        add_sided(s, "forearm", kForearm);
        const std::vector<std::string> kHand = {
            "{L}Hand", "hand_{s}", "{L}_hand", "wrist_{s}", "{L}Wrist"};
        add_sided(s, "wrist", kHand);
        add_sided(s, "hand", kHand);

        // --- Legs -------------------------------------------------------
        add_sided(s, "thigh",
                  {"{L}UpLeg", "thigh_{s}", "{L}UpperLeg", "upleg_{s}",
                   "upper_leg_{s}", "{L}_thigh"});
        // Same collapse as the elbow: the knee is the origin of the shin.
        const std::vector<std::string> kShin = {
            "{L}Leg", "calf_{s}", "{L}LowerLeg", "shin_{s}", "lower_leg_{s}",
            "leg_{s}", "knee_{s}", "{L}_shin"};
        add_sided(s, "knee", kShin);
        add_sided(s, "shin", kShin);
        const std::vector<std::string> kFoot = {
            "{L}Foot", "foot_{s}", "{L}_foot", "ankle_{s}", "{L}Ankle"};
        add_sided(s, "ankle", kFoot);
        add_sided(s, "foot", kFoot);
        add_sided(s, "toes",
                  {"{L}ToeBase", "ball_{s}", "toe_{s}", "toes_{s}",
                   "{L}Toes", "{L}_toe"});

        // --- Fingers ----------------------------------------------------
        // Sided names first so a full-body rig picks the correct hand; the
        // unsided forms at the end are what make the engine's own per-hand
        // fixture (``mixamorig:index_01``) resolve through the same slots.
        static const std::array<const char*, 5> kFingers = {
            "thumb", "index", "middle", "ring", "pinky",
        };
        for (const char* finger : kFingers) {
            const std::string f = finger;
            std::string cap = f;
            cap[0] = static_cast<char>(std::toupper(cap[0]));
            for (int seg = 1; seg <= 3; ++seg) {
                const std::string n = std::to_string(seg);
                const std::string nn = "0" + n;
                add_sided(s, f + "_" + n,
                          {"{L}Hand" + cap + n,       // LeftHandIndex1
                           f + "_" + nn + "_{s}",     // index_01_l
                           f + "_" + n + "_{s}",      // index_1_l
                           "f_" + f + "." + nn + ".{s}",  // f_index.01.L
                           f + "_" + nn,              // index_01 (hand GLB)
                           f + "_" + n});
            }
            // Mixamo ships a fourth tip bone; other rigs stop at 3, so the
            // distal segment is the last fallback and a tip request still
            // lands on real geometry.
            add_sided(s, f + "_tip",
                      {"{L}Hand" + cap + "4", f + "_04_{s}", f + "_4_{s}",
                       f + "_tip_{s}", f + "_04", f + "_4",
                       "{L}Hand" + cap + "3", f + "_03_{s}", f + "_03"});
        }

        std::sort(s.names.begin(), s.names.end());
        return s;
    }();
    return t;
}

}  // namespace

std::string normalize_joint_name(const std::string& name) {
    std::string lowered;
    lowered.reserve(name.size());
    for (const char c : name) {
        lowered += static_cast<char>(
            std::tolower(static_cast<unsigned char>(c)));
    }

    for (const char* prefix : kStrippedPrefixes) {
        const std::string p = prefix;
        if (lowered.size() > p.size() && lowered.compare(0, p.size(), p) == 0) {
            lowered.erase(0, p.size());
            break;
        }
    }

    std::string out;
    out.reserve(lowered.size());
    for (const char c : lowered) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0) out += c;
    }
    return out;
}

const std::vector<std::string>& body_slot_names() { return table().names; }

bool is_body_slot(const std::string& slot) {
    return table().candidates.find(slot) != table().candidates.end();
}

const std::vector<std::string>& body_slot_candidates(const std::string& slot) {
    static const std::vector<std::string> kEmpty;
    const auto& t = table();
    const auto it = t.candidates.find(slot);
    return (it == t.candidates.end()) ? kEmpty : it->second;
}

std::string body_finger_slot(const std::string& finger, int segment,
                             int hand) {
    return finger + "_" + std::to_string(segment) + body_side_suffix(hand);
}

const char* body_side_suffix(int hand) { return hand == 0 ? "_l" : "_r"; }

}  // namespace ptv
