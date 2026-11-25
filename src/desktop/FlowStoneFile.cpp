#include "FlowStoneFile.hpp"
#include "RubyCode.hpp"

#include <QtEndian>
#include <QStringDecoder>
#include <QBuffer>
#include <QImageReader>
#include <zlib.h>
#include <algorithm>
#include <array>
#include <bit>
#include <set>
#include <map>
#include <functional>
#include <cmath>
#include <stdexcept>

namespace openstone::desktop {
namespace {
using green::Type;
using enum PrimitiveKind;
constexpr qsizetype limit = 128 * 1024 * 1024;

class Reader {
public:
    explicit Reader(QByteArray bytes) : bytes_(std::move(bytes)) {
        if (bytes_.size() > limit) fail("File exceeds the 128 MiB import limit");
    }
    qsizetype pos = 0;
    QString path = "Root";
    [[noreturn]] void fail(const QString& reason) const {
        throw std::invalid_argument(QString("FlowStone import: %1 at byte 0x%2 (%3)")
            .arg(reason).arg(pos, 0, 16).arg(path).toStdString());
    }
    QByteArray take(qsizetype n) {
        if (n < 0 || n > bytes_.size() - pos) fail("Truncated record");
        auto result = bytes_.mid(pos, n); pos += n; return result;
    }
    void skip(qsizetype n) { if (n < 0 || n > bytes_.size() - pos) fail("Truncated record"); pos += n; }
    quint8 u8() { return static_cast<quint8>(take(1)[0]); }
    quint16 u16() { const auto b = take(2); return qFromLittleEndian<quint16>(b.constData()); }
    quint32 u32() { const auto b = take(4); return qFromLittleEndian<quint32>(b.constData()); }
    quint32 count(quint32 maximum = 100000) { auto n = u32(); if (n > maximum) fail("Invalid record count"); return n; }
    bool boolean() { auto n = u8(); if (n > 1) fail("Invalid Boolean"); return n; }
    void blob() { skip(count(limit)); }
    std::string string() {
        quint32 n = u8(); bool wide = false;
        if (n == 255) {
            n = u16();
            if (n == 0xfffe) {
                wide = true; n = u8(); if (n == 255) n = u16();
            }
            if (n == 0xffff) n = u32();
        }
        if (n > static_cast<quint32>(limit / 2)) fail("String exceeds the import limit");
        auto b = take(n * (wide ? 2 : 1));
        // CArchive narrow strings in the bundled files use Windows-1252.
        QString text;
        if (wide) {
            auto decoder = QStringDecoder(QStringDecoder::Utf16LE);
            text = decoder.decode(b);
            if (decoder.hasError()) fail("Invalid UTF-16 string");
        } else {
            constexpr char16_t controls[] = {
                0x20ac,0x0081,0x201a,0x0192,0x201e,0x2026,0x2020,0x2021,
                0x02c6,0x2030,0x0160,0x2039,0x0152,0x008d,0x017d,0x008f,
                0x0090,0x2018,0x2019,0x201c,0x201d,0x2022,0x2013,0x2014,
                0x02dc,0x2122,0x0161,0x203a,0x0153,0x009d,0x017e,0x0178};
            text.reserve(b.size());
            for (unsigned char c : b) text.append(QChar(c >= 0x80 && c < 0xa0 ? controls[c - 0x80] : char16_t(c)));
        }
        return text.toUtf8().toStdString();
    }
    bool done() const { return pos == bytes_.size(); }
private:
    QByteArray bytes_;
};

QByteArray schematic(const QByteArray& archive) {
    Reader r(archive);
    // Read the central directory, then exactly the named member. Never extract archive paths.
    qsizetype end = -1;
    for (qsizetype i = archive.size() - 22; i >= std::max<qsizetype>(0, archive.size() - 65557); --i) {
        if (archive.mid(i, 4) == QByteArray("PK\5\6", 4)) {
            r.pos = i + 20;
            if (i + 22 + r.u16() == archive.size()) { end = i; break; }
        }
    }
    if (end < 0) r.fail("Missing ZIP directory");
    r.pos = end + 4;
    if (r.u16() || r.u16()) r.fail("Multi-disk ZIP is unsupported");
    auto diskCount = r.u16(); auto count = r.u16();
    auto size = r.u32(); auto start = r.u32();
    if (count != diskCount || count == 65535 || quint64(start) + size != quint64(end)) r.fail("Invalid ZIP directory");
    r.pos = start;
    struct Member { quint16 method, flags; quint32 crc, packed, unpacked, offset; };
    std::optional<Member> member;
    for (int i = 0; i < count; ++i) {
        if (r.u32() != 0x02014b50) r.fail("Invalid ZIP member");
        r.skip(4); auto flags = r.u16(); auto method = r.u16(); r.skip(4);
        auto crc = r.u32(); auto packed = r.u32(); auto unpacked = r.u32();
        auto nameSize = r.u16(); auto extra = r.u16(); auto comment = r.u16();
        auto disk = r.u16(); r.skip(6); auto offset = r.u32();
        auto name = r.take(nameSize); r.skip(extra + comment);
        if (name == "schematic.osm") {
            if (member) r.fail("Duplicate schematic.osm member");
            if (disk || (flags & ~quint16(0x080e)) || (flags & 1) || (method != 0 && method != 8) ||
                packed > limit || unpacked > limit || !unpacked) r.fail("Unsupported ZIP compression, encryption or size");
            member = Member{method, flags, crc, packed, unpacked, offset};
        }
    }
    if (r.pos != qsizetype(start) + size || !member) r.fail("Missing schematic.osm member");
    auto m = *member; r.pos = m.offset;
    if (r.u32() != 0x04034b50) r.fail("Invalid ZIP local header");
    r.skip(2); auto flags = r.u16(); auto method = r.u16(); r.skip(4);
    auto crc = r.u32(); auto packed = r.u32(); auto unpacked = r.u32();
    auto nameSize = r.u16(); auto extra = r.u16();
    if (flags != m.flags || method != m.method || r.take(nameSize) != "schematic.osm") r.fail("ZIP headers disagree");
    if (!(flags & 8) && (crc != m.crc || packed != m.packed || unpacked != m.unpacked)) r.fail("ZIP sizes disagree");
    r.skip(extra);
    if (quint64(r.pos) + m.packed > start) r.fail("ZIP member overlaps the directory");
    const auto compressed = r.take(m.packed);
    QByteArray result;
    if (method == 0) {
        if (m.packed != m.unpacked) r.fail("Invalid stored ZIP size");
        result = compressed;
    } else {
        result.resize(m.unpacked);
        z_stream stream{};
        stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(compressed.constData()));
        stream.avail_in = m.packed;
        stream.next_out = reinterpret_cast<Bytef*>(result.data()); stream.avail_out = m.unpacked;
        if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) r.fail("Cannot initialize ZIP decompression");
        const auto status = inflate(&stream, Z_FINISH);
        inflateEnd(&stream);
        if (status != Z_STREAM_END || stream.total_in != m.packed || stream.total_out != m.unpacked) r.fail("Invalid deflate stream");
    }
    if (crc32(0, reinterpret_cast<const Bytef*>(result.constData()), result.size()) != m.crc) r.fail("ZIP checksum mismatch");
    return result;
}

class Parser : public Reader {
public:
    using Reader::Reader;
    FlowStoneImport result;
    int version = 0;
    std::vector<QByteArray> imageResources;
    std::map<quint32, yellow::Bitmap> decodedImages;
    qsizetype decodedImageBytes = 0;
    void warning(const QString& message) { if (!result.warnings.contains(message)) result.warnings.append(message); }
    std::optional<Type> type(quint32 n) {
        switch (n) {
        case 0: case 19: return {};
        case 1: return Type::String; case 2: return Type::Float; case 3: return Type::Int;
        case 4: return Type::MIDI; case 5: case 26: return Type::Poly; case 6: return Type::Mono;
        case 7: return Type::View; case 8: return Type::Area; case 9: return Type::Trigger;
        case 13: return Type::Color; case 14: return Type::PaintTool; case 15: return Type::Boolean;
        case 17: return Type::Font; case 18: return Type::StringFormat;
        case 16: return Type::Bitmap; case 23: case 10: return Type::FloatArray;
        case 25: return Type::PolyBoolean; case 28: return Type::Mono4; case 31: return Type::MonoBoolean;
        case 27: return Type::Preset;
        case 37: return Type::RubyValue; case 39: return Type::Stream; case 44: return Type::StreamBoolean;
        case 45: return Type::StreamDouble; case 46: return Type::PolyDouble;
        case 47: return Type::MonoDouble; case 48: return Type::Mono4Double;
        default: fail(QString("Unsupported connector type %1").arg(n));
        }
    }
    float real(const char* field = "panel coordinate") {
        const auto value = std::bit_cast<float>(u32());
        if (!std::isfinite(value) || std::abs(value) > 1000000) fail(QString("Invalid %1").arg(field));
        return value;
    }
    green::Point point() { const auto x = real(); const auto y = real(); return {x, y}; }
    yellow::Area area() { const auto p = point(); const auto size = point(); return {p.x, p.y, size.x, size.y}; }
    yellow::BrushWrap brushWrap() {
        const auto wrap = u32();
        if (wrap > 4) fail("Invalid brush wrap mode");
        return yellow::BrushWrap(wrap);
    }
    yellow::Bitmap rasterBitmap(QByteArray bytes, const QString& description) {
        if (bytes.isEmpty()) return {};
        QBuffer buffer(&bytes); buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer);
        const auto size = reader.size();
        if (!size.isValid() || qint64(size.width()) * size.height() > (limit - decodedImageBytes) / 4)
            fail("Invalid " + description + " image size or decoded image limit exceeded");
        auto image = reader.read().convertToFormat(QImage::Format_RGBA8888);
        if (image.isNull() || image.size() != size) fail("Invalid " + description + " image");
        decodedImageBytes += qsizetype(size.width()) * size.height() * 4;
        std::vector<std::uint8_t> pixels(qsizetype(size.width()) * size.height() * 4);
        for (int y = 0; y < size.height(); ++y)
            std::copy_n(image.constScanLine(y), size.width() * 4, pixels.data() + qsizetype(y) * size.width() * 4);
        return {size.width(), size.height(), std::move(pixels)};
    }
    yellow::Bitmap textureBitmap() { return rasterBitmap(take(count(limit)), "texture"); }
    yellow::Bitmap bitmapRecord() {
        if (version >= 160) { boolean(); if (boolean()) connectorLayout(); }
        if (!boolean()) return rasterBitmap(take(count(limit)), "Bitmap");
        const auto index = std::bit_cast<qint32>(u32());
        if (index == -1) return {};
        if (index < 0 || quint32(index) >= imageResources.size()) fail("Invalid Bitmap image resource index");
        auto found = decodedImages.find(index);
        if (found == decodedImages.end())
            found = decodedImages.emplace(index, rasterBitmap(imageResources[index], "Bitmap resource")).first;
        // Decode each shared resource once; converted projects embed the node's image pixels.
        return found->second;
    }
    std::optional<yellow::PaintTool> paintTool() {
        const auto kind = version >= 167 ? u32() : 1;
        switch (kind) {
        case 0: return {};
        case 1: {
            const auto width = real("Pen width");
            if (width < 0) fail("Invalid Pen width");
            const auto color = u32(); const auto style = u32();
            if (style > 4) fail("Unsupported Pen dash style");
            return yellow::PenStroke{yellow::Color{color}, width, yellow::LineStyle(style)};
        }
        case 2: return yellow::SolidBrush{yellow::Color{u32()}};
        case 3: {
            const auto start = u32(), end = u32();
            const auto bounds = area(); const auto angle = real("brush angle"); const auto wrap = brushWrap();
            return yellow::LinearGradientBrush{yellow::Color{start}, yellow::Color{end}, bounds, angle, wrap};
        }
        case 4: {
            const bool path = boolean();
            yellow::RadialPath boundary;
            if (path) {
                const auto fill = u32();
                if (fill > 1) fail("Invalid radial path fill mode");
                boundary.winding = fill == 1;
            }
            const auto points = count(yellow::maxRadialPathPoints);
            if (!points && !path) return {}; // Native loader clears an empty polygon.
            for (quint32 i = 0; i < points; ++i) {
                boundary.points.push_back(point());
                if (path) boundary.types.push_back(u8());
            }
            yellow::Area bounds;
            try { bounds = yellow::radialPathBounds(boundary); }
            catch (const std::exception& e) { fail(QString::fromUtf8(e.what())); }
            const auto colors = count(yellow::maxRadialPathPoints);
            if (!colors || colors > points) fail("Invalid radial brush surround color count");
            std::vector<yellow::Color> surrounds;
            for (quint32 i = 0; i < colors; ++i) surrounds.push_back(yellow::Color{u32()});
            const auto start = u32();
            const auto focus = point(); const auto center = point();
            if (focus.x < 0 || focus.x > 1 || focus.y < 0 || focus.y > 1) fail("Invalid radial brush focus");
            auto shape = yellow::RadialShape::Ellipse;
            if (points == 4) {
                std::set<std::pair<float,float>> corners;
                for (const auto p : boundary.points)
                    if ((p.x == bounds.x || p.x == bounds.x + bounds.width) && (p.y == bounds.y || p.y == bounds.y + bounds.height))
                        corners.emplace(p.x,p.y);
                if (corners.size() == 4) shape = yellow::RadialShape::Rectangle;
            }
            yellow::RadialGradientBrush brush{yellow::Color{start}, surrounds.front(), bounds, shape, center, focus};
            brush.boundary = std::move(boundary); brush.surroundColors = std::move(surrounds);
            return brush;
        }
        case 5: {
            const auto foreground = u32(), background = u32(), style = u32();
            if (style > quint32(yellow::HatchStyle::SolidDiamond)) fail("Invalid hatch brush style");
            return yellow::HatchBrush{yellow::Color{foreground}, yellow::Color{background}, yellow::HatchStyle(style)};
        }
        case 6: {
            auto bitmap = textureBitmap(); const auto wrap = brushWrap();
            return yellow::TextureBrush{std::move(bitmap), wrap};
        }
        default: fail(QString("Unsupported paint-tool kind %1").arg(kind));
        }
    }
    void moduleLayout(FlowStoneNode* item = nullptr) {
        const auto position = point();
        const auto width = version < 155 ? float(std::bit_cast<qint32>(u32())) : real();
        const auto height = version < 155 ? float(std::bit_cast<qint32>(u32())) : real();
        if (width <= 0 || height <= 0 || width > 1000000 || height > 1000000) fail("Invalid panel size");
        const auto anchors = version >= 155 ? u8() : 0;
        std::optional<yellow::Area> reference;
        if (anchors & 0x10) reference = area();
        const auto layout = u8();
        if ((layout & 15) != 15) {
            area(); warning("Custom FlowStone drawing areas use OpenStone's panel coordinates.");
        }
        const auto enabled = boolean();
        if (item) {
            warning("Panel frames and drawing extents use OpenStone's layout; FlowStone border padding is not reproduced yet.");
            item->panelPosition = position;
            item->node.panelBounds = {0, 0, width, height};
            item->node.panelAnchors = anchors & 15;
            if (reference) item->node.anchorReference = green::Point{reference->width, reference->height};
            item->panelBorder = !(layout & 16);
            item->panelEnabled = enabled;
        }
    }
    void wirelessRoutes() {
        const auto routes = count();
        for (quint32 i = 0; i < routes; ++i) { const auto length = count(); skip(length * 4); }
    }
    void modulePropertyRoutes() {
        if (u32() != 7) fail("Unsupported module property helper type");
        u8(); wirelessRoutes();
    }
    FlowStoneNode& helperPanel(FlowStoneNode& module, bool properties) {
        FlowStoneNode panel{{int(module.children.size()) + 1, properties ? ModulePropertiesGui : ModuleGui, 0, 0}};
        panel.nativeTag = properties ? 1192 : 1191;
        panel.panelMouse = true;
        module.children.push_back(std::move(panel)); ++result.nodes;
        return module.children.back();
    }
    void decorations(FlowStoneNode& out) {
        auto& n = out.node;
        skip(8); const auto flags = u8(); if (flags & ~3) fail("Unknown module flags");
        if (flags & 1) { skip(2); warning("Synchronized modules are imported as independent modules."); }
        const auto gui = u8(); n.showPanelInParent = gui != 0;
        if (boolean()) {
            auto& panel = helperPanel(out, false); moduleLayout(&panel); skip(2);
            modulePropertyRoutes();
        }
        if (boolean()) {
            auto& panel = helperPanel(out, true); moduleLayout(&panel); modulePropertyRoutes();
        }
        const auto visibility = u8();
        n.panelHidden = visibility & 1;
        // Selecting a properties GUI stores its two editor-only visibility flags.
        if (gui == 1 || gui == 3) { boolean(); boolean(); }
        boolean();
    }
    // CStreamCode cache records are consumed structurally, never instantiated or
    // executed. All supported versions use four-lane initializers and 9-byte refs.
    void cacheReferences() {
        const auto refs = u16();
        if (refs > 32767) fail("Invalid compiled-cache reference count");
        skip(refs * 9);
    }
    void skipCodeCache() {
        u8(); // engine kind
        for (bool output : {false, true}) {
            const auto ports = u8();
            for (int i = 0; i < ports; ++i) { u8(); string(); }
        }
        const auto variables = u16();
        if (variables > 32767) fail("Invalid compiled-cache variable count");
        for (int i = 0; i < variables; ++i) {
            const auto kind = u8();
            if (kind < 1 || kind > 8) fail("Unknown compiled-cache variable kind");
            string();
            if (kind == 1 || kind == 3 || kind == 4) {
                skip(kind == 4 ? 32 : 16); skip(4); cacheReferences();
            } else {
                skip(8); cacheReferences(); // array extent / engine binding
                if (kind == 2 || kind == 5 || kind == 6) {
                    const auto mode = u8();
                    const auto width = kind == 5 ? 8 : 4;
                    if (mode == 1) skip(4 * width);
                    else if (mode == 2) skip(2 * width);
                    else if (mode == 3) skip(4); // serialized initializer index
                    else if (mode != 0) fail("Unknown compiled-cache initializer mode");
                }
            }
        }
        for (int stage = 0; stage < 4; ++stage) blob();
        const auto symbols = count();
        for (quint32 i = 0; i < symbols; ++i) { string(); skip(10); }
    }
    // CProperty connector-box geometry; stored values follow this payload.
    void connectorLayout() {
        const auto boxes = count(4096);
        for (quint32 i = 0; i < boxes; ++i) {
            skip(24);
            if (version >= 157) {
                for (int flag = 0; flag < 4; ++flag) boolean();
                if (boolean()) skip(16);
            }
        }
        const auto order = count(4096); skip(order * 2);
        warning("Custom connector layout is not reproduced in OpenStone.");
    }
    // CInstAllMath stores connector types followed by locked-connector indices.
    void genericMath(SchematicNode& n) {
        const auto& spec = primitive(n.kind);
        std::optional<Type> chosen;
        for (int side = 0; side < 2; ++side) {
            const auto& expected = side ? spec.outputs : spec.inputs;
            const auto ports = count(32);
            if (ports != expected.size()) fail("Unsupported generic math connector count");
            for (quint32 i = 0; i < ports; ++i) {
                const auto t = type(u32());
                if (expected[i].type) {
                    if (t && t != expected[i].type) {
                        if (side && spec.behavior == PrimitiveBehavior::Compare) fail("Invalid comparison output connector type");
                        fail("Invalid fixed generic connector type");
                    }
                    continue;
                }
                if (t) {
                    if (chosen && chosen != t) fail("Mixed generic math connector types are not supported yet");
                    chosen = t;
                }
            }
        }
        for (int side = 0; side < 2; ++side) {
            const auto locked = count(32);
            for (quint32 i = 0; i < locked; ++i) {
                if (u32() >= (side ? spec.outputs.size() : spec.inputs.size())) fail("Invalid generic math locked connector");
            }
        }
        if (chosen && !Schematic::supportsTemplateType(n.kind, *chosen)) fail("Unsupported generic math connector type");
        n.typeOverride = chosen;
    }
    FlowStoneNode node(int depth = 0, int id = 0) {
        if (depth > 64 || ++result.nodes > 100000) fail("Module or node limit exceeded");
        auto tag = u16();
        FlowStoneNode out{{id, Module, 0, 0}};
        out.nativeTag = tag;
        auto& n = out.node;
        if (tag == 1) {
            skip(12); n.moduleName = string();
            n.x = u16() * 16.0; n.y = u16() * 16.0;
            auto old = path; path += "/" + QString::fromStdString(n.moduleName);
            auto count = u8(); const auto children = count == 255 ? u16() : count;
            for (int i = 0; i < children; ++i) out.children.push_back(node(depth + 1, i + 1));
            auto links = u16();
            std::set<SchematicLink> unique;
            for (int i = 0; i < links; ++i) {
                const auto route = u8();
                if (route) { skip(route * 4); warning("Link routing points use OpenStone's link layout."); }
                auto source = u16(); auto output = u8(); auto target = u16(); auto input = u8();
                skip(2); if (version >= 165) skip(2); if (version >= 174) skip(1);
                if (source >= out.children.size() || target >= out.children.size()) fail("Invalid link node index");
                auto& a = out.children[source]; auto& b = out.children[target];
                const auto portCount = [](const FlowStoneNode& c, bool output) {
                    if (c.nativeTag == 105) return output ? 3 : 4;
                    if (c.nativeTag == 181) return output ? 1 : 0;
                    if (c.nativeTag == 16 || c.nativeTag == 400) return output ? 3 : c.nativeTag == 400 ? 4 : 5;
                    if (c.nativeTag == 1191 || c.nativeTag == 1192) return output ? 1 : c.nativeTag == 1191 ? 3 : 2;
                    if (c.nativeTag == 131 || c.nativeTag == 132) return output ? 1 : c.nativeTag == 131 ? 4 : 3;
                    if (c.storedCodePorts) return output ? c.storedCodePorts->second + 1 : c.storedCodePorts->first;
                    if (c.node.rubyCode) return static_cast<int>(output ? c.node.rubyCode->ports.outputs.size() : c.node.rubyCode->ports.inputs.size());
                    return static_cast<int>(output ? c.node.outputPorts().size() : c.node.inputPorts().size());
                };
                if (output >= portCount(a, true) || input >= portCount(b, false))
                    fail(QString("Invalid link connector index (%1:%2 -> %3:%4; component IDs %5 -> %6)")
                             .arg(source + 1).arg(output).arg(target + 1).arg(input).arg(a.nativeTag).arg(b.nativeTag));
                // HiRes changes native repaint scheduling, which has no equivalent input yet.
                // Reject an actual connection explicitly; never discard it as unused metadata.
                if ((b.nativeTag == 131 && input == 3) || (b.nativeTag == 132 && input == 2))
                    fail("Linked Redraw HiRes inputs are not supported yet");
                if (b.nativeTag == 105 && input == 2) fail("Linked Bitmap Paste inputs are not supported yet");
                // Bitmap has a native clipboard-Paste socket before Clear.
                if (b.nativeTag == 105 && input == 3) input = 2;
                // Keep native indices until the complete hierarchy's source links resolve.
                SchematicLink link{source + 1, target + 1, input, output};
                if (!unique.insert(link).second) fail("Duplicate link");
                out.links.push_back(link);
                if (++result.links > 1000000) fail("Link limit exceeded");
            }
            // Connector order follows the boundary nodes' vertical positions, then horizontal positions.
            for (bool output : {false, true}) {
                std::vector<SchematicNode*> ports;
                for (auto& c : out.children) if (c.node.kind == (output ? ModuleOutput : ModuleInput)) ports.push_back(&c.node);
                std::ranges::stable_sort(ports, [](auto* a, auto* b) { return std::tie(a->y, a->x) < std::tie(b->y, b->x); });
                for (auto* p : ports) {
                    auto& list = output ? n.moduleOutputs : n.moduleInputs;
                    p->boundaryIndex = list.size(); list.push_back({p->moduleName, p->boundaryType, p->id});
                }
            }
            decorations(out); path = old;
        } else {
            n.x = u16() * 16.0; n.y = u16() * 16.0; n.label = string();
            switch (tag) {
            case 17:
                n.kind = Trigger;
                boolean(); if (boolean()) connectorLayout();
                break;
            case 103: n.kind = FileDialog; n.flowStoneFileDialog = true; break;
            case 105: n.kind = Bitmap; n.localValue = bitmapRecord(); break;
            case 16: case 400: case 1191: case 1192:
                n.kind = tag == 400 || tag == 1192 ? ModulePropertiesGui : ModuleGui;
                moduleLayout(&out); break;
            case 54: n.kind = Area; break;
            case 130: n.kind = AreaToFloat; break;
            case 420: n.kind = ViewArea; break;
            case 609: n.kind = ViewCache; break;
            case 64: n.kind = ArgbToColor; break;
            case 143: n.kind = AhsvToColor; break;
            case 223: n.kind = ColorToArgb; break;
            case 142: n.kind = ColorToAhsv; break;
            case 421: n.kind = ViewSize; break;
            case 62: n.kind = Line; break;
            case 53: case 98: n.kind = RoundRectangle; break;
            case 131: case 132:
                n.kind = tag == 131 ? RedrawArea : Redraw;
                warning("Redraw requests use OpenStone's repaint scheduler; FlowStone's 100Hz automation limit is not reproduced.");
                break;
            case 218: case 219: n.kind = Rect; break;
            case 220: case 221: n.kind = Ellipse; break;
            case 109: n.kind = Arc; break;
            case 386: n.kind = Pie; break;
            case 96: n.kind = Text; break;
            case 214: {
                n.kind = Pen;
                // Before v159 this was a stateless builder, with no property payload.
                if (version < 159) break;
                boolean(); if (boolean()) connectorLayout();
                auto tool = paintTool();
                if (tool && !std::holds_alternative<yellow::PenStroke>(*tool)) fail("Unsupported Pen paint-tool kind");
                if (tool) n.localValue = std::move(*tool);
                break;
            }
            case 351: case 352: case 353: case 354: case 369: {
                n.kind = tag == 351 ? Solid : tag == 352 ? LinearGradientBrush : tag == 353 ? RadialGradientBrush : tag == 354 ? HatchBrush : TextureBrush;
                auto tool = paintTool();
                if (tool && version < 167) {
                    warning("Legacy brush caches use stroke encoding; brush values are rebuilt from their inputs.");
                } else if (tool) {
                    const auto expected = tag == 369 ? 5 : tag - 350;
                    if (tool->index() != expected) fail("Mismatched brush paint-tool kind");
                    n.localValue = std::move(*tool);
                }
                break;
            }
            case 216: {
                n.kind = StringFormat;
                if (version < 159) break;
                boolean(); if (boolean()) connectorLayout();
                const auto flags = u32(), horizontal = u32(), vertical = u32();
                if (flags & ~yellow::stringFormatFlagMask || horizontal > 2 || vertical > 2) fail("Invalid String Format flags or alignment");
                auto format = yellow::stringFormatFromFlags(flags);
                format.horizontal = yellow::HorizontalAlignment(horizontal); format.vertical = yellow::VerticalAlignment(vertical);
                n.localValue = format;
                break;
            }
            case 215: {
                n.kind = Font;
                if (version < 159) break;
                boolean(); if (boolean()) connectorLayout();
                yellow::Font font;
                font.face = string(); font.size = real("Font size");
                if (font.face.empty() || font.size <= 0) fail("Invalid Font face or size");
                const auto style = u32();
                if (style & ~15u) fail("Unsupported Font style");
                font.style = {bool(style & 1), bool(style & 2), bool(style & 4), bool(style & 8)};
                if (version >= 171) {
                    const auto fallbackCount = count(4096);
                    for (quint32 i = 0; i < fallbackCount; ++i) {
                        auto face = string();
                        if (face.empty()) fail("Invalid Font fallback face");
                        if (i == 0) font.alternateFace = std::move(face);
                        else font.fallbackFaces.push_back(std::move(face));
                    }
                }
                n.localValue = std::move(font);
                break;
            }
            case 403: {
                n.kind = Color; boolean(); if (boolean()) connectorLayout();
                const auto a = u8(), r = u8(), g = u8(), b = u8();
                n.localValue = yellow::Color{(quint32(a) << 24) | (quint32(r) << 16) | (quint32(g) << 8) | b};
                break;
            }
            case 402:
                n.kind = WirelessInput; n.typeOverride = type(u8()); u8(); out.viewScope = u16();
                n.wirelessLabel = n.label; out.implicitView = n.typeOverride == Type::View && n.label.empty();
                break;
            case 401: case 419:
                n.kind = tag == 419 ? ModuleWirelessOutput : WirelessOutput;
                n.typeOverride = type(u32()); u8(); n.wirelessLabel = n.label; wirelessRoutes(); break;
            case 137: {
                n.kind = Selector; const auto inputs = count(4096);
                // Saved count excludes Index (first) and the final spare value input.
                n.typeOverride = type(u32()); n.inputTriggers.resize(inputs + 2); break;
            }
            case 149: {
                n.kind = Multiplexer;
                const auto outputs = count(4096); // Excludes the final spare output.
                n.typeOverride = type(u32());
                n.flowStoneRoutingBranches = outputs;
                n.outputTriggers.resize(outputs + 1);
                break;
            }
            case 138: case 139: {
                n.kind = tag == 138 ? ModuleInput : ModuleOutput;
                n.boundaryType = type(u32()); n.typeOverride = n.boundaryType;
                skip(1); n.moduleName = string();
                if (!n.label.empty()) n.moduleName = n.label;
                else n.label = n.moduleName;
                break;
            }
            case 11: case 29: case 88: case 117: case 118: case 119: case 406: {
                skip(4); boolean(); if (boolean()) connectorLayout();
                if (tag == 29) { n.kind = Int; n.localValue = std::bit_cast<qint32>(u32()); }
                else if (tag == 11) { n.kind = Float; n.localValue = std::bit_cast<float>(u32()); }
                else if (tag == 406) { n.kind = Boolean; n.localValue = boolean(); }
                else { n.kind = String; n.localValue = string(); }
                break;
            }
            case 15: case 298: n.kind = Add; n.typeOverride = Type::Stream; break;
            case 20: case 78: n.kind = Multiply; n.typeOverride = Type::Stream; break;
            case 295: case 297: n.kind = Subtract; n.typeOverride = Type::Stream; break;
            case 294: case 296: n.kind = Divide; n.typeOverride = Type::Stream; break;
            case 85: case 308: n.kind = Max; n.typeOverride = Type::Stream; break;
            case 86: case 307: n.kind = Min; n.typeOverride = Type::Stream; break;
            case 236: {
                n.kind = String;
                if (version >= 177) { boolean(); if (boolean()) connectorLayout(); }
                skip(8); n.localValue = string();
                const auto marks = count(); skip(marks * 8);
                break;
            }
            case 1048: case 1049: case 1050: case 1051: case 1052: {
                constexpr PrimitiveKind kinds[]{Equal, LessThan, LessThanOrEqual, GreaterThan, GreaterThanOrEqual};
                n.kind = kinds[tag - 1048]; n.compareArrayElements = true; genericMath(n); break;
            }
            case 622: case 623: case 624: case 625: case 626: {
                constexpr PrimitiveKind kinds[]{EqualTrigger, LessThanTrigger, LessThanOrEqualTrigger, GreaterThanTrigger, GreaterThanOrEqualTrigger};
                n.kind = kinds[tag - 622]; n.compareArrayElements = true; genericMath(n); break;
            }
            case 1042: case 1043: case 1044: case 1045: case 1046: case 1047: {
                constexpr PrimitiveKind kinds[]{Add, Subtract, Multiply, Divide, Max, Min};
                n.kind = kinds[tag - 1042]; genericMath(n); break;
            }
            case 99: case 675:
                n.kind = Power;
                if (version >= 170) genericMath(n); else n.typeOverride = Type::Float;
                break;
            case 240:
                n.kind = Log10;
                if (version >= 170) genericMath(n); else n.typeOverride = Type::Float;
                break;
            case 429: n.kind = FromDecibel; genericMath(n); break;
            case 256: n.kind = Not; break; // Fixed Boolean ports; no primitive-specific state.
            case 1053:
                n.kind = SampleAndHold; genericMath(n); n.flowStoneSampleAndHold = true;
                if (!n.typeOverride) break;
                switch (*n.typeOverride) {
                case Type::Float: n.localValue = std::bit_cast<float>(u32()); break;
                case Type::Int: n.localValue = std::bit_cast<qint32>(u32()); break;
                case Type::Boolean: n.localValue = boolean(); break;
                case Type::String: n.localValue = string(); break;
                case Type::Trigger: break;
                default: fail("Unsupported Sample and Hold cached-value type");
                }
                break;
            case 478: n.kind = RedrawLimiter; break;
            case 181: n.kind = Tick; n.tick = TickState{}; break;
            case 258: n.kind = AfterLoad; break;
            case 551: n.kind = AfterDuplicate; break;
            case 166: n.kind = MidiCC; break; // No primitive-specific state.
            case 326: n.kind = BasicCounter; n.localValue = std::bit_cast<qint32>(u32()); break;
            case 237:
                n.kind = DropList;
                // Native Drop List stores only its selected index. Keeping it
                // as an Int also selects the native value/notification policy.
                n.localValue = std::bit_cast<qint32>(u32());
                break;
            case 189: {
                n.kind = PresetFloat;
                auto& parameter = n.presetParameter.emplace();
                const auto programs = count(maxPresetPrograms);
                for (quint32 program = 0; program < programs; ++program)
                    parameter.saved.emplace(program, std::bit_cast<float>(u32()));
                const bool locked = boolean();
                parameter.current = std::bit_cast<float>(u32());
                const auto order = std::bit_cast<float>(u32());
                parameter.flowStoneFloat = PresetParameterState::FlowStoneFloat{locked, order, programs == 0};
                // Native OnInit refreshes this recording cache from the live Rec input.
                boolean();
                if (version >= 162) {
                    if (version >= 164) {
                        const auto binding = std::bit_cast<qint32>(u32());
                        if (binding != -1) fail("Preset automation bindings are not supported yet");
                    }
                    if (version >= 163) {
                        // Four editor flags for the automation lane; no lane is
                        // restored unless it has an actual recording below.
                        for (int flag = 0; flag < 4; ++flag) boolean();
                    } else if (std::bit_cast<qint32>(u32()) > 0) fail("Preset automation bindings are not supported yet");
                    if (version >= 164) u32(); // cached automation-lane index
                    if (count()) fail("Preset automation recordings are not supported yet");
                }
                break;
            }
            case 408: {
                n.kind = IndexSelector;
                const auto width = real("Index Selector width"), height = real("Index Selector height");
                if (width <= 0 || height <= 0) fail("Invalid Index Selector size");
                const auto selected = std::bit_cast<qint32>(u32());
                if (selected < -1) fail("Invalid Index Selector selection");
                n.localValue = selected;
                boolean(); if (boolean()) connectorLayout();
                warning("Index Selector uses OpenStone's node menu and layout; FlowStone's property-box appearance is not reproduced.");
                break;
            }
            case 1056: n.kind = TriggerBlocker; genericMath(n); break;
            case 1054:
                n.kind = LastSwitch; genericMath(n);
                n.lastSwitchInput = u8();
                if (n.lastSwitchInput > 1) fail("Invalid Last Switch input index");
                n.flowStoneRoutingBranches = 2;
                break;
            case 1060: n.kind = TriggerSwitch; genericMath(n); break;
            case 1057:
                n.kind = Select; genericMath(n);
                // Cached condition for suppressing repeated control notifications;
                // the current branch still comes from the live Condition input.
                n.localValue = boolean(); break;
            case 535: n.kind = DeZipper; break;
            case 594: n.kind = FloatDeZipper; break;
            case 472: n.kind = SampleRate; break;
            case 77: case 1141:
                n.kind = tag == 77 ? Delay : DoubleDelay;
                warning("Delay length changes retain OpenStone's existing delay history.");
                break;
            case 33: n.kind = Sine; break;
            case 1148: n.kind = DoubleSine; break;
            case 27: case 72: case 157: case 196: n.kind = Sawtooth; break;
            case 1149: n.kind = DoubleSawtooth; break;
            case 165: n.kind = Triangle; break;
            case 1150: n.kind = DoubleTriangle; break;
            case 32: n.kind = Ramp; break;
            case 36: n.kind = Impulse; break;
            case 533: n.kind = Glide; break;
            case 35: n.kind = StreamPitchToFreq; break;
            case 30: n.kind = DelayOneSample; break;
            case 205: n.kind = Pack; break;
            case 206: n.kind = Unpack; break;
            case 170: n.kind = TooltipHelp; break;
            case 1110: {
                n.kind = Ruby; n.codeSourcePorts = true;
                auto source = string(); skip(16);
                if (boolean()) fail("Disabled Ruby boxes are not supported yet");
                auto ins = count(maxRubyPorts); auto outs = count(maxRubyPorts);
                RubyPorts ports;
                for (bool output : {false, true}) {
                    auto& list = output ? ports.outputs : ports.inputs;
                    for (quint32 i = 0; i < (output ? outs : ins); ++i) {
                        const auto t = type(u16()).value_or(Type::RubyValue);
                        auto name = string();
                        if (name.empty()) name = std::string(output ? "output" : "input") + std::to_string(i);
                        if (!supportsRubyPort(t, output)) fail("Unsupported Ruby port type");
                        list.push_back({std::move(name), t});
                    }
                }
                auto state = count(limit);
                skip(state);
                if (state) warning("Ruby source and connectors are imported; FlowStone's saved Ruby state is not restored yet.");
                const auto bookmarks = count(); skip(bookmarks * 8);
                n.rubyCode = rubyCode(source, source, std::move(ports)); break;
            }
            case 22: case 154: {
                n.kind = tag == 22 ? Assembler : DspCode; n.codeSourcePorts = true;
                skip(8); const auto ins = u8(); const auto outs = u8();
                out.storedCodePorts = std::pair{ins, outs};
                out.storedSource = string();
                const auto bookmarks = count(); skip(bookmarks * 8);
                const auto caches = u8();
                for (int i = 0; i < caches; ++i) skipCodeCache();
                if (caches) warning("FlowStone's compiled code caches were skipped; source is compiled by OpenStone.");
                break;
            }
            default: fail(QString("Unsupported component ID %1 (0x%2)").arg(tag).arg(tag, 0, 16));
            }
        }
        n.inputTriggers.resize(n.inputPorts().size()); n.outputTriggers.resize(n.outputPorts().size());
        return out;
    }
    void resolveSelectors(FlowStoneNode& item) {
        if (item.node.kind != Module) return;
        const auto original = item.children.size();
        std::vector<std::vector<std::size_t>> indexLinks(original + 1);
        for (std::size_t i = 0; i < item.links.size(); ++i) {
            const auto& link = item.links[i];
            if (link.input == 0 && item.children[link.target - 1].nativeTag == 137) indexLinks[link.target].push_back(i);
        }
        for (std::size_t i = 0; i < original; ++i) {
            if (item.children[i].nativeTag != 137) continue;
            const auto selector = item.children[i].node;
            if (indexLinks[selector.id].empty()) continue;
            if (result.nodes > 100000 - 4 || result.links > 1000000 - 4) fail("Expanded graph exceeds the import limits");
            const auto add = [&](PrimitiveKind kind, double x, double y) -> FlowStoneNode& {
                const auto id = int(item.children.size()) + 1;
                item.children.push_back(FlowStoneNode{{id, kind, x, y}}); ++result.nodes;
                return item.children.back();
            };
            const auto zero = add(Int, selector.x - 320, selector.y).node.id;
            item.children.back().node.localValue = std::int32_t{0};
            const auto upper = add(Int, selector.x - 320, selector.y + 96).node.id;
            item.children.back().node.localValue = std::int32_t(std::max<std::ptrdiff_t>(0, std::ssize(selector.inputTriggers) - 3));
            const auto lowerClamp = add(Max, selector.x - 160, selector.y).node.id;
            item.children.back().node.typeOverride = Type::Int;
            const auto upperClamp = add(Min, selector.x - 160, selector.y + 96).node.id;
            item.children.back().node.typeOverride = Type::Int;
            // Native Selector clamps the combined Index to its populated range;
            // OpenStone's own Selector returns the default for an invalid Index.
            for (const auto index : indexLinks[selector.id]) item.links[index].target = lowerClamp;
            item.links.insert(item.links.end(), {{zero, lowerClamp, 1}, {lowerClamp, upperClamp, 0},
                                                {upper, upperClamp, 1}, {upperClamp, selector.id, 0}});
            result.links += 4;
        }
        for (auto& child : item.children) resolveSelectors(child);
    }
    void resolvePanels(FlowStoneNode& item, FlowStoneNode* parent = nullptr) {
        if (item.node.kind != Module) return;
        const auto add = [&](PrimitiveKind kind) -> FlowStoneNode& {
            const auto id = int(item.children.size()) + 1;
            item.children.push_back(FlowStoneNode{{id, kind, 0, 0}}); ++result.nodes;
            return item.children.back();
        };
        // Translate the old MGUI's coordinate inputs and Width/Height outputs.
        const auto original = item.children.size();
        std::set<PrimitiveKind> configured;
        for (std::size_t i = 0; i < original; ++i) {
            const auto tag = item.children[i].nativeTag;
            if (!ownsPanel(item.children[i].node.kind)) continue;
            const auto panel = item.children[i];
            const bool properties = panel.node.kind == ModulePropertiesGui;
            if (configured.insert(panel.node.kind).second) {
                if (!properties && !panel.panelEnabled) item.node.showPanelInParent = false;
                (properties ? item.node.propertiesPosition : item.node.panelPosition) = *panel.panelPosition;
            } else warning("Multiple drawing surfaces of one kind use the first surface for the module panel.");
            std::vector<SchematicLink> controlLinks;
            for (auto& link : item.links) {
                if (link.target == panel.node.id) {
                    const bool legacy = tag == 16 || tag == 400;
                    if (legacy && (link.input == 1 || link.input == 2)) fail("Linked legacy MGUI position inputs are not supported yet");
                    if ((tag == 16 && link.input == 3) || (tag == 1191 && link.input == 2)) {
                        auto& invert = add(Not); invert.node.typeOverride = Type::Boolean;
                        link.target = invert.node.id; link.input = 0;
                        controlLinks.push_back({invert.node.id, panel.node.id, 2, 0});
                    } else if (legacy && link.input) link.input = 1;
                }
            }
            result.links += controlLinks.size();
            item.links.insert(item.links.end(), controlLinks.begin(), controlLinks.end());
            const bool needsSize = std::ranges::any_of(item.links, [&](const auto& link) {
                return link.source == panel.node.id && link.output > 0;
            });
            if (needsSize) {
                const auto size = add(ViewSize).node.id;
                for (auto& link : item.links) if (link.source == panel.node.id && link.output > 0) {
                    link.source = size; --link.output;
                }
                item.links.push_back({panel.node.id, size, 0, 0}); ++result.links;
            }
            for (int port : {1, 2}) {
                if (properties && port == 2) continue;
                if (std::ranges::any_of(item.links, [&](const auto& l) { return l.target == panel.node.id && l.input == port; })) continue;
                auto& value = add(Boolean); value.node.localValue = port == 1 ? panel.panelMouse : panel.panelBorder;
                item.links.push_back({value.node.id, panel.node.id, port, 0}); ++result.links;
            }
        }
        if (parent) for (auto& child : item.children) if (child.implicitView) {
            const bool properties = child.viewScope >= 20000;
            if (child.viewScope && child.viewScope != 20000 && child.viewScope != 20001)
                warning("Explicit cached View-source selectors use the enclosing drawing surface.");
            const auto source = std::ranges::find_if(parent->children, [&](const auto& n) {
                return n.node.kind == (properties ? ModulePropertiesGui : ModuleGui);
            });
            if (source == parent->children.end()) {
                warning("An inherited View has no enclosing drawing surface in this schematic.");
                continue;
            }
            child.node.kind = ModuleInput; child.node.boundaryType = Type::View;
            child.node.boundaryIndex = item.node.moduleInputs.size();
            child.node.moduleName = child.node.label = "Parent View";
            item.node.moduleInputs.push_back({child.node.label, Type::View, child.node.id});
            parent->links.push_back({source->node.id, item.node.id, child.node.boundaryIndex, 0}); ++result.links;
        }
        for (auto& child : item.children) resolvePanels(child, &item);
    }
    void compileCode(FlowStoneNode& item, const QString& location) {
        path = location;
        if (item.storedCodePorts) {
            try {
                auto program = stream::compileDspCode(item.storedSource, dspLanguage(item.node.kind));
                const auto [ins, outs] = *item.storedCodePorts;
                if (program.inputs().size() != ins || program.outputs().size() != outs) fail("Code declarations disagree with stored connectors");
                item.node.dspCode = DspCodeState{item.storedSource, item.storedSource, std::move(program)};
            } catch (const stream::DspCompileError& error) {
                fail("Cannot compile source: " + QString::fromUtf8(error.what()));
            }
        }
        // Native Ruby's declared connectors are data ports starting at zero.
        // Its stored source was installed during parsing without evaluating Ruby.
        for (auto& child : item.children) compileCode(child, location + "/" + QString::fromStdString(
            child.node.kind == Module ? child.node.moduleName : (child.node.label.empty() ? std::to_string(child.node.id) : child.node.label)));
        for (auto& link : item.links) {
            const auto& source = item.children.at(link.source - 1).node;
            if (source.dspCode && source.codeSourcePorts) link.output = link.output ? link.output - 1 : *source.codeSourcePort(true);
        }
        item.node.inputTriggers.resize(item.node.inputPorts().size());
        item.node.outputTriggers.resize(item.node.outputPorts().size());
    }
    FlowStoneImport parse() {
        version = u16(); result.version = version;
        if (version < 154 || version > 177) fail(QString("Unsupported schematic version %1; supported range is 154–177").arg(version));
        if (u32() != 1) fail("Multiple schematic views are not supported yet");
        auto route = count(64); skip(route * 2);
        skip(3); if (boolean()) skip(12);
        if (version >= 171) {
            auto fonts = count(4096);
            if (fonts) warning("Embedded font resources are not imported yet.");
            while (fonts--) blob();
        }
        auto images = count(4096);
        while (images--) imageResources.push_back(take(count(limit)));
        if (version >= 161) {
            skip(8 + 6 * 8); boolean();
            if (count()) fail("Sequencer tracks are not supported yet");
        }
        if (u16() != 1) fail("Missing schematic root");
        // The document owns the root class tag; its body follows directly.
        pos -= 2;
        result.root = node();
        skip(2); boolean(); // selected connector and editor view mode
        if (!done()) fail("Unexpected trailing schematic data");
        if (std::ranges::any_of(result.root.children, [](const auto& c) { return ownsPanel(c.node.kind); })) {
            auto content = std::move(result.root); content.node.id = 1;
            result.root = FlowStoneNode{{0, Module, 0, 0}};
            result.root.children.push_back(std::move(content)); ++result.nodes;
            warning("The root drawing surface is imported inside a module.");
        }
        resolveSelectors(result.root);
        resolvePanels(result.root);
        if (result.nodes > 100000 || result.links > 1000000) fail("Expanded graph exceeds the import limits");
        compileCode(result.root, "Root");
        path = "Root";
        --result.nodes; // Root is not an editor node.
        warning("FlowStone import is experimental. Some drawing layouts, resource bindings and unsupported component records require further work.");
        return std::move(result);
    }
};
}

bool FlowStoneFile::isArchive(const QByteArray& data) { return data.startsWith(QByteArray("PK\3\4", 4)); }
FlowStoneImport FlowStoneFile::decode(const QByteArray& archive) { return Parser(schematic(archive)).parse(); }
}
