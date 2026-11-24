#pragma once

#include "green/Memory.hpp"
#include "stream/RealtimeValue.hpp"
#include "stream/Runtime.hpp"

#include <variant>

namespace openstone::stream {

inline constexpr std::size_t waveTableBands = 256;
inline constexpr std::size_t maxWavePeriod = 16384;

enum class WaveOperation { Read, ReadHop, ReadArray, ReadTable, WriteMono, WriteStereo };
class WaveBuffer : public RealtimeValue<std::variant<green::Memory, green::MemoryArray>> {};

[[nodiscard]] std::unique_ptr<SampleProcessor> makeWaveProcessor(
    WaveOperation operation, std::shared_ptr<WaveBuffer> buffer, int channel = 0);
[[nodiscard]] std::unique_ptr<SampleProcessor> makeWavePassthrough();

}
