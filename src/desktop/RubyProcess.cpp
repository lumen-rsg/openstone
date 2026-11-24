#include "RubyProcess.hpp"

#include "PanelTextLayout.hpp"

#include "editor/MidiMessage.hpp"
#include "editor/PluginHost.hpp"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace openstone::desktop {
namespace {

QString executable()
{
    const auto configured = qEnvironmentVariable("OPENSTONE_RUBY_EXECUTABLE");
    return QStandardPaths::findExecutable(configured.isEmpty() ? QStringLiteral("ruby") : configured);
}

QJsonValue encode(const std::optional<green::Value>& value)
{
    if (!value) return QJsonValue::Null;
    return std::visit([](const auto& item) -> QJsonValue {
        using Item = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Item, std::string>) {
            return QString::fromStdString(item);
        } else if constexpr (std::is_same_v<Item, green::MidiMessage>) {
            if (item.bytes.empty()) return QJsonValue::Null;
            if (!validMidiMessage(item)) throw std::invalid_argument("Invalid MIDI message on Ruby input");
            const auto bytes = QByteArray::fromRawData(reinterpret_cast<const char*>(item.bytes.data()), item.bytes.size());
            return QJsonObject{{"midi", QString::fromLatin1(bytes.toBase64())}};
        } else if constexpr (std::is_same_v<Item, yellow::Color>) {
            return QJsonObject{{"color", static_cast<qint64>(item.argb)}};
        } else if constexpr (std::is_arithmetic_v<Item>) {
            return item;
        } else if constexpr (std::is_same_v<Item, green::IntArray> ||
                             std::is_same_v<Item, green::FloatArray> ||
                             std::is_same_v<Item, green::StringArray>) {
            QJsonArray array;
            for (const auto& element : item) {
                if constexpr (std::is_same_v<typename Item::value_type, std::string>) {
                    array.append(QString::fromStdString(element));
                } else {
                    array.append(element);
                }
            }
            return array;
        } else {
            throw std::invalid_argument("Unsupported Ruby input type");
        }
    }, *value);
}

using green::Type;

yellow::Color decodeColor(QJsonValue value);

green::Value decodeScalar(QJsonValue value, Type type)
{
    switch (type) {
    case Type::Boolean:
        if (value.isBool()) return value.toBool();
        break;
    case Type::Int:
        if (const auto number = value.toInteger(std::numeric_limits<qint64>::max());
            number >= std::numeric_limits<std::int32_t>::min() && number <= std::numeric_limits<std::int32_t>::max()) {
            return static_cast<std::int32_t>(number);
        }
        break;
    case Type::Float:
        if (value.isDouble()) return green::normalizeFloat(static_cast<float>(value.toDouble()));
        break;
    case Type::Color:
        return decodeColor(value.toObject()["color"]);
    case Type::String:
        if (value.isString()) return value.toString().toStdString();
        break;
    default: break;
    }
    throw std::invalid_argument("Ruby output does not match its connector type");
}

template<class Array>
Array decodeArray(QJsonValue value, Type type)
{
    if (!value.isArray()) throw std::invalid_argument("Ruby output needs an array");
    Array result;
    const auto array = value.toArray();
    result.reserve(array.size());
    for (const auto& item : array) result.push_back(std::get<typename Array::value_type>(decodeScalar(item, type)));
    return result;
}

green::MidiMessage decodeMidi(QJsonValue value)
{
    if (value.isString()) {
        const auto bytes = QByteArray::fromBase64Encoding(value.toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        if (bytes && !bytes.decoded.isEmpty()) {
            const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.decoded.constData());
            green::MidiMessage message;
            message.bytes.assign(data, data + bytes.decoded.size());
            if (validMidiMessage(message)) return message;
        }
    }
    throw std::invalid_argument("Ruby returned an invalid MIDI message");
}

std::optional<green::Value> decode(QJsonValue value, Type type)
{
    switch (type) {
    case Type::Trigger: return std::nullopt;
    case Type::MIDI: return decodeMidi(value);
    case Type::FloatArray: return decodeArray<green::FloatArray>(value, Type::Float);
    case Type::IntArray: return decodeArray<green::IntArray>(value, Type::Int);
    case Type::StringArray: return decodeArray<green::StringArray>(value, Type::String);
    default: return decodeScalar(value, type);
    }
}

float drawingCoordinate(QJsonValue value)
{
    const double number = value.toDouble(std::numeric_limits<double>::quiet_NaN());
    if (!std::isfinite(number) || std::abs(number) > std::numeric_limits<float>::max() / 256.0) {
        throw std::invalid_argument("Ruby drawing coordinate is out of range");
    }
    return static_cast<float>(number);
}

yellow::Area decodeDrawingArea(QJsonValue value)
{
    const auto area = value.toArray();
    if (area.size() != 4) throw std::invalid_argument("Invalid Ruby drawing area");
    return {drawingCoordinate(area.at(0)), drawingCoordinate(area.at(1)),
            drawingCoordinate(area.at(2)), drawingCoordinate(area.at(3))};
}

yellow::Font decodeFont(const QJsonArray& fields)
{
    yellow::Font font{.face = fields.at(0).toString().toStdString(), .size = drawingCoordinate(fields.at(1))};
    if (font.size <= 0) throw std::invalid_argument("Invalid Ruby font size");
    const auto style = fields.at(2);
    if (style.isString()) {
        font.style = yellow::parseFontStyle(style.toString().toStdString());
    } else {
        const auto mask = style.toInt();
        font.style = {bool(mask & 1), bool(mask & 2), bool(mask & 4), bool(mask & 8)};
    }
    return font;
}

RubyEditOptions decodeEditOptions(QJsonValue value)
{
    const auto fields = value.toObject();
    RubyEditOptions options{.area = decodeDrawingArea(fields["area"]),
                            .text = fields["text"].toString().toStdString()};
    if (options.area.width <= 0 || options.area.height <= 0) {
        throw std::invalid_argument("Invalid Ruby edit bounds");
    }
    if (const auto font = fields["font"].toArray(); font.size() == 3) {
        options.font = decodeFont(font);
    }
    if (const auto color = fields["color"]; color.isDouble()) options.textColor.argb = color.toInteger();
    if (const auto color = fields["back"]; color.isDouble()) options.background.argb = color.toInteger();
    options.multiline = fields["multiline"].toBool();
    return options;
}

RubyDropListOptions decodeDropListOptions(QJsonValue value)
{
    const auto fields = value.toObject();
    const auto position = fields["position"].toArray();
    if (position.size() != 2) throw std::invalid_argument("Invalid Ruby drop list position");
    return {{drawingCoordinate(position.at(0)), drawingCoordinate(position.at(1))},
            fields["items"].toString().toStdString(), fields["checked"].toInt(-1),
            fields["style"].toString().toStdString(), fields["disabled"].toInt(-1)};
}

yellow::Color decodeColor(QJsonValue value)
{
    const auto argb = value.toInteger(-1);
    if (argb < 0 || argb > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("Invalid Ruby drawing color");
    }
    return {static_cast<std::uint32_t>(argb)};
}

yellow::TextCommand decodeText(const QJsonArray& fields)
{
    if (fields.size() != 4 || fields.at(2).toArray().size() != 3) {
        throw std::invalid_argument("Invalid Ruby text layout");
    }
    const auto location = fields.at(0).toArray();
    const bool point = location.size() == 2;
    const auto area = point ? yellow::Area{drawingCoordinate(location.at(0)), drawingCoordinate(location.at(1))}
                            : decodeDrawingArea(fields.at(0));

    yellow::StringFormat format;
    if (const auto description = fields.at(3).toArray(); description.size() == 3) {
        const auto horizontal = description.at(0).toString();
        const auto vertical = description.at(1).toString();
        const auto flags = description.at(2).toInt();
        // StringFormat uses GDI+ flag values.
        format.rightToLeft = (flags & 1) != 0;
        format.verticalText = (flags & 2) != 0;
        const bool reverse = format.rightToLeft && !format.verticalText;
        using enum yellow::HorizontalAlignment;
        format.horizontal = horizontal == "center" ? Center : (horizontal == "far") != reverse ? Right : Left;
        using V = yellow::VerticalAlignment;
        format.vertical = vertical == "center" ? V::Center : vertical == "far" ? V::Bottom : V::Top;
        format.showControls = (flags & 32) != 0;
        format.includeTrailingSpaces = (flags & 2048) != 0;
        format.wrap = (flags & 4096) == 0;
        format.wholeLinesOnly = (flags & 8192) != 0;
        format.clip = (flags & 16384) == 0;
    }
    if (point) {
        format.wrap = false;
        format.wholeLinesOnly = false;
        format.clip = false;
    }

    return {.area = area, .text = fields.at(1).toString().toStdString(),
            .font = decodeFont(fields.at(2).toArray()), .format = format, .pointOrigin = point};
}

RubyDrawing decodeDrawing(QJsonValue value, yellow::Area view)
{
    const auto fields = value.toArray();
    if (fields.isEmpty()) throw std::invalid_argument("Invalid Ruby drawing command");
    const auto shape = fields.at(0).toString();
    if (shape == "text") {
        if (fields.size() != 3) throw std::invalid_argument("Invalid Ruby text drawing");
        auto text = decodeText(fields.at(1).toArray());
        text.color = decodeColor(fields.at(2));
        return text;
    }
    if (fields.size() != (shape == "roundRect" ? 5 : 4)) {
        throw std::invalid_argument("Invalid Ruby drawing command");
    }
    const auto color = decodeColor(fields.at(2));
    yellow::PaintTool tool = yellow::SolidBrush{color};
    if (!fields.at(3).isNull()) {
        const auto pen = fields.at(3).toArray();
        if (pen.size() != 2) throw std::invalid_argument("Invalid Ruby pen");
        const auto width = drawingCoordinate(pen.at(0));
        if (width < 0) throw std::invalid_argument("Invalid Ruby pen width");
        tool = yellow::PenStroke{color, width, yellow::parseLineStyle(pen.at(1).toString().toStdString())};
    }
    if (shape == "lines" || shape == "polygon") {
        const bool closed = shape == "polygon";
        const auto points = fields.at(1).toArray();
        if (points.size() < (closed ? 3 : 2) || (!closed && !std::holds_alternative<yellow::PenStroke>(tool))) {
            throw std::invalid_argument("Invalid Ruby path");
        }
        yellow::GraphDrawCommand path{.area = {0, 0, view.width, view.height}, .tool = std::move(tool),
                                      .closed = closed, .clipToArea = false};
        path.vertices.reserve(points.size());
        for (const auto& value : points) {
            const auto point = value.toArray();
            if (point.size() != 2) throw std::invalid_argument("Invalid Ruby drawing point");
            path.vertices.push_back({drawingCoordinate(point.at(0)), drawingCoordinate(point.at(1)),
                                     path.vertices.empty()});
        }
        return path;
    }

    const auto area = decodeDrawingArea(fields.at(1));
    if (shape == "ellipse") return yellow::DrawCommand{yellow::ShapeKind::Ellipse, area, std::move(tool)};
    if (shape == "rectangle" || shape == "roundRect") {
        yellow::DrawCommand rectangle{yellow::ShapeKind::Rect, area, std::move(tool)};
        if (shape == "roundRect") {
            const auto limit = std::min(std::abs(area.width), std::abs(area.height)) / 2;
            rectangle.cornerRadius = std::clamp(drawingCoordinate(fields.at(4)), 0.0F, limit);
        }
        return rectangle;
    }
    throw std::invalid_argument("Unknown Ruby drawing shape");
}

QString schematicDirectory(const SchematicLocation& location)
{
    if (location.pluginHost && !location.pluginHost->pluginFolder.empty()) {
        return QDir(QString::fromStdString(location.pluginHost->pluginFolder)).absolutePath();
    }
    if (location.filePath.empty()) return {};
    return QFileInfo(QString::fromStdString(location.filePath)).absolutePath();
}

class Interpreter {
public:
    explicit Interpreter(RubySoundBackend sound) : sound_(std::move(sound)) {}

    ~Interpreter()
    {
        stop();
    }

    qint64 newInstance()
    {
        if (process_.state() != QProcess::Running) {
            const auto ruby = executable();
            if (ruby.isEmpty()) throw std::runtime_error("Ruby executable not found. Install Ruby or set OPENSTONE_RUBY_EXECUTABLE.");
            QFile script(QStringLiteral(":/openstone/ruby_node.rb"));
            if (!script.open(QIODevice::ReadOnly)) throw std::runtime_error("Could not open the Ruby node support script");
            process_.start(ruby, {QStringLiteral("-E"), QStringLiteral("UTF-8:UTF-8"), QStringLiteral("-e"),
                                  QString::fromUtf8(script.readAll())});
            if (!process_.waitForStarted(timeoutMs)) throw std::runtime_error(process_.errorString().toStdString());
            ++generation;
        }
        retain();
        return ++nextId_;
    }

    QJsonValue retainBitmap(const yellow::Bitmap& bitmap)
    {
        const auto id = ++nextBitmap_;
        bitmaps_.emplace(id, bitmap.copy());
        return QJsonObject{{"bitmap", id}, {"width", bitmap.width()}, {"height", bitmap.height()}};
    }

    yellow::Bitmap bitmap(qint64 id) const
    {
        const auto found = bitmaps_.find(id);
        if (found == bitmaps_.end()) throw std::invalid_argument("Ruby returned an unknown Bitmap");
        return found->second;
    }

    QJsonObject call(qint64 id, QJsonObject request, const SchematicLocation& location, const RubyContext& context)
    {
        if (process_.state() != QProcess::Running) throw std::runtime_error("Ruby interpreter stopped. Run the node again.");
        request.insert("id", id);
        request.insert("schematicLocation", schematicDirectory(location));
        send(request);
        QElapsedTimer elapsed;
        elapsed.start();
        qint64 hostTime = 0;
        for (;;) {
            while (!process_.canReadLine()) {
                const auto remaining = timeoutMs + hostTime - elapsed.elapsed();
                if (process_.bytesAvailable() > maxReplyBytes || remaining <= 0) {
                    stop();
                    throw std::runtime_error("Ruby execution exceeded the time or output limit");
                }
                if (!process_.waitForReadyRead(static_cast<int>(remaining)) && !process_.canReadLine()) {
                    const bool running = process_.state() == QProcess::Running;
                    stop();
                    throw std::runtime_error(running ? "Ruby execution timed out" : "Ruby interpreter stopped. Run the node again.");
                }
            }
            const auto line = process_.readLine(maxReplyBytes + 1);
            process_.readAllStandardError();
            const auto response = QJsonDocument::fromJson(line).object();
            if (response["id"].toInteger(-1) != id) {
                stop();
                throw std::runtime_error("Ruby interpreter returned an invalid reply");
            }
            const auto hostCall = response["hostCall"].toString();
            if (hostCall == "isKeyPressed" || hostCall == "getViewSize") {
                QJsonValue result;
                if (hostCall == "isKeyPressed") {
                    result = keyIsPressed(context.keys, response["key"].toString().toStdString());
                } else {
                    const auto port = response["port"].toInt(-1);
                    if (port >= 0 && std::cmp_less(port, context.viewSizes.size()) && context.viewSizes[port]) {
                        const auto size = *context.viewSizes[port];
                        result = QJsonArray{size.x, size.y};
                    }
                }
                send({{"id", id}, {"hostReply", hostCall}, {"result", result}});
                continue;
            }
            if (hostCall == "measureString") {
                replyToMeasureString(id, response);
                continue;
            }
            if (response["hostCall"] == "playSound") {
                const auto started = elapsed.elapsed();
                replyToSound(id, response, location.filePath);
                // Intentional waits in the playback backend do not spend the Ruby execution budget.
                hostTime += elapsed.elapsed() - started;
                continue;
            }
            if (response.contains("error")) throw std::runtime_error(response["error"].toString().toStdString());
            if (!bitmaps_.empty()) {
                const auto live = response["bitmaps"].toArray();
                std::set<qint64> references;
                for (const auto& token : live) references.insert(token.toInteger(-1));
                std::erase_if(bitmaps_, [&](const auto& entry) { return !references.contains(entry.first); });
            }
            return response;
        }
    }

    void stopSound(qint64 id) noexcept
    {
        if (soundOwner_ != id) return;
        soundOwner_.reset();
        try {
            sound_(std::nullopt, RubySoundMode::Async);
        } catch (...) {
            // Disposing a node or a failed interpreter must succeed even if the device is gone.
        }
    }

    void retain() { ++leases_; }

    void release(qint64 id, std::uint64_t leaseGeneration, const char* operation)
    {
        if (std::string_view(operation) == "drop") stopSound(id);
        if (--leases_ == 0) stop();
        else if (generation == leaseGeneration && process_.state() == QProcess::Running)
            send({{"call", QLatin1String(operation)}, {"id", id}});
    }

    std::uint64_t generation = 0;

private:
    qint64 nextBitmap_ = 0;
    std::map<qint64, yellow::Bitmap> bitmaps_;

    void replyToMeasureString(qint64 id, const QJsonObject& request)
    {
        QJsonObject reply{{"id", id}, {"hostReply", "measureString"}};
        try {
            if (!qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
                throw std::runtime_error("Text measurement requires a Qt GUI application");
            }
            const auto text = decodeText(request["text"].toArray());
            const auto measured = measureTextLayout(text.text, text.area, text.font, text.format, text.pointOrigin);
            const auto& area = measured.bounds;
            reply.insert("result", QJsonObject{{"bounds", QJsonArray{area.x, area.y, area.width, area.height}},
                {"width", area.width}, {"height", area.height}, {"chars", measured.chars}, {"lines", measured.lines}});
        } catch (const std::exception& error) {
            reply.insert("error", QString::fromUtf8(error.what()));
        }
        send(reply);
    }

    void replyToSound(qint64 id, const QJsonObject& request, const std::string& projectFile)
    {
        QJsonObject reply{{"id", id}, {"hostReply", "playSound"}};
        try {
            const auto value = request["file"];
            if (value.isNull()) {
                if (soundOwner_) stopSound(*soundOwner_);
            } else {
                if (!sound_) throw std::runtime_error("Sound playback is unavailable in this host");
                auto file = value.toString();
                if (file.isEmpty() || file.contains(QChar::Null)) throw std::invalid_argument("Choose a sound file");
                file.replace(u'\\', u'/');
                const auto base = projectFile.empty() ? QDir::current()
                    : QFileInfo(QString::fromStdString(projectFile)).absoluteDir();
                const auto name = request["mode"].toString();
                const auto mode = name == "sync" ? RubySoundMode::Sync
                    : name == "loop" ? RubySoundMode::Loop : RubySoundMode::Async;
                sound_(QDir::cleanPath(base.absoluteFilePath(file)).toStdString(), mode);
                soundOwner_ = mode == RubySoundMode::Sync ? std::nullopt : std::optional{id};
            }
        } catch (const std::exception& error) {
            reply.insert("error", QString::fromUtf8(error.what()));
        }
        send(reply);
    }

    void send(const QJsonObject& request)
    {
        process_.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
    }

    void stop()
    {
        bitmaps_.clear();
        if (soundOwner_) stopSound(*soundOwner_);
        if (process_.state() == QProcess::NotRunning) return;
        process_.kill();
        process_.waitForFinished(timeoutMs);
    }

    static constexpr int timeoutMs = 1000;
    static constexpr qsizetype maxReplyBytes = 16 * 1024 * 1024;
    QProcess process_;
    RubySoundBackend sound_;
    std::optional<qint64> soundOwner_;
    qint64 nextId_ = 0;
    std::size_t leases_ = 0;
};

struct RemoteRubyObject final : green::RubyObjectReference {
    RemoteRubyObject(std::shared_ptr<Interpreter> owner, std::uint64_t generation, qint64 id)
        : owner(std::move(owner)), generation(generation), id(id)
    {
        this->owner->retain();
    }

    ~RemoteRubyObject() override { owner->release(id, generation, "release"); }

    const std::shared_ptr<Interpreter> owner;
    const std::uint64_t generation;
    const qint64 id;
};

QJsonArray labels(const std::vector<RubyPort>& ports)
{
    QJsonArray result;
    for (const auto& port : ports) result.append(QString::fromStdString(port.name));
    return result;
}

class Instance final : public RubyInstance {
public:
    Instance(std::shared_ptr<Interpreter> interpreter, RubyPorts ports, std::shared_ptr<const SchematicLocation> location)
        : interpreter_(std::move(interpreter)), ports_(std::move(ports)), location_(std::move(location)),
          id_(interpreter_->newInstance()),
          generation_(interpreter_->generation) {}

    ~Instance() override { interpreter_->release(id_, generation_, "drop"); }

    void stopSound() noexcept override { interpreter_->stopSound(id_); }

    RubyReply start(std::string_view source, RubyInputs inputs, double time,
                    const std::optional<std::string>& savedState) override
    {
        QJsonArray values;
        for (const auto& value : inputs) values.append(encodeInput(value));
        return invoke({{"call", "start"}, {"source", QString::fromUtf8(source)}, {"inputs", labels(ports_.inputs)},
                       {"outputs", labels(ports_.outputs)}, {"time", time}, {"values", values},
                       {"state", savedState ? QJsonValue{QString::fromStdString(*savedState)} : QJsonValue{QJsonValue::Null}},
                       {"triggers", flags(ports_.outputs, Type::Trigger)}, {"inputTriggers", flags(ports_.inputs, Type::Trigger)},
                       {"viewInputs", flags(ports_.inputs, Type::View)},
                       {"midiInputs", flags(ports_.inputs, Type::MIDI)}, {"midiOutputs", flags(ports_.outputs, Type::MIDI)},
                       {"rubyInputs", flags(ports_.inputs, Type::RubyValue)}, {"rubyOutputs", flags(ports_.outputs, Type::RubyValue)},
                       {"floatInputs", flags(ports_.inputs, Type::Float)}, {"floatArrayInputs", flags(ports_.inputs, Type::FloatArray)},
                       {"floatOutputs", flags(ports_.outputs, Type::Float)}, {"floatArrayOutputs", flags(ports_.outputs, Type::FloatArray)}});
    }

    std::optional<std::string> saveState(double time) override
    {
        const auto value = request({{"call", "saveState"}, {"time", time}}).value("state");
        if (value.isNull()) return std::nullopt;
        if (!value.isString() || value.toString().isEmpty()) throw std::runtime_error("Ruby returned an invalid state");
        return value.toString().toStdString();
    }

    std::vector<RubyDrawing> draw(std::size_t input, yellow::Area bounds, double time) override
    {
        const auto reply = request({{"call", "draw"}, {"port", static_cast<int>(input)}, {"time", time},
                                    {"width", bounds.width}, {"height", bounds.height}});
        const auto encoded = reply["drawings"].toArray();
        std::vector<RubyDrawing> commands;
        commands.reserve(encoded.size());
        for (const auto& value : encoded) {
            const auto fields = value.toArray();
            if (!fields.isEmpty() && fields.at(0) == "bitmap") {
                if (fields.size() != 4) throw std::invalid_argument("Invalid Ruby Bitmap drawing");
                auto bitmap = interpreter_->bitmap(fields.at(1).toInteger(-1));
                if (bitmap.empty()) continue;
                const yellow::Area source{0, 0, static_cast<float>(bitmap.width()), static_cast<float>(bitmap.height())};
                const auto alpha = fields.at(3).toInt(-1);
                if (alpha < 0 || alpha > 255) throw std::invalid_argument("Invalid Ruby Bitmap alpha");
                commands.push_back(yellow::BitmapDrawCommand{std::move(bitmap), source, decodeDrawingArea(fields.at(2)),
                                                              {}, static_cast<std::uint8_t>(alpha)});
            } else {
                commands.push_back(decodeDrawing(value, bounds));
            }
        }
        return commands;
    }

    std::optional<yellow::MouseCursor> mouseHit(int input, green::Point position, double time,
                                                bool queryCursor) override
    {
        const auto reply = request({{"call", "mouseHit"}, {"port", input}, {"time", time},
                                    {"x", position.x}, {"y", position.y}, {"queryCursor", queryCursor}});
        if (!reply["hit"].toBool()) return std::nullopt;
        return yellow::parseMouseCursor(reply["cursor"].toString().toStdString()).value_or(yellow::MouseCursor::HandPointer);
    }

    std::optional<RubyReply> mouseEvent(int input, std::string_view callback, green::Point position, double time) override
    {
        const auto reply = request({{"call", "mouseEvent"}, {"port", input}, {"time", time},
                                    {"x", position.x}, {"y", position.y}, {"callback", QString::fromUtf8(callback)}});
        if (!reply["called"].toBool()) return std::nullopt;
        return decodeReply(reply);
    }

    std::optional<RubyReply> editEvent(bool finished, const green::RubyValue& id,
                                       std::string_view text, double time) override
    {
        const auto reply = request({{"call", "editEvent"}, {"finished", finished},
                                    {"editId", encodeInput(green::Value{id})}, {"text", QString::fromUtf8(text)}, {"time", time}});
        if (!reply["called"].toBool()) return std::nullopt;
        return decodeReply(reply);
    }

    std::optional<RubyReply> dropListFinished(const green::RubyValue& id, int index,
                                              std::string_view name, double time) override
    {
        const auto reply = request({{"call", "dropListFinished"}, {"listId", encodeInput(green::Value{id})},
                                    {"index", index}, {"name", QString::fromUtf8(name)}, {"time", time}});
        if (!reply["called"].toBool()) return std::nullopt;
        return decodeReply(reply);
    }

    std::optional<RubyReply> messageBoxFinished(const green::RubyValue& id, std::string_view result,
                                                double time) override
    {
        const auto reply = request({{"call", "messageBoxFinished"}, {"messageId", encodeInput(green::Value{id})},
                                    {"result", QString::fromUtf8(result)}, {"time", time}});
        if (!reply["called"].toBool()) return std::nullopt;
        return decodeReply(reply);
    }

    RubyReply event(std::size_t port, const std::optional<green::Value>& value, double time) override
    {
        return invoke({{"call", "event"}, {"port", static_cast<int>(port)}, {"value", encodeInput(value)}, {"time", time}});
    }

    RubyReply method(std::string_view name, const std::optional<green::Value>& arguments, double time) override
    {
        return invoke({{"call", "method"}, {"method", QString::fromUtf8(name)}, {"arguments", encodeInput(arguments)}, {"time", time}});
    }

    void storeOutput(std::size_t port, const std::optional<green::Value>& value) override
    {
        invoke({{"call", "storeOutput"}, {"port", static_cast<int>(port)}, {"value", encodeInput(value)}});
    }

private:
    static QJsonArray flags(std::span<const RubyPort> ports, Type type)
    {
        QJsonArray result;
        for (const auto& port : ports) result.append(port.type == type);
        return result;
    }

    QJsonValue encodeInput(const std::optional<green::Value>& value) const
    {
        const auto* ruby = value ? std::get_if<green::RubyValue>(&*value) : nullptr;
        if (!ruby) {
            if (const auto* bitmap = value ? std::get_if<yellow::Bitmap>(&*value) : nullptr) {
                return interpreter_->retainBitmap(*bitmap);
            }
            return encode(value);
        }
        if (!ruby->object) return QJsonValue::Null;
        const auto* object = dynamic_cast<const RemoteRubyObject*>(ruby->object.get());
        if (!object || object->owner != interpreter_) throw std::invalid_argument("Ruby Value belongs to another interpreter");
        // After a worker restart, receiver nodes can start before their sources re-emit.
        if (object->generation != generation_) return QJsonValue::Null;
        return QJsonObject{{"rubyObject", object->id}};
    }

    QJsonObject request(QJsonObject message)
    {
        if (generation_ != interpreter_->generation) throw std::runtime_error("Ruby interpreter restarted. Run the node again.");
        return interpreter_->call(id_, std::move(message), *location_, context_);
    }

    RubyReply invoke(QJsonObject message)
    {
        return decodeReply(request(std::move(message)));
    }

    RubyReply decodeReply(const QJsonObject& reply)
    {
        // Adopt every reference first so a later typed output error also releases the Ruby objects.
        std::map<qint64, green::RubyValue> objects;
        for (const auto& value : reply["objects"].toArray()) {
            const auto id = value.toInteger(-1);
            objects.emplace(id, green::RubyValue{std::make_shared<RemoteRubyObject>(interpreter_, generation_, id)});
        }
        RubyReply result{reply["result"].toString().toStdString(), {}, reply["savesState"].toBool(),
                         reply["drawsView"].toBool(), reply["handlesMouse"].toBool()};
        if (const auto capture = reply["captureMouse"]; capture.isBool()) result.captureMouse = capture.toBool();
        for (const auto& item : reply["actions"].toArray()) {
            using enum RubyActionKind;
            const auto fields = item.toArray();
            const auto kind = fields.at(0).toString();
            if (kind == "clear") {
                result.actions.push_back({ClearEvents});
                continue;
            }
            if (kind == "showCursor") {
                if (fields.size() != 2 || !fields.at(1).isBool()) {
                    throw std::invalid_argument("Ruby returned an invalid cursor visibility request");
                }
                result.actions.push_back({.kind = ShowCursor, .value = green::Value{fields.at(1).toBool()}});
                continue;
            }
            if (kind == "messageBox") {
                if (fields.size() != 4 || !fields.at(3).isObject()) {
                    throw std::invalid_argument("Ruby returned an invalid message box request");
                }
                const auto options = fields.at(3).toObject();
                result.actions.push_back({.kind = MessageBox,
                                          .value = green::Value{objects.at(fields.at(2).toInteger(-1))},
                                          .messageBox = RubyMessageBoxOptions{options["text"].toString().toStdString(),
                                                                              options["title"].toString().toStdString(),
                                                                              options["type"].toString().toStdString()}});
                continue;
            }
            if (kind == "redraw" || kind == "createEdit" || kind == "createDropList") {
                const bool redraw = kind == "redraw";
                const auto port = fields.at(1).toInt(-1);
                if (fields.size() != (redraw ? 3 : 4) || port < 0 || std::cmp_greater_equal(port, ports_.inputs.size()) ||
                    ports_.inputs[port].type != Type::View) {
                    throw std::invalid_argument("Ruby returned an invalid View request");
                }
                const auto value = fields.at(2);
                if (redraw) {
                    result.actions.push_back({.kind = Redraw, .port = static_cast<std::size_t>(port),
                                              .area = value.isNull() ? std::nullopt : std::optional{decodeDrawingArea(value)}});
                } else if (kind == "createEdit") {
                    result.actions.push_back({.kind = CreateEdit, .port = static_cast<std::size_t>(port),
                                              .value = green::Value{objects.at(value.toInteger(-1))},
                                              .edit = decodeEditOptions(fields.at(3))});
                } else {
                    result.actions.push_back({.kind = CreateDropList, .port = static_cast<std::size_t>(port),
                                              .value = green::Value{objects.at(value.toInteger(-1))},
                                              .dropList = decodeDropListOptions(fields.at(3))});
                }
                continue;
            }
            if (fields.size() != 4) throw std::invalid_argument("Ruby returned an invalid event");
            const auto stamp = fields.at(3);
            std::optional<double> time;
            if (stamp.isDouble() && std::isfinite(stamp.toDouble())) time = stamp.toDouble();
            else if (!stamp.isNull()) throw std::invalid_argument("Ruby event time must be finite");
            const auto value = fields.at(2);
            if (kind == "method" && time) {
                result.actions.push_back({Method, 0, green::Value{objects.at(value.toInteger(-1))},
                                          time, fields.at(1).toString().toStdString()});
                continue;
            }
            if (kind != "output" && kind != "input") throw std::invalid_argument("Ruby returned an invalid event kind");
            const auto& ports = kind == "output" ? ports_.outputs : ports_.inputs;
            const auto port = fields.at(1).toInt(-1);
            if (port < 0 || std::cmp_greater_equal(port, ports.size())) {
                throw std::invalid_argument("Ruby returned an invalid event port");
            }
            const auto type = ports[port].type;
            auto decoded = type == Type::RubyValue ? std::optional<green::Value>{objects.at(value.toInteger(-1))}
                         : type == Type::Bitmap ? std::optional<green::Value>{
                               interpreter_->bitmap(value.toObject()["bitmap"].toInteger(-1))}
                         : decode(value, type);
            result.actions.push_back({kind == "output" ? Output : Input, static_cast<std::size_t>(port), std::move(decoded), time});
        }
        return result;
    }

    std::shared_ptr<Interpreter> interpreter_;
    RubyPorts ports_;
    std::shared_ptr<const SchematicLocation> location_;
    qint64 id_;
    std::uint64_t generation_;
};

}

bool rubyAvailable()
{
    return !executable().isEmpty();
}

RubyBackend makeRubyBackend(RubySoundBackend sound)
{
    return [interpreter = std::make_shared<Interpreter>(std::move(sound))](
            const RubyPorts& ports, std::shared_ptr<const SchematicLocation> location) {
        return std::make_shared<Instance>(interpreter, ports, std::move(location));
    };
}

}
