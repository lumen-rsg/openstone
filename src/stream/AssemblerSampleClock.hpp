#pragma once

#include <bit>
#include <cstdint>

namespace openstone::stream::detail {

// FlowStone's incoming ECX is an engine clock, not a per-voice age or buffer offset.
struct AssemblerSampleClock {
    std::uint32_t position{};
    void advance() noexcept { ++position; }
    void finishBuffer() noexcept
    {
        // Both native builds use signed CMOVLE and subtract once, strictly above 1e9.
        if (std::bit_cast<std::int32_t>(position) > 1000000000) position -= 1000000000U;
    }
};

}
