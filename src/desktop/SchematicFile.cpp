#include "SchematicFile.hpp"
#include "FlowStoneFile.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QUuid>
#include <QtEndian>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace openstone::desktop {
namespace {

using green::Type;
using green::Value;
constexpr qsizetype maxFileSize = 128 * 1024 * 1024;

[[noreturn]] void invalid(const char* field)
{
    throw std::invalid_argument(std::string("Invalid OpenStone file field ") + field);
}

QJsonArray array(QJsonValue value, const char* field, int length = -1)
{
    if (!value.isArray() || (length >= 0 && value.toArray().size() != length)) {
        invalid(field);
    }
    return value.toArray();
}

double number(QJsonValue value, const char* field)
{
    if (!value.isDouble() || !std::isfinite(value.toDouble())) {
        invalid(field);
    }
    return value.toDouble();
}

int integer(QJsonValue value, const char* field, int minimum = std::numeric_limits<int>::min(),
            int maximum = std::numeric_limits<int>::max())
{
    const auto n = number(value, field);
    if (n < minimum || n > maximum || std::trunc(n) != n) {
        invalid(field);
    }
    return static_cast<int>(n);
}

QJsonValue real(float value)
{
    if (std::isnan(value)) return QStringLiteral("nan");
    if (std::isinf(value)) return value < 0 ? QStringLiteral("-inf") : QStringLiteral("inf");
    return value;
}

float real(QJsonValue value)
{
    if (value == QStringLiteral("nan")) return std::numeric_limits<float>::quiet_NaN();
    if (value == QStringLiteral("-inf")) return -std::numeric_limits<float>::infinity();
    if (value == QStringLiteral("inf")) return std::numeric_limits<float>::infinity();
    const auto n = number(value, "float");
    if (std::abs(n) > std::numeric_limits<float>::max()) invalid("float");
    return static_cast<float>(n);
}

std::optional<Type> type(QJsonValue value)
{
    return value.isNull() || value.isUndefined() ? std::nullopt :
        std::optional{static_cast<Type>(integer(value, "connector type", 1, static_cast<int>(Type::RubyValue)))};
}

QJsonValue type(std::optional<Type> value)
{
    return value ? QJsonValue{static_cast<int>(*value)} : QJsonValue{};
}

std::string string(QJsonValue value)
{
    if (!value.isString()) invalid("string");
    return value.toString().toStdString();
}

green::Point point(QJsonValue value)
{
    const auto xy = array(value, "point", 2);
    return {real(xy[0]), real(xy[1])};
}

QJsonArray point(green::Point value)
{
    return {real(value.x), real(value.y)};
}

QJsonObject bitmap(const yellow::Bitmap& image)
{
    if (image.empty()) return {};
    const auto rgba = image.rgba();
    return {{"width", image.width()}, {"height", image.height()},
            {"rgba", QString::fromLatin1(QByteArrayView(reinterpret_cast<const char*>(rgba.data()), rgba.size())
                                             .toByteArray().toBase64())}};
}

yellow::Bitmap bitmap(QJsonObject object)
{
    if (object.isEmpty()) return {};
    const auto width = integer(object["width"], "bitmap width", 1, maxFileSize / 4);
    const auto height = integer(object["height"], "bitmap height", 1, maxFileSize / 4);
    const auto bytes = QByteArray::fromBase64Encoding(object["rgba"].toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (!bytes || bytes.decoded.size() != qint64{width} * height * 4) invalid("bitmap pixels");
    const auto* rgba = reinterpret_cast<const std::uint8_t*>(bytes.decoded.constData());
    return {width, height, {rgba, rgba + bytes.decoded.size()}};
}

QJsonObject memory(const green::Memory& buffer)
{
    const auto bytes = buffer.snapshotBytes();
    if (bytes.empty()) return {};
    const QByteArrayView view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return {{"channels", buffer.channels()}, {"bytes", QString::fromLatin1(view.toByteArray().toBase64())}};
}

green::Memory memory(QJsonValue value)
{
    if (!value.isObject()) invalid("memory buffer");
    const auto object = value.toObject();
    if (object.isEmpty()) return {};
    const auto channels = integer(object["channels"], "memory channels", 1, 2);
    if (!object["bytes"].isString()) invalid("memory bytes");
    const auto bytes = QByteArray::fromBase64Encoding(object["bytes"].toString().toLatin1(),
                                                      QByteArray::AbortOnBase64DecodingErrors);
    if (!bytes || bytes.decoded.isEmpty()) invalid("memory bytes");
    const auto* begin = reinterpret_cast<const std::uint8_t*>(bytes.decoded.constData());
    return green::Memory({begin, begin + bytes.decoded.size()}, channels);
}

template<class Range>
QJsonArray encodeArray(const Range& source, const auto& convert)
{
    QJsonArray result;
    for (const auto& value : source) result.append(convert(value));
    return result;
}

template<class T>
T decodeArray(QJsonValue source, const auto& convert)
{
    T result;
    const auto values = array(source, "array");
    result.reserve(values.size());
    for (const auto value : values) result.push_back(convert(value));
    return result;
}

yellow::Color color(QJsonValue value)
{
    const auto n = number(value, "ARGB color");
    if (n < 0 || n > UINT32_MAX || std::trunc(n) != n) invalid("ARGB color");
    return yellow::Color{static_cast<std::uint32_t>(n)};
}


QJsonArray area(yellow::Area value) { return {value.x, value.y, value.width, value.height}; }
float drawingNumber(QJsonValue value, const char* field) {
    const auto n = number(value, field);
    if (std::abs(n) > 1000000) invalid(field);
    return float(n);
}
yellow::Area area(QJsonValue value) {
    const auto fields = array(value, "brush area", 4);
    return {drawingNumber(fields[0], "brush x"), drawingNumber(fields[1], "brush y"),
            drawingNumber(fields[2], "brush width"), drawingNumber(fields[3], "brush height")};
}
green::Point drawingPoint(QJsonValue value) {
    const auto fields = array(value, "brush point", 2);
    return {drawingNumber(fields[0], "brush x"), drawingNumber(fields[1], "brush y")};
}
QJsonObject radialPath(const yellow::RadialPath& path) {
    return {{"points", encodeArray(path.points, [](auto p) { return point(p); })},
            {"types", encodeArray(path.types, [](auto type) { return int(type); })}, {"winding", path.winding}};
}
yellow::RadialPath radialPath(QJsonValue value) {
    if (!value.isObject()) invalid("radial boundary");
    const auto fields = value.toObject();
    const auto points = array(fields["points"], "radial boundary points");
    const auto types = array(fields["types"], "radial boundary types");
    if (points.size() > yellow::maxRadialPathPoints || types.size() > yellow::maxRadialPathPoints || !fields["winding"].isBool())
        invalid("radial boundary fields");
    yellow::RadialPath path;
    for (const auto p : points) path.points.push_back(drawingPoint(p));
    for (const auto type : types) path.types.push_back(integer(type,"radial point type",0,255));
    path.winding = fields["winding"].toBool();
    yellow::validateRadialPath(path);
    return path;
}

QJsonObject paintTool(const yellow::PaintTool& tool) {
    return std::visit([](const auto& v) -> QJsonObject {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::is_same_v<T, yellow::PenStroke>)
            return {{"kind", "pen"}, {"color", qint64{v.color.argb}}, {"width", v.width}, {"style", int(v.style)}};
        else if constexpr (std::is_same_v<T, yellow::SolidBrush>)
            return {{"kind", "solid"}, {"color", qint64{v.color.argb}}};
        else if constexpr (std::is_same_v<T, yellow::LinearGradientBrush>)
            return {{"kind", "linear"}, {"start", qint64{v.start.argb}}, {"end", qint64{v.end.argb}},
                    {"area", area(v.area)}, {"angle", v.angle}, {"wrap", int(v.wrap)}};
        else if constexpr (std::is_same_v<T, yellow::RadialGradientBrush>)
            return {{"kind", "radial"}, {"start", qint64{v.start.argb}}, {"end", qint64{v.end.argb}},
                    {"area", area(v.area)}, {"shape", int(v.shape)}, {"center", v.center ? QJsonValue(point(*v.center)) : QJsonValue{}},
                    {"focus", point(v.focus)}, {"boundary", v.boundary ? QJsonValue(radialPath(*v.boundary)) : QJsonValue{}},
                    {"surroundColors", encodeArray(v.surroundColors, [](auto color) { return qint64{color.argb}; })}};
        else if constexpr (std::is_same_v<T, yellow::HatchBrush>)
            return {{"kind", "hatch"}, {"foreground", qint64{v.foreground.argb}}, {"background", qint64{v.background.argb}}, {"style", int(v.style)}};
        else return {{"kind", "texture"}, {"bitmap", bitmap(v.bitmap)}, {"wrap", int(v.wrap)}};
    }, tool);
}
yellow::PaintTool paintTool(QJsonValue value) {
    if (!value.isObject()) invalid("paint tool");
    const auto fields = value.toObject();
    const auto kind = string(fields["kind"]);
    const auto wrap = [&] { return yellow::BrushWrap(integer(fields["wrap"], "brush wrap", 0, 4)); };
    if (kind == "pen") {
        const auto width = drawingNumber(fields["width"], "pen width");
        if (width < 0) invalid("pen width");
        return yellow::PenStroke{color(fields["color"]), width, yellow::LineStyle(integer(fields["style"], "pen style", 0, 4))};
    }
    if (kind == "solid") return yellow::SolidBrush{color(fields["color"])};
    if (kind == "linear") return yellow::LinearGradientBrush{color(fields["start"]), color(fields["end"]), area(fields["area"]), drawingNumber(fields["angle"], "brush angle"), wrap()};
    if (kind == "radial") {
        const auto bounds = area(fields["area"]);
        if (bounds.width <= 0 || bounds.height <= 0) invalid("radial bounds");
        const auto focus = drawingPoint(fields["focus"]);
        if (focus.x < 0 || focus.x > 1 || focus.y < 0 || focus.y > 1) invalid("radial focus");
        std::optional<green::Point> center;
        if (!fields["center"].isNull()) center = drawingPoint(fields["center"]);
        yellow::RadialGradientBrush brush{color(fields["start"]), color(fields["end"]), bounds,
            yellow::RadialShape(integer(fields["shape"], "radial shape", 0, 1)), center, focus};
        // Older converted projects have only the standard shape/two-color fields.
        if (!fields["boundary"].isUndefined() && !fields["boundary"].isNull()) brush.boundary = radialPath(fields["boundary"]);
        if (fields.contains("surroundColors")) {
            const auto colors = array(fields["surroundColors"], "radial surround colors");
            if (colors.size() > yellow::maxRadialPathPoints || (!brush.boundary && !colors.empty()) ||
                (brush.boundary && colors.size() > brush.boundary->points.size())) invalid("radial surround color count");
            for (const auto item : colors) brush.surroundColors.push_back(color(item));
        } else if (brush.boundary) invalid("missing radial surround colors");
        return brush;
    }
    if (kind == "hatch") return yellow::HatchBrush{color(fields["foreground"]), color(fields["background"]),
        yellow::HatchStyle(integer(fields["style"], "hatch style", 0, int(yellow::HatchStyle::SolidDiamond)))};
    if (kind == "texture") {
        if (!fields["bitmap"].isObject()) invalid("texture bitmap");
        return yellow::TextureBrush{bitmap(fields["bitmap"].toObject()), wrap()};
    }
    invalid("paint tool kind");
}

QJsonObject encodeValue(const Value& value)
{
    QJsonValue data = std::visit([](const auto& item) -> QJsonValue {
        using T = std::remove_cvref_t<decltype(item)>;
        if constexpr (std::is_same_v<T, float>) return real(item);
        else if constexpr (std::is_same_v<T, int> || std::is_same_v<T, bool>) return item;
        else if constexpr (std::is_same_v<T, std::string>) return QString::fromStdString(item);
        else if constexpr (std::is_same_v<T, green::Point>) return point(item);
        else if constexpr (std::is_same_v<T, green::FloatArray>) return encodeArray(item, [](float v) { return real(v); });
        else if constexpr (std::is_same_v<T, green::IntArray>) return encodeArray(item, [](int v) { return v; });
        else if constexpr (std::is_same_v<T, green::StringArray>) return encodeArray(item, [](const auto& v) { return QString::fromStdString(v); });
        else if constexpr (std::is_same_v<T, green::PointArray>) return encodeArray(item, [](auto v) { return point(v); });
        else if constexpr (std::is_same_v<T, yellow::Color>) return qint64{item.argb};
        else if constexpr (std::is_same_v<T, yellow::Font>) {
            return QJsonObject{{"face", QString::fromStdString(item.face)}, {"size", item.size},
                {"style", QString::fromStdString(yellow::fontStyleName(item.style))},
                {"alternateFace", QString::fromStdString(item.alternateFace)},
                {"fallbackFaces", encodeArray(item.fallbackFaces, [](const auto& v) { return QString::fromStdString(v); })}};
        }
        else if constexpr (std::is_same_v<T, yellow::PaintTool>) return paintTool(item);
        else if constexpr (std::is_same_v<T, yellow::StringFormat>)
            return QJsonObject{{"flags", int(yellow::stringFormatFlags(item))}, {"horizontal", int(item.horizontal)}, {"vertical", int(item.vertical)}};
        else if constexpr (std::is_same_v<T, yellow::Bitmap>) return bitmap(item);
        else if constexpr (std::is_same_v<T, green::Memory>) return memory(item);
        else if constexpr (std::is_same_v<T, green::MemoryArray>) {
            return encodeArray(item.entries(), [](const auto& entry) { return memory(entry); });
        }
        else if constexpr (std::is_same_v<T, yellow::BitmapArray>) {
            return encodeArray(item.entries(), [](const auto& entry) { return QJsonArray{entry.first, bitmap(entry.second)}; });
        } else {
            throw std::invalid_argument("Unsupported stored node value");
        }
    }, value);
    return {{"type", type(green::typeOf(value))}, {"data", data}};
}

Value decodeValue(const QJsonObject& object)
{
    const auto data = object["data"];
    switch (type(object["type"]).value_or(Type::Trigger)) {
    case Type::Float: return real(data);
    case Type::Int: return integer(data, "int");
    case Type::Boolean:
        if (!data.isBool()) invalid("boolean");
        return data.toBool();
    case Type::String: return string(data);
    case Type::Point: return point(data);
    case Type::FloatArray: return decodeArray<green::FloatArray>(data, [](QJsonValue v) { return real(v); });
    case Type::IntArray: return decodeArray<green::IntArray>(data, [](QJsonValue v) { return integer(v, "int array"); });
    case Type::StringArray: return decodeArray<green::StringArray>(data, string);
    case Type::PointArray: return decodeArray<green::PointArray>(data, [](QJsonValue v) { return point(v); });
    case Type::Color: return color(data);
    case Type::PaintTool: return paintTool(data);
    case Type::StringFormat: {
        if (!data.isObject()) invalid("string format");
        const auto fields = data.toObject();
        const auto flags = integer(fields["flags"], "string format flags", 0, yellow::stringFormatFlagMask);
        if (flags & ~yellow::stringFormatFlagMask) invalid("string format flags");
        auto format = yellow::stringFormatFromFlags(flags);
        format.horizontal = yellow::HorizontalAlignment(integer(fields["horizontal"], "horizontal alignment", 0, 2));
        format.vertical = yellow::VerticalAlignment(integer(fields["vertical"], "vertical alignment", 0, 2));
        return format;
    }
    case Type::Font: {
        if (!data.isObject()) invalid("font");
        const auto fields = data.toObject();
        yellow::Font font;
        font.face = string(fields["face"]);
        const auto size = number(fields["size"], "font size");
        if (font.face.empty() || size <= 0 || size > 1000000) invalid("font face or size");
        font.size = float(size);
        const auto style = string(fields["style"]);
        font.style = yellow::parseFontStyle(style);
        if (yellow::fontStyleName(font.style) != style) invalid("font style");
        font.alternateFace = string(fields["alternateFace"]);
        font.fallbackFaces = decodeArray<std::vector<std::string>>(fields["fallbackFaces"], string);
        if (font.fallbackFaces.size() > 4096 || std::ranges::any_of(font.fallbackFaces, [](const auto& v) { return v.empty(); }))
            invalid("font fallback faces");
        return font;
    }
    case Type::Mem: return memory(data);
    case Type::MemArray:
        return green::MemoryArray(decodeArray<std::vector<green::Memory>>(data, [](QJsonValue value) { return memory(value); }));
    case Type::Bitmap:
        if (!data.isObject()) invalid("bitmap");
        return bitmap(data.toObject());
    case Type::BitmapArray: {
        yellow::BitmapArray result;
        for (const auto value : array(data, "bitmap array")) {
            const auto entry = array(value, "bitmap array entry", 2);
            if (!entry[1].isObject()) invalid("bitmap array image");
            result.set(integer(entry[0], "bitmap array index", 0), bitmap(entry[1].toObject()));
        }
        return result;
    }
    default: invalid("stored value type");
    }
}

QJsonObject document(const QByteArray& data, const QString& format)
{
    if (data.size() > maxFileSize) invalid("file size");
    QJsonParseError failure;
    auto parsed = QJsonDocument::fromJson(data, &failure);
    if (failure.error != QJsonParseError::NoError) throw std::invalid_argument(failure.errorString().toStdString());
    const auto object = parsed.object();
    if (object["format"] != format || object["version"] != 1) invalid("format or version");
    return object;
}

constexpr auto frameBytes = stream::SequenceAudioTrack::Frame{}.size() * sizeof(quint32);

std::size_t savedSequenceFrames(double duration, double sampleRate, std::size_t frames)
{
    return static_cast<std::size_t>(std::llround(std::min(duration * sampleRate, static_cast<double>(frames))));
}

QJsonObject encodeSequenceAudio(const stream::SequenceAudioTrack& track, double duration)
{
    const auto take = track.take();
    const auto frames = take.samples.first(savedSequenceFrames(duration, take.sampleRate, take.samples.size()));
    if (frames.empty()) return {};
    if (frames.size() > maxFileSize / 4 * 3 / frameBytes) invalid("sequencer audio size");
    QByteArray bytes(static_cast<qsizetype>(frames.size() * frameBytes), Qt::Uninitialized);
    auto* next = bytes.data();
    for (const auto& frame : frames) {
        for (const float sample : frame) {
            qToLittleEndian(std::bit_cast<quint32>(sample), next);
            next += sizeof(quint32);
        }
    }
    return {{"sampleRate", take.sampleRate}, {"pcm", QString::fromLatin1(bytes.toBase64())}};
}

QByteArray decodeBase64(QJsonValue value, const char* field)
{
    if (!value.isString() || value.toString().isEmpty()) invalid(field);
    auto bytes = QByteArray::fromBase64Encoding(value.toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (!bytes || bytes.decoded.isEmpty()) invalid(field);
    return std::move(bytes.decoded);
}

std::shared_ptr<stream::SequenceAudioTrack> decodeSequenceAudio(QJsonValue value, double duration)
{
    const auto object = value.toObject();
    const auto sampleRate = number(object["sampleRate"], "sequencer sample rate");
    if (sampleRate < 1 || sampleRate > std::numeric_limits<float>::max()) invalid("sequencer sample rate");
    const auto pcm = decodeBase64(object["pcm"], "sequencer audio data");
    const auto frames = static_cast<std::size_t>(pcm.size()) / frameBytes;
    if (static_cast<std::size_t>(pcm.size()) % frameBytes ||
        frames > savedSequenceFrames(duration, sampleRate, stream::maxSequenceFrames)) {
        invalid("sequencer frame count");
    }
    std::vector<stream::SequenceAudioTrack::Frame> samples(frames);
    const auto* next = pcm.constData();
    for (auto& frame : samples) {
        for (float& sample : frame) {
            sample = std::bit_cast<float>(qFromLittleEndian<quint32>(next));
            next += sizeof(quint32);
        }
    }
    auto track = std::make_shared<stream::SequenceAudioTrack>();
    track->restoreTake(sampleRate, std::move(samples));
    return track;
}

QJsonArray encodeSequenceMidi(const SequenceMidiState& state, double duration)
{
    QJsonArray events;
    for (const auto& [time, message] : state.take) {
        if (time.count() > duration) break;
        events.append(QJsonArray{time.count(), QString::fromLatin1(
            QByteArray(reinterpret_cast<const char*>(message.bytes.data()), message.bytes.size()).toBase64())});
    }
    return events;
}

SequenceMidiState decodeSequenceMidi(QJsonValue value, double duration)
{
    SequenceMidiState state;
    const auto events = array(value, "sequencer MIDI events");
    if (events.size() > stream::maxSequenceMidiEvents) invalid("sequencer MIDI event count");
    state.take.reserve(events.size());
    double previous = 0;
    for (const auto& event : events) {
        const auto fields = array(event, "sequencer MIDI event", 2);
        const auto timestamp = number(fields[0], "sequencer MIDI timestamp");
        if (timestamp < previous || timestamp > duration) invalid("sequencer MIDI timestamp");
        const auto payload = decodeBase64(fields[1], "sequencer MIDI data");
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(payload.constData());
        state.take.emplace_back(TickTime::duration{timestamp}, green::MidiMessage{{bytes, bytes + payload.size()}});
        previous = timestamp;
    }
    return state;
}

auto encodeConnector = [](const ModuleConnector& connector) {
    return QJsonArray{QString::fromStdString(connector.name), type(connector.type), connector.endpoint};
};

auto decodeConnector = [](QJsonValue value) {
    const auto fields = array(value, "module connector", 3);
    return ModuleConnector{string(fields[0]), type(fields[1]), integer(fields[2], "endpoint", 1)};
};

}

std::string_view SchematicFile::primitiveKey(const Primitive& spec)
{
    // Keep the original Stream keys valid in existing projects and library settings.
    using enum PrimitiveKind;
    switch (spec.kind) {
    case IsPlaying: return "Is Playing (Green)";
    case Tempo: return "Tempo (Green)";
    case TimeSignature: return "Time Signature (Green)";
    default: return spec.name;
    }
}

QJsonObject SchematicFile::encode(const Session::NodeCopy& copy, std::optional<double> sequenceDuration)
{
    const auto& node = copy.node;
    QJsonObject object{
        {"id", node.id}, {"kind", QString::fromUtf8(primitiveKey(primitive(node.kind)))}, {"x", node.x}, {"y", node.y},
        {"label", QString::fromStdString(node.label)}, {"moduleName", QString::fromStdString(node.moduleName)},
        {"wirelessLabel", QString::fromStdString(node.wirelessLabel)},
        {"typeOverride", type(node.typeOverride)}, {"boundaryType", type(node.boundaryType)}, {"boundaryIndex", node.boundaryIndex},
        {"channels", encodeArray(node.channelNames, QString::fromStdString)},
        {"panelHidden", node.panelHidden}, {"showInParent", node.showPanelInParent}, {"anchors", node.panelAnchors},
        {"panelPosition", point(node.panelPosition)}, {"propertiesPosition", point(node.propertiesPosition)},
        {"panelBounds", QJsonArray{real(node.panelBounds.x), real(node.panelBounds.y),
                                  real(node.panelBounds.width), real(node.panelBounds.height)}},
        {"moduleInputs", encodeArray(node.moduleInputs, encodeConnector)},
        {"moduleOutputs", encodeArray(node.moduleOutputs, encodeConnector)},
        {"syncGroup", QString::number(copy.syncGroup)}
    };
    if (node.compareArrayElements) object.insert("compareArrayElements", true);
    if (node.flowStoneFileDialog) object.insert("flowStoneFileDialog", true);
    if (node.flowStoneSampleAndHold) object.insert("flowStoneSampleAndHold", true);
    if (node.flowStoneRoutingBranches) {
        QJsonObject routing{{"branches", *node.flowStoneRoutingBranches}};
        if (node.kind == PrimitiveKind::LastSwitch) routing.insert("lastInput", node.lastSwitchInput);
        object.insert("flowStoneRouting", routing);
    }
    if (node.kind == PrimitiveKind::WaveFileArray && node.waveFile.excludeSamples && node.localValue) {
        const auto size = std::get<green::MemoryArray>(*node.localValue).entries().size();
        object.insert("value", encodeValue(green::MemoryArray(std::vector<green::Memory>(size))));
        object.insert("waveRates", encodeArray(green::IntArray(size), [](int rate) { return rate; }));
    } else {
        if (node.localValue) object.insert("value", encodeValue(*node.localValue));
        if (node.kind == PrimitiveKind::WaveFileArray) {
            object.insert("waveRates", encodeArray(node.waveFile.rates, [](int rate) { return rate; }));
        }
    }
    if (node.kind == PrimitiveKind::LoadWaveFile) {
        object.insert("waveRate", node.waveFile.rate);
        object.insert("waveStatus", node.waveFile.status);
    }
    if (node.kind == PrimitiveKind::WaveFileArray) object.insert("waveNoSave", node.waveFile.excludeSamples);
    if (node.midiSelection && !node.midiSelection->ports.empty()) {
        QJsonArray ports;
        for (const auto& id : node.midiSelection->ports) ports.append(QString::fromStdString(id));
        object.insert("midiPorts", ports);
    }
    if (sequenceDuration && node.sequenceAudio) {
        const auto audio = encodeSequenceAudio(*node.sequenceAudio, *sequenceDuration);
        if (!audio.isEmpty()) object.insert("sequenceAudioTake", audio);
    }
    if (sequenceDuration && node.sequenceMidi) {
        const auto midi = encodeSequenceMidi(*node.sequenceMidi, *sequenceDuration);
        if (!midi.isEmpty()) object.insert("sequenceMidiTake", midi);
    }
    if (const auto& manager = node.presetManager) {
        QJsonObject state{{"program", manager->program}, {"names", encodeArray(manager->names, [](const auto& entry) {
            return QJsonArray{entry.first, QString::fromStdString(entry.second)};
        })}};
        if (manager->editedName) state.insert("editedName", QString::fromStdString(*manager->editedName));
        object.insert("presetManager", state);
    }
    if (const auto& parameter = node.presetParameter) {
        QJsonObject state{{"saved", encodeArray(parameter->saved, [](const auto& entry) {
            return QJsonArray{entry.first, encodeValue(entry.second)};
        })}};
        if (parameter->current) state.insert("current", encodeValue(*parameter->current));
        if (parameter->flowStoneFloat)
            state.insert("flowStoneFloat", QJsonObject{{"locked", parameter->flowStoneFloat->locked},
                                                       {"order", real(parameter->flowStoneFloat->order)},
                                                       {"inactive", parameter->flowStoneFloat->inactive}});
        object.insert("presetParameter", state);
    }
    if (node.anchorReference) object.insert("anchorReference", point(*node.anchorReference));
    if (node.rubyCode) {
        object.insert("ruby", QJsonArray{QString::fromStdString(node.rubyCode->source), QString::fromStdString(node.rubyCode->draft)});
        if (node.rubyCode->savedState) object.insert("rubyState", QString::fromStdString(*node.rubyCode->savedState));
        if (node.rubyCode->manualPorts) {
            const auto encodePorts = [](const auto& ports) {
                QJsonArray list;
                for (const auto& port : ports) {
                    list.append(QJsonArray{QString::fromStdString(port.name), static_cast<int>(port.type)});
                }
                return list;
            };
            object.insert("rubyPorts", QJsonArray{encodePorts(node.rubyCode->ports.inputs),
                                                encodePorts(node.rubyCode->ports.outputs)});
        }
        if (node.rubyCode->boxSize != RubyBoxSize{}) {
            object.insert("rubySize", QJsonArray{node.rubyCode->boxSize.width, node.rubyCode->boxSize.height});
        }
    }
    if (node.codeSourcePorts) object.insert("codeSourcePorts", true);
    if (node.dspCode) {
        object.insert("dsp", QJsonArray{QString::fromStdString(node.dspCode->source), QString::fromStdString(node.dspCode->draft)});
    }
    if (node.tick) object.insert("tick", QJsonArray{node.tick->manualRate, node.tick->displaySync});
    if (node.embeddedFonts.data) {
        object.insert("embeddedFonts", encodeArray(*node.embeddedFonts.data, [](const auto& data) {
            return QString::fromLatin1(QByteArray(data.data(), data.size()).toBase64());
        }));
    }
    if (node.bitmapCanvas) {
        const auto& canvas = *node.bitmapCanvas;
        object.insert("bitmapCanvas", QJsonArray{canvas.width, canvas.height, real(canvas.gridStep)});
    }
    if (node.kind == PrimitiveKind::Module) {
        object.insert("nodes", encodeArray(copy.children, [&](const auto& child) {
            auto encoded = encode(child, sequenceDuration);
            const auto& ports = copy.portTypes.at(child.node.id);
            auto encodeType = [](auto t) { return type(t); };
            encoded.insert("inputs", encodeArray(ports.inputs, encodeType));
            encoded.insert("outputs", encodeArray(ports.outputs, encodeType));
            return encoded;
        }));
        object.insert("links", encodeArray(copy.links, [](auto link) {
            return QJsonArray{link.source, link.output, link.target, link.input};
        }));
    }
    return object;
}

Session::NodeCopy SchematicFile::decode(const QJsonObject& object, int depth, std::optional<double> sequenceDuration)
{
    if (depth > 64) invalid("module depth");
    const auto kind = string(object["kind"]);
    const auto library = primitives();
    const auto spec = std::ranges::find(library, kind, primitiveKey);
    if (spec == library.end()) invalid("primitive kind");
    Session::NodeCopy copy{{integer(object["id"], "node id", depth ? 1 : 0, INT_MAX - 1),
                            spec->kind, number(object["x"], "node x"), number(object["y"], "node y")}, {}, {}, {}};
    auto& node = copy.node;
    node.label = string(object["label"]);
    node.moduleName = string(object["moduleName"]);
    node.wirelessLabel = string(object["wirelessLabel"]);
    if ((node.kind == PrimitiveKind::MidiIn || node.kind == PrimitiveKind::MidiOut) && object.contains("midiPorts")) {
        const auto ports = object["midiPorts"];
        if (!ports.isArray()) invalid("MIDI device selection");
        node.midiSelection.emplace();
        for (const auto& port : ports.toArray()) {
            if (!port.isString() || port.toString().isEmpty()) invalid("MIDI device ID");
            auto id = port.toString().toStdString();
            if (!std::ranges::contains(node.midiSelection->ports, id)) node.midiSelection->ports.push_back(std::move(id));
        }
    }
    if (object.contains("sequenceAudioTake")) {
        if (node.kind != PrimitiveKind::StreamSequencerTrack || !sequenceDuration) invalid("sequencer audio take");
        node.sequenceAudio = decodeSequenceAudio(object["sequenceAudioTake"], *sequenceDuration);
    }
    if (object.contains("sequenceMidiTake")) {
        if (node.kind != PrimitiveKind::MidiSequencerTrack || !sequenceDuration) invalid("sequencer MIDI take");
        node.sequenceMidi = decodeSequenceMidi(object["sequenceMidiTake"], *sequenceDuration);
    }
    node.typeOverride = type(object["typeOverride"]);
    if (object.contains("compareArrayElements")) {
        if (!object["compareArrayElements"].isBool() || spec->behavior != PrimitiveBehavior::Compare) invalid("array comparison mode");
        node.compareArrayElements = object["compareArrayElements"].toBool();
    }
    if (node.kind == PrimitiveKind::MidiCCStream) {
        // Older OpenStone projects used a single Mono value from the most recent CC.
        const auto outputs = object["outputs"].toArray();
        if (outputs.size() == 1 && type(outputs.first()) == Type::Mono) node.typeOverride = Type::Mono;
    }
    node.boundaryType = type(object["boundaryType"]);
    node.boundaryIndex = integer(object["boundaryIndex"], "boundary index", 0);
    node.channelNames = decodeArray<std::vector<std::string>>(object["channels"], string);
    if ((node.kind == PrimitiveKind::AsioIn || node.kind == PrimitiveKind::AsioOut) &&
        node.channelNames.size() > maxAsioChannels) invalid("ASIO channel count");
    node.panelHidden = object["panelHidden"].toBool();
    node.showPanelInParent = object["showInParent"].toBool(true);
    node.panelAnchors = integer(object["anchors"], "panel anchors", 0, 15);
    node.panelPosition = point(object["panelPosition"]);
    node.propertiesPosition = point(object["propertiesPosition"]);
    const auto bounds = array(object["panelBounds"], "panel area", 4);
    node.panelBounds = {real(bounds[0]), real(bounds[1]), real(bounds[2]), real(bounds[3])};
    if (object.contains("anchorReference")) node.anchorReference = point(object["anchorReference"]);
    if (object.contains("value")) node.localValue = decodeValue(object["value"].toObject());
    if (node.kind == PrimitiveKind::FontEmbed && object.contains("embeddedFonts")) {
        std::size_t remaining = maxEmbeddedFontBytes;
        auto fonts = decodeArray<std::vector<std::string>>(object["embeddedFonts"], [&](QJsonValue value) {
            const auto encoded = value.toString().toLatin1();
            if (encoded.isEmpty() || static_cast<std::size_t>(encoded.size()) > ((remaining + 2) / 3) * 4) {
                invalid("embedded font size");
            }
            const auto bytes = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
            if (!bytes || bytes.decoded.isEmpty() || static_cast<std::size_t>(bytes.decoded.size()) > remaining) {
                invalid("embedded font data");
            }
            remaining -= bytes.decoded.size();
            return bytes.decoded.toStdString();
        });
        if (!fonts.empty()) node.embeddedFonts.data = std::make_shared<const std::vector<std::string>>(std::move(fonts));
    }
    const auto behavior = spec->behavior;
    if (behavior == PrimitiveBehavior::WaveFile && node.localValue &&
        (spec->outputs.empty() || green::typeOf(*node.localValue) != spec->outputs.front().type)) {
        invalid("wave value");
    }
    if (node.kind == PrimitiveKind::LoadWaveFile && object.contains("waveRate")) {
        node.waveFile.rate = integer(object["waveRate"], "wave sample rate", 0);
        node.waveFile.status = integer(object["waveStatus"], "wave status", 0, 4);
    }
    if (object.contains("flowStoneFileDialog")) {
        if (node.kind != PrimitiveKind::FileDialog || !object["flowStoneFileDialog"].isBool()) invalid("native File Dialog state");
        node.flowStoneFileDialog = object["flowStoneFileDialog"].toBool();
    }
    if (object.contains("flowStoneSampleAndHold")) {
        if (node.kind != PrimitiveKind::SampleAndHold || !object["flowStoneSampleAndHold"].isBool()) invalid("native Sample and Hold state");
        node.flowStoneSampleAndHold = object["flowStoneSampleAndHold"].toBool();
        if (node.flowStoneSampleAndHold && node.localValue && node.typeOverride &&
            green::typeOf(*node.localValue) != node.typeOverride) invalid("native Sample and Hold cache");
    }
    if (node.kind == PrimitiveKind::WaveFileArray) {
        node.waveFile.excludeSamples = object["waveNoSave"].toBool(false);
        if (object.contains("waveRates")) {
            node.waveFile.rates = decodeArray<green::IntArray>(object["waveRates"],
                [](QJsonValue value) { return integer(value, "wave array sample rate", 0); });
        }
        const auto size = node.localValue ? std::get<green::MemoryArray>(*node.localValue).entries().size() : 0;
        node.waveFile.rates.resize(size);
    }
    if (node.kind == PrimitiveKind::PresetManager && object.contains("presetManager")) {
        const auto state = object["presetManager"].toObject();
        auto& manager = node.presetManager.emplace();
        manager.program = integer(state["program"], "preset program", 0, maxPresetPrograms - 1);
        if (state.contains("editedName")) manager.editedName = string(state["editedName"]);
        for (const auto value : array(state["names"], "preset names")) {
            const auto fields = array(value, "preset name", 2);
            const auto program = integer(fields[0], "preset program", 0, maxPresetPrograms - 1);
            if (!manager.names.emplace(program, string(fields[1])).second) invalid("duplicate preset name");
        }
    }
    if (behavior == PrimitiveBehavior::PresetParameter && object.contains("presetParameter")) {
        const auto state = object["presetParameter"].toObject();
        auto& parameter = node.presetParameter.emplace();
        if (state.contains("flowStoneFloat")) {
            if (node.kind != PrimitiveKind::PresetFloat) invalid("native Preset Float state");
            const auto native = state["flowStoneFloat"].toObject();
            if (!native["locked"].isBool() || !native["inactive"].isBool()) invalid("native Preset flags");
            parameter.flowStoneFloat = PresetParameterState::FlowStoneFloat{
                native["locked"].toBool(), real(native["order"]), native["inactive"].toBool()};
        }
        const auto decodeParameter = [&](QJsonValue value) {
            auto decoded = decodeValue(value.toObject());
            if (green::typeOf(decoded) != spec->outputs.front().type) invalid("preset value type");
            if (const auto* values = std::get_if<green::FloatArray>(&decoded);
                values && values->size() > maxPresetArraySize) invalid("preset array size");
            return decoded;
        };
        if (state.contains("current")) parameter.current = decodeParameter(state["current"]);
        for (const auto value : array(state["saved"], "saved presets")) {
            const auto fields = array(value, "preset value", 2);
            const auto program = integer(fields[0], "preset program", 0, maxPresetPrograms - 1);
            if (!parameter.saved.emplace(program, decodeParameter(fields[1])).second) invalid("duplicate preset value");
        }
    }
    const bool storedValue = behavior == PrimitiveBehavior::ValueBox || behavior == PrimitiveBehavior::Counter ||
        behavior == PrimitiveBehavior::IndexSelector ||
        node.kind == PrimitiveKind::MonoToFloat || behavior == PrimitiveBehavior::LoadBitmap ||
        behavior == PrimitiveBehavior::LoadBitmapArray || behavior == PrimitiveBehavior::BitmapCanvas ||
        (behavior == PrimitiveBehavior::BitmapOperation && !spec->outputs.empty() && spec->outputs.front().type != Type::Trigger);
    if (storedValue && (!node.localValue || green::typeOf(*node.localValue) != spec->outputs.front().type)) invalid("node value");
    if (node.kind == PrimitiveKind::Select && node.localValue && green::typeOf(*node.localValue) != Type::Boolean) invalid("Select condition state");
    if (node.kind == PrimitiveKind::DropList && node.localValue && green::typeOf(*node.localValue) != Type::Int) invalid("Drop List selection state");
    if (node.localValue && (behavior == PrimitiveBehavior::MakePen || behavior == PrimitiveBehavior::MakeFont ||
                            behavior == PrimitiveBehavior::MakeSolid || behavior == PrimitiveBehavior::MakeBrush ||
                            behavior == PrimitiveBehavior::MakeStringFormat) &&
        green::typeOf(*node.localValue) != spec->outputs.front().type) invalid("drawing property value");
    if (node.localValue && green::typeOf(*node.localValue) == Type::PaintTool) {
        const auto index = std::get<yellow::PaintTool>(*node.localValue).index();
        const auto expected = node.kind == PrimitiveKind::Pen ? 0 : node.kind == PrimitiveKind::Solid ? 1 :
            node.kind == PrimitiveKind::LinearGradientBrush ? 2 : node.kind == PrimitiveKind::RadialGradientBrush ? 3 :
            node.kind == PrimitiveKind::HatchBrush ? 4 : node.kind == PrimitiveKind::TextureBrush ? 5 : -1;
        if (expected >= 0 && index != expected) invalid("brush value kind");
    }
    if (node.kind == PrimitiveKind::Settings) {
        const auto* index = node.localValue ? std::get_if<std::int32_t>(&*node.localValue) : nullptr;
        if (!index || !validSystemSetting(*index)) invalid("settings selection");
    }
    if (node.localValue && behavior == PrimitiveBehavior::MemoryOperation &&
        (spec->outputs.empty() || green::typeOf(*node.localValue) != spec->outputs.front().type)) {
        invalid("node memory value");
    }
    if (node.kind == PrimitiveKind::Ruby) {
        const auto source = array(object["ruby"], "Ruby source", 2);
        std::optional<RubyPorts> ports;
        if (object.contains("rubyPorts")) {
            const auto lists = array(object["rubyPorts"], "Ruby ports", 2);
            const auto decodePorts = [](QJsonValue value, bool output) {
                std::vector<RubyPort> ports;
                const auto list = array(value, "Ruby connectors");
                if (list.size() > maxRubyPorts) invalid("Ruby connector count");
                for (const auto& item : list) {
                    const auto port = array(item, "Ruby connector", 2);
                    auto name = string(port[0]);
                    const auto type = static_cast<Type>(integer(port[1], "Ruby connector type", 0, 255));
                    if (name.empty() || name.size() > 128 || name.find_first_of("\r\n") != std::string::npos ||
                        !supportsRubyPort(type, output) || std::ranges::contains(ports, name, &RubyPort::name)) {
                        invalid("Ruby connector");
                    }
                    ports.push_back({std::move(name), type});
                }
                return ports;
            };
            ports = RubyPorts{decodePorts(lists[0], false), decodePorts(lists[1], true)};
        }
        node.rubyCode = rubyCode(string(source[1]), string(source[0]), std::move(ports));
        if (object.contains("rubySize")) {
            const auto size = array(object["rubySize"], "Ruby box size", 2);
            node.rubyCode->boxSize = clampRubyBoxSize({integer(size[0], "Ruby box width"),
                                                     integer(size[1], "Ruby box height")});
        }
        if (object.contains("rubyState")) {
            const auto state = object["rubyState"];
            static_cast<void>(decodeBase64(state, "Ruby state"));
            node.rubyCode->savedState = string(state);
        }
    } else if (object.contains("rubyState") || object.contains("rubyPorts") || object.contains("rubySize")) {
        invalid("Ruby data on a non-Ruby node");
    }
    if (isDspEditor(node.kind)) {
        const auto dsp = array(object["dsp"], "DSP source", 2);
        auto source = string(dsp[0]);
        auto program = stream::compileDspCode(source, dspLanguage(node.kind));
        node.dspCode.emplace(DspCodeState{string(dsp[1]), std::move(source), std::move(program)});
    }
    if (object.contains("codeSourcePorts")) {
        if (!object["codeSourcePorts"].isBool() || (!node.dspCode && !node.rubyCode)) invalid("code source ports");
        node.codeSourcePorts = object["codeSourcePorts"].toBool();
    }
    if (node.kind == PrimitiveKind::Tick) {
        const auto tick = array(object["tick"], "tick", 2);
        const auto rate = real(tick[0]);
        if (!std::isfinite(rate) || rate < 0) invalid("tick rate");
        node.tick = TickState{rate, tick[1].toBool()};
    }
    if (object.contains("bitmapCanvas")) {
        const auto canvas = array(object["bitmapCanvas"], "bitmap canvas", 3);
        node.bitmapCanvas = BitmapCanvasState{integer(canvas[0], "width", 1), integer(canvas[1], "height", 1), real(canvas[2])};
    }
    if (node.kind == PrimitiveKind::Module) {
        bool syncOk = false;
        copy.syncGroup = object["syncGroup"].toString().toULongLong(&syncOk);
        if (!syncOk || copy.syncGroup > (std::uint64_t{1} << 53)) invalid("sync group");
        node.moduleInputs = decodeArray<std::vector<ModuleConnector>>(object["moduleInputs"], decodeConnector);
        node.moduleOutputs = decodeArray<std::vector<ModuleConnector>>(object["moduleOutputs"], decodeConnector);
        std::map<int, std::size_t> rows;
        for (const auto value : array(object["nodes"], "nodes")) {
            const auto fields = value.toObject();
            auto child = decode(fields, depth + 1, sequenceDuration);
            const auto id = child.node.id;
            if (!rows.emplace(id, copy.children.size()).second) invalid("duplicate node id");
            const auto decodeType = [](QJsonValue t) { return type(t); };
            auto inputs = decodeArray<std::vector<std::optional<Type>>>(fields["inputs"], decodeType);
            auto outputs = decodeArray<std::vector<std::optional<Type>>>(fields["outputs"], decodeType);
            if (inputs.size() != child.node.inputTriggers.size() || outputs.size() != child.node.outputTriggers.size()) {
                invalid("port count");
            }
            copy.portTypes.emplace(id, Schematic::PortTypes{std::move(inputs), std::move(outputs)});
            copy.children.push_back(std::move(child));
        }
        const auto checkPorts = [&](const auto& ports, PrimitiveKind endpoint) {
            for (std::size_t i = 0; i < ports.size(); ++i) {
                const auto found = rows.find(ports[i].endpoint);
                if (found == rows.end()) invalid("missing module endpoint");
                const auto& child = copy.children[found->second].node;
                if (child.kind != endpoint || child.boundaryIndex != static_cast<int>(i)) invalid("module endpoint order");
            }
        };
        checkPorts(node.moduleInputs, PrimitiveKind::ModuleInput);
        checkPorts(node.moduleOutputs, PrimitiveKind::ModuleOutput);
        for (const auto& child : copy.children) {
            const auto& entry = child.node;
            if (isModuleEndpoint(entry.kind)) {
                const auto& ports = entry.kind == PrimitiveKind::ModuleInput ? node.moduleInputs : node.moduleOutputs;
                if (static_cast<std::size_t>(entry.boundaryIndex) >= ports.size() ||
                    ports[entry.boundaryIndex].endpoint != entry.id) invalid("unregistered module endpoint");
            }
        }
        std::set<SchematicLink> unique;
        for (const auto value : array(object["links"], "links")) {
            const auto wire = array(value, "link", 4);
            const SchematicLink link{integer(wire[0], "source", 1), integer(wire[2], "target", 1),
                                     integer(wire[3], "input", 0), integer(wire[1], "output", 0)};
            if (!rows.contains(link.source) || !rows.contains(link.target) || !unique.insert(link).second ||
                static_cast<std::size_t>(link.input) >= copy.portTypes.at(link.target).inputs.size() ||
                static_cast<std::size_t>(link.output) >= copy.portTypes.at(link.source).outputs.size()) invalid("link endpoints");
            copy.links.push_back(link);
        }
    }
    const auto inputCount = hasExpandableInputs(node.kind)
        ? array(object["inputs"], "expandable inputs").size() : node.inputPorts().size();
    if (object.contains("flowStoneRouting")) {
        if (node.kind != PrimitiveKind::Multiplexer && node.kind != PrimitiveKind::LastSwitch)
            invalid("native routing kind");
        const auto routing = object["flowStoneRouting"].toObject();
        node.flowStoneRoutingBranches = integer(routing["branches"], "native routing branches", 0, 4096);
        if (node.kind == PrimitiveKind::LastSwitch) {
            if (*node.flowStoneRoutingBranches != 2) invalid("native Last Switch branches");
            node.lastSwitchInput = integer(routing["lastInput"], "native Last Switch input", 0, 1);
        }
    }
    if (hasExpandableInputs(node.kind) && inputCount <= firstExpandableInput(node.kind)) invalid("missing spare input");
    const auto outputCount = hasExpandableOutputs(node.kind)
        ? array(object["outputs"], "expandable outputs").size() : node.outputPorts().size();
    if (hasExpandableOutputs(node.kind) && outputCount == 0) invalid("missing spare output");
    if (node.kind == PrimitiveKind::Multiplexer && node.flowStoneRoutingBranches &&
        outputCount <= *node.flowStoneRoutingBranches) invalid("native routing outputs");
    node.inputTriggers.resize(inputCount);
    node.outputTriggers.resize(outputCount);
    return copy;
}

QByteArray SchematicFile::project(const Session& session)
{
    const auto& root = *session.root_;
    std::optional<double> sequenceDuration;
    if (root.hasSequencerTracks()) {
        // Sample the end once so every track in an active recording uses the same boundary.
        const auto duration = root.sequencerMode() == stream::SequenceMode::Recording
            ? root.sequenceElapsed(root.sequenceNow()) : root.sequenceDuration_;
        sequenceDuration = std::clamp(duration.count(), 0.0, stream::maxSequenceSeconds);
    }
    Session::NodeCopy copy{{0, PrimitiveKind::Module, 0, 0}, {}, root.links_, {}};
    // Counter's native saved value is persistent. Keep ordinary copy/undo
    // snapshots' reset policy, but retain project values at every module level.
    const auto retainCounters = [](const auto& self, Session::NodeCopy& item, const Schematic& owner) -> void {
        if (item.node.kind == PrimitiveKind::BasicCounter) item.node.localValue = owner.node(item.node.id).localValue;
        if (const auto* child = owner.module(item.node.id)) {
            for (auto& nested : item.children) self(self, nested, *child);
        }
    };
    for (const auto& node : root.nodes_) {
        copy.children.push_back(Session::snapshotNode(root, node.id, sequenceDuration.has_value()));
        retainCounters(retainCounters, copy.children.back(), root);
        copy.portTypes.emplace(node.id, root.ports_.at(node.id).types);
    }
    QJsonObject saved{{"format", "OpenStone"}, {"version", 1}, {"root", encode(copy, sequenceDuration)}};
    if (sequenceDuration) saved.insert("sequenceDuration", *sequenceDuration);
    return QJsonDocument(saved).toJson(QJsonDocument::Compact);
}

bool SchematicFile::hasUnsavedChanges(const Session& session, const QByteArray& savedProject)
{
    const auto dirty = [](const auto& visit, const Schematic& level) -> bool {
        for (const auto& node : level.nodes_) {
            if (node.rubyCode && node.rubyCode->stateDirty) return true;
        }
        for (const auto& [id, module] : level.modules_) {
            if (visit(visit, *module)) return true;
        }
        return false;
    };
    return dirty(dirty, *session.root_) || project(session) != savedProject;
}

QByteArray SchematicFile::saveProject(Session& session, const QString& path)
{
    std::vector<std::pair<RubyCodeState*, std::optional<std::string>>> captured;
    const auto time = session.root_->rubySeconds(session.root_->rubyNow());
    const auto capture = [&](const auto& visit, Schematic& level) -> void {
        for (auto& node : level.nodes_) {
            if (!node.rubyCode || !node.rubyCode->instance || !node.rubyCode->savesState) continue;
            auto& code = *node.rubyCode;
            code.stateDirty = true;
            try {
                level.updateRubyContext(node);
                captured.emplace_back(&code, code.instance->saveState(time));
            } catch (const std::exception& error) {
                throw std::runtime_error("Ruby saveState failed on node " + std::to_string(node.id) + ". " + error.what());
            }
        }
        for (auto& [id, module] : level.modules_) visit(visit, *module);
    };
    capture(capture, *session.root_);

    // Retain the last saved state if serialization or the atomic file write fails.
    for (auto& [code, state] : captured) code->savedState.swap(state);
    QByteArray data;
    try {
        data = project(session);
        write(path, data);
    } catch (...) {
        for (auto& [code, state] : captured) code->savedState.swap(state);
        throw;
    }
    for (auto& [code, state] : captured) code->stateDirty = false;
    return data;
}

bool SchematicFile::loadProject(Session& session, const QByteArray& data, const std::function<void()>& beforeReplace,
                                const QString& filePath)
{
    const auto& root = *session.root_;
    auto replacement = std::make_unique<Schematic>(root.bitmapLoader_, root.bitmapBackend_,
                                                   root.textMeasurer_, root.textFiles_, root.systemInfo_);
    if (!data.isEmpty()) {
        const auto saved = document(data, QStringLiteral("OpenStone"));
        std::optional<double> sequenceDuration;
        if (saved.contains("sequenceDuration")) {
            sequenceDuration = number(saved["sequenceDuration"], "sequencer duration");
            if (*sequenceDuration < 0 || *sequenceDuration > stream::maxSequenceSeconds) invalid("sequencer duration");
            replacement->sequenceDuration_ = TickTime::duration{*sequenceDuration};
        }
        const auto decoded = decode(saved["root"].toObject(), 0, sequenceDuration);
        if (decoded.node.kind != PrimitiveKind::Module || decoded.node.id != 0) invalid("schematic root");
        for (const auto& child : decoded.children) {
            if (!replacement->canPlace(child.node.kind)) invalid("root primitive");
        }
        Session::restoreGraph(*replacement, decoded, true);
    }
    return session.openSchematic(std::move(replacement), beforeReplace, filePath.toStdString());
}

bool SchematicFile::importFlowStone(Session& session, const QByteArray& data,
                                    const std::function<void()>& beforeReplace, QStringList* warnings)
{
    auto imported = FlowStoneFile::decode(data);
    const auto convert = [](const auto& self, FlowStoneNode& source) -> Session::NodeCopy {
        Session::NodeCopy copy{std::move(source.node), {}, {}, {}};
        copy.links = std::move(source.links);
        for (auto& child : source.children) {
            auto c = self(self, child);
            Schematic::PortTypes ports;
            for (const auto& p : c.node.inputPorts()) ports.inputs.push_back(p.type ? p.type : c.node.typeOverride);
            for (const auto& p : c.node.outputPorts()) ports.outputs.push_back(p.type ? p.type : c.node.typeOverride);
            copy.portTypes.emplace(c.node.id, std::move(ports));
            copy.children.push_back(std::move(c));
        }
        return copy;
    };
    const auto copy = convert(convert, imported.root);
    // Validate with the native schema, then let the full graph resolve Stream templates
    // across module boundaries before any individual runtime wires are installed.
    const auto validated = decode(encode(copy), 0);
    const auto& root = *session.root_;
    auto replacement = std::make_unique<Schematic>(root.bitmapLoader_, root.bitmapBackend_,
                                                   root.textMeasurer_, root.textFiles_, root.systemInfo_);
    for (const auto& child : validated.children) {
        if (!replacement->canPlace(child.node.kind)) invalid("root primitive");
    }
    Session::restoreGraph(*replacement, validated, false, false);
    replacement->rebuildConnectedRuntimes(false, true);
    const auto opened = session.openSchematic(std::move(replacement), beforeReplace);
    if (warnings) *warnings = imported.warnings;
    return opened;
}

QByteArray SchematicFile::read(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error(file.errorString().toStdString());
    const auto data = file.read(maxFileSize + 1);
    if (file.error() != QFileDevice::NoError) throw std::runtime_error(file.errorString().toStdString());
    if (data.isEmpty() || data.size() > maxFileSize) invalid("file size");
    return data;
}

void SchematicFile::write(const QString& path, const QByteArray& data)
{
    if (data.size() > maxFileSize) invalid("file size");
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        throw std::runtime_error(file.errorString().toStdString());
    }
}

void SchematicFile::loadToolbox(Session& session, const QString& path)
{
    if (!QFileInfo::exists(path)) return;
    const auto saved = document(read(path), QStringLiteral("OpenStone Toolbox"));
    auto toolbox = decodeArray<std::vector<Session::ToolboxEntry>>(
        saved["modules"], [](QJsonValue value) {
            const auto object = value.toObject();
            auto content = decode(object["content"].toObject(), 0);
            if (content.node.kind != PrimitiveKind::Module) invalid("saved module");
            return Session::ToolboxEntry{std::move(content), string(object["group"]), object["favorite"].toBool(),
                                         object["id"].toString().toStdString()};
        });
    const auto savedPrimitives = saved["favoritePrimitives"].toArray();
    std::set<PrimitiveKind> favoritePrimitives;
    for (const auto& spec : primitives()) {
        if (savedPrimitives.contains(QString::fromUtf8(primitiveKey(spec)))) favoritePrimitives.insert(spec.kind);
    }
    session.toolbox_ = std::move(toolbox);
    session.favoritePrimitives_ = std::move(favoritePrimitives);
}

void SchematicFile::saveToolbox(const std::vector<Session::ToolboxEntry>& toolbox,
                                 const std::set<PrimitiveKind>& favoritePrimitives, const QString& path)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) throw std::runtime_error("Could not create the toolbox folder");
    const auto modules = encodeArray(toolbox, [](const auto& entry) {
        return QJsonObject{{"group", QString::fromStdString(entry.group)}, {"content", encode(entry.content)},
                           {"favorite", entry.favorite}, {"id", QString::fromStdString(entry.id)}};
    });
    const auto favorites = encodeArray(favoritePrimitives, [](PrimitiveKind kind) {
        return QString::fromUtf8(primitiveKey(primitive(kind)));
    });
    write(path, QJsonDocument(QJsonObject{{"format", "OpenStone Toolbox"}, {"version", 1},
                                         {"modules", modules}, {"favoritePrimitives", favorites}})
                    .toJson(QJsonDocument::Compact));
}

void SchematicFile::addToolboxModule(Session& session, const QString& path, int id, std::string group)
{
    session.saveModuleToToolbox(id, std::move(group));
    try {
        session.toolbox_.back().id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        saveToolbox(session.toolbox_, session.favoritePrimitives_, path);
    } catch (...) {
        session.toolbox_.pop_back();
        throw;
    }
}

void SchematicFile::editToolboxModule(Session& session, const QString& path, std::size_t index, std::string name, std::string group)
{
    auto updated = session.toolbox_;
    auto& entry = updated.at(index);
    if (name.empty()) throw std::invalid_argument("Enter a name for the saved module");
    entry.content.node.moduleName = std::move(name);
    entry.group = std::move(group);
    saveToolbox(updated, session.favoritePrimitives_, path);
    session.toolbox_ = std::move(updated);
}

void SchematicFile::removeToolboxModule(Session& session, const QString& path, std::size_t index)
{
    auto updated = session.toolbox_;
    static_cast<void>(updated.at(index));
    updated.erase(updated.begin() + index);
    saveToolbox(updated, session.favoritePrimitives_, path);
    session.toolbox_ = std::move(updated);
}

void SchematicFile::ensureToolboxModuleId(Session& session, const QString& path, std::size_t index)
{
    auto& id = session.toolbox_.at(index).id;
    if (!id.empty()) return;
    id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    try {
        saveToolbox(session.toolbox_, session.favoritePrimitives_, path);
    } catch (...) {
        id.clear();
        throw;
    }
}

QStringList SchematicFile::loadRecentlyUsed(const QString& path)
{
    if (!QFileInfo::exists(path)) return {};
    const auto saved = document(read(path), QStringLiteral("OpenStone Recently Used"));
    QStringList entries;
    for (const auto& value : array(saved["entries"], "recently used entries")) {
        const auto key = value.toString();
        if (!key.isEmpty() && !entries.contains(key)) entries.append(key);
        if (entries.size() == recentlyUsedLimit) break;
    }
    return entries;
}

void SchematicFile::saveRecentlyUsed(const QString& path, const QStringList& entries)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) throw std::runtime_error("Could not create the library folder");
    const QJsonObject data{{"format", "OpenStone Recently Used"}, {"version", 1},
                           {"entries", QJsonArray::fromStringList(entries)}};
    write(path, QJsonDocument(data).toJson(QJsonDocument::Compact));
}

void SchematicFile::setToolboxFavorite(Session& session, const QString& path, std::size_t index, bool favorite)
{
    auto& entry = session.toolbox_.at(index);
    if (entry.favorite == favorite) return;
    entry.favorite = favorite;
    try {
        saveToolbox(session.toolbox_, session.favoritePrimitives_, path);
    } catch (...) {
        entry.favorite = !favorite;
        throw;
    }
}

void SchematicFile::setPrimitiveFavorite(Session& session, const QString& path, PrimitiveKind kind, bool favorite)
{
    if (session.primitiveFavorite(kind) == favorite) return;
    auto updated = session.favoritePrimitives_;
    if (favorite) updated.insert(kind);
    else updated.erase(kind);
    saveToolbox(session.toolbox_, updated, path);
    session.favoritePrimitives_ = std::move(updated);
}

}
