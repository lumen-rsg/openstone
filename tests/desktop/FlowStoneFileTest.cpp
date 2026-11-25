#include "FlowStoneFile.hpp"
#include "SchematicFile.hpp"
#include "AudioPatch.hpp"
#include "PanelRadialBrush.hpp"
#include "PanelShapePath.hpp"
#include <QColor>
#include <QCryptographicHash>
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
void header(Bytes& b, int version, const std::vector<QByteArray>& images = {}) {
    b.w(version); b.d(1); b.d(1); b.w(0); b.zeros(3); b.b(1); b.zeros(12);
    if (version >= 171) b.d(0); // font resources
    b.d(images.size());
    for (const auto& image : images) { b.d(image.size()); b.append(image); }
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
    throw std::runtime_error(std::string("Expected failed import: ")+fragment);
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
    Bytes b; header(b, 166); beginModule(b, "Root", 3);
    const char* code = "def event i,v; output 0,v; end";
    stringBox(b, "def init; raise 'data is not code'; end");
    primitive(b, 1110, 1, 4); b.str(code); b.zeros(16); b.b(0); b.d(1); b.d(1);
    b.w(1); b.str("value"); b.w(1); b.str("result"); b.d(0); b.d(0);
    stringBox(b, "sink");
    b.w(2); wire(b, 166, 0, 0, 1, 0); wire(b, 166, 1, 0, 2, 0); endModule(b); b.w(0); b.b(0);
    Session session; check(SchematicFile::importFlowStone(session, zip(b), {}), "Import native Ruby data connectors without executing code");
    auto& graph=session.graph();
    check(graph.nodes()[1].rubyCode->source==code && graph.links()[0].input==0 && graph.links()[1].output==0,
          "Native Ruby String input/output zero are data connectors, independent of stored source");
    graph.setValue(1,std::string("different data"));
    check(graph.nodes()[1].rubyCode->source==code,"Changing native data input zero never edits Ruby code");
    // OpenStone's additional Source ports remain available after native import.
    const auto source=graph.addNode(PrimitiveKind::String,0,0,std::string(code));
    graph.connect({source,2,1});
    graph.editRubyPort(2,false,0,RubyPortEdit::Count,3);
    check(graph.links()[0].input==0 && graph.links()[2].input==3,"Resizing Ruby data inputs moves only the added Source link");
    const auto text=graph.addNode(PrimitiveKind::String,0,0);
    graph.connect({2,text,0,1});
    graph.editRubyPort(2,true,0,RubyPortEdit::Count,2);
    check(graph.links()[1].output==0 && graph.links()[3].output==2,"Resizing Ruby outputs preserves native data zero and moves Source");
    check(graph.readValue(text)==green::Value{std::string(code)},"Additional source output reads the unchanged Ruby program");
    const auto saved=SchematicFile::project(session); QTemporaryDir dir; const auto path=dir.filePath("ruby-data-ports.ostone");
    SchematicFile::write(path,saved); Session reopened; SchematicFile::loadProject(reopened,SchematicFile::read(path),{});
    check(SchematicFile::project(reopened)==saved,"Native data links and added Source links survive disk save/reopen");
    const std::string next="def event i,v; output 0,v.upcase; end";
    reopened.graph().setValue(source,next);
    check(reopened.graph().readValue(text)==green::Value{next} && reopened.graph().nodes()[1].rubyCode->source==next,
          "OpenStone's explicitly connected source input still updates code after reopening");
    // The native file cannot address OpenStone's additional synthetic source sockets.
    Bytes c; header(c,166); beginModule(c,"Root",2);
    primitive(c,1110,1,4); c.str("def event i,v; end"); c.zeros(16); c.b(0); c.d(0); c.d(0); c.d(0); c.d(0);
    stringBox(c,"data"); c.w(1); wire(c,166,1,0,0,0); endModule(c); c.w(0); c.b(0);
    expectError(zip(c),"connector index");
    auto bad=b; Bytes link; wire(link,166,0,0,1,0); const auto at=bad.lastIndexOf(link); check(at>0,"Locate native Ruby data link");
    bad[at+6]=1; expectError(zip(bad),"connector index");
    bad=b; link.clear(); wire(link,166,1,0,2,0); const auto out=bad.lastIndexOf(link); check(out>0,"Locate native Ruby data output");
    bad[out+3]=1; expectError(zip(bad),"connector index");
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
Bytes bitmapDocument(int version, const QByteArray& image, bool reference = false, int index = 0,
                     int targetInput = 3, bool layout = false) {
    Bytes b; header(b, version, reference ? std::vector<QByteArray>{image} : std::vector<QByteArray>{});
    beginModule(b, "Root", 1); beginModule(b, "Bitmap test", 6);
    primitive(b, 105, 3, 4, "Image");
    if (version >= 160) { b.b(1); b.b(layout); if (layout) connectorLayout(b, version); }
    b.b(reference); if (reference) b.d(index); else { b.d(image.size()); b.append(image); }
    // A following value proves the variable-length image record ends at the right byte.
    primitive(b, 29, 5, 6, "Sentinel"); b.f(8); b.b(0); b.b(0); b.d(123);
    primitive(b, 138, 0, 10); b.d(9); b.b(0); b.str(targetInput == 1 ? "Load" : "Clear");
    primitive(b, 139, 10, 0); b.d(16); b.b(0); b.str("Image");
    primitive(b, 139, 10, 10); b.d(3); b.b(0); b.str("Width");
    primitive(b, 139, 10, 20); b.d(3); b.b(0); b.str("Height");
    b.w(4); wire(b, version, 2, 0, 0, targetInput); wire(b, version, 0, 0, 3, 0);
    wire(b, version, 0, 1, 4, 0); wire(b, version, 0, 2, 5, 0); endModule(b);
    b.w(0); endModule(b); b.w(0); b.b(0); return b;
}
yellow::Bitmap bitmapValue(Schematic& graph, int id, int width, int height) {
    const auto image = std::get<yellow::Bitmap>(graph.readValue(id));
    check(image.width() == width && image.height() == height &&
          graph.readValue(id, 1) == green::Value{std::int32_t(width)} &&
          graph.readValue(id, 2) == green::Value{std::int32_t(height)}, "Bitmap pixels and Width/Height outputs agree");
    return image;
}
void importedBitmaps() {
    for (int version : {154,159,160,161,166,171,177}) for (bool reference : {false,true}) for (bool empty : {false,true}) {
        const auto raw = bitmapDocument(version, empty ? QByteArray{} : texturePng(), reference, 0, 3, true);
        const auto parsed = FlowStoneFile::decode(zip(raw));
        check(parsed.root.children[0].children[0].nativeTag == 105 && parsed.root.children[0].links[0].input == 2,
              "Bitmap keeps its native record identity and remaps Clear past the Paste socket");
        Session s; SchematicFile::importFlowStone(s, zip(raw), {}); auto& graph = *s.graph().module(1);
        check(graph.nodes()[0].kind == PrimitiveKind::Bitmap && graph.readValue(2) == green::Value{std::int32_t{123}},
              "Import Bitmap and preserve following records");
        const auto verify = [&](Schematic& root) {
            auto& g = *root.module(1); const auto image = bitmapValue(g, 1, empty ? 0 : 2, empty ? 0 : 1);
            if (!empty) check(image.pixelArgb(0,0) == 0x280a141e && image.pixelArgb(1,0) == 0xffc8beb4,
                              "Decode straight alpha, RGB channel order and top-down image pixels exactly");
            check(std::get<yellow::Bitmap>(root.readValue(1)) == image, "Native Bitmap output reaches the module boundary");
            check(root.readValue(1,1) == green::Value{std::int32_t(image.width())} &&
                  root.readValue(1,2) == green::Value{std::int32_t(image.height())}, "Native Width/Height links reach their boundaries");
        };
        verify(s.graph()); const auto saved = SchematicFile::project(s); QTemporaryDir dir;
        const auto path = dir.filePath("bitmap.ostone"); SchematicFile::write(path, saved); Session reopened;
        SchematicFile::loadProject(reopened, SchematicFile::read(path), {});
        check(SchematicFile::project(reopened) == saved, "Bitmap pixels and native wiring survive disk save/reopen"); verify(reopened.graph());
        const auto trigger = reopened.graph().addNode(PrimitiveKind::Trigger, 0, 0);
        reopened.graph().connect({trigger, 1, 0}, LinkNotification::Silent);
        const auto before = reopened.graph().module(1)->inspectOutput(1).triggers;
        reopened.graph().fireTrigger(trigger); bitmapValue(*reopened.graph().module(1), 1, 0, 0);
        check(reopened.graph().module(1)->inspectOutput(1).triggers == before + 1,
              "A native Clear connection clears the image and notifies downstream after reopening");
        // Dropping the cache remains persistent too.
        Session cleared; SchematicFile::loadProject(cleared, SchematicFile::project(reopened), {});
        bitmapValue(*cleared.graph().module(1), 1, 0, 0);
    }
    auto raw = bitmapDocument(177, texturePng(), true, -1);
    Session empty; SchematicFile::importFlowStone(empty, zip(raw), {}); bitmapValue(*empty.graph().module(1), 1, 0, 0);
    Bytes shared; header(shared,177,{QByteArray{},texturePng()});
    beginModule(shared,"Root",1); beginModule(shared,"Shared images",2);
    for (int i=0;i<2;++i) { primitive(shared,105,0,i); shared.b(0); shared.b(0); shared.b(1); shared.d(1); }
    shared.w(0); endModule(shared); shared.w(0); endModule(shared); shared.w(0); shared.b(0);
    Session references; SchematicFile::importFlowStone(references,zip(shared),{});
    const auto first = bitmapValue(*references.graph().module(1),1,2,1);
    check(bitmapValue(*references.graph().module(1),2,2,1) == first, "Repeated references resolve a nonzero image-table index");
    Session referencesReopened; SchematicFile::loadProject(referencesReopened,SchematicFile::project(references),{});
    check(bitmapValue(*referencesReopened.graph().module(1),1,2,1) == first &&
          bitmapValue(*referencesReopened.graph().module(1),2,2,1) == first, "Shared-table pixels persist in both converted Bitmap nodes");
    for (int index : {-2,1,std::numeric_limits<int>::max()})
        expectError(zip(bitmapDocument(177, texturePng(), true, index)), "resource index");
    expectError(zip(bitmapDocument(177, texturePng(), false, 0, 2)), "Bitmap Paste");
    expectError(zip(bitmapDocument(177, texturePng(), false, 0, 4)), "connector index");
    expectError(zip(bitmapDocument(159, "broken image")), "Bitmap image");
    expectError(zip(bitmapDocument(177, "broken image", true)), "Bitmap resource image");
    raw = bitmapDocument(159, texturePng()); const auto imageAt = raw.indexOf(texturePng());
    check(imageAt > 0, "Find inline image");
    for (int cut : {0,1,8,30,int(texturePng().size()-1)}) expectError(zip(raw.left(imageAt + cut)), "Truncated");
    auto bad = raw; qToLittleEndian<quint32>(0xffffffff, bad.data() + imageAt - 4); expectError(zip(bad), "record count");
    bad = raw; bad[imageAt - 5] = 2; expectError(zip(bad), "Boolean");
    // Header dimensions are checked before allocating or inflating any image pixels.
    auto huge = texturePng(); qToBigEndian<quint32>(1000000, huge.data()+16);
    qToBigEndian<quint32>(1000000, huge.data()+20);
    const auto crc = crc32(0,reinterpret_cast<const Bytef*>(huge.constData()+12),17); qToBigEndian<quint32>(crc,huge.data()+29);
    expectError(zip(bitmapDocument(177, huge)), "Bitmap image size");
    // Path is lazy; only the native Load input invokes the host's image loader.
    int loads = 0;
    const yellow::Bitmap replacement{1,2,{0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88}};
    const auto loader = [&](std::string_view path) {
        check(path == "replacement.bmp", "Native Load reads the current Path input"); ++loads; return replacement;
    };
    Session loaded(loader); SchematicFile::importFlowStone(loaded, zip(bitmapDocument(177, texturePng(), false, 0, 1)), {});
    auto& g = *loaded.graph().module(1); const auto path = g.addNode(PrimitiveKind::String,0,0,std::string{"replacement.bmp"});
    g.connect({path,1,0}); check(loads == 0, "Import and Path changes do not load an external image");
    const auto pulse = loaded.graph().addNode(PrimitiveKind::Trigger,0,0);
    loaded.graph().connect({pulse,1,0},LinkNotification::Silent); loaded.graph().fireTrigger(pulse);
    check(loads == 1 && bitmapValue(g,1,1,2) == replacement, "Native Load wiring updates image pixels and both dimensions");
    Session opened(loader); SchematicFile::loadProject(opened,SchematicFile::project(loaded),{});
    check(loads == 1 && bitmapValue(*opened.graph().module(1),1,1,2) == replacement, "Reopening uses saved pixels without reloading the source path");
    opened.graph().fireTrigger(pulse); check(loads == 2, "Native Load still invokes the host loader after reopening");
}
void nativeBitmapRecord(const QString& path) {
    using enum PrimitiveKind;
    Session s; SchematicFile::importFlowStone(s, SchematicFile::read(path), {}); auto& graph = *s.graph().module(1);
    check(graph.nodes().size() == 1 && graph.nodes()[0].kind == Bitmap, "Import unchanged bundled Bitmap record");
    const bool detuner = path.contains("Detuner");
    const auto verify = [&](Schematic& root) {
        const auto image = bitmapValue(*root.module(1), 1, 32, 32); const auto pixels = image.rgba();
        const auto digest = QCryptographicHash::hash(QByteArrayView(reinterpret_cast<const char*>(pixels.data()), pixels.size()),
                                                    QCryptographicHash::Sha256).toHex();
        check(digest == (detuner ? "2e2496a24dab822bf7fa438a32a035f573ab3c1b0e3b485b9e3bb71a35c179b4" :
                                  "50326039dff849e5b0c9f4dbba0b867838923e475d7b7d0a8d6286cdd8689a3e"),
              "Bundled Bitmap matches independently decoded RGBA pixels");
    };
    verify(s.graph()); const auto saved = SchematicFile::project(s); QTemporaryDir dir;
    const auto target = dir.filePath("native-bitmap.ostone"); SchematicFile::write(target, saved); Session reopened;
    SchematicFile::loadProject(reopened, SchematicFile::read(target), {});
    check(SchematicFile::project(reopened) == saved, "Bundled image pixels persist after saving and reopening"); verify(reopened.graph());
    const auto clear = reopened.graph().module(1)->addNode(Trigger, 0, 0);
    reopened.graph().module(1)->connect({clear,1,2}, LinkNotification::Silent);
    reopened.graph().module(1)->fireTrigger(clear); bitmapValue(*reopened.graph().module(1), 1, 0, 0);
    std::cout << path.toStdString() << ": unchanged Bitmap record, exact pixels, Clear and disk save/reopen passed\n";
}
Bytes fileDialogDocument(int version, bool boundaries) {
    Bytes b; header(b,version); beginModule(b,"Root",1); beginModule(b,"File picker",boundaries ? 12 : 2);
    primitive(b,103,20,30,"Choose image");
    if (boundaries) {
        const char* names[]{"Open","Filter","Type","Title","Folder","Name","Ext"};
        for (int i=0;i<7;++i) { primitive(b,138,0,i*2); b.d(i==0 ? 9 : i==2 ? 3 : 1); b.b(0); b.str(names[i]); }
        for (int i=0;i<3;++i) { primitive(b,139,40,i*2); b.d(i==0 ? 1 : 9); b.b(0); b.str(i==0 ? "Path" : i==1 ? "OK" : "Cancel"); }
    }
    primitive(b,29,50,50,"Sentinel"); b.f(8); b.b(0); b.b(0); b.d(123);
    b.w(boundaries ? 10 : 0);
    if (boundaries) {
        for (int i=0;i<7;++i) wire(b,version,i+1,0,0,i);
        for (int i=0;i<3;++i) wire(b,version,0,i,i+8,0);
    }
    endModule(b); b.w(0); endModule(b); b.w(0); b.b(0); return b;
}
struct FileDialogBench { int pulse; std::array<int,6> controls; int okCounter, pathOrder, pathOnOk; bool boundaries; };
FileDialogBench wireFileDialog(Schematic& root, bool boundaries) {
    using enum PrimitiveKind;
    auto& g = boundaries ? root : *root.module(1);
    FileDialogBench bench{g.addNode(Trigger,0,0),{},g.addNode(TriggerCounter,0,0),g.addNode(SampleAndHold,0,0),g.addNode(SampleAndHold,0,0),boundaries};
    g.connect({bench.pulse,1,0},LinkNotification::Silent);
    for (int i=0;i<6;++i) {
        bench.controls[i] = i==1 ? g.addNode(Int,0,0,std::int32_t{1}) : g.addNode(String,0,0,std::string{});
        g.connect({bench.controls[i],1,i+1},LinkNotification::Silent);
    }
    for (auto link : {SchematicLink{1,bench.okCounter,0,1},{bench.okCounter,bench.pathOrder,0},
                      {1,bench.pathOrder,1,0},{1,bench.pathOnOk,0,0},{1,bench.pathOnOk,1,1}})
        g.connect(link,LinkNotification::Silent);
    return bench;
}
void exerciseFileDialog(Schematic& root, const FileDialogBench& bench) {
    using green::Value;
    auto& g = bench.boundaries ? root : *root.module(1); auto& owner = *root.module(1);
    check(!root.pendingAppDialog() && owner.readValue(1)==Value{std::string{}},"Import/reopen does not show a dialog or restore its transient selected path");
    g.setValue(bench.controls[0],std::string{"Images|*.png;*.jpg|Text|*.txt||"});
    g.setValue(bench.controls[2],std::string{"Pick an image"}); g.setValue(bench.controls[3],std::string{"/home/音楽"});
    g.setValue(bench.controls[4],std::string{"new image.png"}); g.setValue(bench.controls[5],std::string{"png"});
    check(!root.pendingAppDialog(),"File Dialog configuration inputs are passive");
    for (int mode : {1,0,2,-1,std::numeric_limits<int>::min(),std::numeric_limits<int>::max()}) {
        g.setValue(bench.controls[1],std::int32_t(mode)); g.fireTrigger(bench.pulse);
        check(root.pendingAppDialog(),"Open trigger creates a headless host dialog request"); const auto request = *root.pendingAppDialog();
        check(request.kind==PrimitiveKind::FileDialog && request.ownerPath==std::vector{1} && request.node==1 &&
              request.type==(mode!=0 ? 1 : 0) && request.filter=="Images|*.png;*.jpg|Text|*.txt|" && request.title=="Pick an image" &&
              request.folder=="/home/音楽" && request.name=="new image.png" && request.suffix=="png",
              "Native connector order snapshots all parameters and uses zero=Save, nonzero=Open");
        g.fireTrigger(bench.pulse); check(root.pendingAppDialog()->token==request.token,"Repeated Open triggers do not create duplicate active dialogs");
        g.setValue(bench.controls[4],std::string{"next image.png"});
        check(root.pendingAppDialog()->name=="new image.png","Pending request retains its captured parameters");
        const auto values=owner.inspectOutput(1,0).triggers, ok=owner.inspectOutput(1,1).triggers, cancel=owner.inspectOutput(1,2).triggers;
        const auto count=g.readValue(bench.okCounter);
        check(root.finishAppDialog(request.token,true,"/chosen/画像.png"),"Complete imported File Dialog at the owning nested node");
        check(owner.readValue(1)==Value{std::string{"/chosen/画像.png"}} && g.readValue(bench.pathOnOk)==owner.readValue(1) &&
              g.readValue(bench.pathOrder)==count && owner.inspectOutput(1,0).triggers==values+1 &&
              owner.inspectOutput(1,1).triggers==ok+1 && owner.inspectOutput(1,2).triggers==cancel,
              "Selected Path notifies before OK, whose listeners see the new Unicode path");
        g.fireTrigger(bench.pulse); const auto next=root.pendingAppDialog()->token;
        check(!root.finishAppDialog(request.token,true,"/stale") && root.pendingAppDialog()->token==next,"Stale response cannot complete a later dialog");
        check(root.finishAppDialog(next,false) && !root.pendingAppDialog() && owner.readValue(1)==Value{std::string{"/chosen/画像.png"}} &&
              owner.inspectOutput(1,0).triggers==values+1 && owner.inspectOutput(1,1).triggers==ok+1 &&
              owner.inspectOutput(1,2).triggers==cancel+1,"Cancel only fires Cancel and retains the last successful Path");
        g.setValue(bench.controls[4],std::string{"new image.png"});
    }
    for (auto [input,expected] : std::vector<std::pair<std::string,std::string>>{
         {"","All files (*.*)|*.*|"},{".png|*.jpg|","*.png|*.png|*.jpg|*.jpg|"},
         {" Images | png | Waves | .wav |","Images|*.png|Waves|*.wav|"}}) {
        g.setValue(bench.controls[0],input); g.fireTrigger(bench.pulse);
        check(root.pendingAppDialog()->filter==expected,"Native shorthand filters expand into host label/pattern pairs");
        root.finishAppDialog(root.pendingAppDialog()->token,false);
    }
}
void fileDialogRoundTrip(const QByteArray& archive, bool boundaries) {
    Session s; SchematicFile::importFlowStone(s,archive,{}); auto& owner=*s.graph().module(1);
    check(owner.nodes()[0].kind==PrimitiveKind::FileDialog && owner.nodes()[0].flowStoneFileDialog,
          "Recognize native File Dialog and retain its default/filter compatibility policy");
    if (!boundaries) {
        const auto pulse=owner.addNode(PrimitiveKind::Trigger,0,0); owner.connect({pulse,1,0},LinkNotification::Silent); owner.fireTrigger(pulse);
        check(s.graph().pendingAppDialog()->type==1 && s.graph().pendingAppDialog()->title=="Choose File" &&
              s.graph().pendingAppDialog()->filter=="All files (*.*)|*.*|","Unconnected native Type defaults to Open with the native title and all-files filter");
        s.graph().finishAppDialog(s.graph().pendingAppDialog()->token,false);
    }
    const auto bench=wireFileDialog(s.graph(),boundaries); exerciseFileDialog(s.graph(),bench);
    // Save while a dialog is open: live requests and results must not be replayed on load.
    auto& g=boundaries ? s.graph() : owner; g.fireTrigger(bench.pulse);
    const auto saved=SchematicFile::project(s); QTemporaryDir dir; const auto path=dir.filePath("file-dialog.ostone"); SchematicFile::write(path,saved);
    Session reopened; SchematicFile::loadProject(reopened,SchematicFile::read(path),{});
    check(!reopened.graph().pendingAppDialog() && SchematicFile::project(reopened)==saved,
          "Dialog policy, controls, boundaries and links survive disk save/reopen without reopening a pending dialog");
    exerciseFileDialog(reopened.graph(),bench);
}
void importedFileDialogs() {
    for (int version : {154,159,160,161,166,171,177}) for (bool boundaries : {false,true}) {
        const auto raw=fileDialogDocument(version,boundaries); fileDialogRoundTrip(zip(raw),boundaries);
        const auto parsed=FlowStoneFile::decode(zip(raw));
        check(parsed.root.children[0].children.back().node.localValue==green::Value{std::int32_t{123}},"Resume immediately after the payload-free File Dialog record");
        Bytes prefix; primitive(prefix,103,20,30,"Choose image"); const auto at=raw.indexOf(prefix);
        for (int cut=0;cut<prefix.size();++cut) expectError(zip(raw.left(at+cut)),"Truncated");
        if (boundaries) {
            auto bad=raw; Bytes link; wire(link,version,1,0,0,0); const auto wireAt=bad.indexOf(link,at+prefix.size());
            check(wireAt>0,"Locate File Dialog wire"); bad[wireAt+6]=char(7); expectError(zip(bad),"connector index");
            bad=raw; link.clear(); wire(link,version,0,0,8,0); const auto outputAt=bad.indexOf(link,wireAt);
            check(outputAt>0,"Locate File Dialog Path wire"); bad[outputAt+3]=char(3); expectError(zip(bad),"connector index");
        }
    }
    Session ordinary; auto& g=ordinary.graph(); const auto dialog=g.addNode(PrimitiveKind::FileDialog,0,0), pulse=g.addNode(PrimitiveKind::Trigger,0,0);
    g.connect({pulse,dialog,0}); g.fireTrigger(pulse);
    check(g.pendingAppDialog()->type==0 && !g.nodes()[0].flowStoneFileDialog,"Existing OpenStone File Dialog defaults retain their behavior");
    Session source; SchematicFile::importFlowStone(source,zip(fileDialogDocument(177,false)),{});
    for (bool wrongKind : {false,true}) {
        auto doc=QJsonDocument::fromJson(SchematicFile::project(source)).object(); auto root=doc["root"].toObject();
        auto nodes=root["nodes"].toArray(); auto module=nodes[0].toObject(); auto children=module["nodes"].toArray();
        auto node=children[0].toObject();
        if (wrongKind) node["kind"]="Int"; else node["flowStoneFileDialog"]="bad";
        children[0]=node; module["nodes"]=children; nodes[0]=module; root["nodes"]=nodes; doc["root"]=root;
        const auto before=SchematicFile::project(ordinary); bool touched=false,rejected=false;
        try { SchematicFile::loadProject(ordinary,QJsonDocument(doc).toJson(),[&] { touched=true; }); }
        catch (const std::exception& e) { rejected=std::string(e.what()).find("native File Dialog state")!=std::string::npos; }
        check(rejected && !touched && SchematicFile::project(ordinary)==before,"Malformed native dialog policy rejects before replacing the project");
    }
}
void nativeFileDialogRecord(const QString& path) {
    fileDialogRoundTrip(SchematicFile::read(path),false);
    std::cout<<path.toStdString()<<": unchanged File Dialog record, requests/results and disk save/reopen passed\n";
}
void nativeBitmapFileDialog(const QString& path) {
    using enum PrimitiveKind;
    int loads=0; const yellow::Bitmap replacement{2,1,{10,20,30,40,50,60,70,80}};
    const auto loader=[&](std::string_view chosen) { check(chosen=="/chosen/new.png","Native Path arrives at Bitmap before Load"); ++loads; return replacement; };
    Session s(loader); SchematicFile::importFlowStone(s,SchematicFile::read(path),{}); auto& g=*s.graph().module(1);
    check(g.nodes().size()==2 && g.nodes()[0].kind==FileDialog && g.nodes()[1].kind==Bitmap && g.links().size()==2,
          "Preserve bundled File Dialog and Bitmap records with Path/Load wiring"); bitmapValue(g,2,32,32);
    const auto pulse=g.addNode(Trigger,0,0); g.connect({pulse,1,0},LinkNotification::Silent); g.fireTrigger(pulse);
    s.graph().finishAppDialog(s.graph().pendingAppDialog()->token,false); check(loads==0,"Cancel does not request a bitmap load");
    const auto verify=[&](Schematic& root) {
        auto& owner=*root.module(1); owner.fireTrigger(pulse); const auto before=loads;
        check(root.finishAppDialog(root.pendingAppDialog()->token,true,"/chosen/new.png") && loads==before+1 &&
              bitmapValue(owner,2,2,1)==replacement,"Native Path/OK connections load the selected bitmap exactly once");
    };
    verify(s.graph()); const auto saved=SchematicFile::project(s); QTemporaryDir dir; const auto target=dir.filePath("bitmap-picker.ostone");
    SchematicFile::write(target,saved); Session reopened(loader); SchematicFile::loadProject(reopened,SchematicFile::read(target),{});
    check(loads==1 && SchematicFile::project(reopened)==saved && bitmapValue(*reopened.graph().module(1),2,2,1)==replacement,
          "Reopening preserves loaded pixels and functional dialog wiring without external file reads"); verify(reopened.graph());
    std::cout<<path.toStdString()<<": native File Dialog/Bitmap records, selection/load ordering and disk save/reopen passed\n";
}
void buttonLayout(Bytes& b, int version) {
    b.d(2);
    for (int i=0;i<2;++i) {
        b.f(12+i); b.f(0); b.d(14); b.d(4); b.f(10); b.f(2);
        if (version>=157) { b.b(0); b.b(0); b.b(0); b.b(0); b.b(i); if (i) { b.f(0); b.f(0); b.f(10); b.f(2); } }
    }
    b.d(2); b.w(1); b.w(0);
}
Bytes buttonDocument(int version, bool enabled, bool layout, bool boundary = true) {
    Bytes b; header(b,version); beginModule(b,"Root",1); beginModule(b,"Button",boundary ? 3 : 2);
    primitive(b,17,12,14,"Load Bitmap..."); b.b(enabled); b.b(layout); if (layout) buttonLayout(b,version);
    primitive(b,29,40,20,"Sentinel"); b.f(8); b.b(0); b.b(0); b.d(123);
    if (boundary) { primitive(b,139,50,10); b.d(9); b.b(0); b.str("Click"); }
    b.w(boundary ? 1 : 0); if (boundary) wire(b,version,0,0,2,0); endModule(b);
    b.w(0); endModule(b); b.w(0); b.b(0); return b;
}
void exerciseButton(Schematic& root, int first, int second, std::optional<int> outer = {}) {
    using green::Value;
    auto& g=*root.module(1); const auto before=g.inspectOutput(1).triggers;
    const auto a=std::get<std::int32_t>(g.readValue(first)), b=std::get<std::int32_t>(g.readValue(second));
    const auto c=outer ? std::get<std::int32_t>(root.readValue(*outer)) : 0;
    for (int press=1;press<=3;++press) {
        g.fireTrigger(1); // The same entry point used by the editor's Fire button.
        check(g.inspectOutput(1).triggers==before+press && g.readValue(first)==Value{a+press} && g.readValue(second)==Value{b+press},
              "Each Button activation emits exactly one Trigger to every connected receiver");
        if (outer) check(root.readValue(*outer)==Value{c+press},"Button pulses cross their native module-output boundary");
    }
}
void buttonRoundTrip(const QByteArray& archive, bool boundary, const std::string& label) {
    using enum PrimitiveKind;
    const auto decoded=FlowStoneFile::decode(archive); const auto& button=decoded.root.children[0].children[0];
    check(button.nativeTag==17 && button.node.kind==Trigger && button.node.label==label && button.node.inputPorts().empty() &&
          button.node.outputPorts().size()==1 && button.node.outputPorts()[0].type==green::Type::Trigger,
          "Native Button maps to the existing Trigger primitive with its caption and fixed connector contract");
    Session s; SchematicFile::importFlowStone(s,archive,{}); auto& g=*s.graph().module(1);
    const auto first=g.addNode(TriggerCounter,0,0), second=g.addNode(TriggerCounter,0,0);
    g.connect({1,first,0},LinkNotification::Silent); g.connect({1,second,0},LinkNotification::Silent);
    std::optional<int> outer;
    if (boundary) { outer=s.graph().addNode(TriggerCounter,0,0); s.graph().connect({1,*outer,0},LinkNotification::Silent); }
    check(g.inspectOutput(1).triggers==0 && !s.graph().pendingAppDialog(),"Loading and wiring a Button does not activate it");
    exerciseButton(s.graph(),first,second,outer);
    const auto saved=SchematicFile::project(s); QTemporaryDir dir; const auto target=dir.filePath("button.ostone"); SchematicFile::write(target,saved);
    Session reopened; SchematicFile::loadProject(reopened,SchematicFile::read(target),{});
    check(SchematicFile::project(reopened)==saved && reopened.graph().module(1)->nodes()[0].label==label &&
          reopened.graph().module(1)->inspectOutput(1).triggers==0,"Button caption, links and position persist without replaying activations after disk save/reopen");
    exerciseButton(reopened.graph(),first,second,outer);
}
void importedButtons() {
    for (int version : {154,156,157,159,160,166,171,177}) for (bool enabled : {false,true}) for (bool layout : {false,true}) {
        const auto raw=buttonDocument(version,enabled,layout); buttonRoundTrip(zip(raw),true,"Load Bitmap...");
        const auto decoded=FlowStoneFile::decode(zip(raw));
        check(decoded.root.children[0].children[0].node.x==12*16 && decoded.root.children[0].children[0].node.y==14*16 &&
              decoded.root.children[0].children[1].node.localValue==green::Value{std::int32_t{123}},
              "Button coordinates and the following value record remain aligned for every native property-header variant");
        Bytes prefix; primitive(prefix,17,12,14,"Load Bitmap..."); const auto at=raw.indexOf(prefix), payload=at+prefix.size();
        check(at>0,"Locate Button record");
        const auto end=payload+2+(layout ? [&] { Bytes boxes; buttonLayout(boxes,version); return boxes.size(); }() : 0);
        for (int cut=at;cut<end;++cut) expectError(zip(raw.left(cut)),"Truncated");
        for (int flag : {0,1}) { auto bad=raw; bad[payload+flag]=2; expectError(zip(bad),"Boolean"); }
        if (layout) {
            auto bad=raw; qToLittleEndian<quint32>(4097,bad.data()+payload+2); expectError(zip(bad),"record count");
            if (version>=157) { bad=raw; bad[payload+2+4+24]=2; expectError(zip(bad),"Boolean"); }
        }
        Bytes link; wire(link,version,0,0,2,0); const auto wireAt=raw.indexOf(link,end); check(wireAt>0,"Locate Button output wire");
        auto bad=raw; bad[wireAt+3]=1; expectError(zip(bad),"connector index");
        bad=raw; qToLittleEndian<quint16>(0,bad.data()+wireAt+4); expectError(zip(bad),"connector index"); // Button has no input zero.
    }
}
void nativeButtonRecord(const QString& path) {
    buttonRoundTrip(SchematicFile::read(path),false,path.contains("Switch") ? "Load Animation..." : "Load Bitmap...");
    std::cout<<path.toStdString()<<": unchanged native Button, per-click pulses and disk save/reopen passed\n";
}
void nativeButtonBitmapPicker(const QString& path) {
    using enum PrimitiveKind;
    int loads=0; const yellow::Bitmap replacement{1,2,{1,2,3,4,5,6,7,8}};
    const auto loader=[&](std::string_view selected) { check(selected=="/chosen/knob.png","Button picker passes the selected Path before Bitmap Load"); ++loads; return replacement; };
    Session s(loader); SchematicFile::importFlowStone(s,SchematicFile::read(path),{}); auto& g=*s.graph().module(1);
    check(g.nodes().size()==3 && g.nodes()[0].kind==Trigger && g.nodes()[1].kind==FileDialog && g.nodes()[2].kind==Bitmap &&
          g.links().size()==3 && g.nodes()[0].label=="Load Bitmap...","Keep the three native Button/Dialog/Bitmap records and their functional links");
    check(!s.graph().pendingAppDialog() && loads==0,"Importing the picker does not activate the Button or read a file"); bitmapValue(g,3,32,32);
    const auto verify=[&](Schematic& root) {
        auto& owner=*root.module(1); const auto before=loads;
        const auto previous=std::get<yellow::Bitmap>(owner.readValue(3));
        owner.fireTrigger(1); const auto request=*root.pendingAppDialog();
        check(request.node==2 && request.ownerPath==std::vector{1} && request.type==1,"The native Button opens its connected File Dialog in default Open mode");
        check(root.finishAppDialog(request.token,false) && loads==before && std::get<yellow::Bitmap>(owner.readValue(3))==previous,
              "Cancel leaves the Bitmap unchanged");
        owner.fireTrigger(1);
        check(root.finishAppDialog(root.pendingAppDialog()->token,true,"/chosen/knob.png") && loads==before+1 &&
              bitmapValue(owner,3,1,2)==replacement,"One Button activation and selection load the chosen bitmap exactly once");
        owner.fireTrigger(1); root.finishAppDialog(root.pendingAppDialog()->token,false);
        check(loads==before+1 && std::get<yellow::Bitmap>(owner.readValue(3))==replacement,"A later cancelled selection preserves loaded pixels");
    };
    verify(s.graph()); g.fireTrigger(1); // Save with a pending request to check that it is not replayed.
    const auto saved=SchematicFile::project(s); QTemporaryDir dir; const auto target=dir.filePath("button-picker.ostone"); SchematicFile::write(target,saved);
    Session reopened(loader); SchematicFile::loadProject(reopened,SchematicFile::read(target),{});
    check(SchematicFile::project(reopened)==saved && !reopened.graph().pendingAppDialog() && loads==1 &&
          bitmapValue(*reopened.graph().module(1),3,1,2)==replacement,"Button picker, loaded pixels and wiring survive disk save/reopen without a click or file read");
    verify(reopened.graph());
    std::cout<<path.toStdString()<<": native Button/Dialog/Bitmap chain, activation and disk save/reopen passed\n";
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

double generatorHarmonics(bool triangle, double phase, double frequency) {
    const int harmonics = std::min(1023, int(1 / std::abs(frequency)));
    double result = 0;
    for (int h = 1; h <= harmonics; ++h) {
        if (triangle && h % 2 == 0) continue;
        const double weight = triangle ? (h % 4 == 1 ? 1. : -1.) / (h * h) : 1. / h;
        result += weight * std::sin(2 * std::acos(-1.) * h * phase);
    }
    return result;
}

void generatorRecordRoundTrip(const QByteArray& archive) {
    using enum PrimitiveKind;
    Session session; SchematicFile::importFlowStone(session, archive, {});
    session.enterModule(1); auto& graph = session.graph(); const auto kind = graph.nodes()[0].kind;
    const bool oscillator = kind == Sawtooth || kind == DoubleSawtooth || kind == Triangle || kind == DoubleTriangle;
    const bool wide = kind == DoubleSawtooth || kind == DoubleTriangle;
    check(oscillator || kind == Ramp || kind == Impulse || kind == Glide, "Expected an imported signal generator");
    const auto signal = wide ? std::string("streamdouble") : std::string("stream");
    const auto mono = wide ? std::string("monodouble") : std::string("mono");
    const auto code = [&](const std::string& source) {
        const int id = graph.addNode(DspCode, 0, 0); graph.setDspDraft(id, source); session.compileDsp(id); return id;
    };
    if (oscillator) {
        const int source = code(signal + "out freq;" + signal + R"(out phase;streamboolout sync;float n;
            freq=n<12 ? .25 : n==13 ? 1 : .5;
            phase=n<3 ? .125 : n<6 ? .375 : n<8 ? 1.125 : .125;
        )" + (wide ? "phase+=0.0000000000009094947017729282379150390625;" : "") + R"(
            sync=n<3 ? false : n<6 ? true : n<8 ? false : n<10;
            n+=1;
        )");
        for (int port = 0; port < 3; ++port) graph.connect({source, 1, port, port});
    } else if (kind == Ramp) {
        const int source = code(R"(streamout freq;streamboolout sync;float n;
            freq=n<9 ? .5 : n<12 ? 1 : 0;
            sync=n<3 ? false : n<6 ? true : n<8 ? false : n<10;n+=1;)");
        graph.connect({source, 1, 0}); graph.connect({source, 1, 1, 1});
    } else if (kind == Glide) {
        const int source = code(R"(streamout target;streamout length;float n;
            target=n<19 ? .25 : n<270 ? 1.25 : n<700 ? -.75 : .5;
            length=n<150 ? 512 : n<370 ? 0 : n<700 ? 256 : 0;n+=1;)");
        graph.connect({source, 1, 0}); graph.connect({source, 1, 1, 1});
    }
    const int tap = code(mono + "in x;" + mono + "out y;y=x;"); graph.connect({1, tap, 0}); session.navigate(0);
    const auto verify = [&](Schematic& root) {
        auto& g = *root.module(1); const auto numeric = wide ? green::Type::MonoDouble : green::Type::Mono;
        check(g.nodes()[0].kind == kind && g.outputType(1) == numeric, "Imported generator retains its primitive and output precision");
        const auto& ports = g.nodes()[0].inputPorts();
        check(ports.size() == (oscillator ? 3 : kind == Impulse ? 0 : 2), "Generator retains its native input count");
        if (kind != Impulse) check(g.inputType(1, 0) == numeric, "Generator numeric input specializes with its output");
        if (oscillator || kind == Ramp)
            check(g.inputType(1, oscillator ? 2 : 1) == green::Type::MonoBoolean, "Imported hard sync is a Boolean stream");
        if (oscillator) check(g.inputType(1, 1) == numeric, "Imported oscillator Phase uses the same precision as Freq");
        if (kind == Glide) check(g.inputType(1, 1) == numeric, "Imported Glide length is a sample-rate Stream, not a Green time input");
        const int frames = oscillator ? 15 : kind == Ramp ? 14 : kind == Glide ? 804 : 65;
        const auto baseline = g.captureDsp(tap, frames, 1).front();
        if (oscillator) {
            constexpr std::array phases{.125,.25,.375,.375,.5,.625,.5,.625,.125,.25,.375,.5,.625,.875,.375};
            const bool triangle = kind == Triangle || kind == DoubleTriangle;
            // The core tests cover absolute wavetable normalization. Here one
            // calibration isolates the imported harmonic content and port routing.
            const double gain = baseline[0] / generatorHarmonics(triangle, phases[0] + (wide ? 0x1p-40 : 0), .25);
            check(gain > .5 && gain < 1, "Imported bandlimited waveform has a nonzero normalized gain");
            for (int n = 0; n < frames; ++n) {
                const auto frequency = n < 12 ? .25 : n == 13 ? 1 : .5;
                const double expected = gain * generatorHarmonics(triangle, phases[n] + (wide ? 0x1p-40 : 0), frequency);
                check(std::abs(baseline[n] - expected) < (wide ? 2e-14 : 2e-6),
                      "Imported oscillator preserves waveform, harmonic selection, wrapped phase, sync edges and double phase precision");
            }
        } else if (kind == Ramp) {
            check(baseline == std::vector{0.,.25,.5,0.,.25,.5,.75,0.,0.,.25,.75,.25,.75,.75},
                  "Imported Ramp starts at zero, wraps and resets only on rising sync edges");
        } else if (kind == Impulse) {
            auto expected = std::vector<double>(frames); expected[0] = 1;
            check(baseline == expected, "Imported Impulse emits one at startup and zero on every later sample");
        } else {
            for (int n = 0; n < frames; ++n) {
                const double expected = n < 384 ? .25 + std::max(0, n - 127) / 512.
                    : n < 768 ? .75 - std::min(256, n - 383) * 3. / 512 : .5;
                check(baseline[n] == expected, "Imported Glide adopts its initial target and follows rising, falling and zero-length steps");
            }
        }
        for (std::size_t block : {3, 7, 64, 257})
            check(g.captureDsp(tap, frames, std::min(block, std::size_t(frames))).front() == baseline,
                  "Imported generator state is independent of buffer boundaries");
    };
    verify(session.graph()); const auto saved = SchematicFile::project(session); QTemporaryDir dir;
    const auto path = dir.filePath("generator.ostone"); SchematicFile::write(path, saved);
    Session reopened; SchematicFile::loadProject(reopened, SchematicFile::read(path), {});
    check(SchematicFile::project(reopened) == saved, "Imported generators, precision, controls and wiring survive disk save/reopen");
    verify(reopened.graph());
}

void importedGeneratorRecords() {
    using enum PrimitiveKind;
    for (int version : {154, 159, 166, 171, 177}) {
        for (const auto [tag, kind] : std::array<std::pair<int, PrimitiveKind>, 10>{{
                 {27, Sawtooth}, {72, Sawtooth}, {157, Sawtooth}, {196, Sawtooth}, {1149, DoubleSawtooth},
                 {165, Triangle}, {1150, DoubleTriangle}, {32, Ramp}, {36, Impulse}, {533, Glide}}}) {
            const auto record = audioRecordDocument(version, tag);
            const auto decoded = FlowStoneFile::decode(zip(record));
            check(decoded.root.children[0].children[0].node.kind == kind, "Native generator ID and legacy alias map to the right runtime");
            generatorRecordRoundTrip(zip(record));
            Bytes bad; header(bad, version); beginModule(bad, "Root", 1); beginModule(bad, "Invalid generator", 2);
            primitive(bad, tag, 0, 0); primitive(bad, 29, 0, 0); bad.f(8); bad.zeros(2); bad.d(0);
            bad.w(1); wire(bad, version, 1, 0, 0, int(openstone::primitive(kind).inputs.size())); endModule(bad);
            bad.w(0); endModule(bad); bad.w(0); bad.b(0); expectError(zip(bad), "connector index");
            const auto& truncated = record;
            Bytes prefix; primitive(prefix, tag, 2, 3); const auto offset = truncated.indexOf(prefix);
            check(offset > 0, "Locate generator record"); expectError(zip(truncated.left(offset + 6)), "Truncated");
        }
    }
}

void nativeGeneratorRecord(const QString& path) {
    generatorRecordRoundTrip(SchematicFile::read(path));
    std::cout << path.toStdString() << ": unchanged generator record, sample checks and disk save/reopen passed\n";
}

void nativeSquareModule(const QString& path) {
    using enum PrimitiveKind;
    Session session; SchematicFile::importFlowStone(session, SchematicFile::read(path), {}); auto& graph = session.graph();
    const auto& module = graph.nodes()[0]; check(module.moduleOutputs.size() == 1, "Native Square has one output boundary");
    const int nativeInputs = module.moduleInputs.size(); check(nativeInputs == 2 || nativeInputs == 3, "Native Square has frequency/phase and optional sync inputs");
    check(std::ranges::count_if(graph.module(1)->nodes(), [](const auto& n) { return n.kind == Sawtooth; }) == 2,
          "Native Square retains both original Sawtooth records");
    const auto code = [&](const std::string& text) { const int id = graph.addNode(DspCode, 0, 0); graph.setDspDraft(id, text); session.compileDsp(id); return id; };
    const int source = code(R"(monoout freq;monoout phase;monoboolout sync;float n;
        freq=n<12 ? .25 : n==13 ? 1 : .5;
        phase=n<3 ? .125 : n<6 ? .375 : n<8 ? 1.125 : .125;
        sync=n<3 ? false : n<6 ? true : n<8 ? false : n<10;n+=1;)");
    const int tap = code("monoin x;monoout y;y=x;");
    for (int input = 0; input < nativeInputs; ++input) graph.connect({source, 1, input, input});
    graph.connect({1, tap, 0});
    const auto verify = [&](Schematic& g) {
        const auto actual = g.captureDsp(tap, 15, 1).front(); std::vector<double> shape(15);
        double phase = 0; bool previous = false;
        for (int n = 0; n < 15; ++n) {
            const double freq = n < 12 ? .25 : n == 13 ? 1 : .5;
            const double offset = n < 3 ? .125 : n < 6 ? .375 : n < 8 ? 1.125 : .125;
            const bool sync = nativeInputs == 3 && (n < 3 ? false : n < 6 ? true : n < 8 ? false : n < 10);
            if (sync && !previous) phase = 0; previous = sync;
            shape[n] = generatorHarmonics(false, phase + offset, freq) - generatorHarmonics(false, phase + offset + .5, freq);
            phase += freq * .5; phase -= std::floor(phase);
        }
        const double gain = actual[0] / shape[0]; check(gain > .5 && gain < 1, "Native Square has the expected Sawtooth normalization");
        for (int n = 0; n < 15; ++n) check(std::abs(actual[n] - gain * shape[n]) < 3e-6,
            "Unchanged Square sums phase-shifted Sawtooths with its negative gain and native sync routing");
        for (std::size_t block : {3, 7, 15}) check(g.captureDsp(tap, 15, block).front() == actual, "Native Square rendering is independent of buffer boundaries");
    };
    verify(graph); const auto saved = SchematicFile::project(session); QTemporaryDir dir; const auto file = dir.filePath("square.ostone");
    SchematicFile::write(file, saved); Session reopened; SchematicFile::loadProject(reopened, SchematicFile::read(file), {});
    check(SchematicFile::project(reopened) == saved, "Complete native Square retains its original graph through disk save/reopen"); verify(reopened.graph());
    std::cout << path.toStdString() << ": unchanged Square module, phase and sync routes, sample checks and disk save/reopen passed\n";
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

QByteArray dropListDocument(int version, std::int32_t selected, bool wired) {
    Bytes b; header(b, version); beginModule(b, "Root", 1); beginModule(b, "Drop List", wired ? 12 : 1);
    primitive(b, 237, 2, 3); b.d(std::bit_cast<quint32>(selected));
    if (wired) {
        nativePanel(b, version < 159 ? 16 : 1191, version);
        for (float value : {1.25f, 2.5f}) { primitive(b, 11, 0, 0); b.f(8); b.zeros(2); b.f(value); }
        primitive(b, 88, 0, 0); b.f(8); b.zeros(2); b.str("Zero,One,Two");
        primitive(b, 138, 0, 6, "Open"); b.d(9); b.b(0); b.str("Open");
        primitive(b, 29, 0, 0); b.f(8); b.zeros(2); b.d(quint32(-1));
        primitive(b, 88, 0, 0); b.f(8); b.zeros(2); b.str("2");
        for (int port = 0; port < 4; ++port) {
            primitive(b, 139, 20, 10 + port * 2); b.d(port == 0 ? 1 : port == 3 ? 9 : 3); b.b(0); b.str("");
        }
        b.w(11);
        for (int input = 0; input < 7; ++input) wire(b, version, input + 1, 0, 0, input);
        for (int output = 0; output < 4; ++output) wire(b, version, 0, output, output + 8, 0);
        panelFooter(b, 254);
    } else { b.w(0); endModule(b); }
    b.w(0); endModule(b); b.w(0); b.b(0); return b;
}

void dropListStateRoundTrip(const QByteArray& archive, std::int32_t selected) {
    using enum PrimitiveKind;
    Session session; SchematicFile::importFlowStone(session, archive, {});
    const auto verify = [&](Session& s) {
        auto& g = *s.graph().module(1);
        check(g.nodes()[0].kind == DropList && g.nodes()[0].localValue == green::Value{selected} &&
              g.readValue(1, 1) == green::Value{selected} && g.readValue(1) == green::Value{std::string{}},
              "Native Drop List reads its signed saved index and an empty text for a missing option list");
    };
    verify(session); const auto saved = SchematicFile::project(session); QTemporaryDir dir; const auto file = dir.filePath("drop-state.ostone");
    SchematicFile::write(file, saved); Session reopened; SchematicFile::loadProject(reopened, SchematicFile::read(file), {});
    check(SchematicFile::project(reopened) == saved, "Native Drop List signed selection survives disk save/reopen without input events"); verify(reopened);
}

void dropListConnectedRoundTrip(const QByteArray& archive) {
    using enum PrimitiveKind;
    Session session; SchematicFile::importFlowStone(session, archive, {}); auto& graph = *session.graph().module(1);
    check(graph.nodes()[0].kind == DropList && graph.nodes()[0].inputPorts().size() == 7 && graph.nodes()[0].outputPorts().size() == 4,
          "Drop List imports its seven native inputs and four outputs");
    constexpr std::array inputs{green::Type::View,green::Type::Float,green::Type::Float,green::Type::String,
                                green::Type::Trigger,green::Type::Int,green::Type::String};
    constexpr std::array outputs{green::Type::String,green::Type::Int,green::Type::Int,green::Type::Trigger};
    for (int i = 0; i < 7; ++i) check(graph.inputType(1, i) == inputs[i], "Native Drop List input order is View/X/Y/List/Open/Check/Style");
    for (int i = 0; i < 4; ++i) check(graph.outputType(1, i) == outputs[i], "Native Drop List output order is Text/Index/Count/Canceled");
    const int trigger = session.graph().addNode(Trigger, 0, 0); session.graph().connect({trigger, 1, 0}, LinkNotification::Silent);
    const auto exercise = [&](Schematic& root) {
        auto& g = *root.module(1);
        const auto pulses = [&] { return std::array{g.inspectOutput(1,0).triggers,g.inspectOutput(1,1).triggers,
                                                  g.inspectOutput(1,2).triggers,g.inspectOutput(1,3).triggers}; };
        const auto selected = [&](int index, const char* text) {
            const auto localIndex=std::get<std::int32_t>(g.readValue(1,1)), outerIndex=std::get<std::int32_t>(root.readValue(1,1));
            const auto localText=std::get<std::string>(g.readValue(1)), outerText=std::get<std::string>(root.readValue(1));
            check(localIndex==index && localText==text && outerIndex==index && outerText==text,
                  ("Drop List expected "+std::to_string(index)+":"+text+", local="+std::to_string(localIndex)+":"+localText+
                   ", module="+std::to_string(outerIndex)+":"+outerText).c_str());
        };
        const auto before = pulses();
        g.setValue(7,std::int32_t{-1}); g.setValue(5,std::string{"Reset"});
        g.setValue(5,std::string{"Red,Green,Blue"}); selected(0,"Red");
        check(pulses() == before && root.readValue(1,2) == green::Value{std::int32_t{3}}, "Native List changes reset the index and update the count without publishing pulses");
        g.setValue(7,std::int32_t{1}); selected(1,"Green");
        g.setValue(7,std::int32_t{99}); selected(1,"Green");
        check(pulses() == before, "Native Check changes select valid indices silently and ignore invalid indices");
        g.setValue(7,std::int32_t{1}); g.setValue(8,std::string{"2"}); root.fireTrigger(trigger);
        const auto request = root.pendingDropList();
        check(request && request->view.isSamePanel(g.frontPanel()->view) && request->ownerPath == std::vector<int>{1} &&
              request->position == green::Point{1.25f,2.5f} && request->checkedIndex == 1 &&
              request->layout == DropListLayout::FixedColumns && request->rowsPerColumn == 2 &&
              request->contents.options == std::vector<std::string>{"Red","Green","Blue"},
              "Native Open retains the View, coordinates, check input, style and ordered menu items");
        root.finishDropList(request->token,2); selected(2,"Blue");
        const auto chosen = pulses();
        check(chosen == std::array{before[0]+1,before[1]+1,before[2],before[3]} && !root.pendingDropList(),
              "A native menu selection sends exactly one Text and one Index pulse");
        g.setValue(7,std::int32_t{99}); selected(2,"Blue"); g.setValue(8,std::string{"Scroll"}); root.fireTrigger(trigger);
        check(root.pendingDropList()->layout == DropListLayout::Scroll, "Imported Drop List accepts a live Scroll style"); selected(0,"Red");
        const auto token = root.pendingDropList()->token; root.finishDropList(request->token,std::nullopt);
        check(root.pendingDropList() && root.pendingDropList()->token == token, "Stale menu completions cannot close a newer imported menu");
        root.finishDropList(token,std::nullopt); selected(0,"Red");
        check(pulses() == std::array{chosen[0],chosen[1],chosen[2],chosen[3]+1},
              "Native Cancel sends only Canceled and retains the silent index reset made by Open");
        // Native values are looked up in the current list even when the popup
        // snapshot was opened before that list changed.
        root.fireTrigger(trigger); const auto changing = root.pendingDropList()->token;
        g.setValue(5,std::string{"New,Names"}); root.finishDropList(changing,1); selected(1,"Names");
        root.fireTrigger(trigger); const auto removed = root.pendingDropList()->token;
        g.setValue(5,std::string{"Only"}); const auto invalidBefore = pulses(); root.finishDropList(removed,1); selected(0,"Only");
        check(pulses() == std::array{invalidBefore[0],invalidBefore[1],invalidBefore[2],invalidBefore[3]+1},
              "Removing a pending native option reports cancellation without publishing a stale selection");
        g.setValue(5,std::string{"Alpha,-----,>>,More,<<,Beta,Gamma,>>"});
        check(root.readValue(1,2) == green::Value{std::int32_t{5}},
              "Native Number of Options counts the separator and submenu header records");
        g.setValue(7,std::int32_t{-1}); selected(-1,"More");
        g.setValue(7,std::int32_t{1}); selected(-1,"More");
        root.fireTrigger(trigger);
        const auto nested = root.pendingDropList();
        check(nested && nested->contents.options == std::vector<std::string>{"Alpha","","Beta","Gamma"} &&
              nested->contents.menus.size()==2 && nested->contents.menus[0].size()==3 &&
              nested->contents.menus[0][1].optionIndex==-1 && nested->contents.menus[0][2].submenu==1 &&
              nested->contents.menus[1][0].optionIndex==2 && nested->contents.menus[1][1].optionIndex==3,
              "Imported submenus preserve native item IDs with the separator slot reserved");
        const auto separatorBefore = pulses(); root.finishDropList(nested->token,1);
        check(pulses() == std::array{separatorBefore[0],separatorBefore[1],separatorBefore[2],separatorBefore[3]+1},
              "A reserved separator ID cannot become a native selection");
        root.fireTrigger(trigger); root.finishDropList(root.pendingDropList()->token,2); selected(2,"Beta");
        g.setValue(5,std::string{}); root.fireTrigger(trigger);
        check(!root.pendingDropList() && root.readValue(1,2) == green::Value{std::int32_t{0}}, "An empty imported list does not open a popup");
        g.setValue(5,std::string{"Red,Green,Blue"}); g.setValue(7,std::int32_t{1}); root.fireTrigger(trigger);
        root.finishDropList(root.pendingDropList()->token,2); selected(2,"Blue");
    };
    exercise(session.graph()); const auto saved = SchematicFile::project(session); QTemporaryDir dir; const auto file = dir.filePath("drop-list.ostone");
    SchematicFile::write(file,saved); Session reopened; SchematicFile::loadProject(reopened,SchematicFile::read(file),{});
    check(SchematicFile::project(reopened) == saved && reopened.graph().readValue(1,1) == green::Value{std::int32_t{2}} &&
          reopened.graph().readValue(1) == green::Value{std::string{"Blue"}} && !reopened.graph().pendingDropList(),
          "Native Drop List selection, all connections and View controls survive disk save/reopen without a transient popup");
    exercise(reopened.graph());
    reopened.graph().module(1)->setValue(5,std::string{"Alpha,-----,>>,More,<<,Beta,Gamma,>>"});
    reopened.graph().fireTrigger(trigger); reopened.graph().finishDropList(reopened.graph().pendingDropList()->token,2);
    const auto nested = SchematicFile::project(reopened); const auto nestedFile = dir.filePath("native-submenu.ostone");
    SchematicFile::write(nestedFile,nested); Session nestedCopy; SchematicFile::loadProject(nestedCopy,SchematicFile::read(nestedFile),{});
    check(SchematicFile::project(nestedCopy)==nested && nestedCopy.graph().readValue(1)==green::Value{std::string{"Beta"}} &&
          nestedCopy.graph().readValue(1,1)==green::Value{std::int32_t{2}} && nestedCopy.graph().readValue(1,2)==green::Value{std::int32_t{5}},
          "Native submenu selection preserves its reserved separator ID and record count through disk save/reopen");
}

void importedDropLists() {
    for (int version : {154,159,166,171,177}) {
        for (auto index : {std::int32_t{-1},std::int32_t{0},std::int32_t{2},std::int32_t{42},
                           std::numeric_limits<std::int32_t>::min(),std::numeric_limits<std::int32_t>::max()})
            dropListStateRoundTrip(zip(dropListDocument(version,index,false)),index);
        dropListConnectedRoundTrip(zip(dropListDocument(version,1,true)));
        const auto raw = dropListDocument(version,0,false);
        Bytes marker; primitive(marker,237,2,3); const auto at = raw.indexOf(marker);
        for (int bytes = 0; bytes < 4; ++bytes) expectError(zip(raw.left(at+7+bytes)),"Truncated");
        auto bad = dropListDocument(version,0,true);
        // First wire is MGUI View -> Drop List View. Reject rather than discard
        // an extra native input or output connector.
        Bytes prefix; prefix.w(11); Bytes link; wire(link,version,1,0,0,0); prefix.append(link);
        const auto linkAt = bad.indexOf(prefix); check(linkAt > 0,"Locate native Drop List wire");
        bad[linkAt+2+6] = char(7); expectError(zip(bad),"connector index");
        bad = dropListDocument(version,0,true); bad[linkAt+2+3] = char(4); expectError(zip(bad),"connector index");
    }
    Session source; SchematicFile::importFlowStone(source,zip(dropListDocument(177,1,false)),{});
    auto doc = QJsonDocument::fromJson(SchematicFile::project(source)).object(); auto root = doc["root"].toObject();
    auto modules = root["nodes"].toArray(); auto module = modules[0].toObject(); auto children = module["nodes"].toArray();
    auto node = children[0].toObject(); node["value"] = QJsonObject{{"type",int(green::Type::String)},{"data","bad"}};
    children[0] = node; module["nodes"] = children; modules[0] = module; root["nodes"] = modules; doc["root"] = root;
    Session retained; retained.graph().addNode(PrimitiveKind::Int,0,0,std::int32_t{42}); const auto before = SchematicFile::project(retained);
    bool touched = false, rejected = false;
    try { SchematicFile::loadProject(retained,QJsonDocument(doc).toJson(),[&] { touched=true; }); }
    catch (const std::exception& e) { rejected = std::string(e.what()).find("Drop List selection state") != std::string::npos; }
    check(rejected && !touched && SchematicFile::project(retained) == before,"Malformed saved Drop List selection rejects atomically");
}

void nativeDropListRecord(const QString& path) {
    const auto index = path.contains("Detuner") ? 5 : path.contains("PWM") ? 1 : 0;
    dropListStateRoundTrip(SchematicFile::read(path),index);
    std::cout << path.toStdString() << ": unchanged Drop List record, saved index and disk save/reopen passed\n";
}

void nativeDropListItems(const QString& path) {
    using enum PrimitiveKind;
    const bool detuner = path.contains("Detuner"), lfo = path.contains("Multi LFO");
    const std::vector<std::string> options = detuner ? std::vector<std::string>{"+5","+4","+3","+2","+1","+0","-1","-2","-3","-4","-5"}
        : lfo ? std::vector<std::string>{"Sine","Sawtooth","Triangle","Square","Noise"}
              : std::vector<std::string>{"By Frequency","By Zero Cross"};
    const int initial = detuner ? 5 : 0;
    Session session; SchematicFile::importFlowStone(session,SchematicFile::read(path),{}); auto& g = *session.graph().module(1);
    check(g.nodes().size() == 3 && g.nodes()[0].kind == String && g.nodes()[2].kind == DropList &&
          g.readValue(3,1) == green::Value{std::int32_t{initial}} && g.readValue(3) == green::Value{options[initial]} &&
          session.graph().readValue(1) == green::Value{std::int32_t{initial}},
          "Bundled Items, Index boundary and Drop List records retain their original strings and saved selection");
    const int gui = g.addNode(ModuleGui,0,0), open = g.addNode(Trigger,0,0);
    g.connect({gui,3,0},LinkNotification::Silent); g.connect({open,3,4},LinkNotification::Silent);
    const auto exercise = [&](Schematic& root) {
        auto& graph = *root.module(1); graph.fireTrigger(open);
        check(root.pendingDropList() && root.pendingDropList()->contents.options == options,"Bundled option strings generate the expected ordered menu");
        root.finishDropList(root.pendingDropList()->token,int(options.size()-1));
        check(graph.readValue(3) == green::Value{options.back()} && root.readValue(1) == green::Value{std::int32_t(options.size()-1)},
              "Bundled Drop List publishes a selected item through its native Index boundary");
    };
    exercise(session.graph()); const auto saved = SchematicFile::project(session); QTemporaryDir dir; const auto file = dir.filePath("native-drop-list.ostone");
    SchematicFile::write(file,saved); Session reopened; SchematicFile::loadProject(reopened,SchematicFile::read(file),{});
    check(SchematicFile::project(reopened) == saved && reopened.graph().readValue(1) == green::Value{std::int32_t(options.size()-1)},
          "Bundled Drop List records, original options, boundary routing and selected index survive disk save/reopen");
    exercise(reopened.graph());
    std::cout << path.toStdString() << ": unchanged native Items/Index/Drop List records, headless menu and disk save/reopen passed\n";
}




struct MidiCcBench {
    int controller, counter, valueOrder, eventValue;
};
MidiCcBench wireMidiCcBench(Schematic& graph, int cc, int midiTarget, int midiInput, int ccTarget, int ccInput) {
    using enum PrimitiveKind;
    const int source=graph.addNode(VstMidiIn,0,0), controller=graph.addNode(Int,0,0,std::int32_t{64});
    const int counter=graph.addNode(TriggerCounter,0,0), order=graph.addNode(SampleAndHold,0,0), value=graph.addNode(SampleAndHold,0,0);
    for (const auto link : {SchematicLink{source,midiTarget,midiInput}, {controller,ccTarget,ccInput},
         {cc,counter,0,1}, {counter,order,0}, {cc,order,1,0}, {cc,value,0,0}, {cc,value,1,1}})
        graph.connect(link,LinkNotification::Silent);
    return {controller,counter,order,value};
}
void exerciseMidiCc(Schematic& graph, int cc, const MidiCcBench& bench) {
    using green::Value;
    check(graph.readValue(cc)==Value{std::int32_t{0}},"MIDI CC starts with zero instead of restoring a transient incoming value");
    const auto initialValue=graph.inspectOutput(cc,0).triggers, initialEvent=graph.inspectOutput(cc,1).triggers;
    const auto initialCounter=std::get<std::int32_t>(graph.readValue(bench.counter));
    std::uint64_t values=0,events=0; int current=0;
    const auto send=[&](std::initializer_list<std::uint8_t> bytes, bool matches, int next=0) {
        const int previous=current;
        graph.receiveHostMidi(green::MidiMessage{std::vector<std::uint8_t>(bytes)});
        if (matches) { ++events; if (next!=previous) ++values; current=next; }
        check(graph.readValue(cc)==Value{std::int32_t{current}} && graph.inspectOutput(cc,0).triggers==initialValue+values &&
              graph.inspectOutput(cc,1).triggers==initialEvent+events,"CC filters controller/status and emits Value only on changes, event on every match");
        check(graph.readValue(bench.counter)==Value{std::int32_t(initialCounter+events)},"Exactly one event is delivered per matching CC message");
        if (matches) {
            check(graph.readValue(bench.eventValue)==Value{std::int32_t{current}},"The event trigger samples the newly updated CC value");
            if (next!=previous) check(graph.readValue(bench.valueOrder)==Value{std::int32_t(initialCounter+events-1)},
                "The Value notification precedes the Changed event trigger");
        }
    };
    graph.setValue(bench.controller,std::int32_t{64});
    check(graph.inspectOutput(cc,1).triggers==initialEvent,"Changing the controller input never invents a MIDI event");
    send({0xb0,64,0},true,0); // A matching zero still sends the event, without a value transition.
    for (int channel : {0,1,15}) for (int value : {1,63,64,126,127,127,0})
        send({std::uint8_t(0xb0|channel),64,std::uint8_t(value)},true,value);
    send({0xb0,65,127},false); send({0x90,64,127},false); send({0x80,64,0},false);
    send({0xe0,64,127},false); send({0xc0,64},false); send({0xd0,64},false); send({0xf8},false);
    // Keep MIDI validation standard: truncated packets, stray data bytes and out-of-range data never arrive as events.
    send({},false); send({0xb0,64},false); send({0xb0,64,128},false); send({0x40,64,64},false);
    for (int number : {-1,128,std::numeric_limits<std::int32_t>::min(),std::numeric_limits<std::int32_t>::max()}) {
        graph.setValue(bench.controller,std::int32_t{number}); send({0xb0,64,127},false);
    }
    graph.setValue(bench.controller,std::int32_t{0}); send({0xbf,0,127},true,127);
    graph.setValue(bench.controller,std::int32_t{127});
    check(graph.readValue(cc)==Value{std::int32_t{127}},"Retargeting CC does not clear the previously received value");
    send({0xb0,0,1},false); send({0xb0,127,64},true,64); send({0xbf,127,64},true,64);
    send({0xbf,127,127},true,127);
}
Bytes midiCcDocument(int version, bool connected) {
    Bytes b; header(b,version); beginModule(b,"Root",1); beginModule(b,"MIDI CC bench",connected ? 6 : 1);
    if (connected) {
        primitive(b,138,0,0); b.d(4); b.b(2); b.str("MIDI");
        primitive(b,138,0,2); b.d(3); b.b(2); b.str("CC");
    }
    primitive(b,166,2,3,"Control Change");
    if (connected) {
        primitive(b,139,8,0); b.d(3); b.b(2); b.str("Value");
        primitive(b,139,8,2); b.d(9); b.b(2); b.str("Changed");
        primitive(b,29,12,3); b.f(8); b.zeros(2); b.d(0x12345678);
    }
    b.w(connected ? 4 : 0);
    if (connected) { wire(b,version,0,0,2,0); wire(b,version,1,0,2,1); wire(b,version,2,0,3,0); wire(b,version,2,1,4,0); }
    endModule(b); b.w(0); endModule(b); b.w(0); b.b(0); return b;
}
void midiCcRecordRoundTrip(const QByteArray& archive) {
    Session session; SchematicFile::importFlowStone(session,archive,{}); auto& graph=*session.graph().module(1);
    check(graph.nodes()[0].kind==PrimitiveKind::MidiCC && graph.inputType(1,0)==green::Type::MIDI && graph.inputType(1,1)==green::Type::Int &&
          graph.outputType(1,0)==green::Type::Int && graph.outputType(1,1)==green::Type::Trigger,"Preserve native MIDI/CC and Value/Changed connector order");
    const auto bench=wireMidiCcBench(graph,1,1,0,1,1); exerciseMidiCc(graph,1,bench);
    const auto saved=SchematicFile::project(session); QTemporaryDir dir; const auto path=dir.filePath("midi-cc.ostone");
    SchematicFile::write(path,saved); Session reopened; SchematicFile::loadProject(reopened,SchematicFile::read(path),{});
    check(SchematicFile::project(reopened)==saved,"MIDI CC node, selected controller and notification wiring survive disk save/reopen");
    exerciseMidiCc(*reopened.graph().module(1),1,bench);
}
void importedMidiCc() {
    for (int version : {154,159,161,166,171,177}) {
        midiCcRecordRoundTrip(zip(midiCcDocument(version,false)));
        const auto raw=midiCcDocument(version,true); Session session; SchematicFile::importFlowStone(session,zip(raw),{});
        check(session.graph().module(1)->readValue(6)==green::Value{std::int32_t{0x12345678}},"Read the next native primitive immediately after the payload-free MIDI CC record");
        auto& root=session.graph(); const auto bench=wireMidiCcBench(root,1,1,0,1,1); exerciseMidiCc(root,1,bench);
        const auto saved=SchematicFile::project(session); QTemporaryDir dir; const auto path=dir.filePath("midi-boundaries.ostone");
        SchematicFile::write(path,saved); Session reopened; SchematicFile::loadProject(reopened,SchematicFile::read(path),{});
        check(SchematicFile::project(reopened)==saved,"Preserve native MIDI/Int/Trigger module boundaries and all four MIDI CC links");
        exerciseMidiCc(reopened.graph(),1,bench);
        Bytes marker; primitive(marker,166,2,3,"Control Change"); const auto at=raw.indexOf(marker);
        check(at>0,"Locate native CC header"); for (int cut=0;cut<marker.size();++cut) expectError(zip(raw.left(at+cut)),"Truncated");
        Bytes link; wire(link,version,0,0,2,0); auto bad=raw; const auto wireAt=bad.indexOf(link,at+marker.size());
        check(wireAt>0,"Locate native CC input wire"); bad[wireAt+6]=char(2); expectError(zip(bad),"connector index");
        link.clear(); wire(link,version,2,0,3,0); bad=raw; const auto outputAt=bad.indexOf(link,wireAt);
        check(outputAt>0,"Locate native CC output wire"); bad[outputAt+3]=char(2); expectError(zip(bad),"connector index");
    }
}
void nativeMidiCcRecord(const QString& path) {
    midiCcRecordRoundTrip(SchematicFile::read(path));
    std::cout<<path.toStdString()<<": unchanged MIDI CC record, notifications and disk save/reopen passed\n";
}
void nativeMidiCcModule(const QString& path) {
    using enum PrimitiveKind;
    Session session; SchematicFile::importFlowStone(session,SchematicFile::read(path),{}); auto& graph=*session.graph().module(1);
    check(graph.nodes().size()>=9 && graph.nodes()[0].kind==MidiCC && graph.nodes()[2].kind==WirelessInput && graph.nodes()[3].kind==Divide &&
          graph.nodes()[5].kind==ModuleOutput && graph.nodes()[7].kind==Selector,"Preserve the complete native MIDI CC module, normalization and enable selector");
    const auto source=graph.addNode(VstMidiIn,0,0); graph.connect({source,1,0},LinkNotification::Silent);
    // The original blank wireless source is left intact; the authored bench supplies MIDI to the decoder.
    const auto verify=[&](Schematic& root) {
        auto& g=*root.module(1); check(g.readValue(1)==green::Value{std::int32_t{0}},"Opening a native module resets transient CC data");
        g.setValue(2,std::int32_t{74}); g.setValue(7,true);
        check(g.readValue(8)==green::Value{std::int32_t{74}},"Native enabled Selector chooses the stored CC number");
        const auto before=g.inspectOutput(1,1).triggers, outBefore=root.inspectOutput(1).triggers;
        for (int value : {0,1,64,127,127}) root.receiveHostMidi(green::MidiMessage{{0xbf,74,std::uint8_t(value)}});
        check(g.readValue(1)==green::Value{std::int32_t{127}} && root.readValue(1)==green::Value{1.f} &&
              g.inspectOutput(1,1).triggers==before+5 && root.inspectOutput(1).triggers==outBefore+3,
              "Native Value notifications reach Divide by 127 and the Float module output, with repeated values suppressed");
        root.receiveHostMidi(green::MidiMessage{{0xb0,74,64}});
        check(root.readValue(1)==green::Value{64.f/127.f},"The original module's normalization uses the exact received CC value");
        const auto events=g.inspectOutput(1,1).triggers; g.setValue(7,false);
        check(g.readValue(8)==green::Value{std::int32_t{-1}},"Native disable selects the stored -1 controller");
        root.receiveHostMidi(green::MidiMessage{{0xb0,74,127}});
        check(g.inspectOutput(1,1).triggers==events && root.readValue(1)==green::Value{64.f/127.f},"Disabled native MIDI CC keeps its value and ignores new messages");
        g.setValue(2,std::int32_t{127}); g.setValue(7,true); root.receiveHostMidi(green::MidiMessage{{0xb0,127,127}});
        check(root.readValue(1)==green::Value{1.f} && g.inspectOutput(1,1).triggers==events+1,"Native CC selection remains editable after disable/enable");
    };
    verify(session.graph()); const auto saved=SchematicFile::project(session); QTemporaryDir dir; const auto pathOut=dir.filePath("native-midi-module.ostone");
    SchematicFile::write(pathOut,saved); Session reopened; SchematicFile::loadProject(reopened,SchematicFile::read(pathOut),{});
    check(SchematicFile::project(reopened)==saved,"Complete native MIDI CC module and authored MIDI feed survive disk save/reopen");
    verify(reopened.graph()); std::cout<<path.toStdString()<<": complete native MIDI CC module, normalized values and disk save/reopen passed\n";
}
Bytes routerDocument(int version, int tag, const QByteArray& payload) {
    Bytes b; header(b,version); beginModule(b,"Root",1); beginModule(b,"Router bench",1);
    primitive(b,tag,2,3); b.append(payload); b.w(0); endModule(b); b.w(0); endModule(b); b.w(0); b.b(0); return b;
}
void multiplexerValues(const QByteArray& archive, green::Type type, PrimitiveKind sourceKind, green::Value first, green::Value next) {
    using enum PrimitiveKind;
    const green::Value empty = type==green::Type::Float ? green::Value{0.f} :
        type==green::Type::Int ? green::Value{std::int32_t{0}} :
        type==green::Type::Boolean ? green::Value{false} : green::Value{std::string{}};
    Session session; SchematicFile::importFlowStone(session,archive,{}); auto& g=*session.graph().module(1);
    const auto index=g.addNode(Int,0,0,std::int32_t{0}), source=g.addNode(sourceKind,0,0,first);
    g.connect({index,1,0},LinkNotification::Silent); g.connect({source,1,1},LinkNotification::Silent);
    const auto verify=[&](Schematic& root) {
        auto& graph=*root.module(1);
        check(graph.inputType(1,0)==green::Type::Int && graph.inputType(1,1)==type && graph.nodes()[0].outputPorts().size()==4,
              "Imported Multiplexer retains Index/Data types, three populated outputs and a spare");
        graph.setValue(source,first); graph.setValue(index,std::int32_t{-1});
        check(graph.readValue(1,0)==first && graph.readValue(1,1)==empty,"Negative native Multiplexer Index clamps to output zero");
        const auto old=graph.inspectOutput(1,0).triggers, newly=graph.inspectOutput(1,1).triggers;
        graph.setValue(index,std::int32_t{1});
        check(graph.readValue(1,0)==empty && graph.readValue(1,1)==first &&
              graph.inspectOutput(1,0).triggers==old && graph.inspectOutput(1,1).triggers==newly+1,
              "Native Multiplexer index changes notify only the newly selected output");
        graph.setValue(source,next);
        check(graph.readValue(1,1)==next && graph.inspectOutput(1,1).triggers==newly+2 && graph.inspectOutput(1,0).triggers==old,
              "Native Multiplexer data changes reach only the selected branch");
        graph.setValue(index,std::numeric_limits<std::int32_t>::max());
        check(graph.readValue(1,2)==next && graph.readValue(1,3)==empty,"Oversized Index clamps before the native spare output");
        const auto pulses=graph.inspectOutput(1,2).triggers; graph.setValue(index,std::int32_t{9});
        check(graph.inspectOutput(1,2).triggers==pulses,"Changing the raw Index within the same clamped branch sends no event");
    };
    verify(session.graph()); auto saved=SchematicFile::project(session); QTemporaryDir dir; const auto path=dir.filePath("mux.ostone");
    SchematicFile::write(path,saved); Session reopened; SchematicFile::loadProject(reopened,SchematicFile::read(path),{});
    check(SchematicFile::project(reopened)==saved,"Native Multiplexer sockets, type and routing policy survive disk save/reopen");
    verify(reopened.graph());
}
void lastSwitchRecord(const QByteArray& archive, int selected) {
    using enum PrimitiveKind;
    Session session; SchematicFile::importFlowStone(session,archive,{}); auto& g=*session.graph().module(1);
    const auto first=g.addNode(Float,0,0,10.f), second=g.addNode(Float,0,0,20.f);
    g.connect({first,1,0},LinkNotification::Silent); g.connect({second,1,1},LinkNotification::Silent);
    check(g.nodes().front().kind==LastSwitch && g.nodes().front().lastSwitchInput==selected &&
          g.readValue(1)==green::Value{selected ? 20.f : 10.f},"Native Last Switch (1054) restores the saved active input");
    const auto checkSaved=[&](Session& original) {
        QTemporaryDir dir; const auto path=dir.filePath("last-switch.ostone"); auto saved=SchematicFile::project(original);
        SchematicFile::write(path,saved); Session reopened; SchematicFile::loadProject(reopened,SchematicFile::read(path),{});
        check(SchematicFile::project(reopened)==saved && reopened.graph().module(1)->readValue(1)==original.graph().module(1)->readValue(1),
              "Last Switch active input and connected values survive disk save/reopen");
        auto& graph=*reopened.graph().module(1); graph.setValue(first,11.f);
        check(graph.readValue(1)==green::Value{11.f},"Reopened Last Switch selects an input when it fires");
        graph.setValue(second,21.f); check(graph.readValue(1)==green::Value{21.f},"Last Switch follows the other input event");
        graph.disconnectInput(1,1);
        check(graph.readValue(1)==green::Value{0.f},"An unconnected saved native Last Switch input uses its default without falling back");
    };
    checkSaved(session); g.setValue(first,12.f); g.setValue(second,22.f); checkSaved(session);
}
void multiplexerViewRecord(const QByteArray& archive) {
    using enum PrimitiveKind;
    Session session; SchematicFile::importFlowStone(session,archive,{}); auto& g=*session.graph().module(1);
    const auto panel=g.addNode(ModuleGui,0,0), index=g.addNode(Int,0,0,std::int32_t{0});
    g.connect({panel,1,1}); g.connect({index,1,0});
    for (int branch=0;branch<2;++branch) {
        const auto rect=g.addNode(Rect,0,0), bounds=g.addNode(String,0,0,std::string(branch ? "2,0,1,1" : "0,0,1,1"));
        g.connect({1,rect,0,branch}); g.connect({bounds,rect,1});
    }
    const auto verify=[&](Schematic& root) {
        auto& graph=*root.module(1); check(graph.outputType(1)==green::Type::View,"Native Multiplexer routes Views");
        for (int request : {-1,0,1,999}) {
            graph.setValue(index,std::int32_t{request});
            const auto commands=graph.panelDrawings(std::get<yellow::View>(graph.readValue(panel)));
            check(commands.size()==1 && std::get<yellow::DrawCommand>(commands[0].command).area.x==(request<1 ? 0 : 2),
                  "Only the selected native Multiplexer View branch draws, with clamped Index");
        }
    };
    verify(session.graph()); const auto saved=SchematicFile::project(session); QTemporaryDir dir; const auto path=dir.filePath("view-mux.ostone");
    SchematicFile::write(path,saved); Session reopened; SchematicFile::loadProject(reopened,SchematicFile::read(path),{});
    check(SchematicFile::project(reopened)==saved,"Native View Multiplexer links survive disk save/reopen"); verify(reopened.graph());
}
void multiplexerDsp(int version, bool doubles) {
    Bytes b; header(b,version); beginModule(b,"Root",1); beginModule(b,"DSP routes",5);
    primitive(b,149,2,3); b.d(3); b.d(doubles ? 45 : 39);
    primitive(b,29,0,0); b.f(8); b.zeros(2); b.d(0);
    const QByteArray domain=doubles ? "monodouble" : "mono";
    const QByteArray source=doubles ? "streamdoubleout out;double n;out=n+1.0000000000009094947017729282379150390625;n=n+1;" :
        "streamout out;float n;out=n+1;n=n+1;";
    codeBox(b,154,source); for (int i=0;i<2;++i) codeBox(b,154,domain+"in x;"+domain+"out out;out=x;",1);
    b.w(4); wire(b,version,1,0,0,0); wire(b,version,2,1,0,1);
    wire(b,version,0,0,3,0); wire(b,version,0,1,4,0); endModule(b); b.w(0); endModule(b); b.w(0); b.b(0);
    Session session; SchematicFile::importFlowStone(session,zip(b),{});
    const auto verify=[&](Schematic& root) {
        auto& g=*root.module(1);
        for (int request : {0,1,2,-1,999}) {
            g.setValue(2,std::int32_t{request}); const int selected=std::clamp(request,0,2);
            for (std::size_t block : {1,7,64,257}) for (int branch=0;branch<2;++branch) {
                const auto samples=g.captureDsp(4+branch,521,block).front();
                for (int i=0;i<521;++i) check(samples[i]==(selected==branch ? i+1.+(doubles ? std::ldexp(1.,-40) : 0.) : 0.),
                    "Native Multiplexer routes exact stream samples across buffer sizes, retaining double precision");
                if (selected!=branch) check(g.dspTrace(3,0,true).empty(),"An unwired native DSP route does not run its upstream code");
            }
        }
    };
    verify(session.graph()); auto saved=SchematicFile::project(session); QTemporaryDir dir; const auto path=dir.filePath("dsp-mux.ostone");
    SchematicFile::write(path,saved); Session reopened; SchematicFile::loadProject(reopened,SchematicFile::read(path),{});
    check(SchematicFile::project(reopened)==saved,"Native stream Multiplexer and unused populated outputs survive disk save/reopen");
    verify(reopened.graph());
    if (!doubles && version==177) {
        reopened.enterModule(1); auto& child=reopened.graph(); child.setDspDraft(3,"streamout out;out=.25;"); reopened.compileDsp(3);
        const auto output=child.addNode(PrimitiveKind::AudioOut,0,0);
        child.connect({1,output,0,0}); child.connect({1,output,1,1}); reopened.navigate(0);
        auto& root=reopened.graph(); auto patch=root.prepareAudio(2); root.setLiveAudio(&patch);
        for (int request : {0,1,2,-1,999,0}) {
            root.module(1)->setValue(2,std::int32_t{request}); std::array<float,4> left{},right{};
            check(root.refreshAudioControls(patch) && patch.processor().process({}, {left,right}),"Refresh imported Multiplexer routes between audio buffers");
            const int selected=std::clamp(request,0,2);
            for (int i=0;i<4;++i) check(left[i]==(selected==0 ? .25f : 0.f) && right[i]==(selected==1 ? .25f : 0.f),
                "The next live audio buffer uses the native Multiplexer's clamped route");
            if (selected==2) check(!root.module(1)->liveDspTrace(3,0,true),"An unconsumed native live-audio route removes its upstream code");
        }
        root.setLiveAudio(nullptr);
    }
}
void importedRouters() {
    using enum PrimitiveKind;
    for (int version : {154,159,166,171,177}) {
        for (int type : {0,1,2,3,15}) {
            Bytes payload; payload.d(3); payload.d(type);
            if (type==0 || type==2) multiplexerValues(zip(routerDocument(version,149,payload)),green::Type::Float,Float,1.25f,2.5f);
            if (type==1) multiplexerValues(zip(routerDocument(version,149,payload)),green::Type::String,String,std::string{"first"},std::string{"next"});
            if (type==3) multiplexerValues(zip(routerDocument(version,149,payload)),green::Type::Int,Int,std::int32_t{17},std::int32_t{-5});
            if (type==15) multiplexerValues(zip(routerDocument(version,149,payload)),green::Type::Boolean,Boolean,true,false);
        }
        for (int selected : {0,1}) { Bytes payload; mathRecord(payload,2,2,true); payload.b(selected); lastSwitchRecord(zip(routerDocument(version,1054,payload)),selected); }
        Bytes trigger; trigger.d(2); trigger.d(9); Session s; SchematicFile::importFlowStone(s,zip(routerDocument(version,149,trigger)),{});
        auto& g=*s.graph().module(1); const auto index=g.addNode(Int,0,0,std::int32_t{0}), pulse=g.addNode(Trigger,0,0);
        g.connect({index,1,0},LinkNotification::Silent); g.connect({pulse,1,1},LinkNotification::Silent);
        const auto before=g.inspectOutput(1,1).triggers; g.setValue(index,std::int32_t{1});
        check(g.inspectOutput(1,1).triggers==before+1,"A native Trigger Multiplexer emits a pulse when its output selection changes");
        g.fireTrigger(pulse); check(g.inspectOutput(1,1).triggers==before+2,"Native Multiplexer passes incoming triggers to the selected output");
        for (int count : {0,1,7,4096}) { Bytes payload; payload.d(count); payload.d(2); Session edge;
            SchematicFile::importFlowStone(edge,zip(routerDocument(version,149,payload)),{});
            check(edge.graph().module(1)->nodes()[0].outputPorts().size()==count+1,"Retain empty, single and large native Multiplexer socket lists");
            auto saved=SchematicFile::project(edge); Session reopened; SchematicFile::loadProject(reopened,saved,{});
            check(SchematicFile::project(reopened)==saved,"Retain unconnected populated Multiplexer sockets on reopen");
        }
        Bytes payload; payload.d(2); payload.d(2); const auto raw=routerDocument(version,149,payload);
        Bytes marker; primitive(marker,149,2,3); const auto at=raw.indexOf(marker)+marker.size();
        for (int cut=0;cut<8;++cut) expectError(zip(raw.left(at+cut)),"Truncated");
        payload.clear(); payload.d(0xffffffff); payload.d(2); expectError(zip(routerDocument(version,149,payload)),"record count");
        payload.clear(); payload.d(2); payload.d(999); expectError(zip(routerDocument(version,149,payload)),"connector type");
        payload.clear(); mathRecord(payload,2,2); payload.b(2); expectError(zip(routerDocument(version,1054,payload)),"Last Switch input index");
        payload.clear(); mathRecord(payload,2,2); expectError(zip(routerDocument(version,1054,payload).left(at+payload.size())),"Truncated");
    }
    for (int version : {159,177}) for (bool doubles : {false,true}) multiplexerDsp(version,doubles);
}
void nativeRouterRecord(const QString& path) {
    const auto archive=SchematicFile::read(path); const auto parsed=FlowStoneFile::decode(archive);
    const auto& node=parsed.root.children[0].children[0].node;
    if (node.kind==PrimitiveKind::Multiplexer) multiplexerViewRecord(archive);
    else lastSwitchRecord(archive,node.lastSwitchInput);
    std::cout<<path.toStdString()<<": unchanged native router, functional wiring and disk save/reopen passed\n";
}
void presetPayload(Bytes& b, int version, const std::vector<float>& values, float current, bool locked = false, float order = -1) {
    b.d(values.size()); for (float value : values) b.f(value);
    b.b(locked); b.f(current); b.f(order); b.b(0); // recording cache is refreshed from Rec
    if (version >= 162) {
        if (version >= 164) b.d(0xffffffff); // no automation binding
        if (version >= 163) b.zeros(4); else b.d(0); // empty automation lane
        if (version >= 164) b.d(0xffffffff); // cached lane index
        b.d(0); // no automation recordings
    }
}
Bytes presetDocument(int version, const QByteArray& payload, bool connected = false) {
    Bytes b; header(b,version); beginModule(b,"Root",1); beginModule(b,"Preset bench",connected ? 14 : 1);
    primitive(b,189,42,7); b.append(payload);
    if (connected) {
        const std::array<int,10> types{27,2,1,1,1,15,15,2,15,2};
        for (int i=0;i<10;++i) { primitive(b,138,0,i*2); b.d(types[i]); b.b(2); b.str(QByteArray::number(i)); }
        for (int i=0;i<3;++i) { primitive(b,139,60,i*2); b.d(i ? 9 : 2); b.b(2); b.str(QByteArray::number(i)); }
    }
    b.w(connected ? 13 : 0);
    if (connected) {
        for (int i=0;i<10;++i) wire(b,version,i+1,0,0,i);
        for (int i=0;i<3;++i) wire(b,version,0,i,11+i,0);
    }
    endModule(b); b.w(0); endModule(b); b.w(0); b.b(0); return b;
}
void presetRecordRoundTrip(const QByteArray& archive, float initial, int programs, bool locked = false) {
    using enum PrimitiveKind;
    Session s; check(SchematicFile::importFlowStone(s,archive,{}),"Import native Preset Float");
    const auto verify = [&](Session& session) {
        auto& g=*session.graph().module(1); const auto& node=g.nodes().front();
        check(node.kind==PresetFloat && node.presetParameter && node.presetParameter->flowStoneFloat &&
              node.presetParameter->flowStoneFloat->locked==locked && node.presetParameter->saved.size()==programs,
              "Preserve native preset bank and cached lock");
        check(g.readValue(1)==green::Value{initial},"Preserve native current Float without clamping");
        check(g.inputType(1,0)==green::Type::Preset && g.inputType(1,9)==green::Type::Float &&
              g.outputType(1,0)==green::Type::Float && g.outputType(1,1)==green::Type::Trigger &&
              g.outputType(1,2)==green::Type::Trigger,"Preserve native Preset connector order and floating Order");
    };
    verify(s); auto saved=SchematicFile::project(s); QTemporaryDir dir; check(dir.isValid(),"Preset temporary directory");
    const auto path=dir.filePath("preset.ostone"); SchematicFile::write(path,saved); Session reopened;
    check(SchematicFile::loadProject(reopened,SchematicFile::read(path),{}) && SchematicFile::project(reopened)==saved,
          "Native Preset current value, all bank entries, order and lock survive disk save/reopen");
    verify(reopened);
    for (auto* session : {&s,&reopened}) {
        auto& g=*session->graph().module(1); const auto before=g.nodes().front().presetParameter->saved;
        const auto edit=g.addNode(Float,0,0,0.f); g.connect({edit,1,1},LinkNotification::Silent);
        const auto count=g.inspectOutput(1).triggers;
        g.setValue(edit,1.5f);
        check(g.readValue(1)==green::Value{1.5f} && g.inspectOutput(1).triggers==count+1,
              "Unconnected native Preset edits preserve out-of-range values and emit notifications");
        check(locked ? g.nodes().front().presetParameter->saved==before :
              g.nodes().front().presetParameter->saved.at(0)==green::Value{1.5f},
              "Unconnected native Preset edits program zero unless the saved lock is on");
    }
}
void importedPresets() {
    using enum PrimitiveKind;
    for (int version : {154,159,161,162,163,164,166,171,177}) {
        Bytes payload; presetPayload(payload,version,{.125f,.5f,1.25f},2.5f,false,1.75f);
        presetRecordRoundTrip(zip(presetDocument(version,payload)),2.5f,3);
        Bytes locked; presetPayload(locked,version,{.125f,.5f,1.25f},2.5f,true,-1);
        presetRecordRoundTrip(zip(presetDocument(version,locked)),2.5f,3,true);
        Session session; check(SchematicFile::importFlowStone(session,zip(presetDocument(version,payload,true)),{}),"Import every native Preset input and output link");
        auto& root=session.graph(); auto& g=*root.module(1);
        check(root.inputType(1,0)==green::Type::Preset && root.inputType(1,9)==green::Type::Float && g.links().size()==13,
              "Native Preset bus type 27 and all thirteen connector indices bind unchanged");
        const auto manager=root.addNode(PresetManager,0,0), count=root.addNode(Int,0,0,std::int32_t{3}),
                   program=root.addNode(Int,0,0,std::int32_t{0}), edit=root.addNode(Float,0,0,0.f),
                   lock=root.addNode(Boolean,0,0,false), order=root.addNode(Float,0,0,2.25f),
                   fallback=root.addNode(Float,0,0,.375f), enabled=root.addNode(Boolean,0,0,true),
                   oldValue=root.addNode(SampleAndHold,0,0), newValue=root.addNode(SampleAndHold,0,0);
        for (const auto link : {SchematicLink{count,manager,0}, {program,manager,1}, {lock,manager,3},
                               {manager,1,0}, {edit,1,1}, {order,1,9}, {fallback,1,7},
                               {enabled,1,5}, {enabled,1,6}, {enabled,1,8},
                               {1,oldValue,0}, {1,oldValue,1,1}, {1,newValue,0}, {1,newValue,1,2}})
            root.connect(link,LinkNotification::Silent);
        const auto exercise=[&](Schematic& r) {
            auto& child=*r.module(1);
            r.setValue(program,std::int32_t{1}); check(child.readValue(1)==green::Value{.5f},"Recall imported bank program one");
            r.setValue(program,std::int32_t{2}); check(child.readValue(1)==green::Value{1.25f},"Recall raw imported bank program two");
            const auto before=child.inspectOutput(1,1).triggers, after=child.inspectOutput(1,2).triggers;
            r.setValue(edit,1.75f);
            check(child.readValue(1)==green::Value{1.75f} && child.inspectOutput(1,1).triggers==before+1 && child.inspectOutput(1,2).triggers==after+1,
                  "Preset edit sends Before, Val and After and stores the selected program");
            check(r.readValue(oldValue)==green::Value{1.25f} && r.readValue(newValue)==green::Value{1.75f},
                  "Before and After see the previous and new values through imported output links");
            r.setValue(program,std::int32_t{1}); r.setValue(program,std::int32_t{2});
            check(child.readValue(1)==green::Value{1.75f},"Recall the edited program after switching away");
            r.setValue(lock,true); r.setValue(edit,.875f);
            check(child.nodes().front().presetParameter->saved.at(2)==green::Value{1.75f},"Manager lock prevents bank writes");
            r.setValue(program,std::int32_t{1}); r.setValue(program,std::int32_t{2}); r.setValue(lock,false);
            check(child.readValue(1)==green::Value{1.75f},"Locked edits leave the imported bank intact");
            const auto parameters=r.pluginParameters();
            check(parameters.size()==1 && parameters[0].order==2.25,"Native floating-point Order remains live through module boundaries");
        };
        exercise(root); auto saved=SchematicFile::project(session); QTemporaryDir dir; const auto path=dir.filePath("connected.ostone");
        SchematicFile::write(path,saved); Session reopened; SchematicFile::loadProject(reopened,SchematicFile::read(path),{});
        check(SchematicFile::project(reopened)==saved,"Save/reopen native Preset bank with all live links");
        auto& rr=reopened.graph(); rr.setValue(program,std::int32_t{1}); rr.setValue(program,std::int32_t{2});
        check(rr.module(1)->readValue(1)==green::Value{1.75f},"Edited program recalls correctly after reopening");
        rr.setValue(edit,-.25f); rr.setValue(program,std::int32_t{1}); rr.setValue(program,std::int32_t{2});
        check(rr.module(1)->readValue(1)==green::Value{-.25f},"Reopened native Preset remains editable without clamping");
    }
    for (int version : {159,162,163,164,177}) {
        Bytes payload; presetPayload(payload,version,{.125f,.5f,1.25f},.75f);
        Bytes marker; primitive(marker,189,42,7);
        const auto raw=presetDocument(version,payload);
        const auto start=raw.indexOf(marker)+marker.size();
        for (int cut=0;cut<payload.size();++cut)
            expectError(zip(raw.left(start+cut)),"Truncated");
        auto bad=payload; for (int i=0;i<4;++i) bad[i]=char(0xff);
        expectError(zip(presetDocument(version,bad)),"record count");
        bad=payload; bad[16]=char(2); expectError(zip(presetDocument(version,bad)),"Boolean");
        bad=payload; bad[25]=char(2); expectError(zip(presetDocument(version,bad)),"Boolean");
        if (version>=162) {
            bad=payload; bad[bad.size()-4]=char(1);
            expectError(zip(presetDocument(version,bad)),"automation recordings");
            if (version>=164 || version==162) {
                bad=payload; for (int i=0;i<4;++i) bad[26+i]=0;
                bad[26]=char(1); expectError(zip(presetDocument(version,bad)),"automation bindings");
            }
            if (version>=163) {
                bad=payload; bad[version>=164 ? 30 : 26]=char(2);
                expectError(zip(presetDocument(version,bad)),"Boolean");
            }
        }
    }
    for (int version : {154,159,162,164,177}) {
        Bytes empty; presetPayload(empty,version,{},2.5f);
        Session session; SchematicFile::importFlowStone(session,zip(presetDocument(version,empty)),{});
        auto& g=*session.graph().module(1); const auto edit=g.addNode(Float,0,0,0.f);
        g.connect({edit,1,1},LinkNotification::Silent); g.setValue(edit,1.75f);
        check(g.readValue(1)==green::Value{0.f} && g.nodes().front().presetParameter->saved.empty() &&
              g.nodes().front().presetParameter->current==green::Value{2.5f},
              "Empty native banks output zero and ignore edits without losing their cached current value");
        auto saved=SchematicFile::project(session); QTemporaryDir dir; const auto path=dir.filePath("empty.ostone");
        SchematicFile::write(path,saved); Session reopened; SchematicFile::loadProject(reopened,SchematicFile::read(path),{});
        check(SchematicFile::project(reopened)==saved && reopened.graph().module(1)->readValue(1)==green::Value{0.f},
              "Empty native bank remains inactive after disk save/reopen");
        auto& r=*reopened.graph().module(1); const auto manager=r.addNode(PresetManager,0,0);
        r.connect({manager,1,0}); r.setValue(edit,.875f);
        check(r.readValue(1)==green::Value{.875f} && r.nodes().front().presetParameter->saved.at(0)==green::Value{.875f},
              "Supplying a manager program activates an empty imported Preset bank");
    }
    // The maximum bank is bounded but valid; do not truncate it to 32 programs.
    Bytes full; presetPayload(full,177,std::vector<float>(maxPresetPrograms,.625f),.875f);
    presetRecordRoundTrip(zip(presetDocument(177,full)),.875f,maxPresetPrograms);
}
void nativePresetRecord(const QString& path) {
    const auto archive=SchematicFile::read(path); const auto imported=FlowStoneFile::decode(archive);
    const float expected=path.contains("Detuner") ? std::bit_cast<float>(quint32{0x3ee8ba2f}) : 0.f;
    const auto& node=imported.root.children[0].children[0].node;
    check(node.presetParameter->saved.size()==32,"Bundled Preset bank contains all 32 programs");
    for (const auto& [program,value] : node.presetParameter->saved)
        check(value==green::Value{expected},"Decode each unchanged bundled Preset Float word");
    presetRecordRoundTrip(archive,expected,32);
    std::cout<<path.toStdString()<<": unchanged Preset bank, edits and disk save/reopen passed\n";
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

Bytes afterDuplicateDocument(int version, bool nested) {
    Bytes b; header(b,version); beginModule(b,"Root",1);
    if (nested) beginModule(b,"Outer",2);
    beginModule(b,"Lifecycle",6);
    primitive(b,551,2,3,"Copy event");
    primitive(b,326,10,3); b.d(0);
    primitive(b,258,2,6,"Load event");
    primitive(b,326,10,6); b.d(0);
    primitive(b,139,20,3); b.d(9); b.b(0); b.str("Duplicated");
    primitive(b,29,20,6,"Sentinel"); b.f(8); b.b(0); b.b(0); b.d(123);
    b.w(3); wire(b,version,0,0,1,0); wire(b,version,2,0,3,0); wire(b,version,0,0,4,0); endModule(b);
    if (nested) {
        primitive(b,139,30,3); b.d(9); b.b(0); b.str("Duplicated");
        b.w(1); wire(b,version,0,0,1,0); endModule(b);
    }
    b.w(0); endModule(b); b.w(0); b.b(0); return b;
}
Schematic& duplicateOwner(Schematic& root, int module, bool nested) {
    auto* owner=root.module(module); if (owner && nested) owner=owner->module(1);
    check(owner,"Find imported lifecycle module"); return *owner;
}
void exerciseDuplicateLifecycle(Session& s, bool nested) {
    using enum PrimitiveKind;
    using green::Value;
    auto& original=duplicateOwner(s.graph(),1,nested);
    check(original.nodes()[0].kind==AfterDuplicate && original.inspectOutput(1).triggers==0 &&
          original.readValue(2)==Value{std::int32_t{0}},"Imported After Duplicate is quiet when the schematic opens");
    const auto loadPulses=original.inspectOutput(3).triggers;
    const auto loadCount=std::get<std::int32_t>(original.readValue(4));
    original.addNode(Int,0,0,std::int32_t{7}); // Rebuilding runtime wiring must not act like a copy.
    const auto outside=s.graph().addNode(TriggerCounter,0,0);
    if (!s.graph().nodes()[0].moduleOutputs.empty()) s.graph().connect({1,outside,0},LinkNotification::Silent);
    check(original.inspectOutput(1).triggers==0,"Runtime rebuilds do not fire After Duplicate");
    const auto verifyCopy=[&](int id) {
        auto& copy=duplicateOwner(s.graph(),id,nested); auto& source=duplicateOwner(s.graph(),1,nested);
        check(!s.takeLifecycleError() && copy.inspectOutput(1).triggers==1 && copy.readValue(2)==Value{std::int32_t{1}} &&
              copy.inspectOutput(3).triggers==0 && copy.readValue(4)==Value{std::int32_t{0}},
              "Only the copied After Duplicate fires once, after its internal links exist; After Load stays quiet");
        check(source.inspectOutput(1).triggers==0 && source.readValue(2)==Value{std::int32_t{0}} &&
              source.inspectOutput(3).triggers==loadPulses && s.graph().readValue(outside)==Value{std::int32_t{0}},
              "The original module and its external receiver receive no duplicate event");
    };
    const auto first=s.duplicateNodes(std::array{1},24,24); check(first.size()==1,"Duplicate imported module"); verifyCopy(first[0]);
    s.copyNode(1);
    check(duplicateOwner(s.graph(),1,nested).inspectOutput(1).triggers==0 &&
          duplicateOwner(s.graph(),first[0],nested).inspectOutput(1).triggers==1,"Copying to the clipboard fires no lifecycle event");
    const auto pasted=s.pasteNodes(400,0,false); check(pasted.size()==1,"Paste imported module"); verifyCopy(pasted[0]);
    const auto copiedAgain=s.duplicateNodes(std::array{first[0]},24,24);
    check(copiedAgain.size()==1,"Duplicate a previously copied module"); verifyCopy(copiedAgain[0]);
    check(duplicateOwner(s.graph(),first[0],nested).readValue(2)==Value{std::int32_t{1}},"Duplicating a copy leaves that source unchanged");
    const std::array modules{1,first[0],pasted[0],copiedAgain[0]};
    // Counter observers reset when copied, while native Counter values are retained in project files.
    const std::array expected{0,1,1,1};
    const std::array loadCounts{loadCount,0,0,0};
    const auto saved=SchematicFile::project(s); QTemporaryDir dir; const auto file=dir.filePath("after-duplicate.ostone");
    SchematicFile::write(file,saved); Session reopened; SchematicFile::loadProject(reopened,SchematicFile::read(file),{});
    check(!reopened.takeLifecycleError(),"Reopen imported lifecycle graph without errors");
    for (std::size_t i=0;i<modules.size();++i) {
        auto& copy=duplicateOwner(reopened.graph(),modules[i],nested);
        check(copy.inspectOutput(1).triggers==0 && copy.readValue(2)==Value{std::int32_t(expected[i])} &&
              copy.inspectOutput(3).triggers==1 && copy.readValue(4)==Value{loadCounts[i]+1},
              "Saving/reopening preserves duplicate counters, runs After Load once and never replays After Duplicate");
    }
    const auto fresh=reopened.duplicateNodes(std::array{1},24,24); check(fresh.size()==1,"Duplicate reopened native module");
    auto& copy=duplicateOwner(reopened.graph(),fresh[0],nested);
    check(!reopened.takeLifecycleError() && copy.inspectOutput(1).triggers==1 && copy.readValue(2)==Value{std::int32_t{1}} &&
          copy.inspectOutput(3).triggers==0 && copy.readValue(4)==Value{std::int32_t{0}},"Duplicate lifecycle remains functional after reopening");
}
void importedAfterDuplicate() {
    for (int version : {154,159,161,166,171,177}) for (bool nested : {false,true}) {
        const auto raw=afterDuplicateDocument(version,nested); Session s; SchematicFile::importFlowStone(s,zip(raw),{});
        auto& owner=duplicateOwner(s.graph(),1,nested);
        check(owner.nodes()[5].localValue==green::Value{std::int32_t{123}} && owner.inspectOutput(3).triggers==1,
              "Payload-free After Duplicate preserves following records and coexists with native After Load");
        exerciseDuplicateLifecycle(s,nested);
        Bytes prefix; primitive(prefix,551,2,3,"Copy event"); const auto at=raw.indexOf(prefix); check(at>0,"Locate After Duplicate header");
        for (int cut=0;cut<prefix.size();++cut) expectError(zip(raw.left(at+cut)),"Truncated");
        Bytes link; wire(link,version,0,0,1,0); const auto wireAt=raw.indexOf(link,at+prefix.size()); check(wireAt>0,"Locate After Duplicate wire");
        auto bad=raw; bad[wireAt+3]=1; expectError(zip(bad),"connector index");
        bad=raw; qToLittleEndian<quint16>(0,bad.data()+wireAt+4); expectError(zip(bad),"connector index");
    }
}
void nativeAfterDuplicateRecord(const QString& path) {
    using enum PrimitiveKind;
    Session s; SchematicFile::importFlowStone(s,SchematicFile::read(path),{}); auto& owner=*s.graph().module(1);
    check(owner.nodes().size()==1 && owner.nodes()[0].kind==AfterDuplicate && owner.nodes()[0].inputPorts().empty() &&
          owner.nodes()[0].outputPorts().size()==1 && owner.outputType(1)==green::Type::Trigger && owner.inspectOutput(1).triggers==0,
          "Decode unchanged After Duplicate record without firing it at import");
    const auto saved=SchematicFile::project(s); QTemporaryDir dir; const auto file=dir.filePath("native-after-duplicate.ostone");
    SchematicFile::write(file,saved); Session reopened; SchematicFile::loadProject(reopened,SchematicFile::read(file),{});
    check(SchematicFile::project(reopened)==saved && reopened.graph().module(1)->inspectOutput(1).triggers==0,
          "Native lifecycle record round-trips unchanged without replay");
    auto& graph=*reopened.graph().module(1);
    const auto count=graph.addNode(BasicCounter,0,0), load=graph.addNode(AfterLoad,0,0), loadCount=graph.addNode(BasicCounter,0,0);
    check(count==2 && load==3 && loadCount==4,"Build lifecycle observers around unchanged record");
    graph.connect({1,count,0},LinkNotification::Silent); graph.connect({load,loadCount,0},LinkNotification::Silent);
    exerciseDuplicateLifecycle(reopened,false);
    std::cout<<path.toStdString()<<": native After Duplicate record, copy/paste isolation and disk save/reopen passed\n";
}
void nativeBitmapLifecycleModule(const QString& path) {
    using enum PrimitiveKind;
    int loads=0; const yellow::Bitmap replacement{2,1,{1,2,3,4,5,6,7,8}};
    const auto loader=[&](std::string_view selected) { check(selected=="/chosen/copy.png","Native copied picker reads its chosen Path"); ++loads; return replacement; };
    Session s(loader); SchematicFile::importFlowStone(s,SchematicFile::read(path),{}); auto& source=*s.graph().module(1);
    check(source.nodes().size()==8 && source.links().size()==8 && source.nodes()[6].kind==AfterDuplicate && source.nodes()[7].kind==AfterLoad &&
          source.inspectOutput(7).triggers==0 && source.inspectOutput(8).triggers==1 && loads==0,
          "Import complete unchanged Bitmap module and its distinct Load/Duplicate events");
    const auto original=std::get<yellow::Bitmap>(s.graph().readValue(1));
    check(original.width()==32 && original.height()==32 && source.readValue(1)==green::Value{std::int32_t{32}} &&
          source.readValue(2)==green::Value{std::int32_t{32}},"Native Bitmap module exposes its retained image and dimension caches");
    const auto originalEvents=s.graph().inspectOutput(1).triggers;
    const auto copied=s.duplicateNodes(std::array{1},24,24); check(copied.size()==1,"Duplicate native Bitmap loader"); const auto id=copied[0];
    auto& copy=*s.graph().module(id);
    check(!s.takeLifecycleError() && copy.inspectOutput(7).triggers==1 && copy.inspectOutput(8).triggers==0 &&
          s.graph().inspectOutput(id).triggers==1 && std::get<yellow::Bitmap>(s.graph().readValue(id))==original &&
          source.inspectOutput(7).triggers==0 && s.graph().inspectOutput(1).triggers==originalEvents && loads==0,
          "Native After Duplicate republishes the copied Bitmap through its original output link without touching the source or loading a file");
    copy.fireTrigger(6); check(s.graph().pendingAppDialog()->ownerPath==std::vector{id} && s.graph().pendingAppDialog()->node==5,
          "Copied native Button still targets its own File Dialog");
    check(s.graph().finishAppDialog(s.graph().pendingAppDialog()->token,true,"/chosen/copy.png") && loads==1 &&
          std::get<yellow::Bitmap>(s.graph().readValue(id))==replacement && copy.readValue(1)==green::Value{std::int32_t{2}} &&
          copy.readValue(2)==green::Value{std::int32_t{1}} && std::get<yellow::Bitmap>(s.graph().readValue(1))==original,
          "Native copied loader updates its own pixels and dimension monitors while the original stays unchanged");
    const auto saved=SchematicFile::project(s); QTemporaryDir dir; const auto file=dir.filePath("native-bitmap-lifecycle.ostone");
    SchematicFile::write(file,saved); Session reopened(loader); SchematicFile::loadProject(reopened,SchematicFile::read(file),{});
    check(!reopened.takeLifecycleError() && SchematicFile::project(reopened)==saved && loads==1 &&
          reopened.graph().module(1)->inspectOutput(7).triggers==0 && reopened.graph().module(id)->inspectOutput(7).triggers==0 &&
          reopened.graph().module(1)->inspectOutput(8).triggers==1 && reopened.graph().module(id)->inspectOutput(8).triggers==1 &&
          std::get<yellow::Bitmap>(reopened.graph().readValue(1))==original && std::get<yellow::Bitmap>(reopened.graph().readValue(id))==replacement,
          "Complete native loaders save/reopen with their separate images; only After Load fires");
    const auto again=reopened.duplicateNodes(std::array{id},24,24); check(again.size()==1 && !reopened.takeLifecycleError() &&
          reopened.graph().module(again[0])->inspectOutput(7).triggers==1 && reopened.graph().module(again[0])->inspectOutput(8).triggers==0 &&
          std::get<yellow::Bitmap>(reopened.graph().readValue(again[0]))==replacement && loads==1,
          "Reopened native Bitmap loader remains reusable without duplicate-time disk reads");
    std::cout<<path.toStdString()<<": complete native Bitmap module, duplicate notifications, independent loading and disk save/reopen passed\n";
}

void heldRecord(Bytes& b, quint32 type, const green::Value& initial, bool locked = true) {
    b.d(2); b.d(type); b.d(9); b.d(1); b.d(type);
    b.d(locked ? 1 : 0); if (locked) b.d(0); b.d(0);
    if (type==2) b.f(std::get<float>(initial));
    else if (type==3) b.d(std::bit_cast<quint32>(std::get<std::int32_t>(initial)));
    else if (type==15) b.b(std::get<bool>(initial));
    else if (type==1) b.str(QByteArray::fromStdString(std::get<std::string>(initial)));
}
Bytes heldDocument(int version, quint32 type, const green::Value& initial) {
    Bytes b; header(b,version); beginModule(b,"Root",1); beginModule(b,"Held value",2);
    primitive(b,1053,2,3,"Held"); heldRecord(b,type,initial);
    primitive(b,29,10,3); b.f(8); b.b(0); b.b(0); b.d(123);
    b.w(0); endModule(b); b.w(0); endModule(b); b.w(0); b.b(0); return b;
}
void importedHeldValues() {
    using enum PrimitiveKind;
    using green::Value;
    const std::vector<std::tuple<quint32,PrimitiveKind,Value,Value>> cases{
        {2,Float,1.25f,-3.5f},{3,Int,std::int32_t{-123},std::int32_t{2147483647}},
        {15,Boolean,false,true},{1,String,std::string{"before"},std::string{"after"}}};
    for (int version : {154,159,161,166,171,177}) for (const auto& [type,kind,initial,next] : cases) {
        const auto raw=heldDocument(version,type,initial); Session s; SchematicFile::importFlowStone(s,zip(raw),{}); auto& g=*s.graph().module(1);
        check(g.nodes()[0].kind==SampleAndHold && g.nodes()[0].flowStoneSampleAndHold && g.readValue(1)==initial &&
              g.readValue(2)==Value{std::int32_t{123}},"Import typed held cache and resume at the following record");
        const auto value=g.addNode(kind,0,0,next), pulse=g.addNode(Trigger,0,0);
        g.connect({value,1,0},LinkNotification::Silent); g.connect({pulse,1,1},LinkNotification::Silent);
        check(g.readValue(1)==initial && g.inspectOutput(1).triggers==0,"Value wiring leaves the imported cache untouched");
        g.setValue(value,initial); g.setValue(value,next);
        check(g.readValue(1)==initial && g.inspectOutput(1).triggers==0,"Value changes alone do not sample or publish");
        g.fireTrigger(pulse); g.fireTrigger(pulse);
        check(g.readValue(1)==next && g.inspectOutput(1).triggers==2,"Every Sample event captures the current value and publishes, including equal samples");
        const auto saved=SchematicFile::project(s); QTemporaryDir dir; const auto file=dir.filePath("held.ostone"); SchematicFile::write(file,saved);
        Session reopened; SchematicFile::loadProject(reopened,SchematicFile::read(file),{});
        check(SchematicFile::project(reopened)==saved && reopened.graph().module(1)->readValue(1)==next &&
              reopened.graph().module(1)->inspectOutput(1).triggers==0,"Native held values survive disk save/reopen without a sample event");
        const auto copy=reopened.duplicateNodes(std::array{1},24,24);
        check(copy.size()==1 && reopened.graph().module(copy[0])->readValue(1)==next &&
              reopened.graph().module(copy[0])->inspectOutput(1).triggers==0,"Copied native Sample and Hold retains its cache without firing");
        Bytes prefix; primitive(prefix,1053,2,3,"Held"); const auto at=raw.indexOf(prefix), payload=at+prefix.size();
        for (int cut=0;cut<prefix.size()+32;++cut) expectError(zip(raw.left(at+cut)),"Truncated");
        auto bad=raw; qToLittleEndian<quint32>(2,bad.data()+payload+8); expectError(zip(bad),"fixed generic connector type");
        bad=raw; qToLittleEndian<quint32>(2,bad.data()+payload+24); expectError(zip(bad),"locked connector");
        if (type==15) { bad=raw; bad[payload+32]=2; expectError(zip(bad),"Boolean"); }
    }
    // Native unresolved connectors have no cache bytes; they can specialize later.
    Session inferred; SchematicFile::importFlowStone(inferred,zip(heldDocument(177,0,{})),{}); auto& g=*inferred.graph().module(1);
    const auto value=g.addNode(Float,0,0,4.5f), pulse=g.addNode(Trigger,0,0);
    g.connect({value,1,0},LinkNotification::Silent); g.connect({pulse,1,1},LinkNotification::Silent); g.fireTrigger(pulse);
    Session restored; SchematicFile::loadProject(restored,SchematicFile::project(inferred),{});
    check(restored.graph().module(1)->readValue(1)==Value{4.5f},"A later-specialized native hold can persist its captured value");
    expectError(zip(heldDocument(177,8,{})),"cached-value type");
    // The native persistence policy must not change ordinary OpenStone holds.
    Session ordinary; auto& controls=ordinary.graph();
    const auto hold=controls.addNode(SampleAndHold,0,0), source=controls.addNode(Float,0,0,7.f), trigger=controls.addNode(Trigger,0,0);
    ordinary.connect({source,hold,0}); ordinary.connect({trigger,hold,1}); ordinary.graph().fireTrigger(trigger);
    Session fresh; SchematicFile::loadProject(fresh,SchematicFile::project(ordinary),{});
    check(fresh.graph().readValue(hold)==Value{0.f},"Ordinary Sample and Hold keeps its existing transient cache policy");
    auto doc=QJsonDocument::fromJson(SchematicFile::project(inferred)).object(); auto root=doc["root"].toObject(); auto nodes=root["nodes"].toArray();
    auto module=nodes[0].toObject(); auto children=module["nodes"].toArray(); auto node=children[0].toObject();
    node["typeOverride"]=int(green::Type::Float); node["value"]=QJsonObject{{"type",int(green::Type::Int)},{"data",7}};
    children[0]=node; module["nodes"]=children; nodes[0]=module; root["nodes"]=nodes; doc["root"]=root;
    const auto before=SchematicFile::project(ordinary); bool touched=false,rejected=false;
    try { SchematicFile::loadProject(ordinary,QJsonDocument(doc).toJson(),[&] { touched=true; }); }
    catch (const std::exception& e) { rejected=std::string(e.what()).find("native Sample and Hold cache")!=std::string::npos; }
    check(rejected && !touched && SchematicFile::project(ordinary)==before,"Invalid saved native cache rejects before replacing the project");
}
Bytes redrawLimiterDocument(int version) {
    Bytes b; header(b,version); beginModule(b,"Root",1); beginModule(b,"RedrawLimiter",4);
    primitive(b,139,20,3); b.d(2); b.b(0); b.str("Out");
    primitive(b,138,0,3); b.d(2); b.b(0); b.str("In");
    primitive(b,1053,10,3); heldRecord(b,2,std::bit_cast<float>(quint32{0x3ed5ab57}));
    primitive(b,478,10,6);
    b.w(4); wire(b,version,1,0,3,0); wire(b,version,3,0,2,1); wire(b,version,1,0,2,0); wire(b,version,2,0,0,0); endModule(b);
    b.w(0); endModule(b); b.w(0); b.b(0); return b;
}
void redrawLimiterRoundTrip(const QByteArray& archive) {
    using namespace std::chrono_literals;
    using enum PrimitiveKind;
    using green::Value;
    TickTime now{}; const Schematic::SystemInfoBackend system{.monotonicClock=[&] { return now; }};
    Session s({}, {}, {}, {}, system); SchematicFile::importFlowStone(s,archive,{}); auto& child=*s.graph().module(1);
    check(child.nodes().size()==4 && child.links().size()==4 && child.nodes()[2].kind==SampleAndHold && child.nodes()[2].flowStoneSampleAndHold &&
          child.nodes()[3].kind==RedrawLimiter && s.graph().readValue(1)==Value{std::bit_cast<float>(quint32{0x3ed5ab57})},
          "Import complete RedrawLimiter module, typed cache and original link order");
    const auto source=s.graph().addNode(Float,0,0,-100.f); s.graph().connect({source,1,0},LinkNotification::Silent);
    const auto verify=[&](Schematic& root) {
        auto& g=*root.module(1); now=TickTime{}; root.setRedrawsSuspended(false); root.setPluginHostInfo({});
        const auto before=g.inspectOutput(3).triggers; std::uint64_t passed=0; float held=std::get<float>(root.readValue(1));
        const auto send=[&](TickTime::duration time,float input,bool accept) {
            now=TickTime{time}; root.setValue(source,input); if (accept) { held=input; ++passed; }
            check(root.readValue(1)==Value{held} && g.inspectOutput(3).triggers==before+passed && g.inspectOutput(4).triggers==before+passed,
                  "Redraw gate samples the current input only on accepted events and holds it between updates");
        };
        send(0ms,.1f,true); send(0ms,.2f,false); send(9ms,.3f,false); send(9999us,.4f,false); send(10ms,.5f,true); send(20ms,.7f,true);
        root.setRedrawsSuspended(true); send(30ms,.8f,false); root.setRedrawsSuspended(false);
        check(root.readValue(1)==Value{.7f},"Resuming redraws does not synthesize a sampling event"); send(30ms,.9f,true);
        root.setPluginHostInfo(PluginHostInfo{"Host","Test","1"}); send(40ms,1.1f,false);
        root.setPluginEditorOpen(true); send(40ms,1.2f,true); root.setPluginHostInfo({});
    };
    verify(s.graph()); const auto saved=SchematicFile::project(s); QTemporaryDir dir; const auto file=dir.filePath("redraw-limiter.ostone");
    SchematicFile::write(file,saved); Session reopened({}, {}, {}, {}, system); SchematicFile::loadProject(reopened,SchematicFile::read(file),{});
    check(SchematicFile::project(reopened)==saved && reopened.graph().readValue(1)==Value{1.2f},"RedrawLimiter held output and all links survive disk save/reopen");
    verify(reopened.graph());
}
void importedRedrawLimiters() {
    for (int version : {154,159,161,166,171,177}) {
        const auto raw=redrawLimiterDocument(version); redrawLimiterRoundTrip(zip(raw));
        Bytes prefix; primitive(prefix,478,10,6); const auto at=raw.indexOf(prefix); check(at>0,"Locate redraw gate header");
        for (int cut=0;cut<prefix.size();++cut) expectError(zip(raw.left(at+cut)),"Truncated");
        Bytes link; wire(link,version,1,0,3,0); const auto wireAt=raw.indexOf(link,at+prefix.size()); check(wireAt>0,"Locate redraw gate wire");
        auto bad=raw; bad[wireAt+6]=1; expectError(zip(bad),"connector index");
        link.clear(); wire(link,version,3,0,2,1); bad=raw; const auto outputAt=bad.indexOf(link,wireAt);
        check(outputAt>0,"Locate gate output wire"); bad[outputAt+3]=1; expectError(zip(bad),"connector index");
    }
}
void nativeRedrawLimiterModule(const QString& path) {
    redrawLimiterRoundTrip(SchematicFile::read(path));
    std::cout<<path.toStdString()<<": complete native RedrawLimiter, held output, host suspension and disk save/reopen passed\n";
}

Bytes ticker25Document(int version) {
    Bytes b; header(b,version); beginModule(b,"Root",1); beginModule(b,"Ticker bench",3);
    primitive(b,181,2,3,"Refresh");
    primitive(b,326,10,3); b.d(0);
    primitive(b,29,20,6,"Sentinel"); b.f(8); b.b(0); b.b(0); b.d(123);
    b.w(1); wire(b,version,0,0,1,0); endModule(b); b.w(0); endModule(b); b.w(0); b.b(0); return b;
}
void ticker25RoundTrip(const QByteArray& archive) {
    using namespace std::chrono_literals;
    using enum PrimitiveKind;
    Session s; SchematicFile::importFlowStone(s,archive,{}); auto& g=*s.graph().module(1);
    check(g.nodes()[0].kind==Tick && g.readTick(1).rate==25 && !g.readTick(1).displaySync &&
          !g.readTick(1).wired && g.outputType(1)==green::Type::Trigger && g.inspectOutput(1).triggers==0,
          "Native Ticker 25 imports as a quiet, timer-driven Tick with 25 pulses per second");
    if(g.nodes().size()==1) { const auto counter=g.addNode(BasicCounter,0,0); g.connect({1,counter,0},LinkNotification::Silent); }
    const auto verify=[](Schematic& root) {
        auto& child=*root.module(1); const auto initial=std::get<std::int32_t>(child.readValue(2));
        const auto advance=[&](auto time,TickWake wake) {
            const auto schedule=root.advanceTicks(TickTime{}+time,wake);
            check(!schedule.error && !schedule.needsDisplay,"Imported fixed-rate ticker dispatches without display synchronization");
            return schedule;
        };
        const auto configured=advance(0ms,TickWake::Configure);
        check(!configured.fired && configured.deadline && *configured.deadline==TickTime{}+40ms && !advance(39ms,TickWake::Timer).fired,
              "Native 25 Hz ticker waits a full 40 ms period");
        check(advance(40ms,TickWake::Timer).fired && !advance(40ms,TickWake::Timer).fired &&
              child.readValue(2)==green::Value{initial+1},"Each due tick emits once through the imported trigger wire");
        const auto late=advance(125ms,TickWake::Timer);
        check(late.fired && late.deadline && *late.deadline==TickTime{}+160ms && child.readValue(2)==green::Value{initial+2},
              "Late timer wakes skip missed pulses and retain the 40 ms phase");
        check(advance(160ms,TickWake::Timer).fired && !advance(200ms,TickWake::Display).fired &&
              child.readValue(2)==green::Value{initial+3},"Display frames do not drive the imported fixed-rate ticker");
    };
    verify(s.graph()); const auto saved=SchematicFile::project(s); QTemporaryDir dir; const auto path=dir.filePath("ticker25.ostone");
    SchematicFile::write(path,saved); Session reopened; SchematicFile::loadProject(reopened,SchematicFile::read(path),{});
    check(SchematicFile::project(reopened)==saved && reopened.graph().module(1)->inspectOutput(1).triggers==0,
          "Ticker rate and wiring survive disk save/reopen without replaying events or restoring an old deadline");
    verify(reopened.graph());
}
void importedTicker25() {
    for(int version : {154,159,166,171,174,177}) {
        const auto raw=ticker25Document(version); ticker25RoundTrip(zip(raw));
        const auto parsed=FlowStoneFile::decode(zip(raw));
        check(parsed.root.children[0].children[2].node.localValue==green::Value{std::int32_t{123}},
              "Payload-free ticker preserves the following native records");
        Bytes prefix; primitive(prefix,181,2,3,"Refresh"); const auto at=raw.indexOf(prefix); check(at>0,"Locate ticker header");
        for(int cut=0;cut<prefix.size();++cut) expectError(zip(raw.left(at+cut)),"Truncated");
        Bytes link; wire(link,version,0,0,1,0); const auto wireAt=raw.indexOf(link,at+prefix.size()); check(wireAt>0,"Locate ticker wire");
        auto bad=raw; bad[wireAt+3]=1; expectError(zip(bad),"connector index");
        bad=raw; qToLittleEndian<quint16>(0,bad.data()+wireAt+4); expectError(zip(bad),"connector index");
    }
}
void nativeTicker25(const QString& path) {
    ticker25RoundTrip(SchematicFile::read(path));
    std::cout<<path.toStdString()<<": unchanged Ticker 25 record, timing, notifications and disk save/reopen passed\n";
}

Bytes stringJoinDocument(int version,int badPort=0) {
    Bytes b; header(b,version); beginModule(b,"Root",1); beginModule(b,"String join",2);
    primitive(b,573,2,3,"Join");
    primitive(b,29,10,3,"Sentinel"); b.f(8); b.b(0); b.b(0); b.d(123);
    b.w(badPort?1:0);
    if(badPort==1) wire(b,version,1,0,0,2);
    if(badPort==2) wire(b,version,0,1,1,0);
    endModule(b); b.w(0); endModule(b); b.w(0); b.b(0); return b;
}
void stringJoinRoundTrip(const QByteArray& archive) {
    using enum PrimitiveKind;
    using green::Value;
    Session s; SchematicFile::importFlowStone(s,archive,{}); auto& g=*s.graph().module(1);
    check(g.nodes()[0].kind==StringArrayToString && g.inputType(1,0)==green::Type::StringArray &&
          g.inputType(1,1)==green::Type::String && g.outputType(1)==green::Type::String &&
          g.readValue(1)==Value{std::string{}} && g.inspectOutput(1).triggers==0,
          "Native String Array to String has Array/Sep inputs and reads an empty unconnected array without firing");
    const auto array=g.addNode(Array,0,0), sep=g.addNode(String,0,0,std::string("|")), pulse=g.addNode(Trigger,0,0);
    const auto element=g.addNode(String,0,0), index=g.addNode(Int,0,0), clear=g.addNode(Trigger,0,0), insert=g.addNode(Trigger,0,0);
    g.connect({array,1,0},LinkNotification::Silent); g.connect({sep,1,1},LinkNotification::Silent);
    g.connect({pulse,sep,0},LinkNotification::Silent);
    g.connect({element,array,0},LinkNotification::Silent); g.connect({index,array,1},LinkNotification::Silent);
    g.connect({clear,array,3},LinkNotification::Silent); g.connect({insert,array,5},LinkNotification::Silent);
    const auto verify=[&](Schematic& owner) {
        const auto send=[&](green::StringArray values,std::string separator,std::string expected) {
            owner.fireTrigger(clear);
            for(std::size_t i=0;i<values.size();++i) {
                owner.setValue(index,std::int32_t(i)); owner.setValue(element,values[i]); owner.fireTrigger(insert);
            }
            owner.setValue(sep,std::move(separator));
            check(owner.readValue(1)==Value{expected},"Joining retains empty entries, Unicode and the exact separator between entries");
            const auto before=owner.inspectOutput(1).triggers; owner.fireTrigger(pulse); owner.fireTrigger(pulse);
            check(owner.inspectOutput(1).triggers==before+2 && owner.readValue(1)==Value{expected},
                  "Every separator notification republishes the joined value, including repeated equal values");
        };
        send({},"|",""); send({""},"|",""); send({"a","","Ω"},"|","a||Ω");
        send({"a","","Ω"},"","aΩ"); send({"a","","Ω"},"\r\n","a\r\n\r\nΩ");
    };
    verify(g); const auto saved=SchematicFile::project(s); QTemporaryDir dir; const auto path=dir.filePath("string-join.ostone");
    SchematicFile::write(path,saved); Session reopened; SchematicFile::loadProject(reopened,SchematicFile::read(path),{});
    check(SchematicFile::project(reopened)==saved,"Native String Array to String and its arrays, separator and links survive disk save/reopen");
    verify(*reopened.graph().module(1));
}
void importedStringJoin() {
    for(int version : {154,159,166,171,174,177}) {
        const auto raw=stringJoinDocument(version); stringJoinRoundTrip(zip(raw)); const auto parsed=FlowStoneFile::decode(zip(raw));
        check(parsed.root.children[0].children[1].node.localValue==green::Value{std::int32_t{123}},"Payload-free join preserves following records");
        Bytes prefix; primitive(prefix,573,2,3,"Join"); const auto at=raw.indexOf(prefix); check(at>0,"Locate String Array to String header");
        for(int cut=0;cut<prefix.size();++cut) expectError(zip(raw.left(at+cut)),"Truncated");
        expectError(zip(stringJoinDocument(version,1)),"connector index"); expectError(zip(stringJoinDocument(version,2)),"connector index");
    }
}
void nativeStringJoin(const QString& path) {
    stringJoinRoundTrip(SchematicFile::read(path));
    std::cout<<path.toStdString()<<": unchanged String Array to String, separator events and disk save/reopen passed\n";
}

Bytes booleanNotDocument(int version) {
    Bytes b; header(b,version); beginModule(b,"Root",1); beginModule(b,"Boolean inversion",6);
    primitive(b,138,0,3); b.d(15); b.b(0); b.str("Value");
    primitive(b,256,6,3,"Invert");
    primitive(b,139,20,3); b.d(15); b.b(0); b.str("Inverted");
    primitive(b,256,6,6,"Restore");
    primitive(b,139,20,6); b.d(15); b.b(0); b.str("Original");
    primitive(b,29,20,9,"Sentinel"); b.f(8); b.b(0); b.b(0); b.d(0x12345678);
    b.w(4); wire(b,version,0,0,1,0); wire(b,version,1,0,2,0);
    wire(b,version,1,0,3,0); wire(b,version,3,0,4,0); endModule(b);
    b.w(0); endModule(b); b.w(0); b.b(0); return b;
}
void exerciseBooleanNot(Schematic& g,int source,int pulse,int inverted,int restored=0) {
    using green::Value;
    for (bool value : {false,true,false,true}) {
        const auto expected = Value{!value};
        g.setValue(source,value);
        check(g.readValue(inverted)==expected && (!restored || g.readValue(restored)==Value{value}),
              "Imported Not negates Boolean values and two inversions restore the original");
        const auto before=g.inspectOutput(inverted).triggers;
        g.fireTrigger(pulse); g.fireTrigger(pulse);
        check(g.readValue(inverted)==expected && g.inspectOutput(inverted).triggers==before+2,
              "Not forwards every incoming notification, including repeated equal values");
    }
}
void importedBooleanNot() {
    using enum PrimitiveKind;
    using green::Value;
    for (int version : {154,159,161,165,166,171,174,177}) {
        const auto raw=booleanNotDocument(version); Session s; SchematicFile::importFlowStone(s,zip(raw),{});
        auto& root=s.graph(); auto& g=*root.module(1);
        check(g.nodes().size()==6 && g.links().size()==4 && g.nodes()[1].kind==Not && g.nodes()[3].kind==Not &&
              g.nodes()[1].label=="Invert" && g.nodes()[1].x==6*16 && g.nodes()[1].y==3*16 &&
              g.nodes()[5].localValue==Value{std::int32_t{0x12345678}},
              "Payload-free Boolean Not preserves labels, positions, following records and original wires");
        check(g.inputType(2,0)==green::Type::Boolean && g.outputType(2)==green::Type::Boolean &&
              root.inputType(1,0)==green::Type::Boolean && root.outputType(1,0)==green::Type::Boolean &&
              root.outputType(1,1)==green::Type::Boolean && root.readValue(1,0)==Value{true} && root.readValue(1,1)==Value{false},
              "Fixed Boolean connectors and both typed module outputs resolve before the first notification");
        const auto source=root.addNode(Boolean,0,0,true), pulse=root.addNode(Trigger,0,0);
        root.connect({source,1,0},LinkNotification::Silent); root.connect({pulse,source,0},LinkNotification::Silent);
        check(root.inspectOutput(1,0).triggers==0 && root.inspectOutput(1,1).triggers==0,
              "Importing and silently wiring Not does not synthesize events");
        const auto verify=[&](Schematic& graph) {
            exerciseBooleanNot(graph,source,pulse,1);
            check(graph.readValue(1,1)==graph.readValue(source),"Fan-out through a second Not reaches the second module output");
            const auto a=graph.inspectOutput(1,0).triggers, b=graph.inspectOutput(1,1).triggers;
            graph.fireTrigger(pulse);
            check(graph.inspectOutput(1,0).triggers==a+1 && graph.inspectOutput(1,1).triggers==b+1,
                  "Each Not fan-out produces exactly one event at each module output");
        };
        verify(root); const auto saved=SchematicFile::project(s); QTemporaryDir dir; const auto path=dir.filePath("boolean-not.ostone");
        SchematicFile::write(path,saved); Session reopened; SchematicFile::loadProject(reopened,SchematicFile::read(path),{});
        check(SchematicFile::project(reopened)==saved,"Boolean Not, its hierarchy, values and all links survive disk save/reopen");
        verify(reopened.graph());
        Bytes prefix; primitive(prefix,256,6,3,"Invert"); const auto at=raw.indexOf(prefix); check(at>0,"Locate Not common header");
        for (int cut=0;cut<prefix.size();++cut) expectError(zip(raw.left(at+cut)),"Truncated");
        Bytes link; wire(link,version,0,0,1,0); const auto wireAt=raw.indexOf(link,at+prefix.size()); check(wireAt>0,"Locate Not input wire");
        auto bad=raw; bad[wireAt+6]=1; expectError(zip(bad),"connector index");
        link.clear(); wire(link,version,1,0,2,0); const auto outputAt=raw.indexOf(link,wireAt); check(outputAt>0,"Locate Not output wire");
        bad=raw; bad[outputAt+3]=1; expectError(zip(bad),"connector index");
    }
}
void nativeBooleanNotRecord(const QString& path) {
    using enum PrimitiveKind;
    Session s; SchematicFile::importFlowStone(s,SchematicFile::read(path),{}); auto& g=*s.graph().module(1);
    check(g.nodes().size()==1 && g.nodes()[0].kind==Not && g.nodes()[0].x==52*16 && g.nodes()[0].y==36*16 &&
          g.nodes()[0].inputPorts().size()==1 && g.nodes()[0].outputPorts().size()==1 &&
          g.inputType(1,0)==green::Type::Boolean && g.outputType(1)==green::Type::Boolean &&
          g.readValue(1)==green::Value{true} && g.inspectOutput(1).triggers==0,
          "Unchanged native Not has fixed Boolean ports and negates an unconnected false input without firing");
    const auto source=g.addNode(Boolean,0,0,false), pulse=g.addNode(Trigger,0,0), restored=g.addNode(Not,0,0);
    g.connect({source,1,0},LinkNotification::Silent); g.connect({pulse,source,0},LinkNotification::Silent);
    g.connect({1,restored,0},LinkNotification::Silent); exerciseBooleanNot(g,source,pulse,1,restored);
    const auto saved=SchematicFile::project(s); QTemporaryDir dir; const auto output=dir.filePath("native-boolean-not.ostone");
    SchematicFile::write(output,saved); Session reopened; SchematicFile::loadProject(reopened,SchematicFile::read(output),{});
    check(SchematicFile::project(reopened)==saved,"Unchanged native Not and its connected bench survive disk save/reopen");
    exerciseBooleanNot(*reopened.graph().module(1),source,pulse,1,restored);
    std::cout<<path.toStdString()<<": native Boolean Not record, values, notifications and disk save/reopen passed\n";
}
void nativeBooleanNotPreset(const QString& path) {
    using enum PrimitiveKind;
    Session s; SchematicFile::importFlowStone(s,SchematicFile::read(path),{}); auto& root=s.graph(); auto& g=*root.module(1);
    check(g.nodes().size()==19 && g.links().size()==20 && g.nodes()[18].kind==Not &&
          g.nodes()[11].kind==ModuleInput && g.nodes()[15].kind==TriggerSwitch &&
          g.inputType(16,1)==green::Type::Boolean && g.inputType(19,0)==green::Type::Boolean,
          "Import complete native Preset module with Boolean Not and its original interaction gates");
    const auto source=root.addNode(Boolean,0,0,false), pulse=g.addNode(Trigger,0,0);
    root.connect({source,1,1},LinkNotification::Silent);
    // Supplemental data notifications exercise both original gates; their value and control routes stay intact.
    g.connect({pulse,15,0},LinkNotification::Silent); g.connect({pulse,16,0},LinkNotification::Silent);
    const auto verify=[&](Schematic& graph) {
        auto& child=*graph.module(1);
        for (bool interacting : {false,true,false,true}) {
            graph.setValue(source,interacting);
            check(child.readValue(19)==green::Value{!interacting},"Native Interact boundary drives the original Boolean Not");
            const auto edit=child.inspectOutput(15).triggers, redraw=child.inspectOutput(16).triggers;
            child.fireTrigger(pulse); child.fireTrigger(pulse);
            check(child.inspectOutput(15).triggers==edit+(interacting?2:0) &&
                  child.inspectOutput(16).triggers==redraw+(interacting?0:2),
                  "Original Preset edit gate opens during interaction while its inverted redraw gate opens only when idle");
        }
    };
    verify(root); const auto saved=SchematicFile::project(s); QTemporaryDir dir; const auto output=dir.filePath("native-preset-not.ostone");
    SchematicFile::write(output,saved); Session reopened; SchematicFile::loadProject(reopened,SchematicFile::read(output),{});
    check(SchematicFile::project(reopened)==saved,"Complete native Preset hierarchy, Boolean controls and all original links survive disk save/reopen");
    verify(reopened.graph());
    std::cout<<path.toStdString()<<": complete native Preset, complementary interaction gates and disk save/reopen passed\n";
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
        } else if (argc > 1 && QByteArray(argv[1]) == "--redraw-limiter-modules") {
            check(argc>2,"Native RedrawLimiter modules required"); for(int i=2;i<argc;++i) nativeRedrawLimiterModule(QString::fromLocal8Bit(argv[i]));
        } else if (argc > 1 && QByteArray(argv[1]) == "--boolean-not-records") {
            check(argc>2,"Native Boolean Not records required"); for(int i=2;i<argc;++i) nativeBooleanNotRecord(QString::fromLocal8Bit(argv[i]));
        } else if (argc > 1 && QByteArray(argv[1]) == "--ticker25-records") {
            check(argc>2,"Native Ticker 25 records required"); for(int i=2;i<argc;++i) nativeTicker25(QString::fromLocal8Bit(argv[i]));
        } else if (argc > 1 && QByteArray(argv[1]) == "--string-join-records") {
            check(argc>2,"Native String Array to String records required"); for(int i=2;i<argc;++i) nativeStringJoin(QString::fromLocal8Bit(argv[i]));
        } else if (argc > 1 && QByteArray(argv[1]) == "--boolean-not-presets") {
            check(argc>2,"Native Preset modules required"); for(int i=2;i<argc;++i) nativeBooleanNotPreset(QString::fromLocal8Bit(argv[i]));
        } else if (argc > 1 && QByteArray(argv[1]) == "--after-duplicate-records") {
            check(argc>2,"Native After Duplicate records required"); for(int i=2;i<argc;++i) nativeAfterDuplicateRecord(QString::fromLocal8Bit(argv[i]));
        } else if (argc > 1 && QByteArray(argv[1]) == "--bitmap-lifecycle-modules") {
            check(argc>2,"Native Bitmap lifecycle modules required"); for(int i=2;i<argc;++i) nativeBitmapLifecycleModule(QString::fromLocal8Bit(argv[i]));
        } else if (argc > 1 && QByteArray(argv[1]) == "--button-records") {
            check(argc>2,"Native Button records required"); for(int i=2;i<argc;++i) nativeButtonRecord(QString::fromLocal8Bit(argv[i]));
        } else if (argc > 1 && QByteArray(argv[1]) == "--button-bitmap-pickers") {
            check(argc>2,"Native Button picker records required"); for(int i=2;i<argc;++i) nativeButtonBitmapPicker(QString::fromLocal8Bit(argv[i]));
        } else if (argc > 1 && QByteArray(argv[1]) == "--file-dialog-records") {
            check(argc>2,"Native File Dialog records required"); for(int i=2;i<argc;++i) nativeFileDialogRecord(QString::fromLocal8Bit(argv[i]));
        } else if (argc > 1 && QByteArray(argv[1]) == "--bitmap-file-dialogs") {
            check(argc>2,"Native Bitmap/File Dialog records required"); for(int i=2;i<argc;++i) nativeBitmapFileDialog(QString::fromLocal8Bit(argv[i]));
        } else if (argc > 1 && QByteArray(argv[1]) == "--bitmap-records") {
            check(argc>2,"Native Bitmap records required"); for(int i=2;i<argc;++i) nativeBitmapRecord(QString::fromLocal8Bit(argv[i]));
        } else if (argc > 1 && QByteArray(argv[1]) == "--midi-cc-records") {
            check(argc>2,"Native MIDI CC records required"); for(int i=2;i<argc;++i) nativeMidiCcRecord(QString::fromLocal8Bit(argv[i]));
        } else if (argc > 1 && QByteArray(argv[1]) == "--midi-cc-modules") {
            check(argc>2,"Native MIDI CC modules required"); for(int i=2;i<argc;++i) nativeMidiCcModule(QString::fromLocal8Bit(argv[i]));
        } else if (argc > 1 && QByteArray(argv[1]) == "--router-records") {
            check(argc>2,"Native router records required"); for(int i=2;i<argc;++i) nativeRouterRecord(QString::fromLocal8Bit(argv[i]));
        } else if (argc > 1 && QByteArray(argv[1]) == "--preset-records") {
            check(argc > 2,"Native Preset records required"); for(int i=2;i<argc;++i) nativePresetRecord(QString::fromLocal8Bit(argv[i]));
        } else if (argc > 1 && QByteArray(argv[1]) == "--drop-list-records") {
            check(argc > 2, "Native Drop List records required"); for (int i=2; i<argc; ++i) nativeDropListRecord(QString::fromLocal8Bit(argv[i]));
        } else if (argc > 1 && QByteArray(argv[1]) == "--drop-list-items") {
            check(argc > 2, "Native Drop List items required"); for (int i=2; i<argc; ++i) nativeDropListItems(QString::fromLocal8Bit(argv[i]));
        } else if (argc > 1 && QByteArray(argv[1]) == "--generator-records") {
            check(argc > 2, "Native generator records required"); for (int i = 2; i < argc; ++i) nativeGeneratorRecord(QString::fromLocal8Bit(argv[i]));
        } else if (argc > 1 && QByteArray(argv[1]) == "--square-modules") {
            check(argc > 2, "Native Square modules required"); for (int i = 2; i < argc; ++i) nativeSquareModule(QString::fromLocal8Bit(argv[i]));
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
        } else { importsAndRoundTrips(); rejectsInvalidFiles(); codePorts(); valuesAndRuby(); linkedCodeSources(); compiledCaches(); valueLayouts(); propertyPanelLayouts(); genericMathRecords(); floatDeZipperRecord(); importedPanels(); legacyPanelControls(); inheritedViews(); propertyWireless(); guiDrawing(); guiSelectorAndRoutedLinks(); penAndFontProperties(); stringFormatsAndBrushes(); customRadialPaths(); importedShapes(); importedDrawingControls(); importedColorConversions(); importedViewCaches(); importedComparisons(); importedRouting(); importedStateRecords(); importedAudioRecords(); importedGeneratorRecords(); importedDropLists(); importedPresets(); importedRouters(); importedMidiCc(); importedBitmaps(); importedFileDialogs(); importedButtons(); importedAfterDuplicate(); importedHeldValues(); importedRedrawLimiters(); importedBooleanNot(); importedTicker25(); importedStringJoin(); heldClose(); }
        std::cout << "FlowStone import tests passed\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
