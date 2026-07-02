#include "ptv/api/IBehaviour.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>

#include "boss_internal.h"

namespace ptv {

// Registration id must match id()

} // namespace ptv

PTV_REGISTER_BEHAVIOUR("generated.boss_ai_fear_behemoth", ptv::FearBehemothBossBehaviour)
