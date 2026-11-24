#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <vector>

namespace openstone {
class Schematic;
}

namespace openstone::yellow {

class Bitmap {
public:
    Bitmap() = default;
    // Tightly packed, top-down rows of 8-bit R, G, B, and straight alpha.
    Bitmap(std::int32_t width, std::int32_t height, std::vector<std::uint8_t> rgba);

    [[nodiscard]] bool empty() const noexcept { return !storage_; }
    [[nodiscard]] std::int32_t width() const noexcept { return storage_ ? storage_->pixels->width : 0; }
    [[nodiscard]] std::int32_t height() const noexcept { return storage_ ? storage_->pixels->height : 0; }
    // Borrowed until the image is edited. Use copy() to retain a stable frame.
    [[nodiscard]] std::span<const std::uint8_t> rgba() const noexcept;
    [[nodiscard]] std::uint32_t pixelArgb(std::int32_t x, std::int32_t y) const noexcept;
    [[nodiscard]] Bitmap copy() const;
    bool operator==(const Bitmap& other) const;

private:
    friend class openstone::Schematic;

    struct Pixels {
        std::int32_t width;
        std::int32_t height;
        std::vector<std::uint8_t> rgba;

        bool operator==(const Pixels&) const = default;
    };

    struct Storage {
        std::shared_ptr<Pixels> pixels;
    };

    [[nodiscard]] bool contains(std::int32_t x, std::int32_t y) const noexcept;
    [[nodiscard]] std::size_t pixelOffset(std::int32_t x, std::int32_t y) const noexcept;
    void setPixel(std::int32_t x, std::int32_t y, std::uint32_t argb);

    // Handles on wires share an image. Independent images share the pixel buffer until a write.
    std::shared_ptr<Storage> storage_;
};

class BitmapArray {
public:
    using Entries = std::map<std::int32_t, Bitmap>;

    // The last occupied index plus one. Gaps are empty bitmaps.
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] Bitmap at(std::int32_t index) const;
    [[nodiscard]] const Entries& entries() const noexcept;
    void set(std::int32_t index, Bitmap bitmap);
    bool operator==(const BitmapArray& other) const;

private:
    // Copy on write keeps previously delivered arrays intact.
    std::shared_ptr<Entries> entries_;
};

}
