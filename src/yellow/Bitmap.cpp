#include "yellow/Bitmap.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

namespace openstone::yellow {

Bitmap::Bitmap(std::int32_t width, std::int32_t height, std::vector<std::uint8_t> rgba)
{
    if (width <= 0 || height <= 0 || static_cast<std::uint64_t>(width) * height * 4 != rgba.size()) {
        throw std::invalid_argument("Bitmap dimensions do not match its pixel data");
    }
    storage_ = std::make_shared<Storage>(Storage{std::make_shared<Pixels>(Pixels{width, height, std::move(rgba)})});
}

std::span<const std::uint8_t> Bitmap::rgba() const noexcept
{
    if (storage_) {
        return storage_->pixels->rgba;
    }
    return {};
}

bool Bitmap::contains(std::int32_t x, std::int32_t y) const noexcept
{
    return x >= 0 && y >= 0 && x < width() && y < height();
}

std::size_t Bitmap::pixelOffset(std::int32_t x, std::int32_t y) const noexcept
{
    return 4 * (static_cast<std::size_t>(y) * width() + x);
}

std::uint32_t Bitmap::pixelArgb(std::int32_t x, std::int32_t y) const noexcept
{
    if (!contains(x, y)) {
        return 0;
    }
    const auto pixel = rgba().subspan(pixelOffset(x, y), 4);
    return std::uint32_t(pixel[3]) << 24 | std::uint32_t(pixel[0]) << 16 |
           std::uint32_t(pixel[1]) << 8 | pixel[2];
}

void Bitmap::setPixel(std::int32_t x, std::int32_t y, std::uint32_t argb)
{
    if (!contains(x, y) || pixelArgb(x, y) == argb) {
        return;
    }
    const auto offset = pixelOffset(x, y);
    auto& pixels = storage_->pixels;
    if (pixels.use_count() != 1) {
        pixels = std::make_shared<Pixels>(*pixels);
    }
    const std::array bytes{std::uint8_t(argb >> 16), std::uint8_t(argb >> 8),
                           std::uint8_t(argb), std::uint8_t(argb >> 24)};
    std::ranges::copy(bytes, pixels->rgba.begin() + offset);
}

Bitmap Bitmap::copy() const
{
    Bitmap result;
    if (storage_) {
        result.storage_ = std::make_shared<Storage>(*storage_);
    }
    return result;
}

bool Bitmap::operator==(const Bitmap& other) const
{
    if (storage_ == other.storage_) {
        return true;
    }
    if (!storage_ || !other.storage_) {
        return false;
    }
    const auto& pixels = storage_->pixels;
    const auto& otherPixels = other.storage_->pixels;
    return pixels == otherPixels || *pixels == *otherPixels;
}

std::size_t BitmapArray::size() const noexcept
{
    return entries_ ? static_cast<std::size_t>(entries_->rbegin()->first) + 1 : 0;
}

Bitmap BitmapArray::at(std::int32_t index) const
{
    const auto& loaded = entries();
    const auto found = loaded.find(index);
    return found == loaded.end() ? Bitmap{} : found->second.copy();
}

const BitmapArray::Entries& BitmapArray::entries() const noexcept
{
    static const Entries empty;
    return entries_ ? *entries_ : empty;
}

void BitmapArray::set(std::int32_t index, Bitmap bitmap)
{
    if (index < 0 || bitmap.empty()) {
        throw std::invalid_argument("Bitmap Array requires a non-negative index and a loaded image");
    }
    if (entries_.use_count() == 1) {
        entries_->insert_or_assign(index, bitmap.copy());
    } else {
        auto replacement = std::make_shared<Entries>(entries());
        replacement->insert_or_assign(index, bitmap.copy());
        entries_ = std::move(replacement);
    }
}

bool BitmapArray::operator==(const BitmapArray& other) const
{
    return entries_ == other.entries_ || entries() == other.entries();
}

}
