#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openstone {

inline constexpr std::size_t maxLegacyPresetBytes = 16 * 1024 * 1024;

struct LegacyPreset {
    struct Program {
        std::string name;
        std::vector<float> values;
    };

    std::string id;
    int version = 0;
    bool single = true;
    int current = 0;
    std::vector<Program> programs;
    std::optional<std::string> namedTable;
};

[[nodiscard]] LegacyPreset readLegacyPreset(std::string_view bytes);
[[nodiscard]] std::string writeLegacyPreset(const LegacyPreset& preset);

}
