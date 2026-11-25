#include "SchematicModel.hpp"
#include "PanelShapePath.hpp"
#include "RubyProcess.hpp"
#include "RubySound.hpp"

#include "AudioDevice.hpp"
#include "MidiDevice.hpp"
#include "SerialDevice.hpp"
#include "SchematicFile.hpp"
#include "FlowStoneFile.hpp"
#include "SystemInfo.hpp"
#include "HttpTransfer.hpp"
#include "Network.hpp"
#include "Media.hpp"
#include "SettingsStore.hpp"
#include "BitmapIO.hpp"
#include "PanelBitmap.hpp"
#include "PanelTextLayout.hpp"
#include "PanelBrushFill.hpp"
#include "ViewRegion.hpp"

#include <QBuffer>
#include <QCache>
#include <QDateTime>
#include <QCursor>
#include <QDir>
#include <QDrag>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QQmlEngine>
#include <QMimeData>
#include <QPointF>
#include <QPermissions>
#include <QPolygonF>
#include <QQuickWindow>
#include <QRectF>
#include <QSaveFile>
#include <QString>
#include <QStringList>
#include <QSettings>
#include <QNetworkAccessManager>
#include <QStandardPaths>
#include <QTextStream>
#include <QVector4D>
#include <rhi/qshader.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <ranges>
#include <stdexcept>
#include <type_traits>

Q_DECLARE_METATYPE(openstone::yellow::MousePolygon)

namespace {

using namespace openstone;
using green::Type;
using green::Value;

constexpr double panelScale = 12.0;

QString primitiveHistoryKey(PrimitiveKind kind)
{
    return QStringLiteral("primitive/") + QString::fromUtf8(desktop::SchematicFile::primitiveKey(primitive(kind)));
}

QString moduleHistoryKey(const std::string& id)
{
    return id.empty() ? QString{} : QStringLiteral("module/") + QString::fromStdString(id);
}

QString lifecycleErrorText(Session& session)
{
    if (auto failure = session.takeLifecycleError()) {
        try {
            std::rethrow_exception(failure);
        } catch (const std::exception& error) {
            return QString::fromUtf8(error.what());
        }
    }
    return {};
}

QRectF panelRect(const yellow::Area& area)
{
    return {area.x, area.y, area.width, area.height};
}

QString nodeKey(std::span<const int> path, int id)
{
    QString key;
    for (const int module : path) {
        key += QString::number(module) + '/';
    }
    return key + QString::number(id);
}

QString viewKey(const yellow::View& view)
{
    return nodeKey(view.schematicPath, view.panel);
}

std::vector<int> nodePath(const QVariantList& list)
{
    std::vector<int> path;
    path.reserve(list.size());
    for (const auto& id : list) {
        path.push_back(id.toInt());
    }
    return path;
}

struct NodeGeometry {
    double width;
    double height;
    double firstInputY;
    double inputSpacing;
    double firstOutputY;
    double outputSpacing = 0;

    double outputY(int index) const { return firstOutputY + index * outputSpacing; }
};

bool isAreaOperator(PrimitiveKind kind)
{
    return kind == PrimitiveKind::AreaUnion || kind == PrimitiveKind::PointInsideArea ||
           kind == PrimitiveKind::PointInsidePolygon;
}

NodeGeometry geometry(const Primitive& spec, std::size_t channels = 0)
{
    if (spec.kind == PrimitiveKind::MultilineString) {
        return {190, 125, 47, 0, 47};
    }
    if (spec.kind == PrimitiveKind::Color) {
        return {88, 58, 39, 0, 39};
    }
    if (spec.kind == PrimitiveKind::Svg) {
        return {208, 191, 37, 27, 37, 27};
    }
    switch (spec.behavior) {
    case PrimitiveBehavior::RubyCode: return {520, 296, 42, 24, 42, 24};
    case PrimitiveBehavior::DspCode: return {520, 276, 42, 24, 42, 24};
    case PrimitiveBehavior::PolyBridge: return {142, 29.0 + 27.0 * spec.inputs.size(), 37, 27, 37};
    case PrimitiveBehavior::StreamMath:
        return spec.inputs.size() == 1 ? NodeGeometry{89, 40, 20, 0, 20} : NodeGeometry{70, 56, 14, 28, 28};
    case PrimitiveBehavior::StreamConversion: return {125, 56, 37, 0, 37};
    case PrimitiveBehavior::SequencerTrack: return {155, 58, 37, 0, 37};
    case PrimitiveBehavior::MediaSource:
    case PrimitiveBehavior::NetworkTransfer:
    case PrimitiveBehavior::ParseSfz:
    case PrimitiveBehavior::MouseCaptureControl:
        return {178, 31.0 + 27.0 * std::max(spec.inputs.size(), spec.outputs.size()), 37, 27, 37, 27};
    case PrimitiveBehavior::MatrixRouting: return {154, 85, 37, 27, 64, 27};
    case PrimitiveBehavior::StreamRouting:
        return {125, 29.0 + 27.0 * std::max(spec.inputs.size(), spec.outputs.size()), 37, 27, 37, 27};
    case PrimitiveBehavior::CaptureSignal: return {166, 60.0 + 27.0 * spec.inputs.size(), 37, 27, 37, 27};
    case PrimitiveBehavior::SampleMono: return {166, 114, 37, 27, 37};
    case PrimitiveBehavior::AudioEndpoint: case PrimitiveBehavior::AudioControl:
        return {140, 29.0 + 27.0 * std::max({spec.inputs.size(), spec.outputs.size(), channels}), 37, 27, 37, 27};
    case PrimitiveBehavior::MidiDevice:
        if (spec.kind == PrimitiveKind::MidiIn || spec.kind == PrimitiveKind::MidiOut) {
            return {178, 98, 37, 27, 37, 27};
        }
        return {195, 29.0 + 27.0 * std::max(spec.inputs.size(), spec.outputs.size()), 37, 27, 37, 27};
    case PrimitiveBehavior::UnaryMath: return {76, 40, 20, 0, 20};
    case PrimitiveBehavior::NoteConversion: return {120, 57, 37, 0, 37};
    case PrimitiveBehavior::GeneralProcessing:
        return {146, 29.0 + 27.0 * std::max(spec.inputs.size(), spec.outputs.size()), 37, 27, 37, 27};
    case PrimitiveBehavior::StateMachineLogic:
    case PrimitiveBehavior::GreenLogic:
        return {144, 29.0 + 27.0 * spec.inputs.size(), 37, 27, 37, 27};
    case PrimitiveBehavior::Compare:
    case PrimitiveBehavior::Binary: return {54, 56, 14, 28, 28};
    case PrimitiveBehavior::MakePoint: return {100, 81, 37, 27, 37};
    case PrimitiveBehavior::SplitPoint: return {130, 81, 37, 27, 37, 27};
    case PrimitiveBehavior::SystemSettings: return {224, 61, 37, 0, 37};
    case PrimitiveBehavior::SystemAction:
    case PrimitiveBehavior::HttpTransfer:
    case PrimitiveBehavior::ParseColorMatrix:
    case PrimitiveBehavior::PenInfo:
    case PrimitiveBehavior::EmbedFonts:
    case PrimitiveBehavior::PersistSettings:
        return {172, 29.0 + 27.0 * spec.inputs.size(), 37, 27, 37, 27};
    case PrimitiveBehavior::IndexSelector: return {194, 143, 37, 27, 37, 27};
    case PrimitiveBehavior::BuildArray: return {132, 58, 37, 27, 37};
    case PrimitiveBehavior::GetArrayElement: return {130, 110, 37, 27, 37, 27};
    case PrimitiveBehavior::AppendArrays: return {130, 83, 37, 27, 37};
    case PrimitiveBehavior::TransformArray: return {152, 29.0 + 27.0 * spec.inputs.size(), 37, 27, 37};
    case PrimitiveBehavior::ProcessArray:
        if (spec.kind == PrimitiveKind::ArraySplitter) {
            return {152, 29.0 + 27.0 * std::max<std::size_t>(1, channels), 37, 27, 37, 27};
        }
        return {206, 29.0 + 27.0 * std::max(spec.inputs.size(), spec.outputs.size()), 37, 27, 37, 27};
    case PrimitiveBehavior::StoreArray:
    case PrimitiveBehavior::ProcessString:
    case PrimitiveBehavior::TextFile:
    case PrimitiveBehavior::FileUtility:
    case PrimitiveBehavior::SerialDevice:
    case PrimitiveBehavior::WaveFile:
    case PrimitiveBehavior::SystemInfo:
    case PrimitiveBehavior::SampleTime:
    case PrimitiveBehavior::Timer:
    case PrimitiveBehavior::MemoryOperation:
    case PrimitiveBehavior::MidiOperation:
    case PrimitiveBehavior::MidiStream:
    case PrimitiveBehavior::NotesFilter:
    case PrimitiveBehavior::VoiceRouting:
    case PrimitiveBehavior::VoiceSignal:
    case PrimitiveBehavior::HostMidi:
    case PrimitiveBehavior::PresetUtility:
    case PrimitiveBehavior::ManagePresets:
    case PrimitiveBehavior::PresetParameter:
    case PrimitiveBehavior::PluginHost:
        return {172, 29.0 + 27.0 * std::max(spec.inputs.size(), spec.outputs.size()), 37, 27, 37, 27};
    case PrimitiveBehavior::MeasureText: return {138, 137, 37, 27, 37};
    case PrimitiveBehavior::AreaGeometry:
        if (isAreaOperator(spec.kind)) {
            return {54, 56, 14, 28, 28};
        }
        return {120, 29.0 + 27.0 * std::max(spec.inputs.size(), spec.outputs.size()), 37, 27, 37, 27};
    case PrimitiveBehavior::SplitColor:
    case PrimitiveBehavior::MakeColor: return {120, 137, 37, 27, 37, 27};
    case PrimitiveBehavior::ColorMatrixSet: return {145, 135, 37, 27, 37};
    case PrimitiveBehavior::ApplyColorMatrix: return {156, 81, 37, 27, 37};
    case PrimitiveBehavior::MakePen: return {115, 108, 37, 27, 37};
    case PrimitiveBehavior::MakeSolid: return {100, 56, 37, 0, 37};
    case PrimitiveBehavior::MakeBrush: return {128, 29.0 + 27.0 * spec.inputs.size(), 37, 27, 37};
    case PrimitiveBehavior::DrawView: {
        double width = 112;
        if (spec.kind == PrimitiveKind::BitmapDrawTransform) {
            width = 174;
        } else if (spec.kind == PrimitiveKind::DrawPointLines) {
            width = 148;
        } else if (spec.kind == PrimitiveKind::DrawPolygon) {
            width = 128;
        }
        return {width, 29.0 + 27.0 * spec.inputs.size(), 37, 27, 37};
    }
    case PrimitiveBehavior::TransformView:
    case PrimitiveBehavior::ViewAttributes:
    case PrimitiveBehavior::SetGuiArea:
    case PrimitiveBehavior::ViewCache:
    case PrimitiveBehavior::ViewEffect:
    case PrimitiveBehavior::PanelUtility:
    case PrimitiveBehavior::PresentationControl:
    case PrimitiveBehavior::KeyboardInput:
    case PrimitiveBehavior::Redraw:
        return {128, 29.0 + 27.0 * std::max(spec.inputs.size(), spec.outputs.size()), 37, 27, 37, 27};
    case PrimitiveBehavior::Window: return {166, 164, 37, 27, 37, 27};
    case PrimitiveBehavior::ViewLoop: return {168, 164, 37, 27, 37, 27};
    case PrimitiveBehavior::DropList: return {210, 218, 37, 27, 37, 27};
    case PrimitiveBehavior::EditControl: return {210, 299, 37, 27, 37, 27};
    case PrimitiveBehavior::PopupList: return {220, 245, 37, 27, 37, 27};
    case PrimitiveBehavior::DropFiles: return {220, 164, 37, 27, 37, 27};
    case PrimitiveBehavior::DragFiles: return {285, 110, 37, 27, 37, 27};
    case PrimitiveBehavior::MouseArea: return {170, 81, 37, 27, 37};
    case PrimitiveBehavior::MouseInfo: return {136, 108, 37, 27, 37};
    case PrimitiveBehavior::ShowCursor: return {124, 83, 37, 27, 0};
    case PrimitiveBehavior::MouseDrag:
        return spec.kind == PrimitiveKind::MouseDragAccumulate ? NodeGeometry{208, 164, 37, 27, 37, 27}
                                                               : NodeGeometry{208, 135, 37, 27, 37, 27};
    case PrimitiveBehavior::ParseMouseMessage:
    case PrimitiveBehavior::MouseMove: return {106, 56, 37, 0, 37};
    case PrimitiveBehavior::MakeFont: return {108, 135, 37, 27, 37};
    case PrimitiveBehavior::FontAttributes: return {130, 108, 37, 0, 37, 27};
    case PrimitiveBehavior::MakeStringFormat: return {126, 108, 37, 27, 37};
    case PrimitiveBehavior::LoadBitmap: return {156, 141, 37, 27, 37, 27};
    case PrimitiveBehavior::LoadBitmapArray:
        return spec.kind == PrimitiveKind::BitmapArrayFromFile ? NodeGeometry{178, 195, 37, 27, 37, 27}
                                                                : NodeGeometry{206, 165, 37, 27, 37, 27};
    case PrimitiveBehavior::BitmapOperation:
    case PrimitiveBehavior::BitmapCanvas:
    case PrimitiveBehavior::BitmapMetadata:
        return {spec.kind == PrimitiveKind::BitmapSampleAndHold || spec.kind == PrimitiveKind::BitmapGetAt ? 184.0 : 144.0,
                30.0 + 27.0 * std::max(spec.inputs.size(), spec.outputs.size()), 37, 27, 37, 27};
    case PrimitiveBehavior::CreateBus: return {160, 56.0 + 27.0 * channels, 37, 27, 37};
    case PrimitiveBehavior::ExtractBus: return {188, std::max(83.0, 29.0 + 27.0 * channels), 37, 27, 37, 27};
    case PrimitiveBehavior::StringFormatAttributes: return {126, 108, 37, 0, 37, 27};
    case PrimitiveBehavior::ModuleGui:
        return spec.kind == PrimitiveKind::ModulePropertiesGui ? NodeGeometry{120, 83, 37, 27, 37}
                                                               : NodeGeometry{110, 113, 37, 27, 37};
    case PrimitiveBehavior::Module: return {164, 52, 50, 27, 50, 27};
    case PrimitiveBehavior::ModuleInput:
    case PrimitiveBehavior::PropertyInput:
    case PrimitiveBehavior::PropertyOutput:
    case PrimitiveBehavior::ModuleOutput: return {182, 88, 41, 0, 41};
    case PrimitiveBehavior::SendWireless:
        return spec.inputs.size() == 2 ? NodeGeometry{124, 82, 37, 27, 0}
                                                        : NodeGeometry{174, 48, 24, 0, 0};
    case PrimitiveBehavior::ReceiveWireless:
        return spec.kind == PrimitiveKind::WirelessReceiver ? NodeGeometry{124, 58, 37, 0, 37}
                                                            : NodeGeometry{174, 48, 0, 0, 24};
    case PrimitiveBehavior::TriggerButton: return {123, 69, 0, 0, 47};
    case PrimitiveBehavior::Tick: return {158, 107, 47, 0, 47};
    case PrimitiveBehavior::Counter:
        return spec.kind == PrimitiveKind::BasicCounter ? NodeGeometry{157, 83, 37, 27, 47}
                                                        : NodeGeometry{157, 69, 47, 0, 47};
    case PrimitiveBehavior::Blocker: return {178, 58, 39, 0, 39};
    case PrimitiveBehavior::Switch: return {142, 81, 37, 27, 37};
    case PrimitiveBehavior::ProcessTrigger: {
        double width = 144;
        if (spec.kind == PrimitiveKind::Loop) width = 180;
        else if (spec.kind == PrimitiveKind::CounterAdvanced) width = 188;
        return {width, 29.0 + 27.0 * std::max(spec.inputs.size(), spec.outputs.size()), 37, 27, 37, 27};
    }
    case PrimitiveBehavior::ValueBox: return {152, 69, 47, 0, 47};
    }
    throw std::logic_error("Unknown primitive layout");
}

double modulePortHeight(const SchematicNode& entry)
{
    return 27.0 * std::max(entry.moduleInputs.size(), entry.moduleOutputs.size());
}

ModulePanelKind modulePanelKind(bool properties)
{
    return properties ? ModulePanelKind::Properties : ModulePanelKind::Front;
}

std::optional<ModulePanel> visibleModulePanel(const Schematic& module, const SchematicNode& entry)
{
    if (entry.propertiesVisibility != PropertiesVisibility::Closed) {
        if (auto properties = module.modulePanel(ModulePanelKind::Properties)) {
            return properties;
        }
    }
    return entry.panelHidden ? std::nullopt : module.frontPanel();
}

std::size_t visibleSplitterOutputs(const Schematic& graph, int id)
{
    try {
        return graph.arraySplitterOutputCount(id);
    } catch (const std::exception&) {
        std::size_t count = 1;
        for (const auto& link : graph.links()) {
            if (link.source == id) count = std::max(count, static_cast<std::size_t>(link.output) + 1);
        }
        return count;
    }
}

NodeGeometry nodeGeometry(const Schematic& graph, const SchematicNode& entry,
                          std::size_t splitterOutputs = 0)
{
    const auto channels = entry.kind == PrimitiveKind::ArraySplitter
        ? (splitterOutputs == 0 ? visibleSplitterOutputs(graph, entry.id) : splitterOutputs)
        : entry.channelNames.size();
    auto layout = geometry(primitive(entry.kind), channels);
    if (hasExpandableInputs(entry.kind)) {
        layout.height = 31.0 + 27.0 * entry.inputTriggers.size();
    }
    if (hasExpandableOutputs(entry.kind)) {
        const auto outputHeight = entry.kind == PrimitiveKind::Matrix
            ? layout.outputY(entry.outputTriggers.size() - 1) + 21.0
            : 31.0 + 27.0 * entry.outputTriggers.size();
        layout.height = std::max(layout.height, outputHeight);
    }
    if (entry.dspCode) {
        const auto ports = std::max(entry.dspCode->program.inputs().size(), entry.dspCode->program.outputs().size());
        layout.height = std::max(layout.height, 76.0 + 24.0 * ports);
    }
    if (entry.rubyCode) {
        const auto ports = std::max(entry.inputTriggers.size(), entry.outputTriggers.size());
        layout.width = entry.rubyCode->boxSize.width;
        layout.height = std::max<double>(entry.rubyCode->boxSize.height, 76.0 + 24.0 * ports);
    }
    if (const auto* child = graph.module(entry.id)) {
        const double portHeight = modulePortHeight(entry);
        if (portHeight > 0) {
            layout.width = 216;
            layout.height += portHeight;
        }
        if (const auto panel = visibleModulePanel(*child, entry)) {
            layout.width = std::max({layout.width, 210.0, 26 + panel->view.bounds.width * panelScale});
            layout.height = 58 + portHeight + panel->view.bounds.height * panelScale;
        }
    }
    return layout;
}

QString qtext(std::string_view text)
{
    return QString::fromUtf8(text.data(), text.size());
}

std::string readFile(std::string_view path, std::optional<std::size_t> limit)
{
    const auto filename = qtext(path);
    if (!QFileInfo(filename).isFile()) {
        throw std::runtime_error(limit ? "Preset Load requires an existing file" : "Text Load requires an existing file");
    }
    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error(file.errorString().toStdString());
    }
    const auto contents = limit ? file.read(static_cast<qint64>(*limit) + 1) : file.readAll();
    if (file.error() != QFileDevice::NoError) {
        throw std::runtime_error(file.errorString().toStdString());
    }
    if (limit && std::cmp_greater(contents.size(), *limit)) throw std::runtime_error("Preset file exceeds the size limit");
    return contents.toStdString();
}

std::string readTextFile(std::string_view path)
{
    return readFile(path, std::nullopt);
}

std::string readPresetFile(std::string_view path, std::size_t limit)
{
    return readFile(path, limit);
}

void saveTextFile(std::string_view path, std::string_view text)
{
    QSaveFile file(qtext(path));
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(text.data(), static_cast<qint64>(text.size())) != static_cast<qint64>(text.size()) ||
        !file.commit()) {
        throw std::runtime_error(file.errorString().toStdString());
    }
}

QString bitmapReadout(const yellow::Bitmap& bitmap, QVariantMap& result)
{
    if (bitmap.empty()) {
        return QStringLiteral("No image loaded");
    }
    const QImage pixels(bitmap.rgba().data(), bitmap.width(), bitmap.height(),
                        static_cast<qsizetype>(bitmap.width()) * 4, QImage::Format_RGBA8888);
    const auto bounds = pixels.size().boundedTo(QSize(224, 112));
    const auto size = pixels.size().scaled(bounds, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
    const auto preview = pixels.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QByteArray png;
    QBuffer buffer(&png);
    if (buffer.open(QIODevice::WriteOnly) && preview.save(&buffer, "PNG")) {
        result.insert("bitmapPreview", QUrl::fromEncoded("data:image/png;base64," + png.toBase64()));
    }
    return QStringLiteral("%1 × %2 px").arg(bitmap.width()).arg(bitmap.height());
}

QVariantList previewDashes(yellow::LineStyle style)
{
    switch (style) {
    case yellow::LineStyle::Dash: return {4, 2};
    case yellow::LineStyle::Dot: return {1, 2};
    case yellow::LineStyle::DashDot: return {4, 2, 1, 2};
    case yellow::LineStyle::DashDotDot: return {4, 2, 1, 2, 1, 2};
    case yellow::LineStyle::Solid: return {};
    }
    return {};
}

QString paintToolReadout(const yellow::PaintTool& tool, QVariantMap& result)
{
    if (const auto* pen = std::get_if<yellow::PenStroke>(&tool)) {
        const auto color = QColor::fromRgba(pen->color.argb);
        result.insert("stroke", QVariantMap{
            {"color", color}, {"width", pen->width}, {"dashes", previewDashes(pen->style)},
        });
        const auto width = pen->width == 0.0F ? QStringLiteral("Hairline")
            : QStringLiteral("%1 grid units").arg(pen->width, 0, 'g', 6);
        return QStringLiteral("Pen · %1\n%2\n%3").arg(qtext(yellow::lineStyleName(pen->style)), width,
                                                        color.name(QColor::HexArgb).toUpper());
    }
    if (const auto* solid = std::get_if<yellow::SolidBrush>(&tool)) {
        const auto color = QColor::fromRgba(solid->color.argb);
        result.insert("swatch", color);
        return QStringLiteral("Solid brush\n%1").arg(color.name(QColor::HexArgb).toUpper());
    }
    if (const auto* linear = std::get_if<yellow::LinearGradientBrush>(&tool)) {
        return QStringLiteral("Linear gradient · %1°\n%2")
            .arg(linear->angle).arg(qtext(yellow::brushWrapName(linear->wrap)));
    }
    if (const auto* radial = std::get_if<yellow::RadialGradientBrush>(&tool)) {
        if (radial->boundary) return QStringLiteral("Custom radial gradient · %1 points · %2 colors")
            .arg(radial->boundary->points.size()).arg(radial->surroundColors.size());
        return radial->shape == yellow::RadialShape::Rectangle
            ? QStringLiteral("Rectangular radial gradient") : QStringLiteral("Elliptical radial gradient");
    }
    if (const auto* hatch = std::get_if<yellow::HatchBrush>(&tool)) {
        result.insert("swatch", QColor::fromRgba(hatch->foreground.argb));
        return QStringLiteral("Hatch brush · %1").arg(qtext(yellow::hatchStyleName(hatch->style)));
    }
    const auto& texture = std::get<yellow::TextureBrush>(tool);
    return texture.bitmap.empty() ? QStringLiteral("Texture brush\nEmpty bitmap")
        : QStringLiteral("Texture brush · %1\n%2 × %3 px").arg(qtext(yellow::brushWrapName(texture.wrap)))
              .arg(texture.bitmap.width()).arg(texture.bitmap.height());
}

QString fontReadout(const yellow::Font& font, QVariantMap& result)
{
    const auto face = qtext(font.face);
    const auto alternate = qtext(font.alternateFace);
    const auto installed = QFontDatabase::families();
    auto family = installed.contains(face, Qt::CaseInsensitive) ? face
        : installed.contains(alternate, Qt::CaseInsensitive) ? alternate
        : QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
    if (!installed.contains(face, Qt::CaseInsensitive) && !installed.contains(alternate, Qt::CaseInsensitive)) {
        for (const auto& fallback : font.fallbackFaces) {
            if (installed.contains(qtext(fallback), Qt::CaseInsensitive)) { family = qtext(fallback); break; }
        }
    }
    const auto pixels = static_cast<int>(std::round(std::clamp(font.size * 24.0, 12.0, 32.0)));
    result.insert("fontSample", QVariantMap{
        {"family", family}, {"pixels", pixels}, {"bold", font.style.bold}, {"italic", font.style.italic},
        {"underline", font.style.underline}, {"strikeout", font.style.strikeout},
    });
    auto text = QStringLiteral("%1 · %2 grid units\n%3")
        .arg(face, QString::number(font.size, 'g', 6), qtext(yellow::fontStyleName(font.style)));
    if (!alternate.isEmpty()) {
        text += QStringLiteral("\nAltF · %1").arg(alternate);
    }
    for (const auto& fallback : font.fallbackFaces) text += QStringLiteral("\nAltF · %1").arg(qtext(fallback));
    if (family != face) {
        text += QStringLiteral("\nPreview · %1").arg(family);
    }
    return text;
}

QString mouseEventName(yellow::MouseEventKind kind)
{
    switch (kind) {
    case yellow::MouseEventKind::None: return QStringLiteral("No mouse event");
    case yellow::MouseEventKind::Move: return QStringLiteral("Move");
    case yellow::MouseEventKind::Drag: return QStringLiteral("Drag");
    case yellow::MouseEventKind::ButtonDown: return QStringLiteral("Button down");
    case yellow::MouseEventKind::ButtonUp: return QStringLiteral("Button up");
    case yellow::MouseEventKind::Click: return QStringLiteral("Click");
    case yellow::MouseEventKind::DoubleClick: return QStringLiteral("Double-click");
    case yellow::MouseEventKind::Wheel: return QStringLiteral("Wheel");
    }
    return QStringLiteral("Unknown mouse event");
}

QString mouseButtonName(yellow::MouseButton button)
{
    switch (button) {
    case yellow::MouseButton::None: return {};
    case yellow::MouseButton::Left: return QStringLiteral("Left");
    case yellow::MouseButton::Middle: return QStringLiteral("Middle");
    case yellow::MouseButton::Right: return QStringLiteral("Right");
    case yellow::MouseButton::Back: return QStringLiteral("Back");
    case yellow::MouseButton::Forward: return QStringLiteral("Forward");
    }
    return QStringLiteral("Unknown button");
}

QString mouseMessageText(const yellow::MouseMessage& message)
{
    auto text = mouseEventName(message.kind);
    if (message.kind == yellow::MouseEventKind::None) {
        return text;
    }
    if (message.button != yellow::MouseButton::None) {
        text += QStringLiteral(" · ") + mouseButtonName(message.button);
    }
    text += QStringLiteral("\nPosition %1, %2").arg(message.position.x, 0, 'g', 9)
                .arg(message.position.y, 0, 'g', 9);
    if (message.kind == yellow::MouseEventKind::Wheel) {
        text += QStringLiteral("\nWheel delta %1, %2 (⅛°)").arg(message.wheelDelta.x).arg(message.wheelDelta.y);
    }
    return text;
}

QString valueText(const Value& value)
{
    if (const auto* text = std::get_if<std::string>(&value)) {
        return qtext(*text);
    }
    if (const auto* number = std::get_if<float>(&value)) {
        return QString::number(*number, 'g', 9);
    }
    if (const auto* point = std::get_if<green::Point>(&value)) {
        return QStringLiteral("%1, %2").arg(point->x, 0, 'g', 9).arg(point->y, 0, 'g', 9);
    }
    if (const auto* area = std::get_if<yellow::Area>(&value)) {
        return QStringLiteral("%1, %2, %3, %4").arg(area->x, 0, 'g', 9).arg(area->y, 0, 'g', 9)
                   .arg(area->width, 0, 'g', 9).arg(area->height, 0, 'g', 9);
    }
    if (const auto* view = std::get_if<yellow::View>(&value)) {
        if (view->panel == 0) {
            return QStringLiteral("No panel connected");
        }
        auto description = QStringLiteral("Panel %1 · %2 × %3").arg(view->panel)
                               .arg(view->bounds.width).arg(view->bounds.height);
        if (view->parentPanel != 0) {
            description += QStringLiteral("\nParent panel %1").arg(view->parentPanel);
        }
        return description;
    }
    if (const auto* mouse = std::get_if<yellow::MouseMessage>(&value)) {
        return mouseMessageText(*mouse);
    }
    if (const auto* message = std::get_if<green::MidiMessage>(&value)) {
        if (message->bytes.empty()) {
            return QStringLiteral("No MIDI message");
        }
        QStringList bytes;
        for (const auto byte : message->bytes | std::views::take(16)) {
            bytes.push_back(QStringLiteral("%1").arg(static_cast<unsigned>(byte), 2, 16, u'0').toUpper());
        }
        if (message->bytes.size() > 16) {
            bytes.push_back(QStringLiteral("…"));
        }
        return bytes.join(u' ');
    }
    if (const auto* notes = std::get_if<green::Notes>(&value)) {
        return QStringLiteral("%1 note events").arg(notes->events.size());
    }
    if (const auto* voices = std::get_if<green::Voices>(&value)) {
        return QStringLiteral("%1 voice events").arg(voices->events.size());
    }
    if (const auto* preset = std::get_if<green::Preset>(&value)) {
        if (const auto* program = std::get_if<green::PresetProgramChange>(preset)) {
            return QStringLiteral("Program %1").arg(program->program);
        }
        if (const auto* parameter = std::get_if<green::PresetParameterChange>(preset)) {
            return QStringLiteral("Parameter %1 = %2").arg(parameter->parameter).arg(parameter->value, 0, 'g', 9);
        }
        return QStringLiteral("No preset change");
    }
    if (const auto* color = std::get_if<yellow::Color>(&value)) {
        return QColor::fromRgba(color->argb).name(QColor::HexArgb).toUpper();
    }
    if (const auto* format = std::get_if<yellow::StringFormat>(&value)) {
        return QStringLiteral("%1 · %2\n%3 · %4\n%5")
            .arg(qtext(yellow::alignmentName(format->horizontal)),
                 qtext(yellow::alignmentName(format->vertical)),
                 format->wrap ? QStringLiteral("Wrap") : QStringLiteral("No wrap"),
                 format->rightToLeft ? QStringLiteral("Right to left") : QStringLiteral("Left to right"),
                 format->verticalText ? QStringLiteral("Vertical text") : QStringLiteral("Horizontal text"));
    }
    if (const auto* array = std::get_if<yellow::BitmapArray>(&value)) {
        const auto& entries = array->entries();
        if (entries.empty()) {
            return QStringLiteral("No images loaded");
        }
        QStringList lines{QStringLiteral("%1 loaded · %2 slots").arg(entries.size()).arg(array->size())};
        constexpr std::size_t visibleEntries = 3;
        std::size_t shown = 0;
        for (const auto& [index, image] : entries) {
            if (shown == visibleEntries) {
                lines.append(QStringLiteral("%1 more").arg(entries.size() - shown));
                break;
            }
            lines.append(QStringLiteral("[%1]  %2 × %3 px").arg(index).arg(image.width()).arg(image.height()));
            ++shown;
        }
        return lines.join(u'\n');
    }
    if (const auto* memory = std::get_if<green::Memory>(&value)) {
        return QStringLiteral("%1 bytes · %2 channels").arg(memory->size()).arg(memory->channels());
    }
    if (const auto* array = std::get_if<green::MemoryArray>(&value)) {
        return QStringLiteral("%1 buffers").arg(array->entries().size());
    }
    if (const auto* ruby = std::get_if<green::RubyValue>(&value)) {
        return ruby->object ? QStringLiteral("Ruby object") : QStringLiteral("Waiting for a Ruby value");
    }
    if (const auto convert = green::findValueConverter(green::typeOf(value), Type::String)) {
        return qtext(std::get<std::string>(convert(value)));
    }
    return QStringLiteral("Preview unavailable");
}

struct AlphaConnector {
    Type type;
    std::string_view label;
    std::string_view palette;
    std::string_view shape;
    std::string_view glyph;
};

constexpr std::array alphaConnectors{
    AlphaConnector{Type::Mem, "Mem", "memory", "memory", ""},
    AlphaConnector{Type::MemArray, "MemArray", "memory", "memoryArray", ""},
    AlphaConnector{Type::Stream, "Stream", "stream", "circle", "∿"},
    AlphaConnector{Type::Poly, "Poly", "poly", "circle", "∿"},
    AlphaConnector{Type::Mono, "Mono", "mono", "circle", "•"},
    AlphaConnector{Type::Mono4, "Mono4", "mono", "circle", "∷"},
    AlphaConnector{Type::StreamBoolean, "StreamBoolean", "stream", "circle", "⎍"},
    AlphaConnector{Type::PolyBoolean, "PolyBoolean", "poly", "circle", "⎍"},
    AlphaConnector{Type::MonoBoolean, "MonoBoolean", "mono", "circle", "⎍"},
    AlphaConnector{Type::StreamDouble, "StreamDouble", "stream", "double", "∿"},
    AlphaConnector{Type::PolyDouble, "PolyDouble", "poly", "double", "∿"},
    AlphaConnector{Type::MonoDouble, "MonoDouble", "mono", "double", "•"},
    AlphaConnector{Type::Mono4Double, "Mono4Double", "mono", "double", "∷"},
    AlphaConnector{Type::MIDI, "MIDI", "midi", "circle", "M"},
    AlphaConnector{Type::Notes, "Notes", "midi", "array", "♪"},
    AlphaConnector{Type::Voices, "Voices", "midi", "array", "V"},
    AlphaConnector{Type::Preset, "Preset", "preset", "circle", "P"},
    AlphaConnector{Type::PolyInt, "PolyInt", "poly", "circle", "I"},
    AlphaConnector{Type::RubyValue, "Ruby Value", "ruby", "circle", "V"},
};

const AlphaConnector* alphaConnector(std::optional<Type> type)
{
    if (!type || (!green::isDspType(*type) && !green::isEventType(*type) && !green::isMemoryType(*type))) {
        return nullptr;
    }
    const auto entry = std::ranges::find(alphaConnectors, *type, &AlphaConnector::type);
    return entry == alphaConnectors.end() ? nullptr : &*entry;
}

QVariantMap portDescription(Port port, double centerY)
{
    QString glyph;
    QString type;
    if (!port.type) {
        return {{"title", qtext(port.name)}, {"glyph", ""}, {"type", "Template"},
                {"centerY", centerY}, {"yellow", false}};
    }
    if (const auto* connector = alphaConnector(port.type)) {
        return {{"title", qtext(port.name)}, {"glyph", qtext(connector->glyph)}, {"type", qtext(connector->label)},
                {"centerY", centerY}, {"yellow", false}, {"bus", false},
                {"palette", qtext(connector->palette)}, {"shape", qtext(connector->shape)}};
    }
    switch (*port.type) {
    case Type::Int:     glyph = "I"; type = "Int"; break;
    case Type::Float:   glyph = "F"; type = "Float"; break;
    case Type::Boolean: glyph = "B"; type = "Boolean"; break;
    case Type::String:  glyph = "S"; type = "String"; break;
    case Type::Trigger: glyph = QStringLiteral("↯"); type = "Trigger"; break;
    case Type::Point: glyph = "P"; type = "Point"; break;
    case Type::IntArray: glyph = "I[]"; type = "Int Array"; break;
    case Type::FloatArray: glyph = "F[]"; type = "Float Array"; break;
    case Type::StringArray: glyph = "S[]"; type = "String Array"; break;
    case Type::PointArray: glyph = "P[]"; type = "Point Array"; break;
    case Type::View: glyph = "V"; type = "View"; break;
    case Type::Area: glyph = "A"; type = "Area"; break;
    case Type::MouseMessage: glyph = "M"; type = "Mouse Messages"; break;
    case Type::Color: glyph = "C"; type = "Color"; break;
    case Type::ColorMatrix: glyph = QStringLiteral("Ⅱ"); type = "Color Matrix"; break;
    case Type::PaintTool: glyph = QStringLiteral("▯"); type = "Paint Tool"; break;
    case Type::Font: glyph = QStringLiteral("ƒ"); type = "Font"; break;
    case Type::StringFormat: glyph = QStringLiteral("¶"); type = "String Format"; break;
    case Type::Bitmap: glyph = QStringLiteral("□"); type = "Bitmap"; break;
    case Type::BitmapArray: glyph = QStringLiteral("□[]"); type = "Bitmap Array"; break;
    case Type::Bus: glyph = "B"; type = "Bus"; break;
    default: throw std::logic_error("Unknown editor port type");
    }
    return {{"title", qtext(port.name)}, {"glyph", glyph}, {"type", type}, {"centerY", centerY},
            {"yellow", green::isYellowType(*port.type)}, {"bus", *port.type == Type::Bus}};
}

QVariantList templateTypeGroups(PrimitiveKind kind, std::optional<Type> typeOverride,
                                std::optional<bool> rubyOutput = {})
{
    QVariantList groups;
    if (!rubyOutput) groups.push_back(QVariant::fromValue(QVariantList{QVariantMap{
        {"label", "Template"}, {"glyph", ""}, {"type", 0}, {"shape", "template"},
        {"selected", !typeOverride},
    }}));

    const auto addGroup = [&](auto types) {
        QVariantList choices;
        for (auto type : types) {
            if (!(rubyOutput ? supportsRubyPort(type, *rubyOutput) : Schematic::supportsTemplateType(kind, type))) {
                continue;
            }
            auto connector = portDescription({"", type}, 0);
            auto glyph = connector.value("glyph").toString();
            const bool array = glyph.endsWith(QStringLiteral("[]"));
            if (array) {
                glyph.chop(2);
            }
            if (type == Type::Point || type == Type::PointArray) {
                glyph = QStringLiteral("⊙");
            }
            const auto shape = connector.value("shape", array ? "array" : type == Type::Trigger ? "trigger" : "scalar");
            choices.push_back(QVariantMap{
                {"label", connector.value("type")}, {"glyph", glyph}, {"type", static_cast<int>(type)},
                {"shape", shape}, {"palette", connector.value("palette")},
                {"yellow", green::isYellowType(type)},
                {"bus", type == Type::Bus},
                {"selected", typeOverride == type},
            });
        }
        if (!choices.isEmpty()) {
            groups.push_back(QVariant::fromValue(choices));
        }
    };
    constexpr std::array numeric{Type::Float, Type::Int, Type::Point};
    constexpr std::array numericArrays{Type::FloatArray, Type::IntArray, Type::PointArray};
    constexpr std::array other{Type::String, Type::StringArray, Type::Boolean, Type::Trigger};
    constexpr std::array gui{Type::View, Type::Area, Type::MouseMessage, Type::Color, Type::ColorMatrix,
                             Type::PaintTool, Type::Font, Type::StringFormat, Type::Bitmap, Type::BitmapArray};
    addGroup(numeric);
    addGroup(numericArrays);
    addGroup(other);
    addGroup(gui);
    addGroup(alphaConnectors | std::views::transform(&AlphaConnector::type));
    constexpr std::array bus{Type::Bus};
    addGroup(bus);
    return groups;
}

QVariantList strokePattern(yellow::LineStyle style)
{
    using enum yellow::LineStyle;
    switch (style) {
    case Solid: return {};
    case Dash: return {3, 1};
    case Dot: return {1, 1};
    case DashDot: return {3, 1, 1, 1};
    case DashDotDot: return {3, 1, 1, 1, 1, 1};
    }
    return {};
}

bool usesBrushTexture(const yellow::PaintTool& tool)
{
    return !std::holds_alternative<yellow::PenStroke>(tool) && !std::holds_alternative<yellow::SolidBrush>(tool);
}

QVariantMap drawingDescription(const yellow::DrawCommand& drawing)
{
    using enum yellow::ShapeKind;
    QString kind;
    switch (drawing.shape) {
    case Arc: kind = QStringLiteral("arc"); break;
    case Ellipse: kind = QStringLiteral("ellipse"); break;
    case Line: kind = QStringLiteral("line"); break;
    case Pie: kind = QStringLiteral("pie"); break;
    case Rect: kind = QStringLiteral("rect"); break;
    }
    const auto area = panelRect(drawing.area);
    if (usesBrushTexture(drawing.tool)) {
        const auto bounds = area.normalized();
        return {{"kind", kind}, {"area", bounds}, {"brush", QVariant::fromValue(drawing.tool)},
                {"brushPath", QVariant::fromValue(desktop::shapeFillPath(drawing, bounds))}};
    }
    const auto* pen = std::get_if<yellow::PenStroke>(&drawing.tool);
    const auto color = QColor::fromRgba(pen ? pen->color.argb : std::get<yellow::SolidBrush>(drawing.tool).color.argb);
    return {{"kind", kind}, {"area", drawing.shape == Line ? area : area.normalized()},
            {"stroke", pen ? color : QColor(Qt::transparent)}, {"fill", pen ? QColor(Qt::transparent) : color},
            {"width", pen ? pen->width : 0.0F}, {"dashes", pen ? strokePattern(pen->style) : QVariantList{}},
            {"start", desktop::panelShapeStartAngle(drawing)},
            {"sweep", drawing.shape == Ellipse ? 360.0 : drawing.sweepAngle},
            {"corner", drawing.cornerRadius}};
}

QVariantMap drawingDescription(const yellow::TextCommand& drawing)
{
    auto properties = desktop::panelTextProperties(drawing.text, drawing.font, drawing.format, drawing.renderingHint);
    properties.insert("kind", "text");
    properties.insert("pointOrigin", drawing.pointOrigin);
    properties.insert("area", panelRect(drawing.area).normalized());
    properties.insert("color", QColor::fromRgba(drawing.color.argb));
    return properties;
}

QVariantList panelPath(std::span<const int> ownerPath)
{
    QVariantList path;
    path.reserve(ownerPath.size());
    for (const auto id : ownerPath) {
        path.push_back(id);
    }
    return path;
}

QVariantMap drawingDescription(const yellow::EditControlCommand& drawing)
{
    auto key = nodeKey(drawing.id.ownerPath, drawing.id.node);
    auto font = desktop::panelTypeface(drawing.font);
    if (drawing.rubySession) {
        key += QStringLiteral(":edit:%1").arg(drawing.rubySession);
        font.setBold(drawing.font.style.bold);
        font.setItalic(drawing.font.style.italic);
        font.setUnderline(drawing.font.style.underline);
        font.setStrikeOut(drawing.font.style.strikeout);
    }
    QVariantMap result{{"kind", "edit"}, {"area", panelRect(drawing.area).normalized()},
                       {"key", key}, {"path", panelPath(drawing.id.ownerPath)}, {"node", drawing.id.node},
                       {"font", font}, {"text", qtext(drawing.text)},
                       {"color", QColor::fromRgba(drawing.textColor.argb)}, {"back", QColor::fromRgba(drawing.editBackground.argb)},
                       {"multiline", drawing.multiline}, {"password", drawing.password}};
    if (drawing.rubySession) {
        result.insert("rubySession", QString::number(drawing.rubySession));
        result.insert("fontSize", drawing.font.size);
    }
    return result;
}

QVariantMap drawingDescription(const yellow::PopupListCommand& drawing)
{
    return {{"kind", "popupList"}, {"area", panelRect(drawing.area).normalized()},
            {"path", panelPath(drawing.id.ownerPath)}, {"node", drawing.id.node},
            {"font", desktop::panelTypeface(drawing.font)}, {"text", qtext(drawing.text)}, {"hasOptions", drawing.hasOptions},
            {"color", QColor::fromRgba(drawing.textColor.argb)}, {"back", QColor::fromRgba(drawing.background.argb)}};
}

QVariantMap drawingDescription(const yellow::DropFilesCommand& drawing)
{
    return {{"kind", "dropFiles"}, {"area", panelRect(drawing.area).normalized()},
            {"key", nodeKey(drawing.id.ownerPath, drawing.id.node)}, {"path", panelPath(drawing.id.ownerPath)},
            {"node", drawing.id.node}, {"draggingOver", drawing.draggingOver}};
}

Qt::CursorShape nativeMouseCursor(yellow::MouseCursor cursor)
{
    using enum yellow::MouseCursor;
    switch (cursor) {
    case IBeam: return Qt::IBeamCursor;
    case Arrow:
    case SmallPointer: return Qt::ArrowCursor;
    case Default:
    case Hand:
    case HandPointer: return Qt::PointingHandCursor;
    case Move:
    case ResizeAll: return Qt::SizeAllCursor;
    case PointerAdd: return Qt::DragCopyCursor;
    case PointerDelete:
    case Forbidden: return Qt::ForbiddenCursor;
    case ResizeHorizontal: return Qt::SizeHorCursor;
    case ResizeVertical: return Qt::SizeVerCursor;
    case OpenHand: return Qt::OpenHandCursor;
    case ClosedHand: return Qt::ClosedHandCursor;
    case Cross:
    case Crosshair: return Qt::CrossCursor;
    case Hidden: return Qt::BlankCursor;
    case ResizeNorthEastSouthWest: return Qt::SizeBDiagCursor;
    case ResizeNorthWestSouthEast: return Qt::SizeFDiagCursor;
    case SplitHorizontal: return Qt::SplitHCursor;
    case SplitVertical: return Qt::SplitVCursor;
    case Help: return Qt::WhatsThisCursor;
    case Wait: return Qt::WaitCursor;
    }
    return Qt::ArrowCursor;
}

QVariantMap drawingDescription(const yellow::MouseAreaCommand& drawing)
{
    QVariantMap result{{"kind", "mouseArea"}, {"path", panelPath(drawing.id.ownerPath)},
                       {"node", drawing.id.node}, {"mouseMoves", drawing.mouseMoves}, {"captured", drawing.captured},
                       {"key", nodeKey(drawing.id.ownerPath, drawing.id.node)}, {"tip", qtext(drawing.hover.tooltip)}};
    if (drawing.rubyInput >= 0) result.insert("rubyInput", drawing.rubyInput);
    if (drawing.hover.cursor) {
        result.insert("cursor", static_cast<int>(nativeMouseCursor(*drawing.hover.cursor)));
    }
    if (const auto* polygon = std::get_if<yellow::MousePolygon>(&drawing.region)) {
        result.insert("area", QRectF(QPointF(polygon->minimum.x, polygon->minimum.y),
                                     QPointF(polygon->maximum.x, polygon->maximum.y)));
        result.insert("polygon", QVariant::fromValue(*polygon));
    } else {
        result.insert("area", panelRect(std::get<yellow::Area>(drawing.region)).normalized());
    }
    return result;
}

QVariantMap drawingDescription(const yellow::BitmapDrawCommand& drawing)
{
    return {{"kind", "bitmap"}, {"area", panelRect(drawing.area)}, {"source", panelRect(drawing.sourcePixels)},
            {"bitmap", QVariant::fromValue(drawing.bitmap)}, {"matrix", QVariant::fromValue(drawing.matrix)},
            {"alpha", drawing.alpha / 255.0}};
}

// Use the smaller adjacent slope, and flatten local extrema, so the curve
// passes through the samples without overshooting between them.
double graphTangent(double left, double right)
{
    if (std::signbit(left) != std::signbit(right)) {
        return 0;
    }
    return std::copysign(std::min(std::abs(left), std::abs(right)), left);
}

double graphSlope(const yellow::GraphVertex& from, const yellow::GraphVertex& to)
{
    return (to.y - from.y) / (to.x - from.x);
}

// A centripetal Catmull-Rom control point. The tangent follows the path in
// two dimensions, including when its X coordinate stays fixed or reverses.
QPointF pointCurveControl(const yellow::GraphVertex* previous, const yellow::GraphVertex& from,
                         const yellow::GraphVertex& to)
{
    const QPointF start(from.x, from.y);
    const QPointF outgoing(to.x - from.x, to.y - from.y);
    if (!previous) {
        return start + outgoing / 3.0;
    }
    const QPointF incoming(from.x - previous->x, from.y - previous->y);
    const double before = std::sqrt(std::hypot(incoming.x(), incoming.y()));
    const double after = std::sqrt(std::hypot(outgoing.x(), outgoing.y()));
    const auto tangent = (incoming / before * after + outgoing / after * before) / (before + after);
    return start + tangent * (after / 3.0);
}

void writePointCurve(QTextStream& svg, const yellow::GraphVertex* previous, const yellow::GraphVertex& from,
                     const yellow::GraphVertex& to, const yellow::GraphVertex* next)
{
    const auto first = pointCurveControl(previous, from, to);
    const auto second = pointCurveControl(next, to, from);
    svg << "C " << first.x() << ' ' << first.y() << ' ' << second.x() << ' ' << second.y() << ' '
        << to.x << ' ' << to.y << ' ';
}

template <typename Line, typename Curve>
void polygonEdges(const yellow::GraphDrawCommand& drawing, Line line, Curve curve)
{
    const auto& vertices = drawing.vertices;
    const auto count = vertices.size();
    if (drawing.style != yellow::GraphStyle::PointCurve) {
        for (std::size_t i = 1; i < count; ++i) {
            line(vertices[i]);
        }
        return;
    }
    // Wrap both neighbors so the last edge and its joins use the same curve as the rest.
    for (std::size_t i = 0; i < count; ++i) {
        const auto previous = i == 0 ? count - 1 : i - 1;
        const auto to = (i + 1) % count;
        const auto next = (to + 1) % count;
        curve(vertices[previous], vertices[i], vertices[to], vertices[next]);
    }
}

QPainterPath brushPath(const yellow::GraphDrawCommand& drawing)
{
    QPainterPath path;
    if (drawing.closed) {
        path.setFillRule(Qt::OddEvenFill);
        path.moveTo(drawing.vertices.front().x, drawing.vertices.front().y);
        polygonEdges(drawing,
            [&](const auto& to) { path.lineTo(to.x, to.y); },
            [&](const auto& previous, const auto& from, const auto& to, const auto& next) {
                path.cubicTo(pointCurveControl(&previous, from, to), pointCurveControl(&next, to, from), QPointF(to.x, to.y));
            });
        path.closeSubpath();
    } else {
        path.setFillRule(Qt::WindingFill);
        for (const auto& point : drawing.vertices) {
            path.addEllipse(QPointF(point.x, point.y), drawing.radius, drawing.radius);
        }
    }
    return path.translated(drawing.area.x, drawing.area.y);
}

QString graphPath(const yellow::GraphDrawCommand& drawing)
{
    const auto& vertices = drawing.vertices;
    QString path;
    QTextStream svg(&path);
    svg.setRealNumberPrecision(14);
    if (drawing.closed) {
        svg << "M " << vertices.front().x << ' ' << vertices.front().y << ' ';
        polygonEdges(drawing,
            [&](const auto& to) { svg << "L " << to.x << ' ' << to.y << ' '; },
            [&](const auto& previous, const auto& from, const auto& to, const auto& next) {
                writePointCurve(svg, &previous, from, to, &next);
            });
        svg << 'Z';
    } else if (drawing.style == yellow::GraphStyle::Dots) {
        const double radius = drawing.radius;
        for (const auto& point : vertices) {
            svg << "M " << point.x - radius << ' ' << point.y
                << " A " << radius << ' ' << radius << " 0 0 1 " << point.x + radius << ' ' << point.y
                << " A " << radius << ' ' << radius << " 0 0 1 " << point.x - radius << ' ' << point.y << " Z ";
        }
    } else {
        for (std::size_t i = 0; i < vertices.size(); ++i) {
            const auto& to = vertices[i];
            if (to.startsRun) {
                svg << "M " << to.x << ' ' << to.y << ' ';
            } else if (drawing.style == yellow::GraphStyle::Lines) {
                svg << "L " << to.x << ' ' << to.y << ' ';
            } else if (drawing.style == yellow::GraphStyle::PointCurve) {
                const auto& from = vertices[i - 1];
                const auto* previous = from.startsRun ? nullptr : &vertices[i - 2];
                const auto* next = i + 1 == vertices.size() || vertices[i + 1].startsRun
                                   ? nullptr : &vertices[i + 1];
                writePointCurve(svg, previous, from, to, next);
            } else {
                const auto& from = vertices[i - 1];
                const double dx = (to.x - from.x) / 3;
                const double slope = graphSlope(from, to);
                const double start = from.startsRun ? slope : graphTangent(graphSlope(vertices[i - 2], from), slope);
                const double end = i + 1 == vertices.size() || vertices[i + 1].startsRun
                                   ? slope : graphTangent(slope, graphSlope(to, vertices[i + 1]));
                svg << "C " << from.x + dx << ' ' << from.y + start * dx << ' '
                    << to.x - dx << ' ' << to.y - end * dx << ' ' << to.x << ' ' << to.y << ' ';
            }
        }
    }
    svg.flush();
    return path;
}

QVariantMap drawingDescription(const yellow::GraphDrawCommand& drawing)
{
    if (usesBrushTexture(drawing.tool)) {
        return {{"kind", "graph"}, {"area", panelRect(drawing.area)}, {"brush", QVariant::fromValue(drawing.tool)},
                {"brushPath", QVariant::fromValue(brushPath(drawing))}};
    }
    const auto* pen = std::get_if<yellow::PenStroke>(&drawing.tool);
    const auto color = QColor::fromRgba(pen ? pen->color.argb : std::get<yellow::SolidBrush>(drawing.tool).color.argb);
    return {{"kind", "graph"}, {"area", panelRect(drawing.area)}, {"path", graphPath(drawing)},
            {"closed", drawing.closed}, {"clipToArea", drawing.clipToArea},
            {"stroke", pen ? color : QColor(Qt::transparent)}, {"fill", pen ? QColor(Qt::transparent) : color},
            {"width", pen ? pen->width : 0.0F}, {"dashes", pen ? strokePattern(pen->style) : QVariantList{}}};
}

QVariant clipDescription(const yellow::ViewMapping& mapping, const yellow::Area& bounds)
{
    if (!mapping.clip) {
        return {};
    }
    const auto surface = panelRect(bounds);
    if (const auto* areas = std::get_if<std::vector<yellow::Area>>(&*mapping.clip)) {
        QVariantList rectangles;
        for (const auto area : *areas) {
            const auto visible = surface.intersected(panelRect(area));
            if (!visible.isEmpty()) {
                rectangles.push_back(visible.translated(-surface.topLeft()));
            }
        }
        return rectangles;
    }

    QVariantList pieces;
    QRectF limit;
    QString path;
    QTextStream svg(&path);
    svg.setRealNumberPrecision(12);
    const auto bounded = yellow::combineClip(*mapping.clip, yellow::regionOf(bounds), 0);
    for (const auto& polygon : std::get<std::vector<yellow::ClipPolygon>>(bounded)) {
        QVariantList points;
        QPolygonF geometry;
        for (const auto vertex : polygon) {
            const auto point = QPointF(vertex.x, vertex.y) - surface.topLeft();
            svg << (points.empty() ? "M " : " L ") << point.x() << ' ' << point.y();
            points.push_back(point);
            geometry.push_back(point);
        }
        svg << " Z ";
        pieces.push_back(QVariant::fromValue(points));
        limit = limit.united(geometry.boundingRect());
    }
    svg.flush();
    return pieces.empty() ? QVariant{QVariantList{}}
                          : QVariant{QVariantMap{{"pieces", pieces}, {"path", path}, {"limit", limit}}};
}

struct PanelRender {
    Schematic& graph;
    std::map<std::uint64_t, QVariantMap>& previous;
    std::map<std::uint64_t, QVariantMap> current;
};

QVariantList drawingDescriptions(const std::vector<yellow::PanelDrawing>& drawings, const yellow::Area& bounds,
                                PanelRender* render);

QVariantMap drawingDescription(const yellow::CachedDrawCommand& cache, PanelRender* render)
{
    if (render) {
        if (const auto found = render->current.find(cache.revision); found != render->current.end()) {
            return found->second;
        }
        if (auto previous = render->previous.extract(cache.revision)) {
            return render->current.insert(std::move(previous)).position->second;
        }
    }

    QVariantMap command{{"kind", "viewCache"}, {"area", panelRect(cache.bounds)},
                        {"revision", QString::number(cache.revision)},
                        {"commands", drawingDescriptions(*cache.drawings, cache.bounds, render)}};
    if (render) {
        render->current.emplace(cache.revision, command);
    }
    return command;
}

QUrl viewShaderUrl(QString path)
{
    if (path.startsWith("file:", Qt::CaseInsensitive)) {
        path = QUrl(path).toLocalFile();
    }
    const QFileInfo file(path);
    if (file.suffix().compare("qsb", Qt::CaseInsensitive) != 0 || !file.isFile() || !file.isReadable()) {
        return {};
    }
    const auto filename = file.absoluteFilePath();

    struct PackageCheck {
        QDateTime modified;
        qint64 size;
        bool fragment;
    };
    // Time and Amount can update every frame. Recheck a file only when it changes.
    static QCache<QString, PackageCheck> checkedPackages(32);
    const auto modified = file.lastModified();
    const auto size = file.size();
    auto* checked = checkedPackages.object(filename);
    if (!checked || checked->modified != modified || checked->size != size) {
        QFile input(filename);
        if (!input.open(QIODevice::ReadOnly)) {
            return {};
        }
        const auto shader = QShader::fromSerialized(input.readAll());
        checked = new PackageCheck{modified, size, shader.isValid() && shader.stage() == QShader::FragmentStage};
        checkedPackages.insert(filename, checked);
    }
    return checked->fragment ? QUrl::fromLocalFile(filename) : QUrl{};
}

QVariantMap drawingDescription(const yellow::EffectDrawCommand& command, PanelRender* render)
{
    using namespace yellow;
    // FlowStone's normal zoom uses eight reference pixels per View grid square.
    constexpr double referencePixels = 8;
    const yellow::ViewMapping mapping{.scale = command.viewScale, .rotation = command.rotation};
    const auto uv = [&](double x, double y) {
        const auto point = mapping.mapVector({static_cast<float>(x), static_cast<float>(y)});
        return QPointF(point.x / command.bounds.width, point.y / command.bounds.height);
    };
    const auto tint = [](Color color) {
        const auto [a, r, g, b] = colorToArgb(color);
        return QVector4D(r / 255.0F, g / 255.0F, b / 255.0F, a / 255.0F);
    };
    QVariantMap options;
    const auto kernel = [&](QPointF taps, QPointF sigma, QPointF step) {
        options.insert("taps", taps);
        options.insert("sigma", sigma);
        options.insert("stepX", uv(step.x(), 0));
        options.insert("stepY", uv(0, step.y()));
    };
    const auto softKernel = [&](green::Point radius) {
        const auto count = [&](float r) { return std::min(32.0, std::ceil(r * referencePixels)); };
        const QPointF taps(count(radius.x), count(radius.y));
        kernel(taps, {std::max(taps.x() / 3, 0.5), std::max(taps.y() / 3, 0.5)},
               {radius.x / std::max(1.0, taps.x()), radius.y / std::max(1.0, taps.y())});
    };
    std::visit([&](const auto& settings) {
        using Effect = std::decay_t<decltype(settings)>;
        if constexpr (std::is_same_v<Effect, ViewBlur>) {
            const QPointF taps((settings.size.x - 1) / 2, (settings.size.y - 1) / 2);
            const auto sigma = [](float provided, double radius) {
                return provided > 0 ? provided : 0.3 * std::max(radius - 1, 0.0) + 0.8;
            };
            kernel(taps, {sigma(settings.sigma.x, taps.x()), sigma(settings.sigma.y, taps.y())},
                   {1 / referencePixels, 1 / referencePixels});
            const auto mask = [&](unsigned bit) { return float((settings.channels >> bit) & 1U); };
            options.insert("mode", 0);
            options.insert("channels", QVector4D(mask(0), mask(1), mask(2), mask(3)));
        } else if constexpr (std::is_same_v<Effect, ViewShadow>) {
            softKernel(settings.blur);
            options.insert("mode", 1);
            options.insert("offset", uv(settings.distance.x, settings.distance.y));
            options.insert("strength", settings.alpha);
            options.insert("tint", tint(settings.color.value_or(Color{})));
            options.insert("mix", static_cast<int>(settings.mix));
            options.insert("onlyEffect", settings.shadowOnly);
        } else if constexpr (std::is_same_v<Effect, ViewGlow>) {
            softKernel(settings.blur);
            options.insert("mode", 2);
            options.insert("tint", tint(settings.color));
            options.insert("strength", settings.strength);
            options.insert("onlyEffect", settings.glowOnly);
        } else if constexpr (std::is_same_v<Effect, ViewBloom>) {
            softKernel(settings.blur);
            options.insert("mode", 3);
            options.insert("threshold", settings.threshold);
            options.insert("strength", settings.strength);
        } else if constexpr (std::is_same_v<Effect, ViewShader>) {
            options.insert("shader", true);
            options.insert("shaderUrl", viewShaderUrl(qtext(settings.path)));
            options.insert("time", settings.time);
            options.insert("amount", settings.amount);
        }
    }, command.effect);

    return {{"kind", "viewEffect"}, {"area", panelRect(command.bounds)}, {"effect", options},
            {"commands", drawingDescriptions(*command.drawings, command.bounds, render)}};
}

QVariantList drawingDescriptions(const std::vector<yellow::PanelDrawing>& drawings, const yellow::Area& bounds,
                                PanelRender* render)
{
    QVariantList commands;
    commands.reserve(static_cast<qsizetype>(drawings.size()));
    for (const auto& drawing : drawings) {
        auto command = std::visit([render](const auto& value) {
            if constexpr (std::is_same_v<std::decay_t<decltype(value)>, yellow::CachedDrawCommand> ||
                          std::is_same_v<std::decay_t<decltype(value)>, yellow::EffectDrawCommand>) {
                return drawingDescription(value, render);
            } else {
                return drawingDescription(value);
            }
        }, drawing.command);
        const auto& mapping = drawing.mapping;
        command.insert("origin", QPointF(mapping.origin.x, mapping.origin.y));
        command.insert("scale", mapping.scale);
        command.insert("rotation", mapping.rotation);
        command.insert("clip", clipDescription(mapping, bounds));
        commands.push_back(std::move(command));
    }
    return commands;
}

QVariantMap frontPanelDescription(const Schematic& graph, const ModulePanel& panel,
                                 PanelRender* render = nullptr, double rasterScale = 1);

void appendPanelItems(const Schematic& graph, const yellow::View& target,
                      const std::vector<yellow::PanelDrawing>& drawings, QVariantList& items,
                      PanelRender* render, double rasterScale, QVariantList path = {})
{
    for (const auto& entry : graph.nodes()) {
        const auto* child = graph.module(entry.id);
        if (!child) {
            continue;
        }
        path.push_back(entry.id);
        for (const auto kind : {ModulePanelKind::Front, ModulePanelKind::Properties}) {
            if (!entry.showPanelInParent) {
                break;
            }
            const auto panel = child->modulePanel(kind);
            if (!panel || !panel->view.attachesTo(target)) {
                continue;
            }
            const auto position = std::ranges::upper_bound(drawings, panel->parentView.drawOrder,
                                                           {}, &yellow::PanelDrawing::order);
            const auto& bounds = panel->view.bounds;
            const auto& mapping = panel->parentView.mapping;
            const auto origin = panel->position;
            const auto anchor = mapping.toPanel(origin);
            items.push_back(QVariantMap{
                {"path", path}, {"name", qtext(entry.moduleName)}, {"properties", kind == ModulePanelKind::Properties},
                {"anchors", panel->anchors},
                {"rect", QRectF(anchor.x, anchor.y, bounds.width * mapping.scale, bounds.height * mapping.scale)},
                {"rotation", mapping.rotation},
                {"position", QPointF(origin.x, origin.y)}, {"scale", mapping.scale},
                {"clip", clipDescription(mapping, target.bounds)},
                {"panel", frontPanelDescription(*child, *panel, render, rasterScale * mapping.scale)},
                {"layer", static_cast<int>(2 * (position - drawings.begin()))},
            });
        }
        // A receiver can pick up a View over more than one module level.
        appendPanelItems(*child, target, drawings, items, render, rasterScale, path);
        path.pop_back();
    }
}

QVariantMap frontPanelDescription(const Schematic& graph, const ModulePanel& panel,
                                 PanelRender* render, double rasterScale)
{
    const auto drawings = render ? render->graph.renderPanel(panel.view, rasterScale) : graph.panelDrawings(panel.view);
    // A Step trigger can resize the panel while its frame is being built.
    const auto current = render ? graph.readPanel(panel.view.panel) : panel;
    QVariantList items;
    appendPanelItems(graph, current.view, drawings, items, render, rasterScale);
    return {{"key", viewKey(current.view)}, {"bounds", panelRect(current.view.bounds)}, {"border", current.borderVisible},
            {"path", panelPath(current.view.schematicPath)}, {"node", current.view.panel},
            {"mouseMoves", current.view.mouseMoves}, {"items", items},
            {"drawings", drawingDescriptions(drawings, current.view.bounds, render)}};
}

QVariantMap describe(const Primitive& spec, const Schematic* graph = nullptr,
                     const SchematicNode* entry = nullptr, const QVariantMap* renderedPanel = nullptr)
{
    const bool splitter = spec.kind == PrimitiveKind::ArraySplitter;
    const auto splitterOutputs = splitter && entry ? visibleSplitterOutputs(*graph, entry->id) : std::size_t{1};
    const auto layout = entry ? nodeGeometry(*graph, *entry, splitterOutputs) : geometry(spec);
    const auto inputPorts = entry ? entry->inputPorts() : spec.inputs;
    auto outputPorts = entry ? entry->outputPorts() : spec.outputs;
    if (splitter) outputPorts.resize(splitterOutputs);
    const int id = entry ? entry->id : 0;
    const bool boundary = isModuleEndpoint(spec.kind);
    QVariantList inputs;
    for (int i = 0; i < static_cast<int>(inputPorts.size()); ++i) {
        auto port = inputPorts[i];
        const bool isTemplate = !port.type && spec.hasTemplateConnectors();
        if (graph) {
            port.type = graph->inputType(id, i);
        }
        // The file loader keeps its original Clear index while displaying the Alpha connector order.
        constexpr std::array fileLoaderRows{0, 1, 2, 4, 3};
        const int row = spec.kind == PrimitiveKind::BitmapArrayFromFile ? fileLoaderRows[i] : i;
        auto description = portDescription(port, layout.firstInputY + row * layout.inputSpacing);
        const auto first = static_cast<int>(firstExpandableInput(spec.kind));
        if (hasExpandableInputs(spec.kind) && i >= first && i + 1 < static_cast<int>(inputPorts.size())) {
            description.insert("title", QString::number(i - first));
        }
        description.insert("canChooseType", isTemplate || boundary || spec.kind == PrimitiveKind::Ruby);
        if (spec.kind == PrimitiveKind::Ruby) description.insert("typeId", static_cast<int>(port.type.value()));
        inputs.push_back(description);
    }
    const auto typeGroups = entry && (spec.hasTemplateConnectors() || boundary)
        ? templateTypeGroups(spec.kind, entry->typeOverride) : QVariantList{};
    QVariantList outputs;
    for (int i = 0; i < static_cast<int>(outputPorts.size()); ++i) {
        auto port = outputPorts[i];
        const bool isTemplate = !port.type && spec.hasTemplateConnectors();
        if (graph) {
            port.type = graph->outputType(id, i);
        }
        auto description = portDescription(port, layout.outputY(i));
        if (splitter || (hasExpandableOutputs(spec.kind) && i + 1 < static_cast<int>(outputPorts.size()))) {
            description.insert("title", QString::number(i));
        }
        description.insert("canChooseType", isTemplate || boundary || spec.kind == PrimitiveKind::Ruby);
        if (spec.kind == PrimitiveKind::Ruby) description.insert("typeId", static_cast<int>(port.type.value()));
        outputs.push_back(description);
    }
    const bool busNode = spec.behavior == PrimitiveBehavior::CreateBus ||
                         spec.behavior == PrimitiveBehavior::ExtractBus;
    const bool bitmapFilePicker = spec.kind == PrimitiveKind::Bitmap || spec.kind == PrimitiveKind::BitmapArrayFromFile;
    const bool areaOperator = isAreaOperator(spec.kind);
    const bool shortTitle = spec.kind == PrimitiveKind::ModulePropertiesGui ||
                            (spec.behavior == PrimitiveBehavior::AreaGeometry && !areaOperator) ||
                            spec.behavior == PrimitiveBehavior::SplitColor || spec.behavior == PrimitiveBehavior::MakeColor ||
                            spec.behavior == PrimitiveBehavior::MakeBrush;
    const auto displayName = entry && shortTitle ? spec.symbol : spec.name;
    const auto title = entry && spec.kind == PrimitiveKind::Module ? qtext(entry->moduleName)
        : entry && !boundary && !entry->label.empty() ? qtext(entry->label) : qtext(displayName);
    const bool linked = entry && std::ranges::any_of(graph->links(), [id](auto link) {
        return link.source == id || link.target == id;
    });
    const bool wireless = spec.behavior == PrimitiveBehavior::SendWireless ||
                          spec.behavior == PrimitiveBehavior::ReceiveWireless;
    const bool wirelessLabel = usesWirelessLabel(spec.kind);
    const auto wirelessName = wireless && entry ? qtext(graph->wirelessName(id)) : QString{};
    const bool wirelessConnected = wireless && entry && graph->wirelessConnected(id);
    QVariantMap displayedPanel;
    bool hasFrontPanel = false;
    bool hasProperties = false;
    bool propertiesOpen = false;
    bool propertiesPinned = false;
    ModuleHelpText help;
    if (entry && spec.kind == PrimitiveKind::Module) {
        const auto& child = *graph->module(id);
        help = graph->readModuleHelp(id);
        const auto contains = [&child](PrimitiveKind kind) {
            return std::ranges::any_of(child.nodes(), [kind](const auto& node) { return node.kind == kind; });
        };
        hasFrontPanel = contains(PrimitiveKind::ModuleGui);
        hasProperties = contains(PrimitiveKind::ModulePropertiesGui);
        propertiesOpen = hasProperties && entry->propertiesVisibility != PropertiesVisibility::Closed;
        propertiesPinned = propertiesOpen && entry->propertiesVisibility == PropertiesVisibility::Pinned;
        if (renderedPanel) {
            displayedPanel = *renderedPanel;
        } else if (const auto panel = visibleModulePanel(child, *entry)) {
            displayedPanel = frontPanelDescription(child, *panel);
        }
    }
    return {{"kind", static_cast<int>(spec.kind)}, {"title", title},
            {"helpTitle", qtext(help.title)}, {"helpBody", qtext(help.body)},
            {"displayedPanel", displayedPanel}, {"hasFrontPanel", hasFrontPanel}, {"hasProperties", hasProperties},
            {"propertiesOpen", propertiesOpen}, {"propertiesPinned", propertiesPinned},
            {"showPanelInParent", entry && entry->showPanelInParent}, {"panelUnlocked", entry && entry->panelUnlocked},
            {"panelScale", panelScale}, {"panelHidden", entry && entry->panelHidden},
            {"module", spec.kind == PrimitiveKind::Module}, {"linked", linked}, {"boundary", boundary},
            {"automaticType", entry && boundary && !entry->typeOverride},
            {"boundaryName", entry && boundary ? qtext(entry->label) : QString{}},
            {"panelTop", 46.0 + (entry ? modulePortHeight(*entry) : 0.0)},
            {"customLabel", entry && !entry->label.empty()},
            {"wireless", wireless}, {"wirelessLabel", wirelessLabel}, {"wirelessName", wirelessName},
            {"wirelessConnected", wirelessConnected},
            {"symbol", qtext(spec.symbol)}, {"width", layout.width}, {"height", layout.height},
            {"inputs", inputs}, {"outputs", outputs}, {"templateTypeGroups", typeGroups},
            {"operatorNode", spec.behavior == PrimitiveBehavior::Binary ||
                             spec.behavior == PrimitiveBehavior::UnaryMath || spec.behavior == PrimitiveBehavior::Compare ||
                             spec.behavior == PrimitiveBehavior::StreamMath || areaOperator},
            {"triggerNode", spec.behavior == PrimitiveBehavior::TriggerButton},
            {"tickNode", spec.behavior == PrimitiveBehavior::Tick},
            {"midiDevicePicker", spec.kind == PrimitiveKind::MidiIn || spec.kind == PrimitiveKind::MidiOut},
            {"multiline", spec.kind == PrimitiveKind::MultilineString},
            {"tickDisplaySync", entry && entry->tick && entry->tick->displaySync},
            {"captureButton", (spec.behavior == PrimitiveBehavior::CaptureSignal || spec.behavior == PrimitiveBehavior::SampleMono)
                                  ? qtext(spec.inputs.back().name) : QString{}},
            {"dspNode", isDspEditor(spec.kind)},
            {"assemblerNode", spec.kind == PrimitiveKind::Assembler},
            {"rubyNode", spec.kind == PrimitiveKind::Ruby},
            {"rubySource", entry && entry->rubyCode ? qtext(entry->rubyCode->draft) : QString{}},
            {"rubyStatus", entry && entry->rubyCode ? qtext(entry->rubyCode->drawError.empty()
                ? entry->rubyCode->status : entry->rubyCode->drawError) : QString{}},
            {"rubyRunning", entry && entry->rubyCode && entry->rubyCode->running},
            {"rubyManualPorts", entry && entry->rubyCode && entry->rubyCode->manualPorts},
            {"rubyPortLimit", maxRubyPorts},
            {"rubyFailed", entry && entry->rubyCode && (entry->rubyCode->error || !entry->rubyCode->drawError.empty())},
            {"dspSource", entry && entry->dspCode ? qtext(entry->dspCode->draft) : QString{}},
            {"dspAppliedSource", entry && entry->dspCode ? qtext(entry->dspCode->source) : QString{}},
            {"valueField", (spec.behavior == PrimitiveBehavior::ValueBox && spec.kind != PrimitiveKind::Color) ||
                           spec.behavior == PrimitiveBehavior::Counter || spec.behavior == PrimitiveBehavior::Tick},
            {"colorField", spec.kind == PrimitiveKind::Color},
            {"bitmapFilePicker", bitmapFilePicker}, {"busNode", busNode},
            {"indexSelector", spec.behavior == PrimitiveBehavior::IndexSelector},
            {"settingsChoice", spec.kind == PrimitiveKind::Settings && entry
                ? std::get<std::int32_t>(*entry->localValue) : 0},
            {"settingsChoices", spec.kind == PrimitiveKind::Settings ? [] {
                QStringList choices;
                for (const auto name : systemSettingNames) choices.push_back(qtext(name));
                return choices;
            }() : QStringList{}},
            {"labeledOutputs", !outputs.isEmpty() && (spec.behavior == PrimitiveBehavior::Module ||
                               spec.behavior == PrimitiveBehavior::AudioEndpoint ||
                               spec.behavior == PrimitiveBehavior::DspCode || spec.behavior == PrimitiveBehavior::RubyCode ||
                               spec.behavior == PrimitiveBehavior::StreamRouting ||
                               spec.behavior == PrimitiveBehavior::CaptureSignal ||
                               spec.behavior == PrimitiveBehavior::SampleMono ||
                               spec.behavior == PrimitiveBehavior::TextFile ||
                               spec.behavior == PrimitiveBehavior::FileUtility ||
                               spec.behavior == PrimitiveBehavior::PersistSettings ||
                               spec.behavior == PrimitiveBehavior::MediaSource ||
                               spec.behavior == PrimitiveBehavior::NetworkTransfer ||
                               spec.behavior == PrimitiveBehavior::ParseSfz ||
                               spec.behavior == PrimitiveBehavior::MouseCaptureControl ||
                               spec.behavior == PrimitiveBehavior::SerialDevice ||
                               spec.behavior == PrimitiveBehavior::WaveFile ||
                               spec.behavior == PrimitiveBehavior::Timer ||
                               spec.behavior == PrimitiveBehavior::MemoryOperation ||
                               spec.behavior == PrimitiveBehavior::MidiOperation ||
                               spec.behavior == PrimitiveBehavior::MidiStream ||
                               spec.behavior == PrimitiveBehavior::NotesFilter ||
                               spec.behavior == PrimitiveBehavior::VoiceRouting ||
                               spec.behavior == PrimitiveBehavior::VoiceSignal ||
                               spec.behavior == PrimitiveBehavior::HostMidi ||
                               spec.behavior == PrimitiveBehavior::PluginHost ||
                               spec.behavior == PrimitiveBehavior::PresetUtility ||
                               outputs.size() > 1 || splitter || spec.behavior == PrimitiveBehavior::ExtractBus ||
                               spec.behavior == PrimitiveBehavior::ParseMouseMessage ||
                               spec.behavior == PrimitiveBehavior::MouseMove || spec.behavior == PrimitiveBehavior::Window)},
            {"labeledInputs", spec.behavior == PrimitiveBehavior::Module || spec.behavior == PrimitiveBehavior::Switch ||
                              spec.behavior == PrimitiveBehavior::ProcessTrigger ||
                              spec.behavior == PrimitiveBehavior::AudioEndpoint ||
                              spec.behavior == PrimitiveBehavior::AudioControl ||
                              spec.behavior == PrimitiveBehavior::WaveFile ||
                              spec.behavior == PrimitiveBehavior::Tick ||
                              spec.behavior == PrimitiveBehavior::DspCode || spec.behavior == PrimitiveBehavior::RubyCode ||
                              spec.behavior == PrimitiveBehavior::StreamRouting ||
                              spec.behavior == PrimitiveBehavior::PolyBridge ||
                              spec.behavior == PrimitiveBehavior::CaptureSignal || spec.behavior == PrimitiveBehavior::SampleMono ||
                              spec.behavior == PrimitiveBehavior::DrawView || spec.behavior == PrimitiveBehavior::DropList ||
                              spec.behavior == PrimitiveBehavior::TransformView || spec.behavior == PrimitiveBehavior::SetGuiArea ||
                              spec.behavior == PrimitiveBehavior::ViewLoop || spec.behavior == PrimitiveBehavior::ViewCache ||
                              spec.behavior == PrimitiveBehavior::Redraw || spec.behavior == PrimitiveBehavior::ViewEffect ||
                              spec.behavior == PrimitiveBehavior::Window || spec.behavior == PrimitiveBehavior::MeasureText ||
                              spec.behavior == PrimitiveBehavior::BitmapOperation || spec.behavior == PrimitiveBehavior::BitmapCanvas ||
                              spec.behavior == PrimitiveBehavior::BitmapMetadata || spec.behavior == PrimitiveBehavior::LoadBitmapArray ||
                              spec.behavior == PrimitiveBehavior::EditControl || spec.behavior == PrimitiveBehavior::PopupList ||
                              spec.behavior == PrimitiveBehavior::DropFiles || spec.behavior == PrimitiveBehavior::DragFiles ||
                              spec.behavior == PrimitiveBehavior::MouseArea ||
                              spec.behavior == PrimitiveBehavior::MouseInfo || spec.behavior == PrimitiveBehavior::MouseDrag ||
                              spec.behavior == PrimitiveBehavior::ShowCursor ||
                              spec.behavior == PrimitiveBehavior::Blocker || spec.behavior == PrimitiveBehavior::MakePoint ||
                              spec.behavior == PrimitiveBehavior::BuildArray || spec.behavior == PrimitiveBehavior::GetArrayElement ||
                              spec.behavior == PrimitiveBehavior::AppendArrays || spec.behavior == PrimitiveBehavior::TransformArray ||
                              spec.behavior == PrimitiveBehavior::StoreArray || spec.behavior == PrimitiveBehavior::ProcessArray ||
                              spec.behavior == PrimitiveBehavior::ProcessString || spec.behavior == PrimitiveBehavior::TextFile ||
                              spec.behavior == PrimitiveBehavior::FileUtility ||
                              spec.behavior == PrimitiveBehavior::SerialDevice ||
                              spec.behavior == PrimitiveBehavior::PersistSettings ||
                              spec.behavior == PrimitiveBehavior::SystemAction ||
                              spec.behavior == PrimitiveBehavior::MediaSource ||
                              spec.behavior == PrimitiveBehavior::NetworkTransfer ||
                              spec.behavior == PrimitiveBehavior::ParseSfz ||
                              spec.behavior == PrimitiveBehavior::MouseCaptureControl ||
                              spec.behavior == PrimitiveBehavior::HttpTransfer ||
                              spec.behavior == PrimitiveBehavior::ParseColorMatrix ||
                              spec.kind == PrimitiveKind::BasicCounter ||
                              spec.behavior == PrimitiveBehavior::SystemInfo || spec.behavior == PrimitiveBehavior::SampleTime ||
                              spec.behavior == PrimitiveBehavior::Timer ||
                              spec.behavior == PrimitiveBehavior::MemoryOperation ||
                              spec.behavior == PrimitiveBehavior::MidiOperation ||
                              spec.behavior == PrimitiveBehavior::MidiStream ||
                              spec.behavior == PrimitiveBehavior::NotesFilter ||
                              spec.behavior == PrimitiveBehavior::VoiceRouting ||
                              spec.behavior == PrimitiveBehavior::VoiceSignal ||
                              spec.behavior == PrimitiveBehavior::HostMidi ||
                              spec.behavior == PrimitiveBehavior::MidiDevice ||
                              spec.behavior == PrimitiveBehavior::ManagePresets ||
                              spec.behavior == PrimitiveBehavior::PresetUtility ||
                              spec.behavior == PrimitiveBehavior::PresetParameter ||
                              spec.behavior == PrimitiveBehavior::PluginHost ||
                              spec.behavior == PrimitiveBehavior::GreenLogic ||
                              spec.behavior == PrimitiveBehavior::StateMachineLogic ||
                              spec.behavior == PrimitiveBehavior::MatrixRouting ||
                              spec.behavior == PrimitiveBehavior::IndexSelector ||
                              (spec.behavior == PrimitiveBehavior::AreaGeometry && !areaOperator && inputs.size() > 1) ||
                              spec.behavior == PrimitiveBehavior::ModuleGui ||
                              spec.behavior == PrimitiveBehavior::ColorMatrixSet ||
                              spec.behavior == PrimitiveBehavior::ApplyColorMatrix ||
                              spec.behavior == PrimitiveBehavior::MakeColor ||
                              spec.behavior == PrimitiveBehavior::MakeBrush ||
                              spec.behavior == PrimitiveBehavior::MakePen || spec.behavior == PrimitiveBehavior::MakeSolid ||
                              spec.behavior == PrimitiveBehavior::MakeFont || spec.behavior == PrimitiveBehavior::EmbedFonts ||
                              spec.behavior == PrimitiveBehavior::FontAttributes ||
                              spec.behavior == PrimitiveBehavior::MakeStringFormat ||
                              spec.behavior == PrimitiveBehavior::StringFormatAttributes ||
                              bitmapFilePicker || busNode || (wireless && !wirelessLabel)}};
}

QString formatDspSample(double sample)
{
    return sample == 0 && std::signbit(sample) ? QStringLiteral("-0") : QString::number(sample, 'g', 17);
}

template<class Trace, class LiveTrace>
QVariantMap dspReadout(QString title, QString type, const AlphaConnector& connector, Trace trace, LiveTrace liveTrace)
{
    QVariantMap result{{"title", title}, {"type", type}};
    if (connector.palette == "memory") {
        return result;
    }
    result.insert("dspPalette", qtext(connector.palette));
    try {
        const auto live = liveTrace();
        const auto samples = live ? std::span<const double>{*live} : trace();
        result.insert("dspLive", live.has_value());
        if (samples.empty()) {
            result.insert("dspStatus", live ? QStringLiteral("Live · Waiting for samples")
                                            : QStringLiteral("Idle · Waiting for capture"));
        } else {
            QVariantList points;
            points.reserve(samples.size());
            for (const double sample : samples) {
                points.push_back(std::isfinite(sample) ? QVariant{sample} : QVariant{});
            }
            result.insert("dspSamples", points);
            const bool firstLane = connector.palette == "poly" || connector.type == Type::Mono4 ||
                                   connector.type == Type::Mono4Double;
            const auto status = live ? QStringLiteral("Live") : QStringLiteral("Last capture");
            result.insert("dspStatus", firstLane ? QStringLiteral("Lane 0 · ") + status : status);
            result.insert("value", formatDspSample(samples.back()));
        }
    } catch (const std::invalid_argument&) {
        result.insert("dspStatus", QStringLiteral("Multiple sources on this signal"));
    }
    return result;
}

template<class Inspect>
QVariantMap readout(QString title, QString type, Inspect inspect)
{
    try {
        auto signal = inspect();
        QString value;
        QVariantMap result{{"title", title}, {"type", type}, {"triggers", QString::number(signal.triggers)}};
        if (const auto* matrix = signal.value ? std::get_if<yellow::ColorMatrix>(&*signal.value) : nullptr) {
            QStringList coefficients;
            for (const auto& row : matrix->coefficients) {
                for (float coefficient : row) {
                    coefficients.push_back(QString::number(coefficient, 'g', 5));
                }
            }
            result.insert("matrix", coefficients);
        } else if (const auto* tool = signal.value ? std::get_if<yellow::PaintTool>(&*signal.value) : nullptr) {
            value = paintToolReadout(*tool, result);
        } else if (const auto* font = signal.value ? std::get_if<yellow::Font>(&*signal.value) : nullptr) {
            value = fontReadout(*font, result);
        } else if (const auto* bitmap = signal.value ? std::get_if<yellow::Bitmap>(&*signal.value) : nullptr) {
            value = bitmapReadout(*bitmap, result);
        } else if (const auto* bus = signal.value ? std::get_if<green::Bus>(&*signal.value) : nullptr) {
            QStringList lines{QStringLiteral("%1 channels").arg(bus->channels.size())};
            for (const auto& channel : bus->channels | std::views::take(4)) {
                const auto connector = portDescription({"", channel.type}, 0);
                lines.push_back(QStringLiteral("%1 · %2").arg(qtext(channel.name), connector.value("type").toString()));
            }
            value = lines.join(u'\n');
        } else if (signal.value) {
            value = valueText(*signal.value);
            if (value.isEmpty() && std::holds_alternative<std::string>(*signal.value)) {
                value = QStringLiteral("(empty string)");
            }
            if (const auto* storedColor = std::get_if<yellow::Color>(&*signal.value)) {
                const auto color = QColor::fromRgba(storedColor->argb);
                result.insert("swatch", color);
                value += QStringLiteral("\nA %1 · R %2 · G %3 · B %4")
                             .arg(color.alpha()).arg(color.red()).arg(color.green()).arg(color.blue());
            }
        }
        result.insert("value", value.left(500));
        return result;
    } catch (const std::exception&) {
        return {{"title", title}, {"type", type}, {"value", QStringLiteral("Value unavailable")}};
    }
}

QVariantList nodeBounds(const Schematic& graph)
{
    QVariantList list;
    for (const auto& entry : graph.nodes()) {
        const auto bounds = nodeGeometry(graph, entry);
        list.push_back(QRectF(entry.x, entry.y, bounds.width, bounds.height));
    }
    return list;
}

}

SchematicModel::SchematicModel(QObject* parent)
    : QAbstractListModel(parent)
    , session_([](std::string_view path) { return desktop::readBitmapFile(qtext(path)); },
        {
            .save = [](std::string_view path, std::string_view encoding, const yellow::Bitmap& bitmap) {
                desktop::saveBitmapFile(qtext(path), qtext(encoding), bitmap);
            },
            .render = [this](const BitmapCanvasState& canvas, const yellow::View& view, const yellow::Bitmap& background,
                             const std::vector<yellow::PanelDrawing>& drawings) {
                auto* engine = qmlEngine(this);
                if (!engine) {
                    throw std::runtime_error("Bitmap rendering needs a Qt Quick scene");
                }
                QVariantList items;
                appendPanelItems(session_.graphAt(0), view, drawings, items, nullptr, canvas.gridStep);
                return desktop::renderBitmapSurface(*engine, QSize(canvas.width, canvas.height), background,
                    {{"graph", QVariant::fromValue(this)}, {"pixelsPerUnit", canvas.gridStep}, {"bitmapTarget", true},
                     {"schematicChrome", false}, {"panel", QVariantMap{
                         {"bounds", panelRect(view.bounds)}, {"items", items},
                         {"drawings", drawingDescriptions(drawings, view.bounds, nullptr)},
                     }}});
            },
            .loadSvg = [](std::string_view path) { return desktop::readSvgFile(qtext(path)); },
        }, desktop::measureText, {.load = readTextFile, .save = saveTextFile, .loadBounded = readPresetFile},
        {.folder = desktop::systemFolder, .fonts = desktop::systemFontFamilies,
         .openUrl = desktop::openWebUrl, .launchProgram = desktop::launchProgram, .macAddresses = desktop::systemMacAddresses,
         .diskSerial = desktop::firstDiskSerial,
         .sendHttp = [this](HttpRequest request) {
             if (!httpNetwork_) httpNetwork_ = std::make_unique<QNetworkAccessManager>();
             desktop::sendHttpRequest(*httpNetwork_, std::move(request),
                 [this](const auto& pending) { return graph().hasPendingHttpRequest(pending); },
                 [this](const auto& pending, HttpResponse response) {
                     try {
                         if (!graph().finishHttpRequest(pending, std::move(response))) return;
                     } catch (const std::exception& error) {
                         setFeedback(QString::fromUtf8(error.what()));
                     }
                     refreshValues();
                 });
         },
         .openNetwork = desktop::openNetwork,
         .openMedia = desktop::openMedia,
         .registerFonts = desktop::registerFonts, .ruby = desktop::makeRubyBackend(desktop::makeRubySoundBackend())})
{
    connect(this, &SchematicModel::geometryChanged, this, &SchematicModel::breadcrumbsChanged);
    connect(this, &SchematicModel::viewportChanged, this, &SchematicModel::breadcrumbsChanged);
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state != Qt::ApplicationActive) {
            graph().clearPressedKeys();
            if (graph().releaseMouseCapture()) refreshValues();
        }
        updateCursor();
    });
    graph().setMidiDeviceBackend(desktop::makeMidiDeviceBackend());
    graph().setSerialDeviceBackend(desktop::makeSerialDeviceBackend());
    graph().setSettingsStoreBackend(desktop::makeSettingsStoreBackend());
    serialPollTimer_.setInterval(10);
    connect(&serialPollTimer_, &QTimer::timeout, this, [this] {
        try {
            const bool serial = graph().pollSerialDevices();
            const bool network = graph().pollNetworkConnections();
            const bool media = graph().pollMedia();
            const bool sequence = graph().advanceSequencer(std::chrono::steady_clock::now());
            if (serial || network || media || sequence ||
                (!graph().hasOpenSerialPorts() && !graph().hasOpenNetworkConnections() &&
                 !session_.graphAt(0).hasOpenMedia() && sequencerMode() != 2)) {
                refreshValues();
            }
        } catch (const std::exception& error) {
            setFeedback(QString::fromUtf8(error.what()));
        }
    });
    graph().setAudioControlCallbacks([this](const AudioDeviceSetup& setup) { return changeAudioSetup(setup); }, [this] {
        try {
            if (!audioDevice_) throw std::runtime_error("Start an ASIO device before opening its control panel");
            auto* window = qGuiApp->focusWindow();
            audioDevice_->openAsioSettings(window ? reinterpret_cast<void*>(window->winId()) : nullptr);
        } catch (const std::exception& error) {
            setFeedback(QString::fromUtf8(error.what()));
        }
    });
    try {
        graph().setAudioDevices(desktop::AudioDevice::devices());
    } catch (const std::exception& error) {
        setFeedback(QString::fromUtf8(error.what()));
    }
    midiScanTimer_.setInterval(1000);
    connect(&midiScanTimer_, &QTimer::timeout, this, [this] {
        if (refreshMidiHardware(true)) refreshValues();
    });
    midiPollTimer_.setTimerType(Qt::PreciseTimer);
    midiPollTimer_.setInterval(5);
    connect(&midiPollTimer_, &QTimer::timeout, this, [this] {
        try {
            if (graph().pollMidiDevices()) refreshValues();
        } catch (const std::exception& error) {
            setFeedback(QString::fromUtf8(error.what()));
        }
    });
    tickTimer_.setTimerType(Qt::PreciseTimer);
    tickTimer_.setSingleShot(true);
    connect(&tickTimer_, &QChronoTimer::timeout, this, [this] { processTicks(TickWake::Timer); });
    connect(this, &SchematicModel::valuesChanged, this, [this] {
        if ((pendingProjectFile_ || windowClose_ == WindowClose::Requested) && session_.schematicReadyToClose()) {
            // Finish after the trigger cascade and the current model update have returned.
            QMetaObject::invokeMethod(this, &SchematicModel::finishPendingClose, Qt::QueuedConnection);
        }
        scheduleDeferredTriggers();
    });
    audioFramesTimer_.setInterval(16);
    connect(&audioFramesTimer_, &QTimer::timeout, this, [this] {
        try {
            if (graph().publishAudioFrames()) refreshValues();
        } catch (const std::exception& error) {
            setFeedback(QString::fromUtf8(error.what()));
        }
    });
    audioStatusTimer_.setInterval(250);
    connect(&audioStatusTimer_, &QTimer::timeout, this, [this] {
        if (!audioDevice_->active()) {
            stopAudio();
            setFeedback(tr("The audio device stopped. Start Audio to reopen it."));
        } else {
            refreshAudioControls();
        }
    });
    const QSettings settings(QStringLiteral("OpenStone"), QStringLiteral("OpenStone"));
    straightWires_ = settings.value(QStringLiteral("wiring/straight"), false).toBool();
    autoLink_ = settings.value(QStringLiteral("wiring/autoLink"), true).toBool();
    const auto libraryFolder = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    toolboxPath_ = libraryFolder + QStringLiteral("/Toolbox.json");
    recentlyUsedPath_ = libraryFolder + QStringLiteral("/RecentlyUsed.json");
    try {
        desktop::SchematicFile::loadToolbox(session_, toolboxPath_);
    } catch (const std::exception& error) {
        toolboxLoaded_ = false;
        setFeedback(tr("Could not load the toolbox. %1").arg(QString::fromUtf8(error.what())));
    }
    try {
        library_.setRecentlyUsed(desktop::SchematicFile::loadRecentlyUsed(recentlyUsedPath_));
    } catch (const std::exception& error) {
        if (toolboxLoaded_) setFeedback(tr("Could not load Recently Used. %1").arg(QString::fromUtf8(error.what())));
    }
    savedProject_ = desktop::SchematicFile::project(session_);
    rebuildLibrary();
    connect(this, &SchematicModel::levelChanged, this, [this] { library_.setInside(level() > 0); });
}

SchematicModel::~SchematicModel()
{
    serialPollTimer_.stop();
    midiPollTimer_.stop();
    midiScanTimer_.stop();
    audioStatusTimer_.stop();
    audioDevice_.reset();
    finishPointerDrag(false);
    setCursorOverride(std::nullopt);
}

double SchematicModel::audioSampleRate() const
{
    return audioDevice_ ? audioDevice_->sampleRate() : 0;
}

void SchematicModel::toggleAudio()
{
    if (audioStarting_) {
        return;
    }
    if (audioDevice_) {
        stopAudio();
        setFeedback({});
        return;
    }
    try {
        graph().setAudioDevices(desktop::AudioDevice::devices());
        audioPatch_.emplace(session_.graphAt(0).prepareAudio(desktop::AudioDevice::blockSize));
#if defined(Q_OS_MACOS) && QT_CONFIG(permissions)
        if (audioPatch_->processor().needsInput()) {
            const QMicrophonePermission microphone;
            switch (qApp->checkPermission(microphone)) {
            case Qt::PermissionStatus::Undetermined:
                audioStarting_ = true;
                emit audioChanged();
                qApp->requestPermission(microphone, this, [this](const QPermission& permission) {
                    if (!audioStarting_) return;
                    audioStarting_ = false;
                    if (permission.status() == Qt::PermissionStatus::Granted) {
                        startAudio();
                    } else {
                        stopAudio();
                        setFeedback(tr("Allow microphone access for OpenStone in System Settings to use Audio In."));
                    }
                });
                return;
            case Qt::PermissionStatus::Denied:
                throw std::runtime_error("Allow microphone access for OpenStone in System Settings to use Audio In");
            case Qt::PermissionStatus::Granted:
                break;
            }
        }
#endif
        startAudio();
    } catch (const std::exception& error) {
        stopAudio();
        setFeedback(QString::fromUtf8(error.what()));
    }
}

void SchematicModel::openAudioDevice(AudioDeviceSetup setup)
{
    setup.inputApi = audioPatch_->inputApi();
    setup.outputApi = audioPatch_->outputApi();
    if (setup.inputApi == AudioApi::Any && !setup.input) setup.inputApi = setup.outputApi;
    if (setup.outputApi == AudioApi::Any && !setup.output) setup.outputApi = setup.inputApi;
    if (setup.inputApi == AudioApi::DirectSound && setup.directSoundInput) setup.input = setup.directSoundInput;
    setup.inputDisabled |= setup.inputApi == AudioApi::DirectSound && setup.directSoundInputDisabled;
    const auto processor = audioPatch_->processorHandle();
    auto device = std::make_unique<desktop::AudioDevice>(processor, setup);
    graph().setSampleRate(device->sampleRate());
    if (!session_.graphAt(0).refreshAudioControls(*audioPatch_) ||
        (processor != audioPatch_->processorHandle() && !device->replaceProcessor(audioPatch_->processorHandle()))) {
        throw std::runtime_error("The schematic changed. Start Audio to use the new wiring");
    }
    device->start();
    audioDevice_ = std::move(device);
    graph().setAsioBufferSizes(audioDevice_->asioBufferSizes());
    graph().setLiveAudio(&*audioPatch_);
    audioStatusTimer_.start();
    if (audioPatch_->hasFrames()) audioFramesTimer_.start();
}

void SchematicModel::startAudio()
{
    try {
        openAudioDevice(graph().audioDeviceSetup());
        graph().setAudioChannels(audioDevice_->channelNames(true), audioDevice_->channelNames(false));
        setFeedback({});
        refreshValues();
        emit audioChanged();
    } catch (const std::exception& error) {
        stopAudio();
        setFeedback(QString::fromUtf8(error.what()));
    }
}

bool SchematicModel::changeAudioSetup(const AudioDeviceSetup& setup)
{
    const auto previous = graph().audioDeviceSetup();
    if (setup.outputDisabled) {
        if (audioDevice_) stopAudio();
        return true;
    }
    if (!audioDevice_) {
        try {
            if (setup.rate != previous.rate) desktop::AudioDevice::validate(setup);
            return true;
        } catch (const std::exception& error) {
            setFeedback(QString::fromUtf8(error.what()));
            return false;
        }
    }

    audioStatusTimer_.stop();
    audioDevice_.reset();
    try {
        openAudioDevice(setup);
        QMetaObject::invokeMethod(this, [this] {
            if (audioDevice_) {
                graph().setAudioChannels(audioDevice_->channelNames(true), audioDevice_->channelNames(false));
                refreshValues();
            }
        }, Qt::QueuedConnection);
        return true;
    } catch (const std::exception& error) {
        setFeedback(QString::fromUtf8(error.what()));
        try {
            openAudioDevice(previous);
        } catch (const std::exception&) {
            stopAudio();
        }
        return false;
    }
}

void SchematicModel::stopAudio()
{
    audioStarting_ = false;
    audioFramesTimer_.stop();
    audioStatusTimer_.stop();
    audioDevice_.reset();
    graph().setLiveAudio(nullptr);
    audioPatch_.reset();
    graph().setAudioChannels({}, {});
    if (graph().setAsioBufferSizes({})) refreshValues();
    emit audioChanged();
}

void SchematicModel::refreshAudioControls()
{
    if (!audioDevice_) {
        return;
    }
    try {
        const auto previous = audioPatch_->processorHandle();
        const auto previousInputApi = audioPatch_->inputApi();
        if (session_.graphAt(0).refreshAudioControls(*audioPatch_)) {
            const auto processor = audioPatch_->processorHandle();
            if (processor == previous) return;
            if ((audioPatch_->inputApi() != AudioApi::Any && audioPatch_->inputApi() != previousInputApi) ||
                !audioDevice_->replaceProcessor(processor)) {
                stopAudio();
                toggleAudio();
                return;
            }
            if (audioPatch_->hasFrames()) audioFramesTimer_.start();
            else audioFramesTimer_.stop();
            return;
        }
        stopAudio();
        setFeedback(tr("The schematic changed. Start Audio to use the new wiring."));
    } catch (const std::exception& error) {
        stopAudio();
        setFeedback(QString::fromUtf8(error.what()));
    }
}

int SchematicModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(graph().nodes().size());
}

QVariant SchematicModel::data(const QModelIndex& index, int role) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid | CheckIndexOption::ParentIsInvalid)) {
        return {};
    }
    const auto& entry = graph().nodes()[index.row()];
    const auto& spec = primitive(entry.kind);
    switch (role) {
    case IdRole: return entry.id;
    case SpecRole: {
        const auto cached = renderedPanels_.find(entry.id);
        auto result = describe(spec, &graph(), &entry, cached == renderedPanels_.end() ? nullptr : &cached->second);
        if (entry.kind == PrimitiveKind::Module) {
            result.insert("syncCount", static_cast<int>(session_.syncCount(entry.id)));
            result.insert("canUnsynchronize", session_.hasSyncGroup(entry.id));
        }
        return result;
    }
    case PositionRole: return QPointF(entry.x, entry.y);
    case EditableRole:
        if (entry.tick) {
            try {
                const auto tick = graph().readTick(entry.id);
                return !tick.displaySync && !tick.wired;
            } catch (const std::exception&) {
                return false;
            }
        }
        return graph().editable(entry.id);
    case ValueRole: {
        if (entry.kind == PrimitiveKind::MidiIn || entry.kind == PrimitiveKind::MidiOut) {
            const auto choices = graph().midiDeviceChoices(entry.id);
            const auto active = std::ranges::count(choices, true, &MidiDeviceChoice::connected);
            const auto selected = std::ranges::count(choices, true, &MidiDeviceChoice::selected);
            if (selected == 0) return tr("Choose devices");
            if (active == 1 && selected == 1) {
                return qtext(std::ranges::find(choices, true, &MidiDeviceChoice::connected)->name);
            }
            if (active == 0) return tr("Device unavailable");
            return active == selected ? tr("%1 devices").arg(active) : tr("%1 of %2 devices").arg(active).arg(selected);
        }
        if (entry.tick) {
            try {
                return entry.tick->displaySync ? QStringLiteral("VSync") : valueText(graph().readTick(entry.id).rate);
            } catch (const std::exception&) {
                return QStringLiteral("Error");
            }
        }
        const bool indexSelector = spec.behavior == PrimitiveBehavior::IndexSelector;
        if (spec.behavior != PrimitiveBehavior::ValueBox && spec.behavior != PrimitiveBehavior::Counter && !indexSelector) {
            return QString{};
        }
        try {
            return valueText(graph().readValue(entry.id, indexSelector ? 1 : 0));
        } catch (const std::exception&) {
            return QStringLiteral("Error");
        }
    }
    default: return {};
    }
}

QHash<int, QByteArray> SchematicModel::roleNames() const
{
    return {{IdRole, "nodeId"}, {SpecRole, "nodeSpec"}, {PositionRole, "nodePosition"},
            {ValueRole, "valueText"}, {EditableRole, "nodeEditable"}};
}

void SchematicModel::enterModule(int id)
{
    if (!graph().module(id)) {
        return;
    }
    beginResetModel();
    session_.enterModule(id);
    endSchematicChange();
}

void SchematicModel::navigate(int depth)
{
    if (depth < 0 || depth >= level()) {
        return;
    }
    beginResetModel();
    session_.navigate(depth);
    endSchematicChange();
}

void SchematicModel::endSchematicChange()
{
    graph().setPresentationPanel(std::nullopt);
    setFeedback({});
    refreshMidiHardware(false);
    renderPanelFrames();
    busLayoutRevision_ = graph().busLayoutRevision();
    endResetModel();
    emit levelChanged();
    emit clipboardChanged();
    emit painterChanged();
    emit geometryChanged();
    emit viewportChanged();
    processTicks(TickWake::Configure);
    emit valuesChanged();
    dispatchEditControlStart();
}

void SchematicModel::renameNode(int id, const QString& name)
{
    try {
        session_.renameNode(id, name.toUtf8().toStdString());
        setFeedback({});
    } catch (const std::exception& error) {
        setFeedback(QString::fromUtf8(error.what()));
    }
    refreshValues(true);
}

bool SchematicModel::copyNodes(const QVariantList& selection)
{
    if (selection.isEmpty()) return false;
    std::vector<int> ids;
    ids.reserve(selection.size());
    for (const auto& id : selection) ids.push_back(id.toInt());
    try {
        session_.copyNodes(ids);
        setFeedback({});
        emit clipboardChanged();
        return true;
    } catch (const std::exception& error) {
        setFeedback(QString::fromUtf8(error.what()));
        return false;
    }
}

bool SchematicModel::cutNodes(const QVariantList& selection)
{
    std::vector<int> ids;
    ids.reserve(selection.size());
    for (const auto& id : selection) {
        const int row = rowFor(id.toInt());
        if (row < 0 || isModuleEndpoint(graph().nodes()[row].kind)) return false;
        ids.push_back(id.toInt());
    }
    if (ids.empty() || !session_.beginEdit(SchematicEdit::CutNodes)) return false;

    beginResetModel();
    QString feedback;
    try {
        session_.cutNodes(ids);
    } catch (const std::exception& error) {
        feedback = QString::fromUtf8(error.what());
    }
    endSchematicChange();
    finishEdit();
    const bool succeeded = feedback.isEmpty();
    setFeedback(std::move(feedback));
    refreshAudioControls();
    return succeeded;
}

QVariantList SchematicModel::pasteNodes(double x, double y, bool synchronized)
{
    if (!canPaste() || (synchronized && !canPasteSynchronized())) {
        return {};
    }
    return insertNodes(SchematicEdit::PasteNodes, [&] { return session_.pasteNodes(x, y, synchronized); });
}

QVariantList SchematicModel::duplicateNodes(const QVariantList& selection)
{
    if (selection.isEmpty()) return {};
    std::vector<int> ids;
    ids.reserve(selection.size());
    for (const auto& id : selection) ids.push_back(id.toInt());
    return insertNodes(SchematicEdit::DuplicateNodes, [&] { return session_.duplicateNodes(ids, 24, 24); });
}

QVariantList SchematicModel::insertNodes(SchematicEdit edit, const std::function<std::vector<int>()>& insert)
{
    if (!session_.beginEdit(edit)) return {};

    beginResetModel();
    QVariantList inserted;
    QString feedback;
    try {
        for (int id : insert()) inserted.push_back(id);
    } catch (const std::exception& error) {
        feedback = QString::fromUtf8(error.what());
    }
    endSchematicChange();
    finishEdit();
    const auto lifecycleError = lifecycleErrorText(session_);
    setFeedback(feedback.isEmpty() ? lifecycleError : std::move(feedback));
    return inserted;
}

void SchematicModel::disconnectNode(int id)
{
    const bool ownsEdit = session_.beginEdit(SchematicEdit::DeleteLinks);
    try {
        session_.disconnectNode(id);
        setFeedback({});
    } catch (const std::exception& error) {
        setFeedback(QString::fromUtf8(error.what()));
    }
    emit geometryChanged();
    refreshValues(true);
    if (ownsEdit) finishEdit();
}

void SchematicModel::synchronizeAll(int id)
{
    try {
        session_.synchronizeAll(id);
        setFeedback({});
    } catch (const std::exception& error) {
        setFeedback(QString::fromUtf8(error.what()));
    }
    emit clipboardChanged();
    refreshValues(true);
}

void SchematicModel::unsynchronize(int id, bool all)
{
    session_.unsynchronize(id, all);
    emit clipboardChanged();
    refreshValues(true);
}

int SchematicModel::followSync(int id, bool previous)
{
    if (session_.syncCount(id) == 0) {
        return -1;
    }
    beginResetModel();
    const int destination = session_.followSync(id, previous);
    if (const auto row = rowFor(destination); row >= 0) {
        const auto& node = graph().nodes()[row];
        const auto bounds = nodeGeometry(graph(), node);
        session_.centerOn(node.x + bounds.width / 2, node.y + bounds.height / 2);
    }
    endSchematicChange();
    return destination;
}

void SchematicModel::startSyncPainter(int id)
{
    session_.startSyncPainter(id);
    setFeedback({});
    emit painterChanged();
}

void SchematicModel::stopSyncPainter()
{
    session_.stopSyncPainter();
    emit painterChanged();
}

void SchematicModel::paintSync(int id)
{
    try {
        session_.paintSync(id);
        setFeedback({});
    } catch (const std::exception& error) {
        setFeedback(QString::fromUtf8(error.what()));
    }
    emit clipboardChanged();
    refreshValues(true);
}

QVariantList SchematicModel::breadcrumbs() const
{
    QVariantList list;
    for (std::size_t depth = 0; depth <= session_.activeLevel(); ++depth) {
        const auto& view = session_.viewport(depth);
        list.push_back(QVariantMap{
            {"name", QString::fromStdString(session_.levelName(depth))},
            {"center", QPointF(view.x, view.y)}, {"zoom", view.zoom},
            {"nodeBounds", nodeBounds(session_.graphAt(depth))},
        });
    }
    return list;
}

QPointF SchematicModel::center() const
{
    const auto& view = session_.activeViewport();
    return {view.x, view.y};
}

double SchematicModel::zoom() const
{
    return session_.activeViewport().zoom;
}

void SchematicModel::pan(double dx, double dy)
{
    session_.pan(dx, dy);
    emit viewportChanged();
}

void SchematicModel::zoomAt(double factor, double x, double y)
{
    const auto previous = zoom();
    session_.zoomAt(factor, x, y);
    const auto& root = session_.graphAt(0);
    if (zoom() != previous && (root.hasDrawingBehavior(PrimitiveBehavior::ViewCache) ||
                               root.hasDrawingBehavior(PrimitiveBehavior::Redraw))) {
        refreshValues();
    }
    emit viewportChanged();
}

void SchematicModel::centerOn(int depth, double x, double y)
{
    if (depth < 0 || depth > level()) {
        return;
    }
    navigate(depth);
    session_.centerOn(x, y);
    emit viewportChanged();
}

void SchematicModel::resetView()
{
    session_.resetView();
    emit viewportChanged();
}

void SchematicModel::filterLibrary(const QString& query, const QString& group, const QString& type)
{
    library_.filter(query, group, type, level() > 0);
}

void SchematicModel::setLibraryFavorite(int kind, int toolboxIndex, bool favorite)
{
    if (!toolboxLoaded_) {
        setFeedback(tr("Could not load the toolbox. Favorites were not saved."));
        return;
    }
    try {
        if (toolboxIndex >= 0) {
            desktop::SchematicFile::setToolboxFavorite(session_, toolboxPath_, toolboxIndex, favorite);
        } else {
            desktop::SchematicFile::setPrimitiveFavorite(session_, toolboxPath_, static_cast<PrimitiveKind>(kind), favorite);
        }
        library_.setFavorite(kind, toolboxIndex, favorite);
        setFeedback({});
    } catch (const std::exception& error) {
        setFeedback(tr("Could not save Favorites. %1").arg(QString::fromUtf8(error.what())));
    }
}

namespace {

struct LibraryShortcut { QStringView key; PrimitiveKind kind; };
constexpr LibraryShortcut libraryShortcuts[]{
    {u"D", PrimitiveKind::Int}, {u"F", PrimitiveKind::Float}, {u"S", PrimitiveKind::String},
    {u"Q", PrimitiveKind::MultilineString}, {u"B", PrimitiveKind::Boolean}, {u"T", PrimitiveKind::Trigger},
    {u"M", PrimitiveKind::Module}, {u"G", PrimitiveKind::ModuleGui},
    {u"I", PrimitiveKind::ModuleInput}, {u"O", PrimitiveKind::ModuleOutput}
};

desktop::LibraryModel::Category libraryCategory(const Primitive& spec)
{
    using enum desktop::LibraryModel::Section;
    using enum PrimitiveBehavior;
    if (isPolyBridge(spec.kind) && spec.kind != PrimitiveKind::MonoToPoly) {
        return {Processing, QStringLiteral("MIDI")};
    }
    if (spec.kind == PrimitiveKind::Color) {
        return {Drawing, QStringLiteral("Color & Text")};
    }
    if (spec.kind == PrimitiveKind::BiquadFrequencyResponse || spec.kind == PrimitiveKind::BiquadPhaseResponse ||
        spec.kind == PrimitiveKind::EnvelopeControl || spec.kind == PrimitiveKind::VstOutput) {
        return {Audio, QStringLiteral("Streams & DSP")};
    }
    switch (spec.behavior) {
    case Module: case ModuleGui: case ModuleInput: case ModuleOutput: case PropertyInput: case PropertyOutput:
    case SendWireless: case ReceiveWireless:
        return {Basics, QStringLiteral("Modules")};
    case ValueBox: case MakePoint: case SplitPoint:
        return {Basics, QStringLiteral("Values")};
    case MemoryOperation:
        return {Processing, QStringLiteral("Memory")};
    case MidiDevice:
    case MidiOperation:
    case MidiStream:
    case NotesFilter:
    case VoiceRouting:
    case HostMidi:
        return {Processing, QStringLiteral("MIDI")};
    case VoiceSignal:
        if (spec.kind == PrimitiveKind::EnvelopeControl) return {Audio, QStringLiteral("Streams & DSP")};
        return {Processing, QStringLiteral("MIDI")};
    case ManagePresets:
    case PresetUtility:
    case PresetParameter:
    case PluginHost:
        return {Audio, QStringLiteral("Presets & Plug-in Host")};
    case SystemInfo:
    case SystemSettings:
    case SystemAction:
    case NetworkTransfer:
    case HttpTransfer:
    case PersistSettings:
    case FileUtility:
    case SampleTime:
        return {Basics, QStringLiteral("System")};
    case TriggerButton: case Counter: case Blocker: case Switch: case Tick: case ProcessTrigger: case Timer:
        return {Basics, QStringLiteral("Triggers")};
    case EditControl: case DropList: case IndexSelector: case PopupList: case DropFiles: case DragFiles: case Window:
    case PresentationControl: case KeyboardInput:
        return {Basics, QStringLiteral("Controls")};
    case Binary: case UnaryMath: case NoteConversion: case GeneralProcessing:
        if (spec.kind == PrimitiveKind::SampleRate) return {Audio, QStringLiteral("Streams & DSP")};
        return {Processing, QStringLiteral("Math")};
    case Compare: case GreenLogic: case StateMachineLogic: case MatrixRouting:
        return {Processing, QStringLiteral("Logic")};
    case CreateBus: case ExtractBus:
        return {Processing, QStringLiteral("Bus")};
    case ProcessString:
    case TextFile:
        return {Processing, QStringLiteral("Strings")};
    case BuildArray: case GetArrayElement: case AppendArrays: case TransformArray: case StoreArray: case ProcessArray:
        return {Processing, QStringLiteral("Arrays")};
    case SerialDevice:
        return {Audio, QStringLiteral("Serial I/O")};
    case SequencerTrack: return {Audio, QStringLiteral("Sequencer")};
    case MediaSource: return {Audio, QStringLiteral("Audio I/O")};
    case AudioEndpoint: case AudioControl:
        return {Audio, spec.kind == PrimitiveKind::MonoToFrame ? QStringLiteral("Streams & DSP") : QStringLiteral("Audio I/O")};
    case RubyCode: return {Processing, QStringLiteral("Ruby")};
    case DspCode: case PolyBridge: case CaptureSignal: case SampleMono: case StreamMath: case StreamConversion:
    case StreamRouting:
    case WaveFile:
        return {Audio, QStringLiteral("Streams & DSP")};
    case DrawView:
        return {Drawing, QStringLiteral("Draw")};
    case PanelUtility:
        if (spec.kind == PrimitiveKind::TooltipHelp) return {Basics, QStringLiteral("Modules")};
        return {Drawing, QStringLiteral("Views")};
    case TransformView: case ViewAttributes: case SetGuiArea: case ViewLoop:
    case ViewCache: case Redraw: case ViewEffect:
        return {Drawing, QStringLiteral("Views")};
    case MouseArea: case MouseCaptureControl: case ParseMouseMessage: case MouseMove: case MouseInfo: case MouseDrag: case ShowCursor:
        return {Drawing, QStringLiteral("Mouse")};
    case AreaGeometry:
        return {Drawing, QStringLiteral("Geometry")};
    case ParseSfz: return {Audio, QStringLiteral("Streams & DSP")};
    case LoadBitmap: case LoadBitmapArray: case BitmapOperation: case BitmapCanvas: case BitmapMetadata:
        return {Drawing, QStringLiteral("Bitmaps")};
    case ParseColorMatrix: case PenInfo: case ColorMatrixSet: case ApplyColorMatrix: case SplitColor: case MakeColor:
    case MakePen: case MakeSolid: case MakeBrush: case MakeFont: case FontAttributes:
    case EmbedFonts: case MakeStringFormat: case StringFormatAttributes: case MeasureText:
        return {Drawing, QStringLiteral("Color & Text")};
    }
    return {Basics, QStringLiteral("Values")};
}

desktop::LibraryModel::Entry libraryEntry(QVariantMap item, desktop::LibraryModel::Category category, const auto& inputs,
                                           const auto& outputs, bool insideOnly = false)
{
    QStringList types;
    const auto collect = [&](const auto& ports) {
        for (const auto& port : ports) {
            const auto type = portDescription(port, 0).value("type").toString();
            if (!types.contains(type)) types.append(type);
        }
    };
    collect(inputs);
    collect(outputs);
    const auto search = (item.value("title").toString() + u' ' + item.value("symbol").toString()).toCaseFolded();
    return {std::move(item), search, std::move(category), std::move(types), insideOnly};
}

}

QVariantList SchematicModel::libraryConnectors() const
{
    QVariantList connectors;
    const auto append = [&](std::optional<Type> type) {
        auto descriptor = portDescription({"", type}, 0);
        if (library_.types().contains(descriptor.value("type").toString())) {
            connectors.push_back(std::move(descriptor));
        }
    };
    append(std::nullopt);
    constexpr std::array greenTypes{Type::Trigger, Type::Boolean, Type::Float, Type::Int, Type::String, Type::Point,
                                   Type::FloatArray, Type::IntArray, Type::StringArray, Type::PointArray};
    for (auto type : greenTypes) append(type);
    for (const auto& connector : alphaConnectors) append(connector.type);
    constexpr std::array yellowTypes{Type::View, Type::Area, Type::MouseMessage, Type::Color, Type::ColorMatrix,
                                     Type::PaintTool, Type::Font, Type::StringFormat, Type::Bitmap, Type::BitmapArray};
    for (auto type : yellowTypes) append(type);
    append(Type::Bus);
    return connectors;
}

QVariantMap SchematicModel::primitiveShortcut(const QString& text) const
{
    const auto found = std::ranges::find(libraryShortcuts, QStringView(text.toUpper()), &LibraryShortcut::key);
    if (found == std::end(libraryShortcuts) || !graph().canPlace(found->kind)) return {};
    return describe(primitive(found->kind));
}

void SchematicModel::rememberPlacement(QString key)
{
    auto entries = library_.recentlyUsed();
    entries.removeAll(key);
    entries.prepend(std::move(key));
    if (entries.size() > desktop::SchematicFile::recentlyUsedLimit) entries.removeLast();
    updateRecentlyUsed(std::move(entries));
}

void SchematicModel::updateRecentlyUsed(QStringList entries)
{
    if (!recentlyUsedDirty_ && entries == library_.recentlyUsed()) return;
    library_.setRecentlyUsed(std::move(entries));
    try {
        desktop::SchematicFile::saveRecentlyUsed(recentlyUsedPath_, library_.recentlyUsed());
        recentlyUsedDirty_ = false;
    } catch (const std::exception& error) {
        recentlyUsedDirty_ = true;
        setFeedback(tr("Could not save Recently Used. %1").arg(QString::fromUtf8(error.what())));
    }
}

void SchematicModel::rebuildLibrary()
{
    if (builtIns_.empty()) {
        for (const auto& spec : primitives()) {
            auto item = describe(spec);
            if (const auto hotkey = std::ranges::find(libraryShortcuts, spec.kind, &LibraryShortcut::kind);
                hotkey != std::end(libraryShortcuts)) {
                item.insert("hotkey", hotkey->key.toString());
            }
            const auto inputs = isDspEditor(spec.kind)
                ? std::vector<Port>{{"Input", Type::Stream}} : spec.inputs;
            const auto outputs = isDspEditor(spec.kind)
                ? std::vector<Port>{{"Output", Type::Stream}} : spec.outputs;
            builtIns_.push_back(libraryEntry(std::move(item), libraryCategory(spec), inputs, outputs,
                                            !session_.graphAt(0).canPlace(spec.kind)));
            auto& entry = builtIns_.back();
            if (spec.kind == PrimitiveKind::StreamToGreen) {
                entry.types.append({"Stream", "Mono", "StreamBoolean", "MonoBoolean", "Float", "Boolean"});
            }
            if (isIndexedRouter(spec.kind)) entry.types.append({"Trigger", "Bus"});
            if (spec.kind == PrimitiveKind::Matrix || isIndexedRouter(spec.kind)) {
                for (const auto& connector : alphaConnectors) {
                    if (!Schematic::supportsTemplateType(spec.kind, connector.type)) continue;
                    const auto label = portDescription({"", connector.type}, 0).value("type").toString();
                    if (!entry.types.contains(label)) entry.types.append(label);
                }
            }
            const auto routedType = spec.behavior == PrimitiveBehavior::StreamRouting && !spec.outputs.empty()
                ? spec.outputs.front().type : std::nullopt;
            const auto numericType = routedType == Type::StreamBoolean && !spec.inputs.empty()
                ? spec.inputs.front().type : routedType;
            if (spec.behavior == PrimitiveBehavior::StreamRouting &&
                (routedType == Type::StreamBoolean || std::ranges::any_of(spec.inputs, [](const Port& input) {
                    return input.type == Type::StreamBoolean;
                }))) {
                entry.types.append({"MonoBoolean", "PolyBoolean"});
            }
            const bool singlePrecisionStream = numericType == Type::Stream;
            const bool doublePrecisionStream = numericType == Type::StreamDouble;
            if (supportsStreamMath(spec) || spec.behavior == PrimitiveBehavior::StreamConversion ||
                singlePrecisionStream || doublePrecisionStream) {
                for (const auto& connector : alphaConnectors) {
                    const auto family = numericStreamFamily(connector.type);
                    if (!family || (singlePrecisionStream && family != Type::Stream) ||
                        (doublePrecisionStream && family != Type::StreamDouble)) {
                        continue;
                    }
                    if (!entry.types.contains(qtext(connector.label))) entry.types.append(qtext(connector.label));
                }
            }
            const bool arrayElements = spec.behavior == PrimitiveBehavior::StoreArray ||
                spec.kind == PrimitiveKind::ArraySplitter || spec.kind == PrimitiveKind::ArrayLoop ||
                spec.kind == PrimitiveKind::ArrayInterpolatedGet || spec.kind == PrimitiveKind::ArraySum;
            if (arrayElements || spec.kind == PrimitiveKind::SortArray || spec.kind == PrimitiveKind::ArrayLength ||
                (spec.behavior == PrimitiveBehavior::GreenLogic && spec.hasTemplateConnectors()) ||
                spec.behavior == PrimitiveBehavior::Compare || spec.behavior == PrimitiveBehavior::UnaryMath ||
                spec.behavior == PrimitiveBehavior::NoteConversion ||
                spec.kind == PrimitiveKind::Power || spec.kind == PrimitiveKind::Logarithm ||
                spec.kind == PrimitiveKind::Hypotenuse || spec.kind == PrimitiveKind::Loop) {
                constexpr std::array types{Type::Float, Type::Int, Type::String, Type::Point, Type::Boolean,
                                           Type::FloatArray, Type::IntArray, Type::StringArray, Type::PointArray};
                for (const auto type : types) {
                    if (!arrayElements && !Schematic::supportsTemplateType(spec.kind, type)) continue;
                    const auto label = portDescription({"", type}, 0).value("type").toString();
                    if (!entry.types.contains(label)) entry.types.append(label);
                }
                if (spec.behavior == PrimitiveBehavior::NoteConversion) {
                    const auto names = spec.kind == PrimitiveKind::IntToNoteName
                        ? std::array{Type::String, Type::StringArray} : std::array{Type::Int, Type::IntArray};
                    for (const auto type : names) entry.types.append(portDescription({"", type}, 0).value("type").toString());
                }
            }
            entry.recentKey = primitiveHistoryKey(spec.kind);
        }
    }
    auto entries = builtIns_;
    for (auto& entry : entries) {
        entry.favorite = session_.primitiveFavorite(static_cast<PrimitiveKind>(entry.item.value("kind").toInt()));
    }
    const auto names = session_.toolboxNames();
    for (std::size_t index = 0; index < names.size(); ++index) {
        auto item = describe(primitive(PrimitiveKind::Module));
        item.insert("title", qtext(names[index]));
        item.insert("toolboxIndex", static_cast<int>(index));
        const auto group = qtext(session_.toolboxGroup(index));
        item.insert("toolboxGroup", group);
        const auto& module = session_.toolboxModule(index);
        const desktop::LibraryModel::Category category{desktop::LibraryModel::Section::Toolbox,
            group.isEmpty() ? tr("Toolbox") : tr("Toolbox / %1").arg(group),
            group.isEmpty() ? tr("Saved modules") : group};
        entries.push_back(libraryEntry(std::move(item), category,
                                       module.inputPorts(), module.outputPorts()));
        entries.back().favorite = session_.toolboxModuleFavorite(index);
        entries.back().recentKey = moduleHistoryKey(session_.toolboxModuleId(index));
    }
    library_.replace(std::move(entries));
    emit toolboxChanged();
}

void SchematicModel::addModuleToToolbox(int id, const QString& group)
{
    try {
        desktop::SchematicFile::addToolboxModule(session_, toolboxPath_, id, group.trimmed().toStdString());
        toolboxLoaded_ = true;
        rebuildLibrary();
        setFeedback(QStringLiteral("Module added to toolbox"));
    } catch (const std::exception& error) {
        setFeedback(QString::fromUtf8(error.what()));
    }
}

void SchematicModel::editToolboxModule(int index, const QString& name, const QString& group)
{
    try {
        desktop::SchematicFile::editToolboxModule(session_, toolboxPath_, index, name.trimmed().toStdString(),
                                                 group.trimmed().toStdString());
        rebuildLibrary();
        setFeedback(tr("Saved module updated"));
    } catch (const std::exception& error) {
        setFeedback(QString::fromUtf8(error.what()));
    }
}

void SchematicModel::removeToolboxModule(int index)
{
    try {
        auto entries = library_.recentlyUsed();
        entries.removeAll(moduleHistoryKey(session_.toolboxModuleId(index)));
        desktop::SchematicFile::removeToolboxModule(session_, toolboxPath_, index);
        rebuildLibrary();
        setFeedback(tr("Saved module removed"));
        updateRecentlyUsed(std::move(entries));
    } catch (const std::exception& error) {
        setFeedback(QString::fromUtf8(error.what()));
    }
}

bool SchematicModel::hasUnsavedChanges() const
{
    try {
        return desktop::SchematicFile::hasUnsavedChanges(session_, savedProject_);
    } catch (const std::exception&) {
        return true;
    }
}

bool SchematicModel::saveProject(const QUrl& file)
{
    try {
        if (!file.isLocalFile()) throw std::invalid_argument("Choose a local schematic file");
        savedProject_ = desktop::SchematicFile::saveProject(session_, file.toLocalFile());
        projectFile_ = file;
        emit projectChanged();
        setFeedback(tr("Schematic saved"));
        try {
            graph().setSchematicFilePath(QFileInfo(file.toLocalFile()).absoluteFilePath().toStdString());
            refreshValues();
        } catch (const std::exception& error) {
            setFeedback(tr("Schematic saved. A path trigger failed. %1").arg(QString::fromUtf8(error.what())));
        }
        return true;
    } catch (const std::exception& error) {
        setFeedback(tr("Could not save schematic. %1").arg(QString::fromUtf8(error.what())));
        return false;
    }
}

bool SchematicModel::replaceProject(const QByteArray& data, const QUrl& url)
{
    if (closePending()) return false;
    pendingFlowStoneImport_ = desktop::FlowStoneFile::isArchive(data) ||
        QFileInfo(url.toLocalFile()).suffix().compare("fsm", Qt::CaseInsensitive) == 0;
    pendingImportWarnings_.clear();
    pendingProjectFile_ = pendingFlowStoneImport_ ? QUrl{} : url;
    emit closePendingChanged();
    try {
        const auto beforeReplace = [this] {
            stopAudio();
            finishPointerDrag(false);
            setCursorOverride({});
            renderedPanels_.clear();
            cachedDrawingDescriptions_.clear();
            beginResetModel();
        };
        const bool opened = pendingFlowStoneImport_
            ? desktop::SchematicFile::importFlowStone(session_, data, beforeReplace, &pendingImportWarnings_)
            : desktop::SchematicFile::loadProject(session_, data, beforeReplace,
                url.isLocalFile() ? QFileInfo(url.toLocalFile()).absoluteFilePath() : QString{});
        if (opened) {
            finishProjectReplacement();
        } else {
            refreshClosingSchematic();
        }
        return true;
    } catch (const std::exception& error) {
        reportProjectOpenError(error);
        return false;
    }
}

void SchematicModel::finishProjectReplacement()
{
    emit historyChanged();
    endSchematicChange();
    projectFile_ = *pendingProjectFile_;
    savedProject_ = pendingFlowStoneImport_ ? QByteArray{} : desktop::SchematicFile::project(session_);
    if (pendingFlowStoneImport_) {
        setFeedback(tr("Imported FlowStone schematic. Save as an .ostone project. %1")
            .arg(pendingImportWarnings_.join(' ')));
    }
    pendingFlowStoneImport_ = false;
    pendingImportWarnings_.clear();
    if (const auto error = lifecycleErrorText(session_); !error.isEmpty()) {
        setFeedback(tr("Schematic lifecycle trigger failed. %1").arg(error));
    }
    pendingProjectFile_.reset();
    emit closePendingChanged();
    emit projectChanged();
}

void SchematicModel::reportProjectOpenError(const std::exception& error)
{
    pendingFlowStoneImport_ = false;
    pendingImportWarnings_.clear();
    pendingProjectFile_.reset();
    emit closePendingChanged();
    setFeedback(tr("Could not open schematic. %1").arg(QString::fromUtf8(error.what())));
}

bool SchematicModel::closePending() const
{
    return pendingProjectFile_.has_value() || windowClose_ != WindowClose::None;
}

void SchematicModel::watchApplicationQuit()
{
    qApp->installEventFilter(this);
}

bool SchematicModel::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == qApp && event->type() == QEvent::Quit && windowClose_ != WindowClose::Ready) {
        event->ignore();
        emit applicationQuitRequested();
        return true;
    }
    if ((event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) &&
        std::ranges::any_of(tickWindows_, [watched](const auto& window) {
            return window && static_cast<QObject*>(window.data()) == watched && window->isVisible();
        })) {
        handleKeyboardEvent(*static_cast<QKeyEvent*>(event));
    }
    return QAbstractListModel::eventFilter(watched, event);
}

void SchematicModel::closeSchematic()
{
    if (closePending()) return;
    windowClose_ = WindowClose::Requested;
    emit closePendingChanged();
    session_.requestSchematicClose();
    refreshClosingSchematic();
}

void SchematicModel::refreshClosingSchematic()
{
    setFeedback(lifecycleErrorText(session_));
    refreshValues(true);
}

void SchematicModel::finishPendingClose()
{
    if (!session_.schematicReadyToClose()) return;
    if (pendingProjectFile_) {
        try {
            if (session_.completeSchematicOpen()) finishProjectReplacement();
        } catch (const std::exception& error) {
            reportProjectOpenError(error);
        }
    } else if (windowClose_ == WindowClose::Requested) {
        windowClose_ = WindowClose::Ready;
        midiScanTimer_.stop();
        midiPollTimer_.stop();
        graph().setMidiDeviceBackend({});
        serialPollTimer_.stop();
        graph().setSerialDeviceBackend({});
        stopAudio();
        emit schematicCloseReady();
    }
}

bool SchematicModel::openProject(const QUrl& file)
{
    if (closePending()) return false;
    try {
        if (!file.isLocalFile()) throw std::invalid_argument("Choose a local schematic file");
        return replaceProject(desktop::SchematicFile::read(file.toLocalFile()), file);
    } catch (const std::exception& error) {
        setFeedback(tr("Could not open schematic. %1").arg(QString::fromUtf8(error.what())));
        return false;
    }
}

bool SchematicModel::newProject()
{
    return replaceProject({}, {});
}

QString SchematicModel::undoText() const
{
    return historyText(false);
}

QString SchematicModel::redoText() const
{
    return historyText(true);
}

QString SchematicModel::historyText(bool redo) const
{
    const auto edit = session_.historyEdit(redo);
    if (!edit) return {};
    QString operation;
    switch (*edit) {
    case SchematicEdit::MoveNode: operation = tr("Move Node"); break;
    case SchematicEdit::MoveNodes: operation = tr("Move Nodes"); break;
    case SchematicEdit::CreateLink: operation = tr("Create Link"); break;
    case SchematicEdit::DeleteLinks: operation = tr("Delete Links"); break;
    case SchematicEdit::MakeModule: operation = tr("Make Module"); break;
    case SchematicEdit::AddNode: operation = tr("Add Node"); break;
    case SchematicEdit::DeleteNodes: operation = tr("Delete Nodes"); break;
    case SchematicEdit::CutNodes: operation = tr("Cut"); break;
    case SchematicEdit::PasteNodes: operation = tr("Paste"); break;
    case SchematicEdit::DuplicateNodes: operation = tr("Duplicate"); break;
    case SchematicEdit::AlignNodes: operation = tr("Align Nodes"); break;
    case SchematicEdit::DistributeNodes: operation = tr("Distribute Nodes"); break;
    case SchematicEdit::EnablePanel: operation = tr("Enable Panel"); break;
    case SchematicEdit::EditRuby: operation = tr("Edit Ruby"); break;
    }
    return (redo ? tr("Redo %1") : tr("Undo %1")).arg(operation);
}

void SchematicModel::undo()
{
    applyHistory(false);
}

void SchematicModel::redo()
{
    applyHistory(true);
}

bool SchematicModel::beginNodeMove(const QVariantList& ids)
{
    std::vector<NodePosition> origins;
    origins.reserve(ids.size());
    for (const auto& id : ids) {
        const auto row = rowFor(id.toInt());
        if (row < 0) return false;
        const auto& node = graph().nodes()[row];
        origins.push_back({node.id, node.x, node.y});
    }
    const auto edit = origins.size() > 1 ? SchematicEdit::MoveNodes : SchematicEdit::MoveNode;
    if (origins.empty() || !session_.beginEdit(edit)) return false;
    dragOrigins_ = std::move(origins);
    emit historyChanged();
    return true;
}

void SchematicModel::moveNodeSelection(double dx, double dy)
{
    if (dragOrigins_.empty()) return;

    auto positions = dragOrigins_;
    for (auto& position : positions) {
        position.x += dx;
        position.y += dy;
    }
    applyNodePositions(positions);
}

void SchematicModel::arrangeNodes(const QVariantList& selection, Arrangement arrangement)
{
    const bool distribute = arrangement == DistributeHorizontally || arrangement == DistributeVertically;
    const bool alongX = arrangement == AlignVertically || arrangement == DistributeHorizontally;
    if (selection.size() < (distribute ? 3 : 2)) return;

    struct Item {
        NodePosition position;
        double extent;
    };
    std::vector<Item> items;
    items.reserve(selection.size());
    for (const auto& id : selection) {
        const int row = rowFor(id.toInt());
        if (row < 0) return;
        const auto& node = graph().nodes()[row];
        if (isModuleEndpoint(node.kind)) return;
        const auto size = nodeGeometry(graph(), node);
        items.push_back({{node.id, node.x, node.y}, alongX ? size.width : size.height});
    }

    const auto start = [alongX](auto& item) -> auto& {
        return alongX ? item.position.x : item.position.y;
    };
    const auto center = [&](const Item& item) { return start(item) + item.extent / 2; };
    if (distribute) {
        std::ranges::stable_sort(items, {}, center);
        const double end = start(items.back()) + items.back().extent;
        double available = end - start(items.front());
        for (const auto& item : items) available -= item.extent;

        // Keep both outer nodes fixed when they enclose enough room for the group.
        const double gap = available > 0 ? available / (items.size() - 1) : 24.0;
        const auto limit = available > 0 ? items.size() - 1 : items.size();
        double next = start(items.front()) + items.front().extent + gap;
        for (std::size_t i = 1; i < limit; ++i) {
            start(items[i]) = next;
            next += items[i].extent + gap;
        }
    } else {
        const double alignedCenter = center(items.back());
        for (auto& item : items | std::views::take(items.size() - 1)) {
            start(item) = alignedCenter - item.extent / 2;
        }
    }

    std::vector<NodePosition> positions;
    positions.reserve(items.size());
    for (const auto& item : items) positions.push_back(item.position);
    if (!session_.beginEdit(distribute ? SchematicEdit::DistributeNodes : SchematicEdit::AlignNodes)) return;
    applyNodePositions(positions);
    finishEdit();
}

void SchematicModel::applyNodePositions(std::span<const NodePosition> positions)
{
    if (positions.empty()) return;
    int first = rowCount();
    int last = -1;
    for (const auto& position : positions) {
        const int row = rowFor(position.id);
        if (row < 0) return;
        first = std::min(first, row);
        last = std::max(last, row);
    }
    try {
        session_.moveNodes(positions);
    } catch (const std::exception& error) {
        setFeedback(QString::fromUtf8(error.what()));
    }
    emit painterChanged();
    emit dataChanged(index(first), index(last), {PositionRole});
    emit geometryChanged();
}

void SchematicModel::beginWireDeletion()
{
    if (session_.beginEdit(SchematicEdit::DeleteLinks)) emit historyChanged();
}

void SchematicModel::finishEdit()
{
    dragOrigins_.clear();
    session_.finishEdit();
    emit historyChanged();
}

void SchematicModel::applyHistory(bool redo)
{
    bool resetModel = false;
    bool audioStopped = false;
    bool resized = false;
    try {
        const bool applied = session_.applyHistory(redo, [&](HistoryUpdate update) {
            if (update == HistoryUpdate::Topology) {
                audioStopped = audioRunning();
                stopAudio();
                finishPointerDrag(false);
                setCursorOverride({});
                renderedPanels_.clear();
                cachedDrawingDescriptions_.clear();
            }
            resized = update == HistoryUpdate::Geometry;
            if (update != HistoryUpdate::Positions && !resized) {
                beginResetModel();
                resetModel = true;
            }
        });
        if (!applied) return;
        if (resetModel) {
            resetModel = false;
            endSchematicChange();
        } else {
            if (rowCount() > 0) {
                const QList<int> roles = resized ? QList<int>{PositionRole, SpecRole} : QList<int>{PositionRole};
                emit dataChanged(index(0), index(rowCount() - 1), roles);
            }
            emit geometryChanged();
            emit painterChanged();
        }
        setFeedback(audioStopped ? tr("The schematic changed. Start Audio to use the new wiring.") : QString{});
    } catch (const std::exception& error) {
        if (resetModel) endSchematicChange();
        setFeedback(QString::fromUtf8(error.what()));
    }
    emit historyChanged();
}

void SchematicModel::setStraightWires(bool enabled)
{
    if (straightWires_ == enabled) return;
    straightWires_ = enabled;
    QSettings(QStringLiteral("OpenStone"), QStringLiteral("OpenStone")).setValue(QStringLiteral("wiring/straight"), enabled);
    emit wiringPreferencesChanged();
}

void SchematicModel::setAutoLink(bool enabled)
{
    if (autoLink_ == enabled) return;
    autoLink_ = enabled;
    QSettings(QStringLiteral("OpenStone"), QStringLiteral("OpenStone")).setValue(QStringLiteral("wiring/autoLink"), enabled);
    emit wiringPreferencesChanged();
}

int SchematicModel::addToolboxModule(int index, double x, double y)
{
    const bool ownsEdit = session_.beginEdit(SchematicEdit::AddNode);
    beginResetModel();
    int id = -1;
    QString feedback;
    try {
        id = session_.insertToolboxModule(static_cast<std::size_t>(index), x, y);
    } catch (const std::exception& error) {
        feedback = QString::fromUtf8(error.what());
    }
    endSchematicChange();
    if (ownsEdit) finishEdit();
    const auto lifecycleError = lifecycleErrorText(session_);
    setFeedback(feedback.isEmpty() ? lifecycleError : std::move(feedback));
    if (id >= 0) {
        try {
            if (session_.toolboxModuleId(index).empty()) {
                desktop::SchematicFile::ensureToolboxModuleId(session_, toolboxPath_, index);
                rebuildLibrary();
            }
            rememberPlacement(moduleHistoryKey(session_.toolboxModuleId(index)));
        } catch (const std::exception& error) {
            setFeedback(tr("Could not save Recently Used. %1").arg(QString::fromUtf8(error.what())));
        }
    }
    return id;
}

QVariantList SchematicModel::wires() const
{
    QVariantList list;
    const auto& nodes = graph().nodes();
    std::map<int, std::pair<const SchematicNode*, NodeGeometry>> geometryByNode;
    const auto forNode = [&](int id) -> const auto& {
        if (const auto found = geometryByNode.find(id); found != geometryByNode.end()) return found->second;
        const auto& node = nodes[rowFor(id)];
        return geometryByNode.emplace(id, std::pair{&node, nodeGeometry(graph(), node)}).first->second;
    };
    for (auto link : graph().links()) {
        const auto& [source, sourceGeometry] = forNode(link.source);
        const auto& [target, targetGeometry] = forNode(link.target);
        const auto sourceType = graph().outputType(link.source, link.output);
        const auto targetType = graph().inputType(link.target, link.input);
        const auto* connector = alphaConnector(sourceType);
        if (!connector) {
            connector = alphaConnector(targetType);
        }
        const bool pulse = sourceType == Type::Trigger || targetType == Type::Trigger;
        const bool yellow = (sourceType && green::isYellowType(*sourceType)) ||
                            (targetType && green::isYellowType(*targetType));
        list.push_back(QVariantMap{
            {"start", QPointF(source->x + sourceGeometry.width, source->y + sourceGeometry.outputY(link.output))},
            {"end", QPointF(target->x, target->y + targetGeometry.firstInputY + link.input * targetGeometry.inputSpacing)},
            {"pulse", pulse}, {"untyped", !pulse && (!sourceType || !targetType)},
            {"yellow", yellow}, {"bus", sourceType == Type::Bus},
            {"palette", connector ? qtext(connector->palette) : QString{}},
            {"source", link.source}, {"output", link.output}, {"target", link.target}, {"input", link.input}
        });
    }
    return list;
}

void SchematicModel::disconnectWire(int source, int output, int target, int input)
{
    const bool ownsEdit = session_.beginEdit(SchematicEdit::DeleteLinks);
    try {
        session_.disconnectLink({source, target, input, output});
        setFeedback({});
        emit geometryChanged();
        refreshValues(true);
    } catch (const std::exception& error) {
        setFeedback(QString::fromUtf8(error.what()));
    }
    if (ownsEdit) finishEdit();
}

QVariantMap SchematicModel::nearbyWire(double zoom) const
{
    if (!autoLink_ || dragOrigins_.empty() || !std::isfinite(zoom) || zoom <= 0) return {};
    std::set<int> movingIds;
    for (const auto& origin : dragOrigins_) movingIds.insert(origin.id);
    std::vector<const SchematicNode*> stationary;
    for (const auto& node : graph().nodes()) {
        if (!movingIds.contains(node.id)) stationary.push_back(&node);
    }
    if (stationary.empty()) return {};

    const double maxGap = 84 / zoom;
    const double maxOffset = 9 / zoom;
    std::set<std::pair<int, int>> wiredInputs;
    for (const auto link : graph().links()) wiredInputs.emplace(link.target, link.input);
    QVariantMap best;
    double bestDistance = std::numeric_limits<double>::infinity();
    const auto consider = [&](const SchematicNode& source, const NodeGeometry& from,
                              const SchematicNode& target, const NodeGeometry& to) {
        const double gap = target.x - (source.x + from.width);
        if (gap < 10 / zoom || gap > maxGap) return;
        const auto outputCount = source.kind == PrimitiveKind::ArraySplitter
            ? visibleSplitterOutputs(graph(), source.id) : source.outputPorts().size();
        const auto inputs = target.inputPorts();
        for (int output = 0; output < static_cast<int>(outputCount); ++output) {
            const double startY = source.y + from.outputY(output);
            const auto fromType = graph().outputType(source.id, output);
            for (int input = 0; input < static_cast<int>(inputs.size()); ++input) {
                const double endY = target.y + to.firstInputY + input * to.inputSpacing;
                const double offset = std::abs(startY - endY);
                const auto distance = gap + 3 * offset;
                if (offset > maxOffset || distance >= bestDistance || wiredInputs.contains({target.id, input})) continue;
                const auto toType = graph().inputType(target.id, input);
                const auto canMatch = [](auto a, auto b) {
                    if (a == b || !a || !b) return true;
                    return (*a == Type::Stream && (*b == Type::Mono || *b == Type::Mono4 || *b == Type::Poly)) ||
                           (*a == Type::StreamDouble && (*b == Type::MonoDouble || *b == Type::Mono4Double || *b == Type::PolyDouble));
                };
                if (!canMatch(fromType, toType) && !canMatch(toType, fromType)) continue;
                if (!graph().canSuggestLink({source.id, target.id, input, output})) continue;
                bestDistance = distance;
                best = {{"source", source.id}, {"output", output}, {"target", target.id}, {"input", input},
                        {"start", QPointF(source.x + from.width, startY)}, {"end", QPointF(target.x, endY)}};
            }
        }
    };
    std::map<int, NodeGeometry> sizes;
    for (const auto& origin : dragOrigins_) {
        const auto row = rowFor(origin.id);
        if (row < 0) continue;
        const auto& moving = graph().nodes()[row];
        const auto movingSize = nodeGeometry(graph(), moving);
        for (const auto* other : stationary) {
            if (std::abs(other->x - moving.x) > 2000 + maxGap ||
                std::abs(other->y - moving.y) > 2000 + maxOffset) continue;
            const auto [size, inserted] = sizes.try_emplace(other->id);
            if (inserted) size->second = nodeGeometry(graph(), *other);
            consider(moving, movingSize, *other, size->second);
            consider(*other, size->second, moving, movingSize);
        }
    }
    return best;
}

qulonglong SchematicModel::triggerCount() const
{
    return graph().triggers();
}

int SchematicModel::addNode(int kind, double x, double y)
{
    if (!std::isfinite(x) || !std::isfinite(y) ||
        std::ranges::none_of(primitives(), [kind](const auto& spec) { return static_cast<int>(spec.kind) == kind; })) {
        return -1;
    }
    const auto primitiveKind = static_cast<PrimitiveKind>(kind);
    if (!graph().canPlace(primitiveKind)) {
        setFeedback(QStringLiteral("%1 can only be placed inside a module").arg(qtext(primitive(primitiveKind).name)));
        return -1;
    }
    const bool ownsEdit = session_.beginEdit(SchematicEdit::AddNode);
    const int row = rowCount();
    beginInsertRows({}, row, row);
    const int id = session_.addNode(primitiveKind, x, y);
    endInsertRows();
    emit painterChanged();
    setFeedback({});
    if (primitiveKind == PrimitiveKind::Tick) {
        processTicks(TickWake::Configure);
    }
    emit geometryChanged();
    if (ownsEdit) finishEdit();
    if (id >= 0) rememberPlacement(primitiveHistoryKey(primitiveKind));
    return id;
}

int SchematicModel::makeModule(const QVariantList& selection)
{
    std::vector<int> ids;
    ids.reserve(selection.size());
    for (const auto& item : selection) {
        ids.push_back(item.toInt());
    }
    const bool ownsEdit = session_.beginEdit(SchematicEdit::MakeModule);
    beginResetModel();
    int id = -1;
    QString error;
    try {
        id = session_.makeModule(ids);
    } catch (const std::exception& exception) {
        error = QString::fromUtf8(exception.what());
    }
    endSchematicChange();
    if (ownsEdit) finishEdit();
    setFeedback(std::move(error));
    return id;
}

void SchematicModel::moveNode(int id, double x, double y)
{
    const auto row = rowFor(id);
    if (row < 0) {
        return;
    }
    const bool ownsEdit = session_.beginEdit(SchematicEdit::MoveNode);
    try {
        session_.moveNode(id, x, y);
    } catch (const std::exception& error) {
        setFeedback(QString::fromUtf8(error.what()));
    }
    emit painterChanged();
    emit dataChanged(index(row), index(row), {PositionRole});
    emit geometryChanged();
    if (ownsEdit) finishEdit();
}

void SchematicModel::removeNode(int id)
{
    const auto row = rowFor(id);
    if (row < 0) {
        return;
    }
    const bool ownsEdit = session_.beginEdit(SchematicEdit::DeleteNodes);
    beginRemoveRows({}, row, row);
    try {
        session_.removeNode(id);
        setFeedback({});
    } catch (const std::exception& error) {
        setFeedback(QString::fromUtf8(error.what()));
    }
    endRemoveRows();
    emit clipboardChanged();
    emit geometryChanged();
    refreshValues(true);
    if (ownsEdit) finishEdit();
}

void SchematicModel::removeNodes(const QVariantList& ids)
{
    const bool ownsEdit = session_.beginEdit(SchematicEdit::DeleteNodes);
    for (const auto& id : ids) removeNode(id.toInt());
    if (ownsEdit) finishEdit();
}

bool SchematicModel::connectNodes(int from, int output, int to, int input)
{
    const bool ownsEdit = session_.beginEdit(SchematicEdit::CreateLink);
    const auto previousLinks = graph().links().size();
    try {
        session_.connect({from, to, input, output});
        setFeedback({});
    } catch (const std::exception& error) {
        const auto message = QString::fromUtf8(error.what());
        if (graph().links().size() == previousLinks) {
            if (ownsEdit) finishEdit();
            setFeedback(message);
            return false;
        }
        setFeedback(QStringLiteral("Link added. %1").arg(message));
    }
    emit geometryChanged();
    refreshValues(true);
    if (ownsEdit) finishEdit();
    return true;
}

void SchematicModel::disconnectInput(int id, int input)
{
    const bool ownsEdit = session_.beginEdit(SchematicEdit::DeleteLinks);
    try {
        session_.disconnectInput(id, input);
        setFeedback({});
    } catch (const std::exception& error) {
        setFeedback(QString::fromUtf8(error.what()));
    }
    emit geometryChanged();
    refreshValues(true);
    if (ownsEdit) finishEdit();
}

void SchematicModel::setTemplateType(int id, int type)
{
    const auto row = rowFor(id);
    if (row < 0) {
        return;
    }
    try {
        session_.setTemplateType(id, type == 0 ? std::nullopt : std::optional{static_cast<Type>(type)});
        setFeedback({});
        emit painterChanged();
        emit geometryChanged();
        refreshValues(true);
    } catch (const std::exception& error) {
        setFeedback(QString::fromUtf8(error.what()));
    }
}

void SchematicModel::setRubyDraft(int id, const QString& text)
{
    if (const int row = rowFor(id); row >= 0 && graph().nodes()[row].rubyCode)
        graph().setRubyDraft(id, text.toStdString());
}

QVariantList SchematicModel::rubyPortTypes(bool output, int selected) const
{
    return templateTypeGroups(PrimitiveKind::Ruby, static_cast<Type>(selected), output);
}

bool SchematicModel::editRubyPort(int id, bool output, int port, const QString& command, int value, const QString& name)
{
    constexpr std::array operations{
        std::pair{"count", RubyPortEdit::Count}, std::pair{"name", RubyPortEdit::Name},
        std::pair{"type", RubyPortEdit::Type}, std::pair{"insert", RubyPortEdit::Insert},
        std::pair{"remove", RubyPortEdit::Remove}, std::pair{"move", RubyPortEdit::Move},
    };
    const auto operation = std::ranges::find_if(operations, [&](const auto& entry) { return command == entry.first; });
    if (operation == operations.end()) return false;

    const bool ownsEdit = session_.beginEdit(SchematicEdit::EditRuby);
    bool success = true;
    try {
        graph().editRubyPort(id, output, port, operation->second, value, name.toStdString());
        setFeedback({});
    } catch (const std::exception& error) {
        setFeedback(QString::fromUtf8(error.what()));
        success = false;
    }
    if (ownsEdit) finishEdit();
    if (success) {
        emit geometryChanged();
        refreshValues(true);
    }
    return success;
}

void SchematicModel::resizeRubyBox(int id, int width, int height)
{
    const auto row = rowFor(id);
    if (row < 0 || !graph().nodes()[row].rubyCode) return;
    const bool ownsEdit = session_.beginEdit(SchematicEdit::EditRuby);
    graph().resizeRubyBox(id, {width, height});
    if (ownsEdit) finishEdit();
    emit geometryChanged();
    refreshValues(true);
}

QString SchematicModel::runRuby(int id, bool portsOnly)
{
    const bool ownsEdit = session_.beginEdit(SchematicEdit::EditRuby);
    QString error;
    try {
        if (portsOnly) graph().applyRubyPorts(id);
        else graph().runRuby(id);
    } catch (const std::exception& failure) {
        error = QString::fromUtf8(failure.what());
    }
    if (ownsEdit) finishEdit();
    emit geometryChanged();
    refreshValues(true);
    return error;
}

void SchematicModel::stopRuby(int id)
{
    if (const int row = rowFor(id); row >= 0 && graph().nodes()[row].rubyCode) {
        graph().stopRuby(id);
        refreshValues();
    }
}

void SchematicModel::setDspDraft(int id, const QString& text)
{
    const int row = rowFor(id);
    if (row >= 0 && graph().nodes()[row].dspCode) {
        graph().setDspDraft(id, text.toUtf8().toStdString());
    }
}

QVariantMap SchematicModel::compileDsp(int id)
{
    try {
        session_.compileDsp(id);
        setFeedback({});
        emit geometryChanged();
        refreshValues(true);
        return {};
    } catch (const stream::DspCompileError& error) {
        return {{"message", QString::fromUtf8(error.what())},
                {"line", static_cast<int>(error.line)}, {"column", static_cast<int>(error.column)}};
    } catch (const std::exception& error) {
        return {{"message", QString::fromUtf8(error.what())}};
    }
}

QVariantMap SchematicModel::captureDsp(int id, int frames, int blockSize)
{
    try {
        const auto samples = graph().captureDsp(id, frames, blockSize);
        QString contents = QStringLiteral("Sample");
        const auto ports = graph().nodes().at(rowFor(id)).outputPorts();
        for (const auto& port : ports) {
            contents += u'\t' + qtext(port.name);
        }
        for (int frame = 0; frame < frames; ++frame) {
            contents += u'\n' + QString::number(frame);
            for (const auto& output : samples) {
                contents += u'\t' + formatDspSample(output[frame]);
            }
        }
        emit dspCaptureChanged();
        return {{"samples", contents},
                {"backend", stream::defaultDspBackend() == stream::DspBackend::LlvmOrc
                    ? QStringLiteral("LLVM ORC") : QStringLiteral("Interpreter")}};
    } catch (const std::exception& error) {
        return {{"message", QString::fromUtf8(error.what())}};
    }
}

void SchematicModel::setText(int id, const QString& text)
{
    const auto row = rowFor(id);
    if (row < 0) {
        return;
    }
    try {
        if (graph().nodes()[row].kind == PrimitiveKind::Tick) {
            bool parsed = false;
            const auto rate = text.toFloat(&parsed);
            if (!parsed) {
                throw std::invalid_argument("Enter ticks per second");
            }
            session_.setTickRate(id, rate);
        } else {
            const auto type = graph().outputType(id);
            Value value = text.toUtf8().toStdString();
            if (type != Type::String) {
                auto convert = type ? green::findValueConverter(Type::String, *type) : nullptr;
                if (!convert) {
                    throw std::invalid_argument("Choose an editable value");
                }
                value = convert(value);
            }
            session_.setValue(id, value);
        }
        setFeedback({});
    } catch (const std::exception& error) {
        setFeedback(QString::fromUtf8(error.what()));
    }
    refreshValues();
}

void SchematicModel::setColor(int id, const QColor& color)
{
    if (rowFor(id) < 0) {
        return;
    }
    try {
        if (!color.isValid()) {
            throw std::invalid_argument("Choose a valid color");
        }
        session_.setValue(id, yellow::Color{color.rgba()});
        setFeedback({});
    } catch (const std::exception& error) {
        setFeedback(QString::fromUtf8(error.what()));
    }
    refreshValues();
}

void SchematicModel::fire(int id)
{
    try {
        graph().fireTrigger(id);
        setFeedback({});
    } catch (const std::exception& error) {
        setFeedback(QString::fromUtf8(error.what()));
    }
    refreshValues();
}

void SchematicModel::setSystemSetting(int id, int index)
{
    try {
        session_.setSystemSetting(id, index);
        setFeedback({});
    } catch (const std::exception& error) {
        setFeedback(QString::fromUtf8(error.what()));
    }
    refreshValues(true);
}

void SchematicModel::setTickDisplaySync(int id, bool enabled)
{
    try {
        session_.setTickDisplaySync(id, enabled);
        setFeedback({});
    } catch (const std::exception& error) {
        setFeedback(QString::fromUtf8(error.what()));
    }
    refreshValues(true);
}

void SchematicModel::scheduleDeferredTriggers()
{
    if (windowClose_ != WindowClose::Ready && !deferredTriggersQueued_ && graph().hasDeferredTriggers()) {
        deferredTriggersQueued_ = true;
        QMetaObject::invokeMethod(this, &SchematicModel::processDeferredTriggers, Qt::QueuedConnection);
    }
}

void SchematicModel::processDeferredTriggers()
{
    if (windowClose_ == WindowClose::Ready) return;
    const auto result = graph().dispatchDeferredTriggers();
    deferredTriggersQueued_ = false;

    if (result.error) {
        try {
            std::rethrow_exception(result.error);
        } catch (const std::exception& error) {
            setFeedback(QString::fromUtf8(error.what()));
        }
    }
    if (result.fired) refreshValues();
    scheduleDeferredTriggers();
}

void SchematicModel::processTicks(TickWake wake)
{
    using namespace std::chrono;
    const auto schedule = graph().advanceTicks(steady_clock::now(), wake);
    if (schedule.error) {
        try {
            std::rethrow_exception(schedule.error);
        } catch (const std::exception& error) {
            setFeedback(QString::fromUtf8(error.what()));
        }
    }
    if (schedule.fired) {
        refreshValues();
        return;
    }

    if (!schedule.deadline) {
        tickTimer_.stop();
        armedTick_.reset();
    } else if (schedule.deadline != armedTick_ || !tickTimer_.isActive()) {
        const auto remaining = std::clamp(duration_cast<duration<double>>(*schedule.deadline - steady_clock::now()),
                                          duration<double>(nanoseconds(1)), duration<double>(hours(24)));
        tickTimer_.stop();
        tickTimer_.setInterval(ceil<nanoseconds>(remaining));
        tickTimer_.start();
        armedTick_ = schedule.deadline;
    }
    displayTicks_ = schedule.needsDisplay;
    updateDisplayTickWindow();
}

void SchematicModel::registerTickWindow(QQuickWindow* window)
{
    if (!window || std::ranges::any_of(tickWindows_, [window](const auto& tracked) { return tracked == window; })) {
        return;
    }
    tickWindows_.emplace_back(window);
    connect(window, &QWindow::visibilityChanged, this, &SchematicModel::updateDisplayTickWindow);
    connect(window, &QObject::destroyed, this, [this] {
        std::erase_if(tickWindows_, [](const auto& window) { return window.isNull(); });
        updateDisplayTickWindow();
    });
    updateDisplayTickWindow();
}

void SchematicModel::updateDisplayTickWindow()
{
    const auto visible = std::ranges::find_if(tickWindows_, [](const auto& window) {
        return window && window->isVisible() && window->visibility() != QWindow::Minimized;
    });
    const auto window = displayTicks_ && visible != tickWindows_.end() ? *visible : QPointer<QQuickWindow>{};
    if (tickFrameWindow_ != window) {
        disconnect(tickFrameConnection_);
        tickFrameWindow_ = window;
        if (window) {
            tickFrameConnection_ = connect(window, &QQuickWindow::frameSwapped, this, [this, window] {
                if (window && window == tickFrameWindow_ && window->isExposed()) {
                    processTicks(TickWake::Display);
                }
            }, Qt::QueuedConnection);
        }
    }
    if (tickFrameWindow_) {
        tickFrameWindow_->update();
    }
}

QString SchematicModel::windowForView(yellow::View view) const
{
    const auto focusKey = focusedPanel_.value("key").toString();
    while (view.panel != 0) {
        if (!focusKey.isEmpty() && viewKey(view) == focusKey) {
            return focusKey;
        }
        const auto* owner = session_.findGraph(view.schematicPath);
        if (!owner) {
            break;
        }
        const auto entry = std::ranges::find(owner->nodes(), view.panel, &SchematicNode::id);
        if (entry == owner->nodes().end() || !ownsPanel(entry->kind)) {
            break;
        }
        if (entry->kind == PrimitiveKind::Window) {
            return viewKey(view);
        }
        view = owner->readPanel(view.panel).parentView;
    }
    return {};
}

void SchematicModel::reportWindowVisible(const QVariantList& path, int id, bool visible)
{
    if (auto* owner = session_.findGraph(nodePath(path)); owner && owner->reportWindowVisible(id, visible)) {
        refreshValues();
    }
}

void SchematicModel::reportWindowSize(const QVariantList& path, int id, int width, int height)
{
    if (auto* owner = session_.findGraph(nodePath(path)); owner && owner->reportWindowSize(id, width, height)) {
        refreshValues();
    }
}

QVariantMap SchematicModel::dropList() const
{
    const auto& request = graph().pendingDropList();
    if (!request) {
        return {};
    }
    QVariantList menus;
    menus.reserve(request->contents.menus.size());
    for (const auto& menu : request->contents.menus) {
        QVariantList entries;
        entries.reserve(menu.size());
        for (const auto& entry : menu) {
            entries.push_back(QVariantMap{{"text", qtext(entry.text)}, {"optionIndex", entry.optionIndex},
                                          {"submenu", entry.submenu}});
        }
        menus.push_back(std::move(entries));
    }
    const bool indexSelector = request->kind == PrimitiveKind::IndexSelector;
    const auto position = indexSelector ? request->position : request->view.mapping.toPanel(request->position);
    return {{"token", QString::number(request->token)}, {"key", indexSelector ? QString{} : viewKey(request->view)},
            {"position", QPointF(position.x, position.y)}, {"menus", menus}, {"windowKey", windowForView(request->view)},
            {"checkedIndex", request->checkedIndex}, {"disabledIndex", request->disabledIndex},
            {"scroll", request->layout == DropListLayout::Scroll},
            {"rowsPerColumn", request->rowsPerColumn}, {"panelList", request->kind == PrimitiveKind::PopupList}};
}

void SchematicModel::openIndexSelector(int id, const QPointF& position)
{
    try {
        if (graph().openIndexSelector(id, {static_cast<float>(position.x()), static_cast<float>(position.y())})) {
            emit valuesChanged();
        }
    } catch (const std::exception& error) {
        setFeedback(QString::fromUtf8(error.what()));
    }
}

QVariantMap SchematicModel::openPopupList(const QVariantList& ownerPath, int id)
{
    if (auto* owner = session_.findGraph(nodePath(ownerPath))) {
        try {
            if (owner->openPopupList(id)) {
                return dropList();
            }
        } catch (const std::exception& error) {
            setFeedback(QString::fromUtf8(error.what()));
        }
    }
    return {};
}

QVariantMap SchematicModel::appDialog() const
{
    const auto* request = graph().pendingAppDialog();
    if (!request) return {};
    if (request->kind == PrimitiveKind::ColorDialog) {
        return {{"token", QString::number(request->token)}, {"kind", "color"},
                {"color", QColor::fromRgb(request->initialColor.argb | 0xff000000U)}};
    }
    const bool messageBox = request->kind == PrimitiveKind::MessageBox || request->rubyMessageBox.has_value();
    const bool folderDialog = request->kind == PrimitiveKind::FolderDialog;
    QVariantMap result{{"token", QString::number(request->token)},
                       {"kind", messageBox ? "message" : folderDialog ? "folder" : "file"},
                       {"title", qtext(request->title)}, {"type", request->type}};
    if (messageBox) {
        result.insert("message", qtext(request->message));
        result.insert("icon", request->icon);
        QStringList buttons;
        if (const auto& ruby = request->rubyMessageBox) {
            for (const auto button : ruby->buttons) buttons.push_back(qtext(button));
            result.insert("rubyMessage", true);
            result.insert("defaultButton", static_cast<int>(ruby->defaultButton));
        } else if (request->type == 2) {
            buttons = {"no", "yes"};
        } else if (request->type == 1) {
            buttons = {"cancel", "ok"};
        } else {
            buttons = {"ok"};
        }
        result.insert("buttons", buttons);
        return result;
    }

    const auto directory = request->folder.empty() ? QDir::home() : QDir(qtext(request->folder));
    result.insert("folder", QUrl::fromLocalFile(directory.absolutePath()));
    if (folderDialog) return result;

    QStringList nameFilters;
    const auto fields = qtext(request->filter).split(u'|');
    for (qsizetype i = 0; i + 1 < fields.size(); i += 2) {
        auto label = fields[i].trimmed();
        if (label.isEmpty()) break;
        auto patterns = fields[i + 1].split(u';', Qt::SkipEmptyParts);
        for (auto& pattern : patterns) {
            pattern = pattern.trimmed();
            if (pattern == QStringLiteral("*.*")) pattern = QStringLiteral("*");
        }
        patterns.removeAll(QString{});
        if (patterns.isEmpty()) continue;
        const auto detail = label.lastIndexOf(QStringLiteral(" ("));
        if (detail >= 0 && label.endsWith(u')') && QStringView(label).sliced(detail).contains(u'*')) {
            label.truncate(detail);
        }
        nameFilters.push_back(QStringLiteral("%1 (%2)").arg(label, patterns.join(u' ')));
    }
    if (nameFilters.isEmpty()) nameFilters.push_back(tr("All files (*)"));
    result.insert("filters", nameFilters);
    result.insert("suffix", qtext(request->suffix));
    result.insert("file", request->name.empty() ? QUrl{} : QUrl::fromLocalFile(directory.absoluteFilePath(qtext(request->name))));
    return result;
}

QQuickWindow* SchematicModel::appDialogWindow() const
{
    const auto* focus = qGuiApp->focusWindow();
    QQuickWindow* visible = nullptr;
    for (const auto& window : tickWindows_) {
        if (!window || !window->isVisible()) continue;
        if (window == focus) return window.data();
        if (!visible) visible = window.data();
    }
    return visible;
}

void SchematicModel::finishAppDialog(const QString& token, bool accepted, const QUrl& path)
{
    bool ok = false;
    const auto id = token.toULongLong(&ok);
    if (!ok) return;
    try {
        if (!graph().finishAppDialog(id, accepted, path.toLocalFile().toStdString())) return;
    } catch (const std::exception& error) {
        setFeedback(tr("Dialog action failed. %1").arg(QString::fromUtf8(error.what())));
    }
    refreshValues();
}

void SchematicModel::finishRubyMessageBox(const QString& token, const QString& result)
{
    bool ok = false;
    const auto id = token.toULongLong(&ok);
    if (!ok) return;
    try {
        if (!graph().finishRubyMessageBox(id, result.toStdString())) return;
    } catch (const std::exception& error) {
        setFeedback(tr("Dialog action failed. %1").arg(QString::fromUtf8(error.what())));
    }
    refreshValues();
}

void SchematicModel::finishColorDialog(const QString& token, bool accepted, const QColor& color)
{
    bool ok = false;
    const auto id = token.toULongLong(&ok);
    const auto* dialog = graph().pendingAppDialog();
    if (!ok || !dialog || dialog->kind != PrimitiveKind::ColorDialog) return;
    try {
        if (!graph().finishAppDialog(id, accepted, {}, color.isValid() ? std::optional{yellow::Color{color.rgba()}}
                                                                     : std::nullopt)) return;
    } catch (const std::exception& error) {
        setFeedback(tr("Dialog action failed. %1").arg(QString::fromUtf8(error.what())));
    }
    refreshValues();
}

void SchematicModel::finishDropList(const QString& token, int selection)
{
    bool ok = false;
    const auto id = token.toULongLong(&ok);
    if (!ok || !graph().pendingDropList() || graph().pendingDropList()->token != id) {
        return;
    }
    try {
        graph().finishDropList(id, selection == -1 ? std::nullopt : std::optional{selection});
    } catch (const std::exception& error) {
        setFeedback(QString::fromUtf8(error.what()));
    }
    refreshValues();
}

QString SchematicModel::beginEditControl(const QVariantList& ownerPath, int id, const QString& rubySession)
{
    auto* owner = session_.findGraph(nodePath(ownerPath));
    if (!owner) {
        return {};
    }
    try {
        if (const auto token = owner->beginEditControl(id, rubySession.toULongLong())) {
            refreshValues();
            return QString::number(token);
        }
    } catch (const std::exception& error) {
        setFeedback(QString::fromUtf8(error.what()));
        refreshValues();
    }
    return {};
}

void SchematicModel::changeEditControl(const QVariantList& ownerPath, int id, const QString& token, const QString& text)
{
    if (auto* owner = session_.findGraph(nodePath(ownerPath))) {
        try {
            if (!owner->changeEditControl(id, token.toULongLong(), text.toUtf8().toStdString())) {
                return;
            }
        } catch (const std::exception& error) {
            setFeedback(QString::fromUtf8(error.what()));
        }
        refreshValues();
    }
}

void SchematicModel::finishEditControl(const QVariantList& ownerPath, int id, const QString& token)
{
    if (auto* owner = session_.findGraph(nodePath(ownerPath))) {
        try {
            if (!owner->finishEditControl(id, token.toULongLong())) {
                return;
            }
        } catch (const std::exception& error) {
            setFeedback(QString::fromUtf8(error.what()));
        }
        refreshValues();
    }
}

bool SchematicModel::trackFileDrag(const QVariantList& ownerPath, int id, const QList<QUrl>& urls,
                                  const QPointF& position, bool dropped)
{
    auto* owner = session_.findGraph(nodePath(ownerPath));
    if (!owner || urls.isEmpty()) {
        return false;
    }
    std::vector<DropFile> files;
    files.reserve(urls.size());
    for (const auto& url : urls) {
        if (!url.isLocalFile()) {
            return false;
        }
        const auto path = url.toLocalFile();
        const QFileInfo info(path);
        const bool directory = info.isDir();
        if (!directory && !info.isFile()) {
            return false;
        }
        files.push_back({QDir::toNativeSeparators(path).toUtf8().toStdString(),
                         info.fileName().toUtf8().toStdString(), directory});
    }

    try {
        const green::Point point{static_cast<float>(position.x()), static_cast<float>(position.y())};
        const auto phase = dropped ? FileDropPhase::Drop : FileDropPhase::Preview;
        if (owner->receiveFileDrop(id, files, point, phase)) {
            refreshValues();
            return true;
        }
    } catch (const std::exception& error) {
        setFeedback(QString::fromUtf8(error.what()));
        refreshValues();
    }
    return false;
}

void SchematicModel::moveFileDrag(const QVariantList& ownerPath, int id, const QPointF& position)
{
    if (auto* owner = session_.findGraph(nodePath(ownerPath))) {
        try {
            if (!owner->moveFileDrag(id, {static_cast<float>(position.x()), static_cast<float>(position.y())})) {
                return;
            }
        } catch (const std::exception& error) {
            setFeedback(QString::fromUtf8(error.what()));
        }
        refreshValues();
    }
}

void SchematicModel::leaveFileDrag(const QVariantList& ownerPath, int id)
{
    if (auto* owner = session_.findGraph(nodePath(ownerPath))) {
        try {
            if (!owner->leaveFileDrag(id)) {
                return;
            }
        } catch (const std::exception& error) {
            setFeedback(QString::fromUtf8(error.what()));
        }
        refreshValues();
    }
}

bool SchematicModel::mousePolygonContains(const QVariant& polygon, const QPointF& position) const
{
    const auto* shape = get_if<yellow::MousePolygon>(&polygon);
    return shape && shape->contains({static_cast<float>(position.x()), static_cast<float>(position.y())});
}

bool SchematicModel::rubyMouseContains(const QVariantList& ownerPath, int id, int input, const QPointF& position)
{
    auto* owner = session_.findGraph(nodePath(ownerPath));
    return owner && owner->rubyMouseContains(id, input, {static_cast<float>(position.x()), static_cast<float>(position.y())});
}

int SchematicModel::rubyMouseCursor(const QVariantList& ownerPath, int id, int input, const QPointF& position)
{
    auto* owner = session_.findGraph(nodePath(ownerPath));
    if (!owner) return -1;
    const auto cursor = owner->rubyMouseCursor(id, input, {static_cast<float>(position.x()), static_cast<float>(position.y())});
    return cursor ? static_cast<int>(nativeMouseCursor(*cursor)) : -1;
}

void SchematicModel::cancelRubyMouseDrag(const QVariantList& ownerPath, int id, int input)
{
    if (auto* owner = session_.findGraph(nodePath(ownerPath))) {
        owner->cancelRubyMouseDrag(id, input);
        updateCursor();
    }
}

bool SchematicModel::sendMouseMessage(const QVariantList& ownerPath, int id, PointerEvent event,
                                      const QPointF& position, int buttons, const QPoint& wheelDelta,
                                      QQuickItem* receiver, const QPoint& screenPosition, qreal units, int rubyInput)
{
    const yellow::PanelControlId source{nodePath(ownerPath), id};
    const bool capturedHere = pointerDrag_ && pointerDrag_->receiver == receiver;
    const bool finishing = capturedHere && event == PointerUp && buttons == Qt::LeftButton;
    const bool dragging = capturedHere && event == PointerDrag && (buttons & Qt::LeftButton) &&
        pointerDrag_->source == source;
    if (dragging && std::exchange(pointerDrag_->recentered, false) &&
        screenPosition == pointerDrag_->screenPosition) {
        return true;
    }
    auto* owner = session_.findGraph(source.ownerPath);
    if (!owner) {
        if (finishing) {
            cancelMouseDrag(receiver);
        }
        return false;
    }
    using enum yellow::MouseEventKind;
    using yellow::MouseButton;
    auto kind = None;
    switch (event) {
    case PointerMove: kind = Move; break;
    case PointerDrag: kind = Drag; break;
    case PointerDown: kind = ButtonDown; break;
    case PointerUp: kind = ButtonUp; break;
    case PointerClick: kind = Click; break;
    case PointerDoubleClick: kind = DoubleClick; break;
    case PointerWheel: kind = Wheel; break;
    }
    auto button = MouseButton::None;
    if (buttons & Qt::LeftButton) {
        button = MouseButton::Left;
    } else if (buttons & Qt::RightButton) {
        button = MouseButton::Right;
    } else if (buttons & Qt::MiddleButton) {
        button = MouseButton::Middle;
    }
    const yellow::MouseMessage message{kind, {static_cast<float>(position.x()), static_cast<float>(position.y())},
                                       button, {wheelDelta.x(), wheelDelta.y()}};
    if (rubyInput >= 0) {
        try {
            if (!owner->sendRubyMouseMessage(id, rubyInput, message)) return false;
        } catch (const std::exception& error) {
            setFeedback(QString::fromUtf8(error.what()));
        }
        refreshValues();
        return true;
    }

    MouseDragEffects effects;
    try {
        if (!owner->sendMouseMessage(id, message, &effects)) {
            if (finishing) {
                cancelMouseDrag(receiver);
            }
            return false;
        }
    } catch (const std::exception& error) {
        setFeedback(QString::fromUtf8(error.what()));
        if (finishing) {
            cancelMouseDrag(receiver);
        } else if (kind == ButtonDown && button == MouseButton::Left) {
            cancelSchematicDrag(source);
        }
        if (effects.fileDrag) {
            graph().finishDragFiles(effects.fileDrag->token, false);
        }
        refreshValues();
        return true;
    }

    if (effects.fileDrag) {
        // QDrag can run a nested event loop. Start it after Qt Quick finishes handling the press.
        QMetaObject::invokeMethod(this, [this, request = std::move(*effects.fileDrag),
                                       item = QPointer<QQuickItem>(receiver)] {
            startFileDrag(request, item);
        }, Qt::QueuedConnection);
    }
    if (effects.started && receiver) {
        if (pointerDrag_) {
            cancelMouseDrag(pointerDrag_->receiver);
        }
        pointerDrag_ = PointerCapture{
            receiver, source, screenPosition, effects.cursor,
            effects.accumulating && effects.cursor == yellow::MouseCursor::Hidden, false,
            connect(receiver, &QObject::destroyed, this, &SchematicModel::cancelMouseDrag),
            connect(receiver, &QQuickItem::windowChanged, this, [this, receiver](QQuickWindow* window) {
                if (!window) {
                    cancelMouseDrag(receiver);
                }
            }),
        };
    } else if (finishing) {
        if (pointerDrag_->source == source) {
            finishPointerDrag(effects.resetPointer);
        } else {
            cancelMouseDrag(receiver);
        }
    } else if (dragging && pointerDrag_->recenterOnEdge) {
        recenterPointerDrag(*owner, position, screenPosition, units);
    }
    refreshValues();
    return true;
}

void SchematicModel::startFileDrag(const DragFilesRequest& request, const QPointer<QQuickItem>& receiver)
{
    QList<QUrl> urls;
    for (const auto& path : request.paths) {
        if (path.empty() || path.find('\0') != std::string::npos) {
            continue;
        }
        const QFileInfo file(qtext(path));
        if (file.isFile()) {
            urls.append(QUrl::fromLocalFile(file.absoluteFilePath()));
        }
    }

    const auto sourceWindow = [&]() -> QQuickWindow* {
        return receiver && receiver->isVisible() && receiver->isEnabled() &&
            receiver->property("pressed").toBool()
            ? receiver->window() : nullptr;
    };
    QPointer<SchematicModel> model(this);
    bool executing = false;
    const auto finish = [&](bool dropped) {
        if (!model) {
            return;
        }
        try {
            if (executing) {
                model->nativeFileDrag_ = false;
            }
            model->graph().finishDragFiles(request.token, dropped);
        } catch (const std::exception& error) {
            model->setFeedback(QString::fromUtf8(error.what()));
        }
        model->refreshValues();
    };

    try {
        if (urls.isEmpty() || !sourceWindow() || !graph().beginDragFiles(request.token)) {
            finish(false);
            return;
        }
        cancelMouseDrag(receiver);
        refreshValues();
        auto* window = model ? sourceWindow() : nullptr;
        if (!window) {
            finish(false);
            return;
        }

        auto action = Qt::CopyAction;
        if (request.mode == FileDragMode::Move) {
            action = Qt::MoveAction;
        } else if (request.mode == FileDragMode::Link) {
            action = Qt::LinkAction;
        }

        auto* drag = new QDrag(window);
        auto* mimeData = new QMimeData;
        mimeData->setUrls(urls);
        drag->setMimeData(mimeData);
        executing = true;
        nativeFileDrag_ = true;
        updateCursor();
        // A Windows destination that performs its own move can return TargetMoveAction.
        finish(drag->exec(action, action) != Qt::IgnoreAction);
    } catch (const std::exception& error) {
        if (model) {
            model->setFeedback(QString::fromUtf8(error.what()));
        }
        finish(false);
    }
}

void SchematicModel::recenterPointerDrag(Schematic& owner, const QPointF& position,
                                       const QPoint& screenPosition, qreal units)
{
    const auto* receiver = pointerDrag_->receiver;
    const auto* window = receiver->window();
    if (!window || !std::isfinite(units) || units <= 0) {
        return;
    }

    const QRect windowBounds(window->mapToGlobal(QPoint{}), window->size());
    const auto insideEdge = windowBounds.marginsRemoved(QMargins(5, 5, 5, 5));
    const auto target = pointerDrag_->screenPosition;
    if (insideEdge.contains(screenPosition) || !insideEdge.contains(target)) {
        return;
    }

    const QPointF delta = receiver->mapFromGlobal(QPointF(target)) - receiver->mapFromGlobal(QPointF(screenPosition));
    const auto anchor = position + delta / units;
    const auto source = pointerDrag_->source;
    auto* screen = QGuiApplication::screenAt(target);
    pointerDrag_->recentered = true;
    QCursor::setPos(screen, target);
    if (QCursor::pos(screen) == target) {
        owner.rebaseMouseDrag(source, {static_cast<float>(anchor.x()), static_cast<float>(anchor.y())});
    } else if (pointerDrag_ && pointerDrag_->source == source) {
        pointerDrag_->recentered = false;
        pointerDrag_->recenterOnEdge = false;
    }
}

void SchematicModel::finishPointerDrag(bool reset)
{
    if (!pointerDrag_) {
        return;
    }
    auto drag = std::exchange(pointerDrag_, std::nullopt);
    disconnect(drag->receiverDestroyed);
    disconnect(drag->windowChanged);
    if (reset) {
        // Let Qt finish delivering the release before the move can generate another pointer event.
        QMetaObject::invokeMethod(this, [this, position = drag->screenPosition] {
            if (!pointerDrag_) {
                QCursor::setPos(QGuiApplication::screenAt(position), position);
            }
        }, Qt::QueuedConnection);
    }
}

void SchematicModel::setCursorOverride(std::optional<Qt::CursorShape> shape)
{
    // Show Cursor and Mouse Drag share one entry on Qt's application cursor stack.
    const auto previous = std::exchange(cursorOverride_, shape);
    if (previous == shape) {
        return;
    }
    if (!shape) {
        QGuiApplication::restoreOverrideCursor();
    } else if (previous) {
        QGuiApplication::changeOverrideCursor(QCursor(*shape));
    } else {
        QGuiApplication::setOverrideCursor(QCursor(*shape));
    }
}

void SchematicModel::updateCursor()
{
    const bool active = QGuiApplication::applicationState() == Qt::ApplicationActive;
    const auto state = graph().cursorVisibility();
    const auto hiddenRevision = active && state.hidden ? std::optional{state.revision} : std::nullopt;
    const auto previousRevision = std::exchange(hiddenCursorRevision_, hiddenRevision);
    if (hiddenRevision != previousRevision) {
        auto heldPosition = std::exchange(heldCursorPosition_, std::nullopt);
        if (hiddenRevision && state.hold) {
            heldCursorPosition_ = QCursor::pos();
        } else if (active && !state.hidden && heldPosition && previousRevision != state.revision) {
            // Apply the warp after the current pointer event and coalesce superseded requests.
            QMetaObject::invokeMethod(this, [this, position = *heldPosition, revision = state.revision] {
                const auto current = graph().cursorVisibility();
                if (!current.hidden && current.revision == revision && !nativeFileDrag_ &&
                    QGuiApplication::applicationState() == Qt::ApplicationActive) {
                    if (auto* screen = QGuiApplication::screenAt(position)) {
                        QCursor::setPos(screen, position);
                    }
                }
            }, Qt::QueuedConnection);
        }
    }

    std::optional<Qt::CursorShape> shape;
    if (active && !nativeFileDrag_) {
        if (state.hidden) {
            shape = Qt::BlankCursor;
        } else if (pointerDrag_ && pointerDrag_->cursor) {
            shape = nativeMouseCursor(*pointerDrag_->cursor);
        }
    }
    setCursorOverride(shape);
}

void SchematicModel::cancelSchematicDrag(const yellow::PanelControlId& source)
{
    try {
        if (!graph().cancelMouseDrag(source)) {
            return;
        }
    } catch (const std::exception& error) {
        setFeedback(QString::fromUtf8(error.what()));
    }
    // A grab can end while a panel delegate is being rebuilt.
    QMetaObject::invokeMethod(this, [this] { refreshValues(); }, Qt::QueuedConnection);
}

void SchematicModel::cancelMouseDrag(QObject* receiver)
{
    if (!pointerDrag_ || pointerDrag_->receiver != receiver) {
        return;
    }
    const auto source = pointerDrag_->source;
    finishPointerDrag(false);
    cancelSchematicDrag(source);
    updateCursor();
}

void SchematicModel::movePanelPointer(const QVariantList& ownerPath, int id, const QPointF& position)
{
    if (auto* owner = session_.findGraph(nodePath(ownerPath))) {
        try {
            if (!owner->movePanelPointer(id, {static_cast<float>(position.x()), static_cast<float>(position.y())})) {
                return;
            }
        } catch (const std::exception& error) {
            setFeedback(QString::fromUtf8(error.what()));
        }
        refreshValues();
    }
}

void SchematicModel::loadBitmap(int id, const QUrl& file)
{
    if (rowFor(id) < 0) {
        return;
    }
    try {
        if (!file.isLocalFile()) {
            throw std::invalid_argument("Choose a local bitmap file");
        }
        session_.loadBitmap(id, file.toLocalFile().toUtf8().toStdString());
        setFeedback({});
    } catch (const std::exception& error) {
        setFeedback(QString::fromUtf8(error.what()));
    }
    refreshValues();
}

QVariantMap SchematicModel::inspectPort(int id, int index, bool output) const
{
    const int row = rowFor(id);
    if (row < 0) {
        return {};
    }
    const auto& entry = graph().nodes()[row];
    const auto ports = output ? entry.outputPorts() : entry.inputPorts();
    if (index < 0 || index >= static_cast<int>(ports.size())) {
        return {};
    }
    auto port = ports[index];
    port.type = output ? graph().outputType(id, index) : graph().inputType(id, index);
    const auto type = portDescription(port, 0).value("type").toString();
    const auto name = output && entry.kind == PrimitiveKind::ArraySplitter ? QString::number(index) : qtext(port.name);
    const auto title = name + (output ? QStringLiteral(" output") : QStringLiteral(" input"));
    if (port.type && green::isDspType(*port.type)) {
        return dspReadout(title, type, *alphaConnector(port.type), [&] {
            return graph().dspTrace(id, index, output);
        }, [&] {
            return graph().liveDspTrace(id, index, output);
        });
    }
    auto result = readout(title, type, [&] {
        return output ? graph().inspectOutput(id, index) : graph().inspectInput(id, index);
    });
    if (!output && index == 1 && (entry.kind == PrimitiveKind::MonoToPoly || entry.kind == PrimitiveKind::PolyToMono) &&
        result.contains("value") && result.value("value").toString().isEmpty()) {
        result.insert("value", QStringLiteral("All voices"));
    }
    return result;
}

QVariantMap SchematicModel::inspectWire(int source, int output, int target, int input) const
{
    const int sourceRow = rowFor(source);
    const int targetRow = rowFor(target);
    if (sourceRow < 0 || targetRow < 0) {
        return {};
    }
    const auto& nodes = graph().nodes();
    const auto inputs = nodes[targetRow].inputPorts();
    const auto outputs = nodes[sourceRow].outputPorts();
    if (input < 0 || input >= static_cast<int>(inputs.size()) ||
        output < 0 || output >= static_cast<int>(outputs.size())) {
        return {};
    }
    auto sourcePort = outputs[output];
    auto targetPort = inputs[input];
    sourcePort.type = graph().outputType(source, output);
    targetPort.type = graph().inputType(target, input);
    auto type = portDescription(targetPort, 0).value("type").toString();
    if (sourcePort.type == Type::Trigger) {
        type = QStringLiteral("Trigger");
    } else if (!sourcePort.type || !targetPort.type) {
        type = QStringLiteral("Template");
    } else if (targetPort.type != Type::Trigger && sourcePort.type != targetPort.type) {
        type = portDescription(sourcePort, 0).value("type").toString() + QStringLiteral(" → ") + type;
    }
    const auto dspType = sourcePort.type && green::isDspType(*sourcePort.type) ? sourcePort.type : targetPort.type;
    if (dspType && green::isDspType(*dspType)) {
        return dspReadout(QStringLiteral("Link"), type, *alphaConnector(dspType), [&] {
            return graph().dspTrace(source, output, true);
        }, [&] {
            return graph().liveDspTrace(source, output, true);
        });
    }
    return readout(QStringLiteral("Link"), type, [&] { return graph().inspectLink({source, target, input, output}); });
}

void SchematicModel::setPanelHidden(int id, bool hidden)
{
    const auto row = rowFor(id);
    if (row < 0 || !graph().module(id)) {
        return;
    }
    session_.setPanelHidden(id, hidden);
    refreshValues(true);
    emit geometryChanged();
}

void SchematicModel::enableFrontPanel(int id)
{
    const bool ownsEdit = session_.beginEdit(SchematicEdit::EnablePanel);
    try {
        session_.enableModulePanel(id, ModulePanelKind::Front);
        session_.setPanelHidden(id, false);
        graph().setPropertiesVisibility(id, PropertiesVisibility::Closed);
        setFeedback({});
    } catch (const std::exception& error) {
        setFeedback(QString::fromUtf8(error.what()));
    }
    refreshValues(true);
    emit geometryChanged();
    if (ownsEdit) finishEdit();
}

void SchematicModel::setPanelShownInParent(int id, bool shown)
{
    try {
        session_.setPanelShownInParent(id, shown);
        setFeedback({});
    } catch (const std::exception& error) {
        setFeedback(QString::fromUtf8(error.what()));
    }
    refreshValues(true);
}

void SchematicModel::setPanelUnlocked(int id, bool unlocked)
{
    if (graph().module(id)) {
        graph().setPanelUnlocked(id, unlocked);
        refreshValues(true);
    }
}

void SchematicModel::focusModule(int id)
{
    const auto* child = graph().module(id);
    if (const auto panel = child ? child->frontPanel() : std::nullopt) {
        graph().setPresentationPanel(panel->view.schematicPath);
        refreshValues();
    }
}

void SchematicModel::closeFocusMode()
{
    if (graph().presentation().panelPath) {
        graph().setPresentationPanel(std::nullopt);
        refreshValues();
    }
}

void SchematicModel::reportFocusWindow(const QVariantList& path, bool fullScreen, double scale)
{
    if (graph().reportPresentation(nodePath(path), fullScreen, static_cast<float>(scale))) {
        refreshValues();
    }
}

void SchematicModel::toggleProperties(int id)
{
    const auto row = rowFor(id);
    if (row < 0) {
        return;
    }
    if (const auto* child = graph().module(id); child && !child->modulePanel(ModulePanelKind::Properties)) {
        const bool ownsEdit = session_.beginEdit(SchematicEdit::EnablePanel);
        try {
            session_.enableModulePanel(id, ModulePanelKind::Properties);
        } catch (const std::exception& error) {
            if (ownsEdit) finishEdit();
            setFeedback(QString::fromUtf8(error.what()));
            return;
        }
        if (ownsEdit) finishEdit();
    }
    const auto visibility = graph().nodes()[row].propertiesVisibility;
    const bool pin = QGuiApplication::keyboardModifiers().testFlag(Qt::ControlModifier);
    setPropertiesVisible(id, pin || visibility == PropertiesVisibility::Closed,
                         pin && visibility != PropertiesVisibility::Pinned);
}

void SchematicModel::setPropertiesVisible(int id, bool visible, bool pinned)
{
    try {
        graph().setPropertiesVisibility(id, !visible ? PropertiesVisibility::Closed :
            pinned ? PropertiesVisibility::Pinned : PropertiesVisibility::Open);
        setFeedback({});
    } catch (const std::exception& error) {
        setFeedback(QString::fromUtf8(error.what()));
        return;
    }
    refreshValues(true);
    emit geometryChanged();
}

void SchematicModel::closeUnpinnedProperties(int exceptId)
{
    bool changed = false;
    for (const auto& entry : graph().nodes()) {
        if (entry.id != exceptId && entry.propertiesVisibility == PropertiesVisibility::Open) {
            graph().setPropertiesVisibility(entry.id, PropertiesVisibility::Closed);
            changed = true;
        }
    }
    if (changed) {
        refreshValues(true);
        emit geometryChanged();
    }
}

void SchematicModel::movePanelItem(int moduleId, const QVariantList& itemPath, double x, double y,
                                   bool properties, bool itemProperties)
{
    try {
        session_.movePanelItem(moduleId, nodePath(itemPath), {static_cast<float>(x), static_cast<float>(y)},
                               modulePanelKind(properties), modulePanelKind(itemProperties));
        setFeedback({});
    } catch (const std::exception& error) {
        setFeedback(QString::fromUtf8(error.what()));
    }
    refreshValues(true);
}

void SchematicModel::resizePanel(int moduleId, double width, double height, bool properties)
{
    try {
        session_.resizePanel(moduleId, static_cast<float>(width), static_cast<float>(height), modulePanelKind(properties));
        setFeedback({});
    } catch (const std::exception& error) {
        setFeedback(QString::fromUtf8(error.what()));
    }
    refreshValues(true);
}

void SchematicModel::setPanelItemAnchors(int moduleId, const QVariantList& itemPath, bool properties,
                                         bool itemProperties, int anchors)
{
    if (anchors < 0 || anchors > 15) {
        return;
    }
    try {
        session_.setPanelItemAnchors(moduleId, nodePath(itemPath), modulePanelKind(properties),
                                     modulePanelKind(itemProperties), static_cast<std::uint8_t>(anchors));
        setFeedback({});
    } catch (const std::exception& error) {
        setFeedback(QString::fromUtf8(error.what()));
    }
    refreshValues(true);
}

void SchematicModel::renderPanelFrames()
{
    renderedPanels_.clear();
    windowFrames_.clear();
    focusedPanel_.clear();
    const auto& root = session_.graphAt(0);
    const auto presentation = root.presentation();
    const auto* focusOwner = presentation.panelPath && !presentation.panelPath->empty()
        ? session_.findGraph(*presentation.panelPath)
        : nullptr;
    const auto focus = focusOwner ? focusOwner->frontPanel() : std::nullopt;
    const bool renderModules = root.hasDrawingBehavior(PrimitiveBehavior::ViewLoop) ||
                               root.hasDrawingBehavior(PrimitiveBehavior::ViewCache) ||
                               root.hasDrawingBehavior(PrimitiveBehavior::Redraw) ||
                               root.hasDrawingBehavior(PrimitiveBehavior::RubyCode);
    const auto nativeWindows = root.requestedWindows();
    if (!renderModules && nativeWindows.empty() && !focus) {
        cachedDrawingDescriptions_.clear();
        return;
    }
    const auto redraw = root.redrawRevision();
    PanelRender render{graph(), cachedDrawingDescriptions_};
    if (renderModules && !focus) {
        // Execute draw-trigger cascades once per update. Painting and model reads reuse the result.
        for (const auto& entry : graph().nodes()) {
            if (entry.kind != PrimitiveKind::Module) {
                continue;
            }
            const auto& child = *graph().module(entry.id);
            std::optional<ModulePanel> panel;
            try {
                panel = visibleModulePanel(child, entry);
                if (panel) {
                    renderedPanels_.emplace(entry.id, frontPanelDescription(child, *panel, &render, zoom()));
                }
            } catch (const std::exception& error) {
                setFeedback(QString::fromUtf8(error.what()));
                QVariantMap fallback;
                if (panel) {
                    fallback.insert("bounds", panelRect(panel->view.bounds));
                }
                renderedPanels_.emplace(entry.id, std::move(fallback));
            }
        }
    }

    if (focus) {
        const auto parentPath = std::span{*presentation.panelPath}.first(presentation.panelPath->size() - 1);
        const auto& parentNodes = session_.findGraph(parentPath)->nodes();
        const auto owner = std::ranges::find(parentNodes, presentation.panelPath->back(), &SchematicNode::id);
        try {
            const auto description = frontPanelDescription(*focusOwner, *focus, &render, presentation.scale);
            focusedPanel_ = {{"key", description.value("key")}, {"title", qtext(owner->moduleName)},
                             {"focused", true}, {"path", panelPath(*presentation.panelPath)},
                             {"panel", description}, {"pixelsPerUnit", panelScale},
                             {"maximumExtent", maximumWindowExtent}, {"zoomScale", presentation.requestedScale},
                             {"fullScreenToggles", QVariant::fromValue<qulonglong>(presentation.fullScreenToggles)}};
            if (const auto color = focusOwner->presentationBackground()) {
                focusedPanel_.insert("backgroundColor", QColor::fromRgba(color->argb));
            }
        } catch (const std::exception& error) {
            setFeedback(QString::fromUtf8(error.what()));
        }
    }

    for (const auto& window : nativeWindows) {
        const auto& view = window.panel.view;
        const auto* owner = session_.findGraph(view.schematicPath);
        if (!owner) {
            continue;
        }
        const auto path = panelPath(view.schematicPath);
        QVariantMap description;
        try {
            description = frontPanelDescription(*owner, window.panel, &render);
        } catch (const std::exception& error) {
            setFeedback(QString::fromUtf8(error.what()));
        }
        windowFrames_.push_back(QVariantMap{
            {"key", viewKey(view)}, {"path", path}, {"node", view.panel},
            {"width", window.width}, {"height", window.height},
            {"type", static_cast<int>(window.type)}, {"title", qtext(window.title)}, {"panel", description},
            {"pixelsPerUnit", windowPixelsPerUnit}, {"maximumExtent", maximumWindowExtent},
        });
    }
    cachedDrawingDescriptions_ = std::move(render.current);
    if (root.redrawRevision() != redraw && !redrawQueued_) {
        redrawQueued_ = true;
        QMetaObject::invokeMethod(this, [this] {
            redrawQueued_ = false;
            refreshValues();
        }, Qt::QueuedConnection);
    }
}

bool SchematicModel::refreshMidiHardware(bool scan)
{
    try {
        const bool changed = graph().refreshMidiDevices(scan);
        updateMidiTimers();
        return changed;
    } catch (const std::exception& error) {
        setFeedback(tr("MIDI device update failed. %1").arg(QString::fromUtf8(error.what())));
        updateMidiTimers();
        return false;
    }
}

void SchematicModel::updateMidiTimers()
{
    const auto update = [](QTimer& timer, bool enabled) {
        if (enabled && !timer.isActive()) timer.start();
        if (!enabled && timer.isActive()) timer.stop();
    };
    const bool open = windowClose_ != WindowClose::Ready;
    update(midiScanTimer_, open && graph().watchesMidiDevices());
    update(midiPollTimer_, open && graph().listensToMidi());
}

QVariantList SchematicModel::midiDeviceChoices(int id)
{
    if (refreshMidiHardware(true)) refreshValues();
    QVariantList devices;
    for (const auto& device : graph().midiDeviceChoices(id)) {
        auto label = qtext(device.name);
        if (!device.available && !device.selected) label += tr(" (in use)");
        else if (device.selected && !device.connected && device.available) label += tr(" (unavailable)");
        devices.push_back(QVariantMap{{"id", qtext(device.id)}, {"label", label},
                                      {"selected", device.selected}, {"available", device.available}});
    }
    return devices;
}

void SchematicModel::setMidiDeviceSelected(int id, const QString& port, bool selected)
{
    try {
        graph().setMidiDeviceSelected(id, port.toUtf8().toStdString(), selected);
        setFeedback({});
        refreshMidiHardware(false);
        refreshValues();
    } catch (const std::exception& error) {
        setFeedback(QString::fromUtf8(error.what()));
    }
}

int SchematicModel::sequencerMode() const
{
    return static_cast<int>(session_.graphAt(0).sequencerMode());
}

bool SchematicModel::hasSequencerTracks() const
{
    return session_.graphAt(0).hasSequencerTracks();
}

void SchematicModel::setSequencerMode(int mode)
{
    if (mode < 0 || mode > 2) return;
    try {
        graph().setSequencerMode(static_cast<stream::SequenceMode>(mode), std::chrono::steady_clock::now());
        refreshValues();
    } catch (const std::exception& error) {
        setFeedback(QString::fromUtf8(error.what()));
    }
}

void SchematicModel::refreshValues(bool portsChanged)
{
    if (portsChanged) refreshMidiHardware(false);
    else updateMidiTimers();
    if (windowClose_ != WindowClose::Ready && (graph().hasOpenSerialPorts() || graph().hasOpenNetworkConnections() || session_.graphAt(0).hasOpenMedia() || sequencerMode() == 2)) {
        if (!serialPollTimer_.isActive()) serialPollTimer_.start();
    } else {
        serialPollTimer_.stop();
    }
    refreshAudioControls();
    renderPanelFrames();
    updateCursor();
    processTicks(TickWake::Configure);
    if (const auto revision = graph().panelGeometryRevision(); revision != panelGeometryRevision_) {
        panelGeometryRevision_ = revision;
        emit geometryChanged();
    }
    if (const auto revision = graph().busLayoutRevision(); revision != busLayoutRevision_) {
        busLayoutRevision_ = revision;
        if (!portsChanged) {
            emit geometryChanged();
        }
        portsChanged = true;
    }
    if (rowCount() > 0) {
        QList<int> roles{ValueRole, EditableRole};
        if (portsChanged || std::ranges::any_of(graph().nodes(), [](const SchematicNode& entry) {
                const auto behavior = primitive(entry.kind).behavior;
                return behavior == PrimitiveBehavior::Module || behavior == PrimitiveBehavior::SendWireless ||
                       behavior == PrimitiveBehavior::ReceiveWireless || behavior == PrimitiveBehavior::RubyCode ||
                       entry.kind == PrimitiveKind::ArraySplitter;
            })) {
            roles.push_back(SpecRole);
        }
        emit dataChanged(index(0), index(rowCount() - 1), roles);
    }
    emit valuesChanged();
    emit painterChanged();
    dispatchEditControlStart();
}

void SchematicModel::dispatchEditControlStart()
{
    if (const auto request = graph().takeEditControlStart()) {
        emit editControlStartRequested(nodeKey(request->ownerPath, request->node));
    }
}

void SchematicModel::setFeedback(QString text)
{
    if (text != feedback_) {
        feedback_ = std::move(text);
        emit feedbackChanged();
    }
}

int SchematicModel::rowFor(int id) const
{
    const auto& nodes = graph().nodes();
    const auto found = std::ranges::find(nodes, id, &SchematicNode::id);
    return found == nodes.end() ? -1 : static_cast<int>(found - nodes.begin());
}
