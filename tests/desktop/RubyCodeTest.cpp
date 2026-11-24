#include "RubyProcess.hpp"
#include "SchematicFile.hpp"

#include <QGuiApplication>
#include <QDir>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {

using namespace std::chrono_literals;
using namespace openstone;
using enum PrimitiveKind;
using green::StringArray;

void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void rubyLinkedSourceUpdatesAfterGreenDispatch(const QString& path)
{
    using desktop::SchematicFile;
    const auto create = [] { return Session({}, {}, {}, {}, {.ruby = desktop::makeRubyBackend()}); };
    auto original = create();
    const auto ruby = original.graph().addNode(Ruby, 0, 0);
    const std::string twice = "def init;output 0,@value*2;end\ndef event i,v;output 0,v*2;end";
    const std::string thrice = "def init;output 0,@value*3;end\ndef event i,v;output 0,v*3;end";
    original.graph().setRubyDraft(ruby, twice); original.graph().applyRubyPorts(ruby);
    // Use the persistent port layout installed by the FlowStone importer.
    auto project = QJsonDocument::fromJson(SchematicFile::project(original)).object();
    auto root = project["root"].toObject(); auto nodes = root["nodes"].toArray();
    auto node = nodes[0].toObject(); node["codeSourcePorts"] = true;
    for (const char* side : {"inputs", "outputs"}) {
        auto ports = node[side].toArray(); ports.append(static_cast<int>(green::Type::String)); node[side] = ports;
    }
    nodes[0] = node; root["nodes"] = nodes; project["root"] = root;
    auto session = create();
    check(SchematicFile::loadProject(session, QJsonDocument(project).toJson(), {}), "Load imported Ruby source-port layout");
    auto& graph = session.graph();
    const auto source = graph.addNode(String, 0, 0, twice);
    const auto value = graph.addNode(Float, 0, 0, 3.0f);
    graph.connect({source, ruby, 1}); graph.connect({value, ruby, 0}); graph.runRuby(ruby);
    check(std::get<float>(graph.readValue(ruby)) == 6, "Source port does not change Ruby input zero");
    graph.setValue(source, thrice);
    check(std::get<float>(graph.readValue(ruby)) == 9, "Editing linked source restarts a running Ruby node");
    const auto writer = graph.addNode(Ruby, 0, 0);
    graph.setRubyDraft(writer, R"ruby(# input Float go
# output String code
def init
  output 0,"def init;output 0,@value*3;end\ndef event i,v;output 0,v*3;end"
end
def event i,v
  factor = v == 7 ? 4 : 3
  output 0,"def init;output 0,@value*#{factor};end\ndef event i,v;output 0,v*#{factor};end"
end
)ruby");
    graph.runRuby(writer);
    graph.disconnectLink({source, ruby, 1}); graph.connect({writer, ruby, 1});
    const auto go = graph.addNode(Float, 0, 0, 0.0f); graph.connect({go, writer, 0});
    graph.setValue(go, 7.0f);
    check(std::get<float>(graph.readValue(ruby)) == 12, "Ruby-generated source is applied after the nested Green dispatch unwinds");
    check(std::get<std::string>(graph.readValue(ruby, 1)).find("*4") != std::string::npos, "Source output tracks running code");
    const auto saved = SchematicFile::project(session); SchematicFile::write(path, saved);
    auto reopened = create();
    check(SchematicFile::loadProject(reopened, SchematicFile::read(path), {}), "Reopen running source bindings");
    check(SchematicFile::project(reopened) == saved, "Source bindings persist without executing Ruby on reopen");
}

void runsIndependentRubyNodesAndReopens(const QString& file)
{
    TickTime now;
    const auto session = [&] {
        return Session({}, {}, {}, {}, {.monotonicClock = [&] { return now; }, .ruby = desktop::makeRubyBackend()});
    };
    auto original = session();
    auto& root = original.graph();
    const auto module = root.addNode(Module, 0, 0);
    auto& graph = *root.module(module);
    const auto first = graph.addNode(Ruby, 0, 0);
    const auto second = graph.addNode(Ruby, 0, 0);
    const auto value = graph.addNode(String, 0, 0, std::string{"one"});
    const auto go = graph.addNode(Trigger, 0, 0);
    const auto count = graph.addNode(BasicCounter, 0, 0);
    const auto code = R"ruby(# input String label
# input Trigger go
# output StringArray words
# output Trigger sent

@hits = 0
def event(i, v, t)
  case i
  when 0
    raise "bad label" if v == "raise"
  when "go"
    @hits += 1
    raise "wrong input values" unless @this == self && @in == @label && @ins[0] == @label && v.nil?
    watch i.index, t.round(2)
    output "words", [@label.upcase, @hits.to_s]
    output 1, nil
  end
end
)ruby";
    for (const auto node : {first, second}) {
        graph.setRubyDraft(node, code);
        graph.runRuby(node);
        graph.connect({value, node, 0});
    }
    graph.connect({go, first, 1});
    graph.connect({first, count, 0, 1});
    now += 50ms;
    graph.fireTrigger(go);
    graph.fireTrigger(go);
    check(std::get<StringArray>(graph.readValue(first)) == StringArray{"ONE", "2"}, "Ruby state persists between events");
    check(std::get<StringArray>(graph.readValue(second)).empty(), "Two nodes have independent Ruby instances");
    check(std::get<std::int32_t>(graph.readValue(count)) == 2, "Ruby output sends a normal Green trigger");
    check(graph.nodes().front().rubyCode->status.find("1: 0.05") != std::string::npos,
          "Ruby sees named connectors, timestamps, and watch results");

    graph.setValue(value, std::string{"raise"});
    check(graph.nodes().front().rubyCode->error, "An exception from an event is shown on the Ruby node");
    graph.setValue(value, std::string{"café"});
    check(!graph.nodes().front().rubyCode->error, "A later event can recover after a Ruby exception");
    graph.setRubyDraft(first, std::string(code) + "\ndef\n");
    bool failed = false;
    try {
        graph.runRuby(first);
    } catch (const std::runtime_error& error) {
        failed = std::string_view(error.what()).find("SyntaxError") != std::string_view::npos;
    }
    check(failed, "A Ruby syntax error is reported by the interpreter");
    graph.fireTrigger(go);
    check(std::get<StringArray>(graph.readValue(first)) == StringArray{"CAFÉ", "3"},
          "A failed Run leaves the previous program and its UTF-8 data working");

    using desktop::SchematicFile;
    SchematicFile::write(file, SchematicFile::project(original));
    auto reopened = session();
    check(SchematicFile::loadProject(reopened, SchematicFile::read(file), {}, file), "Reopen the saved Ruby project");
    auto& restored = *reopened.graph().module(module);
    check(restored.inputType(first, 0) == green::Type::String && restored.outputType(first) == green::Type::StringArray,
          "The project restores the applied Ruby connector types");
    check(restored.nodes().front().rubyCode->draft.ends_with("\ndef\n"), "The unrun draft is also saved");
    restored.fireTrigger(go);
    check(std::get<StringArray>(restored.readValue(first)).empty(), "Opening a project does not execute Ruby");
    restored.setRubyDraft(first, code);
    restored.runRuby(first);
    restored.fireTrigger(go);
    check(std::get<StringArray>(restored.readValue(first)) == StringArray{"CAFÉ", "1"},
          "The reopened Ruby node can run with its saved wiring and inputs");
    restored.stopRuby(first);
    restored.fireTrigger(go);
    check(restored.inspectOutput(first).triggers == 1, "Stop prevents subsequent calls without changing saved wiring");
}

void editsTopLevelScriptAndKeepsCompatibleWires()
{
    Schematic graph({}, {}, {}, {}, {.ruby = desktop::makeRubyBackend()});
    const auto script = graph.addNode(Ruby, 0, 0);
    const auto source = graph.addNode(Float, 0, 0, 2.5F);
    graph.connect({source, script, 0});
    graph.setRubyDraft(script, "raise \"expected Float\" unless @in.is_a?(Float); output @in * 2");
    graph.runRuby(script);
    check(std::get<float>(graph.readValue(script)) == 5, "A Ruby expression can read a connected input before an event");
    graph.setValue(source, 3.0F);
    check(std::get<float>(graph.readValue(script)) == 6, "A script with no event method re-evaluates on input changes");

    graph.setRubyDraft(script, "# input Trigger reset\n# input Float value\n# output Float result\noutput @value * 3");
    graph.runRuby(script);
    check(std::get<float>(graph.readValue(script)) == 9 && graph.links().front().input == 1,
          "Adding a port preserves links by their existing name and type");
    graph.setValue(source, 4.0F);
    check(std::get<float>(graph.readValue(script)) == 12, "The remapped link dispatches to the new port");
}

void appliesPortsWithoutEvaluatingAndSupportsUndo()
{
    Session session({}, {}, {}, {}, {.ruby = desktop::makeRubyBackend()});
    const auto script = session.graph().addNode(Ruby, 0, 0);
    const auto value = session.graph().addNode(Int, 0, 0, 42);
    session.graph().setRubyDraft(script, "# input Int amount\n# output Int total\noutput @amount * 2");
    session.beginEdit(SchematicEdit::EditRuby);
    session.graph().applyRubyPorts(script);
    session.finishEdit();
    check(session.graph().inputType(script, 0) == green::Type::Int && !session.graph().nodes().front().rubyCode->instance,
          "Apply Ports configures a script that needs a wire before it can run");
    check(session.applyHistory(false, [](auto) {}) && session.graph().inputType(script, 0) == green::Type::Float,
          "Undo restores the preceding Ruby source and ports");
    check(session.applyHistory(true, [](auto) {}) && session.graph().inputType(script, 0) == green::Type::Int,
          "Redo reapplies the Ruby source and ports");
    session.graph().connect({value, script, 0});
    session.graph().runRuby(script);
    check(std::get<std::int32_t>(session.graph().readValue(script)) == 84, "The newly wired value is available at Run");
}

void visualRubyPortsSurviveEditsAndReopening(const QString& path)
{
    using enum RubyPortEdit;
    Session session({}, {}, {}, {}, {.ruby = desktop::makeRubyBackend()});
    const auto script = session.graph().addNode(Ruby, 0, 0);
    const auto source = session.graph().addNode(Float, 0, 0, 42.F);
    const auto sink = session.graph().addNode(Float, 0, 0);
    session.graph().connect({source, script, 0});
    session.graph().connect({script, sink, 0});
    const auto edit = [&](bool output, int index, RubyPortEdit operation, int value, std::string label = {}) {
        session.beginEdit(SchematicEdit::EditRuby);
        session.graph().editRubyPort(script, output, index, operation, value, std::move(label));
        session.finishEdit();
    };
    const std::string scriptText = "def init; output @amount; end";
    session.graph().setRubyDraft(script, scriptText);
    edit(false, -1, Count, 3);
    edit(false, 0, Name, 0, "amount");
    edit(false, 0, Move, 2);
    edit(true, 0, Name, 0, "total");
    check(session.graph().links()[0].input == 2 && session.graph().links().size() == 2,
          "Renaming and reordering visual connectors preserves their existing wires");
    check(session.applyHistory(false, [](auto) {}) && session.applyHistory(false, [](auto) {}) &&
          session.graph().links()[0].input == 0, "Undo restores the original input index");
    check(session.applyHistory(true, [](auto) {}) && session.applyHistory(true, [](auto) {}) &&
          session.graph().links()[0].input == 2, "Redo restores the moved wire");
    edit(false, 2, Type, static_cast<int>(green::Type::Int));
    check(session.graph().links().size() == 1 && session.graph().inputType(script, 2) == green::Type::Int,
          "Changing a connector type detaches its old wire");
    check(session.applyHistory(false, [](auto) {}) && session.graph().links().size() == 2, "Undo restores the old type and wire");
    edit(true, 0, Insert, 0);
    edit(true, 1, Remove, 0);
    session.beginEdit(SchematicEdit::EditRuby);
    session.graph().resizeRubyBox(script, {640, 380});
    session.finishEdit();
    check(session.applyHistory(false, [](auto) {}) && session.graph().nodes().front().rubyCode->boxSize == RubyBoxSize{},
          "Resize is one undoable edit");
    check(session.applyHistory(true, [](auto) {}), "Redo the Ruby box size");

    session.graph().runRuby(script);
    check(std::get<float>(session.graph().readValue(sink)) == 42, "Visual names bind to Ruby instance variables at init");
    const auto saved = desktop::SchematicFile::saveProject(session, path);
    Session reopened({}, {}, {}, {}, {.ruby = desktop::makeRubyBackend()});
    check(desktop::SchematicFile::loadProject(reopened, saved, {}, path), "Reopen the saved visual Ruby layout");
    const auto& code = *reopened.graph().nodes().front().rubyCode;
    check(code.manualPorts && code.ports.inputs.size() == 3 && code.ports.inputs[2].name == "amount" &&
          code.ports.outputs[0].name == "total" && code.source == scriptText && code.boxSize == RubyBoxSize{640, 380},
          "The source, port order, names, and size survive a fresh session");
    reopened.graph().runRuby(script);
    check(std::get<float>(reopened.graph().readValue(sink)) == 42, "The reopened visual wiring still drives the script");
}

void rubyRunsTheFlowStoneKnobWithVisualPorts()
{
    using namespace openstone::yellow;
    using green::Type;
    using enum MouseEventKind;
    const auto photo = openstone::yellow::Bitmap(48, 48, std::vector<std::uint8_t>(48 * 48 * 4, 255));
    Schematic root([&](auto) { return photo; }, {}, {}, {}, {.ruby = desktop::makeRubyBackend()});
    auto& child = *root.module(root.addNode(Module, 0, 0));
    const int panel = child.addNode(ModuleGui, 0, 0);
    const int image = child.addNode(PrimitiveKind::Bitmap, 0, 0);
    const int value = child.addNode(Float, 0, 0, 0.25F);
    const int radius = child.addNode(Float, 0, 0, 2.F);
    const int width = child.addNode(Float, 0, 0, 0.5F);
    const int color = child.addNode(PrimitiveKind::Color, 0, 0, openstone::yellow::Color{0xFF1090C0});
    const int marker = child.addNode(Float, 0, 0, 0.15F);
    const int rotation = child.addNode(Boolean, 0, 0, true);
    const int sensitivity = child.addNode(Float, 0, 0, 4.F);
    const int script = child.addNode(Ruby, 0, 0);
    child.loadBitmap(image, "knob.png");

    const std::array types{Type::View, Type::Bitmap, Type::Float, Type::Float, Type::Float, Type::Color,
                           Type::Float, Type::Boolean, Type::Float};
    const std::array labels{"canvas", "image", "value", "radius", "width", "colour", "markerThick",
                            "showRotation", "sensitivity"};
    const std::array sources{panel, image, value, radius, width, color, marker, rotation, sensitivity};
    child.editRubyPort(script, false, -1, RubyPortEdit::Count, static_cast<int>(sources.size()));
    for (int port = 0; std::cmp_less(port, sources.size()); ++port) {
        child.editRubyPort(script, false, port, RubyPortEdit::Name, 0, labels[port]);
        child.editRubyPort(script, false, port, RubyPortEdit::Type,
                           static_cast<int>(types[port]));
        child.connect({sources[port], script, port});
    }
    child.editRubyPort(script, true, -1, RubyPortEdit::Count, 2);
    child.editRubyPort(script, true, 1, RubyPortEdit::Type, static_cast<int>(Type::Boolean));

    child.setRubyDraft(script, R"ruby(def init
  @startAngle = 135
  @sweepAngle = 270
  if @value == nil then @value = 0.0 end
end

def draw v
  angle = @value*@sweepAngle + @startAngle
  while angle < 0 do angle += 360 end
  while angle > 360 do angle -= 360 end
  v.drawBitmap @image,[(v.width-@image.width)/2.0,(v.height-@image.height)/2.0]
  osx = -0.05
  osy = -0.05
  p1 = [@radius,0]
  p2 = [@radius-@width,0]
  p1 = rotatePoint p1,v.width/2+osx,v.height/2+osy,angle
  p2 = rotatePoint p2,v.width/2+osx,v.height/2+osy,angle
  p = Pen.new @colour,@markerThick
  v.drawLine p,p1,p2
  if @showRotation
    p2 = Pen.new (Color.new 100,255,0,0),0.2
    p2.setDashStyle "dot"
    v.drawEllipse p2,[v.width/2.0-@radius+osx,v.height/2.0-@radius+osy,@radius*2,@radius*2]
  end
end

def rotatePoint p,x,y,a
  a *= Math::PI / 180.0
  px = p[0];
  py = p[1];
  p[0] = px*Math.cos(a)-py*Math.sin(a) + x
  p[1] = px*Math.sin(a)+py*Math.cos(a) + y
  return p
end

def isInMousePoint x,y
  size = getViewSize 0
  return (x-size[0]*0.5).abs < @image.width*0.5 &&
         (y-size[1]*0.5).abs < @image.height*0.5
end

def mouseLDown x,y
  captureMouse
  @downY = y
  mouseMoveCaptured x,y
  return 1
end

def mouseMoveCaptured x,y
  sensitivity = ((isKeyPressed "SHIFT") ? 10:1) * @sensitivity
  @value += (@downY-y)/sensitivity
  @downY = y
  @value = [[@value,1.0].min,0.0].max
  output 0,@value
  output 1,true
  redraw
end

def mouseLUpCaptured x,y
  releaseMouse
  output 1,false
end

def event i,v
  if i==2
    @value = [0,v,1].sort[1]
    output @value
  end
  redraw
end

def saveState
  @value
end

def loadState v
  @value=v
end
)ruby");
    child.runRuby(script);

    const auto view = child.frontPanel()->view;
    auto drawings = root.renderPanel(view);
    std::erase_if(drawings, [](const auto& layer) { return std::holds_alternative<MouseAreaCommand>(layer.command); });
    const auto& error = child.nodes().back().rubyCode->drawError;
    if (!error.empty()) throw std::runtime_error(error);
    check(drawings.size() == 3 && std::holds_alternative<BitmapDrawCommand>(drawings[0].command) &&
          std::holds_alternative<GraphDrawCommand>(drawings[1].command) &&
          std::holds_alternative<DrawCommand>(drawings[2].command), "Draw the Bitmap, marker, and ellipse in order");
    const auto& background = std::get<BitmapDrawCommand>(drawings[0].command);
    const auto& line = std::get<GraphDrawCommand>(drawings.at(1).command);
    const auto& outline = std::get<DrawCommand>(drawings.at(2).command);
    check(background.bitmap == photo && background.area.width == 6 &&
          background.area.x == (view.bounds.width - 6) / 2,
          "The Ruby View draws the centered bitmap at its natural grid size");
    check(std::get<PenStroke>(line.tool).color.argb == 0xFF1090C0 &&
          std::get<PenStroke>(outline.tool).style == LineStyle::Dot, "The marker uses its Color input and dotted outline");
    const green::Point center{view.bounds.width / 2, view.bounds.height / 2};
    check(child.rubyMouseContains(script, 0, center) && !child.rubyMouseContains(script, 0, {0, 0}),
          "getViewSize lets the knob hit-test against its bitmap");
    const auto send = [&](MouseEventKind kind, green::Point point) {
        return child.sendRubyMouseMessage(script, 0, {kind, point, MouseButton::Left});
    };
    check(send(ButtonDown, center) && std::get<bool>(child.readValue(script, 1)), "Press captures the mouse");
    check(send(Drag, {center.x, center.y - 1}) && std::get<float>(child.readValue(script)) == 0.5F,
          "A normal drag updates the Float output");
    root.sendKeyEvent(KeyboardEvent{.code = 0x10, .pressed = true});
    check(send(Drag, {center.x, center.y - 2}) && std::abs(std::get<float>(child.readValue(script)) - 0.525F) < 0.000001F,
          "Shift makes the drag ten times finer");
    check(send(ButtonUp, {0, 0}) && !std::get<bool>(child.readValue(script, 1)),
          "The captured release callback clears the Boolean even outside the knob");
    child.setValue(value, 2.F);
    check(std::get<float>(child.readValue(script)) == 1.F, "Input index two drives the knob and clamps to one");
}

void rubyValuePassesLiveArraysHashesAndCustomObjects()
{
    Schematic graph({}, {}, {}, {}, {.ruby = desktop::makeRubyBackend()});
    const auto send = graph.addNode(Trigger, 0, 0);
    const auto producer = graph.addNode(Ruby, 0, 0);
    const auto consumer = graph.addNode(Ruby, 0, 0);
    const auto observer = graph.addNode(Ruby, 0, 0);
    graph.setRubyDraft(producer, R"ruby(# input Trigger send
# output RubyValue packet

@array = [3, :tag]
@hash = {items: @array, enabled: true}
@packet = Class.new do
  attr_reader :data
  def initialize(data)
    @data = data
  end

  def score
    @data[:items].first * 2
  end

  def to_json(*)
    raise "Ruby Value must stay in the interpreter"
  end
end.new(@hash)

def event
  output "packet", @array
  output "packet", @hash
  output "packet", @packet
  output "packet", nil
end
)ruby");
    graph.setRubyDraft(consumer, R"ruby(# input RubyValue packet
# output RubyValue forwarded
# output StringArray seen

@seen = []
def event(i, v)
  raise "input value was copied" unless v.equal?(@packet) && v.equal?(@ins[0])
  case v
  when Array
    raise "array contents changed" unless v == [3, :tag]
    @array = v
    v << :received
    @seen << "array"
  when Hash
    raise "hash keys or array identity changed" unless v[:items].equal?(@array) && v[:enabled]
    @hash = v
    v[:seen] = true
    @seen << "hash"
  when NilClass
    @seen << "nil"
  else
    raise "custom object lost its shared hash" unless v.data.equal?(@hash) && v.data[:seen]
    @seen << "object=#{v.score}"
  end
  output "forwarded", v
  output "seen", @seen
end
)ruby");
    graph.setRubyDraft(observer, R"ruby(# input RubyValue forwarded
# output String summary

def event(i, v)
  case v
  when Array
    @array = v
  when Hash
    raise "forwarded object was copied" unless v[:items].equal?(@array) && v[:seen]
    @hash = v
  else
    if v
      raise "forwarded class state changed" unless v.data.equal?(@hash)
      output "summary", "#{v.score}:#{@array.last}"
    end
  end
end
)ruby");
    for (const auto node : {producer, consumer, observer}) graph.runRuby(node);
    graph.connect({send, producer, 0});
    graph.connect({producer, consumer, 0});
    graph.connect({consumer, observer, 0});
    check(graph.outputType(producer) == green::Type::RubyValue &&
          graph.inputType(consumer, 0) == green::Type::RubyValue &&
          graph.inspectOutput(consumer, 1).triggers == 0, "A Ruby Value wire waits for an event");

    graph.fireTrigger(send);
    check(std::get<StringArray>(graph.readValue(consumer, 1)) == StringArray{"array", "hash", "object=6", "nil"},
          "Arrays, hashes, custom objects, and nil reach the receiver in emission order");
    check(graph.inspectOutput(consumer, 1).triggers == 4, "Every Ruby Value emission reaches the receiver once");
    check(std::get<std::string>(graph.readValue(observer)) == "6:received",
          "Forwarding keeps Ruby class methods, shared nested objects, and mutations");
}

void rubyValueRoutesThroughSavedModulePorts(const QString& path)
{
    const auto makeSession = [] { return Session({}, {}, {}, {}, {.ruby = desktop::makeRubyBackend()}); };
    auto session = makeSession();
    auto& root = session.graph();
    const auto module = root.addNode(Module, 0, 0);
    auto& inside = *root.module(module);
    const auto producer = inside.addNode(Ruby, 0, 0);
    const auto boundary = inside.addNode(ModuleOutput, 0, 0);
    const auto consumer = root.addNode(Ruby, 0, 0);
    inside.setRubyDraft(producer, "# output RubyValue packet\noutput [99]");
    inside.applyRubyPorts(producer);
    inside.setModulePortType(boundary, green::Type::RubyValue);
    inside.connect({producer, boundary, 0});
    root.setRubyDraft(consumer, "# input RubyValue packet\n# output Int number\noutput @packet.fetch(0) if @packet");
    root.applyRubyPorts(consumer);
    root.connect({module, consumer, 0});
    root.runRuby(consumer);
    inside.runRuby(producer);
    check(std::get<std::int32_t>(root.readValue(consumer)) == 99, "Ruby Value travels out of a module");

    inside.stopRuby(producer);
    root.stopRuby(consumer);
    root.runRuby(consumer);
    check(std::get<std::int32_t>(root.readValue(consumer)) == 99,
          "A retained Ruby Value is still readable after both nodes stop");

    using desktop::SchematicFile;
    SchematicFile::write(path, SchematicFile::project(session));
    auto reopened = makeSession();
    check(SchematicFile::loadProject(reopened, SchematicFile::read(path), {}, path), "Reopen a project wired with Ruby Value");
    auto& restored = reopened.graph();
    check(restored.outputType(module) == green::Type::RubyValue &&
          restored.inputType(consumer, 0) == green::Type::RubyValue &&
          !std::get<green::RubyValue>(restored.readValue(module)).object,
          "Opening restores the port types and starts without a live object");
    restored.runRuby(consumer);
    restored.module(module)->runRuby(producer);
    check(std::get<std::int32_t>(restored.readValue(consumer)) == 99, "The reopened wire delivers a new Ruby object");
}

void rubyMidiRoutesChannelMessagesAndSysEx(const QString& file)
{
    using green::MidiMessage;
    using green::Type;
    using desktop::SchematicFile;
    TickTime now = TickTime{} + 100s;
    const auto makeSession = [&] {
        return Session({}, {}, {}, {}, {.monotonicClock = [&] { return now; }, .ruby = desktop::makeRubyBackend()});
    };
    auto session = makeSession();
    auto& graph = session.graph();
    const auto source = graph.addNode(VstMidiIn, 0, 0);
    const auto ruby = graph.addNode(Ruby, 0, 0);
    const auto sink = graph.addNode(VstMidiOut, 0, 0);
    const auto invalid = graph.addNode(Trigger, 0, 0);
    graph.setRubyDraft(ruby, R"ruby(# input MIDI message
# input Trigger invalid
# output MIDI routed

def event(i, value, t)
  if i == "invalid"
    output "routed", MIDI.new(0x90, 17, 60, 100)
    return
  end
  raise "expected a MIDI input" unless value.is_a?(MIDI) && @message.equal?(value)
  status, channel, data1, data2 = value.to_array
  case status
  when 0x90
    output "routed", MIDI.new(status, channel, data1 + 12, data2)
    output "routed", MIDI.new(0x80, channel, data1 + 12, 0), t + 0.25
  when 0xc0
    raise "expected a two-byte program change" unless [channel, data1, data2] == [3, 42, 0]
    output "routed", MIDI.new(status, channel, data1, data2)
  when String
    raise "wrong SysEx representation" unless value.to_array == ["F07D01F7", 4]
    output "routed", MIDI.new("f0 7d 02 f7")
    output "routed", value
  else
    output "routed", value
  end
end
)ruby");
    graph.applyRubyPorts(ruby);
    graph.connect({source, ruby, 0});
    graph.connect({ruby, sink, 0});
    graph.connect({invalid, ruby, 1}, LinkNotification::Silent);

    std::vector<MidiMessage> sent;
    graph.setHostMidiOutput([&](const MidiMessage& message) { sent.push_back(message); });
    graph.runRuby(ruby);
    check(graph.inputType(ruby, 0) == Type::MIDI && graph.outputType(ruby) == Type::MIDI && sent.empty(),
          "Ruby can wire directly to MIDI primitives without emitting any message at startup");

    const MidiMessage noteOn{{0x9f, 72, 100}};
    const MidiMessage noteOff{{0x8f, 72, 0}};
    graph.receiveHostMidi({{0x9f, 60, 100}});
    check(sent == std::vector{noteOn}, "Ruby reads the channel and transposes the note-on immediately");
    now += 249ms;
    auto wake = graph.advanceTicks(now, TickWake::Timer);
    check(!wake.fired && sent == std::vector{noteOn}, "The MIDI note-off waits for its timestamp");
    now += 1ms;
    wake = graph.advanceTicks(now, TickWake::Timer);
    check(wake.fired && !wake.error && sent == std::vector{noteOn, noteOff} &&
          std::get<MidiMessage>(graph.readValue(ruby)) == noteOff,
          "The scheduler delivers the MIDI note-off and updates the held outlet value");

    graph.fireTrigger(invalid);
    check(sent == std::vector{noteOn, noteOff} && graph.inspectOutput(ruby).triggers == 2,
          "An out-of-range MIDI channel cannot emit a partial message");
    const MidiMessage program{{0xc2, 42}};
    graph.receiveHostMidi(program);
    check(sent.back() == program && sent.size() == 3, "Program Change retains its two-byte wire format");

    SchematicFile::saveProject(session, file);
    auto reopened = makeSession();
    check(SchematicFile::loadProject(reopened, SchematicFile::read(file), {}, file), "Reopen the Ruby MIDI schematic");
    auto& restored = reopened.graph();
    sent.clear();
    restored.setHostMidiOutput([&](const MidiMessage& message) { sent.push_back(message); });
    restored.runRuby(ruby);
    const MidiMessage sysex{{0xf0, 0x7d, 0x01, 0xf7}};
    const MidiMessage generated{{0xf0, 0x7d, 0x02, 0xf7}};
    const MidiMessage clock{{0xf8}};
    restored.receiveHostMidi(sysex);
    restored.receiveHostMidi(clock);
    check(restored.inputType(ruby, 0) == Type::MIDI && restored.outputType(ruby) == Type::MIDI &&
          sent == std::vector{generated, sysex, clock},
          "Saved MIDI connectors preserve generated and relayed SysEx plus real-time passthrough");
}

void rubySchedulesInputsAndOutputsAgainstSchematicClock()
{
    TickTime now = TickTime{} + 100s;
    Schematic graph({}, {}, {}, {}, {.monotonicClock = [&] { return now; }, .ruby = desktop::makeRubyBackend()});
    const auto source = graph.addNode(Ruby, 0, 0);
    const auto listener = graph.addNode(Ruby, 0, 0);
    graph.setRubyDraft(source, R"ruby(# input Int step
# output Int result
# output String state

output "result", 100
output "result", 200, time + 0.25
input "step", 7, time + 0.375

def event(i, v, t)
  raise "wrong callback time" unless time == t
  output "state", "#{i.name}:#{@ins[0]}:#{@outs[0]}"
  output "result", v + @outs[0]
  input i, v + 1, t + 0.125 if v == 7
end
)ruby");
    graph.setRubyDraft(listener, R"ruby(# input Int number
# output StringArray trace

@trace = []
def event(i, v, t)
  @trace << format("%d@%.3f", v, t)
  output @trace
end
)ruby");
    graph.applyRubyPorts(source);
    graph.applyRubyPorts(listener);
    graph.connect({source, listener, 0});
    graph.runRuby(listener);
    now += 3s;
    graph.runRuby(source);
    const auto initial = now;
    const auto trace = [&] { return std::get<StringArray>(graph.readValue(listener)); };
    const auto configured = graph.advanceTicks(now, TickWake::Configure);
    check(!configured.fired && configured.deadline == initial + 250ms && trace() == StringArray{"100@3.000"},
          "Immediate output delivers while a future output arms the timer in schematic time");
    now = initial + 249ms;
    check(!graph.advanceTicks(now, TickWake::Timer).fired && graph.inspectOutput(source).triggers == 1,
          "The delayed output stays pending before its timestamp");
    now = initial + 250ms;
    const auto first = graph.advanceTicks(now, TickWake::Timer);
    check(first.fired && !first.error && first.deadline == initial + 375ms &&
          trace() == StringArray{"100@3.000", "200@3.250"},
          "The output reaches the wire at its absolute timestamp");
    now = initial + 510ms;
    const auto late = graph.advanceTicks(now, TickWake::Timer);
    check(late.fired && !late.error && !late.deadline && std::get<std::string>(graph.readValue(source, 1)) == "step:8:207",
          "Delayed inputs update named inputs and see earlier scheduled outputs in @outs");
    check(trace() == StringArray{"100@3.000", "200@3.250", "207@3.375", "215@3.500"},
          "A late wake keeps the scheduled time and repeats without shifting its phase");
}

void rubyQueueOrdersAcrossNodesAndKeepsMethodArguments()
{
    TickTime now{};
    Schematic graph({}, {}, {}, {}, {.monotonicClock = [&] { return now; }, .ruby = desktop::makeRubyBackend()});
    const auto module = graph.addNode(Module, 0, 0);
    auto& child = *graph.module(module);
    const auto first = child.addNode(Ruby, 0, 0);
    const auto boundary = child.addNode(ModuleOutput, 0, 0);
    const auto second = graph.addNode(Ruby, 0, 0);
    const auto listener = graph.addNode(Ruby, 0, 0);
    child.setRubyDraft(first, R"ruby(# output RubyValue message

def emit(record)
  output record[:name]
  output 0, {name: "same time, enqueued later"}, time
end

record = {name: {name: "method"}}
output 0, {name: "later"}, time + 1.0
scheduleMethod "emit", record, time + 0.5
output 0, {name: "same time, after method"}, time + 0.5
GC.start
)ruby");
    graph.setRubyDraft(second, R"ruby(# input Trigger repeat
# output RubyValue message

def event
  output({name: "input"})
end

input "repeat", nil, time + 0.5
)ruby");
    graph.setRubyDraft(listener, R"ruby(# input RubyValue message
# output StringArray trace

@trace = []
def event(i, v)
  @trace << v.fetch(:name)
  output @trace
end
)ruby");
    child.applyRubyPorts(first);
    graph.applyRubyPorts(second);
    graph.applyRubyPorts(listener);
    child.setModulePortType(boundary, green::Type::RubyValue);
    child.connect({first, boundary, 0});
    graph.connect({module, listener, 0});
    graph.connect({second, listener, 0});
    graph.runRuby(listener);
    child.runRuby(first);
    graph.runRuby(second);
    now += 1s;
    const auto result = graph.advanceTicks(now, TickWake::Timer);
    check(result.fired && !result.error && !result.deadline, "Drain all due Ruby actions across modules");
    check(std::get<StringArray>(graph.readValue(listener)) ==
          StringArray{"method", "same time, after method", "input", "same time, enqueued later", "later"},
          "Ruby actions use deadline then submission order, including new events at the same time");
}

void rubyClearEventsCancelsOnlyTheCallingNode()
{
    TickTime now{};
    Schematic graph({}, {}, {}, {}, {.monotonicClock = [&] { return now; }, .ruby = desktop::makeRubyBackend()});
    const auto cancellable = graph.addNode(Ruby, 0, 0);
    const auto other = graph.addNode(Ruby, 0, 0);
    graph.setRubyDraft(cancellable, R"ruby(# input Trigger cancel
# input Trigger discarded
# output String result

def obsolete
  output "discarded method"
end

def event(i)
  if i == "cancel"
    output "result", "already sent"
    input "discarded", nil, time + 0.5
    clearEvents
    output "result", "kept", time + 1.0
  else
    output "discarded input"
  end
end

input "cancel", nil, time + 0.25
input "discarded", nil, time + 0.25
output "result", "discarded output", time + 0.5
scheduleMethod "obsolete", time + 0.75
)ruby");
    graph.setRubyDraft(other, "# output String result\noutput 0, 'other node', time + 0.75");
    graph.runRuby(cancellable);
    graph.runRuby(other);
    now += 250ms;
    const auto cancelled = graph.advanceTicks(now, TickWake::Timer);
    check(cancelled.fired && !cancelled.error && cancelled.deadline == TickTime{} + 750ms &&
          std::get<std::string>(graph.readValue(cancellable)) == "already sent" &&
          graph.inspectOutput(cancellable).triggers == 1,
          "clearEvents removes earlier requests, including an undelivered input at the same timestamp");
    now += 500ms;
    const auto independent = graph.advanceTicks(now, TickWake::Timer);
    check(independent.fired && !independent.error && independent.deadline == TickTime{} + 1250ms &&
          std::get<std::string>(graph.readValue(other)) == "other node" && graph.inspectOutput(cancellable).triggers == 1,
          "clearEvents leaves other nodes' events alone");
    now += 500ms;
    const auto kept = graph.advanceTicks(now, TickWake::Timer);
    check(kept.fired && !kept.error && !kept.deadline &&
          std::get<std::string>(graph.readValue(cancellable)) == "kept" && graph.inspectOutput(cancellable).triggers == 2,
          "Events posted after clearEvents still run");

    graph.runRuby(cancellable);
    graph.stopRuby(cancellable);
    now += 2s;
    const auto stopped = graph.advanceTicks(now, TickWake::Timer);
    check(!stopped.fired && !stopped.deadline, "Stopping a Ruby node cancels its pending events");
}

void rubyStateRoundTripsThroughProject(const QString& path)
{
    using desktop::SchematicFile;
    const auto makeSession = [] { return Session({}, {}, {}, {}, {.ruby = desktop::makeRubyBackend()}); };
    auto original = makeSession();
    const auto module = original.graph().addNode(Module, 0, 0);
    auto& child = *original.graph().module(module);
    const auto node = child.addNode(Ruby, 0, 0);
    const auto trigger = child.addNode(Trigger, 0, 0);
    child.setRubyDraft(node, R"ruby(# input Trigger increment
# output IntArray counter

class ::OpenStoneTestState
  attr_accessor :count
  def initialize
    @count = 0
    @bytes = "\xff\x00".b
  end
  def bytes
    @bytes
  end
end

@object = OpenStoneTestState.new
@saves = 0

def event
  @object.count += 1
  output [@object.count, @saves]
end

def saveState
  @saves += 1
  {counter: [@object, @object], saves: @saves}
end

def loadState(state)
  objects = state.fetch(:counter)
  raise "object identity lost" unless objects[0].equal?(objects[1])
  @object = objects[0]
  raise "binary data changed" unless @object.bytes == "\xff\x00".b
  @saves = state.fetch(:saves)
  output [@object.count, @saves]
end
)ruby");
    child.applyRubyPorts(node);
    child.connect({trigger, node, 0});
    child.runRuby(node);
    child.fireTrigger(trigger);
    child.fireTrigger(trigger);
    const auto nilNode = original.graph().addNode(Ruby, 0, 0);
    original.graph().setRubyDraft(nilNode, R"ruby(# output String restored

def saveState
  nil
end

def loadState(value)
  raise "expected nil" unless value.nil?
  output "restored nil"
end
)ruby");
    original.graph().runRuby(nilNode);
    // Neither a snapshot used by the editor nor checking for unsaved changes may run saveState.
    const auto authored = SchematicFile::project(original);
    check(SchematicFile::hasUnsavedChanges(original, authored), "An unsaved Ruby node reports pending state");
    child.fireTrigger(trigger);
    check(std::get<green::IntArray>(child.readValue(node)) == green::IntArray{3, 0},
          "The save hook runs only when saving");
    child.stopRuby(node);
    child.fireTrigger(trigger);
    check(std::get<green::IntArray>(child.readValue(node)) == green::IntArray{3, 0},
          "Stopping the node pauses event delivery before saving");

    const auto saved = SchematicFile::saveProject(original, path);
    check(saved == SchematicFile::read(path) && !SchematicFile::hasUnsavedChanges(original, saved),
          "A successful save captures stopped Ruby nodes and clears the dirty state");

    auto reopened = makeSession();
    check(SchematicFile::loadProject(reopened, SchematicFile::read(path), {}, path), "Reopen the saved schematic");
    auto& restored = *reopened.graph().module(module);
    check(restored.inspectOutput(node).triggers == 0, "A reopened Ruby node waits for Run");
    check(SchematicFile::saveProject(reopened, path) == saved, "Resaving before Run preserves the stored state");
    restored.runRuby(node);
    check(std::get<green::IntArray>(restored.readValue(node)) == green::IntArray{3, 1},
          "A new interpreter restores the original count and invokes loadState");
    reopened.graph().runRuby(nilNode);
    check(std::get<std::string>(reopened.graph().readValue(nilNode)) == "restored nil",
          "A saved nil is passed to loadState");
    check(!SchematicFile::hasUnsavedChanges(reopened, saved), "Restoring state does not mark it unsaved");
    restored.fireTrigger(trigger);
    check(std::get<green::IntArray>(restored.readValue(node)) == green::IntArray{4, 1} &&
          SchematicFile::hasUnsavedChanges(reopened, saved), "The restored counter continues and reports changes");
}

void failedRubySaveLeavesPreviousProjectIntact(const QString& path)
{
    using desktop::SchematicFile;
    Session session({}, {}, {}, {}, {.ruby = desktop::makeRubyBackend()});
    auto& graph = session.graph();
    const auto node = graph.addNode(Ruby, 0, 0);
    const auto source = graph.addNode(Int, 0, 0, 1);
    graph.setRubyDraft(node, R"ruby(# input Int count

def event
end

def saveState
  raise "cannot save this count" if @count == 2
  @count
end

def loadState(value)
  @count = value
end
)ruby");
    graph.applyRubyPorts(node);
    graph.connect({source, node, 0});
    graph.runRuby(node);
    const auto first = SchematicFile::saveProject(session, path);
    graph.setValue(source, 2);
    bool failed = false;
    try {
        SchematicFile::saveProject(session, path);
    } catch (const std::runtime_error& error) {
        failed = std::string_view(error.what()).find("cannot save this count") != std::string_view::npos;
    }
    check(failed && SchematicFile::read(path) == first, "A Ruby save hook failure preserves the previous file");
    graph.setValue(source, 3);
    const auto beforeWrite = SchematicFile::project(session);
    failed = false;
    try {
        SchematicFile::saveProject(session, path + QStringLiteral("/unwritable.ostone"));
    } catch (const std::runtime_error&) {
        failed = true;
    }
    check(failed && SchematicFile::project(session) == beforeWrite,
          "A failed file write leaves the previous Ruby snapshot in memory");
    check(SchematicFile::saveProject(session, path) != first, "Saving can be retried after correcting a hook failure");
}

void rubyDrawsARectangleAndEllipseThroughThePanelView()
{
    using namespace openstone::yellow;
    Schematic root({}, {}, {}, {}, {.ruby = desktop::makeRubyBackend()});
    auto& child = *root.module(root.addNode(Module, 0, 0));
    const auto panel = child.addNode(ModuleGui, 0, 0);
    const auto script = child.addNode(Ruby, 0, 0);
    child.setRubyDraft(script, R"ruby(# input View canvas

def event
  raise "The View belongs to draw"
end

def draw(view)
  view.drawRectangle(Brush.new(Color.new(128, 16, 48, 96)), [1, 2, view.width / 2, 3])
  view.drawEllipse(Pen.new(Color.new(160, 30, 40), 0.25), [4, 3, 5, 2])
end
)ruby");
    child.runRuby(script);
    const auto view = child.frontPanel()->view;
    check(root.renderPanel(view).empty(), "Ruby draws only on a View connected to the node");
    child.connect({panel, script, 0});
    check(child.inputType(script, 0) == green::Type::View &&
          !child.nodes().back().rubyCode->error, "Connecting the View does not send a Ruby event");
    const auto drawings = root.renderPanel(view);
    check(drawings.size() == 2, "The draw callback emits both shapes");
    const auto& rectangle = std::get<DrawCommand>(drawings.front().command);
    const auto& ellipse = std::get<DrawCommand>(drawings.back().command);
    check(rectangle.shape == ShapeKind::Rect && rectangle.area == openstone::yellow::Area{1, 2, view.bounds.width / 2, 3} &&
          std::get<SolidBrush>(rectangle.tool).color.argb == 0x80103060,
          "The rectangle retains its grid geometry, paint order, and translucent fill");
    check(ellipse.shape == ShapeKind::Ellipse && ellipse.area == openstone::yellow::Area{4, 3, 5, 2} &&
          std::get<PenStroke>(ellipse.tool) == PenStroke{openstone::yellow::Color{0xFFA01E28}, 0.25F},
          "The ellipse retains its outline color and grid-unit width");
    check(root.panelDrawings(view).size() == 2, "Panel previews can render the Ruby draw callback");
    child.stopRuby(script);
    check(root.renderPanel(view).empty(), "Stopping Ruby removes its drawings from the panel");
}

void rubyDrawsLinesPolygonsAndRoundedRectangles()
{
    using namespace openstone::yellow;
    Schematic root({}, {}, {}, {}, {.ruby = desktop::makeRubyBackend()});
    auto& child = *root.module(root.addNode(Module, 0, 0));
    const auto panel = child.addNode(ModuleGui, 0, 0);
    const auto script = child.addNode(Ruby, 0, 0);
    child.setRubyDraft(script, R"ruby(# input View canvas
def draw(view)
  pen = Pen.new(Color.new(18, 52, 86), 0.25)
  brush = Brush.new(Color.new(170, 48, 64, 96))
  view.drawRoundRect(brush, [0, 1, 4, 3], 0.75)
  view.drawLine(pen, [1, 0], [1, 4])
  view.drawLines(pen, [[0, 1], [2, 3], [4, 1]])
  view.drawPolygon(brush, [[0, 0], [2, 0], [1, 2]])
  view.drawPolygon(pen, [[2, 2], [4, 2], [3, 4]])
  view.drawPolygon(brush, [[1, 1], [2, 2], [1, 1]])
end
)ruby");
    child.runRuby(script);
    child.connect({panel, script, 0});
    const auto view = child.frontPanel()->view;
    const auto drawings = root.renderPanel(view);
    check(drawings.size() == 5, "Ruby emits shapes in draw order and skips a degenerate polygon");
    const auto& rectangle = std::get<DrawCommand>(drawings[0].command);
    check(rectangle.shape == ShapeKind::Rect && rectangle.area == openstone::yellow::Area{0, 1, 4, 3} &&
          rectangle.cornerRadius == 0.75F && std::get<SolidBrush>(rectangle.tool).color.argb == 0xAA304060,
          "A rounded rectangle preserves its corner radius, local bounds, and fill");

    const auto& line = std::get<GraphDrawCommand>(drawings[1].command);
    const auto& polyline = std::get<GraphDrawCommand>(drawings[2].command);
    check(!line.closed && !line.clipToArea && line.vertices.size() == 2 &&
          line.vertices[0].x == 1 && line.vertices[0].y == 0 &&
          line.vertices[1].x == 1 && line.vertices[1].y == 4 &&
          std::get<PenStroke>(line.tool) == PenStroke{openstone::yellow::Color{0xFF123456}, 0.25F},
          "A vertical line uses the full View and retains its pen");
    check(!polyline.closed && polyline.vertices.size() == 3 &&
          polyline.vertices[0].startsRun && !polyline.vertices[1].startsRun && !polyline.vertices[2].startsRun &&
          polyline.vertices[1].x == 2 && polyline.vertices[1].y == 3,
          "A polyline joins every point in order without closing");
    const auto& fill = std::get<GraphDrawCommand>(drawings[3].command);
    const auto& outline = std::get<GraphDrawCommand>(drawings[4].command);
    check(fill.closed && fill.vertices.size() == 3 && std::get<SolidBrush>(fill.tool).color.argb == 0xAA304060 &&
          outline.closed && outline.vertices.size() == 3 &&
          std::get<PenStroke>(outline.tool) == std::get<PenStroke>(line.tool),
          "Polygons close their last edge and use the supplied brush or pen");
}

void rubyDrawsTextAtAPointAndInARectangle()
{
    using namespace openstone::yellow;
    Schematic root({}, {}, {}, {}, {.ruby = desktop::makeRubyBackend()});
    auto& child = *root.module(root.addNode(Module, 0, 0));
    const auto panel = child.addNode(ModuleGui, 0, 0);
    const auto script = child.addNode(Ruby, 0, 0);
    child.setRubyDraft(script, R"ruby(# input View canvas
def draw(view)
  font = Font.new("Arial", 1.25, 5)
  brush = Brush.new(Color.new(128, 20, 40, 80))
  label = "Café"
  view.drawString(label, font, nil, [1.5, 2.25], brush)
  label.replace("Meter")
  format = StringFormat.new
  format.setAlignment("CENTER")
  format.setLineAlignment("far")
  format.setFlags(1 | 2 | 32 | 2048 | 4096 | 8192 | 16384)
  view.drawString(label, Font.new("Arial", 1, "italic-strikeout"), format, [0, 1, 5, 3], brush)
  format.setAlignment("near")
  format.setFlags(1)
  view.drawString("RTL", font, format, [0, 0, 5, 2], brush)
  view.drawString("Default", font, nil, [1, 1, 2, 3], brush)
  view.drawString("", font, nil, [0, 0], brush)
end
)ruby");
    child.runRuby(script);
    child.connect({panel, script, 0});
    const auto drawings = root.renderPanel(child.frontPanel()->view);
    check(drawings.size() == 4, "Ruby draws each nonempty label once in order");

    const auto& point = std::get<TextCommand>(drawings[0].command);
    check(point.pointOrigin && point.area == openstone::yellow::Area{1.5F, 2.25F, 0, 0} &&
          point.text == "Café" && point.color.argb == 0x80142850 &&
          point.font == openstone::yellow::Font{"Arial", 1.25F, FontStyle{true, false, true, false}},
          "Point text retains its anchor, Unicode contents, font bitmask, and brush");
    check(!point.format.wrap && !point.format.clip &&
          point.format.horizontal == HorizontalAlignment::Left && point.format.vertical == VerticalAlignment::Top,
          "A point with nil format anchors unwrapped, unclipped text at its top left");

    const auto& rectangle = std::get<TextCommand>(drawings[1].command);
    check(!rectangle.pointOrigin && rectangle.area == openstone::yellow::Area{0, 1, 5, 3} &&
          rectangle.text == "Meter" && rectangle.font.style == FontStyle{false, true, false, true},
          "Rectangle text preserves its layout bounds and named font styles");
    check(rectangle.format.horizontal == HorizontalAlignment::Center && rectangle.format.vertical == VerticalAlignment::Bottom &&
          rectangle.format.rightToLeft && rectangle.format.verticalText && rectangle.format.showControls &&
          rectangle.format.includeTrailingSpaces && rectangle.format.wholeLinesOnly &&
          !rectangle.format.clip && !rectangle.format.wrap,
          "StringFormat retains alignment and flags at the time of each drawing call");
    const auto& rtl = std::get<TextCommand>(drawings[2].command);
    check(rtl.format.horizontal == HorizontalAlignment::Right && rtl.format.wrap && rtl.format.clip,
          "Near is right aligned for horizontal right-to-left text and resetting flags restores defaults");
    const auto& defaults = std::get<TextCommand>(drawings[3].command);
    check(defaults.format == openstone::yellow::StringFormat{}, "A rectangle with nil format wraps and clips");
}

void rubyMeasuresTextWithTheDrawingLayout()
{
    using namespace openstone::yellow;
    Schematic root({}, {}, {}, {}, {.ruby = desktop::makeRubyBackend()});
    auto& child = *root.module(root.addNode(Module, 0, 0));
    const auto panel = child.addNode(ModuleGui, 0, 0);
    const auto script = child.addNode(Ruby, 0, 0);
    child.setRubyDraft(script, R"ruby(# input View canvas
def assert_measurement(condition, message)
  raise message unless condition
end

def draw(view)
  font = Font.new("Arial", 1.2)
  brush = Brush.new(Color.new(64))
  label = "A😀é"
  point = view.measureString(label, font, nil, [2, 3])
  assert_measurement(point.keys.sort == %w[bounds chars height lines width], "Bad measurement keys")
  assert_measurement(point["bounds"] == [2, 3, point["width"], point["height"]], "Bad point origin")
  assert_measurement(point["chars"] == label.length && point["lines"] == 1, "Count Unicode code points")
  assert_measurement(point["width"] > 0 && point["height"] > 0, "Expect a positive label size")

  format = StringFormat.new
  format.setAlignment("center")
  format.setLineAlignment("center")
  centered = view.measureString(label, font, format, [10, 10])
  x, y, w, h = centered["bounds"]
  assert_measurement((x + w / 2 - 10).abs < 0.002 && (y + h / 2 - 10).abs < 0.002, "Center text on its anchor #{centered.inspect}")
  x, y, w, h = view.measureString(label, font, format, [2, 3, 12, 10])["bounds"]
  assert_measurement((x + w / 2 - 8).abs < 0.002 && (y + h / 2 - 8).abs < 0.002, "Center text in its rectangle #{[x, y, w, h].inspect}")

  words = "one two three four"
  narrow = view.measureString(words, font, nil, [2, 3, point["width"] * 1.6, 20])
  assert_measurement(narrow["lines"] > 1 && narrow["chars"] == words.length &&
    narrow["height"] > point["height"], "Wrap across several lines without losing characters")

  multiline = view.measureString("one\r\ntwo", font, nil, [2, 3])
  assert_measurement(multiline["lines"] == 2 && multiline["chars"] == 7, "Treat CRLF as a single line break")
  format.setAlignment("near")
  format.setLineAlignment("near")
  format.setFlags(8192)
  one_row = view.measureString("one", font, nil, [0, 0])
  whole = view.measureString("one\ntwo", font, format, [2, 3, 20, one_row["height"] * 1.4])
  assert_measurement(whole["lines"] == 1 && whole["chars"] == 4, "Measure only whole rows when requested #{whole.inspect}")

  format.setFlags(4096)
  letter = view.measureString("M", font, nil, [0, 0])
  location = [2, 3, letter["width"] * 3.5, 3]
  clipped = view.measureString("MMMMMM", font, format, location)
  assert_measurement(clipped["lines"] == 1 && clipped["chars"] == 3 &&
    (clipped["width"] - location[2]).abs < 0.0001, "Count just the letters that fit #{clipped.inspect}")
  format.setAlignment("far")
  right_aligned = view.measureString("MMMMMM", font, format, location)
  assert_measurement(right_aligned["chars"] == 3, "Count fitted characters after clipping at the left edge #{right_aligned.inspect}")
  format.setAlignment("near")
  format.setFlags(4096 | 16384)
  overflow = view.measureString("MMMMMM", font, format, location)
  assert_measurement(overflow["width"] > clipped["width"] && overflow["chars"] == 6, "NoClip measures overflow")

  format.setFlags(2048)
  spaces = view.measureString("M   ", font, format, [0, 0])
  assert_measurement(spaces["width"] > letter["width"], "Measure trailing spaces when requested")
  format.setFlags(2)
  vertical = view.measureString(label, font, format, [2, 3])
  assert_measurement((vertical["width"] - point["height"]).abs < 0.0001 &&
    (vertical["height"] - point["width"]).abs < 0.0001, "Measure rotated text in local coordinates")

  empty = view.measureString("", font, nil, [2, 3])
  assert_measurement(empty["bounds"] == [0, 0, 0, 0] && empty["chars"] == 0 && empty["lines"] == 0,
    "Empty text has zero size and counts")
  view.drawRectangle(brush, point["bounds"])
  view.drawString(label, font, nil, [2, 3], brush)
end
)ruby");
    child.runRuby(script);
    child.connect({panel, script, 0});
    const auto drawings = root.renderPanel(child.frontPanel()->view);
    const auto& error = child.nodes().back().rubyCode->drawError;
    if (!error.empty()) throw std::runtime_error(error);
    check(drawings.size() == 2, "Measurements are usable immediately within the drawing callback");
    const auto& rectangle = std::get<DrawCommand>(drawings[0].command);
    const auto& text = std::get<TextCommand>(drawings[1].command);
    check(rectangle.area.x == text.area.x && rectangle.area.y == text.area.y &&
          rectangle.area.width > 0 && rectangle.area.height > 0, "Measured geometry can back the next label");
}

void rubyDrawUsesTheViewConnectorAndSectionMapping()
{
    using namespace openstone::yellow;
    Schematic root({}, {}, {}, {}, {.ruby = desktop::makeRubyBackend()});
    auto& child = *root.module(root.addNode(Module, 0, 0));
    const auto panel = child.addNode(ModuleGui, 0, 0);
    const auto section = child.addNode(ViewSection, 0, 0);
    const auto script = child.addNode(Ruby, 0, 0);
    const auto x = child.addNode(Float, 0, 0, 3.F);
    const auto y = child.addNode(Float, 0, 0, 2.F);
    const auto width = child.addNode(Float, 0, 0, 7.F);
    const auto height = child.addNode(Float, 0, 0, 4.F);
    child.setRubyDraft(script, R"ruby(# input View panel
# input View inset

def draw(connector, view)
  if connector == "inset" && connector.index == 1
    view.drawEllipse(Brush.new(Color.new(80)), [0, 0, view.width, view.height])
    view.drawLines(Pen.new(Color.new(255), 0.2), [[-1, 1], [1, 1], [2, 2]])
  end
end
)ruby");
    child.applyRubyPorts(script);
    child.connect({panel, script, 0});
    child.connect({panel, section, 0});
    child.connect({x, section, 1});
    child.connect({y, section, 2});
    child.connect({width, section, 3});
    child.connect({height, section, 4});
    child.connect({section, script, 1});
    child.runRuby(script);

    const auto drawings = root.renderPanel(child.frontPanel()->view);
    check(drawings.size() == 2, "The two-argument callback distinguishes the named View inputs");
    const auto& drawing = drawings.front();
    const auto& ellipse = std::get<DrawCommand>(drawing.command);
    check(ellipse.shape == ShapeKind::Ellipse && ellipse.area == openstone::yellow::Area{0, 0, 7, 4} &&
          drawing.mapping.mapArea(ellipse.area) == openstone::yellow::Area{3, 2, 7, 4} &&
          drawing.mapping.clip && !drawing.mapping.contains({-1, 0}),
          "A Ruby shape receives local View size and retains the section's offset and clipping");
    const auto& path = std::get<GraphDrawCommand>(drawings[1].command);
    check(path.area == ellipse.area && path.vertices[0].x == -1 &&
          drawings[1].mapping.toPanel({1, 1}) == openstone::green::Point{4, 3} &&
          drawings[1].mapping.clip && !drawings[1].mapping.contains({-1, 1}),
          "A Ruby path preserves points in local coordinates and uses the View section's clipping");
}


void rubyRedrawTargetsOneView()
{
    using namespace openstone::yellow;
    Schematic root({}, {}, {}, {}, {.ruby = desktop::makeRubyBackend()});
    auto& child = *root.module(root.addNode(Module, 0, 0));
    const auto front = child.addNode(ModuleGui, 0, 0);
    const auto props = child.addNode(ModulePropertiesGui, 0, 0);
    const auto redrawProps = child.addNode(Trigger, 0, 0);
    const auto redrawDefault = child.addNode(Trigger, 0, 0);
    const auto script = child.addNode(Ruby, 0, 0);
    child.setRubyDraft(script, R"ruby(# input Trigger redrawProps
# input View canvas
# input View properties
# input Trigger redrawDefault

@gray = 32

def event(connector)
  if connector == "redrawProps"
    @gray = 96
    redraw "properties"
  else
    @gray = 192
    redraw
  end
end

def draw(view)
  view.drawRectangle(Brush.new(Color.new(@gray)), [0, 0, view.width, view.height])
end
)ruby");
    child.applyRubyPorts(script);
    child.connect({redrawProps, script, 0});
    child.connect({front, script, 1});
    child.connect({props, script, 2});
    child.connect({redrawDefault, script, 3});
    child.runRuby(script);

    const auto canvas = child.readPanel(front).view;
    const auto properties = child.readPanel(props).view;
    const auto color = [&](const View& view) {
        const auto drawings = root.renderPanel(view);
        check(drawings.size() == 1, "A full redraw leaves one rectangle");
        const auto& drawing = drawings.front();
        check(drawing.mapping.contains({1, 1}) &&
              drawing.mapping.contains({view.bounds.width - 1, view.bounds.height - 1}),
              "A full redraw covers the chosen View");
        return std::get<SolidBrush>(std::get<DrawCommand>(drawing.command).tool).color.argb;
    };

    check(color(canvas) == 0xFF202020 && color(properties) == 0xFF202020, "Render both Views before requesting a redraw");
    child.fireTrigger(redrawProps);
    check(color(canvas) == 0xFF202020 && color(properties) == 0xFF606060,
          "A named redraw repaints only that View");
    child.fireTrigger(redrawDefault);
    check(color(canvas) == 0xFFC0C0C0 && color(properties) == 0xFF606060,
          "An omitted connector redraws the first View input even if a Trigger comes first");
}

void rubyPartialRedrawKeepsPaintOutsideTheMappedArea()
{
    using namespace openstone::yellow;
    Schematic root({}, {}, {}, {}, {.ruby = desktop::makeRubyBackend()});
    auto& child = *root.module(root.addNode(Module, 0, 0));
    const auto panel = child.addNode(ModuleGui, 0, 0);
    const auto section = child.addNode(ViewSection, 0, 0);
    const auto offset = child.addNode(Float, 0, 0, 3.F);
    const auto size = child.addNode(Float, 0, 0, 6.F);
    const auto update = child.addNode(Trigger, 0, 0);
    const auto script = child.addNode(Ruby, 0, 0);
    child.setRubyDraft(script, R"ruby(# input View inset
# input Trigger update

@gray = 32

def event
  @gray = 96
  redraw 0, [-1, 1, 3, 6]
end

def draw(view)
  view.drawRectangle(Brush.new(Color.new(@gray)), [0, 0, view.width, view.height])
end
)ruby");
    child.applyRubyPorts(script);
    child.connect({panel, section, 0});
    child.connect({offset, section, 1});
    child.connect({offset, section, 2});
    child.connect({size, section, 3});
    child.connect({size, section, 4});
    child.connect({section, script, 0});
    child.connect({update, script, 1});
    child.runRuby(script);

    const auto view = child.readPanel(panel).view;
    check(root.renderPanel(view).size() == 1, "Paint the original inset");
    child.fireTrigger(update);
    const auto verify = [&](const std::vector<PanelDrawing>& drawings) {
        check(drawings.size() == 2, "A partial redraw keeps both the old and new paint");
        for (const auto& drawing : drawings) {
            const auto gray = std::get<SolidBrush>(std::get<DrawCommand>(drawing.command).tool).color.argb;
            const bool fresh = gray == 0xFF606060;
            check((fresh || gray == 0xFF202020) && drawing.mapping.toPanel({1, 2}) == green::Point{4, 5} &&
                  drawing.mapping.contains({1, 2}) == fresh &&
                  drawing.mapping.contains({3, 2}) == !fresh &&
                  drawing.mapping.contains({1, 0.5F}) == !fresh &&
                  !drawing.mapping.contains({-0.5F, 2}) && !drawing.mapping.contains({1, 6.5F}),
                  "The redraw rectangle follows the View's offset and clip, retaining pixels outside it");
        }
    };
    verify(root.renderPanel(view));
    verify(root.renderPanel(view));
}


void rubyMouseUsesLocalCoordinatesAndCapture()
{
    using namespace openstone::yellow;
    using green::Point;
    Schematic root({}, {}, {}, {}, {.ruby = desktop::makeRubyBackend()});
    auto& child = *root.module(root.addNode(Module, 0, 0));
    const auto panel = child.addNode(ModuleGui, 0, 0);
    const auto section = child.addNode(ViewSection, 0, 0);
    const auto scale = child.addNode(ViewScale, 0, 0);
    const auto x = child.addNode(Float, 0, 0, 3.F);
    const auto y = child.addNode(Float, 0, 0, 2.F);
    const auto width = child.addNode(Float, 0, 0, 8.F);
    const auto height = child.addNode(Float, 0, 0, 6.F);
    const auto ratio = child.addNode(Float, 0, 0, 2.F);
    const auto mouse = child.addNode(Boolean, 0, 0, false);
    const auto script = child.addNode(Ruby, 0, 0);

    child.setRubyDraft(script, R"ruby(# input View other
# input View inset
# output StringArray events

@events = []

def isInMousePoint(i, x, y)
  i == "inset" && x < 2 ? 1 : 0
end

def record(kind, i, x, y)
  @events << "#{kind}:#{i}:#{x}:#{y}"
  output "events", @events
end

def mouseLDown(i, x, y)
  record("down", i, x, y)
  captureMouse
end

def mouseMoveCaptured(i, x, y)
  record("drag", i, x, y)
end

def mouseLUp(i, x, y)
  record("up", i, x, y)
  releaseMouse
end

def mouseMove(x, y)
  record("move", "inset", x, y)
end
)ruby");
    child.applyRubyPorts(script);
    child.connect({mouse, panel, 1});
    child.connect({panel, section, 0});
    child.connect({x, section, 1});
    child.connect({y, section, 2});
    child.connect({width, section, 3});
    child.connect({height, section, 4});
    child.connect({section, scale, 0});
    child.connect({ratio, scale, 1});
    child.connect({panel, script, 0});
    child.connect({scale, script, 1});
    child.runRuby(script);

    using enum MouseEventKind;
    constexpr Point inside{5.5F, 5.F};
    constexpr Point rejected{8.F, 5.F};
    const auto send = [&](MouseEventKind kind, Point position, MouseButton button = MouseButton::Left) {
        return child.sendRubyMouseMessage(script, 1, {kind, position, button});
    };
    const auto regions = root.renderPanel(child.frontPanel()->view);
    check(regions.size() == 2 && std::get<MouseAreaCommand>(regions.front().command).rubyInput !=
              std::get<MouseAreaCommand>(regions.back().command).rubyInput,
          "Each Ruby View gets a live mouse region even without a draw callback");
    check(child.rubyMouseContains(script, 1, inside) && !child.rubyMouseContains(script, 1, rejected) &&
          !child.rubyMouseContains(script, 0, inside),
          "The local hit test accepts integer 1 and rejects integer 0, independently by connector");
    check(!send(ButtonDown, rejected) && !send(ButtonDown, {2.5F, 5.F}) &&
          !send(Move, inside, MouseButton::None),
          "Both the View bounds and hit test gate presses, and idle moves start disabled");
    check(send(ButtonDown, inside) &&
          !child.sendRubyMouseMessage(script, 0, {Drag, inside, MouseButton::Left}) &&
          !send(Drag, inside, MouseButton::Right) &&
          send(Drag, {15.F, -2.F}) && send(ButtonUp, {-1.F, 10.F}),
          "Captured moves and up reach the same input and button outside the View");
    check(!send(Drag, inside), "Mouse up releases capture");
    child.setValue(mouse, true);
    check(send(Move, {6.F, 4.F}, MouseButton::None) && !send(Move, rejected, MouseButton::None),
          "The panel's Mouse input enables hover only within the hit region");
    check(std::get<StringArray>(child.readValue(script)) ==
              StringArray{"down:inset:1.25:1.5", "drag:inset:6.0:-2.0",
                          "up:inset:-2.0:4.0", "move:inset:1.5:1.0"},
          "Ruby receives ordered, fractional local coordinates and the named connector");

    check(send(ButtonDown, inside), "Capture can start again after release");
    child.cancelRubyMouseDrag(script, 1);
    check(!send(Drag, inside), "Losing the UI grab cancels capture");
    check(send(ButtonDown, inside), "Capture can start again after cancellation");
    child.stopRuby(script);
    check(!send(Drag, inside) && root.renderPanel(child.frontPanel()->view).empty(),
          "Stopping Ruby removes its live regions and cancels capture");
}

void rubyMouseRequiresHitTestAndExplicitCapture()
{
    using namespace openstone::yellow;
    Schematic root({}, {}, {}, {}, {.ruby = desktop::makeRubyBackend()});
    auto& child = *root.module(root.addNode(Module, 0, 0));
    const auto panel = child.addNode(ModuleGui, 0, 0);
    const auto script = child.addNode(Ruby, 0, 0);
    const std::string code = R"ruby(# input View canvas
# output String last

def mouseRDown(x, y)
  output "last", "right down"
end

def mouseRUp(x, y)
  output "last", "right up"
end

def mouseMoveCaptured(x, y)
  raise "Captured without a captureMouse call"
end
)ruby";
    using enum MouseEventKind;
    const auto send = [&](MouseEventKind kind, green::Point position) {
        return child.sendRubyMouseMessage(script, 0, {kind, position, MouseButton::Right});
    };
    child.setRubyDraft(script, code);
    child.applyRubyPorts(script);
    child.connect({panel, script, 0});
    child.runRuby(script);
    check(!send(ButtonDown, {1.F, 2.F}) && root.renderPanel(child.frontPanel()->view).empty(),
          "A mouse callback alone does not opt a Ruby View in to input");

    child.setRubyDraft(script, code + "\ndef isInMousePoint(x, y)\n  true\nend\n");
    child.runRuby(script);
    check(send(ButtonDown, {1.F, 2.F}) && std::get<std::string>(child.readValue(script)) == "right down" &&
          !send(Drag, {3.F, 4.F}) && !send(ButtonUp, {-1.F, 2.F}),
          "An uncaptured press delivers down and leaves drag and outside release unhandled");
    check(send(ButtonUp, {2.F, 2.F}) && std::get<std::string>(child.readValue(script)) == "right up",
          "A right release inside the hit region reaches the two-argument callback");
}


void rubySelectsHoverCursorInLocalViewCoordinates()
{
    using namespace openstone::yellow;
    Schematic root({}, {}, {}, {}, {.ruby = desktop::makeRubyBackend()});
    auto& child = *root.module(root.addNode(Module, 0, 0));
    const auto panel = child.addNode(ModuleGui, 0, 0);
    const auto section = child.addNode(ViewSection, 0, 0);
    const auto x = child.addNode(Float, 0, 0, 3.F);
    const auto y = child.addNode(Float, 0, 0, 2.F);
    const auto script = child.addNode(Ruby, 0, 0);
    const std::string code = R"ruby(# input View canvas
# input View inset

def isInMousePoint(x, y)
  x >= 0 && x < 3 && y >= 0 && y < 2
end
)ruby";
    child.setRubyDraft(script, code + R"ruby(
def mouseCursor(i, x, y)
  return "move" if i == "canvas"
  return "IBeam" if x < 1
  return 12 if x < 2
  "unrecognized cursor"
end
)ruby");
    child.applyRubyPorts(script);
    child.connect({panel, script, 0});
    child.connect({panel, section, 0});
    child.connect({x, section, 1});
    child.connect({y, section, 2});
    child.connect({section, script, 1});
    child.runRuby(script);

    check(!child.readPanel(panel).view.mouseMoves, "Cursor queries do not depend on the Mouse input");
    check(child.rubyMouseCursor(script, 0, {1.F, 1.F}) == MouseCursor::Move &&
          child.rubyMouseCursor(script, 1, {3.5F, 2.25F}) == MouseCursor::IBeam &&
          child.rubyMouseCursor(script, 1, {4.5F, 2.25F}) == MouseCursor::Crosshair,
          "A named connector, a cursor string, and a numeric ID select the cursor in local grid coordinates");
    check(child.rubyMouseCursor(script, 1, {5.5F, 2.25F}) == MouseCursor::HandPointer &&
          !child.rubyMouseCursor(script, 1, {6.5F, 2.25F}) &&
          !child.rubyMouseCursor(script, 1, {2.5F, 2.25F}),
          "An unknown cursor uses the standard hand and rejected points do not claim a cursor");

    child.setRubyDraft(script, code + "\ndef mouseCursor\n  11\nend\n");
    child.runRuby(script);
    check(child.rubyMouseCursor(script, 0, {1.F, 1.F}) == MouseCursor::ClosedHand,
          "A zero-argument callback can select one cursor for the entire hit region");
    child.setRubyDraft(script, code);
    child.runRuby(script);
    check(child.rubyMouseCursor(script, 0, {1.F, 1.F}) == MouseCursor::HandPointer,
          "A View without mouseCursor retains its standard hand cursor");
}

void rubyCursorVisibilityCountsNestedCallsAndClearsDragState()
{
    using namespace openstone::yellow;
    Schematic root({}, {}, {}, {}, {.ruby = desktop::makeRubyBackend()});
    auto& child = *root.module(root.addNode(Module, 0, 0));
    const auto panel = child.addNode(ModuleGui, 0, 0);
    const auto script = child.addNode(Ruby, 0, 0);
    child.setRubyDraft(script, R"ruby(# input View canvas

def isInMousePoint(x, y)
  x < 3 && y < 3
end

def mouseLDown(x, y)
  captureMouse
  showCursor false
  showCursor false
end

def mouseMoveCaptured(x, y)
  showCursor x > 0
end

def mouseLUp(x, y)
  releaseMouse
  showCursor true
end
)ruby");
    child.applyRubyPorts(script);
    child.connect({panel, script, 0});
    child.runRuby(script);

    using enum MouseEventKind;
    const auto send = [&](MouseEventKind kind, green::Point position) {
        return child.sendRubyMouseMessage(script, 0, {kind, position, MouseButton::Left});
    };
    check(!root.cursorVisibility().hidden, "The cursor starts visible");
    check(send(ButtonDown, {1.F, 1.F}) && root.cursorVisibility().hidden && !root.cursorVisibility().hold,
          "A captured drag hides the cursor without holding the pointer position");
    check(send(Drag, {6.F, 1.F}) && root.cursorVisibility().hidden,
          "One show call leaves the second nested hide in effect");
    check(send(Drag, {7.F, 1.F}) && !root.cursorVisibility().hidden,
          "A second show call balances the nested hides outside the View");
    check(send(Drag, {8.F, 1.F}) && send(Drag, {-1.F, 1.F}) && !root.cursorVisibility().hidden,
          "An extra show above zero offsets a later hide");
    check(send(Drag, {-2.F, 1.F}) && root.cursorVisibility().hidden &&
          send(Drag, {-3.F, 1.F}) && root.cursorVisibility().hidden &&
          send(ButtonUp, {-3.F, 1.F}) && !root.cursorVisibility().hidden,
          "Release outside the View clears any remaining hide calls for the drag");
    check(send(ButtonDown, {1.F, 1.F}) && root.cursorVisibility().hidden, "The next drag starts with a fresh balance");
    child.cancelRubyMouseDrag(script, 0);
    check(!root.cursorVisibility().hidden, "Losing the mouse grab restores the cursor");
    check(send(ButtonDown, {1.F, 1.F}) && root.cursorVisibility().hidden, "The View can capture again after cancellation");
    child.stopRuby(script);
    check(!root.cursorVisibility().hidden, "Stopping the Ruby node clears its cursor changes");
}


void rubyEditControlReportsChangesAndCommits()
{
    using namespace openstone::yellow;
    Schematic root({}, {}, {}, {}, {.ruby = desktop::makeRubyBackend()});
    auto& child = *root.module(root.addNode(Module, 0, 0));
    const auto panel = child.addNode(ModuleGui, 0, 0);
    const auto props = child.addNode(ModulePropertiesGui, 0, 0);
    const auto section = child.addNode(ViewSection, 0, 0);
    const auto offset = child.addNode(Float, 0, 0, 2.F);
    const auto script = child.addNode(Ruby, 0, 0);
    child.setRubyDraft(script, R"ruby(# input View properties
# input View inset
# output StringArray events

@events = []

def isInMousePoint(i, x, y)
  i == "inset"
end

def mouseLDown(i, x, y)
  createEdit i, :title, [1, 0.5, 5, 2], "Draft", Font.new("Arial", 0.75, 3),
             Color.new(128, 16, 32, 48), Color.new(248, 240, 232), true
end

def editChanged(id, text)
  @events << ["live", id.inspect, text].join(":")
  output "events", @events
  redraw "inset", [1, 0.5, 5, 2]
end

def editFinished(id, text)
  @events << ["commit", id.inspect, text].join(":")
  output "events", @events
end
)ruby");
    child.applyRubyPorts(script);
    child.connect({props, script, 0});
    child.connect({panel, section, 0});
    child.connect({offset, section, 1});
    child.connect({offset, section, 2});
    child.connect({section, script, 1});
    child.runRuby(script);
    const auto view = child.readPanel(panel).view;
    const auto editor = [&](const View& target) {
        for (const auto& drawing : root.renderPanel(target)) {
            if (std::holds_alternative<EditControlCommand>(drawing.command)) {
                return std::optional{drawing};
            }
        }
        return std::optional<PanelDrawing>{};
    };
    check(!editor(view) && child.sendRubyMouseMessage(script, 1, {MouseEventKind::ButtonDown, {3, 3}, MouseButton::Left}),
          "Clicking the Ruby View opens an edit field");
    auto field = editor(view);
    check(field.has_value() && !editor(child.readPanel(props).view), "Only the requested View shows the in-place editor");
    const auto& control = std::get<EditControlCommand>(field->command);
    check(control.text == "Draft" && control.multiline && control.font.face == "Arial" && control.font.size == 0.75F &&
          control.font.style == FontStyle{true, true} && control.textColor.argb == 0x80102030 &&
          control.editBackground.argb == 0xFFF8F0E8 &&
          field->mapping.mapArea(control.area) == openstone::yellow::Area{3, 2.5F, 5, 2},
          "createEdit uses the initial text, font, colors, multiline mode, and View-local bounds");

    const auto token = child.beginEditControl(script, control.rubySession);
    check(token && std::get<StringArray>(child.readValue(script)).empty() &&
          !child.changeEditControl(script, token, "Draft"), "Seeding identical text does not emit editChanged");
    check(child.changeEditControl(script, token, "Café") &&
          child.changeEditControl(script, token, "Café\nSecond line") &&
          std::get<StringArray>(child.readValue(script)) == StringArray{"live::title:Café", "live::title:Café\nSecond line"},
          "Each edit routes the latest text with the original Ruby ID");
    const auto updated = editor(view);
    check(updated && std::get<EditControlCommand>(updated->command).rubySession == token &&
          std::get<EditControlCommand>(updated->command).text == "Café\nSecond line",
          "A Ruby redraw retains the open field, token, and live text");
    check(child.finishEditControl(script, token) && !child.finishEditControl(script, token) &&
          !child.changeEditControl(script, token, "Stale") && !editor(view) &&
          std::get<StringArray>(child.readValue(script)) ==
              StringArray{"live::title:Café", "live::title:Café\nSecond line", "commit::title:Café\nSecond line"},
          "Finishing removes the editor and publishes the final Unicode text exactly once");
}

void rubyFinishedCallbackCanOpenANewEditor()
{
    using namespace openstone::yellow;
    Schematic root({}, {}, {}, {}, {.ruby = desktop::makeRubyBackend()});
    auto& child = *root.module(root.addNode(Module, 0, 0));
    const auto panel = child.addNode(ModuleGui, 0, 0);
    const auto start = child.addNode(Trigger, 0, 0);
    const auto script = child.addNode(Ruby, 0, 0);
    child.setRubyDraft(script, R"ruby(# input Trigger start
# input View canvas
# output String result

def event
  createEdit "canvas", 42, [1, 1, 5, 1]
end

def editFinished(id, text)
  output "result", "#{id}:#{text}"
  createEdit 1, 43, [1, 1, 5, 1], text if id == 42
end
)ruby");
    child.applyRubyPorts(script);
    child.connect({start, script, 0}, LinkNotification::Silent);
    child.connect({panel, script, 1});
    child.runRuby(script);

    const auto view = child.readPanel(panel).view;
    const auto editor = [&] {
        const auto drawings = root.renderPanel(view);
        check(drawings.size() == 1, "An edit callback can display one editor without a drawing callback");
        return std::get<EditControlCommand>(drawings.front().command);
    };
    child.fireTrigger(start);
    const auto first = editor();
    check(first.text.empty() && !first.multiline && child.changeEditControl(script, first.rubySession, "Keep") &&
          child.finishEditControl(script, first.rubySession), "An editor can open on a Green event and omit editChanged");
    const auto next = editor();
    check(next.rubySession != first.rubySession && next.text == "Keep" &&
          !child.finishEditControl(script, first.rubySession) &&
          std::get<std::string>(child.readValue(script)) == "42:Keep",
          "Committing delivers the numeric ID and can replace the session");
    child.stopRuby(script);
    check(!child.finishEditControl(script, next.rubySession) && root.renderPanel(view).empty(),
          "Stopping Ruby discards its open editors and invalidates their sessions");
}


void rubyDropListReportsTheSelectedIndexAndName()
{
    using namespace openstone::yellow;
    Schematic root({}, {}, {}, {}, {.ruby = desktop::makeRubyBackend()});
    const auto module = root.addNode(Module, 0, 0);
    auto& child = *root.module(module);
    const auto panel = child.addNode(ModuleGui, 0, 0);
    const auto props = child.addNode(ModulePropertiesGui, 0, 0);
    const auto section = child.addNode(ViewSection, 0, 0);
    const auto offset = child.addNode(Float, 0, 0, 2.F);
    const auto script = child.addNode(Ruby, 0, 0);
    child.setRubyDraft(script, R"ruby(# input View properties
# input View inset
# output StringArray selected

def isInMousePoint(i, x, y)
  i == "inset"
end

def mouseLDown(i, x, y)
  createDropList i, :wave, [x + 0.25, y + 0.5], "Sine, -----, Triangle, Carré", 1, 2, 0
end

def dropListFinished(id, index, name)
  output "selected", [id.inspect, index.to_s, name]
end
)ruby");
    child.applyRubyPorts(script);
    child.connect({props, script, 0});
    child.connect({panel, section, 0});
    child.connect({offset, section, 1});
    child.connect({offset, section, 2});
    child.connect({section, script, 1});
    child.runRuby(script);
    const auto open = [&] {
        return child.sendRubyMouseMessage(script, 1, {MouseEventKind::ButtonDown, {3, 3}, MouseButton::Left});
    };

    check(!root.pendingDropList() && open() && root.pendingDropList(), "Clicking the View opens the Ruby menu");
    const auto& list = *root.pendingDropList();
    check(list.view.isSamePanel(child.readPanel(panel).view) && list.ownerPath == std::vector{module} &&
          list.owner == script && list.position == green::Point{1.25F, 1.5F} &&
          list.view.mapping.toPanel(list.position) == green::Point{3.25F, 3.5F},
          "The menu opens from the specified View and maps its local anchor");
    check(list.contents.options == StringArray{"Sine", "Triangle", "Carré"} &&
          list.contents.menus.front().size() == 4 && list.contents.menus.front()[1].optionIndex == -1 &&
          list.checkedIndex == 1 && list.disabledIndex == 0 && list.layout == DropListLayout::FixedColumns &&
          list.rowsPerColumn == 2, "The menu separates visual rows from selectable indices and uses the requested style");

    const auto token = list.token;
    root.finishDropList(token, 0);
    root.finishDropList(token, 10);
    check(root.pendingDropList() && child.inspectOutput(script, 0).triggers == 0,
          "Disabled and out-of-range choices cannot select an item");
    root.finishDropList(token, 2);
    check(!root.pendingDropList() && std::get<StringArray>(child.readValue(script)) == StringArray{":wave", "2", "Carré"} &&
          child.inspectOutput(script, 0).triggers == 1, "Selection reports the original Ruby ID, index, and label once");
    root.finishDropList(token, 1);
    check(open() && root.pendingDropList() && root.pendingDropList()->token != token, "A new opening has a fresh token");
    root.finishDropList(root.pendingDropList()->token, std::nullopt);
    check(!root.pendingDropList() && child.inspectOutput(script, 0).triggers == 1,
          "Dismissal closes the menu without invoking dropListFinished");
}

void rubyDropListCanReopenFromTheSelectionCallback()
{
    Schematic root({}, {}, {}, {}, {.ruby = desktop::makeRubyBackend()});
    auto& child = *root.module(root.addNode(Module, 0, 0));
    const auto panel = child.addNode(ModuleGui, 0, 0);
    const auto start = child.addNode(Trigger, 0, 0);
    const auto script = child.addNode(Ruby, 0, 0);
    child.setRubyDraft(script, R"ruby(# input Trigger start
# input View canvas
# output String selected

def event
  createDropList "canvas", 1, [0, 0], "Apple,Pear", nil, "scroll"
end

def dropListFinished(id, index, name)
  output "selected", "#{id}:#{index}:#{name}"
  createDropList 1, 2, [0, 0], "Yes,No" if id == 1
end
)ruby");
    child.applyRubyPorts(script);
    child.connect({start, script, 0}, LinkNotification::Silent);
    child.connect({panel, script, 1});
    child.runRuby(script);
    child.fireTrigger(start);
    check(root.pendingDropList() && root.pendingDropList()->layout == DropListLayout::Scroll,
          "A Green event can open a scrolling Ruby menu");
    const auto first = root.pendingDropList()->token;
    root.finishDropList(first, 0);
    check(root.pendingDropList() && root.pendingDropList()->token != first &&
          root.pendingDropList()->layout == DropListLayout::AutoColumns &&
          std::get<std::string>(child.readValue(script)) == "1:0:Apple",
          "Selecting index zero can open another menu with the default layout");
    const auto next = root.pendingDropList()->token;
    root.finishDropList(first, 1);
    check(root.pendingDropList() && root.pendingDropList()->token == next,
          "A stale selection leaves its replacement open");
    child.stopRuby(script);
    root.finishDropList(next, 1);
    check(!root.pendingDropList() && child.inspectOutput(script, 0).triggers == 1,
          "Stopping the node closes its popup and invalidates any outstanding response");
}


void rubyMessageBoxReportsEachButton()
{
    Schematic root({}, {}, {}, {}, {.ruby = desktop::makeRubyBackend()});
    const auto module = root.addNode(Module, 0, 0);
    auto& child = *root.module(module);
    const auto start = child.addNode(Trigger, 0, 0);
    const auto flags = child.addNode(String, 0, 0);
    const auto script = child.addNode(Ruby, 0, 0);
    child.setRubyDraft(script, R"ruby(# input Trigger start
# input String options
# output StringArray reply

def event(i)
  messageBox :notice, "Save Café?", "Session", @options if i == "start"
end

def messageBoxFinished(id, result)
  output "reply", [id.inspect, result]
end
)ruby");
    child.applyRubyPorts(script);
    child.connect({start, script, 0}, LinkNotification::Silent);
    child.connect({flags, script, 1});
    child.runRuby(script);

    struct Case {
        std::string flags;
        std::vector<std::string_view> buttons;
        int icon;
        std::size_t defaultButton;
    };
    const Case cases[]{
        {"", {"ok"}, 0, 0},
        {"asterisk / okCancel", {"ok", "cancel"}, 1, 0},
        {"RETRYCANCEL warning", {"retry", "cancel"}, 3, 0},
        {"defbutton2 | question ; yesnocancel", {"yes", "no", "cancel"}, 2, 1},
        {"yesno exclaimation defbutton4", {"yes", "no"}, 3, 0},
        {"ABORTRETRYIGNORE+error+DEFBUTTON3", {"abort", "retry", "ignore"}, 4, 2},
    };
    for (const auto& sample : cases) {
        child.setValue(flags, sample.flags);
        for (const auto button : sample.buttons) {
            const auto pulses = child.inspectOutput(script, 0).triggers;
            child.fireTrigger(start);
            const auto* request = root.pendingAppDialog();
            check(request && request->ownerPath == std::vector{module} && request->node == script &&
                  request->title == "Session" && request->message == "Save Café?" &&
                  request->icon == sample.icon && request->rubyMessageBox &&
                  std::ranges::equal(request->rubyMessageBox->buttons, sample.buttons) &&
                  request->rubyMessageBox->defaultButton == sample.defaultButton,
                  "Opening resolves the Ruby message, title, icon, button set, and default");
            const auto token = request->token;
            check(!root.finishRubyMessageBox(token, "unknown") && !root.finishAppDialog(token, true) &&
                  child.inspectOutput(script, 0).triggers == pulses && root.pendingAppDialog()->token == token,
                  "An unoffered button or a native boolean result leaves the Ruby dialog open");
            check(root.finishRubyMessageBox(token, button) && !root.pendingAppDialog() &&
                  !root.finishRubyMessageBox(token, button) &&
                  child.inspectOutput(script, 0).triggers == pulses + 1 &&
                  std::get<StringArray>(child.readValue(script)) == StringArray{":notice", std::string(button)},
                  "Each button reports its exact name and the original Ruby ID once");
        }
    }
}

void rubyMessageBoxesQueueAndCloseWhenStopped()
{
    Schematic root({}, {}, {}, {}, {.ruby = desktop::makeRubyBackend()});
    const auto start = root.addNode(Trigger, 0, 0);
    const auto script = root.addNode(Ruby, 0, 0);
    root.setRubyDraft(script, R"ruby(# input Trigger start
# output String selected

def event
  messageBox 7, "First"
  messageBox 8, "Second"
end

def messageBoxFinished(id, result)
  output "selected", "#{id}:#{result}"
  messageBox 9, "Follow-up" if id == 7
end
)ruby");
    root.applyRubyPorts(script);
    root.connect({start, script, 0}, LinkNotification::Silent);
    root.runRuby(script);

    root.fireTrigger(start);
    const auto* first = root.pendingAppDialog();
    check(first && first->message == "First" && first->title.empty() &&
          first->rubyMessageBox && first->rubyMessageBox->buttons.size() == 1,
          "The first queued dialog opens with the default OK button");
    const auto firstToken = first->token;
    check(root.finishRubyMessageBox(firstToken, "ok") && root.pendingAppDialog() &&
          root.pendingAppDialog()->message == "Second" &&
          std::get<std::string>(root.readValue(script)) == "7:ok",
          "The result reaches the original numeric ID and advances to the next queued dialog");
    const auto second = root.pendingAppDialog()->token;
    check(!root.finishRubyMessageBox(firstToken, "ok") && root.finishRubyMessageBox(second, "ok") &&
          root.pendingAppDialog() && root.pendingAppDialog()->message == "Follow-up",
          "A callback can enqueue a follow-up and a stale result cannot close the next dialog");
    const auto last = root.pendingAppDialog()->token;
    root.stopRuby(script);
    check(!root.pendingAppDialog() && !root.finishRubyMessageBox(last, "ok") &&
          root.inspectOutput(script, 0).triggers == 2, "Stopping Ruby closes its dialogs without delivering a stale result");
}


void rubyPlaySoundContinuesDuringAsyncPlayback(const QTemporaryDir& files)
{
    using desktop::RubySoundMode;
    std::optional<std::string> playing;
    std::vector<RubySoundMode> modes;
    int stops = 0;
    Schematic root({}, {}, {}, {}, {.ruby = desktop::makeRubyBackend(
        [&](std::optional<std::string> file, RubySoundMode mode) {
            if (file) modes.push_back(mode);
            else ++stops;
            playing = std::move(file);
        })});
    auto& child = *root.module(root.addNode(Module, 0, 0));
    const auto start = child.addNode(Trigger, 0, 0);
    const auto loop = child.addNode(Trigger, 0, 0);
    const auto stop = child.addNode(Trigger, 0, 0);
    const auto script = child.addNode(Ruby, 0, 0);
    child.setRubyDraft(script, R"ruby(# input Trigger start
# input Trigger repeat
# input Trigger stop
# output String result

def event(i)
  if i == "stop"
    playSound nil
  elsif i == "repeat"
    playSound "Sounds\\chime.wav", "loop"
  else
    playSound "Sounds\\chime.wav"
  end
  output "result", i.name
end

def saveState
  nil
end
)ruby");
    child.applyRubyPorts(script);
    child.connect({start, script, 0}, LinkNotification::Silent);
    child.connect({loop, script, 1}, LinkNotification::Silent);
    child.connect({stop, script, 2}, LinkNotification::Silent);
    root.setSchematicFilePath(files.filePath("original/bench.ostone").toStdString());
    child.runRuby(script);

    child.fireTrigger(start);
    check(playing == files.filePath("original/Sounds/chime.wav").toStdString() &&
          modes == std::vector{RubySoundMode::Async} &&
          std::get<std::string>(child.readValue(script)) == "start",
          "Default playback starts from the schematic directory and Ruby continues while it is active");
    root.setSchematicFilePath(files.filePath("moved/bench.ostone").toStdString());
    child.fireTrigger(loop);
    check(playing == files.filePath("moved/Sounds/chime.wav").toStdString() &&
          modes.back() == RubySoundMode::Loop, "Loop playback resolves relative paths after Save As");
    child.fireTrigger(stop);
    check(!playing && stops == 1 && std::get<std::string>(child.readValue(script)) == "stop",
          "Passing nil stops playback and returns to Ruby");
    child.fireTrigger(loop);
    child.stopRuby(script);
    check(!playing && stops == 2, "Stopping a stateful Ruby node also stops its ongoing sound");
}

void rubyPlaySoundSyncWaitsForCompletion()
{
    using desktop::RubySoundMode;
    bool completed = false;
    Schematic root({}, {}, {}, {}, {.ruby = desktop::makeRubyBackend(
        [&](std::optional<std::string> file, RubySoundMode mode) {
            check(file.has_value() && mode == RubySoundMode::Sync, "Ruby requests synchronous playback");
            std::this_thread::sleep_for(1100ms);
            completed = true;
        })});
    const auto start = root.addNode(Trigger, 0, 0);
    const auto script = root.addNode(Ruby, 0, 0);
    root.setRubyDraft(script, R"ruby(# input Trigger start
# output Float elapsed

def event
  before = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  playSound "chime.wav", "sync"
  output "elapsed", Process.clock_gettime(Process::CLOCK_MONOTONIC) - before
end
)ruby");
    root.applyRubyPorts(script);
    root.connect({start, script, 0}, LinkNotification::Silent);
    root.runRuby(script);

    root.fireTrigger(start);
    check(completed && std::get<float>(root.readValue(script)) >= 1.05F &&
          root.inspectOutput(script, 0).triggers == 1,
          "The next Ruby line waits for playback even when it exceeds the Ruby execution budget");
}


void rubySchematicLocationTracksSaveAndReopen(const QTemporaryDir& files)
{
    using desktop::SchematicFile;
    const auto makeSession = [] { return Session({}, {}, {}, {}, {.ruby = desktop::makeRubyBackend()}); };
    auto project = makeSession();
    const auto module = project.graph().addNode(Module, 0, 0);
    auto& child = *project.graph().module(module);
    const auto node = child.addNode(Ruby, 0, 0);
    const auto trigger = child.addNode(Trigger, 0, 0);
    child.setRubyDraft(node, R"ruby(# input Trigger query
# output String folder

output schematicLocation

def event
  location = schematicLocation
  location.replace("changed locally")
  output schematicLocation
end
)ruby");
    child.applyRubyPorts(node);
    child.connect({trigger, node, 0}, LinkNotification::Silent);
    child.runRuby(node);
    check(child.inspectOutput(node).triggers == 1 && std::get<std::string>(child.readValue(node)).empty(),
          "Ruby returns an empty String when initialized in an unsaved project");

    const QDir directory(files.path());
    const auto save = [&](const QString& folder) {
        check(directory.mkpath(folder), "Create the project folder");
        const auto path = directory.filePath(folder + "/Location.ostone");
        SchematicFile::saveProject(project, path);
        project.graph().setSchematicFilePath(path.toStdString());
        child.fireTrigger(trigger);
        check(std::get<std::string>(child.readValue(node)) == directory.filePath(folder).toStdString(),
              "A running Ruby node reads the containing folder after a successful save");
        return path;
    };
    save(QStringLiteral("Original project"));
    const auto moved = save(QStringLiteral("Projet déplacé"));
    check(child.inspectOutput(node).triggers == 3, "Saving twice allows the same Ruby node to report its latest folder");

    auto reopened = makeSession();
    check(SchematicFile::loadProject(reopened, SchematicFile::read(moved), {}, moved), "Reopen the project after Save As");
    auto& restored = *reopened.graph().module(module);
    restored.runRuby(node);
    check(std::get<std::string>(restored.readValue(node)) == directory.filePath(QStringLiteral("Projet déplacé")).toStdString(),
          "Ruby initialization in a reopened module sees the root project's folder");
}

void rubySchematicLocationUsesExportLocation(const QTemporaryDir& files)
{
    Schematic graph({}, {}, {}, {}, {.ruby = desktop::makeRubyBackend()});
    const auto node = graph.addNode(Ruby, 0, 0);
    const auto trigger = graph.addNode(Trigger, 0, 0);
    graph.setRubyDraft(node, R"ruby(# input Trigger query
# output String folder

output schematicLocation

def event
  output schematicLocation
end
)ruby");
    graph.applyRubyPorts(node);
    graph.connect({trigger, node, 0}, LinkNotification::Silent);

    const auto appFolder = files.filePath(QStringLiteral("Standalone"));
    graph.setSchematicFilePath((appFolder + "/Instrument.exe").toStdString());
    graph.runRuby(node);
    check(std::get<std::string>(graph.readValue(node)) == appFolder.toStdString(),
          "A standalone runtime that supplies the executable path sees the executable folder");

    const auto pluginFolder = files.filePath(QStringLiteral("Installed Plug-ins"));
    graph.setPluginHostInfo(PluginHostInfo{.pluginFolder = pluginFolder.toStdString()});
    graph.fireTrigger(trigger);
    check(std::get<std::string>(graph.readValue(node)) == pluginFolder.toStdString(),
          "A hosted runtime sees the installed plug-in folder even with a project path present");

    graph.setSchematicFilePath({});
    graph.fireTrigger(trigger);
    check(std::get<std::string>(graph.readValue(node)) == pluginFolder.toStdString(),
          "The installed plug-in folder also works without a schematic path");

    graph.setPluginHostInfo(std::nullopt);
    graph.fireTrigger(trigger);
    check(std::get<std::string>(graph.readValue(node)).empty(),
          "Removing the host restores the empty location for an unsaved project");
}

}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    if (!openstone::desktop::rubyAvailable()) {
        std::cout << "Ruby executable is unavailable\n";
        return 77;
    }
    try {
        QTemporaryDir files;
        check(files.isValid(), "Create a temporary workspace");
        rubyLinkedSourceUpdatesAfterGreenDispatch(files.filePath(QStringLiteral("ruby-source.ostone")));
        runsIndependentRubyNodesAndReopens(files.filePath(QStringLiteral("ruby.ostone")));
        editsTopLevelScriptAndKeepsCompatibleWires();
        appliesPortsWithoutEvaluatingAndSupportsUndo();
        visualRubyPortsSurviveEditsAndReopening(files.filePath(QStringLiteral("ruby-ports.ostone")));
        rubyRunsTheFlowStoneKnobWithVisualPorts();
        rubyValuePassesLiveArraysHashesAndCustomObjects();
        rubyMidiRoutesChannelMessagesAndSysEx(files.filePath(QStringLiteral("ruby-midi.ostone")));
        rubySchedulesInputsAndOutputsAgainstSchematicClock();
        rubyQueueOrdersAcrossNodesAndKeepsMethodArguments();
        rubyClearEventsCancelsOnlyTheCallingNode();
        rubyDrawsARectangleAndEllipseThroughThePanelView();
        rubyDrawsLinesPolygonsAndRoundedRectangles();
        rubyDrawsTextAtAPointAndInARectangle();
        rubyMeasuresTextWithTheDrawingLayout();
        rubyDrawUsesTheViewConnectorAndSectionMapping();
        rubyRedrawTargetsOneView();
        rubyPartialRedrawKeepsPaintOutsideTheMappedArea();
        rubyMouseUsesLocalCoordinatesAndCapture();
        rubyMouseRequiresHitTestAndExplicitCapture();
        rubySelectsHoverCursorInLocalViewCoordinates();
        rubyCursorVisibilityCountsNestedCallsAndClearsDragState();
        rubyEditControlReportsChangesAndCommits();
        rubyFinishedCallbackCanOpenANewEditor();
        rubyDropListReportsTheSelectedIndexAndName();
        rubyDropListCanReopenFromTheSelectionCallback();
        rubyMessageBoxReportsEachButton();
        rubyMessageBoxesQueueAndCloseWhenStopped();
        rubyPlaySoundContinuesDuringAsyncPlayback(files);
        rubyPlaySoundSyncWaitsForCompletion();
        rubySchematicLocationTracksSaveAndReopen(files);
        rubySchematicLocationUsesExportLocation(files);
        rubyStateRoundTripsThroughProject(files.filePath(QStringLiteral("ruby-state.ostone")));
        failedRubySaveLeavesPreviousProjectIntact(files.filePath(QStringLiteral("ruby-save-error.ostone")));
        rubyValueRoutesThroughSavedModulePorts(files.filePath(QStringLiteral("ruby-value.ostone")));
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
