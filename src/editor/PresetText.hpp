#pragma once

#include "green/Value.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace openstone {

inline constexpr std::size_t maxPresetTableCells = 1024 * 1024;

struct PresetTable {
    struct Parameter {
        std::string name;
        std::vector<green::Value> values;
    };

    std::vector<std::string> programs;
    std::vector<Parameter> parameters;
};

[[nodiscard]] std::string writePresetText(const PresetTable& table);
[[nodiscard]] PresetTable readPresetText(std::string_view text);

}
