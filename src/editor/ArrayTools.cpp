#include "Schematic.hpp"
#include "ArrayTypes.hpp"
#include "green/Fourier.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <functional>
#include <limits>
#include <numbers>
#include <numeric>
#include <type_traits>

namespace openstone {
namespace {

using green::FloatArray;
using green::Point;
using green::Type;
using green::Value;
using array::withGreenArray;

constexpr int arrayAction = 3;
constexpr int spectralCalculate = 2;
constexpr int drawSize = 0;
constexpr int drawIndex = 1;
constexpr int drawValue = 2;
constexpr int drawDragging = 3;
constexpr int drawSet = 4;
constexpr int drawAssign = 5;
constexpr double twoPi = 2 * std::numbers::pi;
constexpr float floatTwoPi = static_cast<float>(twoPi);

std::size_t checkedFloatArraySize(std::int32_t requested)
{
    const auto size = static_cast<std::size_t>(std::max(requested, 0));
    if (size > array::maxArrayPaddingBytes / sizeof(float)) {
        throw std::length_error("A generated Float Array can contain up to 64 MiB of samples");
    }
    return size;
}

bool isSpectral(PrimitiveKind kind)
{
    using enum PrimitiveKind;
    return kind == ArrayFft || kind == ArrayInverseFft ||
           kind == MagnitudePhaseToRealImaginary || kind == RealImaginaryToMagnitudePhase;
}

double interpolate(double a, double b, double c, double d, double t, std::int32_t mode)
{
    switch (mode) {
    case 1: return std::lerp(b, c, t);
    case 2: { // Cubic Hermite with centered tangents.
        const double bt = (c - a) * 0.5;
        const double ct = (d - b) * 0.5;
        return b + t * (bt + t * (3 * (c - b) - 2 * bt - ct + t * (2 * (b - c) + bt + ct)));
    }
    case 3: // Four-point Lagrange interpolation at -1, 0, 1 and 2.
        return -a * t * (t - 1) * (t - 2) / 6 + b * (t + 1) * (t - 1) * (t - 2) / 2
               -c * (t + 1) * t * (t - 2) / 2 + d * (t + 1) * t * (t - 1) / 6;
    case 4: { // Uniform cubic B-spline basis.
        const double remaining = 1 - t;
        return (a * remaining * remaining * remaining + b * (4 + t * t * (3 * t - 6))
                + c * (4 + remaining * remaining * (3 * remaining - 6)) + d * t * t * t) / 6;
    }
    default: return t < 0.5 ? b : c;
    }
}

template<class Array>
typename Array::value_type sampleArray(const Array& values, double index, std::int32_t mode, bool wrap = false)
{
    using Element = typename Array::value_type;
    if (values.empty() || !std::isfinite(index) || index < 0 ||
        (!wrap && index > static_cast<double>(values.size() - 1))) {
        return {};
    }

    const auto base = static_cast<std::ptrdiff_t>(index);
    const auto size = std::ssize(values);
    const auto at = [&](std::ptrdiff_t offset) -> const Element& {
        auto point = base + offset;
        if (wrap) point = (point % size + size) % size;
        else point = std::clamp(point, std::ptrdiff_t{0}, size - 1);
        return values[point];
    };
    const double t = index - static_cast<double>(base);
    if constexpr (std::is_same_v<Element, std::string>) {
        return at(t < 0.5 ? 0 : 1);
    } else {
        if (mode < 1 || mode > 4) return at(t < 0.5 ? 0 : 1);
        if (mode != 4 && t == 0) return at(0);
        const auto component = [&](auto project) {
            return interpolate(std::invoke(project, at(-1)), std::invoke(project, at(0)),
                               std::invoke(project, at(1)), std::invoke(project, at(2)), t, mode);
        };
        if constexpr (std::is_same_v<Element, Point>) {
            return {static_cast<float>(component(&Point::x)), static_cast<float>(component(&Point::y))};
        } else {
            const double sample = component([](Element value) { return value; });
            if constexpr (std::is_same_v<Element, std::int32_t>) {
                return static_cast<Element>(std::clamp(sample, static_cast<double>(std::numeric_limits<Element>::min()),
                                                               static_cast<double>(std::numeric_limits<Element>::max())));
            } else {
                return static_cast<float>(sample);
            }
        }
    }
}

FloatArray resample(const FloatArray& values, std::size_t size, std::int32_t mode, bool periodic)
{
    if (size == 0 || values.empty()) return {};
    if (size == 1) return {sampleArray(values, 0.0, mode, periodic)};

    FloatArray result(size);
    const double extent = static_cast<double>(periodic ? values.size() : values.size() - 1);
    const auto denominator = periodic ? size : size - 1;
    for (std::size_t i = 0; i < size; ++i) {
        const auto index = extent * static_cast<double>(i) / static_cast<double>(denominator);
        result[i] = sampleArray(values, index, mode, periodic);
    }
    return result;
}

bool lessFloat(float left, float right)
{
    return left < right || (!std::isnan(left) && std::isnan(right));
}

template<class Array>
Array sortArray(Array values)
{
    using Element = typename Array::value_type;
    if constexpr (std::is_same_v<Element, Point>) {
        std::ranges::sort(values, [](Point left, Point right) {
            return lessFloat(left.x, right.x) || (!lessFloat(right.x, left.x) && lessFloat(left.y, right.y));
        });
    } else if constexpr (std::is_same_v<Element, float>) {
        std::ranges::sort(values, lessFloat);
    } else {
        std::ranges::sort(values);
    }
    return values;
}

template<class Array>
typename Array::value_type sumArray(const Array& values)
{
    using Element = typename Array::value_type;
    if constexpr (std::is_same_v<Element, std::int32_t>) {
        return std::accumulate(values.begin(), values.end(), Element{}, green::addWrappedInts);
    } else if constexpr (std::is_same_v<Element, Point>) {
        return std::accumulate(values.begin(), values.end(), Point{}, [](Point a, Point b) {
            return Point{a.x + b.x, a.y + b.y};
        });
    } else {
        return std::accumulate(values.begin(), values.end(), Element{});
    }
}

Value extremum(const FloatArray& values, int output, bool maximum)
{
    auto best = values.end();
    for (auto it = values.begin(); it != values.end(); ++it) {
        if (!std::isnan(*it) && (best == values.end() || (maximum ? *it > *best : *it < *best))) {
            best = it;
        }
    }
    if (output == 0) return best == values.end() ? 0.0F : *best;
    if (best == values.end()) return std::int32_t{-1};
    return static_cast<std::int32_t>(std::min<std::ptrdiff_t>(best - values.begin(),
                                                             std::numeric_limits<std::int32_t>::max()));
}

FloatArray normalize(FloatArray values)
{
    float peak = 0;
    for (const float value : values) peak = std::max(peak, std::abs(value));
    if (peak == 0) return values;
    for (auto& value : values) {
        value = std::isinf(value) ? std::copysign(1.0F, value) : value / peak;
    }
    return values;
}

green::Spectrum convertSpectrum(const FloatArray& first, const FloatArray& second, bool toPolar)
{
    const auto size = std::max(first.size(), second.size());
    green::Spectrum result{FloatArray(size), FloatArray(size)};
    for (std::size_t i = 0; i < size; ++i) {
        const double a = i < first.size() ? first[i] : 0;
        const double b = i < second.size() ? second[i] : 0;
        if (toPolar) {
            result[0][i] = static_cast<float>(std::hypot(a, b));
            double radians = (a == 0 && b == 0) ? 0 : std::atan2(b, a);
            if (radians < 0) radians += twoPi;
            const auto phase = static_cast<float>(radians);
            result[1][i] = phase == floatTwoPi ? 0 : phase;
        } else {
            result[0][i] = static_cast<float>(a * std::cos(b));
            result[1][i] = static_cast<float>(a * std::sin(b));
        }
    }
    return result;
}

}

std::size_t Schematic::arraySplitterOutputCount(int id) const
{
    std::size_t count = 1;
    if (const auto type = inputType(id, 0)) {
        count = withGreenArray(*type, [&]<class Array> {
            return std::get<Array>(readInput(id, 0)).size();
        });
    }
    // Keep wired pins visible when a source shrinks so they can still be inspected or removed.
    for (const auto& link : links_) {
        if (link.source == id) count = std::max(count, static_cast<std::size_t>(link.output) + 1);
    }
    return std::clamp(count, std::size_t{1}, primitive(PrimitiveKind::ArraySplitter).outputs.size());
}

FloatArray Schematic::readBiquadResponse(int id, bool phase) const
{
    const auto number = [&](int input) { return static_cast<double>(std::get<float>(readInput(id, input))); };
    const auto count = checkedFloatArraySize(std::get<std::int32_t>(readInput(id, 8)));
    const double sampleRate = number(9);
    double low = number(6);
    double high = number(7);
    const bool logF = std::get<bool>(readInput(id, phase ? 10 : 11));
    const bool logA = !phase && std::get<bool>(readInput(id, 10));
    if (count == 0 || !std::isfinite(sampleRate) || sampleRate <= 0 ||
        !std::isfinite(low) || !std::isfinite(high) || (logF && (low <= 0 || high <= 0))) {
        return {};
    }
    if (logF) {
        low = std::log(low);
        high = std::log(high);
    }

    const double a0 = number(0), a1 = number(1), a2 = number(2);
    const double b0 = number(3), b1 = number(4), b2 = number(5);
    FloatArray response(count);
    for (std::size_t i = 0; i < count; ++i) {
        // FlowStone divides the span by N, so HighF is outside the sampled grid.
        const double coordinate = std::lerp(low, high, static_cast<double>(i) / count);
        const double frequency = logF ? std::exp(coordinate) : coordinate;
        const auto inverseZ = std::polar(1.0, -twoPi * frequency / sampleRate);
        const auto numerator = a0 + inverseZ * (a1 + inverseZ * a2);
        const auto denominator = b0 + inverseZ * (b1 + inverseZ * b2);
        if (phase) {
            const auto product = numerator * std::conj(denominator);
            response[i] = product == std::complex<double>{} ? 0.F : static_cast<float>(std::arg(product));
        } else {
            const double power = std::norm(numerator) / std::norm(denominator);
            response[i] = static_cast<float>(logA ? 10 * std::log10(power) : power);
        }
    }
    return response;
}

Value Schematic::readArrayTool(const SchematicNode& entry, int output) const
{
    using enum PrimitiveKind;
    const auto id = entry.id;
    const auto floats = [&](int input) { return std::get<FloatArray>(readInput(id, input)); };
    const auto integer = [&](int input) { return std::get<std::int32_t>(readInput(id, input)); };
    switch (entry.kind) {
    case SortArray:
    case ArraySum:
        return withGreenArray(*inputType(id, 0), [&]<class Array>() -> Value {
            auto values = std::get<Array>(readInput(id, 0));
            if (entry.kind == SortArray) return sortArray(std::move(values));
            return sumArray(values);
        });
    case ArrayLength:
        return readArrayLength(id);
    case ArrayInterpolatedGet:
    case ArraySplitter:
        return withGreenArray(*inputType(id, 0), [&]<class Array>() -> Value {
            using Element = typename Array::value_type;
            if (entry.kind == ArrayInterpolatedGet) return entry.localValue.value_or(Value{Element{}});
            const auto values = std::get<Array>(readInput(id, 0));
            return static_cast<std::size_t>(output) < values.size() ? Value{values[output]} : Value{Element{}};
        });
    case ArrayLoop:
        if (output == 1) return entry.greenLoopValue ? entry.greenLoopValue->index : 0;
        if (entry.greenLoopValue) return entry.greenLoopValue->value;
        return withGreenArray(*inputType(id, 0), []<class Array>() -> Value {
            return typename Array::value_type{};
        });
    case FloatArrayDraw:
        if (entry.localValue) return *entry.localValue;
        return FloatArray(checkedFloatArraySize(integer(drawSize)));
    case FloatArrayResample:
        return resample(floats(0), checkedFloatArraySize(integer(1)), integer(2), std::get<bool>(readInput(id, 3)));
    case FloatArrayToPointArray: {
        const auto x = floats(0);
        const auto y = floats(1);
        green::PointArray points(std::max(x.size(), y.size()));
        for (std::size_t i = 0; i < x.size(); ++i) points[i].x = x[i];
        for (std::size_t i = 0; i < y.size(); ++i) points[i].y = y[i];
        return points;
    }
    case PointArrayToFloatArray: {
        const auto points = std::get<green::PointArray>(readInput(id, 0));
        FloatArray values;
        values.reserve(points.size());
        const auto member = output == 0 ? &Point::x : &Point::y;
        for (const auto& point : points) values.push_back(point.*member);
        return values;
    }
    case MinFloatArray:
    case MaxFloatArray:
        return extremum(floats(0), output, entry.kind == MaxFloatArray);
    case ArrayNorm:
        return normalize(floats(0));
    case BiquadFrequencyResponse:
    case BiquadPhaseResponse:
        return readBiquadResponse(id, entry.kind == BiquadPhaseResponse);
    case ArrayFft:
    case ArrayInverseFft:
    case MagnitudePhaseToRealImaginary:
    case RealImaginaryToMagnitudePhase:
        return entry.arrayTransformResult.at(output);
    default:
        throw std::logic_error("Expected an Array primitive");
    }
}

void Schematic::runArrayLoop(SchematicNode& entry)
{
    using Clock = std::chrono::steady_clock;
    const auto id = entry.id;
    const auto type = inputType(id, 0);
    entry.greenLoopValue.reset();
    if (type) {
        const bool reverse = std::get<bool>(readInput(id, 1));
        const auto limit = std::get<bool>(readInput(id, 2)) ? Clock::time_point::max()
                                                          : Clock::now() + std::chrono::milliseconds(50);
        withGreenArray(*type, [&]<class Array> {
            const auto values = std::get<Array>(readInput(id, 0));
            if (values.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) + 1) {
                throw std::length_error("Array Loop indices exceed the Int range");
            }
            for (std::size_t i = 0; i < values.size() && Clock::now() < limit; ++i) {
                const auto index = reverse ? values.size() - 1 - i : i;
                entry.greenLoopValue = LoopValue{values[index], static_cast<std::int32_t>(index)};
                fireOutput(entry, 2);
            }
        });
    }
    fireOutput(entry, 3);
}

void Schematic::receiveArrayDrawTrigger(SchematicNode& entry, int input)
{
    const int id = entry.id;
    switch (input) {
    case drawDragging:
        if (!std::get<bool>(readInput(id, drawDragging))) entry.arrayDrawIndex.reset();
        return;
    case drawSize: {
        const auto size = checkedFloatArraySize(std::get<std::int32_t>(readInput(id, drawSize)));
        if (!entry.localValue) entry.localValue = FloatArray{};
        std::get<FloatArray>(*entry.localValue).resize(size);
        entry.arrayDrawIndex.reset();
        break;
    }
    case drawAssign:
        entry.localValue = readInput(id, drawAssign);
        entry.arrayDrawIndex.reset();
        break;
    case drawSet: {
        const auto index = std::get<std::int32_t>(readInput(id, drawIndex));
        const auto size = entry.localValue ? std::get<FloatArray>(*entry.localValue).size()
            : checkedFloatArraySize(std::get<std::int32_t>(readInput(id, drawSize)));
        if (index < 0 || static_cast<std::size_t>(index) >= size) return;
        const auto value = std::get<float>(readInput(id, drawValue));
        const bool dragging = std::get<bool>(readInput(id, drawDragging));
        if (!entry.localValue) entry.localValue = FloatArray(size);
        auto& values = std::get<FloatArray>(*entry.localValue);
        if (dragging && entry.arrayDrawIndex && *entry.arrayDrawIndex != index) {
            const auto from = *entry.arrayDrawIndex;
            const auto first = std::min(from, index) + 1;
            const auto last = std::max(from, index);
            for (auto i = first; i < last; ++i) {
                values[i] = std::lerp(values[from], value, static_cast<float>(i - from) / static_cast<float>(index - from));
            }
        }
        values[index] = value;
        entry.arrayDrawIndex = dragging ? std::optional{index} : std::nullopt;
        break;
    }
    default:
        return;
    }
    fireOutput(entry, 0);
}

void Schematic::receiveArrayToolTrigger(SchematicNode& entry, int input)
{
    using enum PrimitiveKind;
    const int id = entry.id;
    if (isSpectral(entry.kind)) {
        if (input != spectralCalculate) return;
        const auto first = std::get<FloatArray>(readInput(id, 0));
        const auto second = std::get<FloatArray>(readInput(id, 1));
        const bool fft = entry.kind == ArrayFft || entry.kind == ArrayInverseFft;
        entry.arrayTransformResult = fft ? green::fourierTransform(first, second, entry.kind == ArrayInverseFft)
                                         : convertSpectrum(first, second, entry.kind == RealImaginaryToMagnitudePhase);
    } else if (entry.kind == ArrayInterpolatedGet) {
        if (input != arrayAction) return;
        if (const auto type = inputType(id, 0)) {
            const auto mode = std::get<std::int32_t>(readInput(id, 1));
            const auto index = std::get<float>(readInput(id, 2));
            entry.localValue = withGreenArray(*type, [&]<class Array>() -> Value {
                return sampleArray(std::get<Array>(readInput(id, 0)), index, mode);
            });
            fireOutput(entry, 0);
        }
        fireOutput(entry, 1);
        return;
    } else if (entry.kind == ArrayLoop) {
        if (input == arrayAction) runArrayLoop(entry);
        return;
    } else if (entry.kind == ArraySplitter) {
        if (inputType(id, 0)) {
            const auto count = arraySplitterOutputCount(id);
            for (std::size_t i = 0; i < count; ++i) fireOutput(entry, i);
        }
        return;
    } else if (entry.kind == FloatArrayDraw) {
        receiveArrayDrawTrigger(entry, input);
        return;
    }
    fireOutputs(id);
}

}
