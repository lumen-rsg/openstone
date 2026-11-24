#pragma once

#include "green/Memory.hpp"
#include "green/Point.hpp"
#include "yellow/Value.hpp"

#include <cstdint>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace openstone::stream { class VoiceBank; }

namespace openstone::green {

enum class Type : std::uint8_t {
    Float = 1,
    Int = 2,
    Boolean = 3,
    String = 4,
    FloatArray = 5,
    IntArray = 6,
    StringArray = 7,
    Trigger = 8,
    Point = 9,
    PointArray = 10,
    View = 11,
    Area = 12,
    MouseMessage = 13,
    Color = 14,
    ColorMatrix = 15,
    PaintTool = 16,
    Font = 17,
    StringFormat = 18,
    Bitmap = 19,
    BitmapArray = 20,
    Bus = 21,
    Mem = 22,
    MemArray = 23,
    Stream = 24,
    Poly = 25,
    Mono = 26,
    Mono4 = 27,
    StreamBoolean = 28,
    PolyBoolean = 29,
    MonoBoolean = 30,
    StreamDouble = 31,
    PolyDouble = 32,
    MonoDouble = 33,
    Mono4Double = 34,
    MIDI = 35,
    Notes = 36,
    Voices = 37,
    Preset = 38,
    PolyInt = 39,
    RubyValue = 40,
};

[[nodiscard]] constexpr bool isMemoryType(Type type) noexcept
{
    return type == Type::Mem || type == Type::MemArray;
}

[[nodiscard]] constexpr bool isDspType(Type type) noexcept
{
    return (type >= Type::Stream && type <= Type::Mono4Double) || type == Type::PolyInt;
}

[[nodiscard]] constexpr bool isGenericDspStream(Type type) noexcept
{
    return type == Type::Stream || type == Type::StreamDouble || type == Type::StreamBoolean;
}

[[nodiscard]] constexpr bool isEventType(Type type) noexcept
{
    return (type >= Type::MIDI && type <= Type::Preset) || type == Type::RubyValue;
}

[[nodiscard]] constexpr bool deliversDiscreteEvents(Type type) noexcept
{
    return type == Type::MIDI || type == Type::Notes || type == Type::RubyValue;
}

[[nodiscard]] constexpr bool isYellowType(Type type) noexcept
{
    return type == Type::View || type == Type::Area || type == Type::MouseMessage ||
           type == Type::Color || type == Type::ColorMatrix || type == Type::PaintTool ||
           type == Type::Font || type == Type::StringFormat || type == Type::Bitmap || type == Type::BitmapArray;
}

struct BusChannel {
    std::string name;
    std::optional<Type> type;

    bool operator==(const BusChannel&) const = default;
};

// A Bus describes routed channels. Each channel keeps its own pull and trigger semantics.
struct Bus {
    std::vector<BusChannel> channels;

    bool operator==(const Bus&) const = default;
};

struct MidiMessage {
    std::vector<std::uint8_t> bytes;

    bool operator==(const MidiMessage&) const = default;
};

struct NoteEvent {
    std::uint8_t channel{}; // Wire channel nibble, corresponding to MIDI channels 1 through 16.
    std::uint8_t key{};
    std::uint8_t velocity{};
    bool noteOn{};
    std::array<std::int32_t, 16> tags{};

    bool operator==(const NoteEvent&) const = default;
};

struct VoiceEvent {
    std::uint64_t id{};
    NoteEvent note;

    bool operator==(const VoiceEvent&) const = default;
};

struct Notes {
    std::vector<NoteEvent> events;

    bool operator==(const Notes&) const = default;
};

struct Voices {
    std::vector<VoiceEvent> events;
    std::shared_ptr<stream::VoiceBank> bank;

    bool operator==(const Voices&) const = default;
};

struct PresetParameterChange {
    std::uint32_t parameter{};
    float value{};

    bool operator==(const PresetParameterChange&) const = default;
};

enum class PresetAction { Recall, Save, Clear, ClearAll };

struct PresetProgramChange {
    std::uint32_t program{};
    bool locked = false;
    PresetAction action = PresetAction::Recall;
    std::vector<int> managerPath;

    bool operator==(const PresetProgramChange&) const = default;
};

using Preset = std::variant<std::monostate, PresetParameterChange, PresetProgramChange>;
// Ruby owns the object. The host carries a lease on its reference in the interpreter.
struct RubyObjectReference {
    virtual ~RubyObjectReference() = default;
};

struct RubyValue {
    std::shared_ptr<const RubyObjectReference> object;

    bool operator==(const RubyValue&) const = default;
};

using FloatArray = std::vector<float>;
using IntArray = std::vector<std::int32_t>;
using StringArray = std::vector<std::string>;
using PointArray = std::vector<Point>;
using Value = std::variant<float, std::int32_t, bool, std::string, FloatArray, IntArray, StringArray,
                           Point, PointArray, yellow::View, yellow::Area, yellow::MouseMessage,
                           yellow::Color, yellow::ColorMatrix, yellow::PaintTool, yellow::Font, yellow::StringFormat,
                           yellow::Bitmap, yellow::BitmapArray, Bus, MidiMessage, Notes, Voices, Preset, Memory, MemoryArray, RubyValue>;

[[nodiscard]] constexpr bool hasSingleValueSource(Type type) noexcept
{
    return type == Type::Point || type == Type::Bus || isYellowType(type) || isEventType(type) || isMemoryType(type);
}

[[nodiscard]] Type typeOf(const Value& value);
[[nodiscard]] bool isValueType(Type type);
[[nodiscard]] float normalizeFloat(float value) noexcept;
[[nodiscard]] std::int32_t addWrappedInts(std::int32_t left, std::int32_t right) noexcept;

using ValueConverter = Value (*)(const Value&);
[[nodiscard]] ValueConverter findValueConverter(Type source, Type target);

}
