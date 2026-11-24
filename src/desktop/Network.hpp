#pragma once

#include "editor/NetworkBackend.hpp"

namespace openstone::desktop {

[[nodiscard]] std::shared_ptr<NetworkConnection> openNetwork(const NetworkSettings& settings);

}
