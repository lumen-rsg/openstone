#pragma once

#include "stream/Runtime.hpp"

namespace openstone::stream {

// The processor receives the number of Green Reset triggers delivered to the node.
[[nodiscard]] std::unique_ptr<SampleProcessor> makeStreamCounter(SamplePrecision precision);

}
