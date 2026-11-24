#pragma once

#include "editor/MediaBackend.hpp"

namespace openstone::desktop {

[[nodiscard]] std::shared_ptr<MediaPlayback> openMedia(const MediaSettings& settings);

}
