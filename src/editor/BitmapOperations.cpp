#include "Schematic.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace openstone {
namespace {

using green::Value;
using yellow::Bitmap;
using yellow::Color;

constexpr float defaultGridStep = windowPixelsPerUnit;
constexpr int bitmapOutput = 0;
constexpr int viewOutput = 1;
constexpr int canvasDoneOutput = 2;
constexpr int svgLoadInput = 5;
constexpr int svgSuccessOutput = 1;
constexpr int svgErrorOutput = 2;

enum class SvgScaleMode { Center, Fit, Aspect };

SvgScaleMode svgScaleMode(std::string mode)
{
    std::ranges::transform(mode, mode.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
    });
    if (mode == "fit") {
        return SvgScaleMode::Fit;
    }
    if (mode == "aspect") {
        return SvgScaleMode::Aspect;
    }
    return SvgScaleMode::Center;
}

std::size_t pixelBytes(double width, double height)
{
    constexpr int maxExtent = 16384;
    constexpr int maxPixels = 64 * 1024 * 1024;
    if (!(width >= 1 && height >= 1 && width <= maxExtent && height <= maxExtent && width * height <= maxPixels)) {
        throw std::invalid_argument("Bitmap dimensions are outside the supported range");
    }
    return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
}

std::size_t pixelOffset(int x, int y, int width)
{
    return 4 * (static_cast<std::size_t>(y) * width + x);
}

struct SampleAxis {
    int first;
    int second;
    double fraction;
};

SampleAxis sampleAxis(int pixel, int inputSize, int outputSize)
{
    const auto center = (pixel + 0.5) * inputSize / outputSize - 0.5;
    const auto position = std::clamp(center, 0.0, double(inputSize - 1));
    const auto first = static_cast<int>(position);
    return {first, std::min(first + 1, inputSize - 1), position - first};
}

Bitmap resizeBitmap(const Bitmap& source, float scale)
{
    if (!std::isfinite(scale) || scale <= 0) {
        throw std::invalid_argument("Bitmap Resize needs a positive, finite scale");
    }
    if (source.empty() || scale == 1) {
        return source.copy();
    }
    const auto scaled = [scale](int size) { return std::max(1.0, std::trunc(double(size) * scale)); };
    const auto size = pixelBytes(scaled(source.width()), scaled(source.height()));
    const auto width = static_cast<int>(scaled(source.width()));
    const auto height = static_cast<int>(scaled(source.height()));
    if (width == source.width() && height == source.height()) {
        return source.copy();
    }

    const auto pixels = source.rgba();
    std::vector<std::uint8_t> resized(size);
    for (int y = 0; y < height; ++y) {
        const auto row = sampleAxis(y, source.height(), height);
        for (int x = 0; x < width; ++x) {
            const auto col = sampleAxis(x, source.width(), width);
            const std::array offsets{
                pixelOffset(col.first, row.first, source.width()), pixelOffset(col.second, row.first, source.width()),
                pixelOffset(col.first, row.second, source.width()), pixelOffset(col.second, row.second, source.width()),
            };
            const std::array weights{(1 - col.fraction) * (1 - row.fraction), col.fraction * (1 - row.fraction),
                                     (1 - col.fraction) * row.fraction, col.fraction * row.fraction};
            // Interpolate premultiplied channels so transparent pixels cannot bleed RGB into the visible edge.
            std::array<double, 4> sum{};
            for (std::size_t i = 0; i < offsets.size(); ++i) {
                const auto sourceAlpha = pixels[offsets[i] + 3] * weights[i];
                for (int rgb = 0; rgb < 3; ++rgb) {
                    sum[rgb] += pixels[offsets[i] + rgb] * sourceAlpha;
                }
                sum[3] += sourceAlpha;
            }
            const auto dest = pixelOffset(x, y, width);
            const auto alpha = std::lround(sum[3]);
            if (alpha > 0) {
                for (int rgb = 0; rgb < 3; ++rgb) {
                    resized[dest + rgb] = static_cast<std::uint8_t>(std::lround(sum[rgb] / sum[3]));
                }
            }
            resized[dest + 3] = static_cast<std::uint8_t>(alpha);
        }
    }
    return {width, height, std::move(resized)};
}

}

void Schematic::storeBitmapArrayEntry(SchematicNode& entry, std::int32_t index, Bitmap bitmap)
{
    std::get<yellow::BitmapArray>(*entry.localValue).set(index, std::move(bitmap));
    fireOutputs(entry.id);
}

Value Schematic::readBitmapMetadata(const SchematicNode& entry, int output) const
{
    const auto bitmap = std::get<Bitmap>(readInput(entry.id, 0));
    if (entry.kind == PrimitiveKind::BitmapBounds) {
        return yellow::Area{0, 0, static_cast<float>(bitmap.width()), static_cast<float>(bitmap.height())};
    }
    return output == 0 ? bitmap.width() : bitmap.height();
}

void Schematic::receiveBitmapOperation(SchematicNode& entry, int input)
{
    const auto bitmap = [&] { return std::get<Bitmap>(readInput(entry.id, 0)); };
    switch (entry.kind) {
    case PrimitiveKind::Svg:
        receiveSvg(entry, input);
        break;
    case PrimitiveKind::BitmapResize:
        if (input == 2) {
            storeBitmap(entry.id, resizeBitmap(bitmap(), std::get<float>(readInput(entry.id, 1))));
        }
        break;
    case PrimitiveKind::BitmapSampleAndHold:
        if (input == 1) {
            storeBitmap(entry.id, bitmap().copy());
        }
        break;
    case PrimitiveKind::BitmapGetPixel:
        if (input == 3) {
            const auto x = std::get<std::int32_t>(readInput(entry.id, 1));
            const auto y = std::get<std::int32_t>(readInput(entry.id, 2));
            entry.localValue = Color{bitmap().pixelArgb(x, y)};
            fireOutput(entry, 0);
        }
        break;
    case PrimitiveKind::BitmapGetAt:
        if (input == 2) {
            const auto array = std::get<yellow::BitmapArray>(readInput(entry.id, 0));
            const auto index = std::get<std::int32_t>(readInput(entry.id, 1));
            storeBitmap(entry.id, array.at(index));
        }
        break;
    case PrimitiveKind::BitmapSetPixel:
        if (input == 4 && std::ranges::any_of(links_, [&](const SchematicLink& link) {
                return link.target == entry.id && link.input == 3 &&
                       outputType(link.source, link.output) == green::Type::Color;
            })) {
            const auto x = std::get<std::int32_t>(readInput(entry.id, 1));
            const auto y = std::get<std::int32_t>(readInput(entry.id, 2));
            const auto color = std::get<Color>(readInput(entry.id, 3));
            bitmap().setPixel(x, y, color.argb);
        }
        break;
    case PrimitiveKind::BitmapSave:
        if (input == 2) {
            const auto source = bitmap();
            const auto path = std::get<std::string>(readInput(entry.id, 1));
            const auto encoder = std::get<std::string>(readInput(entry.id, 3));
            if (source.empty() || path.empty()) {
                throw std::invalid_argument("Bitmap Save requires a bitmap and a file path");
            }
            const auto& save = rootGraph().bitmapBackend_.save;
            if (!save) {
                throw std::runtime_error("Bitmap file saving is unavailable");
            }
            save(path, encoder, source);
            fireOutput(entry, 0);
        }
        break;
    default:
        throw std::logic_error("Unknown bitmap operation");
    }
}

void Schematic::receiveSvg(SchematicNode& entry, int input)
{
    if (input == 0) {
        return; // Path selects the next file to load.
    }

    const bool loading = input == svgLoadInput;
    auto document = entry.svgDocument;
    Bitmap bitmap;
    try {
        if (loading) {
            const auto& load = rootGraph().bitmapBackend_.loadSvg;
            const auto path = std::get<std::string>(readInput(entry.id, 0));
            if (!load || path.empty()) {
                throw std::invalid_argument("Choose an SVG file to load");
            }
            document = std::make_shared<const SvgDocument>(load(path));
        }
        if (document) {
            const auto dimension = [&](int pin, double intrinsic) {
                const bool wired = std::ranges::any_of(links_, [&](const SchematicLink& link) {
                    if (link.target != entry.id || link.input != pin) {
                        return false;
                    }
                    const auto type = outputType(link.source, link.output);
                    return type && *type != green::Type::Trigger;
                });
                const double requested = wired ? std::get<float>(readInput(entry.id, pin)) : intrinsic;
                if (!std::isfinite(requested)) {
                    throw std::invalid_argument("SVG dimensions must be finite");
                }
                return std::max(1.0, requested);
            };
            if (!document->rasterize || !std::isfinite(document->width) || !std::isfinite(document->height) ||
                document->width <= 0 || document->height <= 0) {
                throw std::invalid_argument("SVG has no intrinsic size");
            }
            const auto width = dimension(1, document->width);
            const auto height = dimension(2, document->height);
            const auto cols = std::ceil(width);
            const auto rows = std::ceil(height);
            static_cast<void>(pixelBytes(cols, rows));

            const auto xMode = svgScaleMode(std::get<std::string>(readInput(entry.id, 3)));
            const auto yMode = svgScaleMode(std::get<std::string>(readInput(entry.id, 4)));
            const double fitX = xMode == SvgScaleMode::Fit ? width / document->width : 1.0;
            const double fitY = yMode == SvgScaleMode::Fit ? height / document->height : 1.0;
            const double scaleX = xMode == SvgScaleMode::Aspect ? fitY : fitX;
            const double scaleY = yMode == SvgScaleMode::Aspect ? fitX : fitY;
            const double artworkWidth = document->width * scaleX;
            const double artworkHeight = document->height * scaleY;
            if (!std::isfinite(artworkWidth) || !std::isfinite(artworkHeight)) {
                throw std::invalid_argument("SVG scaling exceeds the drawing coordinate range");
            }
            const SvgRasterLayout layout{static_cast<std::int32_t>(cols), static_cast<std::int32_t>(rows),
                (width - artworkWidth) / 2, (height - artworkHeight) / 2, scaleX, scaleY};
            bitmap = document->rasterize(layout);
            if (bitmap.width() != layout.width || bitmap.height() != layout.height) {
                throw std::runtime_error("SVG renderer returned the wrong dimensions");
            }
        }
    } catch (const std::exception&) {
        fireOutput(entry, svgErrorOutput);
        return;
    }

    entry.svgDocument = std::move(document);
    entry.localValue = bitmap.copy();
    fireOutput(entry, bitmapOutput);
    if (loading) {
        fireOutput(entry, svgSuccessOutput);
    }
}

yellow::View Schematic::bitmapView(const SchematicNode& entry) const
{
    if (!entry.bitmapCanvas) {
        return {};
    }
    const auto& state = *entry.bitmapCanvas;
    return {.panel = entry.id, .bounds = {0, 0, state.width / state.gridStep, state.height / state.gridStep},
            .schematicPath = path_};
}

void Schematic::renderBitmap(SchematicNode& entry, int input)
{
    const bool create = entry.kind == PrimitiveKind::BitmapCreate;
    const bool redraw = create && input == 4;
    if ((!redraw && input != (create ? 3 : 2)) ||
        (entry.bitmapCanvas && entry.bitmapCanvas->rendering) || (redraw && !entry.bitmapCanvas)) {
        return;
    }

    const auto step = std::get<float>(readInput(entry.id, create ? 2 : 1));
    if (!std::isfinite(step) || step <= 0) {
        throw std::invalid_argument("Bitmap drawing needs a positive, finite grid step");
    }
    Bitmap background;
    if (!create) {
        background = std::get<Bitmap>(readInput(entry.id, 0)).copy();
        if (background.empty()) {
            return;
        }
    }

    const auto dimension = [&](int axis) {
        return std::get<std::int32_t>(readInput(entry.id, axis));
    };
    const auto size = redraw ? *entry.bitmapCanvas
        : BitmapCanvasState{create ? dimension(0) : background.width(), create ? dimension(1) : background.height(), step};
    const auto bytes = pixelBytes(size.width, size.height);
    if (!std::isfinite(size.width / step) || !std::isfinite(size.height / step)) {
        throw std::invalid_argument("Bitmap dimensions exceed the View coordinate range at this grid step");
    }

    const auto previous = std::exchange(entry.bitmapCanvas, BitmapCanvasState{size.width, size.height, step, true});
    Bitmap result;
    try {
        const auto view = bitmapView(entry);
        // A bitmap pass runs the same drawing routes and loop triggers as an on-screen View.
        const auto drawings = evaluatePanel(view, step / defaultGridStep, nullptr, true);
        if (const auto& render = rootGraph().bitmapBackend_.render) {
            result = render(*entry.bitmapCanvas, view, background, drawings).copy();
            if (result.width() != size.width || result.height() != size.height) {
                throw std::runtime_error("Bitmap renderer returned the wrong dimensions");
            }
        } else if (drawings.empty()) {
            result = create ? Bitmap{size.width, size.height, std::vector<std::uint8_t>(bytes)} : background;
        } else {
            throw std::runtime_error("Bitmap drawing is unavailable");
        }
    } catch (...) {
        entry.bitmapCanvas = previous;
        throw;
    }

    entry.bitmapCanvas->rendering = false;
    entry.localValue = std::move(result);
    if (!previous || previous->width != size.width || previous->height != size.height || previous->gridStep != step) {
        fireOutput(entry, viewOutput);
    }
    fireOutput(entry, bitmapOutput);
    if (!redraw) {
        fireOutput(entry, canvasDoneOutput);
    }
}

}
