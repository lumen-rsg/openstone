#include "Schematic.hpp"

#include "green/Fourier.hpp"
#include "stream/Wave.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace openstone {
namespace {

using green::FloatArray;
using green::IntArray;
using green::Memory;
using green::MemoryArray;
using green::Value;

constexpr std::size_t maxWaveBytes = 512 * 1024 * 1024;
constexpr std::size_t maxWaveEntries = 1'000'000;
constexpr int defaultWaveRate = 44100;

struct WaveFile {
    Memory samples;
    int rate = 0;
    int status = 0;
};

std::uint32_t little(std::span<const std::uint8_t> bytes, std::size_t count)
{
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < count; ++i) value |= static_cast<std::uint32_t>(bytes[i]) << (8 * i);
    return value;
}

bool tag(std::span<const std::uint8_t> bytes, std::string_view name)
{
    return bytes.size() >= name.size() && std::ranges::equal(bytes.first(name.size()), name);
}

float decodeSample(std::span<const std::uint8_t> bytes, bool floating)
{
    if (floating) {
        if (bytes.size() == 4) return std::bit_cast<float>(little(bytes, 4));
        const auto bits = (std::uint64_t{little(bytes.subspan(4), 4)} << 32) | little(bytes, 4);
        return static_cast<float>(std::bit_cast<double>(bits));
    }
    if (bytes.size() == 1) return (static_cast<int>(bytes[0]) - 128) / 128.0F;
    const auto shift = 32 - 8 * bytes.size();
    const auto bits = little(bytes, bytes.size()) << shift;
    return static_cast<float>(std::bit_cast<std::int32_t>(bits) / 2147483648.0);
}

WaveFile decodeWave(std::span<const std::uint8_t> bytes, int channel)
{
    if (bytes.size() < 12 || !tag(bytes, "RIFF") || !tag(bytes.subspan(8), "WAVE")) return {{}, 0, 4};
    const auto length = little(bytes.subspan(4), 4);
    if (length < 4 || length > bytes.size() - 8) return {{}, 0, 4};
    auto chunks = bytes.subspan(12, length - 4);
    std::span<const std::uint8_t> format;
    std::optional<std::span<const std::uint8_t>> samples;
    while (chunks.size() >= 8) {
        const std::size_t size = little(chunks.subspan(4), 4);
        if (size > chunks.size() - 8) return {{}, 0, 4};
        const auto data = chunks.subspan(8, size);
        if (tag(chunks, "fmt ") && format.empty()) format = data;
        if (tag(chunks, "data") && !samples) samples = data;
        const auto skip = size + size % 2;
        if (skip > chunks.size() - 8) return {{}, 0, 4};
        chunks = chunks.subspan(8 + skip);
    }
    if (format.size() < 16 || !samples) return {{}, 0, 4};
    auto encoding = little(format, 2);
    if (encoding == 0xfffe && format.size() >= 40 && little(format.subspan(16), 2) >= 22) {
        constexpr std::array<std::uint8_t, 12> waveGuidTail{0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
        if (!std::ranges::equal(format.subspan(28, 12), waveGuidTail)) return {{}, 0, 4};
        encoding = little(format.subspan(24), 4);
    }
    const auto channels = little(format.subspan(2), 2);
    const auto rate = little(format.subspan(4), 4);
    const auto stride = little(format.subspan(12), 2);
    const auto bits = little(format.subspan(14), 2);
    const bool pcm = encoding == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32);
    const bool floating = encoding == 3 && (bits == 32 || bits == 64);
    if ((!pcm && !floating) || !channels || !rate || rate > std::numeric_limits<int>::max() ||
        stride != channels * (bits / 8) || samples->size() % stride ||
        (channel == 0 && channels > 2) || (channel > 0 && std::cmp_greater(channel, channels))) return {{}, 0, 4};

    const auto frames = samples->size() / stride;
    const auto width = bits / 8;
    const auto outputs = channel == 0 ? channels : 1;
    if (frames > maxWaveBytes / sizeof(float) / outputs) return {{}, 0, 4};
    FloatArray values(frames * outputs);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const auto data = samples->subspan(frame * stride, stride);
        if (channel < 0) {
            double sum = 0;
            for (std::size_t c = 0; c < channels; ++c) sum += decodeSample(data.subspan(c * width, width), floating);
            values[frame] = static_cast<float>(sum / channels);
        } else if (channel > 0) {
            values[frame] = decodeSample(data.subspan((channel - 1) * width, width), floating);
        } else {
            for (std::size_t c = 0; c < channels; ++c) {
                values[frame * channels + c] = decodeSample(data.subspan(c * width, width), floating);
            }
        }
    }
    return {Memory::fromSamples(values, outputs), static_cast<int>(rate), 2};
}

WaveFile loadWave(const std::string& filename, int channel)
{
    std::ifstream file(std::filesystem::u8path(filename), std::ios::binary | std::ios::ate);
    if (!file) return {{}, 0, 3};
    const auto size = file.tellg();
    if (size < 0 || size > maxWaveBytes) return {{}, 0, 4};
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!file.seekg(0) || !file.read(reinterpret_cast<char*>(bytes.data()), size)) return {{}, 0, 3};
    return decodeWave(bytes, channel);
}

bool saveWave(const std::string& filename, const Memory& samples, int rate, const std::string& format)
{
    const auto channels = samples.channels();
    const bool floating = format == "float32";
    const int bits = format.empty() || format == "int16" ? 16 : format == "int8" ? 8 :
        format == "int24" ? 24 : format == "int32" || floating ? 32 : 0;
    if (!channels || rate <= 0 || !bits || samples.size() % (sizeof(float) * channels)) return false;
    const std::uint32_t stride = channels * bits / 8;
    const std::size_t dataSize = samples.size() / sizeof(float) * (bits / 8);
    const auto padding = dataSize % 2;
    if (dataSize > maxWaveBytes || std::uint64_t{stride} * rate > UINT32_MAX) return false;
    std::ofstream file(std::filesystem::u8path(filename), std::ios::binary | std::ios::trunc);
    if (!file) return false;
    const auto emit = [&](std::uint32_t value, int width) {
        std::array<char, 4> bytes{};
        for (int i = 0; i < width; ++i) bytes[i] = static_cast<char>(value >> (8 * i));
        file.write(bytes.data(), width);
    };
    file.write("RIFF", 4);
    emit(static_cast<std::uint32_t>(36 + dataSize + padding), 4);
    file.write("WAVEfmt ", 8);
    emit(16, 4);
    emit(floating ? 3 : 1, 2);
    emit(channels, 2);
    emit(rate, 4);
    emit(rate * stride, 4);
    emit(stride, 2);
    emit(bits, 2);
    file.write("data", 4);
    emit(static_cast<std::uint32_t>(dataSize), 4);
    const double scale = std::ldexp(1.0, bits - 1);
    for (std::size_t i = 0; i < samples.size() / sizeof(float); ++i) {
        if (floating) {
            emit(samples.word(i), 4);
        } else {
            const auto value = samples.sample(i);
            const auto number = std::isfinite(value)
                ? std::clamp(std::round(static_cast<double>(value) * scale), -scale, scale - 1)
                : 0.0;
            const auto encoded = static_cast<std::uint32_t>(static_cast<std::int32_t>(number));
            emit(bits == 8 ? encoded + 128U : encoded, bits / 8);
        }
        if (!file) return false;
    }
    if (padding) file.put(0);
    file.close();
    return file.good();
}

int waveFrames(const Memory& buffer)
{
    return buffer.channels() ? static_cast<int>(buffer.size() / sizeof(float) / buffer.channels()) : 0;
}

}

green::Value Schematic::readWaveFile(const SchematicNode& entry, int output) const
{
    using enum PrimitiveKind;
    const auto& state = entry.waveFile;
    if (entry.kind == LoadWaveFile) {
        const auto samples = entry.localValue ? std::get<Memory>(*entry.localValue) : Memory{};
        switch (output) {
        case 0: return samples;
        case 1: return state.rate;
        case 2: return samples.channels();
        case 3: return waveFrames(samples);
        default: return state.status;
        }
    }
    if (entry.kind == WaveTable) return entry.localValue.value_or(Memory{});
    const auto samples = entry.localValue ? std::get<MemoryArray>(*entry.localValue) : MemoryArray{};
    if (output == 0) return samples;
    if (output == 1) return state.rates;
    if (output == 3) return std::get<bool>(readInput(entry.id, 6)) ? 1 : 2;
    IntArray sizes;
    sizes.reserve(samples.entries().size());
    for (const auto& wave : samples.entries()) sizes.push_back(waveFrames(wave));
    return sizes;
}

void Schematic::receiveWaveFile(SchematicNode& entry, int input)
{
    using enum PrimitiveKind;
    auto& state = entry.waveFile;
    const auto integer = [&](int port) { return std::get<int>(readInput(entry.id, port)); };
    const auto path = [&](int port) { return std::get<std::string>(readInput(entry.id, port)); };
    if (entry.kind == LoadWaveFile) {
        if (input != 1 && input != 2) return;
        if (input == 1) {
            state.status = 1;
            fireOutput(entry, 4);
        }
        const auto file = input == 1 ? loadWave(path(0), integer(3)) : WaveFile{};
        entry.localValue = file.samples;
        state.rate = file.rate;
        state.status = file.status;
        fireOutputs(entry.id);
        return;
    }
    if (entry.kind == SaveWaveFile) {
        if (input != 1) return;
        const auto samples = std::get<Memory>(readInput(entry.id, 0));
        const int rate = hasValueInput(entry.id, 3) ? integer(3) : defaultWaveRate;
        fireOutput(entry, saveWave(path(2), samples, rate, path(4)) ? 0 : 1);
        return;
    }
    if (entry.kind == WaveTable) {
        if (input == 3) createWaveTable(entry);
        return;
    }

    const auto existing = entry.localValue ? std::get<MemoryArray>(*entry.localValue) : MemoryArray{};
    std::vector<Memory> samples(existing.entries().begin(), existing.entries().end());
    if (input == 8) {
        state.excludeSamples = std::get<bool>(readInput(entry.id, 8));
        return;
    }
    if (input == 1) {
        const auto size = static_cast<std::size_t>(std::max(0, integer(0)));
        if (size > maxWaveEntries) throw std::length_error("Wave array exceeds one million entries");
        samples.resize(size);
        state.rates.resize(size);
    } else if (input == 2 || input == 5) {
        const int index = integer(3);
        if (index < 0 || std::cmp_greater_equal(index, samples.size())) return;
        const auto file = input == 5 ? loadWave(path(4), 0) : WaveFile{};
        if (input == 5 && file.status != 2) return;
        const int channels = std::get<bool>(readInput(entry.id, 6)) ? 1 : 2;
        samples[index] = file.samples.channels() == channels ? file.samples : file.samples.withChannels(channels);
        state.rates[index] = file.rate;
    } else if (input == 6) {
        const int channels = std::get<bool>(readInput(entry.id, 6)) ? 1 : 2;
        for (auto& sample : samples) {
            if (sample.channels() && sample.channels() != channels) sample = sample.withChannels(channels);
        }
    } else if (input == 7) {
        std::ranges::fill(samples, Memory{});
        std::ranges::fill(state.rates, 0);
    } else {
        return;
    }
    entry.localValue = MemoryArray(std::move(samples));
    fireOutputs(entry.id);
}

void Schematic::createWaveTable(SchematicNode& entry)
{
    const int id = entry.id;
    auto wave = std::get<FloatArray>(readInput(id, 0));
    const auto harmonics = std::get<IntArray>(readInput(id, 2));
    const bool keepDc = std::get<bool>(readInput(id, 1));
    if (wave.size() > stream::maxWavePeriod) throw std::length_error("Wave Table supports at most 16384 samples per period");
    if (wave.empty()) {
        entry.localValue = Memory{};
        fireOutputs(id);
        return;
    }
    const auto width = std::bit_ceil(std::max(wave.size(), std::size_t{2}));
    wave.resize(width);
    auto [real, imag] = green::fourierTransform(wave, {}, false);
    if (!keepDc) real[0] = 0;
    FloatArray table;
    table.reserve(width * stream::waveTableBands);
    std::size_t previousLimit = width;
    FloatArray band;
    for (std::size_t i = 0; i < stream::waveTableBands; ++i) {
        const auto requested = i < harmonics.size() ? std::max(harmonics[i], 0) : static_cast<int>(i + 1);
        const auto limit = std::min(width / 2, static_cast<std::size_t>(requested));
        if (limit != previousLimit) {
            auto bandReal = real;
            auto bandImag = imag;
            for (std::size_t bin = limit + 1; bin < width - limit; ++bin) {
                bandReal[bin] = bandImag[bin] = 0;
            }
            band = green::fourierTransform(bandReal, bandImag, true)[0];
            previousLimit = limit;
        }
        table.insert(table.end(), band.begin(), band.end());
    }
    entry.localValue = Memory::fromSamples(table);
    fireOutputs(id);
}
}
