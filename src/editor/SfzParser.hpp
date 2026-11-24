#pragma once

#include "green/Value.hpp"
#include <span>


namespace openstone {

struct SfzState {
    std::int32_t result = 1;
    std::vector<green::Value> columns;
};

[[nodiscard]] std::span<const green::BusChannel> sfzChannels();
[[nodiscard]] SfzState parseSfz(std::string_view source, std::string_view path);

}
