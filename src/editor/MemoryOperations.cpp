#include "Schematic.hpp"

#include "green/Fourier.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <numbers>
#include <stdexcept>

namespace openstone {
namespace {

using green::FloatArray;
using green::IntArray;
using green::Memory;
using green::MemoryArray;

constexpr std::size_t wordBytes = sizeof(std::uint32_t);
constexpr std::size_t maxMipmapBytes = 128 * 1024 * 1024;
constexpr std::size_t maxMemoryEntries = 1'000'000;

void storeWord(std::span<std::uint8_t> bytes, std::size_t index, std::uint32_t word)
{
    if constexpr (std::endian::native == std::endian::big) word = std::byteswap(word);
    std::memcpy(bytes.data() + index * wordBytes, &word, wordBytes);
}

template<class Sample, class Range>
Memory encodeMemory(const Range& samples, int channels = 1)
{
    std::vector<std::uint8_t> bytes(samples.size() * wordBytes);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        storeWord(bytes, i, std::bit_cast<std::uint32_t>(static_cast<Sample>(samples[i])));
    }
    return Memory(std::move(bytes), channels);
}

std::size_t nonnegative(std::int32_t number)
{
    return static_cast<std::size_t>(std::max(number, 0));
}

std::size_t sectionLength(std::size_t available, std::size_t start, std::int32_t requested)
{
    const auto remaining = available - std::min(available, start);
    return requested <= 0 ? remaining : std::min(remaining, static_cast<std::size_t>(requested));
}

FloatArray readFloats(const Memory& memory, std::size_t start, std::size_t count)
{
    FloatArray result(count);
    for (std::size_t i = 0; i < count; ++i) result[i] = memory.sample(start + i);
    return result;
}

std::array<FloatArray, 4> extrema(const Memory& memory, std::size_t step)
{
    std::array<FloatArray, 4> result;
    if (step == 0 || memory.channels() == 0) return result;
    const auto stride = static_cast<std::size_t>(memory.channels());
    const auto frames = memory.size() / wordBytes / stride;
    const auto groups = frames / step + (frames % step != 0);
    for (auto& channel : result) channel.reserve(groups);

    for (std::size_t start = 0; start < frames; start += step) {
        const auto left = memory.sample(start * stride);
        const auto right = memory.sample(start * stride + stride - 1);
        std::array limits{left, left, right, right};
        const auto end = start + std::min(step, frames - start);
        for (auto frame = start + 1; frame < end; ++frame) {
            const auto x = memory.sample(frame * stride);
            const auto y = memory.sample(frame * stride + stride - 1);
            limits[0] = std::min(limits[0], x);
            limits[1] = std::max(limits[1], x);
            limits[2] = std::min(limits[2], y);
            limits[3] = std::max(limits[3], y);
        }
        for (std::size_t i = 0; i < result.size(); ++i) result[i].push_back(limits[i]);
    }
    return result;
}

MemoryArray splitMemory(const Memory& memory, std::size_t samples)
{
    if (samples == 0) return {};
    const auto width = samples * wordBytes;
    const auto count = memory.size() / width;
    if (count > maxMemoryEntries) throw std::length_error("MemArray exceeds one million entries");
    std::vector<Memory> entries;
    entries.reserve(count);
    for (std::size_t i = 0; i < count; ++i) entries.push_back(memory.view(i * width, width, 1));
    return MemoryArray(std::move(entries));
}

MemoryArray generateMipmaps(const Memory& memory, std::size_t size, double maxCut,
                            double perOctave, int stack, bool fft)
{
    if (size < 3 || !std::isfinite(perOctave) || perOctave <= 0) return {};
    const auto waves = memory.size() / wordBytes / size;
    const auto requested = std::floor(std::log2(size / 2.0) * perOctave);
    if (waves == 0 || requested < 1) return {};
    if (requested > maxMipmapBytes / wordBytes / size / waves || requested > maxMemoryEntries / waves) {
        throw std::length_error("Mem to Mipmap exceeds the memory or entry limit");
    }
    const auto levels = static_cast<std::size_t>(requested);
    std::vector<Memory> entries;
    entries.reserve(waves * levels);
    // FlowStone Alpha calculates its cutoffs against 44.1 kHz.
    constexpr double referenceRate = 44'100.0;
    const auto cutoff = std::isfinite(maxCut) ? std::clamp(maxCut, 0.0, referenceRate / 2) : 0.0;
    const auto stages = 2 * std::clamp(stack, 0, 5);
    for (std::size_t level = 0; level < levels; ++level) {
        const auto ratio = std::exp2(static_cast<double>(level) / perOctave);
        const auto lastBin = static_cast<std::size_t>(std::floor(size / (2 * ratio)));
        const auto coefficient = -std::expm1(-2 * std::numbers::pi * cutoff / (ratio * referenceRate));
        double state[10]{};
        for (std::size_t wave = 0; wave < waves; ++wave) {
            auto samples = readFloats(memory, wave * size, size);
            if (!fft && stages > 0) {
                for (auto& sample : samples) {
                    double value = sample;
                    for (int stage = 0; stage < stages; ++stage) {
                        state[stage] += coefficient * (value - state[stage]);
                        value = state[stage];
                    }
                    sample = static_cast<float>(value);
                }
            }
            auto [real, imaginary] = green::fourierTransform(samples, {}, false);
            for (std::size_t bin = lastBin + 1; bin < size - lastBin; ++bin) {
                real[bin] = imaginary[bin] = 0.0F;
            }
            entries.push_back(encodeMemory<float>(green::fourierTransform(real, imaginary, true)[0]));
        }
    }
    return MemoryArray(std::move(entries));
}

}

green::Value Schematic::readMemory(const SchematicNode& entry, int output) const
{
    using enum PrimitiveKind;
    const int id = entry.id;
    const auto memory = [&](int input) { return std::get<Memory>(readInput(id, input)); };
    const auto integer = [&](int input) { return std::get<std::int32_t>(readInput(id, input)); };
    switch (entry.kind) {
    case FloatArrayToFloatMem:
    case IntArrayToFloatMem:
    case IntArrayToIntMem:
        if (!entry.memoryCache.conversion) {
            if (entry.kind == FloatArrayToFloatMem) {
                entry.memoryCache.conversion = encodeMemory<float>(std::get<FloatArray>(readInput(id, 0)));
            } else {
                const auto samples = std::get<IntArray>(readInput(id, 0));
                entry.memoryCache.conversion = entry.kind == IntArrayToFloatMem ? encodeMemory<float>(samples)
                                                                       : encodeMemory<std::int32_t>(samples);
            }
        }
        return *entry.memoryCache.conversion;
    case MemCreate:
    case MemArrayGetAt:
    case MemSection:
        return entry.localValue.value_or(Memory{});
    case MemToFloatArray:
        return entry.localValue.value_or(FloatArray{});
    case MemInfo: {
        const auto data = memory(0);
        const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data.address()));
        switch (output) {
        case 0: return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(address >> 32));
        case 1: return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(address));
        case 2: return static_cast<std::int32_t>(data.size());
        default: return static_cast<std::int32_t>(data.channels());
        }
    }
    case MemReadByte: {
        const auto data = memory(0);
        const auto index = integer(1);
        if (data.size() == 0) return std::int32_t{-9999};
        if (index < 0 || static_cast<std::size_t>(index) >= data.size()) return std::int32_t{9999};
        return static_cast<std::int32_t>(data.byte(index));
    }
    case MemMinMax:
        if (!entry.memoryCache.extrema) {
            entry.memoryCache.extrema = std::make_shared<const std::array<FloatArray, 4>>(
                extrema(memory(0), nonnegative(integer(1))));
        }
        if (output == 4) return static_cast<std::int32_t>(entry.memoryCache.extrema->front().size());
        return entry.memoryCache.extrema->at(output);
    case MemToMemArray:
    case MemToMipmap:
        if (!entry.memoryCache.array) {
            const auto data = memory(0);
            const auto size = nonnegative(integer(1));
            entry.memoryCache.array = entry.kind == MemToMemArray ? splitMemory(data, size)
                : generateMipmaps(data, size, std::get<float>(readInput(id, 2)), std::get<float>(readInput(id, 3)),
                                  integer(4), std::get<bool>(readInput(id, 5)));
        }
        if (output == 1) return static_cast<std::int32_t>(entry.memoryCache.array->entries().size());
        return *entry.memoryCache.array;
    case MemArraySetAt: {
        const auto array = entry.localValue ? std::get<MemoryArray>(*entry.localValue) : MemoryArray{};
        if (output == 0) return array;
        if (output == 2) return std::int32_t{std::get<bool>(readInput(id, 6)) ? 1 : 2};
        IntArray sizes;
        sizes.reserve(array.entries().size());
        for (const auto& wave : array.entries()) {
            sizes.push_back(wave.channels() == 0 ? 0 : static_cast<std::int32_t>(wave.size() / wordBytes / wave.channels()));
        }
        return sizes;
    }
    default: throw std::invalid_argument("This memory connector has no value");
    }
}

void Schematic::receiveMemoryTrigger(SchematicNode& entry, int input)
{
    using enum PrimitiveKind;
    const int id = entry.id;
    const auto memory = [&](int input) { return std::get<Memory>(readInput(id, input)); };
    const auto integer = [&](int input) { return std::get<std::int32_t>(readInput(id, input)); };
    switch (entry.kind) {
    case FloatArrayToFloatMem: case IntArrayToFloatMem: case IntArrayToIntMem:
        entry.memoryCache.conversion.reset();
        break;
    case MemToMemArray: case MemToMipmap:
        entry.memoryCache.array.reset();
        break;
    case MemMinMax:
        entry.memoryCache.extrema.reset();
        break;
    case MemCreate:
        if (input == 1) {
            const auto size = nonnegative(integer(0));
            const auto channels = std::get<bool>(readInput(id, 3)) ? 2 : 1;
            entry.localValue = Memory(std::vector<std::uint8_t>(size), channels);
        } else if (input == 2) {
            entry.localValue.reset();
        } else if (input == 3 && entry.localValue) {
            const auto& data = std::get<Memory>(*entry.localValue);
            const auto channels = std::get<bool>(readInput(id, 3)) ? 2 : 1;
            entry.localValue = data.view(0, data.size(), channels);
        } else {
            return;
        }
        break;
    case MemArrayGetAt:
        if (input != 2) return;
        entry.localValue = std::get<MemoryArray>(readInput(id, 0)).at(integer(1));
        break;
    case MemSection:
    case MemToFloatArray: {
        if (input != 3) return;
        const auto data = memory(0);
        const auto start = nonnegative(integer(1));
        const auto requested = integer(2);
        if (entry.kind == MemSection) {
            entry.localValue = data.view(start, sectionLength(data.size(), start, requested),
                                         std::max(1, data.channels())).copy();
        } else {
            entry.localValue = readFloats(data, start, sectionLength(data.size() / wordBytes, start, requested));
        }
        break;
    }
    case MemCopy:
        if (input == 3) memory(0).copyFrom(nonnegative(integer(1)), memory(2));
        return;
    case MemWriteByte:
        if (input == 3) memory(0).writeByte(integer(1), integer(2));
        return;
    case MemArraySetAt: {
        if (input != 1 && input != 2 && input != 5 && input != 6) return;
        const auto array = entry.localValue ? std::get<MemoryArray>(*entry.localValue) : MemoryArray{};
        const auto entries = array.entries();
        std::vector<Memory> waves(entries.begin(), entries.end());
        const auto channels = std::get<bool>(readInput(id, 6)) ? 1 : 2;
        if (input == 1) {
            const auto size = nonnegative(integer(0));
            if (size > maxMemoryEntries) throw std::length_error("MemArray exceeds one million entries");
            if (size == waves.size()) return;
            waves.resize(size);
        } else if (input == 6) {
            for (auto& wave : waves) {
                if (wave.size() != 0 && wave.channels() != channels) wave = wave.withChannels(channels);
            }
        } else {
            const auto index = integer(3);
            if (index < 0 || static_cast<std::size_t>(index) >= waves.size()) return;
            const auto wave = input == 5 ? memory(4) : Memory{};
            if (input == 5 && wave.size() == 0) return;
            waves[index] = wave.withChannels(channels);
        }
        entry.localValue = MemoryArray(std::move(waves));
        fireOutput(entry, 0);
        fireOutput(entry, 1);
        if (input == 6) fireOutput(entry, 2);
        return;
    }
    default: break;
    }
    fireOutputs(id);
}

}
