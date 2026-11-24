#include "FlowStoneFile.hpp"
#include "SchematicFile.hpp"
#include "PanelRadialBrush.hpp"
#include "PanelShapePath.hpp"
#include <QColor>
#include <QJsonArray>
#include <QJsonDocument>
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <zlib.h>
#include <bit>
#include <cmath>
#include <algorithm>
#include <limits>
#include <iostream>
#include <stdexcept>
#include <tuple>

namespace {
using namespace openstone;
using namespace openstone::desktop;
void check(bool b, const char* message) { if (!b) throw std::runtime_error(message); }
struct Bytes : QByteArray {
    void b(quint8 x) { append(char(x)); }
    void w(quint16 x) { char a[2]; qToLittleEndian(x, a); append(a, 2); }
    void d(quint32 x) { char a[4]; qToLittleEndian(x, a); append(a, 4); }
    void f(float x) { d(std::bit_cast<quint32>(x)); }
    void str(const QByteArray& s) { if (s.size() < 255) b(s.size()); else { b(255); w(s.size()); } append(s); }
    void zeros(int n) { append(QByteArray(n, 0)); }
};
QByteArray zip(const QByteArray& payload, bool deflate = true) {
    QByteArray packed = payload;
    if (deflate) {
        packed.resize(compressBound(payload.size())); z_stream stream{};
        check(deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK, "Initialize fixture compression");
        stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(payload.constData())); stream.avail_in = payload.size();
        stream.next_out = reinterpret_cast<Bytef*>(packed.data()); stream.avail_out = packed.size();
        check(::deflate(&stream, Z_FINISH) == Z_STREAM_END, "Compress fixture");
        packed.resize(stream.total_out); deflateEnd(&stream);
    }
    auto crc = crc32(0, reinterpret_cast<const Bytef*>(payload.constData()), payload.size());
    Bytes z; z.d(0x04034b50); z.w(20); z.w(0); z.w(deflate ? 8 : 0); z.d(0);
    z.d(crc); z.d(packed.size()); z.d(payload.size()); z.w(13); z.w(0); z.append("schematic.osm"); z.append(packed);
    auto start = z.size(); z.d(0x02014b50); z.w(20); z.w(20); z.w(0); z.w(deflate ? 8 : 0); z.d(0);
    z.d(crc); z.d(packed.size()); z.d(payload.size()); z.w(13); z.w(0); z.w(0); z.w(0); z.w(0); z.d(0); z.d(0); z.append("schematic.osm");
    auto size = z.size() - start; z.d(0x06054b50); z.w(0); z.w(0); z.w(1); z.w(1); z.d(size); z.d(start); z.w(0);
    return z;
}
void beginModule(Bytes& b, const char* name, int count) {
    b.w(1); b.f(8); b.f(0); b.f(0); b.str(name); b.w(10); b.w(10); b.b(count);
}
void endModule(Bytes& b) { b.d(6); b.d(4); b.zeros(6); }
void header(Bytes& b, int version) {
    b.w(version); b.d(1); b.d(1); b.w(0); b.zeros(3); b.b(1); b.zeros(12);
    if (version >= 171) b.d(0); // font resources
    b.d(0); // images
    if (version >= 161) { b.zeros(8 + 6 * 8); b.b(1); b.d(0); }
}
void primitive(Bytes& b, int type, int x, int y, const char* label = "") { b.w(type); b.w(x); b.w(y); b.str(label); }
void wire(Bytes& b, int version, int source, int output, int target, int input) {
    b.b(0); b.w(source); b.b(output); b.w(target); b.b(input); b.zeros(2);
    if (version >= 165) b.zeros(2); if (version >= 174) b.zeros(1);
}
QByteArray fixture(int version = 166) {
    Bytes b; header(b, version); beginModule(b, "Root", 3);
    primitive(b, 154, 2, 3, "Input value"); b.f(20); b.f(10); b.b(0); b.b(1);
    b.str("streamout output; output=0.25;"); b.d(0); b.b(0);
    beginModule(b, "Phase invert", 3);
    primitive(b, 138, 2, 2); b.d(6); b.b(2); b.str("In");
    primitive(b, 139, 20, 2); b.d(6); b.b(2); b.str("Out");
    primitive(b, 295, 10, 4);
    b.w(2); wire(b, version, 0, 0, 2, 1); wire(b, version, 2, 0, 1, 0); endModule(b);
    primitive(b, 154, 30, 3); b.f(20); b.f(10); b.b(1); b.b(1);
    b.str("streamin input; streamout output; output=input;"); b.d(0); b.b(0);
    b.w(2); wire(b, version, 0, 1, 1, 0); wire(b, version, 1, 0, 2, 0); endModule(b);
    b.w(1); b.b(0); return b;
}
void expectError(const QByteArray& input, const char* fragment) {
    Session session; const auto original = session.graph().addNode(PrimitiveKind::Int, 7, 9, 42);
    auto before = SchematicFile::project(session); bool touched = false;
    try { SchematicFile::importFlowStone(session, input, [&] { touched = true; }); }
    catch (const std::exception& e) {
        check(std::string(e.what()).find(fragment) != std::string::npos, e.what());
        check(!touched && SchematicFile::project(session) == before && session.graph().nodes().front().id == original, "Failed import must leave current project unchanged"); return;
    }
    throw std::runtime_error("Expected failed import");
}
void importsAndRoundTrips() {
    for (int version : {154, 159, 161, 165, 166, 171, 174, 177}) for (bool deflate : {false, true}) {
        auto archive = zip(fixture(version), deflate); auto parsed = FlowStoneFile::decode(archive);
        check(parsed.version == version && parsed.nodes == 6 && parsed.links == 4, "Decode hierarchy and wiring");
        check(parsed.root.children[1].node.moduleInputs[0].name == "In", "Import module port names");
        check(parsed.root.children[0].node.x == 32 && parsed.root.children[0].node.y == 48, "Import grid coordinates");
        Session session; int calls = 0;
        check(SchematicFile::importFlowStone(session, archive, [&] { ++calls; }), "Import into Session");
        check(calls == 1 && session.graph().nodes().size() == 3, "Replace once after successful validation");
        check(session.graph().module(2)->links().size() == 2 && session.graph().links().size() == 2, "Preserve internal and external wiring");
        for (std::size_t block : {1, 7, 64}) {
            const auto captured = session.graph().captureDsp(3, 128, block);
            check(captured.size() == 1 && captured[0].size() == 128, "Capture imported DSP graph");
            for (double sample : captured[0]) check(sample == -0.25, "Imported nested subtract graph renders correctly");
        }
        auto saved = SchematicFile::project(session); Session reopened;
        check(SchematicFile::loadProject(reopened, saved, {}), "Reopen converted .ostone");
        check(SchematicFile::project(reopened) == saved, "Converted project round-trips unchanged");
    }
}
void rejectsInvalidFiles() {
    expectError("not a schematic", "Missing ZIP");
    auto bad = zip(fixture(), false); bad[30 + 13] ^= 1; expectError(bad, "checksum");
    auto payload = fixture(); payload[0] = 1; payload[1] = 0; expectError(zip(payload), "version");
    payload = fixture(); payload.chop(4); expectError(zip(payload), "Truncated");
    payload = fixture(); payload.append('x'); expectError(zip(payload), "trailing");
    payload = fixture();
    const auto boundary = payload.indexOf(QByteArray::fromHex("8b00140002000006000000"));
    check(boundary >= 0, "Locate output boundary"); payload[boundary + 7] = 15;
    expectError(zip(payload), "DSP links require matching");
    for (int n : {0, 1, 10, 22, 35, 80}) expectError(zip(fixture().left(n)), n ? "Truncated" : "Unsupported ZIP");
    payload = fixture(); // First primitive follows Root module header.
    const auto at = payload.indexOf("Input value") - 7;
    // Replace its tag through the known common record prefix.
    const auto tag = payload.indexOf(QByteArray::fromHex("9a0002000300"));
    check(at >= 0 && tag >= 0, "Locate fixture node"); payload[tag] = char(0xfe); payload[tag + 1] = char(0x7f);
    expectError(zip(payload), "Unsupported component ID 32766");
}
void heldClose() {
    using enum PrimitiveKind;
    Session session; auto& graph = session.graph();
    const auto close = graph.addNode(AboutToClose, 0, 0);
    const auto hold = graph.addNode(Boolean, 0, 0, true);
    const auto done = graph.addNode(Trigger, 0, 0);
    graph.connect({hold, close, 0}); graph.connect({done, close, 1});
    int replacements = 0; QStringList warnings;
    check(!SchematicFile::importFlowStone(session, zip(fixture()), [&] { ++replacements; }, &warnings), "Import respects a held close");
    check(replacements == 0 && &session.graph() == &graph && !warnings.isEmpty(), "Old schematic remains live while Done is pending");
    graph.fireTrigger(done);
    check(session.completeSchematicOpen() && replacements == 1 && session.graph().nodes().size() == 3, "Complete the prepared import once Done fires");
}

void valuesAndRuby() {
    Bytes b; header(b, 166); beginModule(b, "Root", 5);
    primitive(b, 29, 0, 0); b.f(8); b.zeros(2); b.d(quint32(-123));
    primitive(b, 406, 0, 1); b.f(8); b.zeros(2); b.b(1);
    primitive(b, 11, 0, 2); b.f(8); b.zeros(2); b.f(-1.25f);
    primitive(b, 88, 0, 3); b.f(8); b.zeros(2);
    b.b(255); b.w(0xfffe); b.b(3); b.w('A'); b.w(0x03a9); b.w(0x0436);
    primitive(b, 1110, 0, 4); const char* source = "def event i,v; output 0,v; end";
    b.str(source); b.zeros(16); b.b(0); b.d(1); b.d(1);
    b.w(2); b.str("value"); b.w(2); b.str("result"); b.d(0); b.d(0);
    b.w(0); endModule(b); b.w(0); b.b(0);
    Session session; check(SchematicFile::importFlowStone(session, zip(b), {}), "Import Green values and Ruby");
    const auto& nodes = session.graph().nodes();
    check(std::get<std::int32_t>(*nodes[0].localValue) == -123, "Preserve signed integer");
    check(std::get<bool>(*nodes[1].localValue), "Preserve Boolean");
    check(std::bit_cast<quint32>(std::get<float>(*nodes[2].localValue)) == 0xbfa00000, "Preserve Float value bits");
    check(std::get<std::string>(*nodes[3].localValue) == "AΩж", "Decode Unicode CString");
    check(nodes[4].rubyCode->source == source && nodes[4].rubyCode->ports.inputs[0].name == "value", "Preserve Ruby source and named ports");
    const auto saved = SchematicFile::project(session); Session reopened;
    check(SchematicFile::loadProject(reopened, saved, {}), "Reopen values and Ruby");
}

void codePorts() {
    for (int kind : {154, 22}) {
        Bytes b; header(b, 166); beginModule(b, "Root", 1);
        primitive(b, kind, 2, 2);
        b.f(20); b.f(10); b.b(1); b.b(1);
        const auto code = kind == 154 ? "streamin input;streamout output;output=input;" : "streamin input;streamout output;movaps xmm0,input;movaps output,xmm0;";
        b.str(code); b.d(0); b.b(0); b.w(0); endModule(b); b.w(0); b.b(0);
        Session session; check(SchematicFile::importFlowStone(session, zip(b), {}), "Import unchanged code source");
        check(session.graph().nodes().front().dspCode->source == code, "Preserve source exactly");
    }
}
void codeBox(Bytes& b, int kind, const QByteArray& source, int ins = 0, int outs = 1, const QByteArray& caches = {}) {
    primitive(b, kind, 2, 2); b.f(20); b.f(10); b.b(ins); b.b(outs);
    b.str(source); b.d(0); b.b(caches.isEmpty() ? 0 : 2);
    b.append(caches); b.append(caches); // Both architecture caches, followed by real records.
}
void stringBox(Bytes& b, const QByteArray& source) {
    primitive(b, 88, 1, 1); b.f(8); b.zeros(2); b.str(source);
}
QByteArray linkedFixture(int kind, const QByteArray& source) {
    Bytes b; header(b, 166); beginModule(b, "Root", 2);
    beginModule(b, "Source producer", 2);
    codeBox(b, kind, source);
    primitive(b, 139, 5, 0); b.d(1); b.b(2); b.str("Code");
    b.w(1); wire(b, 166, 0, 0, 1, 0); endModule(b);
    stringBox(b, "stale text");
    b.w(1); wire(b, 166, 0, 0, 1, 0);
    endModule(b); b.w(0); b.b(0); return b;
}
void checkSignal(Schematic& graph, int id, double expected) {
    for (std::size_t block : {1, 7, 64, 257}) {
        const auto samples = graph.captureDsp(id, 521, block);
        check(!samples.empty(), "Capture linked code");
        for (double v : samples[0]) check(v == expected, "Linked source compiles to expected signal");
    }
}
void linkedCodeSources() {
    for (int kind : {154, 22}) {
        const auto code = kind == 154 ? "streamout output;output=0.25;" : "streamout output;float value=0.25;movaps xmm0,value;movaps output,xmm0;";
        Session session; check(SchematicFile::importFlowStone(session, zip(linkedFixture(kind, code)), {}), "Import linked code-source outputs");
        auto& graph = session.graph();
        check(graph.module(1)->nodes()[0].dspCode->source == code, "Preserve linked code text exactly");
        check(graph.links().size() == 1 && graph.module(1)->links().size() == 1, "Keep source links across boundaries");
        check(graph.module(1)->nodes()[0].outputPorts().back().type == green::Type::String, "Expose source-string output");
        check(graph.module(1)->nodes()[0].inputPorts().empty(), "DSP inputs have no implicit source-string input");
        check(std::get<std::string>(graph.readValue(2)) == code, "Pull linked source into String box");
        checkSignal(*graph.module(1), 1, 0.25);
        const auto next = kind == 154 ? "streamout renamed,second;renamed=0.5;second=0.75;" : "streamout renamed,second;float value=0.5;movaps xmm0,value;movaps renamed,xmm0;movaps second,xmm0;";
        session.enterModule(1); session.graph().setDspDraft(1, next); session.compileDsp(1); session.navigate(0);
        checkSignal(*graph.module(1), 1, 0.5);
        check(graph.module(1)->links()[0].output == 2, "Keep source output when DSP port counts change");
        check(std::get<std::string>(graph.readValue(2)) == next, "Read current code through source link");
        QTemporaryDir dir; check(dir.isValid(), "Temporary project directory");
        const auto path = dir.filePath("converted.ostone");
        const auto saved = SchematicFile::project(session); SchematicFile::write(path, saved);
        Session reopened; check(SchematicFile::loadProject(reopened, SchematicFile::read(path), {}), "Reopen converted linked code from disk");
        check(SchematicFile::project(reopened) == saved, "Persist source ports, code and links exactly");
        checkSignal(*reopened.graph().module(1), 1, 0.5);
        reopened.enterModule(1); reopened.graph().setDspDraft(1, code); reopened.compileDsp(1); reopened.navigate(0);
        check(std::get<std::string>(reopened.graph().readValue(2)) == code, "Source links remain editable after reopening");
    }
    Bytes b; header(b, 166); beginModule(b, "Root", 2);
    const char* code = "def event i,v; output 0,v*2; end";
    stringBox(b, code);
    primitive(b, 1110, 1, 4); b.str("old Ruby source"); b.zeros(16); b.b(0); b.d(1); b.d(1);
    b.w(2); b.str("value"); b.w(2); b.str("result"); b.d(0); b.d(0);
    b.w(1); wire(b, 166, 0, 0, 1, 0); endModule(b); b.w(0); b.b(0);
    Session session; check(SchematicFile::importFlowStone(session, zip(b), {}), "Import linked Ruby source without executing it");
    check(session.graph().nodes()[1].rubyCode->source == code, "Bind Ruby source independently of data port zero");
    session.graph().setValue(1, std::string("def event i,v; output 0,v*3; end"));
    check(session.graph().nodes()[1].rubyCode->source.find("v*3") != std::string::npos, "Update stopped Ruby source");
    session.graph().editRubyPort(2, false, 0, RubyPortEdit::Count, 3);
    check(session.graph().links()[0].input == 3, "Keep Ruby source input when data ports change");
    const auto text = session.graph().addNode(PrimitiveKind::String, 0, 0);
    session.graph().connect({2, text, 0, 1});
    session.graph().editRubyPort(2, true, 0, RubyPortEdit::Count, 2);
    check(session.graph().links()[1].output == 2, "Keep Ruby source output when data ports change");
    check(std::get<std::string>(session.graph().readValue(text)).find("v*3") != std::string::npos, "Read Ruby source after remapping");
    const auto saved = SchematicFile::project(session); Session reopened;
    check(SchematicFile::loadProject(reopened, saved, {}) && SchematicFile::project(reopened) == saved, "Reopen linked Ruby source");
    reopened.graph().setValue(1, std::string(code));
    check(std::get<std::string>(reopened.graph().readValue(text)) == code, "Update linked Ruby source after reopening");
    // A Ruby source self loop cannot be resolved from stored text.
    Bytes c; header(c, 166); beginModule(c, "Root", 1);
    primitive(c, 1110, 1, 4); c.str("def event i,v; end"); c.zeros(16); c.b(0); c.d(0); c.d(0); c.d(0); c.d(0);
    c.w(1); wire(c, 166, 0, 0, 0, 0); endModule(c); c.w(0); c.b(0);
    expectError(zip(c), "Cyclic code-source");
}
Bytes compiledCache() {
    Bytes c; c.b(2); c.b(1); c.b(39); c.str("in"); c.b(1); c.b(39); c.str("out");
    c.w(17);
    const auto refs = [&] { c.w(1); c.d(4); c.b(2); c.d(8); };
    for (int kind : {1, 3, 4, 7, 8}) {
        c.b(kind); c.str("variable");
        if (kind == 1 || kind == 3 || kind == 4) { c.zeros(kind == 4 ? 32 : 16); c.d(0); }
        else { c.d(44100); c.d(0); }
        refs();
    }
    for (int kind : {2, 5, 6}) for (int mode = 0; mode < 4; ++mode) {
        c.b(kind); c.str("array"); c.d(44100); c.d(0); refs(); c.b(mode);
        if (mode == 1) c.zeros(kind == 5 ? 32 : 16);
        if (mode == 2) c.zeros(kind == 5 ? 16 : 8);
        if (mode == 3) c.d(0);
    }
    for (int i = 0; i < 4; ++i) { c.d(3); c.append(QByteArray::fromHex("cc0f0b")); }
    c.d(1); c.str("engine-symbol"); c.zeros(10); return c;
}
void compiledCaches() {
    const auto cache = compiledCache();
    const auto fixture = [&](const QByteArray& c) {
        Bytes b; header(b, 170); beginModule(b, "Root", 2);
        codeBox(b, 154, "streamout output;output=0.25;", 0, 1, c);
        primitive(b, 29, 0, 0); b.f(8); b.zeros(2); b.d(42);
        b.w(0); endModule(b); b.w(0); b.b(0); return b;
    };
    Session session; QStringList warnings;
    check(SchematicFile::importFlowStone(session, zip(fixture(cache)), {}, &warnings), "Skip both complete compiled caches");
    check(warnings.join(' ').contains("caches were skipped"), "Report ignored vendor caches");
    check(std::get<std::int32_t>(*session.graph().nodes()[1].localValue) == 42, "Resume at the next primitive after the caches");
    checkSignal(session.graph(), 1, 0.25);
    const auto saved = SchematicFile::project(session);
    check(!saved.contains("engine-symbol"), "Never serialize native cache records into OpenStone projects");
    Session reopened; check(SchematicFile::loadProject(reopened, saved, {}), "Reopen source rebuilt after cache skip");
    checkSignal(reopened.graph(), 1, 0.25);
    auto bad = cache;
    // Locate the signed variable count after cache port declarations.
    const auto variables = cache.indexOf(QByteArray::fromHex("110001087661726961626c65"));
    check(variables >= 0, "Locate cache variable count");
    bad = cache; bad[variables] = char(0xff); bad[variables + 1] = char(0xff);
    expectError(zip(fixture(bad)), "compiled-cache variable count");
    bad = cache; bad[variables + 2] = char(9); expectError(zip(fixture(bad)), "variable kind");
    bad = cache; const auto code = cache.indexOf(QByteArray::fromHex("03000000cc0f0b"));
    check(code >= 0, "Locate cache code section"); bad[code + 3] = char(0xff);
    expectError(zip(fixture(bad)), "record count");
    expectError(zip(fixture(cache.left(20))), "compiled-cache");
    auto invalidSource = fixture(cache);
    const auto sourceAt = invalidSource.indexOf("output=0.25;");
    check(sourceAt >= 0, "Locate cached block source"); invalidSource.replace(sourceAt, 6, "badbad");
    expectError(zip(invalidSource), "Cannot compile source");
}

// Authored archive layouts, independent of OpenStone's JSON writer.
void connectorLayout(Bytes& b, int version) {
    b.d(2);
    for (int i = 0; i < 2; ++i) {
        for (float x : {1.f, 2.f, 3.f, 4.f, 5.f, 6.f}) b.f(x);
        if (version >= 157) { b.b(1); b.b(0); b.b(1); b.b(0); b.b(i); if (i) b.zeros(16); }
    }
    b.d(2); b.w(1); b.w(0);
}
void valueLayouts() {
    for (int version : {154, 156, 157, 169, 177}) {
        Bytes b; header(b, version); beginModule(b, "Root", 5);
        for (int tag : {29, 11, 406, 88}) {
            primitive(b, tag, 0, 0); b.f(8); b.b(1); b.b(1); connectorLayout(b, version);
            if (tag == 29) b.d(std::bit_cast<quint32>(qint32(-123)));
            else if (tag == 11) b.f(40);
            else if (tag == 406) b.b(1);
            else b.str("Text after custom layout");
        }
        primitive(b, 236, 0, 0, "Multiline");
        if (version >= 177) { b.b(0); b.b(1); connectorLayout(b, version); }
        b.f(20); b.f(12);
        b.b(255); b.w(0xfffe); b.b(5); b.w('A'); b.w('\n'); b.w(0x03a9); b.w('\n'); b.w(0x0436);
        b.d(2); b.d(1); b.d(2); b.d(3); b.d(4);
        b.w(0); endModule(b); b.w(0); b.b(0);
        Session session; QStringList warnings;
        check(SchematicFile::importFlowStone(session, zip(b), {}, &warnings), "Import custom value layouts and multiline strings");
        check(warnings.join(' ').contains("connector layout"), "Report skipped visual layout");
        const auto& nodes = session.graph().nodes();
        check(std::get<std::int32_t>(*nodes[0].localValue) == -123, "Int survives custom layout");
        check(std::get<float>(*nodes[1].localValue) == 40, "Float follows custom layout exactly");
        check(std::get<bool>(*nodes[2].localValue), "Boolean survives custom layout");
        check(std::get<std::string>(*nodes[3].localValue) == "Text after custom layout", "String survives custom layout");
        check(std::get<std::string>(*nodes[4].localValue) == "A\nΩ\nж", "Multiline text, Unicode and bookmarks are consumed in order");
        const auto saved = SchematicFile::project(session); Session reopened;
        check(SchematicFile::loadProject(reopened, saved, {}) && SchematicFile::project(reopened) == saved, "Reopen values after discarding native layout");
        QByteArray bad = b; auto at = bad.indexOf(QByteArray::fromHex("020000000000803f00000040"));
        check(at >= 0, "Locate custom layout");
        bad[at + 3] = char(0x80); expectError(zip(bad), "record count");
        bad = b.left(at + 15); expectError(zip(bad), "Truncated");
        if (version >= 157) {
            bad = b; bad[at + 4 + 24] = char(2); expectError(zip(bad), "Boolean");
        }
    }
}
void moduleLayout(Bytes& b, int version, bool clip, bool area) {
    b.d(0); b.d(0); if (version < 155) { b.d(15); b.d(6); } else { b.f(15); b.f(6); }
    if (version >= 155) { b.b(clip ? 0x10 : 0); if (clip) b.zeros(16); }
    b.b(area ? 0x11 : 0x0f); if (area) b.zeros(16); b.b(1);
}
void propertyPanelLayouts() {
    for (int version : {154, 155, 169, 177}) for (bool pins : {false, true}) {
        Bytes b; header(b, version); beginModule(b, "Root", 1);
        primitive(b, 29, 0, 0); b.f(8); b.zeros(2); b.d(42);
        b.w(0); b.d(6); b.d(4); b.b(0); b.b(0);
        b.b(pins);
        if (pins) {
            moduleLayout(b, version, true, false); b.w(0);
            b.d(7); b.b(1); b.d(1); b.d(2); b.d(0); b.d(1);
        }
        b.b(1); moduleLayout(b, version, false, true);
        b.d(7); b.b(1); b.d(0); b.b(0); b.b(0); b.w(0); b.b(0);
        Session session; QStringList warnings;
        check(SchematicFile::importFlowStone(session, zip(b), {}, &warnings), "Import optional module property helper layouts");
        check(warnings.join(' ').contains("root drawing surface"), "Report omitted property panel styling");
        check(std::get<std::int32_t>(*session.graph().module(1)->nodes()[0].localValue) == 42, "Property helper data never replaces graph values");
        const auto saved = SchematicFile::project(session); Session reopened;
        check(SchematicFile::loadProject(reopened, saved, {}), "Reopen after property helper layout");
        auto bad = b; bad.chop(8); expectError(zip(bad), "Truncated");
        const auto helper = b.indexOf(QByteArray::fromHex("0700000001"));
        check(helper >= 0, "Locate module helper type");
        bad = b; bad[helper] = 6; expectError(zip(bad), "helper type");
        bad = b; bad[helper + 8] = char(0x80); expectError(zip(bad), "record count");
    }
}
void mathRecord(Bytes& b, int inputs, quint32 type, bool locked = false) {
    b.d(inputs); for (int i = 0; i < inputs; ++i) b.d(type);
    b.d(1); b.d(type);
    b.d(locked ? 1 : 0); if (locked) b.d(inputs - 1);
    b.d(locked ? 1 : 0); if (locked) b.d(0);
}
void genericMathRecords() {
    struct Math { int tag; double left, right, expected; bool unary = false; };
    for (int version : {169, 170, 177}) for (bool doubles : {false, true})
        for (const Math m : {Math{1042, 4, 2, 6}, Math{1043, 4, 2, 2}, Math{1044, 4, 2, 8},
             Math{1045, 4, 2, 2}, Math{1046, 4, 2, 4}, Math{1047, 4, 2, 2},
             Math{99, 4, 3, 64}, Math{675, 4, 3, 64}, Math{240, 100, 0, 2, true}}) {
            // Old Power/Log10 records default to Green Float and have no connector payload.
            if (version < 170 && (m.tag == 99 || m.tag == 675 || m.tag == 240)) continue;
            const std::string prefix = doubles ? "monodouble" : "mono";
            Bytes b; header(b, version); beginModule(b, "Root", m.unary ? 3 : 4);
            codeBox(b, 154, QByteArray::fromStdString(prefix + "out a;a=" + std::to_string(m.left) + ";"));
            if (!m.unary) codeBox(b, 154, QByteArray::fromStdString(prefix + "out b;b=" + std::to_string(m.right) + ";"));
            primitive(b, m.tag, 4, 0); mathRecord(b, m.unary ? 1 : 2, doubles ? 45 : 39, true);
            codeBox(b, 154, QByteArray::fromStdString(prefix + "in x;" + prefix + "out y;y=x;"), 1);
            b.w(m.unary ? 2 : 3);
            const int math = m.unary ? 1 : 2, probe = math + 1;
            wire(b, version, 0, 1, math, 0);
            if (!m.unary) wire(b, version, 1, 1, math, 1);
            wire(b, version, math, 0, probe, 0); endModule(b); b.w(0); b.b(0);
            Session session; check(SchematicFile::importFlowStone(session, zip(b), {}), "Import generic stream math records");
            for (std::size_t block : {1, 7, 64, 257}) {
                const auto samples = session.graph().captureDsp(probe + 1, 521, block);
                for (double x : samples.at(0)) check(x == m.expected, "Imported generic math preserves signal semantics");
            }
            const auto saved = SchematicFile::project(session); Session reopened;
            check(SchematicFile::loadProject(reopened, saved, {}) && SchematicFile::project(reopened) == saved, "Reopen typed generic stream math");
        }
    for (int version : {159, 169, 170}) {
        Bytes b; header(b, version); beginModule(b, "Root", 3);
        for (float x : {4.f, 3.f}) { primitive(b, 11, 0, 0); b.f(8); b.zeros(2); b.f(x); }
        primitive(b, 99, 0, 0); if (version >= 170) mathRecord(b, 2, 2, true);
        b.w(2); wire(b, version, 0, 0, 2, 0); wire(b, version, 1, 0, 2, 1); endModule(b); b.w(0); b.b(0);
        Session session; check(SchematicFile::importFlowStone(session, zip(b), {}), "Import versioned Green Power record");
        check(std::get<float>(session.graph().readValue(3)) == 64, "Imported Green math evaluates correctly");
    }

    {
        Bytes b; header(b, 170); beginModule(b, "Root", 3);
        for (int x : {12, 7}) { primitive(b, 29, 0, 0); b.f(8); b.zeros(2); b.d(x); }
        primitive(b, 1042, 0, 0); mathRecord(b, 2, 3, true);
        b.w(2); wire(b, 170, 0, 0, 2, 0); wire(b, 170, 1, 0, 2, 1); endModule(b); b.w(0); b.b(0);
        Session session; check(SchematicFile::importFlowStone(session, zip(b), {}), "Import native Int value and generic connector IDs");
        check(session.graph().inputType(3, 0) == green::Type::Int && std::get<std::int32_t>(session.graph().readValue(3)) == 19,
              "Native type 3 and component 29 are Integer");
    }
    {
        Bytes b; header(b, 170); beginModule(b, "Root", 1); beginModule(b, "Control", 3);
        primitive(b, 11, 0, 0); b.f(8); b.zeros(2); b.f(20);
        primitive(b, 429, 0, 0); mathRecord(b, 1, 2, true);
        primitive(b, 139, 10, 0); b.d(2); b.b(2); b.str("Gain");
        b.w(2); wire(b, 170, 0, 0, 1, 0); wire(b, 170, 1, 0, 2, 0); endModule(b);
        b.w(0); endModule(b); b.w(0); b.b(0);
        Session session; check(SchematicFile::importFlowStone(session, zip(b), {}), "Import native Float values, math and module boundary");
        check(session.graph().outputType(1) == green::Type::Float && std::get<float>(session.graph().readValue(1)) == 10,
              "Native type 2 and component 11 preserve Float values through typed module outputs");
    }
    const auto invalid = [&](const Bytes& record) {
        Bytes b; header(b, 170); beginModule(b, "Root", 1); primitive(b, 1044, 0, 0); b.append(record);
        b.w(0); endModule(b); b.w(0); b.b(0); return zip(b);
    };
    Bytes bad; bad.d(1000000); expectError(invalid(bad), "record count");
    bad.clear(); bad.d(1); expectError(invalid(bad), "connector count");
    bad.clear(); mathRecord(bad, 2, 1); expectError(invalid(bad), "connector type");
    bad.clear(); mathRecord(bad, 2, 39, true); bad[bad.size() - 1] = char(0x80); expectError(invalid(bad), "locked connector");
    bad.clear(); mathRecord(bad, 2, 39); bad[8] = 45; expectError(invalid(bad), "Mixed generic");
    bad.clear(); bad.d(2); bad.d(999); expectError(invalid(bad), "connector type");
}

void floatDeZipperRecord() {
    Bytes b; header(b, 169); beginModule(b, "Root", 3);
    primitive(b, 11, 0, 0); b.f(8); b.zeros(2); b.f(.25f);
    primitive(b, 594, 4, 0);
    codeBox(b, 154, "monoin x;monoout y;y=x;", 1);
    b.w(2); wire(b, 169, 0, 0, 1, 0); wire(b, 169, 1, 0, 2, 0); endModule(b); b.w(0); b.b(0);
    Session session; check(SchematicFile::importFlowStone(session, zip(b), {}), "Import Float De-Zipper's empty native state");
    checkSignal(session.graph(), 3, .25);
    const auto saved = SchematicFile::project(session); Session reopened;
    check(SchematicFile::loadProject(reopened, saved, {}), "Reopen imported Float De-Zipper");
    checkSignal(reopened.graph(), 3, .25);
}

void nativePanel(Bytes& b, int tag, int version, float x = 2, float y = 3) {
    primitive(b, tag, 2, 2);
    b.f(x); b.f(y);
    if (version < 155) { b.d(15); b.d(6); } else { b.f(15); b.f(6); b.b(0); }
    b.b(15); b.b(1);
}
void panelFooter(Bytes& b, int gui) {
    b.d(6); b.d(4); b.b(0); b.b(gui); b.b(0); b.b(0); b.b(0);
    if (gui == 1 || gui == 3) { b.b(0); b.b(0); }
    b.b(1);
}
void importedPanels() {
    for (int tag : {16, 400, 1191, 1192}) for (int version : {154, 155, 170, 177}) {
        Bytes b; header(b, version); beginModule(b, "Root", 1); beginModule(b, "Panel", 2);
        nativePanel(b, tag, version);
        primitive(b, 420, 4, 2);
        b.w(1); wire(b, version, 0, 0, 1, 0);
        panelFooter(b, tag == 400 || tag == 1192 ? 1 : 254);
        b.w(0); endModule(b); b.w(0); b.b(0);
        Session session; check(SchematicFile::importFlowStone(session, zip(b), {}), "Import current and legacy MGUI variants");
        const auto verify = [&](Schematic& root) {
            const auto* child = root.module(1);
            const auto panel = child->modulePanel(tag == 400 || tag == 1192 ? ModulePanelKind::Properties : ModulePanelKind::Front);
            check(panel && panel->position == green::Point{2, 3} && panel->view.bounds == yellow::Area{0, 0, 15, 6},
                  "Preserve native panel position, dimensions and surface kind");
            check(std::get<yellow::Area>(child->readValue(2)) == panel->view.bounds, "View Area remains connected to the imported drawing surface");
        };
        verify(session.graph());
        const auto saved = SchematicFile::project(session); Session reopened;
        check(SchematicFile::loadProject(reopened, saved, {}) && SchematicFile::project(reopened) == saved, "Persist GUI nodes and translated defaults");
        verify(reopened.graph());
        auto bad = b; const auto pattern = QByteArray::fromHex("0000004000004040"); const auto pos = b.indexOf(pattern);
        check(pos >= 0, "Locate panel coordinates");
        bad[pos + 2] = char(0xc0); bad[pos + 3] = char(0x7f); expectError(zip(bad), "panel coordinate");
    }
    // Width and Height are actual View Size routes, so they follow later resizing.
    Bytes b; header(b, 170); beginModule(b, "Root", 1); beginModule(b, "Legacy", 3);
    nativePanel(b, 16, 170); primitive(b, 11, 5, 0); b.f(8); b.zeros(2); b.f(0);
    primitive(b, 11, 5, 2); b.f(8); b.zeros(2); b.f(0);
    b.w(2); wire(b, 170, 0, 1, 1, 0); wire(b, 170, 0, 2, 2, 0); panelFooter(b, 254);
    b.w(0); endModule(b); b.w(0); b.b(0);
    Session session; check(SchematicFile::importFlowStone(session, zip(b), {}), "Translate legacy MGUI size outputs");
    check(std::get<float>(session.graph().module(1)->readValue(2)) == 15 && std::get<float>(session.graph().module(1)->readValue(3)) == 6,
          "Legacy Width/Height preserve their output indices");
    session.graph().resizePanel(1, 22, 9);
    check(std::get<float>(session.graph().module(1)->readValue(2)) == 22 && std::get<float>(session.graph().module(1)->readValue(3)) == 9,
          "Translated legacy size routes remain live");
}
void legacyPanelControls() {
    Bytes b; header(b, 170); beginModule(b, "Root", 1); beginModule(b, "Controls", 3);
    nativePanel(b, 16, 170);
    for (int i = 0; i < 2; ++i) { primitive(b, 406, 0, i); b.f(8); b.zeros(2); b.b(1); }
    b.w(2); wire(b, 170, 1, 0, 0, 3); wire(b, 170, 2, 0, 0, 4); panelFooter(b, 254);
    b.w(0); endModule(b); b.w(0); b.b(0);
    Session session; check(SchematicFile::importFlowStone(session, zip(b), {}), "Import legacy borderless and mouse controls");
    const auto panel = session.graph().module(1)->frontPanel();
    check(panel && !panel->borderVisible && panel->view.mouseMoves, "Invert legacy Borderless while retaining Mouse control polarity");
    const auto saved = SchematicFile::project(session); Session reopened;
    check(SchematicFile::loadProject(reopened, saved, {}) && !reopened.graph().module(1)->frontPanel()->borderVisible, "Preserve translated panel control links");
}

void inheritedViews() {
    Bytes b; header(b, 170); beginModule(b, "Root", 1); beginModule(b, "Parent", 2);
    nativePanel(b, 1191, 170);
    beginModule(b, "Child", 2); nativePanel(b, 1191, 170);
    primitive(b, 402, 0, 0); b.b(7); b.b(0); b.w(0);
    b.w(1); wire(b, 170, 1, 0, 0, 0); panelFooter(b, 254);
    b.w(0); panelFooter(b, 254); b.w(0); endModule(b); b.w(0); b.b(0);
    Session session; check(SchematicFile::importFlowStone(session, zip(b), {}), "Bind an inherited native wireless View");
    const auto verify = [&](Schematic& root) {
        const auto* parent = root.module(1); const auto* child = parent->module(2);
        check(child->frontPanel()->view.attachesTo(parent->frontPanel()->view), "Imported child panel attaches to its enclosing View");
        check(parent->nodes()[1].moduleInputs.size() == 1 && child->nodes()[1].kind == PrimitiveKind::ModuleInput,
              "Represent the implicit View route as a persistent module boundary");
    };
    verify(session.graph()); const auto saved = SchematicFile::project(session); Session reopened;
    check(SchematicFile::loadProject(reopened, saved, {}), "Reopen inherited panel routing"); verify(reopened.graph());
}
void propertyWireless() {
    for (int tag : {401, 419}) {
        Bytes b; header(b, 170); beginModule(b, "Root", tag == 419 ? 2 : 3);
        if (tag == 419) beginModule(b, "Settings", 2);
        primitive(b, 11, 0, 0); b.f(8); b.zeros(2); b.f(.75f);
        primitive(b, tag, 4, 0, "Gain"); b.d(2); b.b(0); b.d(1); b.d(2); b.d(1); b.d(2);
        if (tag == 419) { b.w(1); wire(b, 170, 0, 0, 1, 0); endModule(b); }
        beginModule(b, "Display", 2);
        primitive(b, 402, 0, 0, "Gain"); b.b(2); b.b(0); b.w(0xffff);
        primitive(b, 139, 7, 0); b.d(2); b.b(0); b.str("Value");
        b.w(1); wire(b, 170, 0, 0, 1, 0); endModule(b);
        b.w(tag == 419 ? 0 : 1); if (tag != 419) wire(b, 170, 0, 0, 1, 0);
        endModule(b); b.w(0); b.b(0);
        Session session; check(SchematicFile::importFlowStone(session, zip(b), {}), "Import scoped native property publishers and receivers");
        const auto verify = [&](Session& s) {
            auto& root = s.graph(); const auto display = tag == 419 ? 2 : 3;
            check(std::get<float>(root.readValue(display)) == .75f, "Wireless property value crosses the expected module scope");
        };
        verify(session); const auto saved = SchematicFile::project(session); Session reopened;
        check(SchematicFile::loadProject(reopened, saved, {}), "Reopen property routes after ignoring native route caches"); verify(reopened);
        if (tag == 419) reopened.enterModule(1);
        reopened.graph().setValue(1, .25f); if (tag == 419) reopened.navigate(0);
        check(std::get<float>(reopened.graph().readValue(tag == 419 ? 2 : 3)) == .25f, "Property routes keep propagating changes after reopening");
    }
}
void guiDrawing() {
    Bytes b; header(b, 170); beginModule(b, "Root", 1); beginModule(b, "Text panel", 6);
    nativePanel(b, 1191, 170);
    primitive(b, 403, 0, 0); b.b(0); b.b(0); b.b(128); b.b(17); b.b(34); b.b(51);
    primitive(b, 96, 6, 0); stringBox(b, "Imported text");
    primitive(b, 138, 0, 6); b.d(8); b.b(0); b.str("Bounds");
    primitive(b, 402, 0, 8, "Accent"); b.b(13); b.b(0); b.w(0xffff);
    b.w(4); wire(b, 170, 0, 0, 2, 0); wire(b, 170, 1, 0, 2, 2);
    wire(b, 170, 3, 0, 2, 3); wire(b, 170, 4, 0, 2, 1); panelFooter(b, 254);
    b.w(0); endModule(b); b.w(0); b.b(0);
    Session session; check(SchematicFile::importFlowStone(session, zip(b), {}), "Import GUI Color, Text, Area and typed property nodes");
    auto& root = session.graph();
    const auto area = root.addNode(PrimitiveKind::Area, 0, 0);
    const auto width = root.addNode(PrimitiveKind::Float, 0, 0, 8.f);
    root.connect({width, area, 2}); root.connect({width, area, 3}); root.connect({area, 1, 0});
    const auto verify = [&](Schematic& g) {
        const auto* child = g.module(1);
        check(std::get<yellow::Color>(*child->nodes()[1].localValue) == yellow::Color{0x80112233}, "Preserve native ARGB channel order");
        const auto commands = g.panelDrawings(child->frontPanel()->view);
        check(commands.size() == 1 && std::get<yellow::TextCommand>(commands[0].command).text == "Imported text",
              "Imported GUI produces a connected text drawing command");
    };
    verify(root); const auto saved = SchematicFile::project(session); Session reopened;
    check(SchematicFile::loadProject(reopened, saved, {}), "Reopen drawing primitives"); verify(reopened.graph());
}

void guiSelectorAndRoutedLinks() {
    Bytes b; header(b, 170); beginModule(b, "Root", 5);
    stringBox(b, "First"); stringBox(b, "Second");
    primitive(b, 29, 0, 0); b.f(8); b.zeros(2); b.d(1);
    primitive(b, 137, 0, 0); b.d(2); b.d(1);
    primitive(b, 29, 0, 0); b.f(8); b.zeros(2); b.d(0);
    b.w(4);
    // One native link contains two short-coordinate routing points.
    b.b(2); b.w(10); b.w(11); b.w(12); b.w(13);
    b.w(0); b.b(0); b.w(3); b.b(1); b.zeros(4);
    wire(b, 170, 1, 0, 3, 2); wire(b, 170, 2, 0, 3, 0); wire(b, 170, 4, 0, 3, 0); endModule(b); b.w(0); b.b(0);
    Session session; QStringList warnings;
    check(SchematicFile::importFlowStone(session, zip(b), {}, &warnings), "Preserve native Selector ordering and routed link endpoints");
    check(warnings.join(' ').contains("routing points"), "Report replaced link geometry");
    check(std::get<std::string>(session.graph().readValue(4)) == "Second", "Native Selector Index stays first and chooses the second data input");
    session.graph().setValue(5, std::int32_t{-2});
    check(std::get<std::string>(session.graph().readValue(4)) == "First", "Native Selector clamps the combined negative Index to its first value");
    session.graph().setValue(5, std::int32_t{99});
    check(std::get<std::string>(session.graph().readValue(4)) == "Second", "Native Selector clamps the combined high Index to its last value, excluding the spare");
    session.graph().setValue(5, std::int32_t{0}); session.graph().setValue(3, std::int32_t{0});
    check(std::get<std::string>(session.graph().readValue(4)) == "First", "Imported property Selector responds to index changes");
    const auto saved = SchematicFile::project(session); Session reopened;
    check(SchematicFile::loadProject(reopened, saved, {}), "Reopen Selector and routed link conversion");
    check(std::get<std::string>(reopened.graph().readValue(4)) == "First", "Preserve selected route after reopening");
    check(reopened.graph().nodes()[3].inputPorts().size() == 4, "Selector retains Index, two values and its final spare input");
    for (int count : {0, 1}) {
        Bytes small; header(small, 170); beginModule(small, "Root", 2);
        stringBox(small, "Only"); primitive(small, 137, 0, 0); small.d(count); small.d(1);
        small.w(count); if (count) wire(small, 170, 0, 0, 1, 1);
        endModule(small); small.w(0); small.b(0);
        Session single; SchematicFile::importFlowStone(single, zip(small), {});
        check(single.graph().nodes()[1].inputPorts().size() == std::size_t(count + 2) &&
              std::get<std::string>(single.graph().readValue(2)) == (count ? "Only" : ""),
              "Empty and single-value native Selectors retain their spare connector");
    }
}


QByteArray drawingProperties(int version, float width = .125f, quint32 dash = 3, float size = 1.25f, quint32 style = 15, quint32 kind = 1) {
    Bytes b; header(b, version); beginModule(b, "Root", 2);
    primitive(b, 214, 2, 3, "Stroke");
    if (version >= 159) {
        b.zeros(2); if (version >= 167) b.d(kind);
        b.f(width); b.d(0x50a1b2c3); b.d(dash);
    }
    primitive(b, 215, 4, 5, "Typeface");
    if (version >= 159) {
        b.zeros(2); b.str("Test Face"); b.f(size); b.d(style);
        if (version >= 171) { b.d(3); b.str("Arial"); b.str("Helvetica"); b.str("Sans Serif"); }
    }
    b.w(0); endModule(b); b.w(0); b.b(0); return b;
}
void penAndFontProperties() {
    using enum PrimitiveKind;
    for (const int version : {154, 158, 159, 166, 167, 170, 171, 177}) {
        Session s; check(SchematicFile::importFlowStone(s, zip(drawingProperties(version)), {}), "Import versioned Pen and Font properties");
        const auto expectedPen = version < 159 ? yellow::PenStroke{} : yellow::PenStroke{yellow::Color{0x50a1b2c3}, .125f, yellow::LineStyle::DashDot};
        yellow::Font expectedFont;
        if (version >= 159) { expectedFont.face = "Test Face"; expectedFont.size = 1.25f; expectedFont.style = {true, true, true, true}; }
        if (version >= 171) { expectedFont.alternateFace = "Arial"; expectedFont.fallbackFaces = {"Helvetica", "Sans Serif"}; }
        const auto verify = [&](Schematic& g) {
            check(std::get<yellow::PenStroke>(std::get<yellow::PaintTool>(g.readValue(1))) == expectedPen, "Preserve Pen color, width and dash style");
            check(std::get<yellow::Font>(g.readValue(2)) == expectedFont, "Preserve Font face, size, style and ordered fallback faces");
        };
        verify(s.graph());
        const auto width = s.graph().addNode(Float, 0, 0, .5f);
        const auto face = s.graph().addNode(String, 0, 0, std::string("Connected Face"));
        s.graph().connect({width, 1, 1}); s.graph().connect({face, 2, 0});
        check(std::get<yellow::PenStroke>(std::get<yellow::PaintTool>(s.graph().readValue(1))).width == .5f, "Connected Pen input overrides stored width");
        check(std::get<yellow::Font>(s.graph().readValue(2)).face == "Connected Face", "Connected Font input overrides stored face");
        const auto saved = SchematicFile::project(s); Session reopened;
        check(SchematicFile::loadProject(reopened, saved, {}) && SchematicFile::project(reopened) == saved, "Pen and Font properties and links survive conversion");
        reopened.graph().setValue(width, .75f); reopened.graph().setValue(face, std::string("Changed Face"));
        check(std::get<yellow::PenStroke>(std::get<yellow::PaintTool>(reopened.graph().readValue(1))).width == .75f, "Reopened Pen follows connected changes");
        check(std::get<yellow::Font>(reopened.graph().readValue(2)).face == "Changed Face", "Reopened Font follows connected changes");
        reopened.graph().disconnectInput(1, 1); reopened.graph().disconnectInput(2, 0); verify(reopened.graph());
    }
    // Native Paint Tool (14) and Font (17) values also cross module boundaries.
    {
        Bytes b; header(b, 177); beginModule(b, "Root", 1); beginModule(b, "Drawing values", 4);
        primitive(b, 214, 0, 0); b.zeros(2); b.d(1); b.f(.25f); b.d(0xff123456); b.d(2);
        primitive(b, 215, 0, 1); b.zeros(2); b.str("Arial"); b.f(1.5f); b.d(3); b.d(0);
        primitive(b, 139, 10, 0); b.d(14); b.b(0); b.str("Pen");
        primitive(b, 139, 10, 1); b.d(17); b.b(0); b.str("Font");
        b.w(2); wire(b, 177, 0, 0, 2, 0); wire(b, 177, 1, 0, 3, 0); endModule(b);
        b.w(0); endModule(b); b.w(0); b.b(0);
        Session s; SchematicFile::importFlowStone(s, zip(b), {});
        const auto saved = SchematicFile::project(s); Session reopened; SchematicFile::loadProject(reopened, saved, {});
        check(std::get<yellow::PenStroke>(std::get<yellow::PaintTool>(reopened.graph().readValue(1, 0))) ==
              yellow::PenStroke{yellow::Color{0xff123456}, .25f, yellow::LineStyle::Dot}, "Native Paint Tool boundary stays connected after reopening");
        const auto font = std::get<yellow::Font>(reopened.graph().readValue(1, 1));
        check(font.face == "Arial" && font.size == 1.5f && font.style.bold && font.style.italic, "Native Font boundary stays connected after reopening");
    }
    for (quint32 dash = 0; dash <= 4; ++dash) {
        Session s; SchematicFile::importFlowStone(s, zip(drawingProperties(177, 0, dash)), {});
        const auto pen = std::get<yellow::PenStroke>(std::get<yellow::PaintTool>(s.graph().readValue(1)));
        check(pen.width == 0 && int(pen.style) == dash, "All standard dash styles and hairline widths import");
    }
    expectError(zip(drawingProperties(177, -.1f)), "Pen width");
    expectError(zip(drawingProperties(177, .1f, 5)), "dash style");
    expectError(zip(drawingProperties(177, .1f, 0, 0)), "Font face or size");
    expectError(zip(drawingProperties(177, .1f, 0, 1, 16)), "Font style");
    expectError(zip(drawingProperties(177, .1f, 0, 1, 0, 2)), "paint-tool kind");
    expectError(zip(drawingProperties(177, std::numeric_limits<float>::infinity())), "Pen width");
    auto invalid = drawingProperties(177); const auto fallbacks = invalid.indexOf(QByteArray::fromHex("0300000005417269616c"));
    check(fallbacks > 0, "Find font fallbacks"); qToLittleEndian<quint32>(4097, invalid.data() + fallbacks);
    expectError(zip(invalid), "count");
    expectError(zip(drawingProperties(177).left(drawingProperties(177).size() - 25)), "Truncated");
    // Reject malformed converted defaults before replacing the open schematic.
    Session s; SchematicFile::importFlowStone(s, zip(drawingProperties(177)), {});
    const auto before = SchematicFile::project(s);
    // Value validation is also exercised by corrupting the serialized property payload directly.
    auto bad = before; bad.replace("Test Face", ""); bool rejected = false;
    try { SchematicFile::loadProject(s, bad, {}); } catch (const std::exception&) { rejected = true; }
    check(rejected && SchematicFile::project(s) == before, "Invalid saved Font defaults leave the open project unchanged");
}

void nativeGuiModule(const QString& path) {
    Session s; check(SchematicFile::importFlowStone(s, SchematicFile::read(path), {}), "Import complete unchanged bundled GUI submodule");
    s.enterModule(1); auto& graph = s.graph();
    // Eleven native nodes/fourteen links plus the two explicit Mouse/Border controls.
    check(graph.nodes().size() == 13 && graph.links().size() == 16, "Preserve native GUI graph and translated panel controls");
    check(graph.frontPanel().has_value(), "Native GUI submodule retains its panel");
    const auto stroke = [&] { return std::get<yellow::PenStroke>(std::get<yellow::PaintTool>(graph.readValue(7))); };
    check(stroke() == yellow::PenStroke{yellow::Color{0x50ffffff}, .125f, yellow::LineStyle::Solid}, "Native Color and Float links drive the imported Pen");
    graph.setValue(9, .25f); check(stroke().width == .25f, "Native linked Pen width follows edits"); s.navigate(0);
    QTemporaryDir dir; check(dir.isValid(), "Native GUI module round-trip directory");
    const auto saved = SchematicFile::project(s); const auto target = dir.filePath("separator.ostone"); SchematicFile::write(target, saved);
    Session reopened; check(SchematicFile::loadProject(reopened, SchematicFile::read(target), {}), "Reopen complete converted GUI submodule");
    check(SchematicFile::project(reopened) == saved, "Complete converted GUI schematic round-trips unchanged");
    const auto* copy = reopened.graph().module(1);
    check(copy->links().size() == 16 && std::get<yellow::PenStroke>(std::get<yellow::PaintTool>(copy->readValue(7))).width == .25f,
          "Native property links and edited values work after reopening");
    std::cout << "LED separator: complete native GUI module, property links and save/reopen passed\n";
}

void nativeDrawingRecord(const QString& path) {
    using enum PrimitiveKind;
    Session s; check(SchematicFile::importFlowStone(s, SchematicFile::read(path), {}), "Import unchanged native Pen/Font component");
    const auto name = s.graph().nodes().at(0).moduleName;
    const bool isPen = name.ends_with("Pen");
    s.enterModule(1); auto& graph = s.graph();
    const auto view = graph.addNode(ModuleGui, 0, 0);
    const auto bounds = graph.addNode(Area, 0, 0);
    const auto extent = graph.addNode(Float, 0, 0, 4.f);
    graph.connect({extent, bounds, 2}); graph.connect({extent, bounds, 3});
    const auto draw = graph.addNode(isPen ? Line : Text, 0, 0);
    graph.connect({view, draw, 0}); graph.connect({bounds, draw, 1});
    graph.connect({1, draw, isPen ? 2 : 4});
    if (!isPen) { const auto text = graph.addNode(String, 0, 0, std::string("Native Font")); graph.connect({text, draw, 3}); }
    s.navigate(0);
    const auto verify = [&](Schematic& root) {
        const auto* g = root.module(1);
        const auto commands = g->panelDrawings(std::get<yellow::View>(g->readValue(view)));
        check(commands.size() == 1, "Imported drawing property reaches a GUI drawing command");
        if (isPen) {
            const auto& pen = std::get<yellow::PenStroke>(std::get<yellow::DrawCommand>(commands[0].command).tool);
            check(pen == yellow::PenStroke{yellow::Color{0x50ffffff}, .125f, yellow::LineStyle::Solid}, "Native Label/LED pen descriptor matches stored bits");
        } else {
            const auto& font = std::get<yellow::TextCommand>(commands[0].command).font;
            check(font.face == "Arial" && font.size == (name == "Label Font" ? 1.25f : 1.2f) &&
                  font.style == yellow::FontStyle{name == "Label Font", false, false, false} &&
                  font.alternateFace.empty() && font.fallbackFaces.empty(), "Native Font descriptor matches stored values");
        }
    };
    verify(s.graph()); QTemporaryDir dir; check(dir.isValid(), "Native drawing round-trip directory");
    const auto saved = SchematicFile::project(s); const auto target = dir.filePath("drawing.ostone"); SchematicFile::write(target, saved);
    Session reopened; check(SchematicFile::loadProject(reopened, SchematicFile::read(target), {}), "Reopen converted native Pen/Font from disk");
    check(SchematicFile::project(reopened) == saved, "Native Pen/Font properties round-trip unchanged"); verify(reopened.graph());
    std::cout << name << ": native record, drawing and save/reopen passed\n";
}


QByteArray texturePng() {
    return QByteArray::fromHex("89504e470d0a1a0a0000000d4948445200000002000000010806000000f4227f8a0000001149444154789c63e01291d338b16fcb7f000a22039e76e90c9f0000000049454e44ae426082");
}
void brushPayload(Bytes& b, int version, int kind, bool ellipse = false) {
    if (version < 167) { b.f(.1f); b.d(0xff000000); b.d(0); return; }
    b.d(kind);
    switch (kind) {
    case 2: b.d(0x80112233); break;
    case 3: b.d(0xff112233); b.d(0x40556677); b.f(1); b.f(2); b.f(4); b.f(6); b.f(45); b.d(2); break;
    case 4: {
        b.b(ellipse);
        if (ellipse) {
            b.d(0); b.d(13);
            constexpr float kx = 4 * .276142375f, ky = 6 * .276142375f;
            const std::array<green::Point,13> points{{{5,5},{5,5+ky},{3+kx,8},{3,8},{3-kx,8},{1,5+ky},
                {1,5},{1,5-ky},{3-kx,2},{3,2},{3+kx,2},{5,5-ky},{5,5}}};
            for (int i = 0; i < 13; ++i) { b.f(points[i].x); b.f(points[i].y); b.b(i == 0 ? 0 : i == 12 ? 0x83 : 3); }
        } else {
            b.d(4); for (auto p : {green::Point{1,2}, {5,2}, {5,8}, {1,8}}) { b.f(p.x); b.f(p.y); }
        }
        b.d(4); for (int i = 0; i < 4; ++i) b.d(0x40556677);
        b.d(0xff112233); b.f(.5f); b.f(.75f); b.f(1.25f); b.f(2.5f); break;
    }
    case 5: b.d(0xff112233); b.d(0x40556677); b.d(52); break;
    case 6: { const auto png = texturePng(); b.d(png.size()); b.append(png); b.d(1); break; }
    }
}
QByteArray brushDocument(int version = 177, bool ellipse = false) {
    Bytes b; header(b, version); beginModule(b, "Root", 1); beginModule(b, "Drawing Properties", 6);
    primitive(b, 216, 0, 0);
    if (version >= 159) { b.zeros(2); b.d(yellow::stringFormatFlagMask); b.d(2); b.d(1); }
    int kind = 2;
    for (int tag : {351,352,353,354,369}) { primitive(b, tag, kind, 0); brushPayload(b, version, kind++, ellipse); }
    b.w(0); endModule(b); b.w(0); endModule(b); b.w(0); b.b(0); return b;
}
yellow::PaintTool expectedBrush(int node, int version, bool ellipse = false) {
    if (version < 167) {
        switch (node) {
        case 2: return yellow::SolidBrush{}; case 3: return yellow::LinearGradientBrush{};
        case 4: return yellow::RadialGradientBrush{}; case 5: return yellow::HatchBrush{};
        default: return yellow::TextureBrush{};
        }
    }
    const yellow::Color start{0xff112233}, end{0x40556677}; const yellow::Area area{1,2,4,6};
    switch (node) {
    case 2: return yellow::SolidBrush{yellow::Color{0x80112233}};
    case 3: return yellow::LinearGradientBrush{start,end,area,45,yellow::BrushWrap::FlipY};
    case 4: {
        yellow::RadialGradientBrush brush{start,end,area,ellipse ? yellow::RadialShape::Ellipse : yellow::RadialShape::Rectangle, green::Point{1.25f,2.5f}, {.5f,.75f}};
        yellow::RadialPath boundary;
        if (ellipse) {
            constexpr float kx=4*.276142375f,ky=6*.276142375f;
            boundary.points={{5,5},{5,5+ky},{3+kx,8},{3,8},{3-kx,8},{1,5+ky},{1,5},{1,5-ky},{3-kx,2},{3,2},{3+kx,2},{5,5-ky},{5,5}};
            boundary.types={0,3,3,3,3,3,3,3,3,3,3,3,0x83};
        } else boundary.points={{1,2},{5,2},{5,8},{1,8}};
        brush.boundary=boundary;brush.surroundColors=std::vector<yellow::Color>(4,end);return brush;
    }
    case 5: return yellow::HatchBrush{start,end,yellow::HatchStyle::SolidDiamond};
    default: return yellow::TextureBrush{yellow::Bitmap{2,1,{10,20,30,40,200,190,180,255}},yellow::BrushWrap::FlipX};
    }
}
void stringFormatsAndBrushes() {
    using enum PrimitiveKind;
    for (const int version : {154,158,159,166,167,171,177}) for (const bool ellipse : {false,true}) {
        Session s; SchematicFile::importFlowStone(s, zip(brushDocument(version, ellipse)), {}); s.enterModule(1);
        yellow::StringFormat expectedFormat;
        if (version >= 159) { expectedFormat = yellow::stringFormatFromFlags(yellow::stringFormatFlagMask); expectedFormat.horizontal = yellow::HorizontalAlignment::Right; expectedFormat.vertical = yellow::VerticalAlignment::Center; }
        const auto verify = [&](Schematic& graph) {
            check(std::get<yellow::StringFormat>(graph.readValue(1)) == expectedFormat, "Preserve all String Format flags and alignments");
            for (int id = 2; id <= 6; ++id) check(std::get<yellow::PaintTool>(graph.readValue(id)) == expectedBrush(id, version, ellipse), "Preserve brush colors, geometry, styles and texture pixels");
        };
        verify(s.graph());
        const auto formatStyle = s.graph().addNode(String,0,0,std::string("nowrap"));
        s.graph().connect({formatStyle,1,0});
        auto overridden = yellow::stringFormatFromFlags(4096);
        overridden.horizontal = expectedFormat.horizontal; overridden.vertical = expectedFormat.vertical;
        check(std::get<yellow::StringFormat>(s.graph().readValue(1)) == overridden,"Connected String Format style replaces flags while preserving alignment defaults");
        const auto linkedFormat = SchematicFile::project(s); Session formatCopy; SchematicFile::loadProject(formatCopy,linkedFormat,{});formatCopy.enterModule(1);
        check(std::get<yellow::StringFormat>(formatCopy.graph().readValue(1)) == overridden,"String Format style override survives reopening");
        s.graph().disconnectInput(1,0); verify(s.graph());
        if (version >= 167) {
            const auto color = s.graph().addNode(Color, 0, 0, yellow::Color{0xffabcdef});
            s.graph().connect({color,2,0});
            check(std::get<yellow::SolidBrush>(std::get<yellow::PaintTool>(s.graph().readValue(2))).color == yellow::Color{0xffabcdef}, "Connected brush color overrides stored default");
            const auto saved = SchematicFile::project(s); Session copy; SchematicFile::loadProject(copy, saved, {}); copy.enterModule(1);
            check(std::get<yellow::SolidBrush>(std::get<yellow::PaintTool>(copy.graph().readValue(2))).color == yellow::Color{0xffabcdef}, "Brush override survives reopening");
            s.graph().disconnectInput(2,0);
            const auto angle = s.graph().addNode(Float,0,0,90.f), center = s.graph().addNode(Float,0,0,3.f);
            const auto style = s.graph().addNode(String,0,0,std::string("horizontal"));
            const auto wrap = s.graph().addNode(String,0,0,std::string("clamp"));
            s.graph().connect({angle,3,3}); s.graph().connect({center,4,4}); s.graph().connect({style,5,2}); s.graph().connect({wrap,6,1});
            check(std::get<yellow::LinearGradientBrush>(std::get<yellow::PaintTool>(s.graph().readValue(3))).angle == 90, "Connected gradient angle overrides stored angle");
            check(std::get<yellow::RadialGradientBrush>(std::get<yellow::PaintTool>(s.graph().readValue(4))).center == green::Point{3,2.5f}, "A radial center input preserves the other stored coordinate");
            check(std::get<yellow::HatchBrush>(std::get<yellow::PaintTool>(s.graph().readValue(5))).style == yellow::HatchStyle::Horizontal, "Connected hatch style overrides stored style");
            check(std::get<yellow::TextureBrush>(std::get<yellow::PaintTool>(s.graph().readValue(6))).wrap == yellow::BrushWrap::Clamp, "Connected texture wrap overrides stored wrap");
            const auto wired = SchematicFile::project(s); Session reopened; SchematicFile::loadProject(reopened, wired, {}); reopened.enterModule(1);
            check(SchematicFile::project(reopened) == wired, "Every brush override persists with its stored defaults");
            for (const auto [id,input] : {std::pair{3,3}, {4,4}, {5,2}, {6,1}}) s.graph().disconnectInput(id,input);
        }
        verify(s.graph());
        // Draw with the imported values, then repeat the same checks after a disk round-trip.
        const auto view = s.graph().addNode(ModuleGui,0,0);
        const auto bounds = s.graph().addNode(String,0,0,std::string("0,0,4,4"));
        const auto text = s.graph().addNode(String,0,0,std::string("Imported format"));
        for (int id = 1; id <= 6; ++id) {
            const auto draw = s.graph().addNode(id == 1 ? Text : Rect,0,0);
            s.graph().connect({view,draw,0}); s.graph().connect({bounds,draw,1});
            if (id == 1) { s.graph().connect({id,draw,5}); s.graph().connect({text,draw,3}); }
            else s.graph().connect({id,draw,2});
        }
        const auto drawings = [&](Schematic& graph) {
            const auto commands = graph.panelDrawings(std::get<yellow::View>(graph.readValue(view)));
            check(commands.size() == 6, "Imported String Format and brushes reach drawing commands");
            check(std::get<yellow::TextCommand>(commands[0].command).format == expectedFormat, "Text drawing retains imported String Format flags");
            for (int id = 2; id <= 6; ++id) check(std::get<yellow::DrawCommand>(commands[id-1].command).tool == expectedBrush(id,version,ellipse), "Drawing retains imported brush descriptor");
        };
        drawings(s.graph()); QTemporaryDir dir; const auto path = dir.filePath("brushes.ostone");
        const auto saved = SchematicFile::project(s); SchematicFile::write(path,saved); Session reopened;
        SchematicFile::loadProject(reopened,SchematicFile::read(path),{}); reopened.enterModule(1);
        check(SchematicFile::project(reopened) == saved, "Brushes and String Format round-trip unchanged");
        verify(reopened.graph()); drawings(reopened.graph());
    }
    auto bad = brushDocument();
    // Locate the String Format flag word independently of its value.
    const auto format = bad.indexOf(QByteArray::fromHex("d80000000000000000"));
    check(format >= 0, "Find String Format fixture"); qToLittleEndian<quint32>(8,bad.data()+format+9);
    expectError(zip(bad), "String Format");
    bad = brushDocument(); const auto png = bad.indexOf(texturePng()); check(png > 0,"Find texture fixture"); bad[png] = 'x'; expectError(zip(bad), "texture image");
    bad = brushDocument(); const auto kind = bad.indexOf(QByteArray::fromHex("5f01020000000002000000"));
    check(kind >= 0,"Find solid brush fixture"); qToLittleEndian<quint32>(99,bad.data()+kind+7); expectError(zip(bad), "paint-tool kind");
    const auto payload = [&](QByteArray& data, int tag, int x) {
        Bytes prefix; primitive(prefix,tag,x,0); const auto at=data.indexOf(prefix);check(at>=0,"Find brush payload");return at+prefix.size();
    };
    bad=brushDocument();qToLittleEndian<quint32>(5,bad.data()+payload(bad,352,3)+32);expectError(zip(bad),"brush wrap");
    bad=brushDocument();qToLittleEndian<quint32>(4097,bad.data()+payload(bad,353,4)+5);expectError(zip(bad),"record count");
    bad=brushDocument(177,true);bad[payload(bad,353,4)+21]=char(3);expectError(zip(bad),"radial path point type");
    bad=brushDocument();qToLittleEndian<quint32>(5,bad.data()+payload(bad,353,4)+41);expectError(zip(bad),"surround color count");
    bad=brushDocument();qToLittleEndian<quint32>(53,bad.data()+payload(bad,354,5)+12);expectError(zip(bad),"hatch brush style");
    bad=brushDocument();qToLittleEndian<quint32>(0xffffffff,bad.data()+payload(bad,369,6)+4);expectError(zip(bad),"record count");
    bad=brushDocument();const auto solid=payload(bad,351,2)-7;bad[solid]=char(352&255);bad[solid+1]=char(352>>8);expectError(zip(bad),"Mismatched brush");
    bad=brushDocument();qToLittleEndian<quint32>(3,bad.data()+format+13);expectError(zip(bad),"String Format");
    // Empty native paint-tool caches do not invent a cached drawing value.
    {
        Bytes b;header(b,177);beginModule(b,"Root",5);
        for(int tag:{351,352,353,354,369}){primitive(b,tag,0,0);b.d(0);}
        b.w(0);endModule(b);b.w(0);b.b(0);Session empty;SchematicFile::importFlowStone(empty,zip(b),{});
        for(int id=1;id<=5;++id)check(std::get<yellow::PaintTool>(empty.graph().readValue(id))==expectedBrush(id+1,166),"Empty brush caches use builder defaults");
    }
    expectError(zip(brushDocument().left(brushDocument().size()-30)), "Truncated");
}


QByteArray customRadialDocument(const yellow::RadialPath& path, const std::vector<yellow::Color>& colors,
                               int version=177, green::Point center={2.5f,2.5f}, green::Point focus={}) {
    Bytes b;header(b,version);beginModule(b,"Root",1);beginModule(b,"Custom radial",1);primitive(b,353,3,4,"Path gradient");
    b.d(4);b.b(!path.types.empty());if(!path.types.empty())b.d(path.winding?1:0);b.d(path.points.size());
    for(std::size_t i=0;i<path.points.size();++i){b.f(path.points[i].x);b.f(path.points[i].y);if(!path.types.empty())b.b(path.types[i]);}
    b.d(colors.size());for(const auto color:colors)b.d(color.argb);
    b.d(0xfffefdfc);b.f(focus.x);b.f(focus.y);b.f(center.x);b.f(center.y);
    b.w(0);endModule(b);b.w(0);endModule(b);b.w(0);b.b(0);return b;
}
void customRadialPaths() {
    using enum PrimitiveKind;
    using yellow::RadialPath;
    const RadialPath triangle{{{0,0},{8,0},{0,8}}, {}, false};
    const RadialPath concave{{{-2,-1},{6,-1},{6,2},{1.125f,2},{1.125f,6},{-2,6}}, {}, false};
    const RadialPath curve{{{0,4},{0,-4},{8,-4},{8,4}}, {0x20,0x13,0x23,0x83}, true};
    const RadialPath openFigures{{{-2,-2},{-2,2},{0,0},{4,-2},{6,0},{4,2}}, {0,1,0x21,0x20,1,1}, false};
    const RadialPath ring{{{0,0},{8,0},{8,8},{0,8},{2,2},{6,2},{6,6},{2,6}}, {0x20,0x11,1,0xa1,0,1,0x21,0x81}, false};
    const std::vector<std::pair<RadialPath,std::vector<yellow::Color>>> fixtures{
        {triangle,{{0xffff0000},{0xff00ff00},{0xff0000ff}}},
        {concave,{{0x80112233},{0x0000ffff},{0xffaabbcc},{0x11010203},{0xff000000},{0x80112233}}},
        {curve,{{0xffff0000},{0x40112233},{0xff00ff00},{0xff0000ff}}},
        {openFigures,{{0x80563412}}},
        {ring,{{0xffff0000},{0xff00ff00},{0xff0000ff},{0xffffff00},{0xffff00ff},{0xff00ffff},{0xffffffff},{0xff000000}}},
        {RadialPath{ring.points,ring.types,true},{{0xff123456},{0x80010203}}},
    };
    for(const auto& [boundary,colors]:fixtures)for(int version:{167,170,177}) {
        const green::Point center{2.5f,2.5f},focus{.25f,.75f};
        Session s;SchematicFile::importFlowStone(s,zip(customRadialDocument(boundary,colors,version,center,focus)),{});s.enterModule(1);
        const auto imported=std::get<yellow::RadialGradientBrush>(std::get<yellow::PaintTool>(s.graph().readValue(1)));
        check(imported.boundary==boundary && imported.surroundColors==colors,"Retain every native point, type/flag byte, fill rule and ordered ARGB surround color");
        check(imported.start==yellow::Color{0xfffefdfc} && imported.center==center && imported.focus==focus,"Retain custom radial center color, position and focus");
        const auto view=s.graph().addNode(ModuleGui,0,0),bounds=s.graph().addNode(String,0,0,std::string("-3,-5,12,14")),draw=s.graph().addNode(Rect,0,0);
        s.graph().connect({view,draw,0});s.graph().connect({bounds,draw,1});s.graph().connect({1,draw,2});
        const auto verify=[&](Schematic& graph){
            const auto commands=graph.panelDrawings(std::get<yellow::View>(graph.readValue(view)));
            check(commands.size()==1,"Custom radial brush reaches its drawing command");
            const auto actual=std::get<yellow::RadialGradientBrush>(std::get<yellow::DrawCommand>(commands[0].command).tool);
            check(actual==imported,"Drawing keeps custom geometry and colors without reducing to a standard shape");return actual;
        };
        const QRectF coverage(-3,-5,12,14);const QSize size(48,56);
        const auto image=rasterizeRadialBrush(verify(s.graph()),coverage,size);check(!image.isNull(),"Custom path rasterizes headlessly");
        QTemporaryDir dir;const auto target=dir.filePath("custom-radial.ostone");const auto saved=SchematicFile::project(s);SchematicFile::write(target,saved);
        Session reopened;SchematicFile::loadProject(reopened,SchematicFile::read(target),{});reopened.enterModule(1);
        check(SchematicFile::project(reopened)==saved,"Custom radial schematic survives disk save/reopen unchanged");
        const auto restored=verify(reopened.graph());
        check(restored.boundary==boundary && restored.surroundColors==colors,"Reopened radial data retains original geometry and all color bits");
        check(rasterizeRadialBrush(restored,coverage,size)==image,"Reopened custom geometry and colors produce identical raster output");
        const auto tint=reopened.graph().addNode(PrimitiveKind::Color,0,0,yellow::Color{0x40556677});reopened.graph().connect({tint,1,1});
        const auto uniform=std::get<yellow::RadialGradientBrush>(std::get<yellow::PaintTool>(reopened.graph().readValue(1)));
        check(uniform.boundary==boundary && uniform.surroundColors==std::vector<yellow::Color>(colors.size(),yellow::Color{0x40556677}),"An explicit Col2 input overrides boundary colors while retaining geometry and count");
        reopened.graph().disconnectInput(1,1);check(std::get<yellow::RadialGradientBrush>(std::get<yellow::PaintTool>(reopened.graph().readValue(1)))==imported,"Disconnecting restores the original multicolor array");
        const auto shape=reopened.graph().addNode(String,0,0,std::string("rectangle"));reopened.graph().connect({shape,1,3});
        const auto standard=std::get<yellow::RadialGradientBrush>(std::get<yellow::PaintTool>(reopened.graph().readValue(1)));
        check(!standard.boundary && standard.surroundColors.empty() && standard.shape==yellow::RadialShape::Rectangle,"An explicit Shape input replaces imported custom geometry");
        reopened.graph().disconnectInput(1,3);check(std::get<yellow::RadialGradientBrush>(std::get<yellow::PaintTool>(reopened.graph().readValue(1)))==imported,"Disconnecting Shape restores custom paths and colors");
    }
    // Independent rendering checks: a triangle is not its bounding ellipse or rectangle.
    Session triangleSession;SchematicFile::importFlowStone(triangleSession,zip(customRadialDocument(triangle,fixtures[0].second)),{});triangleSession.enterModule(1);
    auto brush=std::get<yellow::RadialGradientBrush>(std::get<yellow::PaintTool>(triangleSession.graph().readValue(1)));
    const auto triangleImage=rasterizeRadialBrush(brush,QRectF(0,0,8,8),QSize(8,8));
    check(triangleImage.pixelColor(2,2).rgba()==0xfffefdfc && triangleImage.pixelColor(6,6).alpha()==0,"Custom path clips outside its triangle and retains its exact center color");
    const auto greenPixel=triangleImage.pixelColor(6,0),bluePixel=triangleImage.pixelColor(0,6);
    check(greenPixel.green()>greenPixel.red() && greenPixel.green()>greenPixel.blue() && bluePixel.blue()>bluePixel.red() && bluePixel.blue()>bluePixel.green(),"Distinct boundary points contribute distinct surround colors");
    brush.boundary=curve;brush.area={0,-4,8,8};brush.center=green::Point{4,1};brush.surroundColors=fixtures[2].second;
    const auto curved=rasterizeRadialBrush(brush,QRectF(0,-4,8,8),QSize(16,16));
    check(curved.pixelColor(8,0).alpha()==0 && curved.pixelColor(8,8).alpha()>0,"Bezier geometry retains its curved boundary rather than its control-point bounding box");
    brush.boundary=ring;brush.area={0,0,8,8};brush.center=green::Point{4.5f,4.5f};brush.surroundColors=fixtures[4].second;
    const auto alternate=rasterizeRadialBrush(brush,QRectF(0,0,8,8),QSize(8,8));brush.boundary->winding=true;
    const auto winding=rasterizeRadialBrush(brush,QRectF(0,0,8,8),QSize(8,8));
    check(alternate.pixelColor(4,4).alpha()==0 && winding.pixelColor(4,4).alpha()>0,"Multiple figures preserve alternate versus winding fill behavior");
    // Invalid path structures and counts still fail atomically.
    for(const auto types:std::vector<std::vector<std::uint8_t>>{{1,1,1,1},{0,3,1,0x83},{0,0x83,3,3},{0,0x41,1,0x81},{0,0x81,1,1}})
        expectError(zip(customRadialDocument({curve.points,types,false},fixtures[2].second)),"radial path");
    expectError(zip(customRadialDocument(triangle,{})),"surround color count");
    expectError(zip(customRadialDocument(triangle,std::vector<yellow::Color>(4,yellow::Color{0xffffffff}))),"surround color count");
    auto badPath=triangle;badPath.points[0].x=std::numeric_limits<float>::infinity();expectError(zip(customRadialDocument(badPath,fixtures[0].second)),"coordinate");
    expectError(zip(customRadialDocument(curve,fixtures[2].second).left(80)),"Truncated");
    check(rasterizeRadialBrush(brush,QRectF(0,0,8,8),QSize(100000,100000)).isNull(),"Oversized render requests are bounded");
    // Converted-file validation uses the same path rules and leaves the open project intact.
    {
        Session current;SchematicFile::importFlowStone(current,zip(customRadialDocument(curve,fixtures[2].second)),{});
        const auto before=SchematicFile::project(current);
        for(int mutation=0;mutation<3;++mutation){
            auto object=QJsonDocument::fromJson(before).object();auto root=object["root"].toObject();auto modules=root["nodes"].toArray();auto module=modules[0].toObject();auto nodes=module["nodes"].toArray();auto node=nodes[0].toObject();auto value=node["value"].toObject();auto data=value["data"].toObject();
            auto path=data["boundary"].toObject();
            if(mutation==0)path["types"]=QJsonArray{0,3};
            if(mutation==1)path["winding"]=7;
            if(mutation==2)data["surroundColors"]=QJsonArray{1,2,3,4,5};
            data["boundary"]=path;value["data"]=data;node["value"]=value;nodes[0]=node;module["nodes"]=nodes;modules[0]=module;root["nodes"]=modules;object["root"]=root;
            bool rejected=false,touched=false;
            try{SchematicFile::loadProject(current,QJsonDocument(object).toJson(),[&]{touched=true;});}catch(const std::exception&){rejected=true;}
            check(rejected && !touched && SchematicFile::project(current)==before,"Invalid saved radial geometry/color data cannot replace the current schematic");
        }
    }
    // Pre-extension .ostone radial descriptors continue to load without new fields.
    Session old;SchematicFile::importFlowStone(old,zip(customRadialDocument(triangle,fixtures[0].second)),{});
    auto object=QJsonDocument::fromJson(SchematicFile::project(old)).object();auto root=object["root"].toObject();auto modules=root["nodes"].toArray();auto module=modules[0].toObject();auto nodes=module["nodes"].toArray();auto node=nodes[0].toObject();auto value=node["value"].toObject();auto data=value["data"].toObject();
    data.remove("boundary");data.remove("surroundColors");value["data"]=data;node["value"]=value;nodes[0]=node;module["nodes"]=nodes;modules[0]=module;root["nodes"]=modules;object["root"]=root;
    Session legacy;SchematicFile::loadProject(legacy,QJsonDocument(object).toJson(),{});legacy.enterModule(1);
    check(!std::get<yellow::RadialGradientBrush>(std::get<yellow::PaintTool>(legacy.graph().readValue(1))).boundary,"Older standard radial brush saves stay readable");
}

void nativeLabelModule(const QString& path) {
    using enum PrimitiveKind;
    Session s; SchematicFile::importFlowStone(s,SchematicFile::read(path),{});
    s.enterModule(1); auto& graph=s.graph();
    check(graph.nodes().size()==11 && graph.module(10)->nodes().size()==9,"Import the unchanged Label GUI and its nested Inflate graph");
    // Use the native color and View/Area routes to exercise a brush inside this GUI.
    const auto brush=graph.addNode(Solid,0,0), fill=graph.addNode(Rect,0,0);
    graph.connect({9,brush,0}); graph.connect({5,fill,0}); graph.connect({4,fill,1}); graph.connect({brush,fill,2});
    graph.setValue(2,std::string("4096,right,bottom")); s.navigate(0);
    const auto benchId = s.makeModule(std::array{1}); s.enterModule(benchId);
    const auto view=s.graph().addNode(ModuleGui,0,0), label=s.graph().addNode(String,0,0,std::string("Imported Label"));
    s.graph().connect({view,1,0});s.graph().connect({label,1,1});
    const auto verify=[&](Schematic& root) {
        const auto* bench = root.module(benchId);
        const auto commands=bench->panelDrawings(std::get<yellow::View>(bench->readValue(view)));
        check(commands.size()==2,"Native Label GUI produces text and a connected brush drawing");
        const auto& text=std::get<yellow::TextCommand>(commands[0].command);
        check(text.text=="Imported Label" && text.font.face=="Arial" && text.font.size==1.2f &&
              text.color==yellow::Color{0xff626262},"Native GUI Text/Font/Color links remain intact");
        check(!text.format.wrap && text.format.horizontal==yellow::HorizontalAlignment::Right &&
              text.format.vertical==yellow::VerticalAlignment::Bottom,"Legacy String Format text remains live through its original link");
        check(std::get<yellow::SolidBrush>(std::get<yellow::DrawCommand>(commands[1].command).tool).color==text.color,
              "A brush uses the bundled GUI's native property color");
    };
    s.navigate(0); verify(s.graph());QTemporaryDir dir;const auto target=dir.filePath("native-label.ostone");const auto saved=SchematicFile::project(s);SchematicFile::write(target,saved);
    Session reopened;SchematicFile::loadProject(reopened,SchematicFile::read(target),{});
    check(SchematicFile::project(reopened)==saved,"Converted Label GUI and its live String Format link round-trip unchanged");verify(reopened.graph());
    std::cout<<"Stream Power Label: native GUI, legacy String Format, brush drawing and save/reopen passed\n";
}

void nativeFormatRecord(const QString& path) {
    using enum PrimitiveKind;
    Session s; SchematicFile::importFlowStone(s,SchematicFile::read(path),{}); const auto name = s.graph().nodes()[0].moduleName;
    s.enterModule(1); auto& graph = s.graph();
    auto expected = yellow::StringFormat{}; expected.horizontal = yellow::HorizontalAlignment::Center; expected.vertical = yellow::VerticalAlignment::Center;
    check(std::get<yellow::StringFormat>(graph.readValue(1)) == expected, "Native String Format record retains centered alignment");
    const auto view=graph.addNode(ModuleGui,0,0), bounds=graph.addNode(String,0,0,std::string("0,0,4,4"));
    const auto text=graph.addNode(String,0,0,std::string("Native format")), draw=graph.addNode(Text,0,0);
    graph.connect({view,draw,0}); graph.connect({bounds,draw,1}); graph.connect({text,draw,3}); graph.connect({1,draw,5});
    const auto verify=[&](Schematic& root) {
        const auto* g=root.module(1);const auto commands=g->panelDrawings(std::get<yellow::View>(g->readValue(view)));
        check(commands.size()==1 && std::get<yellow::TextCommand>(commands[0].command).format==expected,"Native String Format reaches a Text drawing");
    };
    s.navigate(0); verify(s.graph()); QTemporaryDir dir;const auto target=dir.filePath("native-format.ostone");const auto saved=SchematicFile::project(s);SchematicFile::write(target,saved);
    Session reopened;SchematicFile::loadProject(reopened,SchematicFile::read(target),{});
    check(SchematicFile::project(reopened)==saved,"Native String Format survives conversion unchanged");verify(reopened.graph());
    std::cout<<name<<": native String Format, text drawing and save/reopen passed\n";
}


QByteArray shapeDocument(int version=177) {
    Bytes b;header(b,version);beginModule(b,"Root",1);beginModule(b,"Shapes",13);
    nativePanel(b,version<159?16:1191,version);
    stringBox(b,"1.25,-2,8,6");
    primitive(b,403,1,2,"Fill");b.zeros(2);b.b(128);b.b(17);b.b(34);b.b(51);
    for(float value:{-450.f,225.f,3.f}){primitive(b,11,2,3);b.f(6);b.zeros(2);b.f(value);}
    for(int tag:{218,219,220,221,109,386})primitive(b,tag,10,4);
    primitive(b,214,1,10);
    if(version>=159){b.zeros(2);if(version>=167)b.d(1);b.f(.125f);b.d(0xff102030);b.d(2);}
    b.w(25);
    for(int index=6;index<12;++index){wire(b,version,index==6?0:index-1,0,index,0);wire(b,version,1,0,index,1);wire(b,version,2,0,index,2);}
    for(int index:{10,11}){wire(b,version,3,0,index,3);wire(b,version,4,0,index,4);}
    wire(b,version,5,0,6,3);wire(b,version,5,0,7,3);wire(b,version,2,0,12,0);
    panelFooter(b,254);b.w(0);endModule(b);b.w(0);b.b(0);return b;
}
void importedShapes() {
    using enum PrimitiveKind;using yellow::DrawCommand;using yellow::ShapeKind;
    for(int version:{154,159,166,171,177}) {
        Session s;SchematicFile::importFlowStone(s,zip(shapeDocument(version)),{});s.enterModule(1);
        const std::array expected{ShapeKind::Rect,ShapeKind::Rect,ShapeKind::Ellipse,ShapeKind::Ellipse,ShapeKind::Arc,ShapeKind::Pie};
        const auto verify=[&](Schematic& graph,float start,float sweep,yellow::Color color){
            const auto view=std::get<yellow::View>(graph.readValue(1));const auto commands=graph.panelDrawings(view);
            check(commands.size()==6,"All imported shape aliases draw in their native View chain order");
            check(std::get<yellow::View>(graph.readValue(12)).isSamePanel(view),"Imported Pie passes the original View onward");
            for(int i=0;i<6;++i){
                const auto& command=std::get<DrawCommand>(commands[i].command);
                check(command.shape==expected[i] && command.area==yellow::Area{1.25f,-2,8,6},"Imported shape kind and Area connections are preserved");
                check(std::get<yellow::SolidBrush>(command.tool).color==color,"Native Color links convert directly to solid Paint Tools with exact alpha");
                if(i<2)check(command.cornerRadius==3,"Native Rect retains its Corner input");
                if(i>=4)check(command.startAngle==start && command.sweepAngle==sweep,"Arc/Pie keep start and signed sweep inputs");
            }
        };
        verify(s.graph(),-90,225,{0x80112233});
        s.graph().setValue(4,450.f);s.graph().setValue(5,-90.f);s.graph().setValue(3,yellow::Color{0x40123456});
        verify(s.graph(),90,-90,{0x40123456});
        QTemporaryDir dir;const auto target=dir.filePath("shapes.ostone");const auto saved=SchematicFile::project(s);SchematicFile::write(target,saved);
        Session reopened;SchematicFile::loadProject(reopened,SchematicFile::read(target),{});reopened.enterModule(1);
        check(SchematicFile::project(reopened)==saved,"Converted shape records and live connections survive disk save/reopen unchanged");
        verify(reopened.graph(),90,-90,{0x40123456});
        reopened.graph().disconnectInput(11,2);reopened.graph().connect({13,11,2});
        auto commands=reopened.graph().panelDrawings(std::get<yellow::View>(reopened.graph().readValue(1)));
        check(std::holds_alternative<yellow::PenStroke>(std::get<DrawCommand>(commands[4].command).tool),"Imported Arc accepts a Pen as well as a brush");
        const auto gradient=reopened.graph().addNode(LinearGradientBrush,0,0);reopened.graph().connect({2,gradient,2});
        reopened.graph().disconnectInput(11,2);reopened.graph().connect({gradient,11,2});
        commands=reopened.graph().panelDrawings(std::get<yellow::View>(reopened.graph().readValue(1)));
        const auto& filled=std::get<DrawCommand>(commands[4].command);
        check(std::holds_alternative<yellow::LinearGradientBrush>(filled.tool) && !shapeFillPath(filled,QRectF(0,0,8,6)).isEmpty(),"Imported gradient Arc has a closed fill path");
        reopened.graph().setValue(5,0.f);
        check(reopened.graph().panelDrawings(std::get<yellow::View>(reopened.graph().readValue(1))).size()==4,"Zero sweep hides Arc/Pie while keeping earlier shapes and the View route");
    }
    // Geometry checks use the same helper as the actual gradient renderer.
    yellow::DrawCommand arc{.shape=ShapeKind::Arc,.area={0,0,8,8},.tool=yellow::SolidBrush{},.startAngle=0,.sweepAngle=90};
    const auto segment=shapeFillPath(arc,QRectF(0,0,8,8));
    check(segment.elementAt(0).x==8 && segment.elementAt(0).y==4 && panelShapeStartAngle(arc)==0,"Zero-angle imported arcs start at +X without an added half turn");
    auto pie=arc;pie.shape=ShapeKind::Pie;const auto sector=shapeFillPath(pie,QRectF(0,0,8,8));
    check(segment.contains({6.5,6.5}) && !segment.contains({5,5}) && sector.contains({5,5}),"Brush Arc fills a chord segment while Pie includes the center");
    arc.sweepAngle=-90;const auto counterclockwise=shapeFillPath(arc,QRectF(0,0,8,8));
    check(counterclockwise.contains({6.5,1.5}) && !counterclockwise.contains({6.5,6.5}),"Negative sweeps keep their counterclockwise direction");
    pie.sweepAngle=360;check(shapeFillPath(pie,QRectF(0,0,8,8)).contains({4,4}),"A full-turn Pie fills the whole ellipse");
    auto bad=shapeDocument();const auto arcRecord=bad.indexOf(QByteArray::fromHex("6d000a00040000"));check(arcRecord>=0,"Find Arc record fixture");
    expectError(zip(bad.left(arcRecord+4)),"Truncated");
    // Native arity stays meaningful: Ellipse has no angle input.
    Bytes b;header(b,177);beginModule(b,"Root",2);primitive(b,220,0,0);primitive(b,11,0,0);b.f(6);b.zeros(2);b.f(30);
    b.w(1);wire(b,177,1,0,0,3);endModule(b);b.w(0);b.b(0);expectError(zip(b),"connector index");
}

QByteArray drawingControlsDocument(int version) {
    Bytes b; header(b, version); beginModule(b, "Root", 1); beginModule(b, "Drawing controls", 12);
    nativePanel(b, version < 159 ? 16 : 1191, version);
    stringBox(b, "1,2,8,4");
    primitive(b, 403, 0, 0); b.zeros(2); b.b(255); b.b(238); b.b(32); b.b(48);
    primitive(b, 11, 0, 0); b.f(8); b.zeros(2); b.f(.75f);
    primitive(b, 53, 5, 4, "Legacy rounded"); primitive(b, 98, 12, 4, "Rounded");
    primitive(b, 132, 2, 4, "Full repaint"); primitive(b, 131, 9, 4, "Partial repaint");
    for (const auto* name : {"Full", "Partial"}) {
        primitive(b, 138, 0, name[0] == 'F' ? 0 : 1, name); b.d(9); b.b(0); b.str("");
    }
    stringBox(b, "2,3,2,1"); primitive(b, 421, 4, 8);
    b.w(14);
    wire(b, version, 0, 0, 6, 0); wire(b, version, 6, 0, 4, 0);
    wire(b, version, 4, 0, 7, 0); wire(b, version, 7, 0, 5, 0);
    for (int node : {4, 5}) {
        wire(b, version, 1, 0, node, 1); wire(b, version, 2, 0, node, 2); wire(b, version, 3, 0, node, 3);
    }
    wire(b, version, 8, 0, 6, 1); wire(b, version, 9, 0, 7, 2);
    wire(b, version, 10, 0, 7, 1); wire(b, version, 0, 0, 11, 0);
    panelFooter(b, 254); b.w(0); endModule(b); b.w(0); b.b(0); return b;
}

void importedDrawingControls() {
    using enum PrimitiveKind;
    using yellow::DrawCommand;
    constexpr yellow::Color red{0xffee2030}, blue{0xff2468df};
    for (int version : {154, 159, 166, 171, 177}) {
        Session session; SchematicFile::importFlowStone(session, zip(drawingControlsDocument(version)), {});
        auto& root = session.graph(); auto& graph = *root.module(1);
        check(root.nodes()[0].moduleInputs[0].name == "Full" && root.nodes()[0].moduleInputs[1].name == "Partial",
              "Module boundaries retain connector names stored in the native common header");
        const int full = root.addNode(Trigger, 0, 0), partial = root.addNode(Trigger, 0, 0);
        root.connect({full, 1, 0}); root.connect({partial, 1, 1});
        check(graph.nodes()[4].kind == RoundRectangle && graph.nodes()[5].kind == RoundRectangle &&
              graph.nodes()[6].kind == Redraw && graph.nodes()[7].kind == RedrawArea,
              "Import both native rounded rectangle aliases and both redraw records");
        const auto view = std::get<yellow::View>(graph.readValue(1));
        check(std::get<yellow::View>(graph.readValue(6)).isSamePanel(view) &&
              std::get<yellow::View>(graph.readValue(7)).isSamePanel(view), "Redraw records pass the native View chain through");
        auto paint = graph.renderPanel(view);
        check(paint.size() == 2 && std::get<DrawCommand>(paint[0].command).cornerRadius == .75f &&
              std::get<DrawCommand>(paint[1].command).cornerRadius == .75f, "Native rounded rectangles retain their Corner links");
        graph.disconnectInput(5, 3);
        check(std::get<DrawCommand>(graph.panelDrawings(view)[0].command).cornerRadius == 1,
              "Unconnected Round Rectangle corners use the native radius of one");
        graph.connect({4, 5, 3}); graph.setValue(4, 0.f);
        check(std::get<DrawCommand>(graph.panelDrawings(view)[0].command).cornerRadius == 0,
              "An explicit zero corner overrides the rounded rectangle default");
        graph.setValue(4, .75f); root.fireTrigger(full); paint = graph.renderPanel(view);
        graph.setValue(3, blue); paint = graph.renderPanel(view);
        check(paint.size() == 2 && std::get<yellow::SolidBrush>(std::get<DrawCommand>(paint[0].command).tool).color == red,
              "Imported Redraw keeps its previous paint until DoIt fires");
        root.fireTrigger(partial); paint = graph.renderPanel(view);
        int oldPaint = 0, freshPaint = 0;
        for (const auto& layer : paint) {
            const auto color = std::get<yellow::SolidBrush>(std::get<DrawCommand>(layer.command).tool).color;
            if (color == red) {
                ++oldPaint; check(layer.mapping.contains({1.5f, 2.5f}) && !layer.mapping.contains({2.5f, 3.5f}),
                                  "Partial redraw retains the old paint outside the native damage Area");
            } else if (color == blue) {
                ++freshPaint; check(!layer.mapping.contains({1.5f, 2.5f}) && layer.mapping.contains({2.5f, 3.5f}),
                                    "Partial redraw clips new paint to the native damage Area");
            }
        }
        check(oldPaint == 2 && freshPaint == 2, "Imported partial redraw replaces only the requested region on both shapes");
        root.fireTrigger(full); paint = graph.renderPanel(view);
        check(paint.size() == 2 && std::ranges::all_of(paint, [&](const auto& layer) {
            return !layer.mapping.clip && std::get<yellow::SolidBrush>(std::get<DrawCommand>(layer.command).tool).color == blue;
        }), "Imported full redraw replaces the entire retained frame");
        root.resizePanel(1, 24, 12);
        const auto resized = graph.frontPanel()->view;
        check(graph.readValue(12, 0) == green::Value{resized.bounds.width} &&
              graph.readValue(12, 1) == green::Value{resized.bounds.height}, "Native View Size outputs remain live after resizing");
        QTemporaryDir dir; const auto target = dir.filePath("drawing-controls.ostone");
        const auto saved = SchematicFile::project(session); SchematicFile::write(target, saved);
        Session reopened; SchematicFile::loadProject(reopened, SchematicFile::read(target), {});
        check(SchematicFile::project(reopened) == saved, "Drawing controls, trigger boundaries and sizes survive disk save/reopen");
        auto& copy = *reopened.graph().module(1); const auto reopenedView = copy.frontPanel()->view;
        (void)copy.renderPanel(reopenedView); copy.setValue(3, red); reopened.graph().fireTrigger(full);
        const auto restored = copy.renderPanel(reopenedView);
        check(restored.size() == 2 && std::get<yellow::SolidBrush>(std::get<DrawCommand>(restored[0].command).tool).color == red,
              "Converted redraw trigger links still repaint after reopening");
    }
    for (int tag : {131, 132}) {
        Bytes b; header(b, 177); beginModule(b, "Root", 2); primitive(b, tag, 0, 0);
        primitive(b, 406, 0, 0); b.f(8); b.zeros(2); b.b(1);
        b.w(1); wire(b, 177, 1, 0, 0, tag == 131 ? 3 : 2); endModule(b); b.w(0); b.b(0);
        expectError(zip(b), "Linked Redraw HiRes");
        // Do not mistake an output beyond the single View for a valid connector.
        Bytes bad; header(bad, 177); beginModule(bad, "Root", 2);
        primitive(bad, tag, 0, 0); primitive(bad, 421, 0, 0);
        bad.w(1); wire(bad, 177, 0, 1, 1, 0); endModule(bad); bad.w(0); bad.b(0);
        expectError(zip(bad), "connector index");
    }
    auto truncated = drawingControlsDocument(177);
    const auto record = truncated.indexOf(QByteArray::fromHex("840002000400"));
    check(record > 0, "Find native Redraw record fixture"); expectError(zip(truncated.left(record + 5)), "Truncated");
}

QByteArray colorConversionsDocument(int version) {
    Bytes b; header(b, version); beginModule(b, "Root", 1); beginModule(b, "Color conversions", 8);
    primitive(b, 403, 1, 2); b.zeros(2); b.b(128); b.b(17); b.b(34); b.b(51);
    for (int tag : {223,64,142,143}) primitive(b, tag, 4, 2);
    nativePanel(b, version < 159 ? 16 : 1191, version); stringBox(b, "1,2,4,3"); primitive(b, 219, 12, 2);
    b.w(13); wire(b, version, 0, 0, 1, 0); wire(b, version, 0, 0, 3, 0);
    for (int channel = 0; channel < 4; ++channel) {
        wire(b, version, 1, channel, 2, channel); wire(b, version, 3, channel, 4, channel);
    }
    wire(b, version, 5, 0, 7, 0); wire(b, version, 6, 0, 7, 1); wire(b, version, 2, 0, 7, 2);
    panelFooter(b, 254); b.w(0); endModule(b); b.w(0); b.b(0); return b;
}

void importedColorConversions() {
    using enum PrimitiveKind;
    using yellow::ColorChannels;
    for (int version : {154,159,166,171,177}) {
        Session session; SchematicFile::importFlowStone(session, zip(colorConversionsDocument(version)), {});
        auto& graph = *session.graph().module(1);
        check(graph.nodes()[1].kind == ColorToArgb && graph.nodes()[2].kind == ArgbToColor &&
              graph.nodes()[3].kind == ColorToAhsv && graph.nodes()[4].kind == AhsvToColor, "Import all four native color conversion IDs");
        const auto channels = [&](int node) {
            ColorChannels values;
            for (int i = 0; i < 4; ++i) {
                check(graph.outputType(node, i) == green::Type::Int, "Native split color outputs have Integer connectors");
                values[i] = std::get<std::int32_t>(graph.readValue(node, i));
            }
            return values;
        };
        check(channels(2) == ColorChannels{128,17,34,51} && channels(4) == ColorChannels{128,210,170,51},
              "Retain ARGB channel order, AHSV degrees and byte channel ranges");
        for (auto [color, hsv] : std::vector<std::pair<yellow::Color,ColorChannels>>{
                 {{0x00ff0000},{0,0,255,255}}, {{0x7f00ff00},{127,120,255,255}},
                 {{0x400000ff},{64,240,255,255}}, {{0x80808080},{128,0,0,128}},
                 {{0xffffffff},{255,0,0,255}}, {{0x00000000},{0,0,0,0}}}) {
            graph.setValue(1, color);
            check(channels(4) == hsv && graph.readValue(3) == green::Value{color} && graph.readValue(5) == green::Value{color},
                  "Connected color conversions preserve primary colors, gray, transparency and black");
        }
        const auto panel = graph.frontPanel()->view;
        graph.setValue(1, yellow::Color{0x40123456});
        const auto drawings = graph.panelDrawings(panel);
        check(drawings.size() == 1 && std::get<yellow::SolidBrush>(std::get<yellow::DrawCommand>(drawings[0].command).tool).color == yellow::Color{0x40123456},
              "Converted ARGB outputs remain connected to actual GUI drawing");
        std::array<int,4> values;
        for (int i = 0; i < 4; ++i) {
            values[i] = graph.addNode(Int, 0, 0, std::int32_t{0});
            graph.disconnectInput(3,i); graph.disconnectInput(5,i);
            graph.connect({values[i],3,i}); graph.connect({values[i],5,i});
        }
        const auto set = [&](ColorChannels channels) { for(int i=0;i<4;++i) graph.setValue(values[i],channels[i]); };
        set({-1,256,511,-2});
        check(graph.readValue(3) == green::Value{yellow::Color{0xff00fffe}}, "Native ARGB integer components retain their low eight bits");
        for (auto [hue,color] : std::vector<std::pair<int,yellow::Color>>{
                 {30,{0x40c86400}}, {90,{0x4064c800}}, {150,{0x4000c864}},
                 {210,{0x400064c8}}, {270,{0x406400c8}}, {330,{0x40c80064}},
                 {-30,{0x40c80064}}, {390,{0x40c86400}}}) {
            set({64,hue,255,200});
            check(graph.readValue(5) == green::Value{color}, "Native AHSV inputs retain sector ordering and signed hue wrapping");
        }
        const auto saved = SchematicFile::project(session); QTemporaryDir dir; const auto target = dir.filePath("color-conversions.ostone");
        SchematicFile::write(target,saved); Session reopened; SchematicFile::loadProject(reopened,SchematicFile::read(target),{});
        check(SchematicFile::project(reopened) == saved && reopened.graph().module(1)->readValue(5) == green::Value{yellow::Color{0x40c86400}},
              "Converted color records, channel links and input edits survive disk save/reopen");
        reopened.graph().module(1)->setValue(values[1],std::int32_t{210});
        check(reopened.graph().module(1)->readValue(5) == green::Value{yellow::Color{0x400064c8}}, "Reopened color conversions respond to live channel changes");
    }
    for (int tag : {64,142,143,223,609}) {
        Bytes bad; header(bad,177); beginModule(bad,"Root",2); primitive(bad,tag,0,0); primitive(bad,29,0,0); bad.f(8); bad.zeros(2); bad.d(0);
        bad.w(1); wire(bad,177,0,(tag==142 || tag==223)?4:1,1,0); endModule(bad); bad.w(0); bad.b(0);
        expectError(zip(bad),"connector index");
        Bytes truncated; header(truncated,177); beginModule(truncated,"Root",1); truncated.w(tag); truncated.w(1);
        expectError(zip(truncated),"Truncated");
    }
}

QByteArray viewCacheDocument(int version) {
    Bytes b; header(b,version); beginModule(b,"Root",1); beginModule(b,"Cached drawing",14);
    nativePanel(b,version<159?16:1191,version);
    for(int tag:{609,132,132,219,219}) primitive(b,tag,4,4);
    stringBox(b,"1,2,4,3");
    for(int rgb:{0xee2030,0x2468df}) { primitive(b,403,0,0); b.zeros(2); b.b(255); b.b(rgb>>16); b.b(rgb>>8); b.b(rgb); }
    for(int i=0;i<3;++i) { primitive(b,138,0,i); b.d(9); b.b(0); b.str(i==0?"Clear":i==1?"Inside":"Outside"); }
    for(int i=0;i<2;++i) { primitive(b,406,0,i); b.f(8); b.zeros(2); b.b(1); }
    b.w(14);
    for(auto link:{SchematicLink{1,2,0}, {2,3,0}, {3,5,0}, {1,4,0}, {4,6,0}, {7,5,1}, {7,6,1},
                   {8,5,2}, {9,6,2}, {10,2,1}, {11,3,1}, {12,4,1}})
        wire(b,version,link.source-1,link.output,link.target-1,link.input);
    wire(b,version,12,0,0,version<159?4:1); wire(b,version,13,0,0,version<159?3:2);
    panelFooter(b,254); b.w(0); endModule(b); b.w(0); b.b(0); return b;
}

void importedViewCaches() {
    using enum PrimitiveKind;
    constexpr yellow::Color red{0xffee2030}, blue{0xff2468df}, green{0xff00dd66};
    const auto cached = [](const auto& drawings) -> yellow::CachedDrawCommand {
        for(const auto& layer:drawings) if(const auto* cache=std::get_if<yellow::CachedDrawCommand>(&layer.command)) return *cache;
        throw std::runtime_error("Expected an imported cached drawing layer");
    };
    const auto color = [](const auto& cache) { return std::get<yellow::SolidBrush>(std::get<yellow::DrawCommand>(cache.drawings->at(0).command).tool).color; };
    for(int version:{154,159,166,171,177}) {
        Session session; SchematicFile::importFlowStone(session,zip(viewCacheDocument(version)),{});
        auto& root=session.graph(); auto& graph=*root.module(1);
        const int clear=root.addNode(Trigger,0,0), inside=root.addNode(Trigger,0,0), outside=root.addNode(Trigger,0,0);
        root.connect({clear,1,0}); root.connect({inside,1,1}); root.connect({outside,1,2});
        check(graph.nodes()[1].kind==ViewCache && graph.inputType(2,1)==green::Type::Trigger && graph.outputType(2)==green::Type::View,
              "Import native View Cache with View and Clear connectors in order");
        check(graph.frontPanel()->view.mouseMoves && !graph.frontPanel()->borderVisible,
              "Current and legacy MGUI Mouse and Borderless controls retain their native meanings");
        graph.setValue(13,false); graph.setValue(14,false);
        check(!graph.frontPanel()->view.mouseMoves && graph.frontPanel()->borderVisible, "Imported MGUI control links stay live");
        auto view=graph.frontPanel()->view; const auto initial=cached(graph.renderPanel(view));
        check(initial.drawings->size()==1 && color(initial)==red, "First render captures the imported cache branch");
        graph.setValue(8,blue); graph.setValue(9,green); root.fireTrigger(outside);
        const auto reused=graph.renderPanel(view); const auto external=cached(reused);
        check(external.drawings==initial.drawings && external.revision==initial.revision && color(external)==red &&
              std::ranges::any_of(reused,[&](const auto& layer) {
                  const auto* draw=std::get_if<yellow::DrawCommand>(&layer.command);
                  return draw && std::get<yellow::SolidBrush>(draw->tool).color==green;
              }), "Sibling redraws update their branch while reusing the imported cached drawing");
        root.fireTrigger(clear);
        check(cached(graph.renderPanel(view)).drawings==external.drawings, "Native Clear waits for the next external repaint");
        root.fireTrigger(outside); const auto refreshed=cached(graph.renderPanel(view));
        check(refreshed.revision!=external.revision && color(refreshed)==blue, "A repaint following native Clear rebuilds the cached branch");
        graph.setValue(8,green); root.fireTrigger(inside); const auto internal=cached(graph.renderPanel(view));
        check(internal.revision!=refreshed.revision && color(internal)==green, "A native Redraw on the cached View route invalidates that cache");
        root.resizePanel(1,24,12); view=graph.frontPanel()->view;
        check(cached(graph.renderPanel(view)).bounds==view.bounds, "Imported View Cache follows panel resizing");
        const auto saved=SchematicFile::project(session); QTemporaryDir dir; const auto target=dir.filePath("cached-gui.ostone");
        SchematicFile::write(target,saved); Session reopened; SchematicFile::loadProject(reopened,SchematicFile::read(target),{});
        check(SchematicFile::project(reopened)==saved, "Native cache routing, MGUI controls and redraw trigger boundaries survive disk save/reopen");
        auto& copy=*reopened.graph().module(1); const auto reopenedView=copy.frontPanel()->view;
        check(color(cached(copy.renderPanel(reopenedView)))==green, "Reopened caches rebuild their transient drawing from the saved graph");
        copy.setValue(8,red); reopened.graph().fireTrigger(clear); reopened.graph().fireTrigger(outside);
        check(color(cached(copy.renderPanel(reopenedView)))==red, "Reopened native Clear and redraw links remain functional");
    }
    {
        Bytes b; header(b,177); beginModule(b,"Root",1); beginModule(b,"Properties controls",2);
        nativePanel(b,1192,177); primitive(b,406,0,0); b.f(8); b.zeros(2); b.b(1);
        b.w(1); wire(b,177,1,0,0,1); panelFooter(b,1); b.w(0); endModule(b); b.w(0); b.b(0);
        Session props; SchematicFile::importFlowStone(props,zip(b),{});
        auto& child=*props.graph().module(1);
        check(child.modulePanel(ModulePanelKind::Properties)->view.mouseMoves,"Current Properties GUI accepts its native Mouse input");
        child.setValue(2,false);
        const auto saved=SchematicFile::project(props); Session reopened; SchematicFile::loadProject(reopened,saved,{});
        check(!reopened.graph().module(1)->modulePanel(ModulePanelKind::Properties)->view.mouseMoves && SchematicFile::project(reopened)==saved,
              "Current Properties GUI Mouse polarity and control link survive reopening");
    }
    // Properties GUI has Mouse but no third Borderless input.
    Bytes bad; header(bad,177); beginModule(bad,"Root",2); nativePanel(bad,1192,177);
    primitive(bad,406,0,0); bad.f(8); bad.zeros(2); bad.b(1); bad.w(1); wire(bad,177,1,0,0,2);
    panelFooter(bad,1); bad.w(0); bad.b(0); expectError(zip(bad),"connector index");
}

QByteArray audioRecordDocument(int version,int tag) {
    Bytes b;header(b,version);beginModule(b,"Root",1);beginModule(b,"Audio",1);
    primitive(b,tag,2,3);b.w(0);endModule(b);b.w(0);endModule(b);b.w(0);b.b(0);return b;
}

void audioRecordRoundTrip(const QByteArray& archive) {
    using enum PrimitiveKind;
    Session session;SchematicFile::importFlowStone(session,archive,{});session.enterModule(1);auto& graph=session.graph();
    const auto kind=graph.nodes()[0].kind;const bool delay=kind==Delay || kind==DoubleDelay;
    const bool wide=kind==DoubleDelay || kind==DoubleSine;const bool pitch=kind==StreamPitchToFreq;
    check(delay || pitch || kind==Sine || kind==DoubleSine,"Expected imported audio primitive");
    const std::string signal=wide?"streamdouble":"stream",mono=wide?"monodouble":"mono";
    const auto code=[&](const std::string& source) {
        const int id=graph.addNode(DspCode,0,0);graph.setDspDraft(id,source);session.compileDsp(id);return id;
    };
    int samples=0;
    if(delay) {
        const int source=code(signal+"out x; float n; x=n==0 ? "+
            (wide?"1.0000000000009094947017729282379150390625":"1")+" : 0; n+=1;");
        samples=graph.addNode(Int,0,0,std::int32_t{0});graph.connect({source,1,0});graph.connect({samples,1,1});
    } else if(pitch) {
        const int source=code("streamout pitch;float n;pitch=n==0 ? 57 : n==1 ? 69 : n==2 ? 81 : 69.5;n=n==3 ? 0 : n+1;");
        graph.connect({source,1,0});
    } else {
        const int source=code(signal+"out freq;"+signal+R"(out phase;streamboolout sync;float n;
            freq=n<12 ? .25 : n==13 ? 1 : .5;
            phase=n<3 ? .125 : n<6 ? .375 : n<8 ? 1.125 : .125;
        )"+(wide?"phase+=0.0000000000009094947017729282379150390625;":"")+R"(
            sync=n<3 ? false : n<6 ? true : n<8 ? false : n<10;
            n+=1;
        )");
        for(int port=0;port<3;++port)graph.connect({source,1,port,port});
    }
    const int tap=code(mono+"in x;"+mono+"out y;y=x;");graph.connect({1,tap,0});session.navigate(0);
    const auto verify=[&](Schematic& root) {
        auto& g=*root.module(1);const auto expectedType=wide?green::Type::MonoDouble:green::Type::Mono;
        check(g.inputType(1,0)==expectedType && g.outputType(1)==expectedType,
              "Imported audio primitive specializes its signal ports at the expected precision");
        if(delay) {
            check(g.inputType(1,1)==green::Type::Int,"Native Delay sample count is a Green Integer");
            for(int length:{-7,0,1,5,17,33}) {
                g.setValue(samples,std::int32_t{length});std::vector<double> expected(64);
                expected[std::max(1,length)]=wide?1.0+0x1p-40:1.0;
                for(std::size_t block:{1,3,7,64})
                    check(g.captureDsp(tap,64,block).front()==expected,
                          "Imported Delay preserves the exact impulse and minimum one-sample delay across buffer sizes");
            }
        } else if(pitch) {
            for(double rate:{3200.,44100.,48000.,96000.}) {
                root.setSampleRate(rate);const auto baseline=g.captureDsp(tap,16,1).front();
                constexpr std::array pitches{57.,69.,81.,69.5};
                for(int n=0;n<16;++n) {
                    const double expected=880*std::exp2((pitches[n%4]-69)/12)/rate;
                    check(std::abs(baseline[n]-expected)<std::abs(expected)*5e-7,
                          "Imported Pitch to Freq uses Nyquist-normalized frequency at the current sample rate");
                }
                for(std::size_t block:{2,3,7,16})check(g.captureDsp(tap,16,block).front()==baseline,
                    "Imported pitch conversion has identical samples across buffer sizes");
            }
        } else {
            check(g.inputType(1,1)==expectedType && g.inputType(1,2)==green::Type::MonoBoolean,
                  "Native Sine retains Phase and specializes its Boolean Sync port");
            constexpr std::array phases{.125,.25,.375,.375,.5,.625,.5,.625,.125,.25,.375,.5,.625,.875,.375};
            const auto baseline=g.captureDsp(tap,phases.size(),1).front();
            for(std::size_t n=0;n<phases.size();++n) {
                const double expected=std::sin(2*std::acos(-1.)*(phases[n]+(wide?0x1p-40:0)));
                check(std::abs(baseline[n]-expected)<(wide?2e-14:2e-6),
                      "Imported Sine retains phase offset, wrapping, rising-edge sync and double precision");
            }
            for(std::size_t block:{2,3,5,7,15})check(g.captureDsp(tap,phases.size(),block).front()==baseline,
                "Imported oscillator phase and sync state carry across buffer boundaries");
        }
    };
    verify(session.graph());const auto saved=SchematicFile::project(session);QTemporaryDir dir;
    const auto target=dir.filePath("audio-record.ostone");SchematicFile::write(target,saved);
    Session reopened;SchematicFile::loadProject(reopened,SchematicFile::read(target),{});
    check(SchematicFile::project(reopened)==saved,"Imported audio types, controls, code and links survive disk save/reopen");
    verify(reopened.graph());
}

void importedAudioRecords() {
    for(int version:{154,159,166,171,177}) for(int tag:{33,35,77,1141,1148})audioRecordRoundTrip(zip(audioRecordDocument(version,tag)));
    auto b=audioRecordDocument(177,33);const auto pos=b.indexOf(QByteArray::fromHex("21000200030000"));
    check(pos>0,"Locate Sine fixture");expectError(zip(b.left(pos+6)),"Truncated");
    b[pos]=char(0xfe);b[pos+1]=char(0xff);expectError(zip(b),"Unsupported component ID");
    Bytes bad;header(bad,177);beginModule(bad,"Root",1);beginModule(bad,"Bad ports",2);
    primitive(bad,33,0,0);primitive(bad,29,0,0);bad.f(8);bad.zeros(2);bad.d(0);
    bad.w(1);wire(bad,177,1,0,0,3);endModule(bad);bad.w(0);endModule(bad);bad.w(0);bad.b(0);
    expectError(zip(bad),"connector index");
}

void nativeAudioRecord(const QString& path) {
    audioRecordRoundTrip(SchematicFile::read(path));
    std::cout<<path.toStdString()<<": unchanged audio record, buffer-independent rendering and disk save/reopen passed\n";
}

void nativeEchoModule(const QString& path) {
    using enum PrimitiveKind;
    Session session;SchematicFile::importFlowStone(session,SchematicFile::read(path),{});auto& graph=session.graph();
    check(graph.module(1)->nodes().size()==10 && graph.module(1)->module(3)->nodes()[0].kind==Delay,
          "Complete native Echo module keeps its nested Delay and original feedback wiring");
    const auto input=[&](const char* name) {
        const auto& ports=graph.nodes()[0].moduleInputs;
        for(int i=0;i<int(ports.size());++i)if(ports[i].name==name)return i;
        throw std::runtime_error(std::string("Missing Echo input: ")+name);
    };
    const auto code=[&](const std::string& source) {
        const int id=graph.addNode(DspCode,0,0);graph.setDspDraft(id,source);session.compileDsp(id);return id;
    };
    const int source=code("monoout x;float n;x=n==0 ? 1 : 0;n+=1;");
    const int tap=code("monoin x;monoout y;y=x;");
    const int seconds=graph.addNode(Float,0,0,0.f),feed=graph.addNode(Float,0,0,.5f),level=graph.addNode(Float,0,0,.75f);
    const auto& ports=graph.nodes()[0].moduleInputs;int signal=-1;
    for(int i=0;i<int(ports.size());++i)if(ports[i].type==green::Type::Mono)signal=i;
    check(signal>=0,"Native Echo retains its Mono input boundary");
    graph.connect({source,1,signal});graph.connect({seconds,1,input("Delay (secs)")});
    graph.connect({feed,1,input("Feed")});graph.connect({level,1,input("Level")});graph.connect({1,tap,0});
    const auto verify=[&](Schematic& root) {
        for(double rate:{3200.,48000.})for(int delay:{5,17})for(float feedback:{.5f,-.5f}) {
            root.setSampleRate(rate);root.setValue(seconds,float(delay/rate));root.setValue(feed,feedback);
            std::vector<double> expected(65);expected[0]=1;float echo=.75f;
            for(int n=delay;n<65;n+=delay){expected[n]=echo;echo*=feedback;}
            for(std::size_t block:{1,3,7,32,65})check(root.captureDsp(tap,65,block).front()==expected,
                "Unchanged Echo converts seconds to samples and renders dry signal plus signed feedback echoes across buffer sizes");
        }
    };
    verify(graph);const auto saved=SchematicFile::project(session);QTemporaryDir dir;const auto target=dir.filePath("echo.ostone");
    SchematicFile::write(target,saved);Session reopened;SchematicFile::loadProject(reopened,SchematicFile::read(target),{});
    check(SchematicFile::project(reopened)==saved,"Complete native Echo feedback graph survives disk save/reopen");verify(reopened.graph());
    std::cout<<"Echo Delay Echo: unchanged nested module, seconds/sample-rate controls, signed feedback and disk save/reopen passed\n";
}

void nativeFmDemodulator(const QString& path) {
    using enum PrimitiveKind;
    Session session;SchematicFile::importFlowStone(session,SchematicFile::read(path),{});auto& graph=session.graph();
    check(graph.module(1)->nodes().size()==9 && graph.module(1)->nodes()[0].kind==Delay &&
          graph.module(1)->readValue(7)==green::Value{std::int32_t{2}},
          "Complete native FM De-modulator retains its Delay, multipliers and stored gain");
    const auto code=[&](const std::string& source) {
        const int id=graph.addNode(DspCode,0,0);graph.setDspDraft(id,source);session.compileDsp(id);return id;
    };
    const int source=code("monoout x;float n;x=n==0 ? .5 : n==1 ? -.25 : n==2 ? .75 : -1;n=n==3 ? 0 : n+1;");
    const int tap=code("monoin x;monoout y;y=x;"),length=graph.addNode(Int,0,0,std::int32_t{0});
    check(graph.nodes()[0].moduleInputs.size()==2 && graph.nodes()[0].moduleInputs[1].name=="Phase Delay",
          "Native FM input and Phase Delay keep their boundary order");
    graph.connect({source,1,0});graph.connect({length,1,1});graph.connect({1,tap,0});
    const auto verify=[&](Schematic& root) {
        constexpr std::array pattern{.5,-.25,.75,-1.};
        for(int requested:{0,1,3,7,17}) {
            root.setValue(length,std::int32_t{requested});const int delay=std::max(1,requested);
            std::vector<double> expected(65);
            for(int n=delay;n<65;++n)expected[n]=2*pattern[n%4]*pattern[(n-delay)%4];
            for(std::size_t block:{1,3,7,32,65})check(root.captureDsp(tap,65,block).front()==expected,
                "Unchanged FM De-modulator multiplies each sample by its delayed sample and stored gain across buffer sizes");
        }
    };
    verify(graph);const auto saved=SchematicFile::project(session);QTemporaryDir dir;const auto target=dir.filePath("fm-demodulator.ostone");
    SchematicFile::write(target,saved);Session reopened;SchematicFile::loadProject(reopened,SchematicFile::read(target),{});
    check(SchematicFile::project(reopened)==saved,"Complete FM De-modulator preserves original nodes, controls and links through disk save/reopen");
    verify(reopened.graph());
    std::cout<<"FM De-modulator: complete unchanged module, delayed sample products and disk save/reopen passed\n";
}

QByteArray stateRecordDocument(int version,int tag,std::int32_t value=0,bool properties=false,bool layout=false) {
    Bytes b;header(b,version);beginModule(b,"Root",1);beginModule(b,"State",1);primitive(b,tag,0,0);
    if(tag==326) b.d(std::bit_cast<quint32>(value));
    if(tag==408) {
        b.f(12);b.f(8);b.d(std::bit_cast<quint32>(value));b.b(properties);b.b(layout);
        if(layout) {
            b.d(1);b.zeros(24);
            if(version>=157) {b.zeros(4);b.b(1);b.zeros(16);}
            b.d(1);b.w(0);
        }
    }
    b.w(0);endModule(b);b.w(0);endModule(b);b.w(0);b.b(0);return b;
}

void indexSelectorRoundTrip(const QByteArray& archive) {
    using enum PrimitiveKind;
    Session session;SchematicFile::importFlowStone(session,archive,{});auto& graph=*session.graph().module(1);
    check(graph.nodes()[0].kind==IndexSelector && graph.inputType(1,0)==green::Type::String &&
          graph.inputType(1,1)==green::Type::Boolean && graph.inputType(1,2)==green::Type::String &&
          graph.outputType(1,0)==green::Type::Int && graph.outputType(1,1)==green::Type::String,
          "Imported Index Selector retains its List/Sort/Set and Index/Text connector order");
    const int list=graph.addNode(MultilineString,0,0,std::string{"Zebra,Apricot\nMango"});
    const int sort=graph.addNode(Boolean,0,0,false),set=graph.addNode(String,0,0,std::string{});
    const int pulse=graph.addNode(Trigger,0,0);
    for(auto link:{SchematicLink{list,1,0},SchematicLink{sort,1,1},SchematicLink{set,1,2},SchematicLink{pulse,1,2}})
        graph.connect(link,LinkNotification::Silent);
    const auto exercise=[&](Schematic& root) {
        auto& g=*root.module(1);g.setValue(set,std::string{});g.setValue(sort,false);
        g.setValue(list,std::string{"Zebra,Apricot\nMango"});
        const auto selected=[&](int index,const char* text) {
            check(g.readValue(1)==green::Value{std::int32_t{index}} && g.readValue(1,1)==green::Value{std::string{text}},
                  "Imported Index Selector publishes the selected index and matching text");
        };
        g.setValue(set,std::string{"Mango"});selected(2,"Mango");
        const auto before=g.inspectOutput(1).triggers, textBefore=g.inspectOutput(1,1).triggers;
        g.fireTrigger(pulse);g.setValue(set,std::string{"missing"});
        check(g.inspectOutput(1).triggers==before && g.inspectOutput(1,1).triggers==textBefore,
              "Repeated or missing Index Selector Set values do not publish selection events");
        g.setValue(set,std::string{"Mango"});g.setValue(sort,true);selected(1,"Mango");
        check(g.openIndexSelector(1,{14,20}) && root.pendingDropList()->checkedIndex==1 &&
              root.pendingDropList()->contents.options==std::vector<std::string>{"Apricot","Mango","Zebra"},
              "Imported Index Selector opens the sorted node menu with the active option checked");
        const auto indexBefore=g.inspectOutput(1).triggers, menuTextBefore=g.inspectOutput(1,1).triggers;
        root.finishDropList(root.pendingDropList()->token,2);selected(2,"Zebra");
        check(g.inspectOutput(1).triggers==indexBefore+1 && g.inspectOutput(1,1).triggers==menuTextBefore+1,
              "A menu selection sends one event on each imported Index Selector output");
        check(g.openIndexSelector(1,{}),"Reopen native Index Selector menu");
        root.finishDropList(root.pendingDropList()->token,std::nullopt);selected(2,"Zebra");
        check(g.inspectOutput(1).triggers==indexBefore+1,"Canceling the imported menu sends no selection event");
    };
    exercise(session.graph());const auto saved=SchematicFile::project(session);QTemporaryDir dir;
    const auto target=dir.filePath("index-selector.ostone");SchematicFile::write(target,saved);
    Session reopened;SchematicFile::loadProject(reopened,SchematicFile::read(target),{});
    check(SchematicFile::project(reopened)==saved && reopened.graph().module(1)->readValue(1)==green::Value{std::int32_t{2}},
          "Index Selector's chosen row, connector types and links survive disk save/reopen");
    exercise(reopened.graph());
}

void counterRoundTrip(const QByteArray& archive,std::int32_t initial) {
    using enum PrimitiveKind;
    Session session;SchematicFile::importFlowStone(session,archive,{});auto& graph=*session.graph().module(1);
    check(graph.nodes()[0].kind==BasicCounter && graph.readValue(1)==green::Value{initial},"Native Counter restores its signed saved count");
    const auto saved=SchematicFile::project(session);QTemporaryDir dir;const auto target=dir.filePath("counter.ostone");
    SchematicFile::write(target,saved);Session reopened;SchematicFile::loadProject(reopened,SchematicFile::read(target),{});
    check(SchematicFile::project(reopened)==saved && reopened.graph().module(1)->readValue(1)==green::Value{initial},
          "Counter's signed count survives disk save/reopen before any input event");
    for(auto* root:{&session.graph(),&reopened.graph()}) {
        auto& g=*root->module(1);const int step=g.addNode(Trigger,0,0),reset=g.addNode(Trigger,0,0);
        g.connect({step,1,0},LinkNotification::Silent);g.connect({reset,1,1},LinkNotification::Silent);
        auto expected=green::addWrappedInts(initial,1);if(expected<0)expected=0;
        g.fireTrigger(step);
        check(g.readValue(1)==green::Value{expected} && g.inspectOutput(1).triggers==1,
              "Native Counter increments, resetting a negative wrapped result to zero and publishing once");
        g.fireTrigger(reset);g.fireTrigger(reset);
        check(g.readValue(1)==green::Value{std::int32_t{0}} && g.inspectOutput(1).triggers==3,
              "Each Counter Reset event publishes zero, including repeated resets");
    }
}

void afterLoadRoundTrip(const QByteArray& archive) {
    Session session;SchematicFile::importFlowStone(session,archive,{});
    const auto verify=[](const Session& s) {
        const auto& g=*s.graph().module(1);
        check(g.nodes()[0].kind==PrimitiveKind::AfterLoad && g.inspectOutput(1).triggers==1,
              "Imported After Load emits exactly once when its schematic opens");
    };
    verify(session);const auto saved=SchematicFile::project(session);verify(session);
    QTemporaryDir dir;const auto target=dir.filePath("after-load.ostone");SchematicFile::write(target,saved);
    Session reopened;SchematicFile::loadProject(reopened,SchematicFile::read(target),{});verify(reopened);
    check(SchematicFile::project(reopened)==saved,"After Load and its graph survive disk save/reopen");verify(reopened);
}

void importedStateRecords() {
    for(int version:{154,159,166,171,177}) {
        for(bool layout:{false,true}) for(bool properties:{false,true}) for(int selected:{-1,0,1,42}) {
            const auto data=stateRecordDocument(version,408,selected,properties,layout);
            const auto parsed=FlowStoneFile::decode(zip(data));
            check(parsed.root.children[0].children[0].node.localValue==green::Value{std::int32_t{selected}},
                  "Index Selector reads its selection and optional property-box layout without losing following records");
            indexSelectorRoundTrip(zip(data));
        }
        for(auto initial:{std::int32_t{0},std::int32_t{41},std::int32_t{-1},std::numeric_limits<std::int32_t>::min(),std::numeric_limits<std::int32_t>::max()})
            counterRoundTrip(zip(stateRecordDocument(version,326,initial)),initial);
        afterLoadRoundTrip(zip(stateRecordDocument(version,258)));
    }
    Session copied;SchematicFile::importFlowStone(copied,zip(stateRecordDocument(177,326,41)),{});
    check(copied.enterModule(1),"Enter imported Counter module");
    const auto copies=copied.duplicateNodes(std::array{1},16,16);
    check(copies.size()==1 && copied.graph().readValue(1)==green::Value{std::int32_t{41}} &&
          copied.graph().readValue(copies[0])==green::Value{std::int32_t{0}},
          "Saving Counter values does not change the existing duplication reset policy");
    Session rootCounter;const int counter=rootCounter.graph().addNode(PrimitiveKind::BasicCounter,0,0,std::int32_t{41});
    Session rootCopy;SchematicFile::loadProject(rootCopy,SchematicFile::project(rootCounter),{});
    check(rootCopy.graph().readValue(counter)==green::Value{std::int32_t{41}},"Project saves preserve Counter values at the root too");
    const auto data=stateRecordDocument(177,408);const auto pos=data.indexOf(QByteArray::fromHex("98010000000000"));
    check(pos>0,"Locate Index Selector fixture");
    auto bad=data;qToLittleEndian<quint32>(0,bad.data()+pos+7);expectError(zip(bad),"Index Selector size");
    bad=data;qToLittleEndian<quint32>(0x7fc00000,bad.data()+pos+7);expectError(zip(bad),"Index Selector width");
    bad=data;qToLittleEndian<quint32>(0xfffffffe,bad.data()+pos+15);expectError(zip(bad),"Index Selector selection");
    bad=data;bad[pos+19]=2;expectError(zip(bad),"Invalid Boolean");
    bad=data;bad[pos+20]=2;expectError(zip(bad),"Invalid Boolean");
    expectError(zip(data.left(pos+20)),"Truncated");
    bad=stateRecordDocument(177,408,0,false,true);qToLittleEndian<quint32>(4097,bad.data()+pos+21);expectError(zip(bad),"record count");
}

void nativeStateRecord(const QString& path) {
    const auto archive=SchematicFile::read(path);const auto parsed=FlowStoneFile::decode(archive);
    const auto& node=parsed.root.children[0].children[0].node;
    if(node.kind==PrimitiveKind::IndexSelector) indexSelectorRoundTrip(archive);
    else if(node.kind==PrimitiveKind::BasicCounter)counterRoundTrip(archive,std::get<std::int32_t>(*node.localValue));
    else if(node.kind==PrimitiveKind::AfterLoad)afterLoadRoundTrip(archive);
    else throw std::runtime_error("Unexpected bundled state primitive");
    std::cout<<path.toStdString()<<": unchanged native state record, runtime behavior and disk save/reopen passed\n";
}

void nativeStartupCounter(const QString& path) {
    Session session;SchematicFile::importFlowStone(session,SchematicFile::read(path),{});
    const auto verify=[](const Session& s,int expected) {
        const auto& g=*s.graph().module(1);
        check(g.nodes().size()==2 && g.nodes()[0].kind==PrimitiveKind::AfterLoad && g.nodes()[1].kind==PrimitiveKind::BasicCounter &&
              g.readValue(2)==green::Value{std::int32_t{expected}} && g.inspectOutput(1).triggers==1 && g.inspectInput(2,0).triggers==1,
              "Original After Load and Counter records deliver one startup event across their converted link");
    };
    verify(session,1);const auto saved=SchematicFile::project(session);verify(session,1);
    QTemporaryDir dir;const auto target=dir.filePath("startup-counter.ostone");SchematicFile::write(target,saved);
    Session reopened;SchematicFile::loadProject(reopened,SchematicFile::read(target),{});verify(reopened,2);
    static_cast<void>(SchematicFile::project(reopened));verify(reopened,2);
    std::cout<<"Bundled After Load/Counter: one event per open and saved count restored before startup event passed\n";
}

QByteArray routingDocument(int version, int tag, int valueType, bool selected = false) {
    Bytes b; header(b,version); beginModule(b,"Root",1); beginModule(b,"Routing",1);
    primitive(b,tag,0,0);
    b.d(tag==1057?3:tag==1060?2:1); b.d(valueType);
    if(tag==1057) b.d(valueType);
    if(tag!=1056) b.d(15);
    b.d(1); b.d(valueType);
    b.d(tag==1057?2:tag==1056?1:0);
    if(tag!=1060) b.d(0);
    if(tag==1057) b.d(1);
    b.d(0);
    if(tag==1057) b.b(selected);
    b.w(0); endModule(b); b.w(0); endModule(b); b.w(0); b.b(0); return b;
}

void routingRoundTrip(const QByteArray& archive) {
    using enum PrimitiveKind;
    Session session; SchematicFile::importFlowStone(session,archive,{});
    auto& graph=*session.graph().module(1);
    const auto kind=graph.nodes()[0].kind;
    const auto type=graph.outputType(1).value_or(green::Type::Float);
    PrimitiveKind source=Float; green::Value first=1.25f,second=2.5f;
    switch(type) {
    case green::Type::Float: break;
    case green::Type::Int: source=Int; first=std::int32_t{12}; second=std::int32_t{25}; break;
    case green::Type::String: source=String; first=std::string{"first"}; second=std::string{"second"}; break;
    case green::Type::Boolean: source=Boolean; first=true; second=false; break;
    case green::Type::FloatArray: source=MultilineString; first=std::string{"1\n2"}; second=std::string{"3\n4"}; break;
    case green::Type::Trigger: source=Trigger; break;
    default: throw std::runtime_error("Unexpected routing fixture type");
    }
    const auto expectedFirst=type==green::Type::FloatArray?green::Value{green::FloatArray{1,2}}:first;
    const auto expectedSecond=type==green::Type::FloatArray?green::Value{green::FloatArray{3,4}}:second;
    const int a=graph.addNode(source,0,0,source==Trigger?std::nullopt:std::optional{first});
    const int b=graph.addNode(source,0,0,source==Trigger?std::nullopt:std::optional{second});
    const int condition=graph.addNode(Boolean,0,0,false);
    const int pulseA=graph.addNode(Trigger,0,0),pulseB=graph.addNode(Trigger,0,0),pulseCondition=graph.addNode(Trigger,0,0);
    graph.connect({a,1,0},LinkNotification::Silent); graph.connect({pulseA,1,0},LinkNotification::Silent);
    if(kind==Select) {
        graph.connect({b,1,1},LinkNotification::Silent); graph.connect({pulseB,1,1},LinkNotification::Silent);
    }
    if(kind!=TriggerBlocker) {
        const int control=kind==Select?2:1;
        graph.connect({condition,1,control},LinkNotification::Silent);
        graph.connect({pulseCondition,1,control},LinkNotification::Silent);
    }
    const auto exercise=[&](Schematic& g) {
        check(g.outputType(1)==type,"Imported routing data type stays independent of fixed Boolean controls");
        const auto pulses=[&] {return g.inspectOutput(1).triggers;};
        const auto delta=[&](int trigger,std::uint64_t expected) {
            const auto before=pulses(); g.fireTrigger(trigger);
            if(pulses()!=before+expected) throw std::runtime_error("Routing event mismatch: kind="+std::to_string(int(kind))+
                " type="+std::to_string(int(type))+" trigger="+std::to_string(trigger)+" expected="+std::to_string(expected)+
                " actual="+std::to_string(pulses()-before));
        };
        if(source!=Trigger) g.setValue(a,first);
        g.setValue(condition,false);
        if(kind==Select) {
            // Setting an unchanged value box is silent. The first explicit
            // condition pulse must still compare against the restored cache.
            delta(pulseCondition,g.nodes()[0].localValue && std::get<bool>(*g.nodes()[0].localValue)?1:0);
            check(g.readValue(1)==expectedFirst,"Select reads its live false branch regardless of the serialized last condition");
            delta(pulseB,0); delta(pulseA,1); delta(pulseCondition,0);
            const auto before=pulses(); g.setValue(condition,true);
            check(pulses()==before+1 && g.readValue(1)==expectedSecond,"Changing Select's condition publishes its true branch");
            delta(pulseCondition,0); delta(pulseA,0); delta(pulseB,1);
        } else if(kind==TriggerBlocker) {
            delta(pulseA,0);
            g.setValue(a,second);
            check(g.readValue(1)==expectedSecond && pulses()==0,"Trigger Blocker exposes current values without sending events");
        } else {
            delta(pulseA,0); delta(pulseCondition,0);
            const auto before=pulses(); g.setValue(condition,true);
            check(pulses()==before,"Opening Trigger Switch does not publish a signal event");
            delta(pulseA,1); delta(pulseCondition,0);
            g.setValue(condition,false); delta(pulseA,0);
            if(source!=Trigger) {
                g.setValue(a,second);
                check(g.readValue(1)==expectedSecond,"A closed Trigger Switch still exposes the current data value");
            }
        }
    };
    exercise(graph);
    const auto saved=SchematicFile::project(session); QTemporaryDir dir; check(dir.isValid(),"Routing round-trip directory");
    const auto target=dir.filePath("routing.ostone"); SchematicFile::write(target,saved);
    Session reopened; SchematicFile::loadProject(reopened,SchematicFile::read(target),{});
    check(SchematicFile::project(reopened)==saved,"Native routing types, state and links survive disk save/reopen");
    exercise(*reopened.graph().module(1));
}

void importedRouting() {
    for(int version:{154,159,166,171,177}) for(int tag:{1056,1057,1060}) {
        for(int type:{0,19,1,2,3,15,10,23}) routingRoundTrip(zip(routingDocument(version,tag,type,true)));
        if(tag==1060) routingRoundTrip(zip(routingDocument(version,tag,9)));
    }
    for(bool cached:{false,true}) {
        Session session; SchematicFile::importFlowStone(session,zip(routingDocument(177,1057,2,cached)),{});
        check(session.graph().module(1)->nodes()[0].localValue==green::Value{cached},"Select restores its saved condition notification cache");
    }
    auto fixture=routingDocument(177,1057,2);
    const auto pos=fixture.indexOf(QByteArray::fromHex("21040000000000")); check(pos>0,"Locate Select fixture");
    auto bad=fixture; qToLittleEndian<quint32>(2,bad.data()+pos+19); expectError(zip(bad),"fixed generic connector type");
    bad=fixture; qToLittleEndian<quint32>(3,bad.data()+pos+27); expectError(zip(bad),"Mixed generic math");
    bad=fixture; qToLittleEndian<quint32>(3,bad.data()+pos+35); expectError(zip(bad),"locked connector");
    bad=fixture; bad[pos+47]=2; expectError(zip(bad),"Invalid Boolean");
    expectError(zip(fixture.left(pos+47)),"Truncated");
    Session session; SchematicFile::importFlowStone(session,zip(fixture),{});
    auto saved=QJsonDocument::fromJson(SchematicFile::project(session)).object(); auto root=saved["root"].toObject();
    auto nodes=root["nodes"].toArray(); auto module=nodes[0].toObject(); auto children=module["nodes"].toArray();
    auto select=children[0].toObject(); select["value"]=QJsonObject{{"type",int(green::Type::Int)},{"data",7}};
    children[0]=select;module["nodes"]=children;nodes[0]=module;root["nodes"]=nodes;saved["root"]=root;
    const auto original=SchematicFile::project(session); bool rejected=false,touched=false;
    try { SchematicFile::loadProject(session,QJsonDocument(saved).toJson(),[&]{touched=true;}); }
    catch(const std::exception& e) {rejected=std::string(e.what()).find("Select condition state")!=std::string::npos;}
    check(rejected && !touched && SchematicFile::project(session)==original,"Invalid saved Select cache rejects before replacing the project");
}

void nativeRoutingRecord(const QString& path) {
    routingRoundTrip(SchematicFile::read(path));
    std::cout<<path.toStdString()<<": unchanged routing record, event behavior and disk save/reopen passed\n";
}

void nativeStereoSplit(const QString& path) {
    using enum PrimitiveKind;
    Session session; SchematicFile::importFlowStone(session,SchematicFile::read(path),{});
    check(session.graph().module(1)->nodes().size()==13 && session.graph().module(1)->nodes()[6].kind==Select,
          "Import all original Reverb Left/Right split nodes");
    const auto& outputs=session.graph().nodes()[0].moduleOutputs;
    check(outputs.size()==2 && outputs[0].name=="left" && outputs[1].name=="right",
          "Native output order follows connector positions, not the serialized child order");
    const int balance=session.graph().addNode(Float,0,0,0.5f);session.graph().connect({balance,1,0});
    const auto verify=[&](Schematic& root) {
        for(float value:{0.0f,0.25f,0.5f,0.75f,1.0f}) {
            root.setValue(balance,value);
            const auto left=std::get<float>(root.readValue(1,0)),right=std::get<float>(root.readValue(1,1));
            if(right!=std::min(2*value,1.0f) || left!=std::min(2-2*value,1.0f))
                throw std::runtime_error("Native Reverb split mismatch: balance="+std::to_string(value)+
                    " left="+std::to_string(left)+" right="+std::to_string(right));
        }
    };
    verify(session.graph()); const auto saved=SchematicFile::project(session); QTemporaryDir dir;
    const auto target=dir.filePath("stereo-split.ostone");SchematicFile::write(target,saved);
    Session reopened;SchematicFile::loadProject(reopened,SchematicFile::read(target),{});
    check(SchematicFile::project(reopened)==saved,"Native stereo split graph and saved Select cache survive disk save/reopen");
    verify(reopened.graph());
    std::cout<<"Reverb Left/Right split: unchanged module, live gain routing and disk save/reopen passed\n";
}

QByteArray comparisonsDocument(int version, int operandType) {
    Bytes b; header(b,version); beginModule(b,"Root",1); beginModule(b,"Comparisons",12);
    for(int i=0;i<2;++i) { primitive(b,138,0,i,i?"B":"A"); b.d(operandType); b.b(0); b.str(""); }
    for(int tag:{1048,1049,1050,1051,1052,622,623,624,625,626}) {
        primitive(b,tag,5,4); b.d(2); b.d(operandType); b.d(operandType); b.d(1); b.d(tag>=1048?15:9);
        b.d(2); b.d(0); b.d(1); b.d(0);
    }
    b.w(20);
    for(int node=2;node<12;++node) { wire(b,version,0,0,node,0); wire(b,version,1,0,node,1); }
    endModule(b); b.w(0); endModule(b); b.w(0); b.b(0); return b;
}

void importedComparisons() {
    using enum PrimitiveKind;
    const auto expected=[](int order) { return std::array{order==0,order<0,order<=0,order>0,order>=0}; };
    const auto verify=[](Session& session,int pulse,const std::array<bool,5>& expected) {
        auto& graph=*session.graph().module(1); std::array<std::uint64_t,10> before;
        for(int i=0;i<10;++i) before[i]=graph.inspectOutput(i+3).triggers;
        session.graph().fireTrigger(pulse);
        for(int i=0;i<5;++i) {
            check(graph.outputType(i+3)==green::Type::Boolean && graph.outputType(i+8)==green::Type::Trigger,
                  "Native comparison result types stay Boolean or Trigger independently of operand types");
            check(std::get<bool>(graph.readValue(i+3))==expected[i],"Imported comparison ID selects the correct predicate");
            check(graph.inspectOutput(i+3).triggers==before[i]+1 &&
                  graph.inspectOutput(i+8).triggers==before[i+5]+(expected[i]?1:0),
                  "Boolean comparisons publish each pulse; trigger comparisons pass only when their predicate is true");
        }
    };
    for(int version:{154,159,166,171,177}) for(int type:{1,2,3,10,23,0,19}) {
        Session session; SchematicFile::importFlowStone(session,zip(comparisonsDocument(version,type)),{});
        auto& root=session.graph(); auto& graph=*root.module(1); const bool automatic=type==0 || type==19;
        const auto sourceKind=automatic?Int:String;
        const int a=root.addNode(sourceKind,0,0,automatic?green::Value{std::int32_t{1}}:green::Value{std::string{"1"}});
        const int b=root.addNode(sourceKind,0,0,automatic?green::Value{std::int32_t{2}}:green::Value{std::string{"2"}});
        const int pulseA=root.addNode(Trigger,0,0), pulseB=root.addNode(Trigger,0,0);
        root.connect({a,1,0}); root.connect({b,1,1}); root.connect({pulseA,1,0}); root.connect({pulseB,1,1});
        for(int i=2;i<12;++i) check(graph.nodes()[i].compareArrayElements,"Imported comparisons retain the native paired-array rule");
        verify(session,pulseA,expected(-1)); verify(session,pulseB,expected(-1));
        if(type==10 || type==23) {
            for(const auto& [left,right,result]:std::vector<std::tuple<std::string,std::string,std::array<bool,5>>>{
                    {"1\n2","2\n3",{false,true,true,false,false}},
                    {"1\n2","1\n2",{true,false,true,false,true}},
                    {"1\n9","2\n3",{false,false,false,false,false}},
                    {"1\n2","1\n3",{false,false,true,false,false}},
                    {"1\n2","1\n2\n3",{false,false,false,false,false}},
                    {"2\n3","1\n2",{false,false,false,true,true}},
                    {"nan","0",{false,true,true,true,true}},
                    {"0","nan",{false,true,true,true,true}},
                    {"nan","nan",{false,true,true,true,true}},
                    {"nan\n1","0\n2",{false,true,true,false,false}},
                    {"nan\n3","0\n2",{false,false,false,true,true}},
                    {"-0\n0","0\n-0",{true,false,true,false,true}},
                    {"","",{true,true,true,true,true}}}) {
                root.setValue(a,left); root.setValue(b,right); verify(session,pulseA,result);
            }
            root.setValue(a,std::string{"nan\n1"}); root.setValue(b,std::string{"0\n2"});
            verify(session,pulseA,{false,true,true,false,false});
        } else if(automatic) {
            root.setValue(a,std::int32_t{2}); verify(session,pulseA,expected(0));
            root.setValue(a,std::int32_t{3}); verify(session,pulseB,expected(1));
        } else {
            for(const auto& [left,right,order]:std::vector<std::tuple<std::string,std::string,int>>{
                    {"-3","10",-1},{"2","2",0},{"9","3",1}}) {
                root.setValue(a,left); root.setValue(b,right); verify(session,pulseA,expected(order));
            }
            if(type==1) { root.setValue(a,std::string{"Z"}); root.setValue(b,std::string{"a"}); verify(session,pulseA,expected(-1)); }
            if(type==2) {
                root.setValue(a,std::string{"1.25"}); root.setValue(b,std::string{"1.5"}); verify(session,pulseA,expected(-1));
                root.setValue(a,std::string{"nan"}); verify(session,pulseA,{false,false,false,false,false});
                root.setValue(a,std::string{"-0"}); root.setValue(b,std::string{"0"}); verify(session,pulseB,expected(0));
            }
        }
        std::array<bool,5> savedResult;
        for(int i=0;i<5;++i) savedResult[i]=std::get<bool>(graph.readValue(i+3));
        const auto saved=SchematicFile::project(session); QTemporaryDir dir; const auto target=dir.filePath("comparisons.ostone");
        SchematicFile::write(target,saved); Session reopened; SchematicFile::loadProject(reopened,SchematicFile::read(target),{});
        check(SchematicFile::project(reopened)==saved,"Comparison types, paired-array behavior and trigger links survive disk save/reopen");
        verify(reopened,pulseA,savedResult);
        if(version==177 && type==23) {
            const auto copies=reopened.duplicateNodes(std::array{1},24,24);
            check(copies.size()==1 && reopened.graph().module(copies[0])->nodes()[2].compareArrayElements,
                  "Duplicating an imported comparison module preserves its array rule");
            auto& copy=*reopened.graph().module(copies[0]);
            const int left=copy.addNode(String,0,0,std::string{"1\n9"}),right=copy.addNode(String,0,0,std::string{"2\n3"});
            copy.connect({left,4,0}); copy.connect({right,4,1});
            check(!std::get<bool>(copy.readValue(4)),"Copied native Less Than still requires every array pair to satisfy the comparison");
        }
    }
    // The same saved node without the optional mode retains OpenStone's normal
    // lexicographic array ordering, including when reading earlier projects.
    Session native; SchematicFile::importFlowStone(native,zip(comparisonsDocument(177,23)),{});
    auto& root=native.graph(); const int a=root.addNode(String,0,0,std::string{"1\n9"}),b=root.addNode(String,0,0,std::string{"2\n3"});
    root.connect({a,1,0});root.connect({b,1,1});
    auto object=QJsonDocument::fromJson(SchematicFile::project(native)).object(); auto document=object["root"].toObject();
    auto nodes=document["nodes"].toArray(); auto module=nodes[0].toObject(); auto children=module["nodes"].toArray();
    for(int i=2;i<12;++i) { auto child=children[i].toObject();child.remove("compareArrayElements");children[i]=child; }
    module["nodes"]=children;nodes[0]=module;document["nodes"]=nodes;object["root"]=document;
    Session normal;SchematicFile::loadProject(normal,QJsonDocument(object).toJson(),{});
    check(std::get<bool>(normal.graph().module(1)->readValue(4)),"Earlier OpenStone comparisons keep their existing array ordering");
    const auto original=SchematicFile::project(normal);
    for(bool badType:{false,true}) {
        auto changed=object;auto doc=changed["root"].toObject();auto list=doc["nodes"].toArray();auto m=list[0].toObject();auto cs=m["nodes"].toArray();
        const int index=badType?2:0;auto node=cs[index].toObject();node["compareArrayElements"]=badType?QJsonValue{7}:QJsonValue{true};cs[index]=node;
        m["nodes"]=cs;list[0]=m;doc["nodes"]=list;changed["root"]=doc;
        bool rejected=false,touched=false;
        try{SchematicFile::loadProject(normal,QJsonDocument(changed).toJson(),[&]{touched=true;});}catch(const std::exception&){rejected=true;}
        check(rejected && !touched && SchematicFile::project(normal)==original,"Invalid saved comparison modes are rejected before replacing the open project");
    }
    const auto fixture=comparisonsDocument(177,3); const auto record=fixture.indexOf(QByteArray::fromHex("18040500040000"));
    check(record>0,"Find comparison fixture record");
    auto bad=fixture;qToLittleEndian<quint32>(3,bad.data()+record+23);expectError(zip(bad),"comparison output");
    bad=fixture;qToLittleEndian<quint32>(1,bad.data()+record+7);expectError(zip(bad),"connector count");
    bad=fixture;qToLittleEndian<quint32>(1,bad.data()+record+15);expectError(zip(bad),"Mixed generic math");
    bad=fixture;qToLittleEndian<quint32>(2,bad.data()+record+31);expectError(zip(bad),"locked connector");
    expectError(zip(fixture.left(record+41)),"Truncated");
}

void nativeShapeRecord(const QString& path) {
    using enum PrimitiveKind;Session s;SchematicFile::importFlowStone(s,SchematicFile::read(path),{});
    const auto name=s.graph().nodes()[0].moduleName;s.enterModule(1);auto& graph=s.graph();const auto kind=graph.nodes()[0].kind;
    const bool rectangle=kind==Rect || kind==RoundRectangle;
    const int expectedX=kind==RoundRectangle?56:kind==Rect?42:kind==Arc?74:name=="Group Adornment Ellipse"?94:47;
    check(graph.nodes()[0].x==expectedX*16,"Retain the native shape's grid coordinates");
    const auto view=graph.addNode(ModuleGui,0,0),bounds=graph.addNode(String,0,0,std::string("1,2,8,6"));
    const auto color=graph.addNode(Color,0,0,yellow::Color{0x80446688}),start=graph.addNode(Float,0,0,37.f),sweep=graph.addNode(Float,0,0,-240.f),corner=graph.addNode(Float,0,0,.75f);
    graph.connect({view,1,0});graph.connect({bounds,1,1});graph.connect({color,1,2});
    if(kind==Arc){graph.connect({start,1,3});graph.connect({sweep,1,4});}else if(rectangle)graph.connect({corner,1,3});
    const auto verify=[&](Schematic& root){
        const auto* g=root.module(1);const auto commands=g->panelDrawings(std::get<yellow::View>(g->readValue(view)));
        check(commands.size()==1,"Unchanged native shape record produces a connected drawing");const auto& draw=std::get<yellow::DrawCommand>(commands[0].command);
        const auto expected=rectangle?yellow::ShapeKind::Rect:kind==Arc?yellow::ShapeKind::Arc:yellow::ShapeKind::Ellipse;
        check(draw.shape==expected && draw.area==yellow::Area{1,2,8,6} && std::get<yellow::SolidBrush>(draw.tool).color==yellow::Color{0x80446688},"Native shape retains geometry and direct Color input");
        if(kind==Arc)check(draw.startAngle==37 && draw.sweepAngle==-240,"Native Arc retains both angle connectors");
        if(rectangle)check(draw.cornerRadius==.75f,"Native rectangle retains its optional corner connector");
    };
    s.navigate(0);verify(s.graph());QTemporaryDir dir;const auto target=dir.filePath("native-shape.ostone");const auto saved=SchematicFile::project(s);SchematicFile::write(target,saved);
    Session reopened;SchematicFile::loadProject(reopened,SchematicFile::read(target),{});check(SchematicFile::project(reopened)==saved,"Native shape record round-trips unchanged");verify(reopened.graph());
    std::cout<<name<<": native shape, drawing and disk save/reopen passed\n";
}

void nativeBackgroundModule(const QString& path) {
    using enum PrimitiveKind;Session s;SchematicFile::importFlowStone(s,SchematicFile::read(path),{});
    const auto bench=s.makeModule(std::array{1});s.enterModule(bench);const auto view=s.graph().addNode(ModuleGui,0,0);s.graph().connect({view,1,0});
    const auto verify=[&](Schematic& root,yellow::Color color){
        const auto* outer=root.module(bench);const auto* bg=outer->module(1);const auto panel=std::get<yellow::View>(outer->readValue(view));const auto drawings=outer->panelDrawings(panel);
        check(outer->nodes()[0].moduleInputs.size()==1 && bg->nodes()[1].kind==Rect && drawings.size()==1,"Complete native background GUI keeps its View boundary and drawing");
        const auto& draw=std::get<yellow::DrawCommand>(drawings[0].command);
        check(draw.shape==yellow::ShapeKind::Rect && draw.area==panel.bounds && std::get<yellow::SolidBrush>(draw.tool).color==color,"Native View Area and Color links drive the imported rectangle");
    };
    s.navigate(0);verify(s.graph(),{0xff353535});s.enterModule(bench);s.enterModule(1);s.graph().setValue(4,yellow::Color{0x80123456});s.navigate(0);verify(s.graph(),{0x80123456});
    QTemporaryDir dir;const auto target=dir.filePath("native-background.ostone");const auto saved=SchematicFile::project(s);SchematicFile::write(target,saved);
    Session reopened;SchematicFile::loadProject(reopened,SchematicFile::read(target),{});check(SchematicFile::project(reopened)==saved,"Complete native background GUI survives disk conversion unchanged");verify(reopened.graph(),{0x80123456});
    std::cout<<path.toStdString()<<": complete native background GUI, live Color and save/reopen passed\n";
}

void nativeRedrawAreaRecord(const QString& path) {
    using enum PrimitiveKind;
    Session session; SchematicFile::importFlowStone(session, SchematicFile::read(path), {});
    session.enterModule(1); auto& graph = session.graph();
    check(graph.nodes()[0].kind == RedrawArea && graph.nodes()[0].x == 86 * 16,
          "Import the unchanged Redraw Area record from Multi Sample");
    const int view = graph.addNode(ModuleGui, 0, 0), area = graph.addNode(String, 0, 0, std::string{"1,2,4,3"});
    const int color = graph.addNode(Color, 0, 0, yellow::Color{0xffee2030}), rect = graph.addNode(Rect, 0, 0);
    const int pulse = graph.addNode(Trigger, 0, 0);
    for (auto wire : {SchematicLink{view,1,0}, {area,1,1}, {pulse,1,2}, {1,rect,0}, {area,rect,1}, {color,rect,2}})
        graph.connect(wire, LinkNotification::Silent);
    const auto panel = std::get<yellow::View>(graph.readValue(view));
    const auto initial = graph.renderPanel(panel);
    graph.setValue(color, yellow::Color{0xff2468df}); const auto before = graph.redrawRevision(); graph.fireTrigger(pulse);
    const auto paint = graph.renderPanel(panel);
    check(initial.size() == 1 && graph.redrawRevision() > before &&
          std::ranges::any_of(paint, [](const auto& layer) {
              return std::get<yellow::SolidBrush>(std::get<yellow::DrawCommand>(layer.command).tool).color == yellow::Color{0xff2468df};
          }), "Native Redraw Area DoIt fires through its original connector and repaints the requested region");
    session.navigate(0); const auto saved = SchematicFile::project(session); QTemporaryDir dir;
    const auto target = dir.filePath("native-redraw-area.ostone"); SchematicFile::write(target, saved);
    Session reopened; SchematicFile::loadProject(reopened, SchematicFile::read(target), {});
    check(SchematicFile::project(reopened) == saved, "Native Redraw Area and its connected bench survive disk save/reopen");
    auto& copy = *reopened.graph().module(1); copy.fireTrigger(pulse);
    check(!copy.renderPanel(std::get<yellow::View>(copy.readValue(view))).empty(), "Reopened native Redraw Area still renders");
    std::cout << "Multi Sample Redraw Area: native record, repaint trigger and save/reopen passed\n";
}

void nativeGroupLabel(const QString& path) {
    using enum PrimitiveKind;
    Session session; SchematicFile::importFlowStone(session, SchematicFile::read(path), {});
    const auto verify = [](Schematic& root, yellow::Color color) {
        auto& graph = *root.module(1); const auto panel = graph.frontPanel()->view;
        check(graph.nodes()[9].kind == RoundRectangle && graph.nodes()[21].kind == RoundRectangle &&
              graph.nodes()[29].kind == ViewSize, "Complete native label retains its rounded shapes and explicit View Size record");
        check(graph.readValue(30,0) == green::Value{panel.bounds.width} &&
              graph.readValue(30,1) == green::Value{panel.bounds.height}, "Bundled label reads its current panel dimensions");
        const auto commands = graph.panelDrawings(panel); int rectangles = 0, ellipses = 0, texts = 0;
        for (const auto& layer : commands) {
            if (const auto* shape = std::get_if<yellow::DrawCommand>(&layer.command)) {
                rectangles += shape->shape == yellow::ShapeKind::Rect; ellipses += shape->shape == yellow::ShapeKind::Ellipse;
                check(std::get<yellow::SolidBrush>(shape->tool).color == color, "Bundled Tab color reaches each native shape through its original links");
            } else if (const auto* text = std::get_if<yellow::TextCommand>(&layer.command)) {
                ++texts; check(text->area.width == panel.bounds.width && text->area.height == panel.bounds.height,
                               "Native View Size drives the label's Text Area");
            }
        }
        check(rectangles == 2 && ellipses == 1 && texts == 1, "Complete bundled label draws two rounded rectangles, an ellipse and text");
    };
    auto& label = *session.graph().module(1); const auto initial = std::get<yellow::Color>(label.readValue(17));
    verify(session.graph(), initial); label.setValue(17, yellow::Color{0x80446688});
    session.graph().resizePanel(1, 24, 12); verify(session.graph(), {0x80446688});
    const auto saved = SchematicFile::project(session); QTemporaryDir dir; const auto target = dir.filePath("group-label.ostone");
    SchematicFile::write(target, saved); Session reopened; SchematicFile::loadProject(reopened, SchematicFile::read(target), {});
    check(SchematicFile::project(reopened) == saved, "Complete Group Adornment Label survives disk conversion unchanged");
    verify(reopened.graph(), {0x80446688});
    std::cout << "Group Adornment Label: unchanged GUI, rounded shapes, live sizing and save/reopen passed\n";
}

void nativeSweepArc(const QString& path) {
    using enum PrimitiveKind;
    Session session; SchematicFile::importFlowStone(session, SchematicFile::read(path), {});
    const int bench = session.makeModule(std::array{1}); session.enterModule(bench); auto& graph = session.graph();
    const int view = graph.addNode(ModuleGui, 0, 0);
    const auto inputs = graph.nodes()[0].moduleInputs;
    for (int i = 0; i < int(inputs.size()); ++i) {
        if (inputs[i].type == green::Type::View) graph.connect({view,1,i});
        else {
            const float value = inputs[i].name == "Size" ? 10.f : inputs[i].name == "Start" ? 135.f : 270.f;
            const int source = graph.addNode(Float, 0, 0, value); graph.connect({source,1,i});
        }
    }
    const auto property = [&](const char* name, green::Value value) {
        const auto type = green::typeOf(value);
        const int source = graph.addNode(type == green::Type::Color ? Color : type == green::Type::Int ? Int : Float, 0, 0, value);
        const int send = graph.addNode(WirelessOutput, 0, 0); graph.setTemplateType(send, type); graph.setWirelessLabel(send, name);
        graph.connect({source,send,0}); return source;
    };
    property("Sweep", yellow::Color{0xff123456}); property("Sweep", 1.5f); property("SweepThick", 1.5f); const int style = property("Sweep", std::int32_t{2});
    session.navigate(0);
    const auto verify = [&](Schematic& root, yellow::LineStyle lineStyle) {
        auto& parent = *root.module(bench); auto& arc = *parent.module(1); const auto panel = std::get<yellow::View>(parent.readValue(view));
        check(arc.nodes()[14].kind == Redraw && arc.module(12)->nodes()[2].kind == Selector,
              "Unchanged Multi Sample Arc retains its native Redraw and nested Pen Selectors");
        const auto commands = parent.panelDrawings(panel); check(commands.size() == 1, "Complete native Arc submodule produces one drawing");
        const auto& command = std::get<yellow::DrawCommand>(commands[0].command);
        check(command.shape == yellow::ShapeKind::Arc && command.area == yellow::Area{.75f,.75f,8.5f,8.5f} &&
              command.startAngle == 135 && command.sweepAngle == 270, "Native Size, Start, Sweep and Inflate links retain their geometry");
        check(std::get<yellow::PenStroke>(command.tool) == yellow::PenStroke{yellow::Color{0xff123456},1.5f,lineStyle},
              "Native Selector Index stays first and selects the correct Pen and dash-style data");
        check(std::get<yellow::View>(arc.readValue(15)).isSamePanel(panel), "Native Redraw passes the Arc View through");
    };
    verify(session.graph(), yellow::LineStyle::Dot);
    auto& parent = *session.graph().module(bench); const auto before = parent.redrawRevision(); parent.setValue(style,std::int32_t{1});
    check(parent.redrawRevision() > before, "Native Sweep changes trigger Redraw through the unchanged wireless-to-DoIt link");
    verify(session.graph(), yellow::LineStyle::Solid);
    const auto saved = SchematicFile::project(session); QTemporaryDir dir; const auto target = dir.filePath("native-sweep-arc.ostone");
    SchematicFile::write(target, saved); Session reopened; SchematicFile::loadProject(reopened, SchematicFile::read(target), {});
    check(SchematicFile::project(reopened) == saved, "Complete Multi Sample Arc and its nested native modules survive disk save/reopen");
    verify(reopened.graph(), yellow::LineStyle::Solid);
    std::cout << "Multi Sample Arc: unchanged GUI, nested Selectors, wireless repaint and save/reopen passed\n";
}

int countSourceLinks(const FlowStoneNode& item) {
    int count = 0;
    for (const auto& link : item.links) {
        if (item.children.at(link.source - 1).node.codeSourcePort(true) == link.output) ++count;
    }
    for (const auto& child : item.children) count += countSourceLinks(child);
    return count;
}

void nativeColorModule(const QString& path) {
    using enum PrimitiveKind;
    Session session; SchematicFile::importFlowStone(session,SchematicFile::read(path),{});
    auto& graph=*session.graph().module(1);
    check(graph.nodes().size()==5 && graph.nodes()[0].kind==ArgbToColor && graph.nodes()[2].kind==ColorToArgb &&
          graph.outputType(3,0)==green::Type::Int && session.graph().outputType(1)==green::Type::Color,
          "Import the complete unchanged Stream Power Colour submodule and its original channel wiring");
    const auto verify=[](Schematic& root,yellow::Color expected) {
        const auto* child=root.module(1);
        check(child->readValue(1)==green::Value{expected} && root.readValue(1)==green::Value{expected},
              "Native color split/combine links retain RGB while replacing alpha");
    };
    graph.setValue(2,yellow::Color{0x8066aa11}); graph.setValue(4,std::int32_t{300}); verify(session.graph(),{0x2c66aa11});
    const auto saved=SchematicFile::project(session); QTemporaryDir dir; const auto target=dir.filePath("native-colour.ostone");
    SchematicFile::write(target,saved); Session reopened; SchematicFile::loadProject(reopened,SchematicFile::read(target),{});
    check(SchematicFile::project(reopened)==saved,"Native Colour submodule and edited channel values survive disk save/reopen");
    verify(reopened.graph(),{0x2c66aa11});
    reopened.graph().module(1)->setValue(4,std::int32_t{-1}); verify(reopened.graph(),{0xff66aa11});
    std::cout<<"Stream Power Colour: unchanged module, live ARGB wiring and disk save/reopen passed\n";
}

void nativeGroupAdornment(const QString& path) {
    using enum PrimitiveKind;
    Session session; SchematicFile::importFlowStone(session,SchematicFile::read(path),{});
    const auto verify=[](Schematic& root,yellow::Color expected) {
        auto& group=*root.module(1); const auto* alpha=group.module(15);
        check(group.nodes()[0].kind==RoundRectangle && group.module(7) && alpha &&
              alpha->nodes()[0].kind==ColorToArgb && alpha->nodes()[1].kind==ArgbToColor,
              "Complete Group Adornment retains its Label and native Alpha submodules");
        check(group.readValue(15)==green::Value{expected},"Native Alpha submodule replaces alpha without changing RGB");
        const auto panel=group.frontPanel()->view; const auto commands=group.panelDrawings(panel);
        check(std::ranges::any_of(commands,[&](const auto& layer) {
            const auto* draw=std::get_if<yellow::DrawCommand>(&layer.command);
            if(!draw || draw->shape!=yellow::ShapeKind::Rect || draw->area!=panel.bounds) return false;
            const auto* brush=std::get_if<yellow::SolidBrush>(&draw->tool);
            return brush && brush->color==expected;
        }),"Native Alpha output reaches the actual Group Adornment background drawing");
        const auto* label=group.module(7); const auto labelPanel=label->frontPanel();
        check(labelPanel.has_value(),"Complete native Group Adornment retains its nested label surface");
        const auto labelCommands=label->panelDrawings(labelPanel->view);
        check(std::ranges::any_of(labelCommands,[](const auto& layer) { return std::holds_alternative<yellow::TextCommand>(layer.command); }),
              "Complete native Group Adornment still renders text on its nested label surface");
    };
    auto& group=*session.graph().module(1); const auto back=std::get<yellow::Color>(group.readValue(14));
    verify(session.graph(),{0x50000000u|(back.argb&0xffffff)});
    group.setValue(14,yellow::Color{0x40123456}); group.module(15)->setValue(3,std::int32_t{293});
    verify(session.graph(),{0x25123456});
    const auto saved=SchematicFile::project(session); QTemporaryDir dir; const auto target=dir.filePath("group-adornment.ostone");
    SchematicFile::write(target,saved); Session reopened; SchematicFile::loadProject(reopened,SchematicFile::read(target),{});
    check(SchematicFile::project(reopened)==saved,"Complete bundled Group Adornment survives disk save/reopen unchanged");
    verify(reopened.graph(),{0x25123456});
    reopened.graph().module(1)->setValue(14,yellow::Color{0xffabcdef}); verify(reopened.graph(),{0x25abcdef});
    std::cout<<"Group Adornment: complete native module, nested GUI, live alpha and disk save/reopen passed\n";
}

void nativeViewCacheRecord(const QString& path) {
    using enum PrimitiveKind;
    Session session; SchematicFile::importFlowStone(session,SchematicFile::read(path),{});
    auto& graph=*session.graph().module(1);
    check(graph.nodes()[0].kind==ViewCache && graph.nodes()[0].x==47*16 && graph.nodes()[0].y==6*16,
          "Import the unchanged native View Cache record from Float Graph Recorder");
    const auto view=graph.addNode(ModuleGui,0,0), rect=graph.addNode(Rect,0,0);
    const auto source=graph.addNode(Color,0,0,yellow::Color{0x80112233}), clear=graph.addNode(Trigger,0,0);
    const auto area=graph.addNode(String,0,0,std::string{"1,2,4,3"});
    for(auto wire:{SchematicLink{view,1,0},{1,rect,0},{source,rect,2},{clear,1,1},{area,rect,1}})
        graph.connect(wire,LinkNotification::Silent);
    const auto cachedColor=[&](Schematic& child) {
        const auto paint=child.renderPanel(std::get<yellow::View>(child.readValue(view)));
        check(paint.size()==1,"Native View Cache emits one cached drawing layer");
        const auto& layer=std::get<yellow::CachedDrawCommand>(paint[0].command);
        return std::get<yellow::SolidBrush>(std::get<yellow::DrawCommand>(layer.drawings->at(0).command).tool).color;
    };
    check(cachedColor(graph)==yellow::Color{0x80112233},"Native cache captures its connected branch");
    graph.setValue(source,yellow::Color{0x40556677});
    check(cachedColor(graph)==yellow::Color{0x80112233},"Native cache reuses drawing while inputs change");
    graph.fireTrigger(clear); check(cachedColor(graph)==yellow::Color{0x40556677},"Native cache Clear refreshes on the next render");
    const auto saved=SchematicFile::project(session); QTemporaryDir dir; const auto target=dir.filePath("native-cache.ostone");
    SchematicFile::write(target,saved); Session reopened; SchematicFile::loadProject(reopened,SchematicFile::read(target),{});
    check(SchematicFile::project(reopened)==saved && cachedColor(*reopened.graph().module(1))==yellow::Color{0x40556677},
          "Unchanged native View Cache record, links and drawing survive disk save/reopen");
    std::cout<<"Float Graph Recorder View Cache: native record, capture/Clear and disk save/reopen passed\n";
}

void nativeComparisonRecord(const QString& path) {
    using enum PrimitiveKind;
    Session session;SchematicFile::importFlowStone(session,SchematicFile::read(path),{});
    auto& graph=*session.graph().module(1);const auto kind=graph.nodes()[0].kind;
    check(graph.nodes()[0].compareArrayElements && graph.outputType(1)==green::Type::Boolean,
          "Unchanged native comparison retains its fixed Boolean output and array rule");
    const auto a=graph.addNode(String,0,0,std::string{"1"}),b=graph.addNode(String,0,0,std::string{"2"});
    graph.connect({a,1,0});graph.connect({b,1,1});
    const auto expected=[&](int order) {
        switch(kind) {
        case Equal:return order==0;case LessThan:return order<0;case LessThanOrEqual:return order<=0;
        case GreaterThan:return order>0;case GreaterThanOrEqual:return order>=0;
        default:throw std::runtime_error("Unexpected bundled comparison kind");
        }
    };
    for(auto [left,right,order]:std::vector<std::tuple<std::string,std::string,int>>{{"1","2",-1},{"2","2",0},{"3","2",1}}) {
        graph.setValue(a,left);graph.setValue(b,right);
        check(std::get<bool>(graph.readValue(1))==expected(order),"Bundled comparison record evaluates its unchanged predicate");
    }
    const auto saved=SchematicFile::project(session);QTemporaryDir dir;const auto target=dir.filePath("native-comparison.ostone");
    SchematicFile::write(target,saved);Session reopened;SchematicFile::loadProject(reopened,SchematicFile::read(target),{});
    check(SchematicFile::project(reopened)==saved && std::get<bool>(reopened.graph().module(1)->readValue(1))==expected(1),
          "Unchanged bundled comparison and its connected values survive disk save/reopen");
    std::cout<<path.toStdString()<<": native comparison, live predicate and disk save/reopen passed\n";
}

void nativePresetNameModule(const QString& path) {
    using enum PrimitiveKind;
    Session session;SchematicFile::importFlowStone(session,SchematicFile::read(path),{});auto& root=session.graph();
    check(root.module(1)->nodes()[4].kind==Equal && root.module(1)->nodes()[3].kind==Selector,
          "Complete native Preset Name submodule retains comparison and Selector wiring");
    const auto name=root.addNode(String,0,0,std::string{"Cutoff"}),prefix=root.addNode(String,0,0,std::string{});
    const auto sender=root.addNode(WirelessOutput,0,0);root.setTemplateType(sender,green::Type::String);root.setWirelessLabel(sender,"PresetName");
    root.connect({name,1,0});root.connect({prefix,sender,0});
    check(root.readValue(1)==green::Value{std::string{"Cutoff"}},"An empty native prefix uses only the parameter name");
    root.setValue(prefix,std::string{"Patch"});
    check(root.readValue(1)==green::Value{std::string{"Patch-Cutoff"}},"Native String comparison chooses the separator for a populated prefix");
    root.setValue(name,std::string{"Resonance"});
    check(root.readValue(1)==green::Value{std::string{"Patch-Resonance"}},"Native parameter-name boundary remains live");
    const auto saved=SchematicFile::project(session);QTemporaryDir dir;const auto target=dir.filePath("preset-name.ostone");
    SchematicFile::write(target,saved);Session reopened;SchematicFile::loadProject(reopened,SchematicFile::read(target),{});
    check(SchematicFile::project(reopened)==saved && reopened.graph().readValue(1)==green::Value{std::string{"Patch-Resonance"}},
          "Complete native Preset Name module, wireless source and String routes survive disk save/reopen");
    reopened.graph().setValue(prefix,std::string{});
    check(reopened.graph().readValue(1)==green::Value{std::string{"Resonance"}},"Reopened comparison removes the separator when the prefix becomes empty");
    std::cout<<"Moog Filter Preset Name: unchanged module, String comparison, wireless naming and disk save/reopen passed\n";
}

void nativeGuiRecord(const QString& path) {
    Session session; check(SchematicFile::importFlowStone(session, SchematicFile::read(path), {}), "Import unchanged native GUI component record");
    const auto name = session.graph().nodes().at(0).moduleName;
    const green::Point position = name == "Amp GUI" ? green::Point{0, 6.25f} : name == "Label GUI" ? green::Point{-9, -7} : green::Point{};
    const yellow::Area bounds = name == "Amp GUI" ? yellow::Area{0, 0, 10, 4} : name == "Label GUI" ? yellow::Area{0, 0, 17, 5} : yellow::Area{0, 0, 6, 4};
    const auto verify = [&](Schematic& root) {
        const auto panel = root.module(1)->frontPanel();
        check(panel && panel->position == position && panel->view.bounds == bounds, "Native MGUI coordinates match independently recorded fields");
        if (name != "Amp GUI") check(root.module(1)->nodes()[0].anchorReference.has_value(), "Preserve modern native anchor references");
    };
    verify(session.graph()); QTemporaryDir dir; check(dir.isValid(), "Native GUI round-trip directory");
    const auto saved = SchematicFile::project(session); const auto target = dir.filePath("gui.ostone"); SchematicFile::write(target, saved);
    Session reopened; check(SchematicFile::loadProject(reopened, SchematicFile::read(target), {}), "Reopen converted native GUI record from disk");
    check(SchematicFile::project(reopened) == saved, "Native GUI conversion survives unchanged"); verify(reopened.graph());
    std::cout << name << ": native record, geometry and save/reopen passed\n";
}

void bundledModule(const QString& path) {
    const auto archive = SchematicFile::read(path);
    const auto imported = FlowStoneFile::decode(archive);
    const auto& name = imported.root.children.at(0).node.moduleName;
    if(name=="FM De-modulator") { nativeFmDemodulator(path);return; }
    const bool delay = name == "Delay", clip = name == "Clip";
    const bool denormal = name == "Denormal Remover (Bitmask)";
    const bool noise = name == "Denormal Remover (Noise)", trunc = name == "Denormal Remover (Trunc)";
    const bool doublePrecision = name.starts_with("Double Stream Biquad (");
    const bool biquad = doublePrecision || name.starts_with("Stream Biquad (");
    const bool toHz = name == "0-1 To Hz", fromHz = name == "Hz To 0-1", dezip = name == "DeZip";
    check(delay || clip || biquad || denormal || noise || trunc || toHz || fromHz || dezip || name == "Phase Invert", "Expected a verified bundled module");
    if (clip || biquad || denormal) check(imported.warnings.join(' ').contains("caches were skipped"), "Bundled compiled cache was actually skipped");
    Session session;
    check(SchematicFile::importFlowStone(session, archive, {}), "Import bundled module");
    auto& graph = session.graph();
    const auto original = SchematicFile::project(session);
    Session originalReopened;
    check(SchematicFile::loadProject(originalReopened, original, {}) && SchematicFile::project(originalReopened) == original,
          "Unchanged bundled hierarchy and source survive conversion");
    // Exercise the imported code-source connector without changing the vendor code.
    session.enterModule(1);
    auto& module = session.graph();
    int codeId = 0;
    for (const auto& node : module.nodes()) if (node.dspCode) { codeId = node.id; break; }
    if (codeId) {
        const auto source = std::ranges::find(module.nodes(), codeId, &SchematicNode::id)->dspCode->source;
        const auto text = module.addNode(PrimitiveKind::String, 0, 0);
        const auto& code = *std::ranges::find(module.nodes(), codeId, &SchematicNode::id);
        module.connect({codeId, text, 0, *code.codeSourcePort(true)});
        check(std::get<std::string>(module.readValue(text)) == source, "Bundled code supplies its unchanged source through a link");
    }
    session.navigate(0);
    const auto addCode = [&](const std::string& code) {
        const int id = graph.addNode(PrimitiveKind::DspCode, 0, 0);
        graph.setDspDraft(id, code); session.compileDsp(id); return id;
    };
    const std::string prefix = doublePrecision ? "monodouble" : "mono";
    const auto impulse = addCode(prefix + "out signal;float n;stage(0){n=0;}signal=" +
        (dezip ? "(n>=128)&1" : trunc ? "n==0 ? 2 : n==1 ? 0.0000000001 : n==2 ? -2 : n==3 ? -0.0000000001 : 0" : "(n==0)&2") + ";n=n+1;");
    const auto probe = addCode(prefix + "in signal;" + prefix + "out result;result=signal;");
    graph.connect({impulse, 1, 0}); graph.connect({1, probe, 0});
    if (delay) {
        const auto length = addCode("monoout length;length=4;");
        graph.connect({length, 1, 1});
    } else if (biquad) {
        const auto b0 = addCode(prefix + "out gain;gain=0.5;");
        graph.connect({b0, 1, 1});
    }
    const auto verify = [&](Schematic& g) {
        g.setSampleRate(3200); // Deliberately exercise imported Sample Rate and stored 40 ms time.
        for (std::size_t block : {1, 7, 64, 257}) {
            const auto samples = g.captureDsp(probe, 521, block);
            check(samples.size() == 1 && samples[0].size() == 521, "Capture converted bundled module");
            std::uint32_t x1 = 0x67452301, x2 = 0xefcdab89;
            for (int i = 0; i < 521; ++i) {
                const auto oldX2 = x2; x1 ^= x2; x2 = x1 + x2;
                const float injected = (float(std::bit_cast<std::int32_t>(oldX2)) * 0x1p-31f) * std::pow(10.f, -9.f);
                const double expected = noise ? double(float((i ? 0.f : 2.f) + injected)) :
                    trunc ? (i == 0 ? 2.0 : i == 2 ? -2.0 : 0.0) : delay ? (i == 4 ? 2.0 : 0.0) :
                    dezip ? std::clamp(i - 127, 0, 128) / 128.0 :
                    (i ? 0.0 : (clip ? double(0.99f) : biquad ? 1.0 : denormal ? 2.0 :
                               toHz ? 3200.0 : fromHz ? double(2.f / 1600.f) : -2.0));
                if (samples[0][i] != expected) throw std::runtime_error(name + " at frame " + std::to_string(i) +
                    " (buffer " + std::to_string(block) + "): got " + std::to_string(samples[0][i]) +
                    ", expected " + std::to_string(expected));
            }
        }
    };
    verify(graph);
    QTemporaryDir dir; check(dir.isValid(), "Bundled round-trip directory");
    const auto saved = SchematicFile::project(session);
    const auto savedPath = dir.filePath("converted.ostone"); SchematicFile::write(savedPath, saved);
    Session reopened;
    check(SchematicFile::loadProject(reopened, SchematicFile::read(savedPath), {}), "Reopen bundled conversion from disk");
    check(SchematicFile::project(reopened) == saved, "Persist bundled nodes, source links and rebuilt programs");
    verify(reopened.graph());
    std::cout << name << ": import, source links, render and save/reopen passed\n";
}

}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        if (argc > 1 && QByteArray(argv[1]) == "--shape-records") {
            for(int i=2;i<argc;++i)nativeShapeRecord(QString::fromLocal8Bit(argv[i]));
        } else if (argc > 1 && QByteArray(argv[1]) == "--audio-records") {
            check(argc>2,"Native audio records required");for(int i=2;i<argc;++i)nativeAudioRecord(QString::fromLocal8Bit(argv[i]));
        } else if (argc > 1 && QByteArray(argv[1]) == "--echo-module") {
            check(argc==3,"Native Echo module required");nativeEchoModule(QString::fromLocal8Bit(argv[2]));
        } else if (argc > 1 && QByteArray(argv[1]) == "--state-records") {
            check(argc>2,"Native state records required");for(int i=2;i<argc;++i)nativeStateRecord(QString::fromLocal8Bit(argv[i]));
        } else if (argc > 1 && QByteArray(argv[1]) == "--startup-counter") {
            check(argc==3,"Native startup counter fixture required");nativeStartupCounter(QString::fromLocal8Bit(argv[2]));
        } else if (argc > 1 && QByteArray(argv[1]) == "--routing-records") {
            check(argc>2,"Native routing records required");for(int i=2;i<argc;++i)nativeRoutingRecord(QString::fromLocal8Bit(argv[i]));
        } else if (argc > 1 && QByteArray(argv[1]) == "--stereo-split") {
            check(argc==3,"Native Reverb Left/Right split fixture required");nativeStereoSplit(QString::fromLocal8Bit(argv[2]));
        } else if (argc > 1 && QByteArray(argv[1]) == "--comparison-records") {
            check(argc>2,"Native comparison fixtures required");for(int i=2;i<argc;++i)nativeComparisonRecord(QString::fromLocal8Bit(argv[i]));
        } else if (argc > 1 && QByteArray(argv[1]) == "--preset-name-module") {
            check(argc==3,"Native Preset Name module fixture required");nativePresetNameModule(QString::fromLocal8Bit(argv[2]));
        } else if (argc > 1 && QByteArray(argv[1]) == "--color-module") {
            check(argc==3,"Native Colour module fixture required"); nativeColorModule(QString::fromLocal8Bit(argv[2]));
        } else if (argc > 1 && QByteArray(argv[1]) == "--group-adornment") {
            check(argc==3,"Native Group Adornment fixture required"); nativeGroupAdornment(QString::fromLocal8Bit(argv[2]));
        } else if (argc > 1 && QByteArray(argv[1]) == "--view-cache-record") {
            check(argc==3,"Native View Cache record fixture required"); nativeViewCacheRecord(QString::fromLocal8Bit(argv[2]));
        } else if (argc > 1 && QByteArray(argv[1]) == "--redraw-area-record") {
            check(argc == 3, "Native Redraw Area fixture required"); nativeRedrawAreaRecord(QString::fromLocal8Bit(argv[2]));
        } else if (argc > 1 && QByteArray(argv[1]) == "--group-label") {
            check(argc == 3, "Native Group Adornment Label fixture required"); nativeGroupLabel(QString::fromLocal8Bit(argv[2]));
        } else if (argc > 1 && QByteArray(argv[1]) == "--sweep-arc") {
            check(argc == 3, "Native Multi Sample Arc fixture required"); nativeSweepArc(QString::fromLocal8Bit(argv[2]));
        } else if (argc > 1 && QByteArray(argv[1]) == "--background-modules") {
            for(int i=2;i<argc;++i)nativeBackgroundModule(QString::fromLocal8Bit(argv[i]));
        } else if (argc > 1 && QByteArray(argv[1]) == "--label-module") {
            check(argc == 3, "Native Label GUI fixture required"); nativeLabelModule(QString::fromLocal8Bit(argv[2]));
        } else if (argc > 1 && QByteArray(argv[1]) == "--format-records") {
            for (int i=2;i<argc;++i) nativeFormatRecord(QString::fromLocal8Bit(argv[i]));
        } else if (argc > 1 && QByteArray(argv[1]) == "--gui-modules") {
            check(argc > 2, "Native GUI module fixture paths required");
            for (int i = 2; i < argc; ++i) nativeGuiModule(QString::fromLocal8Bit(argv[i]));
        } else if (argc > 1 && QByteArray(argv[1]) == "--drawing-records") {
            check(argc > 2, "Native drawing fixture paths required");
            for (int i = 2; i < argc; ++i) nativeDrawingRecord(QString::fromLocal8Bit(argv[i]));
        } else if (argc > 1 && QByteArray(argv[1]) == "--gui-records") {
            check(argc > 2, "Native GUI fixture paths required");
            for (int i = 2; i < argc; ++i) nativeGuiRecord(QString::fromLocal8Bit(argv[i]));
        } else if (argc > 1 && QByteArray(argv[1]) == "--bundled") {
            check(argc > 2, "Bundled module fixture paths required");
            for (int i = 2; i < argc; ++i) bundledModule(QString::fromLocal8Bit(argv[i]));
        } else if (argc > 1) {
            for (int i = 1; i < argc; ++i) {
                const auto imported = FlowStoneFile::decode(SchematicFile::read(QString::fromLocal8Bit(argv[i])));
                std::cout << argv[i] << ": version " << imported.version << ", " << imported.nodes << " nodes, " << imported.links << " links, " << countSourceLinks(imported.root) << " source links\n";
                std::cout << imported.warnings.join("\n").toStdString() << "\n";
                Session s; SchematicFile::importFlowStone(s, SchematicFile::read(QString::fromLocal8Bit(argv[i])), {});
                Session reopened; SchematicFile::loadProject(reopened, SchematicFile::project(s), {});
            }
        } else { importsAndRoundTrips(); rejectsInvalidFiles(); codePorts(); valuesAndRuby(); linkedCodeSources(); compiledCaches(); valueLayouts(); propertyPanelLayouts(); genericMathRecords(); floatDeZipperRecord(); importedPanels(); legacyPanelControls(); inheritedViews(); propertyWireless(); guiDrawing(); guiSelectorAndRoutedLinks(); penAndFontProperties(); stringFormatsAndBrushes(); customRadialPaths(); importedShapes(); importedDrawingControls(); importedColorConversions(); importedViewCaches(); importedComparisons(); importedRouting(); importedStateRecords(); importedAudioRecords(); heldClose(); }
        std::cout << "FlowStone import tests passed\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
