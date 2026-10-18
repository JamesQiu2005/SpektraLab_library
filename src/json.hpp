// json.hpp -- the one JSON type the SDK speaks (nlohmann, vendored, MIT).
// Ordered, so a reply lists its fields in the order the code wrote them.

#pragma once

#include "nlohmann/json.hpp"

namespace spklib {
using Json = nlohmann::ordered_json;
}
