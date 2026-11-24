#include "green/Memory.hpp"

#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <utility>

namespace openstone::green {
namespace {

constexpr std::size_t width = sizeof(std::uint32_t);
static_assert(std::atomic_ref<std::uint32_t>::is_always_lock_free);

}

Memory::Memory(std::vector<std::uint8_t> bytes, int channels)
{
    if (channels != 1 && channels != 2) throw std::invalid_argument("Mem requires one or two channels");
    if (bytes.empty()) return;
    size_ = bytes.size();
    channels_ = channels;
    storage_ = std::make_shared<std::vector<std::uint32_t>>(size_ / width + (size_ % width != 0));
    for (std::size_t i = 0; i < size_; ++i) {
        (*storage_)[i / width] |= static_cast<std::uint32_t>(bytes[i]) << (8 * (i % width));
    }
}

Memory Memory::fromSamples(std::span<const float> samples, int channels)
{
    if (channels != 1 && channels != 2) throw std::invalid_argument("Mem requires one or two channels");
    Memory result;
    if (samples.empty()) return result;
    result.channels_ = channels;
    result.size_ = samples.size_bytes();
    result.storage_ = std::make_shared<std::vector<std::uint32_t>>();
    result.storage_->reserve(samples.size());
    for (float sample : samples) result.storage_->push_back(std::bit_cast<std::uint32_t>(sample));
    return result;
}

std::span<const std::uint8_t> Memory::bytes() const noexcept
{
    return {static_cast<const std::uint8_t*>(address()), size_};
}

std::vector<std::uint8_t> Memory::snapshotBytes() const
{
    std::vector<std::uint8_t> result(size_);
    std::size_t offset = 0;
    for (; offset / width < size_ / width; offset += width) {
        const auto value = word(offset / width);
        for (std::size_t i = 0; i < width; ++i) result[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
    for (; offset < size_; ++offset) result[offset] = byte(offset);
    return result;
}

const void* Memory::address() const noexcept
{
    return storage_ ? reinterpret_cast<const std::uint8_t*>(storage_->data()) + start_ : nullptr;
}

Memory Memory::copy() const
{
    auto data = snapshotBytes();
    return data.empty() ? Memory{} : Memory(std::move(data), channels_);
}

bool Memory::operator==(const Memory& other) const
{
    if (channels_ != other.channels_ || size_ != other.size_) return false;
    if (storage_ == other.storage_ && start_ == other.start_) return true;
    for (std::size_t i = 0; i < size_ / width; ++i) {
        if (word(i) != other.word(i)) return false;
    }
    for (auto i = size_ - size_ % width; i < size_; ++i) {
        if (byte(i) != other.byte(i)) return false;
    }
    return true;
}

Memory Memory::view(std::size_t start, std::size_t length, int channels) const
{
    if (start >= size_ || length == 0) return {};
    if (channels != 1 && channels != 2) throw std::invalid_argument("Mem requires one or two channels");
    auto result = *this;
    result.start_ += start;
    result.size_ = std::min(length, size_ - start);
    result.channels_ = channels;
    return result;
}

Memory Memory::withChannels(int channels) const
{
    if (channels != 1 && channels != 2) throw std::invalid_argument("Mem requires one or two channels");
    if (size_ == 0 || channels_ == channels) return copy();
    const auto frames = size_ / width / channels_;
    std::vector<float> converted(frames * channels);
    for (std::size_t i = 0; i < frames; ++i) {
        if (channels == 1) converted[i] = 0.5F * sample(2 * i) + 0.5F * sample(2 * i + 1);
        else converted[2 * i] = converted[2 * i + 1] = sample(i);
    }
    return fromSamples(converted, channels);
}

std::uint32_t Memory::word(std::size_t index) const noexcept
{
    if (index >= size_ / width) return 0;
    return wordAt(index * width);
}

std::uint32_t Memory::wordAt(std::size_t offset) const noexcept
{
    if (offset > size_ || size_ - offset < width) return 0;
    const auto position = start_ + offset;
    const auto wordIndex = position / width;
    const auto shift = 8 * (position % width);
    const auto first = std::atomic_ref((*storage_)[wordIndex]).load(std::memory_order_relaxed);
    if (!shift) return first;
    const auto second = std::atomic_ref((*storage_)[wordIndex + 1]).load(std::memory_order_relaxed);
    return (first >> shift) | (second << (32 - shift));
}

void Memory::writeWordAt(std::size_t offset, std::uint32_t value) const noexcept
{
    if (offset > size_ || size_ - offset < width) return;
    const auto position = start_ + offset;
    if (position % width == 0) {
        std::atomic_ref((*storage_)[position / width]).store(value, std::memory_order_relaxed);
        return;
    }
    for (std::size_t byte = 0; byte < width; ++byte) {
        storeByte(offset + byte, static_cast<std::uint8_t>(value >> (8 * byte)));
    }
}

std::uint8_t Memory::byte(std::size_t index) const noexcept
{
    if (index >= size_) return 0;
    const auto position = start_ + index;
    const auto word = std::atomic_ref((*storage_)[position / width]).load(std::memory_order_relaxed);
    return static_cast<std::uint8_t>(word >> (8 * (position % width)));
}

void Memory::writeSample(std::size_t index, float value) const noexcept
{
    if (index >= size_ / width || start_ % width) return;
    std::atomic_ref((*storage_)[start_ / width + index])
        .store(std::bit_cast<std::uint32_t>(value), std::memory_order_relaxed);
}

void Memory::writeByte(std::int32_t index, std::int32_t value) const noexcept
{
    if (index >= 0 && static_cast<std::size_t>(index) < size_) {
        storeByte(static_cast<std::size_t>(index), static_cast<std::uint8_t>(value));
    }
}

void Memory::storeByte(std::size_t index, std::uint8_t value) const noexcept
{
    const auto position = start_ + index;
    const auto shift = 8 * (position % width);
    const auto mask = std::uint32_t{255} << shift;
    const auto replacement = static_cast<std::uint32_t>(value) << shift;
    std::atomic_ref word((*storage_)[position / width]);
    auto previous = word.load(std::memory_order_relaxed);
    while (!word.compare_exchange_weak(previous, (previous & ~mask) | replacement, std::memory_order_relaxed)) {}
}

void Memory::copyFrom(std::size_t offset, const Memory& source) noexcept
{
    if (offset >= size_ || source.size() == 0) return;
    const auto count = std::min(size_ - offset, source.size());
    // Copy backwards when views overlap and the destination follows the source.
    const bool backwards = storage_ == source.storage_ && start_ + offset > source.start_ &&
        start_ + offset - source.start_ < count;
    for (std::size_t i = 0; i < count; ++i) {
        const auto at = backwards ? count - 1 - i : i;
        storeByte(offset + at, source.byte(at));
    }
}

MemoryArray::MemoryArray(std::vector<Memory> entries)
{
    if (!entries.empty()) entries_ = std::make_shared<const std::vector<Memory>>(std::move(entries));
}

std::span<const Memory> MemoryArray::entries() const noexcept
{
    return entries_ ? std::span<const Memory>{*entries_} : std::span<const Memory>{};
}

Memory MemoryArray::at(std::int32_t index) const noexcept
{
    const auto all = entries();
    return index >= 0 && static_cast<std::size_t>(index) < all.size() ? all[index] : Memory{};
}

MemoryArray MemoryArray::copy() const
{
    std::vector<Memory> result;
    result.reserve(entries().size());
    for (const auto& memory : entries()) result.push_back(memory.copy());
    return MemoryArray(std::move(result));
}

bool MemoryArray::operator==(const MemoryArray& other) const
{
    return entries_ == other.entries_ || std::ranges::equal(entries(), other.entries());
}

}
