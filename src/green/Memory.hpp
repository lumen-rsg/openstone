#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace openstone::green {

// A Mem carries tightly packed bytes. A view owns the underlying buffer for its lifetime.
class Memory {
public:
    Memory() = default;
    explicit Memory(std::vector<std::uint8_t> bytes, int channels = 1);
    [[nodiscard]] static Memory fromSamples(std::span<const float> samples, int channels = 1);

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] int channels() const noexcept { return channels_; }
    // Use a snapshot when the audio thread may be writing to the buffer.
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept;
    [[nodiscard]] std::vector<std::uint8_t> snapshotBytes() const;
    [[nodiscard]] const void* address() const noexcept;
    [[nodiscard]] Memory copy() const;
    [[nodiscard]] Memory view(std::size_t start, std::size_t length, int channels) const;
    [[nodiscard]] Memory withChannels(int channels) const;
    [[nodiscard]] std::uint32_t word(std::size_t index) const noexcept;
    [[nodiscard]] std::uint32_t wordAt(std::size_t offset) const noexcept;
    void writeWordAt(std::size_t offset, std::uint32_t value) const noexcept;
    [[nodiscard]] float sample(std::size_t index) const noexcept { return std::bit_cast<float>(word(index)); }
    [[nodiscard]] std::uint8_t byte(std::size_t index) const noexcept;
    void writeSample(std::size_t index, float value) const noexcept;
    void writeByte(std::int32_t index, std::int32_t value) const noexcept;
    void copyFrom(std::size_t offset, const Memory& source) noexcept;

    bool operator==(const Memory& other) const;

private:
    void storeByte(std::size_t index, std::uint8_t value) const noexcept;
    std::shared_ptr<std::vector<std::uint32_t>> storage_;
    std::size_t start_ = 0;
    std::size_t size_ = 0;
    int channels_ = 0;
};

class MemoryArray {
public:
    MemoryArray() = default;
    explicit MemoryArray(std::vector<Memory> entries);

    [[nodiscard]] std::span<const Memory> entries() const noexcept;
    [[nodiscard]] Memory at(std::int32_t index) const noexcept;
    [[nodiscard]] MemoryArray copy() const;
    bool operator==(const MemoryArray& other) const;

private:
    std::shared_ptr<const std::vector<Memory>> entries_;
};

}
