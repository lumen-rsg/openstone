#include "../ReferenceFixtures.hpp"
#include "Schematic.hpp"
#include "Session.hpp"
#include "PresetText.hpp"
#include "LegacyPreset.hpp"
#include "AudioPatch.hpp"
#include "StringConversions.hpp"
#include "stream/VoiceBank.hpp"
#include "stream/PolyRuntime.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <numbers>
#include <stdexcept>

namespace {

using openstone::LinkNotification;
using openstone::PrimitiveKind;
using openstone::Schematic;
using openstone::SchematicLink;

void check(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::int32_t integer(const Schematic& schematic, int id, int output = 0)
{
    return std::get<std::int32_t>(schematic.readValue(id, output));
}

const openstone::SchematicNode& findNode(const Schematic& graph, int id)
{
    const auto found = std::ranges::find(graph.nodes(), id, &openstone::SchematicNode::id);
    check(found != graph.nodes().end(), "The expected node should exist");
    return *found;
}

void keyDownPublishesOneCoherentRecordPerMatchingPress()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    Schematic graph;
    const int down = graph.addNode(KeyDown, 0, 0);
    const int enabled = graph.addNode(Boolean, 0, 0, false);
    const int codes = graph.addNode(MultilineString, 0, 0, std::string{"65\n112"});
    const int filter = graph.addNode(Array, 0, 0);
    graph.setTemplateType(filter, Type::IntArray);
    graph.connect({codes, filter, 4});
    const int textAtCodePulse = graph.addNode(SampleAndHold, 0, 0);
    graph.connect({enabled, down, 0});
    graph.connect({filter, down, 1});
    graph.connect({down, textAtCodePulse, 0, 3});
    graph.connect({down, textAtCodePulse, 1, 0}, LinkNotification::Silent);

    check(graph.inputType(down, 0) == Type::Boolean && graph.inputType(down, 1) == Type::IntArray &&
          graph.outputType(down, 0) == Type::Int && graph.outputType(down, 1) == Type::Int &&
          graph.outputType(down, 2) == Type::Int && graph.outputType(down, 3) == Type::String,
          "Key Down exposes the FlowStone Alpha connector types");
    check(!graph.sendKeyEvent({65, true, 1, 0, "a"}) && graph.inspectOutput(down).triggers == 0,
          "Disabled Key Down ignores presses");
    graph.setValue(enabled, true);
    check(!graph.sendKeyEvent({66, true, 1, 0, "b"}) && graph.inspectOutput(down).triggers == 0,
          "Unlisted keys and enable changes do not send press triggers");
    check(graph.sendKeyEvent({65, true, 1, 0x1e, "a"}) && graph.readValue(down, 0) == Value{65} &&
          graph.readValue(down, 1) == Value{1} && graph.readValue(down, 2) == Value{0x1e} &&
          graph.readValue(textAtCodePulse) == Value{std::string{"a"}},
          "The Code pulse can read the new character and the event's Int outputs");
    check(graph.sendKeyEvent({65, true, 3, 0x401e, "A"}) &&
          graph.readValue(down, 1) == Value{3} && graph.readValue(down, 2) == Value{0x401e} &&
          graph.readValue(down, 3) == Value{std::string{"A"}},
          "Auto-repeat delivers each incoming message with its repeat count and flags");
    check(!graph.sendKeyEvent({65, false, 1, 0xc01e, "A"}) && graph.inspectOutput(down).triggers == 2 &&
          graph.inspectOutput(down, 1).triggers == 2 && graph.inspectOutput(down, 2).triggers == 2 &&
          graph.inspectOutput(down, 3).triggers == 2,
          "Each matched press pulses all four outlets once and releases leave them alone");
}

void keyUpTracksReleasesInNestedModulesAndHonorsTheFilter()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    Schematic graph;
    auto& child = *graph.module(graph.addNode(Module, 0, 0));
    const int up = child.addNode(KeyUp, 0, 0);
    const int enabled = child.addNode(Boolean, 0, 0, true);
    const int codes = child.addNode(MultilineString, 0, 0, std::string{});
    const int filter = child.addNode(Array, 0, 0);
    child.setTemplateType(filter, Type::IntArray);
    child.connect({codes, filter, 4});
    child.connect({enabled, up, 0});
    child.connect({filter, up, 1});
    check(child.inputType(up, 0) == Type::Boolean && child.inputType(up, 1) == Type::IntArray &&
          child.outputType(up, 0) == Type::Int && child.outputType(up, 1) == Type::Int &&
          child.outputType(up, 2) == Type::Int && child.outputType(up, 3) == Type::String,
          "Key Up exposes the same connector shape as Key Down");
    check(!graph.sendKeyEvent({37, true}) && child.inspectOutput(up).triggers == 0,
          "Presses do not fire Key Up");
    check(graph.sendKeyEvent({37, false, 1, 0xc100, ""}) && child.readValue(up, 0) == Value{37} &&
          child.readValue(up, 1) == Value{1} && child.readValue(up, 2) == Value{0xc100} &&
          child.readValue(up, 3) == Value{std::string{}},
          "An empty filter accepts all releases, including keys with no text");
    child.setValue(codes, std::string{"65"});
    check(!graph.sendKeyEvent({66, false, 1, 0xc000, "b"}) && child.inspectOutput(up).triggers == 1,
          "Changing Key Codes restricts future releases without issuing an event");
    check(graph.sendKeyEvent({65, false, 1, 0xc000, "a"}) &&
          child.readValue(up, 3) == Value{std::string{"a"}} && child.inspectOutput(up).triggers == 2,
          "A matching release sends its payload to a nested module");
    child.setValue(enabled, false);
    check(!graph.sendKeyEvent({65, false, 1, 0xc000, "a"}) && child.inspectOutput(up).triggers == 2,
          "Disabling a tracker leaves its last record in place");
}

void isKeyPressedReadsHeldStateAndPulsesOnlyOnCheck()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    Schematic graph;
    auto& child = *graph.module(graph.addNode(Module, 0, 0));
    const int key = child.addNode(String, 0, 0, std::string{"q"});
    const int checkPulse = child.addNode(Trigger, 0, 0);
    const int query = child.addNode(IsKeyPressed, 0, 0);
    child.connect({key, query, 0});
    child.connect({checkPulse, query, 1});
    check(child.inputType(query, 0) == Type::String && child.inputType(query, 1) == Type::Trigger &&
          child.outputType(query) == Type::Boolean && child.readValue(query) == Value{false} &&
          child.inspectOutput(query).triggers == 0, "The Check connector waits for an actual pulse");

    graph.sendKeyEvent({81, true, 1, 0, "q"});
    check(child.readValue(query) == Value{true} && child.inspectOutput(query).triggers == 0,
          "Held state is readable across modules without emitting a trigger");
    child.fireTrigger(checkPulse);
    check(child.readValue(query) == Value{true} && child.inspectOutput(query).triggers == 1,
          "Check sends the current Boolean");
    child.setValue(key, std::string{"81"});
    check(child.readValue(query) == Value{true} && child.inspectOutput(query).triggers == 1,
          "A decimal Windows virtual-key code selects the same key without pulsing");
    graph.sendKeyEvent({81, false});
    child.fireTrigger(checkPulse);
    check(child.readValue(query) == Value{false} && child.inspectOutput(query).triggers == 2,
          "Checking a released key sends false");
    graph.sendKeyEvent({0xa2, true});
    graph.sendKeyEvent({0xa3, true});
    child.setValue(key, std::string{" CtRl "});
    check(child.readValue(query) == Value{true}, "A case-insensitive alias matches either side of Ctrl");
    graph.sendKeyEvent({0xa2, false});
    check(child.readValue(query) == Value{true}, "Releasing left Ctrl leaves right Ctrl held");
    graph.clearPressedKeys();
    check(child.readValue(query) == Value{false} && child.inspectOutput(query).triggers == 2,
          "Losing keyboard focus clears stale state without generating a Check pulse");
    child.setValue(key, std::string{"256"});
    child.fireTrigger(checkPulse);
    check(child.readValue(query) == Value{false}, "Out-of-range key codes read false");
}

void presentationControlsTrackTheFocusedPanelAndHostReports()
{
    using enum PrimitiveKind;
    using openstone::green::Value;
    using PanelColor = openstone::yellow::Color;
    Schematic graph;
    const int focused = graph.addNode(Module, 0, 0);
    const int sibling = graph.addNode(Module, 0, 0);
    auto& panel = *graph.module(focused);
    auto& other = *graph.module(sibling);
    const int nested = panel.addNode(Module, 0, 0);
    auto& controls = *panel.module(nested);
    const int color = panel.addNode(PrimitiveKind::Color, 0, 0, PanelColor{0xff123456});
    const int background = panel.addNode(PanelBackgroundColor, 0, 0);
    other.addNode(PanelBackgroundColor, 0, 0);
    const int toggle = controls.addNode(Trigger, 0, 0);
    const int fullScreen = controls.addNode(FullScreen, 0, 0);
    const int otherFullScreen = other.addNode(FullScreen, 0, 0);
    const int otherToggle = other.addNode(Trigger, 0, 0);
    const int exit = controls.addNode(FocusMode, 0, 0);
    const int exitPulse = controls.addNode(Trigger, 0, 0);
    const int scale = controls.addNode(Float, 0, 0, 1.5F);
    const int zoom = controls.addNode(PanelZoom, 0, 0);
    const int set = controls.addNode(Trigger, 0, 0);
    const std::array path{focused};
    const std::array siblingPath{sibling};

    check(!panel.presentationBackground() && !other.presentationBackground(),
          "An unconnected Background Color lets the window use its default color");
    panel.connect({color, background, 0});
    check(panel.presentationBackground() == PanelColor{0xff123456}, "The focused panel reads its authored background");
    panel.setValue(color, PanelColor{0xffabcdef});
    check(panel.presentationBackground() == PanelColor{0xffabcdef} && !other.presentationBackground(),
          "The background updates with the input and belongs to its panel");

    controls.connect({toggle, fullScreen, 0});
    other.connect({otherToggle, otherFullScreen, 0});
    controls.connect({scale, zoom, 0});
    controls.fireTrigger(toggle);
    check(graph.presentation().fullScreenToggles == 0, "A window command has no effect before a presentation opens");

    graph.setPresentationPanel(std::vector<int>{focused});
    controls.connect({set, zoom, 1});
    controls.connect({exitPulse, exit, 0});
    check(graph.presentation().panelPath && graph.presentation().requestedScale == 1,
          "Connecting Set and Exit does not issue presentation commands");
    other.fireTrigger(otherToggle);
    check(graph.presentation().fullScreenToggles == 0, "Commands in a sibling module cannot affect the focused panel");
    controls.fireTrigger(toggle);
    check(graph.presentation().fullScreenToggles == 1 && controls.readValue(fullScreen) == Value{false},
          "A nested toggle requests full screen and waits for a host report");
    check(!graph.reportPresentation(siblingPath, true, 2) && graph.reportPresentation(path, true, 1),
          "Only the active window can report presentation state");
    check(controls.readValue(fullScreen) == Value{true} && controls.inspectOutput(fullScreen).triggers == 1 &&
          other.readValue(otherFullScreen) == Value{false} && other.inspectOutput(otherFullScreen).triggers == 0,
          "The active panel's Full Screen output updates independently of sibling panels");

    controls.fireTrigger(set);
    check(graph.presentation().requestedScale == 1.5F && controls.readValue(zoom) == Value{1.F},
          "Zoom accepts a fractional scale and reports the applied scale separately");
    check(graph.reportPresentation(path, true, 1.25F) && controls.readValue(zoom) == Value{1.25F},
          "The Zoom output follows the scale achieved on the host");
    controls.setValue(scale, 0.F);
    check(graph.presentation().requestedScale == 1.5F, "A change on Scale waits for Set");
    controls.fireTrigger(set);
    check(graph.presentation().requestedScale == 0.F && graph.reportPresentation(path, true, 2.25F),
          "Zero requests fit to screen and reports the fitted scale");
    check(!graph.reportPresentation(path, true, 2.25F) && controls.inspectOutput(zoom).triggers == 2,
          "An unchanged host report produces no new pulse");
    controls.setValue(scale, -1.F);
    controls.fireTrigger(set);
    check(graph.presentation().requestedScale == 0.F, "Negative zoom requests leave the current scale in place");

    controls.fireTrigger(toggle);
    check(graph.presentation().fullScreenToggles == 2 && graph.reportPresentation(path, false, 2.F),
          "A second pulse requests return to the windowed state");
    controls.fireTrigger(exitPulse);
    check(!graph.presentation().panelPath && controls.readValue(zoom) == Value{1.F} &&
          controls.readValue(fullScreen) == Value{false} && !graph.reportPresentation(path, true, 3),
          "Exit ends the preview, resets readouts and ignores reports from the closed window");
    panel.disconnectLink({color, background, 0});
    check(!panel.presentationBackground(), "Removing the color restores the window default");
}

void gridConversionsRoundTripFloatsAndSignedAreas()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    Schematic graph;
    const int distance = graph.addNode(Float, 0, 0, 1.25F);
    const int pixels = graph.addNode(GridToPixel, 0, 0);
    const int grid = graph.addNode(PixelToGrid, 0, 0);
    graph.connect({distance, pixels, 0});
    graph.connect({pixels, grid, 0});
    check(graph.readValue(pixels) == Value{10.F} && graph.readValue(grid) == Value{1.25F},
          "Float conversions use the logical window grid scale and retain fractions");
    const auto pulses = graph.inspectOutput(grid).triggers;
    graph.setValue(distance, -2.125F);
    check(graph.readValue(pixels) == Value{-17.F} && graph.readValue(grid) == Value{-2.125F} &&
          graph.inspectOutput(grid).triggers == pulses + 1, "Float updates propagate through both conversions");

    const int area = graph.addNode(Area, 0, 0);
    const int areaPixels = graph.addNode(GridToPixelArea, 0, 0);
    const int areaGrid = graph.addNode(PixelToGridArea, 0, 0);
    const std::array coordinates{-1.25F, .5F, 2.25F, -.75F};
    for (int i = 0; i < coordinates.size(); ++i) {
        const int field = graph.addNode(Float, 0, 0, coordinates[i]);
        graph.connect({field, area, i});
    }
    graph.connect({area, areaPixels, 0});
    graph.connect({areaPixels, areaGrid, 0});
    check(graph.readValue(areaPixels) == Value{openstone::yellow::Area{-10, 4, 18, -6}} &&
          graph.readValue(areaGrid) == graph.readValue(area),
          "Area conversions scale the origin and signed extent independently");
}

void moduleTooltipUsesItsLocalTitleAndParagraphs()
{
    using enum PrimitiveKind;
    Schematic graph;
    check(!graph.canPlace(TooltipHelp), "Tooltip Help belongs inside a module");
    const int module = graph.addNode(Module, 0, 0);
    const int peer = graph.addNode(Module, 100, 0);
    auto& child = *graph.module(module);
    const int help = child.addNode(TooltipHelp, 0, 0);
    const int text = child.addNode(String, 0, 0, std::string{"Envelope\\nAttack & decay.\r\n\r\nKeep <0 dB."});
    child.connect({text, help, 0});
    const auto contents = graph.readModuleHelp(module);
    check(contents.title == "Envelope" && contents.body == "Attack & decay.\n\nKeep <0 dB." &&
          graph.readModuleHelp(peer).title.empty(), "Only the containing module receives its tooltip");
    child.setValue(text, std::string{"Revised\nA new paragraph"});
    const auto revised = graph.readModuleHelp(module);
    check(revised.title == "Revised" && revised.body == "A new paragraph", "Tooltip updates when the source changes");
}

void redrawLimiterTracksTheClockAndHostSuspension()
{
    using namespace std::chrono_literals;
    using namespace openstone;
    using namespace openstone::green;
    using enum PrimitiveKind;
    TickTime now{};
    Schematic graph({}, {}, {}, {}, {.monotonicClock = [&] { return now; }});
    const int source = graph.addNode(Trigger, 0, 0);
    const int limiter = graph.addNode(RedrawLimiter, 0, 0);
    const int state = graph.addNode(RedrawsSuspended, 0, 0);
    graph.connect({source, limiter, 0});
    const auto pulses = [&] { return graph.inspectOutput(limiter).triggers; };
    check(pulses() == 0 && graph.readValue(state) == Value{false}, "The editor starts with redraws enabled");
    graph.fireTrigger(source);
    now += 9ms;
    graph.fireTrigger(source);
    check(pulses() == 1, "The limiter passes the first pulse and drops pulses within ten milliseconds");
    now = TickTime{} + 10ms;
    graph.fireTrigger(source);
    check(pulses() == 2, "A pulse at the next interval advances the limiter");
    const auto statePulses = graph.inspectOutput(state).triggers;
    graph.setPluginHostInfo(PluginHostInfo{"Host", "Test", "1"});
    now = TickTime{} + 50ms;
    graph.fireTrigger(source);
    check(pulses() == 2 && graph.readValue(state) == Value{true}, "A hidden plug-in editor suspends redraw triggers");
    graph.setPluginEditorOpen(true);
    graph.fireTrigger(source);
    check(pulses() == 3 && graph.readValue(state) == Value{false} &&
          graph.inspectOutput(state).triggers == statePulses + 2,
          "Opening the editor re-enables new pulses and publishes both state changes");
    now = TickTime{} + 80ms;
    graph.setRedrawsSuspended(true);
    graph.fireTrigger(source);
    graph.setRedrawsSuspended(false);
    check(pulses() == 3, "Explicit host suspension drops pulses without replaying them");
    graph.fireTrigger(source);
    check(pulses() == 4, "A fresh trigger passes once redraws resume");
}

void presetRecallSuspendsRedrawsDuringParameterChanges()
{
    using namespace openstone;
    using namespace openstone::green;
    using enum PrimitiveKind;
    Schematic graph;
    const int manager = graph.addNode(PresetManager, 0, 0);
    const int gain = graph.addNode(PresetFloat, 0, 0);
    const int count = graph.addNode(Int, 0, 0, 2);
    const int program = graph.addNode(Int, 0, 0, 0);
    const int value = graph.addNode(Float, 0, 0, .5F);
    const int limiter = graph.addNode(RedrawLimiter, 0, 0);
    const int suspended = graph.addNode(RedrawsSuspended, 0, 0);
    const int held = graph.addNode(SampleAndHold, 0, 0);
    graph.connect({count, manager, 0});
    graph.connect({program, manager, 1});
    graph.connect({manager, gain, 0});
    graph.connect({value, gain, 1});
    graph.connect({suspended, held, 0});
    graph.connect({gain, held, 1}, LinkNotification::Silent);
    graph.connect({gain, limiter, 0});
    graph.setValue(program, 1);
    check(graph.readValue(held) == Value{true} && graph.inspectOutput(limiter).triggers == 0,
          "Listeners see suspension during a preset value change and its redraw pulse is dropped");
    check(graph.readValue(suspended) == Value{false} && graph.inspectOutput(suspended).triggers == 2,
          "Preset recall restores redraws and publishes entry and exit");
}

void presetsRecallTypedProgramsAndRespectLock()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    Schematic graph;
    const int manager = graph.addNode(PresetManager, 0, 0);
    const int gain = graph.addNode(PresetFloat, 0, 0);
    const int toggle = graph.addNode(PresetBool, 0, 0);
    const int text = graph.addNode(PresetString, 0, 0);
    const int array = graph.addNode(PresetArray, 0, 0);
    const int count = graph.addNode(Int, 0, 0, 2);
    const int program = graph.addNode(Int, 0, 0);
    const int lock = graph.addNode(Boolean, 0, 0);
    const int save = graph.addNode(Trigger, 0, 0);
    const int clear = graph.addNode(Trigger, 0, 0);
    const int clearAll = graph.addNode(Trigger, 0, 0);
    const int value = graph.addNode(Float, 0, 0, .25f);
    const int enabled = graph.addNode(Boolean, 0, 0, true);
    const int label = graph.addNode(String, 0, 0, std::string{"First"});
    const int size = graph.addNode(Int, 0, 0, 3);
    const int index = graph.addNode(Int, 0, 0, 1);
    const int capture = graph.addNode(SampleAndHold, 0, 0);
    const int names = graph.addNode(String, 0, 0, std::string{"Init\r\nSecond"});
    graph.connect({count, manager, 0});
    graph.connect({program, manager, 1});
    graph.connect({lock, manager, 3});
    graph.connect({names, manager, 5});
    check(std::get<std::string>(graph.readValue(manager, 3)) == "Init\nSecond",
          "Preset Manager reads CRLF-delimited names");
    graph.connect({save, manager, 4}, LinkNotification::Silent);
    graph.connect({clear, manager, 6}, LinkNotification::Silent);
    graph.connect({clearAll, manager, 7}, LinkNotification::Silent);
    for (const auto parameter : {gain, toggle, text, array}) graph.connect({manager, parameter, 0});
    graph.connect({value, gain, 1});
    graph.connect({enabled, toggle, 1});
    graph.connect({label, text, 1});
    graph.connect({label, manager, 2});
    graph.connect({size, array, 1});
    graph.connect({index, array, 2});
    graph.connect({value, array, 3});
    graph.connect({gain, capture, 0});
    graph.connect({gain, capture, 1, 1}, LinkNotification::Silent);

    const auto gainValue = [&] { return std::get<float>(graph.readValue(gain)); };
    const auto arrayValue = [&] { return std::get<FloatArray>(graph.readValue(array)); };
    check(gainValue() == .25f && arrayValue() == FloatArray{0, .25f, 0}, "The first program stores typed edits");
    graph.setValue(program, 1);
    check(gainValue() == 0 && !std::get<bool>(graph.readValue(toggle)) &&
          std::get<std::string>(graph.readValue(text)).empty() && arrayValue() == FloatArray{0, 0, 0},
          "A fresh program uses parameter defaults");
    check(std::get<float>(graph.readValue(capture)) == .25f, "Before Change can read the previous value");
    check(graph.inspectOutput(manager, 4).triggers == 1 && graph.inspectOutput(manager, 5).triggers == 1,
          "A program change fires the two manager notifications once each");

    graph.setValue(value, .75f);
    graph.setValue(label, std::string{"Second"});
    graph.setValue(program, 0);
    check(gainValue() == .25f && std::get<bool>(graph.readValue(toggle)) &&
          std::get<std::string>(graph.readValue(text)) == "First" &&
          std::get<std::string>(graph.readValue(manager, 2)) == "First",
          "Recall restores each parameter, and the name, for the original program");

    graph.setValue(lock, true);
    graph.setValue(value, .5f);
    graph.setValue(label, std::string{"Discard me"});
    graph.setValue(program, 1);
    check(gainValue() == .75f && std::get<std::string>(graph.readValue(manager, 2)) == "Second",
          "A locked edit does not overwrite another program");
    graph.setValue(program, 0);
    check(gainValue() == .25f && std::get<std::string>(graph.readValue(manager, 2)) == "First",
          "Leaving a locked program discards unsaved edits");
    graph.setValue(value, .625f);
    graph.setValue(label, std::string{"Saved while locked"});
    graph.fireTrigger(save);
    graph.setValue(program, 1);
    graph.setValue(program, 0);
    check(gainValue() == .625f && arrayValue() == FloatArray{0, .625f, 0} &&
          std::get<std::string>(graph.readValue(text)) == "Saved while locked",
          "An explicit Save commits the current program while locked");
    graph.fireTrigger(clear);
    check(gainValue() == 0 && arrayValue() == FloatArray{0, 0, 0}, "Clear restores defaults for the current program");
    graph.setValue(program, 1);
    check(gainValue() == .75f, "Clear preserves the other programs");
    graph.fireTrigger(clearAll);
    graph.setValue(program, 0);
    graph.setValue(program, 1);
    check(gainValue() == 0, "Clear All resets other programs too");
}

void presetTextCodecReadsReferenceTablesAndPreservesStrings()
{
    using namespace openstone;
    using namespace openstone::green;
    constexpr std::string_view reference =
        "\xef\xbb\xbfPreset Table Format with Ordering\r\n"
        "Number of programs = 2\r\n"
        "Number of parameters = 3\r\n\r\n"
        "Parameter\tType\tOrder\tClean\tBright\r\n"
        "Mix\tFloat\t-1\t0.125\t+0.75\r\n"
        "Steps\tFloat Array (2)\t1\t\t\r\n"
        "0\t\t\t0.25\t0.5\r\n"
        "1\t\t\t0.75\t1\r\n"
        "Path\tString\t2\tC:\\new\\sample.wav\t\r\n";
    const auto table = readPresetText(reference);
    check(table.programs == std::vector<std::string>{"Clean", "Bright"} && table.parameters.size() == 3 &&
          table.parameters[0].values == std::vector<Value>{.125f, .75f} &&
          table.parameters[1].values == std::vector<Value>{FloatArray{.25f, .75f}, FloatArray{.5f, 1.0f}} &&
          table.parameters[2].values == std::vector<Value>{std::string{"C:\\new\\sample.wav"}, std::string{}},
          "The reference table accepts CRLF, an Order column, array rows and literal backslashes");

    const PresetTable extension{{"A\tB", "Next"}, {
        {"Notes", {std::string{"Line 1\r\nLine 2\t\\end"}, std::string{}}},
        {"Steps", {FloatArray{.123456789f, .125f}, FloatArray{}}},
        {"Enabled", {true, false}},
    }};
    const auto result = readPresetText(writePresetText(extension));
    check(result.programs == extension.programs && result.parameters[0].values == extension.parameters[0].values &&
          result.parameters[1].values == extension.parameters[1].values &&
          result.parameters[2].values == std::vector<Value>{1.0f, 0.0f},
          "Text round trips escaped strings, exact floats, unequal arrays and Boolean numeric values");
}

void presetTextTransfersScopedProgramsAndRejectsBrokenTables()
{
    using enum PrimitiveKind;
    using namespace openstone;
    using namespace openstone::green;
    Schematic graph;
    const int manager = graph.addNode(PresetManager, 0, 0);
    const int other = graph.addNode(PresetManager, 0, 0);
    const int group = graph.addNode(Module, 0, 0);
    const int sender = graph.addNode(WirelessOutput, 0, 0);
    auto& child = *graph.module(group);
    const int receiver = child.addNode(WirelessInput, 0, 0);
    graph.setWirelessLabel(sender, "Program Bank");
    child.setWirelessLabel(receiver, "Program Bank");
    graph.connect({manager, sender, 0});
    const int gain = child.addNode(PresetFloat, 0, 0);
    const int gainName = child.addNode(String, 0, 0, std::string{"Gain"});
    const int gainIn = child.addNode(Float, 0, 0, .25f);
    child.connect({receiver, gain, 0});
    child.connect({gainName, gain, 2});
    child.connect({gainIn, gain, 1});
    const int toggle = graph.addNode(PresetBool, 0, 0);
    const int unrelated = graph.addNode(PresetFloat, 0, 0);
    const int otherIn = graph.addNode(Float, 0, 0, .875f);
    const int sameName = graph.addNode(String, 0, 0, std::string{"Gain"});
    const int count = graph.addNode(Int, 0, 0, 2);
    const int index = graph.addNode(Int, 0, 0, 0);
    const int names = graph.addNode(String, 0, 0, std::string{"Clean,Bright"});
    const int save = graph.addNode(Trigger, 0, 0);
    const int load = graph.addNode(Trigger, 0, 0);
    const int toText = graph.addNode(PresetToText, 0, 0);
    const int fromText = graph.addNode(PresetFromText, 0, 0);
    const int input = graph.addNode(MultilineString, 0, 0);
    const int current = graph.addNode(Boolean, 0, 0);
    graph.connect({other, unrelated, 0});
    graph.connect({sameName, unrelated, 2});
    graph.connect({otherIn, unrelated, 1});
    graph.connect({count, manager, 0});
    graph.connect({index, manager, 1});
    graph.connect({names, manager, 5});
    graph.connect({manager, toggle, 0});
    graph.connect({manager, toText, 0});
    graph.connect({manager, fromText, 0});
    graph.connect({input, fromText, 1});
    graph.connect({current, fromText, 2});
    graph.connect({current, toText, 2});
    graph.connect({save, toText, 3}, LinkNotification::Silent);
    graph.connect({load, fromText, 3}, LinkNotification::Silent);

    graph.setValue(index, 1);
    child.setValue(gainIn, .75f);
    graph.fireTrigger(save);
    const auto text = std::get<std::string>(graph.readValue(toText));
    const auto table = readPresetText(text);
    const auto gainRow = std::ranges::find(table.parameters, "Gain", &PresetTable::Parameter::name);
    check(gainRow != table.parameters.end() && gainRow->values == std::vector<Value>{.25f, .75f} &&
          table.parameters.size() == 2 && graph.inspectOutput(toText, 1).triggers == 1,
          "Export reaches nested wireless parameters but excludes the other manager");

    child.setValue(gainIn, .5f);
    graph.setValue(input, text.substr(0, text.size() - 2));
    graph.fireTrigger(load);
    check(graph.inspectOutput(fromText, 2).triggers == 1 && graph.inspectOutput(fromText, 1).triggers == 0 &&
          std::get<float>(child.readValue(gain)) == .5f,
          "Malformed data after a valid row cannot partially load the bank");

    graph.setValue(input, text);
    graph.fireTrigger(load);
    check(graph.inspectOutput(fromText, 1).triggers == 1 &&
          std::get<float>(child.readValue(gain)) == .75f &&
          std::get<float>(graph.readValue(unrelated)) == .875f,
          "Loading a bank refreshes the active parameter and keeps other managers isolated");
    graph.setValue(index, 0);
    check(std::get<float>(child.readValue(gain)) == .25f, "Recalling an inactive imported program restores its gain");

    graph.setValue(current, true);
    graph.fireTrigger(save);
    check(readPresetText(std::get<std::string>(graph.readValue(toText))).programs == std::vector<std::string>{"Clean"},
          "Curr exports exactly the selected program");
    graph.setValue(index, 1);
    graph.setValue(input, writePresetText({{"Loaded", "Ignored"}, {
        {"Gain", {.625f, .125f}},
        {"Removed parameter", {std::string{"kept in the source"}, std::string{}}},
    }}));
    const auto toggleTriggers = graph.inspectOutput(toggle).triggers;
    graph.fireTrigger(load);
    check(std::get<std::string>(graph.readValue(manager, 2)) == "Loaded" &&
          std::get<float>(child.readValue(gain)) == .625f &&
          graph.inspectOutput(toggle).triggers == toggleTriggers,
          "A single program maps the first column to the current slot and leaves omitted parameters alone");
    graph.setValue(index, 0);
    check(std::get<float>(child.readValue(gain)) == .25f &&
          std::get<std::string>(graph.readValue(manager, 2)) == "Clean",
          "Current-only loading leaves the other program untouched");
}

void legacyPresetCodecReadsParameterProgramsAndBanks()
{
    using namespace openstone;
    const std::string fixture =
        std::string("CcnK\0\0\0\x34" "FxCk\0\0\0\1" "Test\0\0\0\7\0\0\0\1" "Lead", 32) +
        std::string(24, '\0') + std::string("\x3f\x20\0\0", 4);
    const auto program = readLegacyPreset(fixture);
    check(program.single && program.id == "Test" && program.version == 7 && program.programs.size() == 1 &&
          program.programs.front().name == "Lead" && program.programs.front().values == std::vector{.625f},
          "Read a standard big-endian FXP parameter record");
    check(writeLegacyPreset(program) == fixture, "Export the same parameter record byte for byte");
    auto bank = program;
    bank.single = false;
    bank.current = 1;
    bank.programs.push_back({"Bass", {.125f}});
    auto bytes = writeLegacyPreset(bank);
    check(bytes.substr(8, 4) == "FxBk" && bytes.substr(156, 4) == "CcnK", "Write a standard bank of FXP records");
    auto decoded = readLegacyPreset(bytes);
    check(!decoded.single && decoded.current == 1 && decoded.programs.size() == 2 &&
          decoded.programs[0].values == std::vector{.625f} && decoded.programs[1].values == std::vector{.125f},
          "Read each bank program and the active index");
    bytes.replace(4, 4, 4, '\0');
    bytes.replace(160, 4, 4, '\0');
    check(readLegacyPreset(bytes).programs[1].name == "Bass", "Read legacy records that leave byteSize at zero");
}

void legacyPresetPrimitivesTransferFilesAndRejectInvalidBanks()
{
    using enum PrimitiveKind;
    using namespace openstone;
    using namespace openstone::green;
    std::map<std::string, std::string, std::less<>> files;
    Schematic::TextFileBackend backend;
    backend.save = [&](std::string_view path, std::string_view bytes) { files.insert_or_assign(std::string(path), bytes); };
    backend.loadBounded = [&](std::string_view path, std::size_t limit) {
        check(limit == maxLegacyPresetBytes, "The preset loader requests a bounded read");
        const auto entry = files.find(path);
        if (entry == files.end()) throw std::runtime_error("No file");
        return entry->second;
    };
    Schematic graph({}, {}, {}, std::move(backend));
    const int manager = graph.addNode(PresetManager, 0, 0);
    const int count = graph.addNode(Int, 0, 0, 2);
    const int slot = graph.addNode(Int, 0, 0, 0);
    const int names = graph.addNode(String, 0, 0, std::string{"Clean,Drive"});
    const int path = graph.addNode(String, 0, 0, std::string{"test.fxb"});
    const int id = graph.addNode(String, 0, 0, std::string{"Pst1"});
    const int minVersion = graph.addNode(Int, 0, 0, 0);
    const int version = graph.addNode(Int, 0, 0, 7);
    const int current = graph.addNode(Boolean, 0, 0, false);
    const int gain = graph.addNode(PresetFloat, 0, 0);
    const int knob = graph.addNode(Float, 0, 0, .25f);
    const int gain2 = graph.addNode(PresetFloat, 0, 0);
    const int knob2 = graph.addNode(Float, 0, 0, .125f);
    const int firstOrder = graph.addNode(Int, 0, 0, -1);
    const int save = graph.addNode(PresetSave, 0, 0);
    const int load = graph.addNode(PresetLoad, 0, 0);
    const int saveButton = graph.addNode(Trigger, 0, 0);
    const int loadButton = graph.addNode(Trigger, 0, 0);
    for (const SchematicLink link : {
            SchematicLink{count, manager, 0}, {slot, manager, 1}, {names, manager, 5},
            {manager, gain, 0}, {knob, gain, 1}, {manager, gain2, 0}, {knob2, gain2, 1}, {firstOrder, gain2, 9},
            {manager, save, 0}, {path, save, 1}, {current, save, 3}, {id, save, 4}, {version, save, 5},
            {manager, load, 0}, {path, load, 1}, {id, load, 3}, {minVersion, load, 4}}) {
        graph.connect(link);
    }
    graph.connect({saveButton, save, 6}, LinkNotification::Silent);
    graph.connect({loadButton, load, 6}, LinkNotification::Silent);
    const auto gainValue = [&] { return std::get<float>(graph.readValue(gain)); };

    graph.setValue(slot, 1);
    graph.setValue(knob, .75f);
    graph.setValue(knob2, .875f);
    graph.fireTrigger(saveButton);
    const auto bank = files.at("test.fxb");
    check(bank.substr(8, 4) == "FxBk" && graph.inspectOutput(save, 0).triggers == 1 &&
          readLegacyPreset(bank).programs[1].values == std::vector{.875f, .75f},
          "Preset Save writes standard normalized parameter banks");
    graph.setValue(knob, .5f);
    graph.setValue(knob2, .625f);

    graph.setValue(minVersion, 8);
    graph.fireTrigger(loadButton);
    graph.setValue(minVersion, 0);
    files["test.fxb"][16] = 'X';
    graph.fireTrigger(loadButton);
    files["test.fxb"] = bank.substr(0, bank.size() - 1);
    graph.fireTrigger(loadButton);
    check(gainValue() == .5f && graph.inspectOutput(load, 2).triggers == 3 &&
          graph.inspectOutput(load, 1).triggers == 0,
          "Version mismatch, ID mismatch, and truncation leave the target bank intact");

    files["test.fxb"] = bank;
    graph.setValue(slot, 0);
    graph.setValue(knob, .125f);
    graph.fireTrigger(loadButton);
    check(gainValue() == .25f && std::get<std::string>(graph.readValue(load)) == "Clean\nDrive" &&
          integer(graph, load, 3) == 7 && integer(graph, load, 4) == 1 &&
          graph.inspectOutput(load, 1).triggers == 1,
          "Preset Load restores both programs and reports the source version and active program");
    graph.setValue(slot, 1);
    check(gainValue() == .75f && graph.readValue(gain2) == Value{.875f},
          "Recall both positional parameters even when they use the same name");

    const int secondName = graph.addNode(String, 0, 0, std::string{"Second Gain"});
    graph.connect({secondName, gain2, 2});
    const int label = graph.addNode(PresetString, 0, 0);
    const int text = graph.addNode(String, 0, 0, std::string{"Réglage\t2\nLine"});
    graph.connect({manager, label, 0});
    graph.connect({text, label, 1});
    graph.fireTrigger(loadButton);
    check(graph.readValue(label) == Value{std::string{"Réglage\t2\nLine"}},
          "A standard parameter bank leaves preset-only values untouched");
    graph.fireTrigger(saveButton);
    const auto typedBank = files.at("test.fxb");
    check(typedBank.substr(8, 4) == "FBCh", "A bank with text uses an OpenStone state chunk");
    graph.setValue(text, std::string{"Edit"});
    graph.fireTrigger(loadButton);
    check(std::get<std::string>(graph.readValue(label)) == "Réglage\t2\nLine",
          "A typed bank restores the original text");

    graph.setValue(current, true);
    graph.setValue(path, std::string{"test.fxp"});
    graph.fireTrigger(saveButton);
    check(files.at("test.fxp").substr(8, 4) == "FPCh", "Curr writes one program as an FXP file");
    graph.setValue(slot, 0);
    graph.fireTrigger(loadButton);
    check(gainValue() == .75f && std::get<std::string>(graph.readValue(label)) == "Réglage\t2\nLine" &&
          std::get<std::string>(graph.readValue(load)) == "Drive",
          "A single program loads into the selected slot even when the load Curr input is off");
}

void sfzParserPublishesAlignedRegionColumns()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    Schematic graph({}, {}, {}, {.load = [](std::string_view path) -> std::string {
        if (path == "missing.sfz") throw std::runtime_error("Could not open file");
        if (path == "bad.sfz") return "<region> lokey=invalid";
        return "\xef\xbb\xbf<control> default_path=Sounds\\Kit\n"
               "<global> volume=-3\n"
               "<group> lovel=32 loop_mode=loop_sustain\n"
               "<region> sample=Kick Close.wav key=C4\n"
               "<region> sample=Snare.wav lokey=64 hikey=72 // tail comment\n";
    }});
    const int sfz = graph.addNode(SfzParser, 0, 0);
    const int input = graph.addNode(String, 0, 0, std::string{"bank/patch.sfz"});
    const int load = graph.addNode(Trigger, 0, 0);
    const int names = graph.addNode(String, 0, 0, std::string{"sample,lokey,hikey,volume,lovel,loop_mode"});
    const int extract = graph.addNode(BusExtract, 0, 0);
    graph.connect({input, sfz, 0});
    graph.connect({load, sfz, 1});
    graph.connect({names, extract, 0});
    graph.connect({sfz, extract, 1, 1});
    graph.fireTrigger(load);
    check(integer(graph, sfz) == 0 && graph.outputType(extract, 0) == Type::StringArray &&
          graph.outputType(extract, 1) == Type::FloatArray &&
          graph.readValue(extract, 0) == Value{StringArray{"bank/Sounds/Kit/Kick Close.wav", "bank/Sounds/Kit/Snare.wav"}} &&
          graph.readValue(extract, 1) == Value{FloatArray{60, 64}} &&
          graph.readValue(extract, 2) == Value{FloatArray{60, 72}} &&
          graph.readValue(extract, 3) == Value{FloatArray{-3, -3}} &&
          graph.readValue(extract, 4) == Value{FloatArray{32, 32}} &&
          graph.readValue(extract, 5) == Value{FloatArray{3, 3}}, "SFZ keys, paths, defaults and inherited opcodes align by region");
    graph.setValue(input, std::string{"missing.sfz"});
    graph.fireTrigger(load);
    check(integer(graph, sfz) == 1 && graph.readValue(extract, 0) == Value{StringArray{}},
          "An unreadable SFZ clears the result and returns 1");
    graph.setValue(input, std::string{"bad.sfz"});
    graph.fireTrigger(load);
    check(integer(graph, sfz) == 2 && graph.readValue(extract, 1) == Value{FloatArray{}},
          "A syntax failure clears the result and returns 2");
}

class TestNetworkConnection final : public openstone::NetworkConnection {
public:
    openstone::NetworkPoll poll() override
    {
        return {.status = "Open", .address = "127.0.0.1", .received = std::exchange(received, {})};
    }

    bool send(std::string_view bytes) override
    {
        sent.emplace_back(bytes);
        return true;
    }

    std::vector<std::string> received, sent;
};

void networkNodesRouteBytesAndLifecycleTriggers()
{
    using enum PrimitiveKind;
    using openstone::green::Value;
    for (const bool server : {false, true}) {
        auto endpoint = std::make_shared<TestNetworkConnection>();
        std::optional<openstone::NetworkSettings> settings;
        Schematic graph({}, {}, {}, {}, {.openNetwork = [&](const openstone::NetworkSettings& requested) {
            settings = requested;
            return endpoint;
        }});
        const int node = graph.addNode(server ? NetworkServer : NetworkClient, 0, 0);
        const int base = server ? 2 : 3;
        const int port = graph.addNode(Int, 0, 0, 49001);
        const int protocol = graph.addNode(String, 0, 0, std::string{"UDP"});
        const int address = graph.addNode(String, 0, 0, std::string{"localhost"});
        const int data = graph.addNode(String, 0, 0, std::string{"00 41 FE"});
        const int flag = graph.addNode(Boolean, 0, 0, true);
        const int start = graph.addNode(Trigger, 0, 0);
        const int stop = graph.addNode(Trigger, 0, 0);
        const int send = graph.addNode(Trigger, 0, 0);
        const int clear = graph.addNode(Trigger, 0, 0);
        if (!server) graph.connect({address, node, 0});
        graph.connect({port, node, base - 2});
        graph.connect({protocol, node, base - 1});
        graph.connect({start, node, base});
        graph.connect({stop, node, base + 1});
        graph.connect({data, node, base + 2});
        graph.connect({flag, node, base + 3});
        graph.connect({send, node, base + 4});
        graph.connect({flag, node, base + 5});
        graph.connect({clear, node, base + 6});
        check(!settings && graph.readValue(node) == Value{std::string{"Closed"}}, "Wiring commands must leave sockets closed");
        graph.fireTrigger(start);
        check(settings && settings->server == server && settings->datagram && settings->port == 49001 &&
              settings->host == (server ? "" : "localhost") && graph.readValue(node) == Value{std::string{"Open"}},
              "The open command samples protocol, address, and port");
        if (server) check(graph.readValue(node, 1) == Value{std::string{"127.0.0.1"}}, "The server reports its address");

        graph.fireTrigger(send);
        check(endpoint->sent == std::vector<std::string>{std::string{"\0A\xfe", 3}} &&
              graph.inspectOutput(node, server ? 3 : 2).triggers == 1,
              "The send trigger transmits decoded bytes and reports acceptance");
        endpoint->received = {std::string{"\0\xab", 2}, std::string{"\0\xab", 2}};
        const int received = server ? 2 : 1;
        check(graph.pollNetworkConnections() && graph.inspectOutput(node, received).triggers == 2 &&
              openstone::decodeHexBytes(std::get<std::string>(graph.readValue(node, received))) == std::string{"\0\xab", 2},
              "Polling publishes each packet with hex data, including repeated packets");
        graph.fireTrigger(clear);
        check(graph.readValue(node, server ? 4 : 3) == Value{std::string{}}, "Clear Log empties the activity log");
        graph.fireTrigger(stop);
        check(!graph.hasOpenNetworkConnections() && graph.readValue(node) == Value{std::string{"Closed"}},
              "Stopping closes the endpoint and polling");
    }
}

class TestMediaPlayback final : public openstone::MediaPlayback {
public:
    void command(openstone::MediaCommand command, const openstone::MediaOptions& options) override
    {
        setOptions(options);
        if (command == openstone::MediaCommand::PausePlay) update.state = update.state == 1 ? 2 : 1;
        if (command == openstone::MediaCommand::Grab && options.manual) {
            update.frame = openstone::yellow::Bitmap(1, 1, {123, 45, 6, 255});
        }
    }

    void setOptions(const openstone::MediaOptions& options) override { current = options; }

    openstone::MediaPoll poll() override
    {
        auto result = update;
        update.frame = {};
        return result;
    }

    openstone::MediaOptions current;
    openstone::MediaPoll update{.state = 1, .position = 12.F};
};

void streamSequencerRecordsAndReplaysMono4AcrossBlocks()
{
    using enum PrimitiveKind;
    using enum openstone::stream::SequenceMode;
    using openstone::green::Type;
    for (const std::size_t block : {1, 3, 7}) {
        Schematic graph;
        graph.setSampleRate(1000);
        const int input = graph.addNode(AudioIn, 0, 0);
        const int output = graph.addNode(AudioOut, 0, 0);
        const int pack = graph.addNode(Pack, 0, 0);
        const int unpack = graph.addNode(Unpack, 0, 0);
        const int track = graph.addNode(StreamSequencerTrack, 0, 0);
        graph.connect({input, pack, 0, 0});
        graph.connect({input, pack, 3, 1});
        graph.connect({pack, track, 0});
        graph.connect({track, unpack, 0});
        graph.connect({unpack, output, 0, 0});
        graph.connect({unpack, output, 1, 3});
        check(graph.inputType(track, 0) == Type::Mono4 && graph.outputType(track) == Type::Mono4,
              "A Stream Sequencer Track uses one four-lane inlet and outlet");
        auto patch = graph.prepareAudio(block);
        graph.setLiveAudio(&patch);
        graph.setSequencerMode(Recording, {});
        const std::array left{1.F, 2.F, 3.F, 4.F, 5.F, 6.F, 7.F};
        const std::array right{-7.F, -6.F, -5.F, -4.F, -3.F, -2.F, -1.F};
        std::array<float, 7> first{}, fourth{};
        check(patch.processor().process({left, right}, {first, fourth}) && first == left && fourth == right,
              "A recording monitors the incoming stream without mixing the lanes");
        graph.setSequencerMode(Stopped, {});
        graph.setSequencerMode(Playing, {});
        check(patch.processor().process({}, {first, fourth}) && first == left && fourth == right,
              "Playback restarts at the first recorded frame and preserves the lanes");
        check(graph.advanceSequencer({}) && graph.sequencerMode() == Stopped,
              "The audio clock stops the transport when the take finishes");
    }
}

void mediaPrimitivesPublishFramesAndStereoWhileAudioIsPrepared()
{
    using enum PrimitiveKind;
    using openstone::green::Value;
    auto playback = std::make_shared<TestMediaPlayback>();
    std::optional<openstone::MediaSettings> settings;
    Schematic graph({}, {}, {}, {}, {.openMedia = [&](const openstone::MediaSettings& opened) {
        settings = opened;
        if (opened.samples) opened.samples->setPlaying(true);
        return playback;
    }});
    const int source = graph.addNode(String, 0, 0, std::string{"asset.wav"});
    const int start = graph.addNode(Trigger, 0, 0);
    const int stop = graph.addNode(Trigger, 0, 0);
    const int grab = graph.addNode(Trigger, 0, 0);
    const int audio = graph.addNode(AudioStreamIn, 0, 0);
    const int video = graph.addNode(VideoStreamIn, 0, 0);
    const int output = graph.addNode(AudioOut, 0, 0);
    const int manual = graph.addNode(Boolean, 0, 0, true);
    graph.connect({source, audio, 0});
    graph.connect({start, audio, 1});
    graph.connect({stop, audio, 3});
    graph.connect({audio, output, 0, 0});
    graph.connect({audio, output, 1, 1});
    auto patch = graph.prepareAudio(2);
    graph.fireTrigger(start);
    check(settings && settings->samples && settings->source == "asset.wav" &&
          graph.pollMedia() && graph.readValue(audio, 2) == Value{std::string{"1"}},
          "Audio playback can start after the Stream graph is prepared");
    const std::array<openstone::MediaAudioBuffer::Frame, 3> pcm{{{.1F, .2F}, {.3F, .4F}, {.5F, .6F}}};
    settings->samples->push(pcm);
    std::array<float, 4> left{}, right{};
    check(graph.refreshAudioControls(patch) && patch.processor().process({}, {left, right}) &&
          left == std::array{.1F, .3F, .5F, 0.F} && right == std::array{.2F, .4F, .6F, 0.F},
          "Both outputs share the same PCM frame and underruns are silent");
    settings->samples->push(pcm);
    graph.fireTrigger(stop);
    check(patch.processor().process({}, {left, right}) && left == std::array<float, 4>{} &&
          graph.readValue(audio, 2) == Value{std::string{"0"}}, "Stop clears audio without replacing the patch");

    const int startVideo = graph.addNode(Trigger, 0, 0);
    graph.connect({source, video, 0});
    graph.connect({startVideo, video, 3});
    graph.connect({manual, video, 7});
    graph.connect({grab, video, 8});
    graph.setValue(source, std::string{"asset.mp4"});
    graph.fireTrigger(startVideo);
    check(settings && !settings->samples && settings->video.manual && settings->video.width == 640 &&
          settings->video.height == 480 && std::get<openstone::yellow::Bitmap>(graph.readValue(video)).empty(),
          "Manual video playback waits for Grab at the default frame size");
    graph.fireTrigger(grab);
    check(std::get<openstone::yellow::Bitmap>(graph.readValue(video)).pixelArgb(0, 0) == 0xff7b2d06 &&
          integer(graph, video, 1) == 1 && graph.readValue(video, 2) == Value{12.F},
          "Grab publishes a bitmap with position and play state");
}

void mouseCaptureDeliversOutsideEventsUntilStopped()
{
    using enum PrimitiveKind;
    using openstone::yellow::MouseAreaCommand;
    using openstone::yellow::MouseButton;
    using openstone::yellow::MouseEventKind;
    Schematic root;
    auto& graph = *root.module(root.addNode(Module, 0, 0));
    const int panel = graph.addNode(ModuleGui, 0, 0);
    const int region = graph.addNode(String, 0, 0, std::string{"0,0,3,3"});
    const int mouse = graph.addNode(MouseArea, 0, 0);
    const int capture = graph.addNode(MouseCapture, 0, 0);
    const int start = graph.addNode(Trigger, 0, 0);
    const int stop = graph.addNode(Trigger, 0, 0);
    const int press = graph.addNode(MouseLDown, 0, 0);
    graph.connect({panel, mouse, 0});
    graph.connect({region, mouse, 1});
    graph.connect({mouse, capture, 0});
    graph.connect({start, capture, 1});
    graph.connect({stop, capture, 2});
    graph.connect({capture, press, 0});
    const auto outside = openstone::yellow::MouseMessage{MouseEventKind::ButtonDown, {7, 7}, MouseButton::Left};
    check(!graph.sendMouseMessage(mouse, outside), "Outside presses normally miss the Mouse Area");
    graph.fireTrigger(start);
    const auto drawings = graph.panelDrawings(graph.frontPanel()->view);
    check(std::get<MouseAreaCommand>(drawings.at(0).command).captured && graph.sendMouseMessage(mouse, outside) &&
          graph.inspectOutput(press).triggers == 1, "Starting capture expands the hit region and forwards the press");
    graph.fireTrigger(stop);
    check(!graph.sendMouseMessage(mouse, outside), "Stopping restores the hit region");
    graph.fireTrigger(start);
    graph.removeNode(capture);
    check(!graph.sendMouseMessage(mouse, outside), "Removing a capture node drops its claim on the pointer");
}

void httpRequestsPublishOnlyTheCurrentReply()
{
    using enum PrimitiveKind;
    using openstone::green::Value;
    using openstone::HttpRequest;
    using openstone::HttpResponseKind;
    std::vector<HttpRequest> sent;
    Schematic graph({}, {}, {}, {}, {.sendHttp = [&](HttpRequest request) { sent.push_back(std::move(request)); }});
    auto& child = *graph.module(graph.addNode(Module, 0, 0));
    const int url = child.addNode(String, 0, 0, std::string{"https://example.test/content"});
    const int names = child.addNode(MultilineString, 0, 0, std::string{"name\ncount"});
    const int values = child.addNode(MultilineString, 0, 0, std::string{"a & b\n2"});
    const int trigger = child.addNode(Trigger, 0, 0);
    const int http = child.addNode(HttpTextRequest, 0, 0);
    child.connect({url, http, 0});
    child.connect({names, http, 1});
    child.connect({values, http, 2});
    child.connect({trigger, http, 3});
    check(sent.empty(), "Wiring the HTTP inputs does not send a request");
    child.fireTrigger(trigger);
    check(sent.size() == 1 && sent.back().kind == HttpResponseKind::Text &&
          sent.back().url == "https://example.test/content" &&
          sent.back().names == openstone::green::StringArray{"name", "count"} &&
          sent.back().values == openstone::green::StringArray{"a & b", "2"},
          "Send snapshots the URL and paired request parameters");
    child.fireTrigger(trigger);
    check(sent.size() == 2 && !graph.hasPendingHttpRequest(sent[0]) &&
          graph.hasPendingHttpRequest(sent[1]) && !graph.finishHttpRequest(sent[0], {.text = "stale"}),
          "A stale reply is discarded");
    check(graph.finishHttpRequest(sent[1], {.text = "a reply"}) &&
          child.readValue(http) == Value{std::string{"a reply"}} &&
          child.readValue(http, 1) == Value{true} && child.readValue(http, 2) == Value{std::string{}},
          "A current text reply publishes Response, Ok and Error together");
    child.setValue(values, std::string{"only one"});
    child.fireTrigger(trigger);
    check(sent.size() == 2 && child.readValue(http, 1) == Value{false} &&
          child.readValue(http) == Value{std::string{"a reply"}} &&
          child.inspectOutput(http).triggers == 1 &&
          !std::get<std::string>(child.readValue(http, 2)).empty(), "Uneven parameter lists report failure");

    const int image = child.addNode(HttpImageRequest, 0, 0);
    child.connect({url, image, 0});
    child.connect({trigger, image, 3});
    child.fireTrigger(trigger);
    const auto imageRequest = sent.back();
    check(imageRequest.kind == HttpResponseKind::Image &&
          graph.finishHttpRequest(imageRequest, {.image = openstone::yellow::Bitmap{1, 1, {128, 64, 32, 255}}}) &&
          std::get<openstone::yellow::Bitmap>(child.readValue(image)).width() == 1,
          "An image reply publishes a Bitmap");

    const int file = child.addNode(HttpFileRequest, 0, 0);
    const int destination = child.addNode(String, 0, 0, std::string{"out.bin"});
    const int override = child.addNode(Boolean, 0, 0, true);
    child.connect({url, file, 0});
    child.connect({destination, file, 3});
    child.connect({override, file, 4});
    child.connect({trigger, file, 5});
    child.fireTrigger(trigger);
    const auto fileRequest = sent.back();
    check(fileRequest.kind == HttpResponseKind::File && fileRequest.destination == "out.bin" && fileRequest.overwrite,
          "A file request snapshots its destination and override input");
    check(graph.finishHttpRequest(fileRequest, {.error = "permission denied"}) &&
          child.readValue(file) == Value{false} &&
          child.readValue(file, 1) == Value{std::string{"permission denied"}}, "File errors publish on Ok and Error");
    child.fireTrigger(trigger);
    const auto removedRequest = sent.back();
    child.removeNode(file);
    check(!graph.finishHttpRequest(removedRequest, {}), "A deleted primitive cannot receive an old reply");
}

void remainingSystemActionsUseTheCurrentInputsOnTrigger()
{
    using enum PrimitiveKind;
    using openstone::green::Value;
    std::string opened;
    std::vector<std::tuple<std::string, std::string, bool>> launches;
    int quits = 0;
    Schematic graph({}, {}, {}, {}, {
        .openUrl = [&](std::string_view value) { opened = value; return true; },
        .launchProgram = [&](std::string_view path, std::string_view args, bool background) {
            launches.emplace_back(path, args, background);
            return true;
        },
        .macAddresses = [] { return openstone::green::StringArray{"00:11:22:33:44:55", "", "AA:BB:CC:DD:EE:FF"}; },
        .diskSerial = [] { return std::string{"fixture-serial"}; },
        .quitApplication = [&] { ++quits; },
    });
    const int pulse = graph.addNode(Trigger, 0, 0);
    const int url = graph.addNode(String, 0, 0, std::string{"https://example.test/"});
    const int web = graph.addNode(WebUrl, 0, 0);
    const int path = graph.addNode(String, 0, 0, std::string{"a program"});
    const int args = graph.addNode(String, 0, 0, std::string{"--name \"two words\""});
    const int background = graph.addNode(Boolean, 0, 0, true);
    const int shell = graph.addNode(ShellExecute, 0, 0);
    const int mac = graph.addNode(MacAddresses, 0, 0);
    const int disk = graph.addNode(HardDiskSerial, 0, 0);
    const int quit = graph.addNode(ExeQuit, 0, 0);
    const int counter = graph.addNode(BasicCounter, 0, 0);
    const int reset = graph.addNode(Trigger, 0, 0);
    graph.connect({url, web, 0});
    graph.connect({pulse, web, 1});
    graph.connect({path, shell, 0});
    graph.connect({args, shell, 1});
    graph.connect({pulse, shell, 2});
    graph.connect({background, shell, 3});
    graph.connect({pulse, mac, 0});
    graph.connect({pulse, disk, 0});
    graph.connect({pulse, quit, 0});
    graph.connect({pulse, counter, 0});
    graph.connect({reset, counter, 1});
    check(opened.empty() && launches.empty() && quits == 0 && integer(graph, counter) == 0,
          "Wiring and loading command arguments leave system actions idle");
    graph.fireTrigger(pulse);
    check(opened == "https://example.test/" && quits == 1, "A pulse opens the URL and requests host shutdown");
    check(launches == std::vector{std::tuple{std::string{"a program"}, std::string{"--name \"two words\""}, true}} &&
          graph.inspectOutput(shell).triggers == 1, "Shell Execute passes the program, arguments and background flag");
    check(graph.readValue(mac) == Value{std::string{"00:11:22:33:44:55,AA:BB:CC:DD:EE:FF"}},
          "MAC Addresses publishes the adapters as a comma-separated String");
    check(graph.readValue(disk) == Value{std::string{"fixture-serial"}} && graph.inspectOutput(disk).triggers == 1,
          "A Get pulse reads and publishes the disk serial supplied by the host");
    graph.setValue(background, false);
    graph.fireTrigger(pulse);
    check(launches.size() == 2 && !std::get<2>(launches.back()) && integer(graph, counter) == 2,
          "A second pulse reads current settings and increments the counter again");
    graph.fireTrigger(reset);
    check(integer(graph, counter) == 0 && graph.inspectOutput(counter).triggers == 3,
          "Counter Reset returns to zero and publishes");
}

void fontEmbedRestoresBytesInCopiedModules()
{
    using enum PrimitiveKind;
    using openstone::green::Value;
    std::vector<std::vector<std::string>> registered;
    int reads = 0;
    const std::string fontBytes{"\0fixture", 8};
    openstone::Session session({}, {}, {},
        {.loadBounded = [&](std::string_view path, std::size_t) {
            ++reads;
            if (path != "fixture.ttf") throw std::runtime_error("No such font");
            return fontBytes;
        }},
        {.registerFonts = [&](const auto& bytes) {
            registered.push_back(bytes);
            return std::make_shared<openstone::FontRegistration>();
        }});
    auto& root = session.graph();
    const int moduleId = root.addNode(Module, 0, 0);
    auto& child = *root.module(moduleId);
    const int embed = child.addNode(FontEmbed, 0, 0);
    const int load = child.addNode(Trigger, 0, 0);
    const int files = child.addNode(MultilineString, 0, 0, std::string{"fixture.ttf"});
    child.connect({files, embed, 0});
    child.connect({load, embed, 1});
    check(reads == 0 && child.readValue(embed) == Value{false}, "Wiring paths does not load fonts");
    child.fireTrigger(load);
    check(registered == std::vector<std::vector<std::string>>{{fontBytes}} && child.readValue(embed) == Value{true},
          "Load passes the binary font data unchanged to the host");
    child.setValue(files, std::string{"missing.ttf"});
    child.fireTrigger(load);
    check(child.readValue(embed) == Value{false} && registered.size() == 1, "A missing font reports failure");

    session.copyNode(moduleId);
    const int copy = session.pasteNode(0, 0, false);
    check(reads == 2 && registered == std::vector<std::vector<std::string>>{{fontBytes}, {fontBytes}} &&
          root.module(copy)->readValue(embed) == Value{true},
          "A restored module registers the embedded bytes without reading the original path");
}

void remainingDrawingPrimitivesPublishTypedResults()
{
    using enum PrimitiveKind;
    using openstone::green::Value;
    using ColorValue = openstone::yellow::Color;
    Schematic graph;
    const int chooser = graph.addNode(ColorDialog, 0, 0);
    const int alpha = graph.addNode(Int, 0, 0, std::int32_t{96});
    const int color = graph.addNode(PrimitiveKind::Color, 0, 0, ColorValue{0xff102030});
    const int open = graph.addNode(Trigger, 0, 0);
    graph.connect({alpha, chooser, 1});
    graph.connect({color, chooser, 2});
    graph.connect({open, chooser, 0});
    graph.fireTrigger(open);
    auto request = graph.pendingAppDialog();
    check(request && request->initialColor == ColorValue{0xff102030} && request->alpha == 96,
          "Color Dialog snapshots the starting color and alpha");
    const auto token = request->token;
    graph.setValue(alpha, std::int32_t{255});
    check(graph.finishAppDialog(token, true, {}, ColorValue{0x11998877}) &&
          graph.readValue(chooser) == Value{ColorValue{0x60998877}} &&
          graph.inspectOutput(chooser, 1).triggers == 1, "OK publishes the chosen RGB with the requested alpha");
    graph.fireTrigger(open);
    check(graph.finishAppDialog(graph.pendingAppDialog()->token, false) &&
          graph.readValue(chooser) == Value{ColorValue{0x60998877}} &&
          graph.inspectOutput(chooser, 2).triggers == 1, "Cancel keeps the last accepted color");

    const int pen = graph.addNode(Pen, 0, 0);
    const int penInfo = graph.addNode(PenAttributes, 0, 0);
    const int width = graph.addNode(Float, 0, 0, 0.5F);
    const int style = graph.addNode(String, 0, 0, std::string{"dashdot"});
    graph.connect({color, pen, 0});
    graph.connect({width, pen, 1});
    graph.connect({style, pen, 2});
    graph.connect({pen, penInfo, 0});
    check(graph.readValue(penInfo) == Value{ColorValue{0xff102030}} && graph.readValue(penInfo, 1) == Value{0.5F} &&
          graph.readValue(penInfo, 2) == Value{std::string{"dashdot"}}, "Pen Attributes reports the stroke parameters");

    const int matrix = graph.addNode(PrimitiveKind::ColorMatrix, 0, 0);
    const int matrixText = graph.addNode(MultilineString, 0, 0, std::string{
        "0.5,0,0,0,0,\n0,1,0,0,0,\n0,0,1,0,0,\n0,0,0,1,0,\n0,0,0,0,1"});
    const int parse = graph.addNode(Trigger, 0, 0);
    const int apply = graph.addNode(ApplyColorMatrix, 0, 0);
    graph.connect({matrixText, matrix, 0});
    graph.connect({parse, matrix, 1});
    graph.connect({color, apply, 0});
    graph.connect({matrix, apply, 1});
    check(graph.readValue(matrix, 1) == Value{false}, "Editing the input doesn't parse the matrix");
    graph.fireTrigger(parse);
    check(graph.readValue(matrix, 1) == Value{true} && graph.readValue(apply) == Value{ColorValue{0xff082030}},
          "A 5×5 text matrix can be applied to a color");
    graph.setValue(matrixText, std::string{"1,2,3"});
    graph.fireTrigger(parse);
    check(graph.readValue(matrix, 1) == Value{false} && graph.readValue(apply) == Value{ColorValue{0xff082030}} &&
          graph.inspectOutput(matrix).triggers == 1, "Invalid data leaves the last matrix in place");
}

void stateMachineFollowsTheMapAndResets()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    Schematic graph;
    const int machine = graph.addNode(StateMachine, 0, 0);
    const int control = graph.addNode(Int, 0, 0, std::int32_t{99});
    const int map = graph.addNode(MultilineString, 0, 0, std::string{
        "s10;\r\n +7 -> 20;\n"
        "s20; 7 -> -3;\n"
        "s-3; -4 -> 10;\n"});
    const int reset = graph.addNode(Trigger, 0, 0);
    const int repeatControl = graph.addNode(Trigger, 0, 0);
    graph.connect({control, machine, 0});
    graph.connect({map, machine, 1});
    graph.connect({reset, machine, 2});
    graph.connect({repeatControl, machine, 0});

    const auto state = [&] { return integer(graph, machine); };
    const auto valid = [&] { return std::get<bool>(graph.readValue(machine, 1)); };
    const auto pulses = [&] { return graph.inspectOutput(machine).triggers; };
    const auto ports = findNode(graph, machine).inputPorts();
    check(ports.size() == 3 && ports[0].name == "Ctrl" && ports[1].name == "Map" && ports[2].name == "Reset" &&
          graph.inputType(machine, 0) == Type::Int && graph.inputType(machine, 1) == Type::String &&
          graph.inputType(machine, 2) == Type::Trigger && graph.outputType(machine, 0) == Type::Int &&
          graph.outputType(machine, 1) == Type::Boolean &&
          findNode(graph, machine).outputPorts().size() == 2,
          "State Machine exposes the Alpha connectors in order");
    check(valid() && state() == 0 && pulses() == 0 && graph.inspectOutput(machine, 1).triggers == 1,
          "A map initializes routing and publishes syntax status");
    graph.fireTrigger(reset);
    check(state() == 10 && pulses() == 1, "Reset reports the first declared state");
    graph.setValue(control, 0);
    check(state() == 10 && pulses() == 1, "An unmatched control leaves the state and output pulse count alone");
    graph.setValue(control, 7);
    check(state() == 20 && pulses() == 2, "A matched control reports its target");
    graph.fireTrigger(repeatControl);
    check(state() == -3 && pulses() == 3, "A repeated control is dispatched in the new state");
    graph.setValue(control, -4);
    check(state() == 10 && pulses() == 4, "Signed states and controls work");
    graph.fireTrigger(reset);
    check(state() == 10 && pulses() == 5, "Reset publishes even when the first state is already active");

    graph.setValue(map, std::string{"s42; 8 -> 5; s5;"});
    check(valid() && state() == 10 && pulses() == 5, "A replacement map restarts routing and only pulses syntax status");
    graph.setValue(control, 8);
    check(state() == 5 && pulses() == 6, "Ctrl uses the replacement map");
    graph.setValue(map, std::string{"s6; 9 -> 7; s7; 0 -> ;"});
    check(!valid() && graph.inspectOutput(machine, 1).triggers == 3 && pulses() == 6,
          "An incomplete transition reports invalid map syntax");
    graph.setValue(control, 9);
    check(state() == 7 && pulses() == 7, "Complete statements before the syntax error still run");
    graph.setValue(map, std::string{"s2147483648;"});
    check(!valid(), "A state number must fit in a signed Int");
    graph.setValue(map, std::string{" \r\n "});
    check(valid(), "An empty map parses and has no transitions");
    graph.setValue(control, 8);
    check(state() == 7 && pulses() == 7, "Removing all states prevents stale transitions");
}

void stateMachinePrimesItsMapWhenOpeningASchematic()
{
    using enum PrimitiveKind;
    openstone::Session session;
    auto project = std::make_unique<Schematic>();
    auto& child = *project->module(project->addNode(Module, 0, 0));
    const int machine = child.addNode(StateMachine, 0, 0);
    const int map = child.addNode(String, 0, 0, std::string{"s13; 6 -> 17; s17;"});
    const int control = child.addNode(Int, 0, 0, std::int32_t{0});
    const int start = child.addNode(AfterLoad, 0, 0);
    child.connect({control, machine, 0}, LinkNotification::Silent);
    child.connect({map, machine, 1}, LinkNotification::Silent);
    child.connect({start, machine, 2}, LinkNotification::Silent);
    check(session.openSchematic(std::move(project)), "Open a nested machine with its original map");
    check(std::get<bool>(child.readValue(machine, 1)) && integer(child, machine) == 13 &&
          child.inspectOutput(machine, 1).triggers == 0 && child.inspectOutput(machine).triggers == 1,
          "The map is available to the first After Load reset without an extra syntax pulse");
    child.setValue(control, 6);
    check(integer(child, machine) == 17, "A machine restored from a schematic can transition");
    child.disconnectInput(machine, 1);
    child.setValue(control, 0);
    child.setValue(control, 6);
    check(integer(child, machine) == 17 && child.inspectOutput(machine).triggers == 2,
          "Disconnecting Map disables its transitions");
}

void settingsReportsTheSelectedSystemValue()
{
    using enum PrimitiveKind;
    using openstone::SystemSetting;
    openstone::Session session;
    auto& root = session.graph();
    auto& graph = *root.module(root.addNode(Module, 0, 0));
    const int zoom = root.addNode(Settings, 0, 0);
    const int query = graph.addNode(Settings, 0, 0);
    const auto value = [&] { return std::get<float>(graph.readValue(query)); };
    const auto pulses = [&] { return graph.inspectOutput(query).triggers; };

    check(findNode(graph, query).inputPorts().empty() && findNode(graph, query).outputPorts().size() == 1 &&
          findNode(graph, query).outputPorts().front().name.empty() &&
          graph.outputType(query) == openstone::green::Type::Float && value() == 8.0F,
          "Settings exposes the reference Float connector and default zoom");
    graph.setSystemSetting(query, static_cast<int>(SystemSetting::SampleRate));
    check(value() == 44100.0F && pulses() == 1, "The selector publishes the current sample rate");
    root.setSampleRate(96000.0);
    check(value() == 96000.0F && pulses() == 2 && root.inspectOutput(zoom).triggers == 0,
          "Sample rate changes update nested rate queries without pulsing zoom");

    graph.setSystemSetting(query, static_cast<int>(SystemSetting::AsioPreferredFrames));
    check(value() == 0, "ASIO sizes default to zero without a device");
    root.setAsioBufferSizes({.minimum = 64, .maximum = 2048, .preferred = 256, .granularity = -1});
    check(value() == 256 && pulses() == 4, "An ASIO device supplies its preferred size");
    root.setAsioBufferSizes({.minimum = 32, .maximum = 1024, .preferred = 256, .granularity = -1});
    check(pulses() == 4, "Only a change to the selected value pulses the output");
    graph.setSystemSetting(query, static_cast<int>(SystemSetting::AsioMinimumFrames));
    check(value() == 32, "Settings reads the minimum buffer size");
    graph.setSystemSetting(query, static_cast<int>(SystemSetting::AsioMaximumFrames));
    check(value() == 1024, "Settings reads the maximum buffer size");
    graph.setSystemSetting(query, static_cast<int>(SystemSetting::AsioGranularity));
    check(value() == -1, "Settings preserves ASIO's power-of-two granularity marker");
    root.setAsioBufferSizes({});
    check(value() == 0 && pulses() == 8, "Removing the ASIO device clears the queried sizes");
    root.setSystemSetting(zoom, static_cast<int>(SystemSetting::SampleRate));
    session.copyNode(zoom);
    const int copy = session.pasteNode(0, 0, false);
    check(std::get<float>(root.readValue(copy)) == 96000.0F, "A copy retains the authored selector");
}

void startFolderUsesTheInstalledDirectoryAndEditorFallbacks()
{
    using enum PrimitiveKind;
    using openstone::PluginHostInfo;
    using openstone::green::Type;
    Schematic root;
    auto& module = *root.module(root.addNode(Module, 0, 0));
    auto& nested = *module.module(module.addNode(Module, 0, 0));
    const int projectConfig = root.addNode(VstPluginInfo, 0, 0);
    const int projectPath = root.addNode(String, 0, 0, std::string{"/work/Exports"});
    const int config = module.addNode(VstPluginInfo, 0, 0);
    const int modulePath = module.addNode(String, 0, 0, std::string{"/work/Instrument Exports"});
    const int start = nested.addNode(StartFolder, 0, 0);
    const int test = nested.addNode(String, 0, 0, std::string{"/work/Test Assets"});
    const auto folder = [&] { return std::get<std::string>(nested.readValue(start)); };
    const auto pulses = [&] { return nested.inspectOutput(start).triggers; };
    check(findNode(nested, start).inputPorts().size() == 1 && findNode(nested, start).outputPorts().size() == 1 &&
          nested.inputType(start, 0) == Type::String && nested.outputType(start) == Type::String &&
          findNode(nested, start).inputPorts().front().name == "Test" &&
          findNode(nested, start).outputPorts().front().name == "Folder",
          "Start Folder exposes the FlowStone String Test input and String Folder output");
    check(folder().empty(), "The editor has no fallback until an export folder is configured");

    root.connect({projectPath, projectConfig, 11});
    check(folder() == "/work/Exports" && pulses() == 1, "An unconnected Test uses the project export folder");
    module.connect({modulePath, config, 11});
    check(folder() == "/work/Instrument Exports" && pulses() == 2,
          "A nested primitive uses the closest containing export configuration");
    root.setValue(projectPath, std::string{"/work/Updated Exports"});
    check(folder() == "/work/Instrument Exports" && pulses() == 2, "An outer edit does not pulse a shadowed fallback");
    module.setValue(modulePath, std::string{});
    check(folder() == "/work/Updated Exports" && pulses() == 3, "Clearing a local export folder inherits the outer folder");

    nested.connect({test, start, 0});
    check(folder() == "/work/Test Assets" && pulses() == 4, "In the editor, a connected Test overrides the export folder");
    nested.setValue(test, std::string{});
    check(folder().empty(), "An explicitly wired empty Test String is returned as-is");
    nested.disconnectInput(start, 0);
    check(folder() == "/work/Updated Exports", "Disconnecting Test restores the export fallback");
    nested.setValue(test, std::string{"/work/Test Assets"});
    nested.connect({test, start, 0});

    const auto beforeHost = pulses();
    PluginHostInfo host{"Vendor", "Host", "1", "/Library/Audio/Plug-Ins/VST3/Éffects"};
    root.setPluginHostInfo(host);
    check(folder() == host.pluginFolder && pulses() == beforeHost + 1,
          "The installed plug-in folder takes precedence over Test and export settings");
    host.version = "2";
    root.setPluginHostInfo(host);
    check(pulses() == beforeHost + 1, "Unrelated host metadata does not pulse Start Folder");
    nested.setValue(test, std::string{"/work/New Test Assets"});
    check(folder() == host.pluginFolder, "Editing Test while hosted leaves the installed folder selected");
    host.pluginFolder = "/another/plug-in folder";
    const auto beforeMove = pulses();
    nested.setPluginHostInfo(host);
    check(folder() == host.pluginFolder && pulses() == beforeMove + 1,
          "The host can publish a changed directory from any level");
    host.pluginFolder.clear();
    root.setPluginHostInfo(host);
    check(folder().empty(), "An attached host with no folder does not expose editor paths");
    root.setPluginHostInfo(std::nullopt);
    check(folder() == "/work/New Test Assets", "Detaching the host restores the editor's Test value");
}

void greenTransportPublishesChangedHostValues()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    using openstone::green::Value;
    using openstone::stream::HostTransport;
    Schematic root;
    auto& panel = *root.module(root.addNode(Module, 0, 0));
    const int playing = panel.addNode(IsPlaying, 0, 0);
    const int offline = panel.addNode(OfflineMode, 0, 0);
    const int tempo = panel.addNode(Tempo, 0, 0);
    const int signature = panel.addNode(TimeSignature, 0, 0);
    const int stopped = panel.addNode(TriggerCounter, 0, 0);
    const int tempoOnPlay = panel.addNode(SampleAndHold, 0, 0);
    const int meterOnPlay = panel.addNode(SampleAndHold, 0, 0);
    panel.connect({playing, stopped, 0, 1});
    panel.connect({tempo, tempoOnPlay, 0});
    panel.connect({playing, tempoOnPlay, 1}, LinkNotification::Silent);
    panel.connect({signature, meterOnPlay, 0, 1});
    panel.connect({playing, meterOnPlay, 1}, LinkNotification::Silent);

    const auto pulses = [&] {
        return std::array{panel.inspectOutput(playing, 0).triggers, panel.inspectOutput(playing, 1).triggers,
                          panel.inspectOutput(offline).triggers, panel.inspectOutput(tempo).triggers,
                          panel.inspectOutput(signature, 0).triggers, panel.inspectOutput(signature, 1).triggers};
    };
    const auto expectPulses = [&](std::array<std::uint64_t, 6> expected, const char* message) {
        check(pulses() == expected, message);
    };
    check(findNode(panel, playing).inputPorts().empty() && findNode(panel, offline).inputPorts().empty() &&
          findNode(panel, tempo).inputPorts().empty() && findNode(panel, signature).inputPorts().empty() &&
          panel.outputType(playing, 0) == Type::Boolean && panel.outputType(playing, 1) == Type::Trigger &&
          panel.outputType(offline) == Type::Boolean && panel.outputType(tempo) == Type::Float &&
          panel.outputType(signature, 0) == Type::Float && panel.outputType(signature, 1) == Type::Float,
          "The Green host primitives expose the Alpha transport connector types");
    check(panel.readValue(playing) == Value{false} && panel.readValue(offline) == Value{false} &&
          panel.readValue(tempo) == Value{120.F} && panel.readValue(signature, 0) == Value{4.F} &&
          panel.readValue(signature, 1) == Value{4.F} && panel.readValue(stopped) == Value{0},
          "Standalone defaults are stopped, realtime, 120 BPM and 4/4; connecting Stop sends no pulse");

    HostTransport update{.tempo = 137.25, .numerator = 7, .denominator = 8, .playing = true, .offline = true};
    panel.setPluginHostTransport(update);
    check(panel.readValue(playing) == Value{true} && panel.readValue(offline) == Value{true} &&
          panel.readValue(tempo) == Value{137.25F} && panel.readValue(signature, 0) == Value{7.F} &&
          panel.readValue(signature, 1) == Value{8.F}, "A host update reaches nested primitives");
    check(panel.readValue(tempoOnPlay) == Value{137.25F} && panel.readValue(meterOnPlay) == Value{8.F},
          "Playback listeners read the new tempo and meter during the same update");
    expectPulses({1, 0, 1, 1, 1, 1}, "Starting emits one pulse for each changed value");

    update.samplePosition = 2048;
    update.quarterPosition = 4;
    update.tempo += 1e-7;
    root.setPluginHostTransport(update);
    expectPulses({1, 0, 1, 1, 1, 1}, "Position and tempo changes below Float precision do not resend transport values");

    update.numerator = 5;
    root.setPluginHostTransport(update);
    check(panel.readValue(signature, 0) == Value{5.F} && panel.readValue(signature, 1) == Value{8.F},
          "A numerator-only update preserves the denominator");
    expectPulses({1, 0, 1, 1, 2, 1}, "Only the numerator outlet pulses");

    update.playing = false;
    root.setPluginHostTransport(update);
    root.setPluginHostTransport(update);
    check(panel.readValue(playing) == Value{false} && panel.readValue(offline) == Value{true} &&
          panel.readValue(stopped) == Value{1}, "Stopping fires Stop once while offline mode remains active");
    expectPulses({2, 1, 1, 1, 2, 1}, "An identical stopped update does not replay Stop");

    update.offline = false;
    update.tempo = 90.5;
    update.denominator = 16;
    root.setPluginHostTransport(update);
    check(panel.readValue(offline) == Value{false} && panel.readValue(tempo) == Value{90.5F} &&
          panel.readValue(signature, 1) == Value{16.F}, "A stopped host can change render mode, tempo and denominator");
    expectPulses({2, 1, 2, 2, 2, 2}, "Only the three changed outlets pulse");

    root.setPluginHostTransport({.tempo = std::numeric_limits<double>::quiet_NaN(), .numerator = 0, .denominator = -1});
    check(panel.readValue(tempo) == Value{120.F} && panel.readValue(signature, 0) == Value{4.F} &&
          panel.readValue(signature, 1) == Value{4.F}, "Invalid timing fields use standalone defaults");
    root.setPluginHostTransport({});
    expectPulses({2, 1, 2, 3, 3, 3}, "Returning to standalone defaults does not repeat identical notifications");
}

void greenTransportSurvivesSchematicReplacementAndUndo()
{
    using enum PrimitiveKind;
    using openstone::green::Value;
    openstone::Session session;
    const int tempo = session.addNode(Tempo, 0, 0);
    session.graph().setPluginHostTransport({.tempo = 144.5, .playing = true});
    session.beginEdit(openstone::SchematicEdit::AddNode);
    session.addNode(OfflineMode, 0, 0);
    session.finishEdit();
    check(session.applyHistory(false, [](openstone::HistoryUpdate update) {
              check(update == openstone::HistoryUpdate::Topology, "Undo rebuilds the schematic");
          }) && session.graph().readValue(tempo) == Value{144.5F}, "Undo retains the active host timing");

    auto project = std::make_unique<Schematic>();
    const int projectTempo = project->addNode(Tempo, 0, 0);
    const int afterLoad = project->addNode(AfterLoad, 0, 0);
    const int loadedTempo = project->addNode(SampleAndHold, 0, 0);
    project->connect({projectTempo, loadedTempo, 0});
    project->connect({afterLoad, loadedTempo, 1});
    check(session.openSchematic(std::move(project)) && session.graph().readValue(loadedTempo) == Value{144.5F},
          "After Load observes the active host timing in a replacement schematic");
}

void hostTransportAndMonoOutFollowCallbackTiming()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    using openstone::stream::HostTransport;
    openstone::Session session;
    auto& root = session.graph();
    const int host = session.addNode(Module, 0, 0);
    session.enterModule(host);
    auto& graph = session.graph();
    const auto add = [&](PrimitiveKind kind) { return graph.addNode(kind, 0, 0); };
    const auto tap = [&](PrimitiveKind kind, int port, int channel, std::string source) {
        const int node = add(kind);
        const int dsp = add(DspCode);
        graph.setDspDraft(dsp, std::move(source));
        session.compileDsp(dsp);
        const int out = add(VstOutput);
        const int index = graph.addNode(Int, 0, 0, channel);
        graph.connect({node, dsp, 0, port});
        graph.connect({index, out, 0});
        graph.connect({dsp, out, 1});
    };
    tap(SamplePosition, 0, 0, "monoin x; monoout y; y = x - 16777216;");
    tap(DoubleSamplePosition, 0, 1, "monodoublein x; monoout y; y = x - 16777216;");
    tap(DoublePpqPosition, 0, 2, "monodoublein x; monoout y; y = x - 2;");
    tap(DoubleBarStartPosition, 0, 3, "monodoublein x; monoout y; y = x;");
    tap(StreamTempo, 0, 4, "monoin x; monoout y; y = x;");
    tap(DoubleTimeSignature, 1, 5, "monodoublein x; monoout y; y = x;");
    tap(StreamIsPlaying, 0, 6, "monoboolin x; monoout y; y = x ? 1 : 0;");
    check(root.pluginAudioLayout(host).mainOutputs == 7, "Mono Out contributes to the host layout");

    for (const std::size_t block : {1, 2, 4, 7}) {
        auto patch = root.preparePluginAudio(host, block);
        std::array<std::array<float, 7>, 7> rendered{};
        std::array<std::span<float>, 7> output;
        for (std::size_t i = 0; i < output.size(); ++i) output[i] = rendered[i];
        HostTransport transport{.samplePosition = 16777216, .quarterPosition = 2, .barStart = -1,
                                .tempo = 150, .numerator = 3, .denominator = 8, .playing = true, .sampleRate = 48000};
        check(patch.processor().processHost({}, output, 7, transport), "The host supplies one transport snapshot per callback");
        for (int frame = 0; frame < 7; ++frame) {
            const auto narrow = static_cast<float>(transport.samplePosition + frame) - 16777216;
            check(rendered[0][frame] == narrow && rendered[1][frame] == frame &&
                  std::abs(rendered[2][frame] - frame * 150.0 / (60 * 48000)) < 1e-9 &&
                  rendered[3][frame] == -1 && rendered[4][frame] == 150 &&
                  rendered[5][frame] == 8 && rendered[6][frame] == 1,
                  "Host timing follows sample offsets across internal blocks and retains declared precision");
        }
        transport.samplePosition += 123;
        transport.quarterPosition = 7;
        transport.playing = false;
        check(patch.processor().processHost({}, output, 7, transport) &&
              std::ranges::all_of(rendered[1], [](float x) { return x == 123; }) &&
              std::ranges::all_of(rendered[2], [](float x) { return x == 5; }) &&
              std::ranges::all_of(rendered[6], [](float x) { return x == 0; }),
              "A stopped host can seek without advancing the positions");
    }
}

void clearAudioRestartsTheRunningStream()
{
    using enum PrimitiveKind;
    openstone::Schematic graph;
    const auto add = [&](PrimitiveKind kind) { return graph.addNode(kind, 0, 0); };
    const int impulse = add(Impulse);
    const int delay = add(DelayOneSample);
    const int output = add(AudioOut);
    const int clear = add(ClearAudio);
    const int trigger = add(Trigger);
    graph.connect({impulse, delay, 0});
    graph.connect({delay, output, 0});
    graph.connect({trigger, clear, 0});
    auto patch = graph.prepareAudio(2);
    std::array<float, 3> left{}, right{};
    check(patch.processor().process({}, {left, right}) && left == std::array{0.f, 1.f, 0.f}, "Start the delay");
    check(patch.processor().process({}, {left, right}) && left == std::array{0.f, 0.f, 0.f}, "The impulse is spent");
    graph.fireTrigger(trigger);
    check(patch.processor().process({}, {left, right}) && left == std::array{0.f, 1.f, 0.f},
          "Clear Audio restarts the active DSP runtime at the next callback");
}

void waveWritingReadingAndFilesRoundTrip()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    namespace fs = std::filesystem;
    struct TempWave {
        fs::path path = fs::temp_directory_path() /
            ("openstone-wave-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".wav");
        ~TempWave() { std::error_code error; fs::remove(path, error); }
    } file;
    openstone::Schematic graph;
    const auto add = [&](PrimitiveKind kind) { return graph.addNode(kind, 0, 0); };
    const auto value = [&](PrimitiveKind kind, Value data) { return graph.addNode(kind, 0, 0, std::move(data)); };
    const int memory = add(MemCreate);
    const int size = value(Int, 32);
    const int stereo = value(Boolean, true);
    const int trigger = add(Trigger);
    graph.connect({size, memory, 0});
    graph.connect({stereo, memory, 3});
    graph.connect({trigger, memory, 1});
    graph.fireTrigger(trigger);
    const int counter = add(StreamCounter);
    const int left = value(Float, 0.25F);
    const int right = value(Float, -0.5F);
    const int write = add(WaveWriteStereo);
    const int audio = add(AudioOut);
    graph.connect({memory, write, 0});
    graph.connect({counter, write, 1});
    graph.connect({left, write, 2});
    graph.connect({right, write, 3});
    graph.connect({write, audio, 1, 1});
    auto patch = graph.prepareAudio(3);
    std::array<float, 7> l{}, r{};
    check(patch.processor().process({}, {l, r}) &&
          std::ranges::all_of(r, [](float sample) { return sample == -0.5F; }),
          "The right passthrough schedules the stereo writer");
    const auto written = std::get<Memory>(graph.readValue(memory));
    for (std::size_t frame = 0; frame < 4; ++frame) {
        check(written.sample(2 * frame) == 0.25F && written.sample(2 * frame + 1) == -0.5F,
              "Wave Write Stereo updates interleaved memory and ignores indices beyond the last frame");
    }

    const int path = value(String, file.path.string());
    const int format = value(String, std::string{"int24"});
    const int save = add(SaveWaveFile);
    const int writeFile = add(Trigger);
    graph.connect({memory, save, 0});
    graph.connect({writeFile, save, 1});
    graph.connect({path, save, 2});
    graph.connect({format, save, 4});
    graph.fireTrigger(writeFile);
    check(graph.inspectOutput(save, 0).triggers == 1 && fs::exists(file.path), "Save Wave File writes 24-bit PCM");
    const int load = add(LoadWaveFile);
    const int readFile = add(Trigger);
    const int channel = value(Int, 0);
    graph.connect({path, load, 0});
    graph.connect({readFile, load, 1});
    graph.connect({channel, load, 3});
    graph.fireTrigger(readFile);
    check(integer(graph, load, 1) == 44100 && integer(graph, load, 2) == 2 &&
          integer(graph, load, 3) == 4 && integer(graph, load, 4) == 2 &&
          graph.readValue(load) == Value{written}, "Load restores the stereo samples and metadata");
    graph.setValue(channel, -1);
    graph.fireTrigger(readFile);
    const auto mixed = std::get<Memory>(graph.readValue(load));
    check(mixed.channels() == 1 && mixed.sample(0) == -0.125F, "A negative channel request mixes to mono");

    const int read = add(WaveRead);
    const int index = add(AudioIn);
    graph.connect({load, read, 0});
    graph.connect({index, read, 1});
    graph.disconnectInput(audio, 1);
    graph.connect({read, audio, 0});
    graph.connect({read, audio, 1, 1});
    for (const std::size_t block : {1, 2, 3, 5}) {
        patch = graph.prepareAudio(block);
        const std::array indices{-2.0F, 0.5F, 1.5F, 3.0F, 7.0F, 1.0F, 0.0F};
        check(patch.processor().process({indices, {}}, {l, r}) && l == r &&
              std::ranges::all_of(l, [](float sample) { return sample == -0.125F; }),
              "Wave Read clamps indices and duplicates mono into stereo across blocks");
    }
}

void waveReadHopAndWaveTableKeepTheirPhaseAcrossBuffers()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    openstone::Schematic graph;
    const auto add = [&](PrimitiveKind kind) { return graph.addNode(kind, 0, 0); };
    const int text = graph.addNode(MultilineString, 0, 0, std::string{"0\n1\n0\n-1\n"});
    const int encode = add(FloatArrayToFloatMem);
    const int counter = add(StreamCounter);
    const int read = add(WaveReadHop);
    const int hop = graph.addNode(Int, 0, 0, 1);
    const int audio = add(AudioOut);
    graph.connect({text, encode, 0});
    graph.connect({encode, read, 0});
    graph.connect({counter, read, 1});
    graph.connect({hop, read, 2});
    graph.connect({read, audio, 0});
    std::array<float, 7> l{}, r{};
    for (const std::size_t block : {1, 3, 5}) {
        auto patch = graph.prepareAudio(block);
        check(patch.processor().process({}, {l, r}) && l == std::array{0.F, 0.F, 0.F, 0.F, -1.F, -1.F, -1.F},
              "Hop 1 holds each sample lookup for two frames, including across buffer boundaries");
    }
    const int table = add(WaveTable);
    const int create = add(Trigger);
    const int oscillator = add(WaveTableRead);
    const int frequency = graph.addNode(Float, 0, 0, 0.5F);
    graph.connect({text, table, 0});
    graph.connect({create, table, 3});
    const auto before = graph.inspectOutput(table, 1).triggers;
    graph.fireTrigger(create);
    check(graph.inspectOutput(table, 1).triggers == before + 1 &&
          std::get<Memory>(graph.readValue(table, 0)).size() == 4 * 256 * sizeof(float),
          "Wave Table publishes the generated bands");
    graph.connect({table, oscillator, 0});
    graph.connect({frequency, oscillator, 1});
    graph.disconnectInput(audio, 0);
    graph.connect({oscillator, audio, 0});
    for (const std::size_t block : {1, 3, 5}) {
        auto patch = graph.prepareAudio(block);
        check(patch.processor().process({}, {l, r}), "Render a wavetable across buffers");
        const std::array expected{0.F, 1.F, 0.F, -1.F, 0.F, 1.F, 0.F};
        for (std::size_t i = 0; i < l.size(); ++i) {
            check(std::abs(l[i] - expected[i]) < 1e-6F, "A quarter-turn per sample advances the table phase");
        }
    }
}

void audioDeviceControlsAndMultichannelEndpoints()
{
    using enum PrimitiveKind;
    using namespace openstone;
    using namespace openstone::green;
    Schematic graph;
    const auto add = [&](PrimitiveKind kind) { return graph.addNode(kind, 0, 0); };
    const int list = add(AudioDevices), directList = add(DirectSoundInDevices);
    const int channels = add(AudioInputsOutputs), select = add(AudioSelect), directSelect = add(DirectSoundInSelect);
    const int rateControl = add(SetSampleRate), trigger = add(Trigger), settings = add(OpenAsioSettings);
    const int index = graph.addNode(Int, 0, 0, 1);
    const int directIndex = graph.addNode(Int, 0, 0, 0);
    const int rate = graph.addNode(Int, 0, 0, 48000);
    std::optional<AudioDeviceSetup> lastSetup;
    bool opened = false;
    graph.setAudioControlCallbacks([&](const AudioDeviceSetup& setup) {
        lastSetup = setup;
        return setup.rate != 12345;
    }, [&] { opened = true; });
    graph.setAudioDevices({{5, "System", AudioApi::Any, 1, 2}, {8, "Multichannel", AudioApi::Asio, 4, 4},
                           {12, "Legacy input", AudioApi::DirectSound, 2, 0}});
    graph.setAudioChannels({"In 1"}, {"Out 1", "Out 2"});
    check(graph.readValue(list, 0) == Value{3} &&
          graph.readValue(list, 1) == Value{StringArray{"System", "Multichannel", "Legacy input"}},
          "Audio Devices enumerates the backend's reported drivers");
    check(graph.readValue(directList, 1) == Value{StringArray{"Legacy input"}} &&
          graph.readValue(channels, 0) == Value{StringArray{"In 1"}} &&
          graph.readValue(channels, 1) == Value{StringArray{"Out 1", "Out 2"}},
          "DirectSound input filtering and active connector names are independent");
    graph.connect({index, select, 0});
    graph.connect({directIndex, directSelect, 0});
    check(graph.readValue(select, 0) == Value{1} && graph.readValue(directSelect, 0) == Value{0} &&
          lastSetup && lastSetup->input == 8 && lastSetup->output == 8 && lastSetup->directSoundInput == 12,
          "The selectors pass device IDs to the backend and preserve the DirectSound choice separately");
    graph.connect({rate, rateControl, 0});
    graph.connect({trigger, rateControl, 1});
    graph.connect({trigger, settings, 0});
    const auto errors = graph.inspectOutput(rateControl, 0).triggers;
    graph.fireTrigger(trigger);
    check(graph.sampleRate() == 48000 && lastSetup->rate == 48000 && opened,
          "Set Sample Rate and Open ASIO Settings dispatch to the device backend");
    graph.setValue(rate, 12345);
    graph.fireTrigger(trigger);
    check(graph.sampleRate() == 48000 && graph.inspectOutput(rateControl, 0).triggers == errors + 1,
          "An unsupported rate pulses Error and preserves the previous rate");

    const int asioIn = add(AsioIn), asioOut = add(AsioOut);
    graph.connect({asioIn, asioOut, 3, 2});
    auto patch = graph.prepareAudio(2);
    const std::array input{0.25F, 0.5F, -0.25F};
    std::array<float, 3> output{};
    const std::array<std::span<const float>, 4> source{{{}, {}, input, {}}};
    const std::array<std::span<float>, 4> destination{{{}, {}, {}, output}};
    check(patch.inputApi() == AudioApi::Asio && patch.outputApi() == AudioApi::Asio &&
          patch.processor().outputCount() == 4 && patch.processor().processHost(source, destination, input.size()) &&
          output == input, "ASIO endpoints route their numbered Mono channels without a stereo limit");
}

void monoToFramePublishesAlignedFrames()
{
    using enum PrimitiveKind;
    using namespace openstone;
    using namespace openstone::green;
    for (std::size_t block : {1, 2, 5}) {
        Schematic graph;
        const auto add = [&](PrimitiveKind kind) { return graph.addNode(kind, 0, 0); };
        const int audio = add(AudioIn), output = add(AudioOut), frame = add(MonoToFrame);
        const int size = graph.addNode(Int, 0, 0, 3);
        graph.connect({audio, frame, 0, 1});
        graph.connect({size, frame, 1});
        auto patch = graph.prepareAudio(block);
        graph.setLiveAudio(&patch);
        const auto process = [&](std::initializer_list<float> input) {
            std::vector<float> l(input.size(), 1.F), r(input.size(), 1.F);
            check(patch.processor().process({std::span<const float>{}, std::span{input}}, {l, r}) &&
                  std::ranges::all_of(l, [](float v) { return v == 0; }) &&
                  std::ranges::all_of(r, [](float v) { return v == 0; }),
                  "A frame tap renders its branch without routing it to the audio outputs");
        };
        process({1, 2});
        check(!graph.publishAudioFrames(), "Mono to Frame waits for a complete frame");
        process({3, 4});
        check(graph.publishAudioFrames() && graph.readValue(frame) == Value{FloatArray{1, 2, 3}} &&
              !graph.publishAudioFrames(), "The same complete frame is published once");
        process({5, 6, 7});
        check(graph.publishAudioFrames() && graph.readValue(frame) == Value{FloatArray{4, 5, 6}},
              "Frames remain sample-aligned while audio callback sizes change");
    }
}

void pluginModuleRoutesHostAudioAndReportsAuxChannels()
{
    using enum PrimitiveKind;
    using namespace openstone;
    using namespace openstone::green;
    Schematic root;
    const int moduleId = root.addNode(Module, 0, 0);
    auto& graph = *root.module(moduleId);
    const int main = graph.addNode(ModuleInput, 0, 0);
    graph.setModulePortType(main, Type::Mono);
    const auto addOutput = [&](int source, int y) {
        const int port = graph.addNode(ModuleOutput, 600, y);
        graph.setModulePortType(port, Type::Mono);
        graph.connect({source, port, 0});
    };
    const int host1 = graph.addNode(VstInput, 0, 75);
    const int host2 = graph.addNode(VstInput, 0, 150);
    const int one = graph.addNode(Int, 0, 225, 1);
    const int two = graph.addNode(Int, 0, 300, 2);
    const int hostIndex = graph.addNode(Int, 0, 375, 2);
    graph.connect({one, host1, 0});
    graph.connect({hostIndex, host2, 0});
    addOutput(main, 0);
    addOutput(host1, 75);
    addOutput(host2, 150);
    const int aux = graph.addNode(Vst3AuxChannels, 0, 450);
    graph.connect({two, aux, 0});
    graph.connect({one, aux, 1});
    const auto layout = root.pluginAudioLayout(moduleId);
    check(layout.mainInputs == 1 && layout.auxInputs == 2 && layout.mainOutputs == 2 && layout.auxOutputs == 1,
          "Auxiliary channels occupy the last slots in the module's input and output layout");
    check(graph.readValue(aux, 0) == Value{IntArray{0, 0}} && graph.readValue(aux, 1) == Value{IntArray{0}},
          "Auxiliary states start inactive");
    const auto inputPulses = graph.inspectOutput(aux, 0).triggers;
    const auto outputPulses = graph.inspectOutput(aux, 1).triggers;
    root.setPluginAuxStates(moduleId, std::array{0, 1}, std::array{1});
    root.setPluginAuxStates(moduleId, std::array{0, 1}, std::array{1});
    check(graph.readValue(aux, 0) == Value{IntArray{0, 1}} && graph.readValue(aux, 1) == Value{IntArray{1}} &&
          graph.inspectOutput(aux, 0).triggers == inputPulses + 1 &&
          graph.inspectOutput(aux, 1).triggers == outputPulses + 1,
          "Host bus changes publish each changed state list once");

    const int info = graph.addNode(VstPluginInfo, 0, 525);
    const int name = graph.addNode(String, 0, 600, std::string{"Sidechain"});
    const int disabled = graph.addNode(Boolean, 0, 675, false);
    graph.connect({name, info, 1});
    graph.connect({two, info, 3});
    graph.connect({disabled, info, 12});
    const auto metadata = root.pluginExportInfo(moduleId);
    check(metadata.name == "Sidechain" && metadata.version == 2 && metadata.vst2 == false && !metadata.vst3 &&
          root.pluginExportInfo(root.addNode(Module, 0, 0)).name.empty(),
          "Export metadata is scoped to the module and distinguishes unset from false");

    auto patch = root.preparePluginAudio(moduleId, 2);
    const std::array left{1.f, 2.f, 3.f, 4.f, 5.f}, side{-.5f, 0.f, .5f, 1.f, -1.f};
    const std::array<float, 5> silence{};
    std::array<float, 5> mainOut{}, spareOut{}, auxOut{};
    spareOut.fill(99.f);
    const std::array<std::span<const float>, 3> input{left, {}, side};
    const std::array<std::span<float>, 3> output{mainOut, spareOut, auxOut};
    check(!patch.processor().processHost(input, output, 4), "Reject an inconsistent host frame count");
    check(patch.processor().processHost(input, output, 5) && mainOut == left && spareOut == silence && auxOut == side,
          "Host buffers route independently and an inactive input gives silence across internal blocks");
    check(patch.processor().processHost(input, std::array<std::span<float>, 3>{mainOut, spareOut, {}}, 5),
          "A host may omit an inactive output buffer");
    graph.setValue(hostIndex, 1);
    check(!root.refreshAudioControls(patch), "Changing VST Input's channel invalidates the compiled wiring");
    patch = root.preparePluginAudio(moduleId, 2);
    check(patch.processor().processHost(input, output, 5) && auxOut == silence,
          "Rebuilding uses the new host channel");

    const int hardwareOut = graph.addNode(AudioOut, 600, 225);
    graph.connect({host2, hardwareOut, 0});
    patch = root.prepareAudio(2);
    check(patch.processor().process({left, side}, {mainOut, spareOut}) && mainOut == silence && spareOut == silence,
          "VST Input does not bind to desktop audio");
}

void presetOrderAndBypassMatchHostParameters()
{
    using enum PrimitiveKind;
    using namespace openstone;
    Schematic graph;
    const int manager = graph.addNode(PresetManager, 0, 0);
    const int enabled = graph.addNode(PresetBypass, 0, 0);
    const int gain = graph.addNode(PresetFloat, 0, 0);
    const int text = graph.addNode(PresetString, 0, 0);
    const int report = graph.addNode(PresetShowOrder, 0, 0);
    const int get = graph.addNode(Trigger, 0, 0);
    const int grouped = graph.addNode(Boolean, 0, 0);
    const int order = graph.addNode(Int, 0, 0, -1);
    const int count = graph.addNode(Int, 0, 0, 2);
    const int program = graph.addNode(Int, 0, 0);
    graph.connect({count, manager, 0});
    graph.connect({program, manager, 1});
    graph.connect({manager, enabled, 0});
    graph.connect({order, text, 4});
    graph.connect({grouped, report, 0});
    graph.connect({get, report, 1}, LinkNotification::Silent);
    graph.fireTrigger(get);
    check(std::get<std::string>(graph.readValue(report)) == "Preset String\nPreset Bypass\nPreset Float",
          "Preset Show Order sorts across both parameter groups");
    graph.setValue(grouped, true);
    graph.fireTrigger(get);
    check(std::get<std::string>(graph.readValue(report)) == "Preset Bypass\nPreset Float\nPreset String",
          "Grouped lists host-automatable parameters before the preset-only parameters");

    const auto parameters = graph.pluginParameters();
    check(parameters.size() == 2 && parameters[0].bypass && parameters[0].discrete &&
          parameters[0].address.path == std::vector{enabled} && parameters[1].address.path == std::vector{gain},
          "The host can identify the dedicated bypass parameter");
    check(graph.receivePluginParameter(parameters[0].address, 1.0f) &&
          std::get<bool>(graph.readValue(enabled)), "The host can enable bypass from its parameter API");
    graph.setValue(program, 1);
    check(std::get<bool>(graph.readValue(enabled)), "Preset recall preserves the host-owned bypass value");
    check(graph.receivePluginParameter(parameters[0].address, 0.0f) &&
          !std::get<bool>(graph.readValue(enabled)), "The host can disable bypass");
}

void pluginHostControlsAreAvailableThroughTheControlThread()
{
    using enum PrimitiveKind;
    using namespace openstone;
    Schematic graph;
    const int container = graph.addNode(Module, 0, 0);
    auto& child = *graph.module(container);
    const int latency = child.addNode(DelayCompensation, 0, 0);
    const int samples = child.addNode(Int, 0, 0, 128);
    const int editor = child.addNode(VstEditorOpen, 0, 0);
    const int info = child.addNode(VstHostInfo, 0, 0);
    const int product = child.addNode(String, 0, 0, std::string{"Preview"});
    const int gain = child.addNode(PresetFloat, 0, 0);
    const int toggle = child.addNode(PresetBool, 0, 0);
    const int gainInput = child.addNode(Float, 0, 0);
    const int first = child.addNode(Int, 0, 0, -2);
    const int enabled = child.addNode(Boolean, 0, 0, true);
    std::vector<int> delays;
    std::vector<std::pair<PluginParameterAddress, float>> recorded;
    graph.setPluginHostCallbacks({[&](int delay) { delays.push_back(delay); },
        [&](const auto& address, float value) { recorded.emplace_back(address, value); }});
    child.connect({samples, latency, 0});
    child.setValue(samples, 256);
    child.disconnectInput(latency, 0);
    check(delays == std::vector{0, 128, 256, 0} && graph.pluginLatencySamples() == 0,
          "Latency changes are published once and reset when disconnected");
    child.connect({product, info, 1});
    check(std::get<std::string>(child.readValue(info, 1)) == "Preview", "Host Info uses test inputs when standalone");
    child.setPluginHostInfo(PluginHostInfo{"Vendor", "Test Host", "2"});
    check(std::get<std::string>(child.readValue(info, 0)) == "Vendor" &&
          std::get<std::string>(child.readValue(info, 1)) == "Test Host" &&
          std::get<std::string>(child.readValue(info, 2)) == "2",
          "The host identity is shared with nested modules");
    graph.setPluginHostInfo(std::nullopt);
    check(std::get<std::string>(child.readValue(info, 1)) == "Preview", "Detaching the host restores the test inputs");
    graph.setPluginEditorOpen(true);
    graph.setPluginEditorOpen(true);
    check(std::get<bool>(child.readValue(editor)) && child.inspectOutput(editor).triggers == 1,
          "Host editor visibility only triggers when it changes");
    graph.setPluginEditorOpen(false);
    check(!std::get<bool>(child.readValue(editor)), "Closing the host editor clears the flag");

    child.connect({gainInput, gain, 1});
    child.connect({first, toggle, 9});
    child.connect({enabled, toggle, 5});
    const auto parameters = graph.pluginParameters();
    check(parameters.size() == 2 && parameters[0].address.path == std::vector{container, toggle} &&
          parameters[0].discrete && parameters[1].address.path == std::vector{container, gain},
          "The host can enumerate parameters from nested modules in authored order");
    check(graph.receivePluginParameter(parameters[0].address, .7f) &&
          graph.receivePluginParameter(parameters[1].address, .375f) && recorded.empty() &&
          std::get<bool>(child.readValue(toggle)) && std::get<float>(child.readValue(gain)) == .375f,
          "Host edits reach the typed outputs without recording automation back to the host");
    child.setValue(gainInput, .5f);
    check(recorded.size() == 1 && recorded.front().first.path == parameters[1].address.path &&
          recorded.front().second == .5f, "A local parameter edit records automation for the addressed node");
    child.setValue(enabled, false);
    check(graph.pluginParameters().size() == 1 && !graph.receivePluginParameter(parameters[0].address, 0),
          "A parameter with Auto disabled is omitted from host automation");
}

void greenArrayPrimitivesPreserveTypesValuesAndTriggers()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    Schematic graph;
    const int builder = graph.addNode(ArrayBuilder, 100, 0);
    const int append = graph.addNode(AppendArray, 240, 0);
    const int get = graph.addNode(ArrayGet, 380, 0);
    const int source = graph.addNode(Int, 0, 0, std::int32_t{10});
    const int next = graph.addNode(Float, 0, 60, 2.75F);
    const int last = graph.addNode(Int, 0, 120, std::int32_t{30});
    const int index = graph.addNode(Int, 240, 120, std::int32_t{1});
    const int pulse = graph.addNode(Trigger, 240, 200);
    const int valueCount = graph.addNode(TriggerCounter, 530, 0);
    const int doneCount = graph.addNode(TriggerCounter, 530, 70);

    check(findNode(graph, builder).inputPorts().size() == 1 && !graph.outputType(builder),
          "An unconnected Array Builder has one spare template input");
    graph.connect({builder, append, 0});
    graph.connect({append, get, 0});
    graph.connect({index, get, 1});
    graph.connect({get, valueCount, 0});
    graph.connect({get, doneCount, 0, 1});
    graph.connect({pulse, get, 2}, LinkNotification::Silent);
    const auto valuePulses = integer(graph, valueCount);
    const auto donePulses = integer(graph, doneCount);

    graph.connect({source, builder, 0});
    graph.connect({next, builder, 1});
    graph.connect({last, builder, 2});
    check(findNode(graph, builder).inputPorts().size() == 4 && graph.inputType(builder, 1) == Type::Int &&
          std::get<IntArray>(graph.readValue(builder)) == IntArray{10, 2, 30},
          "Wiring the spare adds one input, reuses the first type, and preserves element order");
    const int color = graph.addNode(Color, 0, 180);
    bool rejected = false;
    try {
        graph.connect({color, builder, 3});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected && findNode(graph, builder).inputPorts().size() == 4 &&
          std::get<IntArray>(graph.readValue(builder)) == IntArray{10, 2, 30}, "A rejected wire leaves the spare available");
    graph.connect({builder, append, 1});
    check(std::get<IntArray>(graph.readValue(append)) == IntArray{10, 2, 30, 10, 2, 30},
          "Append Array joins the arrays in order");
    check(integer(graph, get) == 0 && integer(graph, valueCount) == valuePulses && integer(graph, doneCount) == donePulses,
          "Changes at the array or index inputs leave the held value and its trigger counts unchanged");
    graph.fireTrigger(pulse);
    check(integer(graph, get) == 2 && integer(graph, valueCount) == valuePulses + 1 && integer(graph, doneCount) == donePulses + 1,
          "Get publishes the selected value and then signals completion");
    graph.setValue(index, std::int32_t{2});
    graph.setValue(last, std::int32_t{31});
    check(integer(graph, get) == 2 && integer(graph, doneCount) == donePulses + 1, "Get holds its previous result");
    graph.fireTrigger(pulse);
    check(integer(graph, get) == 31, "Get reads current values on the next trigger");
    graph.setValue(index, std::int32_t{-1});
    graph.fireTrigger(pulse);
    check(integer(graph, get) == 0 && integer(graph, doneCount) == donePulses + 3,
          "A negative index returns the typed default and signals completion");
    graph.setValue(index, std::int32_t{6});
    graph.fireTrigger(pulse);
    check(integer(graph, get) == 0 && integer(graph, doneCount) == donePulses + 4, "An index at the length is out of bounds");

    graph.disconnectLink({next, builder, 1});
    check(std::get<IntArray>(graph.readValue(builder)) == IntArray{10, 0, 31}, "Removing a middle wire preserves later indices");
    graph.disconnectLink({last, builder, 2});
    check(findNode(graph, builder).inputPorts().size() == 2 && std::get<IntArray>(graph.readValue(builder)) == IntArray{10},
          "Removing the last wire leaves one spare after the last occupied index");
    graph.setTemplateType(builder, Type::Float);
    check(graph.outputType(get) == Type::Float && std::get<float>(graph.readValue(get)) == 0.F &&
          std::get<FloatArray>(graph.readValue(append)) == FloatArray{10.F, 10.F}, "Retyping resets the held value and propagates the new type");
    graph.setTemplateType(builder, Type::Int);

    const int typedBuilder = graph.addNode(ArrayBuilder, 100, 300);
    const int typedGet = graph.addNode(ArrayGet, 240, 300);
    const int typedReadout = graph.addNode(Float, 380, 300);
    graph.connect({typedBuilder, typedGet, 0});
    graph.connect({typedGet, typedReadout, 0});
    check(graph.outputType(typedBuilder) == Type::FloatArray && std::get<FloatArray>(graph.readValue(typedBuilder)).empty(),
          "A destination hint passes through Array Get and an unwired builder returns an empty array");

    const auto checkType = [&]<class Array>(Type type, int first, int second, Array expected) {
        const int left = graph.addNode(ArrayBuilder, 0, 500);
        const int right = graph.addNode(ArrayBuilder, 100, 500);
        const int join = graph.addNode(AppendArray, 240, 500);
        const int pick = graph.addNode(ArrayGet, 380, 500);
        graph.connect({first, left, 0});
        graph.connect({second, right, 0});
        graph.connect({right, join, 1});
        check(graph.outputType(join) == type && std::get<Array>(graph.readValue(join)) == Array{expected[1]},
              "An unconnected append operand contributes an empty array");
        graph.connect({left, join, 0});
        graph.connect({join, pick, 0});
        graph.connect({pulse, pick, 2});
        check(std::get<Array>(graph.readValue(join)) == expected &&
              std::get<typename Array::value_type>(graph.readValue(pick)) == expected[0],
              "Each supported Green array type can be built, appended and sampled without changing its element values");
    };
    const int word = graph.addNode(String, 0, 430, std::string{"abc"});
    const int other = graph.addNode(String, 0, 460, std::string{"def"});
    checkType(Type::StringArray, word, other, StringArray{"abc", "def"});
    const int point = graph.addNode(Point, 0, 600);
    const int otherPoint = graph.addNode(Point, 0, 660);
    graph.connect({next, point, 0});
    graph.connect({source, otherPoint, 1});
    checkType(Type::PointArray, point, otherPoint, PointArray{{2.75F, 0}, {0, 10}});
    checkType(Type::FloatArray, next, next, FloatArray{2.75F, 2.75F});
}

void arrayBuilderPortsFollowTopologyEdits()
{
    using enum PrimitiveKind;
    using openstone::green::IntArray;
    using openstone::SchematicEdit;
    openstone::Session session;
    const int first = session.addNode(Int, 0, 0);
    const int second = session.addNode(Int, 0, 100);
    const int builder = session.addNode(ArrayBuilder, 140, 0);
    session.setValue(first, std::int32_t{3});
    session.setValue(second, std::int32_t{5});
    session.connect({first, builder, 0});
    session.beginEdit(SchematicEdit::CreateLink);
    session.connect({second, builder, 1});
    session.finishEdit();
    const auto expect = [&](std::size_t portCount, const IntArray& values) {
        check(findNode(session.graph(), builder).inputPorts().size() == portCount &&
              std::get<IntArray>(session.graph().readValue(builder)) == values, "Builder shape and values follow link topology");
    };
    expect(3, {3, 5});
    const auto beforeApply = [](openstone::HistoryUpdate) {};
    check(session.applyHistory(false, beforeApply), "Undo the wire that grew the builder");
    expect(2, {3});
    check(session.applyHistory(true, beforeApply), "Redo the wire that grew the builder");
    expect(3, {3, 5});

    const auto copies = session.duplicateNodes(std::array{first, second, builder}, 20, 20);
    check(findNode(session.graph(), copies[2]).inputPorts().size() == 3 &&
          std::get<IntArray>(session.graph().readValue(copies[2])) == IntArray{3, 5}, "Duplicate retains dynamic connector indices");
    const int module = session.makeModule(std::array{builder});
    const auto* child = session.graph().module(module);
    const auto inside = std::ranges::find(child->nodes(), ArrayBuilder, &openstone::SchematicNode::kind);
    check(inside != child->nodes().end() && inside->inputPorts().size() == 3 &&
          std::get<IntArray>(child->readValue(inside->id)) == IntArray{3, 5}, "Make Module preserves both dynamic inputs and the spare");
}

void greenArrayTransformsSampleOnTrigger()
{
    using enum PrimitiveKind;
    using namespace openstone::green;

    Schematic graph;
    const int text = graph.addNode(MultilineString, 0, 0, std::string{"10\n20\n30"});
    const int amount = graph.addNode(Int, 0, 100, std::int32_t{2});
    const int first = graph.addNode(Int, 0, 200, std::int32_t{1});
    const int pad = graph.addNode(Float, 0, 300, 7.75F);
    const int pulse = graph.addNode(Trigger, 0, 400);
    const int section = graph.addNode(ArraySection, 180, 0);
    const int rotate = graph.addNode(RotateArray, 180, 200);
    const int resize = graph.addNode(ArrayFixedSize, 180, 400);
    const int counter = graph.addNode(TriggerCounter, 380, 0);

    graph.setTemplateType(section, Type::IntArray);
    graph.setTemplateType(rotate, Type::IntArray);
    graph.connect({text, section, 0});
    graph.connect({text, rotate, 0});
    graph.connect({amount, section, 1});
    graph.connect({amount, rotate, 1});
    graph.connect({amount, resize, 1});
    graph.connect({first, section, 2});
    graph.connect({pad, resize, 2});
    graph.connect({section, resize, 0});
    graph.connect({resize, counter, 0}, LinkNotification::Silent);
    graph.connect({pulse, section, 3}, LinkNotification::Silent);
    graph.connect({pulse, rotate, 2}, LinkNotification::Silent);
    graph.connect({pulse, resize, 3}, LinkNotification::Silent);

    const auto read = [&](int id) { return std::get<IntArray>(graph.readValue(id)); };
    check(graph.outputType(resize) == Type::IntArray && graph.inputType(resize, 2) == Type::Int &&
          read(section).empty() && read(rotate).empty() && read(resize).empty(),
          "The array input determines the resize type and all outputs start empty");
    graph.fireTrigger(pulse);
    check(read(section) == IntArray{20, 30} && read(rotate) == IntArray{20, 30, 10} &&
          read(resize) == IntArray{20, 30} && integer(graph, counter) == 1,
          "Section uses a zero-based index and right rotation wraps. Resize publishes one trigger");

    graph.setValue(amount, std::int32_t{5});
    graph.setValue(first, std::int32_t{2});
    check(read(section) == IntArray{20, 30} && read(rotate) == IntArray{20, 30, 10} &&
          read(resize) == IntArray{20, 30} && integer(graph, counter) == 1,
          "Editing input values leaves snapshots and trigger counts unchanged");
    graph.fireTrigger(pulse);
    check(read(section) == IntArray{30} && read(rotate) == IntArray{20, 30, 10} &&
          read(resize) == IntArray{30, 7, 7, 7, 7},
          "Sections truncate at the end, large shifts wrap, and padding converts to the element type");
    graph.setValue(amount, std::int32_t{1});
    graph.setValue(first, std::int32_t{0});
    graph.fireTrigger(pulse);
    check(read(section) == IntArray{10} && read(resize) == IntArray{10}, "A section can start at index zero");

    graph.disconnectLink({section, resize, 0});
    const int builder = graph.addNode(ArrayBuilder, 120, 600);
    graph.connect({amount, builder, 0});
    graph.connect({first, builder, 1});
    graph.connect({builder, resize, 0});
    graph.fireTrigger(pulse);
    check(read(resize) == IntArray{1}, "Resize truncates the source when the requested length is shorter");
    graph.setValue(amount, std::int32_t{-1});
    graph.fireTrigger(pulse);
    check(read(section).empty() && read(rotate) == IntArray{20, 30, 10} && read(resize).empty(),
          "Negative sizes yield empty arrays and a negative shift rotates left");
    graph.setValue(amount, std::numeric_limits<std::int32_t>::min());
    graph.fireTrigger(pulse);
    check(read(rotate) == IntArray{30, 10, 20}, "The smallest signed shift wraps without overflow");
    graph.setValue(amount, std::int32_t{1});
    graph.setValue(first, std::int32_t{-1});
    graph.fireTrigger(pulse);
    check(read(section).empty(), "A section with a negative start is empty");
    graph.setValue(first, std::int32_t{3});
    graph.fireTrigger(pulse);
    check(read(section).empty(), "A section starting at the array length is empty");

    const auto previous = read(resize);
    const auto triggers = integer(graph, counter);
    graph.setValue(amount, std::numeric_limits<std::int32_t>::max());
    bool rejected = false;
    try {
        graph.fireTrigger(pulse);
    } catch (const std::length_error&) {
        rejected = true;
    }
    check(rejected && read(resize) == previous && integer(graph, counter) == triggers,
          "An oversized resize preserves the previous result and does not publish a trigger");
    graph.setValue(amount, std::int32_t{2});
    graph.fireTrigger(pulse);
    check(read(resize) == IntArray{2, 3} && integer(graph, counter) == triggers + 1,
          "A subsequent trigger can update the result after a rejected resize");
    graph.setTemplateType(rotate, Type::FloatArray);
    check(std::get<FloatArray>(graph.readValue(rotate)).empty(), "Changing array type clears the snapshot");
}

void greenArrayTransformsSupportNonIntegerArrays()
{
    using enum PrimitiveKind;
    using namespace openstone::green;

    Schematic graph;
    const int count = graph.addNode(Int, 0, 0, std::int32_t{2});
    const int pulse = graph.addNode(Trigger, 0, 80);
    const int word = graph.addNode(String, 0, 160, std::string{"fill"});
    const int number = graph.addNode(Float, 0, 240, 1.5F);
    const int point = graph.addNode(Point, 0, 320);
    graph.connect({number, point, 0});

    const auto verify = [&]<class Array>(Type type, int scalar, Array expected) {
        const int resize = graph.addNode(ArrayFixedSize, 150, 0);
        const int rotate = graph.addNode(RotateArray, 320, 0);
        const int section = graph.addNode(ArraySection, 500, 0);
        graph.connect({scalar, resize, 2});
        graph.connect({count, resize, 1});
        graph.connect({resize, rotate, 0});
        graph.connect({rotate, section, 0});
        graph.connect({count, section, 1});
        graph.connect({pulse, resize, 3}, LinkNotification::Silent);
        graph.connect({pulse, rotate, 2}, LinkNotification::Silent);
        graph.connect({pulse, section, 3}, LinkNotification::Silent);
        check(graph.outputType(resize) == type && graph.outputType(section) == type,
              "The Default connector can infer and propagate an array type");
        graph.fireTrigger(pulse);
        check(std::get<Array>(graph.readValue(section)) == expected,
              "The typed default survives padding, rotation and extraction");
    };
    verify(Type::StringArray, word, StringArray{"fill", "fill"});
    verify(Type::FloatArray, number, FloatArray{1.5F, 1.5F});
    verify(Type::PointArray, point, PointArray{{1.5F, 0}, {1.5F, 0}});
}

void indexedArrayStoresValuesOnActionTriggers()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    Schematic graph;
    const int array = graph.addNode(Array, 170, 0);
    const int value = graph.addNode(Int, 0, 0, std::int32_t{7});
    const int index = graph.addNode(Int, 0, 70, std::int32_t{2});
    const int set = graph.addNode(Trigger, 0, 140);
    const int insert = graph.addNode(Trigger, 0, 210);
    const int erase = graph.addNode(Trigger, 0, 280);
    const int clear = graph.addNode(Trigger, 0, 350);
    const int changes = graph.addNode(TriggerCounter, 350, 0);
    graph.connect({value, array, 0});
    graph.connect({index, array, 1});
    graph.connect({set, array, 2}, LinkNotification::Silent);
    graph.connect({insert, array, 5}, LinkNotification::Silent);
    graph.connect({erase, array, 6}, LinkNotification::Silent);
    graph.connect({clear, array, 3}, LinkNotification::Silent);
    graph.connect({array, changes, 0, 1}, LinkNotification::Silent);

    const auto read = [&] { return std::get<IntArray>(graph.readValue(array)); };
    check(graph.outputType(array) == Type::IntArray && graph.inputType(array, 4) == Type::IntArray &&
          read().empty(), "Array infers the stored type from Value and starts empty");
    graph.fireTrigger(set);
    check(read() == IntArray{0, 0, 7} && std::get<std::int32_t>(graph.readValue(array, 2)) == 3 &&
          integer(graph, changes) == 1, "Set extends the array with defaults and signals the new size and contents");

    graph.setValue(index, std::int32_t{1});
    graph.setValue(value, std::int32_t{4});
    check(read() == IntArray{0, 0, 7} && integer(graph, changes) == 1, "Value and Index only stage inputs");
    graph.fireTrigger(insert);
    graph.setValue(index, std::int32_t{2});
    graph.fireTrigger(erase);
    check(read() == IntArray{0, 4, 7} && integer(graph, changes) == 3, "Insert and Delete shift the remaining values");

    graph.setValue(index, std::int32_t{4});
    graph.fireTrigger(insert);
    graph.setValue(index, std::int32_t{-1});
    graph.fireTrigger(set);
    graph.fireTrigger(erase);
    check(read() == IntArray{0, 4, 7, 0, 4} && integer(graph, changes) == 4,
          "Insert beyond the end pads the gap, and negative indices do not mutate the array");

    graph.setValue(index, std::numeric_limits<std::int32_t>::max());
    bool rejected = false;
    try {
        graph.fireTrigger(set);
    } catch (const std::length_error&) {
        rejected = true;
    }
    check(rejected && read() == IntArray{0, 4, 7, 0, 4} && integer(graph, changes) == 4,
          "Oversized edits leave array contents and trigger counts intact");

    const int replacement = graph.addNode(ArrayBuilder, 0, 420);
    graph.connect({value, replacement, 0});
    graph.connect({replacement, array, 4});
    check(read() == IntArray{4} && integer(graph, changes) == 5, "Assign replaces the array on its value trigger");
    graph.fireTrigger(clear);
    check(read().empty() && integer(graph, changes) == 6, "Clear empties the array");
    graph.fireTrigger(erase);
    check(integer(graph, changes) == 6, "Delete beyond the end has no effect");
}

void arrayHistoryKeepsLatestValues()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    Schematic graph;
    const int history = graph.addNode(ArrayHistory, 140, 0);
    const int capacity = graph.addNode(Int, 0, 0, std::int32_t{2});
    const int x = graph.addNode(Float, 0, 70, 1.0F);
    const int point = graph.addNode(Point, 0, 140);
    const int add = graph.addNode(Trigger, 0, 210);
    const int reverse = graph.addNode(Boolean, 0, 280, false);
    graph.connect({x, point, 0});
    graph.connect({capacity, history, 0});
    graph.connect({point, history, 1});
    graph.connect({add, history, 2}, LinkNotification::Silent);
    graph.connect({reverse, history, 3});
    const auto read = [&] { return std::get<PointArray>(graph.readValue(history)); };
    check(graph.outputType(history) == Type::PointArray && read().empty(), "History starts empty and infers Point Array");
    graph.fireTrigger(add);
    graph.setValue(x, 2.0F);
    check(read() == PointArray{{1, 0}}, "A value change alone does not append to History");
    graph.fireTrigger(add);
    graph.setValue(x, 3.0F);
    graph.fireTrigger(add);
    check(read() == PointArray{{3, 0}, {2, 0}}, "History displaces the oldest sample and puts the newest first");
    graph.setValue(reverse, true);
    check(read() == PointArray{{2, 0}, {3, 0}}, "Rev puts the newest sample last");
    graph.setValue(capacity, std::int32_t{1});
    check(read() == PointArray{{3, 0}}, "Reducing Size trims the existing history");
    graph.setValue(capacity, std::int32_t{0});
    graph.fireTrigger(add);
    check(read().empty(), "Zero Size clears the history and suppresses additional samples");
}

void arrayQueueAndStackExposePopOrder()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    Schematic graph;
    const int queue = graph.addNode(ArrayQueue, 150, 0);
    const int stack = graph.addNode(ArrayStack, 150, 180);
    const int readout = graph.addNode(String, 350, 0);
    const int text = graph.addNode(String, 0, 0, std::string{"first"});
    const int push = graph.addNode(Trigger, 0, 60);
    const int pop = graph.addNode(Trigger, 0, 120);
    const int clear = graph.addNode(Trigger, 0, 180);

    graph.connect({queue, readout, 0, 0});
    check(graph.inputType(queue, 0) == Type::String && graph.outputType(queue, 2) == Type::StringArray,
          "A typed consumer of Top can specialize the scalar and array connectors");
    for (const auto id : {queue, stack}) {
        graph.connect({text, id, 0});
        graph.connect({push, id, 1}, LinkNotification::Silent);
        graph.connect({pop, id, 2}, LinkNotification::Silent);
        graph.connect({clear, id, 3}, LinkNotification::Silent);
    }
    const auto contents = [&](int id) { return std::get<StringArray>(graph.readValue(id, 2)); };
    const auto top = [&](int id) { return std::get<std::string>(graph.readValue(id)); };
    const auto size = [&](int id) { return std::get<std::int32_t>(graph.readValue(id, 1)); };
    graph.fireTrigger(push);
    graph.setValue(text, std::string{"second"});
    graph.fireTrigger(push);
    check(contents(queue) == StringArray{"first", "second"} && contents(stack) == StringArray{"second", "first"} &&
          top(queue) == "first" && top(stack) == "second" && size(queue) == 2 && size(stack) == 2,
          "Both All outputs list the values in pop order, starting with Top");
    graph.fireTrigger(pop);
    check(top(queue) == "second" && top(stack) == "first" && size(queue) == 1 && size(stack) == 1,
          "Queue pops the oldest entry and Stack pops the newest");
    graph.fireTrigger(pop);
    const auto queuePulses = graph.inspectOutput(queue, 2).triggers;
    graph.fireTrigger(pop);
    check(top(queue).empty() && top(stack).empty() && size(queue) == 0 && size(stack) == 0 &&
          graph.inspectOutput(queue, 2).triggers == queuePulses, "Empty pop preserves the defaults without sending a pulse");

    const int first = graph.addNode(String, 0, 240, std::string{"first"});
    const int replacement = graph.addNode(ArrayBuilder, 0, 300);
    graph.connect({first, replacement, 0});
    graph.connect({text, replacement, 1});
    graph.connect({replacement, queue, 4});
    graph.connect({replacement, stack, 4});
    check(contents(queue) == StringArray{"first", "second"} && contents(stack) == contents(queue),
          "Replace accepts the same pop order as All for both primitives");
    graph.fireTrigger(pop);
    check(top(queue) == "second" && top(stack) == "second", "Replace puts the first array element at Top");
    graph.fireTrigger(clear);
    check(contents(queue).empty() && contents(stack).empty(), "Clear removes all entries");

    graph.disconnectInput(stack, 4);
    graph.fireTrigger(push);
    check(contents(stack) == StringArray{"second"}, "The stack still holds values after a same-type rewire");
    graph.setTemplateType(stack, Type::FloatArray);
    check(graph.outputType(stack, 2) == Type::FloatArray && std::get<FloatArray>(graph.readValue(stack, 2)).empty(),
          "Changing the resolved type discards the previous storage");
    graph.disconnectInput(stack, 0);
    const int number = graph.addNode(Float, 0, 360, 1.5F);
    graph.connect({number, stack, 0});
    graph.fireTrigger(push);
    check(std::get<FloatArray>(graph.readValue(stack, 2)) == FloatArray{1.5F}, "The new type accepts pushes");
}

void arrayInterpolationAndResamplingRespectModesAndBoundaries()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    Schematic graph;
    const int source = graph.addNode(MultilineString, 0, 0, std::string{"0\n4\n8\n16"});
    const int index = graph.addNode(Float, 0, 60, 1.25F);
    const int mode = graph.addNode(Int, 0, 120, std::int32_t{0});
    const int pulse = graph.addNode(Trigger, 0, 180);
    const int lookup = graph.addNode(ArrayInterpolatedGet, 200, 0);
    graph.setTemplateType(lookup, Type::FloatArray);
    graph.connect({source, lookup, 0});
    graph.connect({mode, lookup, 1});
    graph.connect({index, lookup, 2});
    graph.connect({pulse, lookup, 3}, LinkNotification::Silent);
    constexpr std::array expected{4.0F, 5.0F, 4.90625F, 4.84375F, 5.0104167F};
    for (int interpolation = 0; interpolation < static_cast<int>(expected.size()); ++interpolation) {
        graph.setValue(mode, std::int32_t{interpolation});
        graph.fireTrigger(pulse);
        check(std::abs(std::get<float>(graph.readValue(lookup)) - expected[interpolation]) < 1e-6F,
              "Interpolated Get samples nearest, linear, Hermite, Lagrange and B-spline values on Get");
    }
    graph.setValue(index, -1.0F);
    check(graph.inspectOutput(lookup, 1).triggers == expected.size(), "Updating Index does not send Done");
    graph.fireTrigger(pulse);
    check(std::get<float>(graph.readValue(lookup)) == 0, "Out-of-range indices read as the element default");

    const int size = graph.addNode(Int, 0, 240, std::int32_t{4});
    const int smooth = graph.addNode(Boolean, 0, 300, false);
    const int fit = graph.addNode(FloatArrayResample, 200, 240);
    graph.setValue(mode, std::int32_t{1});
    graph.setValue(source, std::string{"0\n12"});
    graph.connect({source, fit, 0});
    graph.connect({size, fit, 1});
    graph.connect({mode, fit, 2});
    graph.connect({smooth, fit, 3});
    check(std::get<FloatArray>(graph.readValue(fit)) == FloatArray{0, 4, 8, 12},
          "Open linear resampling preserves both endpoints");
    graph.setValue(smooth, true);
    check(std::get<FloatArray>(graph.readValue(fit)) == FloatArray{0, 6, 12, 6},
          "Smth interpolates across the periodic boundary");
    graph.setValue(size, std::int32_t{1});
    check(std::get<FloatArray>(graph.readValue(fit)) == FloatArray{0}, "A one-element resample is defined");
    graph.setValue(size, std::int32_t{-1});
    check(std::get<FloatArray>(graph.readValue(fit)).empty(), "A negative resample size produces an empty array");
}

void arrayLoopAndSplitterPreserveTypesAndIndices()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    Schematic graph;
    const int text = graph.addNode(MultilineString, 0, 0, std::string{"cat\ndog\nbird"});
    const int reverse = graph.addNode(Boolean, 0, 60, false);
    const int trigger = graph.addNode(Trigger, 0, 120);
    const int capacity = graph.addNode(Int, 0, 180, std::int32_t{6});
    const int loop = graph.addNode(ArrayLoop, 200, 0);
    const int seen = graph.addNode(ArrayHistory, 400, 0);
    const int indices = graph.addNode(ArrayHistory, 400, 160);
    graph.connect({text, loop, 0});
    graph.connect({reverse, loop, 1});
    graph.connect({trigger, loop, 3}, LinkNotification::Silent);
    for (const auto [history, output] : {std::pair{seen, 0}, {indices, 1}}) {
        graph.connect({capacity, history, 0});
        graph.connect({loop, history, 1, output});
        graph.connect({loop, history, 2, 2}, LinkNotification::Silent);
    }
    graph.fireTrigger(trigger);
    check(std::get<StringArray>(graph.readValue(seen)) == StringArray{"bird", "dog", "cat"} &&
          std::get<IntArray>(graph.readValue(indices)) == IntArray{2, 1, 0},
          "Array Loop makes the String value and original index available on each Step");
    graph.setValue(reverse, true);
    graph.fireTrigger(trigger);
    check(std::get<StringArray>(graph.readValue(seen)) == StringArray{"cat", "dog", "bird", "bird", "dog", "cat"} &&
          std::get<IntArray>(graph.readValue(indices)) == IntArray{0, 1, 2, 2, 1, 0} &&
          graph.inspectOutput(loop, 3).triggers == 2, "Reverse traversal preserves original indices and sends Finished once");

    const int split = graph.addNode(ArraySplitter, 200, 250);
    const int receiver = graph.addNode(Int, 400, 320, std::int32_t{0});
    graph.setTemplateType(split, Type::IntArray);
    graph.connect({text, split, 0});
    check(graph.arraySplitterOutputCount(split) == 3, "Splitter exposes one output for each available element");
    std::string numbers;
    for (int i = 0; i < 52; ++i) {
        if (i != 0) numbers += '\n';
        numbers += std::to_string(i);
    }
    graph.setValue(text, numbers);
    check(graph.arraySplitterOutputCount(split) == 50 && std::get<std::int32_t>(graph.readValue(split, 49)) == 49,
          "Splitter exposes the first 50 entries without changing their indices");
    graph.connect({split, receiver, 0, 49});
    check(integer(graph, receiver) == 49, "The last supported output can be wired");
    graph.setValue(text, std::string{"5"});
    check(graph.arraySplitterOutputCount(split) == 50 && integer(graph, receiver) == 0,
          "Shrinking the source retains connected pins and delivers the element default");
    graph.disconnectInput(receiver, 0);
    check(graph.arraySplitterOutputCount(split) == 1, "Only one pin remains visible when the extra wire is removed");
}

void arrayDrawInterpolatesBetweenDragSamples()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    Schematic graph;
    const int size = graph.addNode(Int, 0, 0, std::int32_t{5});
    const int index = graph.addNode(Int, 0, 60, std::int32_t{0});
    const int value = graph.addNode(Float, 0, 120, 0.0F);
    const int drag = graph.addNode(Boolean, 0, 180, true);
    const int set = graph.addNode(Trigger, 0, 240);
    const int draw = graph.addNode(FloatArrayDraw, 200, 0);
    graph.connect({size, draw, 0});
    graph.connect({index, draw, 1});
    graph.connect({value, draw, 2});
    graph.connect({drag, draw, 3});
    graph.connect({set, draw, 4}, LinkNotification::Silent);
    graph.fireTrigger(set);
    graph.setValue(index, std::int32_t{4});
    graph.setValue(value, 8.0F);
    graph.fireTrigger(set);
    const auto read = [&] { return std::get<FloatArray>(graph.readValue(draw)); };
    check(read() == FloatArray{0, 2, 4, 6, 8}, "Dragging fills skipped samples using a straight segment");
    graph.setValue(index, std::int32_t{2});
    graph.setValue(value, 2.0F);
    graph.fireTrigger(set);
    check(read() == FloatArray{0, 2, 2, 5, 8}, "A backward drag interpolates the same way");
    graph.setValue(drag, false);
    graph.setValue(drag, true);
    graph.setValue(index, std::int32_t{0});
    graph.setValue(value, 10.0F);
    graph.fireTrigger(set);
    check(read() == FloatArray{10, 2, 2, 5, 8}, "A new drag begins a fresh segment");
    const int replacement = graph.addNode(MultilineString, 0, 300, std::string{"3\n1"});
    graph.connect({replacement, draw, 5});
    check(read() == FloatArray{3, 1}, "Assign replaces the array");
    graph.setValue(index, std::int32_t{4});
    graph.fireTrigger(set);
    check(read() == FloatArray{3, 1}, "Setting an out-of-bounds index leaves the assigned array intact");
    graph.setValue(size, std::int32_t{3});
    check(read() == FloatArray{3, 1, 0}, "Changing N resizes the array while preserving its existing prefix");
}

void arrayExtremaNormalizationAndCoordinateConversions()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    Schematic graph;
    const int x = graph.addNode(MultilineString, 0, 0, std::string{"-4\n2\n2"});
    const int y = graph.addNode(MultilineString, 0, 100, std::string{"1\n3"});
    const int min = graph.addNode(MinFloatArray, 180, 0);
    const int max = graph.addNode(MaxFloatArray, 180, 100);
    const int norm = graph.addNode(ArrayNorm, 180, 200);
    const int points = graph.addNode(FloatArrayToPointArray, 180, 300);
    const int axes = graph.addNode(PointArrayToFloatArray, 400, 300);
    for (const int id : {min, max, norm, points}) graph.connect({x, id, 0});
    graph.connect({y, points, 1});
    graph.connect({points, axes, 0});
    check(std::get<float>(graph.readValue(min)) == -4 && std::get<std::int32_t>(graph.readValue(min, 1)) == 0 &&
          std::get<float>(graph.readValue(max)) == 2 && std::get<std::int32_t>(graph.readValue(max, 1)) == 1,
          "Extrema return the first index of a tied value");
    check(std::get<FloatArray>(graph.readValue(norm)) == FloatArray{-1, 0.5F, 0.5F},
          "Norm scales by absolute peak and preserves signs");
    check(std::get<PointArray>(graph.readValue(points)) == PointArray{{-4, 1}, {2, 3}, {2, 0}} &&
          std::get<FloatArray>(graph.readValue(axes, 0)) == FloatArray{-4, 2, 2} &&
          std::get<FloatArray>(graph.readValue(axes, 1)) == FloatArray{1, 3, 0},
          "Point conversions combine and extract axes with zero-fill for missing coordinates");
    graph.setValue(x, std::string{});
    check(std::get<std::int32_t>(graph.readValue(min, 1)) == -1 && std::get<FloatArray>(graph.readValue(norm)).empty(),
          "Empty input has no extremum and normalizes to an empty array");
}

void arrayReductionsAndSortUseGreenTypes()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    Schematic graph;
    const int source = graph.addNode(MultilineString, 0, 0, std::string{"3\n-2\n1"});
    const int sort = graph.addNode(SortArray, 200, 0);
    const int sum = graph.addNode(ArraySum, 420, 0);
    const int count = graph.addNode(ArrayLength, 420, 100);
    const int textCount = graph.addNode(ArrayLength, 420, 200);
    graph.setTemplateType(sort, Type::IntArray);
    graph.connect({source, sort, 0});
    graph.connect({sort, sum, 0});
    graph.connect({sort, count, 0});
    graph.connect({source, textCount, 0});
    check(std::get<IntArray>(graph.readValue(sort)) == IntArray{-2, 1, 3} &&
          integer(graph, sum) == 2 && integer(graph, count) == 3,
          "Sort, Sum and Length infer array types and compute from the same input");
    graph.setValue(source, std::string{"2147483647\n1"});
    check(integer(graph, sum) == std::numeric_limits<std::int32_t>::min(), "Int Array Sum wraps like Green Int addition");
    graph.setValue(source, std::string{"aé🎧"});
    check(integer(graph, textCount) == 3, "String Length counts characters consistently with String Extract");
}

void spectralArrayToolsPublishPairedResultsOnCalc()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    Schematic graph;
    const int real = graph.addNode(MultilineString, 0, 0, std::string{"1\n0\n-1\n0"});
    const int imaginary = graph.addNode(MultilineString, 0, 80, std::string{});
    const int pulse = graph.addNode(Trigger, 0, 160);
    const int fft = graph.addNode(ArrayFft, 200, 0);
    const int inverse = graph.addNode(ArrayInverseFft, 440, 0);
    const int polar = graph.addNode(RealImaginaryToMagnitudePhase, 200, 140);
    const int rectangular = graph.addNode(MagnitudePhaseToRealImaginary, 440, 140);
    for (const auto [from, to] : {std::pair{fft, inverse}, {polar, rectangular}}) {
        graph.connect({real, from, 0});
        graph.connect({imaginary, from, 1});
        graph.connect({pulse, from, 2}, LinkNotification::Silent);
        graph.connect({from, to, 0, 0});
        graph.connect({from, to, 1, 1});
        graph.connect({from, to, 2, 0}, LinkNotification::Silent);
    }
    const auto expect = [&](int node, int output, const FloatArray& values) {
        const auto actual = std::get<FloatArray>(graph.readValue(node, output));
        check(actual.size() == values.size(), "Spectral results preserve length");
        for (std::size_t i = 0; i < values.size(); ++i) {
            check(std::abs(actual[i] - values[i]) < 1e-5F, "Spectral result matches the expected coefficient");
        }
    };
    expect(fft, 0, {});
    graph.fireTrigger(pulse);
    expect(fft, 0, {0, 2, 0, 2});
    expect(fft, 1, {0, 0, 0, 0});
    expect(inverse, 0, {1, 0, -1, 0});
    graph.setValue(real, std::string{"0\n1\n0"});
    expect(fft, 0, {0, 2, 0, 2});
    graph.fireTrigger(pulse);
    expect(fft, 0, {1, -0.5F, -0.5F});
    expect(fft, 1, {0, -std::sqrt(3.0F) * 0.5F, std::sqrt(3.0F) * 0.5F});
    expect(inverse, 0, {0, 1, 0});

    graph.setValue(real, std::string{"-1\n0"});
    graph.setValue(imaginary, std::string{"0\n-2\n0"});
    graph.fireTrigger(pulse);
    expect(polar, 0, {1, 2, 0});
    expect(polar, 1, {std::numbers::pi_v<float>, 1.5F * std::numbers::pi_v<float>, 0});
    expect(rectangular, 0, {-1, 0, 0});
    expect(rectangular, 1, {0, -2, 0});
    expect(inverse, 0, {-1, 0, 0});
    expect(inverse, 1, {0, -2, 0});
}

void stringPrimitivesReadCurrentValuesAndForwardTriggers()
{
    using enum PrimitiveKind;
    Schematic graph;
    const auto node = [&](PrimitiveKind kind) { return graph.addNode(kind, 0, 0); };
    const auto text = [&](int id, int output = 0) { return std::get<std::string>(graph.readValue(id, output)); };

    const int source = node(String);
    const int pos = node(Int);
    const int count = node(Int);
    const int needle = node(String);
    const int replacement = node(String);
    const int flag = node(Boolean);
    const int rev = node(Boolean);
    const int extract = node(StringExtract);
    const int split = node(StringSplit);
    const int find = node(StringFind);
    const int replace = node(StringReplace);
    const int overwrite = node(StringOverride);
    const int rightCounter = node(TriggerCounter);
    const int leftCounter = node(TriggerCounter);

    for (const int operation : {extract, split, find, replace, overwrite}) graph.connect({source, operation, 0});
    for (const int operation : {extract, split, overwrite}) graph.connect({pos, operation, 1});
    for (const int operation : {find, replace}) graph.connect({needle, operation, 1});
    graph.connect({count, extract, 2});
    graph.connect({flag, find, 2});
    graph.connect({rev, find, 3});
    graph.connect({replacement, replace, 2});
    graph.connect({flag, replace, 3});
    graph.connect({replacement, overwrite, 2});
    graph.connect({split, leftCounter, 0}, LinkNotification::Silent);
    graph.connect({split, rightCounter, 0, 1}, LinkNotification::Silent);

    graph.setValue(source, std::string{"FlowStone"});
    graph.setValue(pos, std::int32_t{4});
    graph.setValue(count, std::int32_t{5});
    check(text(extract) == "Stone" && text(split) == "Flow" && text(split, 1) == "Stone",
          "The documented zero-based split and extract examples work");
    check(integer(graph, leftCounter) == 2 && integer(graph, rightCounter) == 2,
          "Both split outputs forward changes from the source string and position");

    graph.setValue(source, std::string{"é😀aBa"});
    graph.setValue(pos, std::int32_t{1});
    graph.setValue(count, std::int32_t{1});
    check(text(extract) == "😀" && text(split) == "é" && text(split, 1) == "😀aBa",
          "String positions count UTF-8 code points");
    graph.setValue(replacement, std::string{"ø"});
    check(text(overwrite) == "éøaBa", "Override replaces the same number of code points as the replacement");
    graph.setValue(pos, std::int32_t{99});
    check(text(extract).empty() && text(split) == "é😀aBa" && text(split, 1).empty(),
          "Out-of-range positions clamp at the end");

    graph.setValue(source, std::string{"éALPHA-beta-alpha"});
    graph.setValue(needle, std::string{"alpha"});
    check(integer(graph, find) == 1, "Find folds ASCII and returns a character index");
    graph.setValue(rev, true);
    check(integer(graph, find) == 12, "Reverse String Find reports an index from the start");
    graph.setValue(rev, false);
    graph.setValue(flag, true);
    check(integer(graph, find) == 12, "Case-sensitive Find skips the uppercase match");
    graph.setValue(needle, std::string{"missing"});
    check(integer(graph, find) == -1, "Find returns minus one when there is no match");

    graph.setValue(source, std::string{"aba"});
    graph.setValue(needle, std::string{"a"});
    graph.setValue(replacement, std::string{"aa"});
    check(text(replace) == "aaba", "Once replaces only the first occurrence");
    graph.setValue(flag, false);
    check(text(replace) == "aabaa", "Replace all searches the original text even if the replacement contains the needle");
    graph.setValue(needle, std::string{});
    check(text(replace) == "aba", "An empty needle leaves the source intact");
}

void stringArrayPrimitivesSplitJoinAndFind()
{
    using enum PrimitiveKind;
    using openstone::green::StringArray;
    Schematic graph;
    const auto node = [&](PrimitiveKind kind) { return graph.addNode(kind, 0, 0); };
    const int source = node(String);
    const int sep = node(String);
    const int width = node(Int);
    const int pos = node(Int);
    const int needle = node(String);
    const int rev = node(Boolean);
    const int tokenize = node(StringToStringArray);
    const int split = node(StringArraySplit);
    const int join = node(StringArrayToString);
    const int find = node(StringArrayFind);
    const int special = node(SpecialCharacters);

    graph.connect({source, tokenize, 0});
    graph.connect({sep, tokenize, 1});
    graph.connect({width, tokenize, 2});
    graph.connect({tokenize, split, 0});
    graph.connect({pos, split, 1});
    graph.connect({tokenize, join, 0});
    graph.connect({tokenize, find, 0});
    graph.connect({needle, find, 1});
    graph.connect({rev, find, 3});
    graph.connect({pos, special, 0});

    const auto array = [&](int id, int output = 0) { return std::get<StringArray>(graph.readValue(id, output)); };
    const auto text = [&](int id) { return std::get<std::string>(graph.readValue(id)); };
    graph.setValue(source, std::string{"aé😀"});
    check(array(tokenize) == StringArray{"a", "é", "😀"} && text(join) == "aé😀",
          "Unwired split and join options round-trip individual characters");
    graph.setValue(source, std::string{"a::::beta::GAMMA::"});
    graph.setValue(sep, std::string{"::"});
    graph.setValue(pos, std::int32_t{2});
    check(array(split) == StringArray{"a", ""} && array(split, 1) == StringArray{"beta", "GAMMA", ""},
          "A multi-character delimiter preserves empty entries and Array Split keeps the two sides in order");
    graph.connect({sep, join, 1});
    check(text(join) == "a::::beta::GAMMA::", "Join inserts the separator between every pair of entries");

    graph.setValue(needle, std::string{"gamma"});
    check(integer(graph, find) == 3, "Array Find matches a whole entry using ASCII case folding");
    graph.setValue(rev, true);
    check(integer(graph, find) == 1, "Reverse Array Find returns the offset from the end");
    graph.setValue(needle, std::string{"gam"});
    check(integer(graph, find) == -1, "An entry must match the whole search string");

    graph.setValue(width, std::int32_t{2});
    graph.setValue(source, std::string{"αβγ::xyz"});
    check(array(tokenize) == StringArray{"αβ", "γ", "xy", "z"},
          "Fixed width splits each delimited field without splitting a UTF-8 code point");
    graph.setValue(sep, std::string{});
    graph.setValue(source, std::string{"1011011101"});
    graph.setValue(width, std::int32_t{4});
    check(array(tokenize) == StringArray{"1011", "0111", "01"}, "Fixed widths retain a shorter final row");

    for (const auto [index, character] : std::array<std::pair<int, const char*>, 5>{
             {{0, "\r\n"}, {1, "\n"}, {2, "\r"}, {3, "\t"}, {4, ""}}}) {
        graph.setValue(pos, std::int32_t{index});
        check(text(special) == character, "Special Characters maps indices to their control strings");
    }
}

void stringConversionsPreserveCharacterCodesAndBytes()
{
    using enum PrimitiveKind;
    Schematic graph;
    const auto add = [&](PrimitiveKind kind) { return graph.addNode(kind, 0, 0); };
    const auto text = [&](int id) { return std::get<std::string>(graph.readValue(id)); };

    const int number = add(Int);
    const int character = add(AsciiToString);
    const int code = add(StringToAscii);
    const int intHex = add(IntToHex);
    const int hexInt = add(HexToInt);
    graph.connect({number, character, 0});
    graph.connect({character, code, 0});
    graph.connect({number, intHex, 0});
    graph.connect({intHex, hexInt, 0});
    graph.setValue(number, std::int32_t{65});
    check(text(character) == "A" && integer(graph, code) == 65, "ASCII value 65 is A");
    graph.setValue(number, std::int32_t{256 + 233});
    check(text(character) == "é" && integer(graph, code) == 233, "The character code wraps as an ISO Latin-1 byte");
    graph.setValue(number, std::int32_t{-1});
    check(text(character) == "ÿ" && integer(graph, code) == 255 &&
          text(intHex) == "FFFFFFFF" && integer(graph, hexInt) == -1,
          "Negative values wrap and Int to Hex preserves their 32-bit representation");
    graph.setValue(number, std::numeric_limits<std::int32_t>::min());
    check(text(intHex) == "80000000" && integer(graph, hexInt) == std::numeric_limits<std::int32_t>::min(),
          "The sign bit survives an integer hexadecimal round trip");

    const int source = add(String);
    const int fromHex = add(HexToString);
    const int toHex = add(StringToHex);
    const int fromBits = add(BinaryToHex);
    const int toBits = add(HexToBinary);
    graph.connect({source, fromHex, 0});
    graph.connect({fromHex, toHex, 0});
    graph.connect({source, toBits, 0});
    graph.connect({toBits, fromBits, 0});
    graph.setValue(source, std::string{"2f bb"});
    check(text(toBits) == "0010111110111011" && text(fromBits) == "2FBB", "Binary conversion preserves byte boundaries");
    graph.setValue(source, std::string{"4100FF"});
    check(text(fromHex) == std::string("A\0\xFF", 3) && text(toHex) == "4100FF",
          "Hex string conversion round-trips null bytes and high bytes");
    graph.setValue(source, std::string{"3"});
    bool rejected = false;
    try {
        (void)text(fromHex);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    graph.setValue(source, std::string{"68656c6c6f"});
    check(rejected && text(fromHex) == "hello", "An incomplete byte is rejected and a later valid update still works");
    graph.connect({source, hexInt, 0});
    graph.disconnectLink({intHex, hexInt, 0});
    graph.setValue(source, std::string{"  0x7FFFFFFF  "});
    check(integer(graph, hexInt) == std::numeric_limits<std::int32_t>::max(), "Hex to Int accepts a conventional prefix");
}

void stringCharsetConvertsNamesAndCodePages()
{
    using enum PrimitiveKind;
    Schematic graph;
    const int source = graph.addNode(String, 0, 0, std::string{"café €"});
    const int codePage = graph.addNode(Int, 0, 0, std::int32_t{1252});
    const int utf8 = graph.addNode(String, 0, 0, std::string{" UTF-8 "});
    const int charset = graph.addNode(StringCharset, 0, 0);
    const int back = graph.addNode(StringCharset, 0, 0);
    const int hex = graph.addNode(StringToHex, 0, 0);
    graph.connect({source, charset, 0});
    graph.connect({utf8, charset, 1});
    graph.connect({codePage, charset, 2});
    graph.connect({charset, hex, 0});
    graph.connect({charset, back, 0});
    graph.connect({codePage, back, 1});
    graph.connect({utf8, back, 2});
    const auto text = [&](int id) { return std::get<std::string>(graph.readValue(id)); };

    check(text(hex) == "636166E92080" && text(back) == "café €",
          "Windows-1252 selected by code page ID survives a round trip to UTF-8");
    graph.setValue(codePage, std::int32_t{1200});
    graph.setValue(source, std::string("A\0é", 4));
    check(text(hex) == "41000000E900" && text(back) == std::string("A\0é", 4),
          "UTF-16LE preserves embedded zeros and converts independently of host byte order");
    graph.setValue(codePage, std::int32_t{28591});
    std::string longText;
    for (int i = 0; i < 2500; ++i) longText += "é";
    graph.setValue(source, longText);
    check(text(back) == longText, "Character set conversion continues across output buffers");

    graph.setValue(source, std::string("\xFF", 1));
    bool rejected = false;
    try {
        (void)text(back);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    graph.setValue(source, std::string{"é"});
    check(rejected && text(back) == "é", "Invalid encoding is reported and a subsequent conversion works");
}

void formatStringUsesOneNumericInputAndPropagatesTriggers()
{
    using enum PrimitiveKind;
    Schematic graph;
    const int value = graph.addNode(Int, 0, 0, std::int32_t{3});
    const int floating = graph.addNode(Float, 0, 0, 12.375F);
    const int format = graph.addNode(String, 0, 0, std::string{"ID %04d"});
    const int formatted = graph.addNode(FormatString, 0, 0);
    const int pulses = graph.addNode(TriggerCounter, 0, 0);
    graph.connect({value, formatted, 0});
    graph.connect({format, formatted, 1});
    graph.connect({formatted, pulses, 0}, LinkNotification::Silent);
    const auto text = [&] { return std::get<std::string>(graph.readValue(formatted)); };
    check(text() == "ID 0003", "Format an Int with padding and a literal prefix");
    graph.setValue(value, std::int32_t{-1});
    graph.setValue(format, std::string{"0x%08X"});
    check(text() == "0xFFFFFFFF" && integer(graph, pulses) == 2, "The inputs each send a trigger and unsigned fields preserve bits");
    graph.disconnectLink({value, formatted, 0});
    graph.connect({floating, formatted, 0});
    graph.setValue(format, std::string{"%.2f %% load"});
    check(text() == "12.38 % load", "Format a Float with precision and an escaped percent");
    graph.setValue(format, std::string{"%+.1lf"});
    check(text() == "+12.4", "The C floating l modifier and explicit sign are supported");
    for (const std::string invalid : {"%", "%s", "%*f", "%d %d", "%.2c"}) {
        graph.setValue(format, invalid);
        bool rejected = false;
        try {
            (void)text();
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        check(rejected, "Malformed or incompatible format specifications are rejected");
    }
    graph.setValue(format, std::string{});
    check(text() == "12.375", "An empty format passes through the numeric string after a format error");
}

void textLoadPublishesOnlyAfterReadingTheFile()
{
    using enum PrimitiveKind;
    using openstone::green::Type;

    constexpr std::string_view text = "café\r\nsecond line\n";
    int reads = 0;
    Schematic graph({}, {}, {}, {.load = [&](std::string_view path) -> std::string {
        ++reads;
        if (path == "notes.txt") return std::string(text);
        if (path == "empty.txt") return {};
        throw std::runtime_error("File does not exist");
    }});
    const int path = graph.addNode(String, 0, 0, std::string{"notes.txt"});
    const int load = graph.addNode(TextLoad, 0, 0);
    const int trigger = graph.addNode(Trigger, 0, 0);
    const int textPulses = graph.addNode(TriggerCounter, 0, 0);
    const int loadedPulses = graph.addNode(TriggerCounter, 0, 0);
    const int sampled = graph.addNode(SampleAndHold, 0, 0);
    check(graph.inputType(load, 0) == Type::String && graph.inputType(load, 1) == Type::Trigger &&
          graph.outputType(load, 0) == Type::String && graph.outputType(load, 1) == Type::Trigger,
          "Text Load exposes a String and Trigger on each side");
    graph.connect({path, load, 0});
    graph.connect({load, textPulses, 0}, LinkNotification::Silent);
    graph.connect({load, loadedPulses, 0, 1}, LinkNotification::Silent);
    graph.connect({load, sampled, 0}, LinkNotification::Silent);
    graph.connect({load, sampled, 1, 1}, LinkNotification::Silent);
    graph.connect({trigger, load, 1}, LinkNotification::Silent);

    const auto loaded = [&] { return std::get<std::string>(graph.readValue(load)); };
    check(reads == 0 && loaded().empty(), "Setting Path leaves Text empty until Load fires");
    graph.fireTrigger(trigger);
    check(reads == 1 && loaded() == text && std::get<std::string>(graph.readValue(sampled)) == text &&
          integer(graph, textPulses) == 1 && integer(graph, loadedPulses) == 1,
          "A successful load publishes the text before Loaded fires");

    graph.setValue(path, std::string{"missing.txt"});
    check(reads == 1 && loaded() == text, "Changing Path keeps the last loaded text until the next Load");
    bool failed = false;
    try {
        graph.fireTrigger(trigger);
    } catch (const std::runtime_error&) {
        failed = true;
    }
    check(failed && reads == 2 && loaded() == text &&
          integer(graph, textPulses) == 1 && integer(graph, loadedPulses) == 1,
          "A failed load keeps the last successful text and fires neither output");

    graph.setValue(path, std::string{"empty.txt"});
    graph.fireTrigger(trigger);
    graph.fireTrigger(trigger);
    check(reads == 4 && loaded().empty() && integer(graph, textPulses) == 3 && integer(graph, loadedPulses) == 3,
          "Each successful Load fires once even if the file is empty or its content is unchanged");
}

void textSaveWritesTheCurrentPathAndTextOnSave()
{
    using enum PrimitiveKind;
    using openstone::green::Type;

    std::map<std::string, std::string, std::less<>> files{{"notes.txt", "old content"}};
    int writes = 0;
    Schematic graph({}, {}, {}, {.save = [&](std::string_view path, std::string_view text) {
        ++writes;
        if (path == "unwritable.txt") throw std::runtime_error("Cannot write file");
        files.insert_or_assign(std::string(path), std::string(text));
    }});
    const int text = graph.addNode(MultilineString, 0, 0, std::string{"draft"});
    const int path = graph.addNode(String, 0, 0, std::string{"notes.txt"});
    const int save = graph.addNode(TextSave, 0, 0);
    const int trigger = graph.addNode(Trigger, 0, 0);
    const int savedPulses = graph.addNode(TriggerCounter, 0, 0);
    check(graph.inputType(save, 0) == Type::String && graph.inputType(save, 1) == Type::String &&
          graph.inputType(save, 2) == Type::Trigger && graph.outputType(save, 0) == Type::Trigger,
          "Text Save takes Text and Path strings plus a Save trigger and returns a Saved trigger");
    graph.connect({text, save, 0});
    graph.connect({path, save, 1});
    graph.connect({save, savedPulses, 0}, LinkNotification::Silent);
    graph.connect({trigger, save, 2}, LinkNotification::Silent);

    const std::string contents{"café\r\n"};
    graph.setValue(text, contents);
    check(writes == 0 && files.at("notes.txt") == "old content" && integer(graph, savedPulses) == 0,
          "Updating Text and Path leaves the file and Saved trigger alone");
    graph.fireTrigger(trigger);
    check(writes == 1 && files.at("notes.txt") == contents && integer(graph, savedPulses) == 1,
          "Save replaces the file with the current text and fires Saved");

    graph.setValue(path, std::string{"unwritable.txt"});
    bool failed = false;
    try {
        graph.fireTrigger(trigger);
    } catch (const std::runtime_error&) {
        failed = true;
    }
    check(failed && writes == 2 && integer(graph, savedPulses) == 1, "A failed save never fires Saved");

    graph.setValue(path, std::string{"empty.txt"});
    graph.setValue(text, std::string{});
    graph.fireTrigger(trigger);
    check(writes == 3 && files.at("empty.txt").empty() && files.at("notes.txt") == contents &&
          integer(graph, savedPulses) == 2, "A later Save can create an empty file at a different path");
}

void systemFoldersUsesItsIntInputForTheCustomPath()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    Schematic graph({}, {}, {}, {}, {.folder = [](std::int32_t id) -> std::string {
        switch (id) {
        case -1: return "/fixture/OpenStone";
        case 0: return "/fixture/Desktop";
        case 5: return "/fixture/Documents";
        case 26: return "/fixture/Application Data";
        default: return {};
        }
    }});
    const int folders = graph.addNode(SystemFolders, 0, 0);
    const int id = graph.addNode(Int, 0, 0, std::int32_t{5});
    const int docsPulses = graph.addNode(TriggerCounter, 0, 0);
    const int dataPulses = graph.addNode(TriggerCounter, 0, 0);
    const int customPulses = graph.addNode(TriggerCounter, 0, 0);
    check(graph.inputType(folders, 0) == Type::Int &&
          graph.outputType(folders, 0) == Type::String && graph.outputType(folders, 1) == Type::String &&
          graph.outputType(folders, 2) == Type::String, "System Folders exposes an Int ID and three String paths");
    const auto path = [&](int output) { return std::get<std::string>(graph.readValue(folders, output)); };
    check(path(0) == "/fixture/Documents" && path(1) == "/fixture/Application Data" &&
          path(2) == "/fixture/Desktop", "System Folders resolves both standard paths and defaults Custom to ID 0");

    graph.connect({folders, docsPulses, 0, 0}, LinkNotification::Silent);
    graph.connect({folders, dataPulses, 0, 1}, LinkNotification::Silent);
    graph.connect({folders, customPulses, 0, 2}, LinkNotification::Silent);
    graph.connect({id, folders, 0});
    check(path(2) == path(0), "The Documents ID resolves to the Docs path");
    graph.setValue(id, std::int32_t{26});
    check(path(2) == path(1), "The AppData ID resolves to the AppData path");
    graph.setValue(id, std::int32_t{-1});
    check(path(2) == "/fixture/OpenStone", "ID -1 resolves to the application directory");
    graph.setValue(id, std::int32_t{999});
    check(path(2).empty() && integer(graph, customPulses) == 4 &&
          integer(graph, docsPulses) == 0 && integer(graph, dataPulses) == 0,
          "A missing folder gives an empty path and ID updates pulse Custom alone");
}

void systemFontsSharesOneSnapshotAcrossItsOutputs()
{
    using enum PrimitiveKind;
    using openstone::green::StringArray;
    using openstone::green::Type;
    StringArray installed{"Alpha Sans", "Café Serif"};
    int queries = 0;
    Schematic graph({}, {}, {}, {}, {.fonts = [&] {
        ++queries;
        return installed;
    }});
    const int fonts = graph.addNode(SystemFonts, 0, 0);
    const int update = graph.addNode(Trigger, 0, 0);
    const int arrayPulses = graph.addNode(TriggerCounter, 0, 0);
    const int textPulses = graph.addNode(TriggerCounter, 0, 0);
    const int countPulses = graph.addNode(TriggerCounter, 0, 0);
    check(graph.inputType(fonts, 0) == Type::Trigger && graph.outputType(fonts, 0) == Type::StringArray &&
          graph.outputType(fonts, 1) == Type::String && graph.outputType(fonts, 2) == Type::Int,
          "System Fonts exposes Update plus the font array, text and count");
    check(queries == 0, "Adding the primitive does not enumerate font families");
    const auto names = [&] { return std::get<StringArray>(graph.readValue(fonts, 0)); };
    const auto text = [&] { return std::get<std::string>(graph.readValue(fonts, 1)); };
    const auto count = [&] { return std::get<std::int32_t>(graph.readValue(fonts, 2)); };
    check(names() == installed && text() == "Alpha Sans\nCafé Serif" && count() == 2 && queries == 1,
          "All three outputs use one lazy snapshot and preserve the font names");

    graph.connect({fonts, arrayPulses, 0, 0}, LinkNotification::Silent);
    graph.connect({fonts, textPulses, 0, 1}, LinkNotification::Silent);
    graph.connect({fonts, countPulses, 0, 2}, LinkNotification::Silent);
    graph.connect({update, fonts, 0}, LinkNotification::Silent);
    installed = {"New Font"};
    check(count() == 2 && queries == 1, "Reading again reuses the cached list until Update");
    graph.fireTrigger(update);
    check(names() == installed && text() == "New Font" && count() == 1 && queries == 2 &&
          integer(graph, arrayPulses) == 1 && integer(graph, textPulses) == 1 && integer(graph, countPulses) == 1,
          "Update refreshes the list once and pulses all three outputs");

    installed.clear();
    graph.fireTrigger(update);
    check(names().empty() && text().empty() && count() == 0 && queries == 3 &&
          integer(graph, arrayPulses) == 2 && integer(graph, textPulses) == 2 && integer(graph, countPulses) == 2,
          "An empty refresh replaces the previous list and still pulses all outputs");
}

void memoryCreateReadWriteSectionAndCopy()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    Schematic graph;
    const auto link = [&](int from, int to, int input, int output = 0) {
        graph.connect({from, to, input, output}, LinkNotification::Silent);
    };
    const int create = graph.addNode(Trigger, 0, 0);
    const int erase = graph.addNode(Trigger, 0, 0);
    const int write = graph.addNode(Trigger, 0, 0);
    const int get = graph.addNode(Trigger, 0, 0);
    const int copy = graph.addNode(Trigger, 0, 0);
    const int size = graph.addNode(Int, 0, 0, std::int32_t{4});
    const int index = graph.addNode(Int, 0, 0, std::int32_t{1});
    const int byte = graph.addNode(Int, 0, 0, std::int32_t{255});
    const int stereo = graph.addNode(Boolean, 0, 0, false);
    const int storage = graph.addNode(MemCreate, 0, 0);
    const int writer = graph.addNode(MemWriteByte, 0, 0);
    const int reader = graph.addNode(MemReadByte, 0, 0);
    const int section = graph.addNode(MemSection, 0, 0);
    const int copier = graph.addNode(MemCopy, 0, 0);
    const int info = graph.addNode(MemInfo, 0, 0);
    link(size, storage, 0);
    link(create, storage, 1);
    link(erase, storage, 2);
    link(stereo, storage, 3);
    link(storage, writer, 0);
    link(index, writer, 1);
    link(byte, writer, 2);
    link(write, writer, 3);
    link(storage, reader, 0);
    link(index, reader, 1);
    link(storage, section, 0);
    link(index, section, 1);
    link(get, section, 3);
    link(storage, copier, 0);
    link(section, copier, 2);
    link(copy, copier, 3);
    link(storage, info, 0);

    const auto bytes = [&](int node) {
        const auto value = std::get<Memory>(graph.readValue(node));
        const auto view = value.bytes();
        return std::vector<std::uint8_t>(view.begin(), view.end());
    };
    check(graph.outputType(storage) == Type::Mem && graph.inputType(writer, 3) == Type::Trigger &&
          integer(graph, reader) == -9999, "An unallocated Mem reads as missing");
    graph.fireTrigger(create);
    check(bytes(storage) == std::vector<std::uint8_t>(4) && integer(graph, info, 2) == 4 &&
          integer(graph, info, 3) == 1, "Create publishes zeroed bytes and mono metadata");
    const auto mem = std::get<Memory>(graph.readValue(storage));
    const auto address = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(integer(graph, info, 0))) << 32) |
        static_cast<std::uint32_t>(integer(graph, info, 1));
    check(address == reinterpret_cast<std::uintptr_t>(mem.bytes().data()),
          "The two Mem Info Ints describe the address of the current buffer");
    graph.setValue(stereo, true);
    check(integer(graph, info, 3) == 2, "Stereo updates metadata on an existing buffer");
    graph.fireTrigger(write);
    check(integer(graph, reader) == 255 && std::get<Memory>(graph.readValue(section)).size() == 0,
          "Write updates the shared Mem and Section waits for Get");
    graph.fireTrigger(get);
    check(bytes(section) == std::vector<std::uint8_t>({255, 0, 0}), "Section copies the remaining bytes at a byte offset");
    graph.setValue(index, std::int32_t{-1});
    graph.fireTrigger(write);
    check(integer(graph, reader) == 9999 && bytes(storage) == std::vector<std::uint8_t>({0, 255, 0, 0}),
          "Negative byte indexes stay within the buffer");
    graph.setValue(index, std::int32_t{4});
    graph.fireTrigger(write);
    check(integer(graph, reader) == 9999, "A read at the end of Mem reports an invalid index");
    graph.fireTrigger(copy);
    check(bytes(storage) == std::vector<std::uint8_t>({255, 0, 0, 0}), "Copy mutates the destination");
    graph.fireTrigger(create);
    check(bytes(storage) == std::vector<std::uint8_t>(4) && bytes(section) == std::vector<std::uint8_t>({255, 0, 0}),
          "Create resets its buffer and leaves independent sections intact");
    graph.fireTrigger(erase);
    check(bytes(storage).empty() && integer(graph, info, 2) == 0, "Delete clears the Mem output");
}

void memoryArraysConvertSplitAndFeedDsp()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    openstone::Session session;
    auto& graph = session.graph();
    const auto link = [&](int from, int to, int input, int output = 0) {
        graph.connect({from, to, input, output}, LinkNotification::Silent);
    };
    const auto size = [&](std::int32_t value) { return graph.addNode(Int, 0, 0, value); };
    const int text = graph.addNode(String, 0, 0, std::string{"1.5\n-2\n3\n4\n5"});
    const int ints = graph.addNode(String, 0, 0, std::string{"16777217\n-2"});
    const int convert = graph.addNode(FloatArrayToFloatMem, 0, 0);
    const int intAsFloat = graph.addNode(IntArrayToFloatMem, 0, 0);
    const int intAsInt = graph.addNode(IntArrayToIntMem, 0, 0);
    const int minMax = graph.addNode(MemMinMax, 0, 0);
    const int split = graph.addNode(MemToMemArray, 0, 0);
    const int find = graph.addNode(MemArrayGetAt, 0, 0);
    const int toFloats = graph.addNode(MemToFloatArray, 0, 0);
    const int done = graph.addNode(TriggerCounter, 0, 0);
    const int get = graph.addNode(Trigger, 0, 0);
    const int index = size(1);
    link(text, convert, 0);
    link(ints, intAsFloat, 0);
    link(ints, intAsInt, 0);
    link(convert, minMax, 0);
    link(size(2), minMax, 1);
    link(convert, split, 0);
    link(size(2), split, 1);
    link(split, find, 0);
    link(index, find, 1);
    link(get, find, 2);
    link(find, done, 0, 1);
    link(find, toFloats, 0);
    link(index, toFloats, 1);
    link(size(1), toFloats, 2);
    link(find, toFloats, 3, 1);

    check(std::get<Memory>(graph.readValue(intAsFloat)).sample(0) == 16'777'216.0F &&
          std::get<Memory>(graph.readValue(intAsInt)).word(0) == 16'777'217U &&
          std::get<Memory>(graph.readValue(intAsInt)).word(1) == 0xfffffffeU,
          "Float Mem stores numeric conversions and Int Mem preserves all 32 bits");
    const auto leftMin = FloatArray{-2, 3, 5};
    const auto leftMax = FloatArray{1.5F, 4, 5};
    check(graph.readValue(minMax, 0) == Value{leftMin} && graph.readValue(minMax, 1) == Value{leftMax} &&
          graph.readValue(minMax, 2) == Value{leftMin} && graph.readValue(minMax, 3) == Value{leftMax} &&
          integer(graph, minMax, 4) == 3, "Min/Max keeps the partial group and duplicates mono extrema");
    check(std::get<MemoryArray>(graph.readValue(split)).entries().size() == 2 && graph.outputType(split) == Type::MemArray,
          "Mem to MemArray uses the requested float count and drops an incomplete group");
    graph.fireTrigger(get);
    check(graph.readValue(toFloats) == Value{FloatArray{4}} && integer(graph, done) == 1,
          "Get At publishes the selected Mem before firing Done");
    graph.setValue(index, std::int32_t{4});
    check(graph.readValue(toFloats) == Value{FloatArray{4}}, "Changing Index alone leaves the sampled output intact");
    graph.fireTrigger(get);
    check(graph.readValue(toFloats) == Value{FloatArray{}} && integer(graph, done) == 2,
          "An invalid index produces an empty Mem before Done");

    const int dsp = graph.addNode(DspCode, 0, 0);
    graph.setDspDraft(dsp, "memin buffer[4]; monoout result; result = buffer[0] - buffer[1] + buffer[3];");
    session.compileDsp(dsp);
    check(graph.inputType(dsp, 0) == Type::Mem, "Compile a DSP block with a Mem connector");
    link(convert, dsp, 0);
    check(graph.captureDsp(dsp, 5, 2).front() == std::vector<double>(5, 7.5),
          "DSP Code reads the same Mem samples from each render block");
    const int assembly = graph.addNode(Assembler, 0, 0);
    graph.setDspDraft(assembly, R"asm(
        memin buffer[4];
        monoout result;
        mov eax,12;
        fld buffer[0];
        fld buffer[1];
        fsub;
        fld buffer[eax];
        fadd;
        fstp result[0];
    )asm");
    session.compileDsp(assembly);
    check(graph.inputType(assembly, 0) == Type::Mem, "Compile an Assembler node with a Mem connector");
    link(convert, assembly, 0);
    check(graph.captureDsp(assembly, 5, 2).front() == std::vector<double>(5, 7.5),
          "Assembler reads fixed and register-indexed samples from the connected Mem");
    graph.setValue(text, std::string{"10\n2"});
    check(graph.captureDsp(dsp, 3, 2).front() == std::vector<double>(3, 8.0),
          "A changed source reaches the next DSP capture and the short buffer is zero padded");
    check(graph.captureDsp(assembly, 3, 2).front() == std::vector<double>(3, 8.0),
          "Assembler reads the updated Mem in the next capture");
    graph.disconnectInput(convert, 0);
    check(graph.captureDsp(dsp, 3, 2).front() == std::vector<double>(3, 0.0),
          "Disconnecting the converter clears the derived buffer");
}

void waveArraysConvertChannelsAndGenerateMipmaps()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    Schematic graph;
    const auto link = [&](int from, int to, int input, int output = 0) {
        graph.connect({from, to, input, output}, LinkNotification::Silent);
    };
    const int text = graph.addNode(String, 0, 0, std::string{"1\n-3\n5\n7\n-2\n8\n1\n2"});
    const int convert = graph.addNode(FloatArrayToFloatMem, 0, 0);
    const int storage = graph.addNode(MemCreate, 0, 0);
    const int copy = graph.addNode(MemCopy, 0, 0);
    const int minMax = graph.addNode(MemMinMax, 0, 0);
    const int wave = graph.addNode(MemArraySetAt, 0, 0);
    const int mipmap = graph.addNode(MemToMipmap, 0, 0);
    const int pulse = graph.addNode(Trigger, 0, 0);
    const int add = graph.addNode(Trigger, 0, 0);
    const int del = graph.addNode(Trigger, 0, 0);
    const int mono = graph.addNode(Boolean, 0, 0, false);
    const int stereo = graph.addNode(Boolean, 0, 0, true);
    const int fft = graph.addNode(Boolean, 0, 0, true);
    const int frames = graph.addNode(Int, 0, 0, std::int32_t{8});
    const int count = graph.addNode(Int, 0, 0, std::int32_t{2});
    const int storageBytes = graph.addNode(Int, 0, 0, std::int32_t{32});
    const int perOct = graph.addNode(Float, 0, 0, 1.0F);
    const int cutoff = graph.addNode(Float, 0, 0, 10'000.0F);
    const int stack = graph.addNode(Int, 0, 0, std::int32_t{1});
    link(text, convert, 0);
    link(storageBytes, storage, 0);
    link(pulse, storage, 1);
    link(stereo, storage, 3);
    link(storage, copy, 0);
    link(convert, copy, 2);
    link(pulse, copy, 3);
    link(storage, minMax, 0);
    link(count, minMax, 1);
    link(count, wave, 0);
    link(pulse, wave, 1);
    link(del, wave, 2);
    link(storage, wave, 4);
    link(add, wave, 5);
    link(mono, wave, 6);
    link(convert, mipmap, 0);
    link(frames, mipmap, 1);
    link(cutoff, mipmap, 2);
    link(perOct, mipmap, 3);
    link(stack, mipmap, 4);
    link(fft, mipmap, 5);

    graph.fireTrigger(pulse);
    check(graph.readValue(minMax, 0) == Value{FloatArray{1, -2}} &&
          graph.readValue(minMax, 3) == Value{FloatArray{7, 8}}, "Stereo Min/Max uses interleaved channel pairs");
    graph.fireTrigger(add);
    check(graph.readValue(wave, 1) == Value{IntArray{4, 0}} && integer(graph, wave, 2) == 2,
          "Wave Array Set resizes first and copies the wave with its sample count");
    graph.setValue(mono, true);
    const auto combined = std::get<MemoryArray>(graph.readValue(wave)).at(0);
    check(combined.channels() == 1 && combined.sample(0) == -1 && combined.sample(1) == 6,
          "Mono mixes each stereo frame and updates the whole array");
    graph.fireTrigger(del);
    check(std::get<MemoryArray>(graph.readValue(wave)).at(0).size() == 0,
          "Delete empties the selected wave slot");

    auto output = std::get<MemoryArray>(graph.readValue(mipmap));
    check(integer(graph, mipmap, 1) == 2 && output.at(0).size() == 32 && output.at(1).size() == 32 &&
          std::abs(output.at(0).sample(0) - 1.0F) < 1e-6F, "Mipmap returns equal-sized band-limited tables");
    graph.setValue(fft, false);
    auto filtered = std::get<MemoryArray>(graph.readValue(mipmap));
    check(filtered.entries().size() == 2 && std::abs(filtered.at(0).sample(0) - output.at(0).sample(0)) > 1e-3F,
          "The stacked low-pass path honors the cutoff and differs from FFT only");
}

void timeSamplesAndHoldsTheLocalCalendarFields()
{
    using namespace std::chrono;
    using namespace std::chrono_literals;
    using enum PrimitiveKind;
    using openstone::green::Type;

    std::tm local{};
    local.tm_year = 2024 - 1900;
    local.tm_mon = 11;
    local.tm_mday = 31;
    local.tm_hour = 23;
    local.tm_min = 59;
    local.tm_sec = 59;
    local.tm_isdst = -1;
    const auto instant = std::mktime(&local);
    check(instant != std::time_t{-1}, "The local calendar fixture is representable");
    auto now = system_clock::from_time_t(instant) + 875ms;
    int samples = 0;
    Schematic graph({}, {}, {}, {}, {.wallClock = [&] {
        ++samples;
        return now;
    }});
    const int time = graph.addNode(Time, 0, 0);
    const int get = graph.addNode(Trigger, 0, 0);
    const int holdYear = graph.addNode(SampleAndHold, 0, 0);
    graph.connect({get, time, 0}, LinkNotification::Silent);
    graph.connect({time, holdYear, 0, 3}, LinkNotification::Silent);
    graph.connect({time, holdYear, 1, 0}, LinkNotification::Silent);
    const auto checkFields = [&](std::array<std::int32_t, 8> expected, std::uint64_t pulses) {
        const auto& spec = openstone::primitive(Time);
        check(spec.inputs.size() == 1 && graph.inputType(time, 0) == Type::Trigger && spec.outputs.size() == expected.size(),
              "Time exposes one Get trigger and eight outputs");
        for (int i = 0; i < static_cast<int>(expected.size()); ++i) {
            check(graph.outputType(time, i) == Type::Int && std::get<std::int32_t>(graph.readValue(time, i)) == expected[i] &&
                  graph.inspectOutput(time, i).triggers == pulses, "Each Int output uses the same calendar sample");
        }
    };

    checkFields({2, 31, 12, 2024, 23, 59, 59, 875}, 0);
    now += 125ms;
    checkFields({2, 31, 12, 2024, 23, 59, 59, 875}, 0);
    graph.fireTrigger(get);
    checkFields({3, 1, 1, 2025, 0, 0, 0, 0}, 1);
    check(samples == 2 && integer(graph, holdYear) == 2025,
          "Get samples once and publishes the entire new date before sending the first output trigger");
    now += 4 * days{1} + 321ms;
    graph.fireTrigger(get);
    checkFields({0, 5, 1, 2025, 0, 0, 0, 321}, 2);
    check(samples == 3, "A Sunday is weekday zero and January is month one");
}

void timerRepeatsAndReschedulesOnGreenTriggers()
{
    using namespace std::chrono_literals;
    using enum PrimitiveKind;
    using openstone::green::Type;
    using openstone::TickTime;
    using openstone::TickWake;
    TickTime now{};
    Schematic graph({}, {}, {}, {}, {.monotonicClock = [&] { return now; }});
    const int timer = graph.addNode(CustomTimer, 0, 0);
    const int duration = graph.addNode(Int, 0, 0, std::int32_t{250});
    const int start = graph.addNode(Trigger, 0, 0);
    const int stop = graph.addNode(Trigger, 0, 0);
    const int count = graph.addNode(TriggerCounter, 0, 0);
    check(graph.inputType(timer, 0) == Type::Int && graph.inputType(timer, 1) == Type::Trigger &&
          graph.inputType(timer, 2) == Type::Trigger && graph.outputType(timer) == Type::Trigger,
          "Timer has a millisecond Int, Start and Stop triggers, and a Trigger output");
    graph.connect({duration, timer, 0});
    graph.connect({start, timer, 1}, LinkNotification::Silent);
    graph.connect({stop, timer, 2}, LinkNotification::Silent);
    graph.connect({timer, count, 0}, LinkNotification::Silent);
    const auto advance = [&](std::chrono::milliseconds time, TickWake wake = TickWake::Timer) {
        now = TickTime{} + time;
        auto result = graph.advanceTicks(now, wake);
        check(!result.error, "Timer dispatch should succeed");
        return result;
    };
    const auto deadlineIs = [](const auto& schedule, std::chrono::milliseconds expected) {
        return schedule.deadline && std::abs((*schedule.deadline - (TickTime{} + expected)).count()) < 1e-9;
    };
    check(!advance(0ms, TickWake::Configure).deadline, "A wired Duration does not start a stopped Timer");
    graph.fireTrigger(start);
    check(deadlineIs(advance(10ms, TickWake::Configure), 250ms) && !advance(249ms).fired,
          "The first interval starts at Start and waits the entire duration");
    check(!advance(250ms, TickWake::Display).fired && advance(250ms).fired && integer(graph, count) == 1,
          "Only a timer wake delivers the elapsed pulse");
    check(deadlineIs(advance(875ms), 1000ms) && integer(graph, count) == 2,
          "A late wake delivers one pulse and retains the original phase");

    graph.setValue(duration, std::int32_t{125});
    check(deadlineIs(advance(900ms, TickWake::Configure), 1000ms), "A Duration change re-arms the running timer");
    advance(950ms, TickWake::Configure);
    graph.fireTrigger(start);
    check(!advance(1000ms).fired && advance(1076ms).fired && integer(graph, count) == 3,
          "A repeated Start resets the phase without creating an extra timer");
    graph.setValue(duration, std::int32_t{-1});
    check(!advance(1250ms).deadline && integer(graph, count) == 3, "A nonpositive duration leaves the timer idle");
    graph.setValue(duration, std::int32_t{125});
    check(deadlineIs(advance(1300ms, TickWake::Configure), 1375ms) && advance(1375ms).fired,
          "A positive interval resumes a timer whose Start state was held");
    graph.fireTrigger(stop);
    graph.setValue(duration, std::int32_t{250});
    check(!advance(2000ms).deadline && integer(graph, count) == 4, "Stop cancels future pulses until Start");

    graph.connect({timer, timer, 2}, LinkNotification::Silent);
    graph.fireTrigger(start);
    const auto shot = advance(2250ms);
    check(shot.fired && !shot.deadline && integer(graph, count) == 5 && !advance(3000ms).fired,
          "Elapsed wired back to Stop makes the timer a one-shot");
}

void editHistoryReplaysMovesAndLinksAsGestures()
{
    using enum PrimitiveKind;
    using openstone::SchematicEdit;
    using openstone::HistoryUpdate;
    using openstone::NodePosition;
    openstone::Session session;
    auto update = HistoryUpdate::Positions;
    const auto recordUpdate = [&](HistoryUpdate value) { update = value; };
    const int a = session.addNode(String, 0, 0);
    const int b = session.addNode(String, 0, 100);
    const int target = session.addNode(String, 300, 0);
    const int companion = session.addNode(Int, 40, 250);
    session.setValue(a, std::string{"a"});
    session.setValue(b, std::string{"b"});
    const SchematicLink first{a, target, 0};
    const SchematicLink second{b, target, 0};
    session.connect(first);
    const auto combined = [&] { return std::get<std::string>(session.graph().readValue(target)); };

    session.beginEdit(SchematicEdit::MoveNodes);
    session.moveNodes(std::array{NodePosition{b, 20, 50}, NodePosition{companion, 60, 200}});
    session.connect(second); // Auto-link after the pause, before the mouse is released.
    session.moveNodes(std::array{NodePosition{b, 60, 20}, NodePosition{companion, 100, 170}});
    session.finishEdit();
    check(combined() == "ab", "The auto-link should participate in the original fan-in");
    check(session.applyHistory(false, recordUpdate) && update == HistoryUpdate::Topology &&
          findNode(session.graph(), b).x == 0 && findNode(session.graph(), b).y == 100 &&
          findNode(session.graph(), companion).x == 40 && findNode(session.graph(), companion).y == 250 &&
          session.graph().links() == std::vector{first} && !session.historyEdit(false),
          "One undo must restore the whole selection and the exact wires");
    session.setValue(a, std::string{"edited"});
    check(session.applyHistory(true, recordUpdate) && findNode(session.graph(), b).x == 60 &&
          findNode(session.graph(), b).y == 20 && combined() == "editedb",
          "Redo should restore the linking node and fan-in order and keep later value edits");
    check(findNode(session.graph(), companion).x == 100 && findNode(session.graph(), companion).y == 170,
          "Redo should restore the companion's position with the same offset");

    session.beginEdit(SchematicEdit::DeleteLinks);
    session.disconnectLink(first);
    session.disconnectLink(second);
    session.finishEdit();
    check(session.graph().links().empty(), "The sweep should cut both wires");
    check(session.applyHistory(false, recordUpdate) && combined() == "editedb",
          "One undo must restore all the wires cut by a sweep in their original order");

    session.beginEdit(SchematicEdit::MoveNode);
    session.moveNode(b, 61, 20);
    session.finishEdit();
    check(!session.historyEdit(true), "A new edit should discard the redo branch");
    const auto* originalGraph = &session.graph();
    check(session.applyHistory(false, recordUpdate) && update == HistoryUpdate::Positions &&
          &session.graph() == originalGraph && findNode(session.graph(), b).x == 60,
          "Undoing a position-only edit should keep the live runtime");
    session.beginEdit(SchematicEdit::MoveNode);
    session.finishEdit();
    check(session.historyEdit(true) == SchematicEdit::MoveNode, "Empty gestures should preserve redo");
}

void editHistoryRestoresGroupedBoundaryWires()
{
    using enum PrimitiveKind;
    using openstone::SchematicEdit;
    openstone::Session session;
    const int outer = session.addNode(Module, 0, 0);
    session.enterModule(outer);
    const int source = session.addNode(Int, -200, 0);
    const int inner = session.addNode(Int, 0, 0);
    const int leaf = session.addNode(Int, 160, 90);
    const int outside = session.addNode(Int, 360, 0);
    session.setValue(source, std::int32_t{17});
    const std::vector links{SchematicLink{source, inner, 0}, SchematicLink{inner, leaf, 0},
                            SchematicLink{leaf, outside, 0}};
    for (auto link : links) session.connect(link);

    session.beginEdit(SchematicEdit::MakeModule);
    const int wrapper = session.makeModule(std::array{inner, leaf});
    session.finishEdit();
    const auto wrapperLinks = session.graph().links();
    const auto childLinks = session.graph().module(wrapper)->links();
    session.enterModule(wrapper);
    session.renameNode(inner, "Renamed inside");

    const auto apply = [&](bool redo) { return session.applyHistory(redo, [](auto) {}); };
    check(apply(false) && session.activeLevel() == 1 && !session.graph().module(wrapper) &&
          session.graph().links() == links && session.graph().nodes()[1].id == inner &&
          findNode(session.graph(), leaf).x == 160 && findNode(session.graph(), leaf).y == 90 &&
          findNode(session.graph(), inner).label == "Renamed inside" && integer(session.graph(), outside) == 17,
          "Undo Make Module should restore the original nodes, order, positions and both crossing wires");
    check(apply(true) && session.graph().links() == wrapperLinks &&
          session.graph().module(wrapper)->links() == childLinks &&
          findNode(*session.graph().module(wrapper), inner).label == "Renamed inside" &&
          integer(session.graph(), outside) == 17,
          "Redo Make Module should reuse the wrapper and restore its generated connectors and interior wiring");
}

void editHistoryRestoresReorderedModuleInputs()
{
    using enum PrimitiveKind;
    openstone::Session session;
    const int module = session.addNode(Module, 200, 0);
    const int source = session.addNode(Int, 0, 0);
    session.enterModule(module);
    const int upper = session.addNode(ModuleInput, 0, 0);
    const int lower = session.addNode(ModuleInput, 0, 100);
    session.navigate(0);
    session.connect({source, module, 0});
    session.enterModule(module);

    session.beginEdit(openstone::SchematicEdit::MoveNode);
    session.moveNode(upper, 0, 200);
    session.finishEdit();
    const auto connectorOrder = [&](int first, int second, int externalPort) {
        const auto& parent = session.graphAt(0);
        const auto& ports = findNode(parent, module).moduleInputs;
        return ports[0].endpoint == first && ports[1].endpoint == second &&
               parent.links() == std::vector{SchematicLink{source, module, externalPort}};
    };
    check(connectorOrder(lower, upper, 1), "The moved connector keeps its outside wire");
    check(session.applyHistory(false, [](auto) {}) && connectorOrder(upper, lower, 0),
          "Undo restores the connector order and outside wire");
    check(session.applyHistory(true, [](auto) {}) && connectorOrder(lower, upper, 1),
          "Redo moves the connector and outside wire back together");
}

void copySelectionPreservesOnlyInternalWires()
{
    using enum PrimitiveKind;
    openstone::Session session;
    const int first = session.addNode(String, -100, 10);
    const int second = session.addNode(String, -20, -80);
    const int outside = session.addNode(String, -500, 10);
    const int joined = session.addNode(String, 130, 30);
    session.setValue(first, std::string{"A"});
    session.setValue(second, std::string{"B"});
    session.setValue(outside, std::string{"!"});
    session.connect({second, joined, 0});
    session.connect({first, joined, 0});
    session.connect({outside, joined, 0});

    const int relay = session.addNode(Module, -10, 180);
    session.enterModule(relay);
    const int source = session.addNode(Int, 0, 0);
    const int output = session.addNode(ModuleOutput, 100, 0);
    session.setValue(source, std::int32_t{9});
    session.connect({source, output, 0});
    session.navigate(0);
    const int monitor = session.addNode(Int, 100, 180);
    session.connect({relay, monitor, 0});
    const int destination = session.addNode(Module, 500, 0);
    const auto originalWires = session.graph().links();

    session.copyNodes(std::array{joined, relay, first, monitor, second});
    check(session.canPaste() && !session.canPasteSynchronized(),
          "A selection can be pasted as independent nodes");
    session.setValue(first, std::string{"Changed"});
    session.enterModule(destination);
    session.beginEdit(openstone::SchematicEdit::PasteNodes);
    const auto pasted = session.pasteNodes(400, 500, false);
    session.finishEdit();
    check(pasted.size() == 5, "Paste should clone all five nodes in schematic order");
    const std::vector copiedWires{SchematicLink{pasted[1], pasted[2], 0},
                                 SchematicLink{pasted[0], pasted[2], 0},
                                 SchematicLink{pasted[3], pasted[4], 0}};
    check(session.graph().links() == copiedWires && session.graphAt(0).links() == originalWires,
          "Paste should remap internal wires in their original order and leave external wiring alone");
    const std::array originals{first, second, joined, relay, monitor};
    for (std::size_t index = 0; index < originals.size(); ++index) {
        const auto& original = findNode(session.graphAt(0), originals[index]);
        const auto& clone = findNode(session.graph(), pasted[index]);
        check(clone.x == original.x + 500 && clone.y == original.y + 580,
              "Every pasted node should receive the same position offset");
    }
    const auto verifyValues = [&] {
        return session.graph().readValue(pasted[2]) == openstone::green::Value{std::string{"BA"}} &&
               integer(session.graph(), pasted[4]) == 9;
    };
    check(verifyValues(), "Copied values, fan-in order and module boundary signals should survive the paste");
    check(session.applyHistory(false, [](auto) {}) && session.graph().nodes().empty() && session.graph().links().empty() &&
          !session.historyEdit(false) && session.canPaste(),
          "One Undo should remove the whole paste and retain the clipboard");
    check(session.applyHistory(true, [](auto) {}) && session.graph().links() == copiedWires && verifyValues(),
          "One Redo should restore the pasted nodes, wires and nested module");
}

void cutSelectionRestoresOriginalWiring()
{
    using enum PrimitiveKind;
    openstone::Session session;
    const int outside = session.addNode(String, -320, 0);
    const int local = session.addNode(String, -240, 100);
    const int join = session.addNode(String, 0, 40);
    const int sink = session.addNode(String, 280, 40);
    session.setValue(outside, std::string{"!"});
    session.setValue(local, std::string{"A"});
    session.connect({local, join, 0});
    session.connect({outside, join, 0});
    session.connect({join, sink, 0});
    const int module = session.makeModule(std::array{join});
    const auto originalLinks = session.graph().links();
    const auto internal = std::ranges::find(originalLinks, local, &SchematicLink::source);
    check(internal != originalLinks.end(), "The module should retain the selected node's connection");
    const auto childLinks = session.graph().module(module)->links();
    const std::array selection{module, local};

    session.beginEdit(openstone::SchematicEdit::CutNodes);
    session.cutNodes(selection);
    session.finishEdit();
    const auto checkCut = [&] {
        return session.graph().nodes().size() == 2 && session.graph().links().empty() &&
               session.canPaste() && findNode(session.graph(), outside).id == outside &&
               findNode(session.graph(), sink).id == sink;
    };
    check(checkCut() && session.historyEdit(false) == openstone::SchematicEdit::CutNodes,
          "Cut should remove the selected nodes and all their connections in one history entry");

    check(session.applyHistory(false, [](auto) {}) && session.graph().nodes().size() == 4 &&
          session.graph().links() == originalLinks && session.graph().module(module)->links() == childLinks &&
          findNode(session.graph(), local).x == -240 && findNode(session.graph(), local).y == 100 &&
          findNode(session.graph(), module).x == 0 && findNode(session.graph(), module).y == 40 &&
          session.graph().readValue(sink) == openstone::green::Value{std::string{"A!"}} && !session.historyEdit(false),
          "One undo should restore the original nodes, module contents, positions and crossing wires");
    check(session.applyHistory(true, [](auto) {}) && checkCut(), "One redo should cut the whole selection again");

    const auto pasted = session.pasteNodes(100, 150, false);
    check(pasted.size() == 2 && session.graph().links() == std::vector{SchematicLink{pasted[0], pasted[1], internal->input}} &&
          session.graph().module(pasted[1])->links() == childLinks &&
          session.graph().module(pasted[1])->readValue(join) == openstone::green::Value{std::string{"A"}},
          "Pasting a cut selection should restore its internal wires without the old external connections");
}

void duplicateSelectionPreservesSpacingWiringAndClipboard()
{
    using enum PrimitiveKind;
    openstone::Session session;
    const int first = session.addNode(String, -240, 30);
    const int second = session.addNode(String, -80, -100);
    const int joined = session.addNode(String, 180, 30);
    const int outside = session.addNode(String, -480, 0);
    session.setValue(first, std::string{"A"});
    session.setValue(second, std::string{"B"});
    session.setValue(outside, std::string{"clipboard"});
    session.connect({second, joined, 0});
    session.connect({first, joined, 0});
    session.connect({outside, joined, 0});
    session.copyNode(outside);
    const auto originalLinks = session.graph().links();

    session.beginEdit(openstone::SchematicEdit::DuplicateNodes);
    const auto copies = session.duplicateNodes(std::array{joined, first, second}, 24, 24);
    session.finishEdit();
    check(copies.size() == 3 && session.graph().nodes().size() == 7, "Duplicate should insert all selected nodes");
    const std::array originals{first, second, joined};
    for (std::size_t i = 0; i < originals.size(); ++i) {
        const auto& original = findNode(session.graph(), originals[i]);
        const auto& duplicate = findNode(session.graph(), copies[i]);
        check(duplicate.x == original.x + 24 && duplicate.y == original.y + 24,
              "Each copy should move by the same offset");
    }
    auto duplicateLinks = originalLinks;
    duplicateLinks.push_back({copies[1], copies[2], 0});
    duplicateLinks.push_back({copies[0], copies[2], 0});
    check(session.graph().links() == duplicateLinks &&
          session.graph().readValue(copies[2]) == openstone::green::Value{std::string{"BA"}},
          "Duplicate should retain internal fan-in order and omit external connections");
    check(session.applyHistory(false, [](auto) {}) && session.graph().nodes().size() == 4 &&
          session.graph().links() == originalLinks && !session.historyEdit(false),
          "One undo should remove the entire duplicated group");
    check(session.applyHistory(true, [](auto) {}) && session.graph().links() == duplicateLinks,
          "One redo should restore the entire duplicated group");
    const int pasted = session.pasteNode(400, 400, false);
    check(session.graph().readValue(pasted) == openstone::green::Value{std::string{"clipboard"}},
          "Duplicate should leave the previous clipboard contents available to paste");
}

void ticksUseTheManualClockOrDisplayFrames()
{
    using namespace std::chrono_literals;
    using enum PrimitiveKind;
    using enum openstone::TickWake;
    using openstone::TickTime;
    openstone::Session session;
    auto& graph = session.graph();
    const auto tick = session.addNode(Tick, 0, 0);
    const auto count = session.addNode(TriggerCounter, 150, 0);
    const auto pulse = session.addNode(Trigger, 0, 80);
    graph.connect({tick, count, 0}, LinkNotification::Silent);
    graph.connect({pulse, tick, 0}, LinkNotification::Silent);
    graph.fireTrigger(pulse);
    check(integer(graph, count) == 0 && !graph.readTick(tick).wired && graph.readTick(tick).rate == 25,
          "The default is 25 TPS and a trigger-only link should never fire the Tick output");

    const auto advance = [&](std::chrono::milliseconds time, openstone::TickWake wake) {
        auto schedule = graph.advanceTicks(TickTime{} + time, wake);
        check(!schedule.error, "Tick dispatch should succeed");
        return schedule;
    };
    const auto deadlineIs = [](const auto& schedule, std::chrono::milliseconds expected) {
        return schedule.deadline && std::abs((*schedule.deadline - (TickTime{} + expected)).count()) < 1e-9;
    };
    check(deadlineIs(advance(0ms, Configure), 40ms) && !advance(39ms, Timer).fired,
          "The first tick should wait one full period");
    check(advance(40ms, Timer).fired && integer(graph, count) == 1, "The manual clock should deliver a trigger");
    check(deadlineIs(advance(133ms, Timer), 160ms) && integer(graph, count) == 2,
          "A late wake should skip missed pulses and preserve the original phase");

    session.setTickRate(tick, 50.0f);
    check(deadlineIs(advance(134ms, Configure), 154ms), "The inline TPS should change the period");
    const auto rate = graph.addNode(Float, 0, 160, 12.5f);
    session.connect({rate, tick, 0});
    check(graph.readTick(tick).wired && graph.readTick(tick).rate == 12.5f &&
          deadlineIs(advance(135ms, Configure), 215ms), "A wired fractional TPS should replace the inline rate");

    session.setTickDisplaySync(tick, true);
    const auto synced = advance(140ms, Configure);
    check(synced.needsDisplay && !synced.deadline && !advance(300ms, Timer).fired,
          "Display Sync should cancel the manual deadline, including with a wired TPS");
    check(advance(310ms, Display).fired && integer(graph, count) == 3,
          "Each display wake should deliver one Trigger");

    const auto module = session.addNode(Module, 0, 240);
    auto& child = *graph.module(module);
    const auto nested = child.addNode(Tick, 0, 0);
    child.setTickRate(nested, 0);
    child.setTickDisplaySync(nested, true);
    check(advance(320ms, Display).fired && child.inspectOutput(nested).triggers == 1 && integer(graph, count) == 4,
          "Display-synced nodes in modules should run even when the root is being edited");
    session.setTickDisplaySync(tick, false);
    child.setTickDisplaySync(nested, false);
    check(deadlineIs(advance(322ms, Configure), 402ms), "Switching back should restore the current wired rate");
    graph.setValue(rate, 0.0f);
    const auto paused = advance(323ms, Configure);
    check(!paused.deadline && !paused.needsDisplay && !advance(420ms, Timer).fired,
          "Zero TPS should pause the manual clock");
    graph.disconnectInput(tick, 0);
    check(graph.readTick(tick).rate == 50.0f && deadlineIs(advance(421ms, Configure), 441ms),
          "Removing the value wire should restore the saved manual TPS");

    session.copyNode(tick);
    const auto copy = session.pasteNode(200, 180, false);
    check(graph.readTick(copy).rate == 50.0f && deadlineIs(advance(430ms, Configure), 441ms),
          "A copy should keep its authored rate");
    advance(442ms, Timer);
    check(graph.inspectOutput(copy).triggers == 0, "A copy should have its own timer phase");
    graph.removeNode(tick);
    check(deadlineIs(advance(444ms, Configure), 450ms), "Removing a Tick should cancel its pending wake");
}

void triggerDivCountsPulsesAndRestartsOnDivisorEvents()
{
    using enum PrimitiveKind;
    Schematic graph;
    const int pulse = graph.addNode(Trigger, 0, 0);
    const int divider = graph.addNode(TriggerDiv, 180, 0);
    const int counter = graph.addNode(TriggerCounter, 360, 0);
    graph.connect({pulse, divider, 0}, LinkNotification::Silent);
    graph.connect({divider, counter, 0}, LinkNotification::Silent);
    graph.fireTrigger(pulse);
    check(integer(graph, counter) == 1 && !graph.inspectOutput(divider).value,
          "An unconnected divisor passes every source pulse as a Trigger");

    const int divisor = graph.addNode(Int, 0, 80, std::int32_t{3});
    graph.connect({divisor, divider, 1});
    for (int i = 1; i <= 7; ++i) {
        graph.fireTrigger(pulse);
        check(integer(graph, counter) == 1 + i / 3, "Only every third source pulse should pass");
    }

    graph.setValue(divisor, std::int32_t{2});
    graph.fireTrigger(pulse);
    check(integer(graph, counter) == 3, "A divisor change resets the partial count without emitting");
    graph.fireTrigger(pulse);
    check(integer(graph, counter) == 4, "Two new pulses should now complete a group");

    const int reset = graph.addNode(Trigger, 0, 160);
    graph.connect({reset, divider, 1}, LinkNotification::Silent);
    graph.fireTrigger(pulse);
    graph.fireTrigger(reset);
    graph.fireTrigger(pulse);
    check(integer(graph, counter) == 4, "An unchanged divisor pulse still resets the partial count");
    graph.fireTrigger(pulse);
    check(integer(graph, counter) == 5, "The unchanged divisor should keep dividing by two");

    graph.setValue(divisor, std::int32_t{0});
    graph.fireTrigger(pulse);
    graph.setValue(divisor, std::int32_t{-3});
    graph.fireTrigger(pulse);
    graph.disconnectInput(divider, 1);
    graph.fireTrigger(pulse);
    check(integer(graph, counter) == 8, "Nonpositive and disconnected divisors should pass each pulse");
}

void triggerDeltaMeasuresSuccessivePulsesInMilliseconds()
{
    using namespace std::chrono_literals;
    using enum PrimitiveKind;
    using openstone::TickTime;
    using openstone::TickWake;

    Schematic graph;
    const int tick = graph.addNode(Tick, 0, 0);
    const int delta = graph.addNode(TriggerDelta, 180, 0);
    const int readout = graph.addNode(Float, 360, 0);
    graph.setTickDisplaySync(tick, true);
    graph.connect({tick, delta, 0}, LinkNotification::Silent);
    graph.connect({delta, readout, 0}, LinkNotification::Silent);

    const auto advance = [&](std::chrono::microseconds time, TickWake wake = TickWake::Display) {
        const auto schedule = graph.advanceTicks(TickTime{} + time, wake);
        check(!schedule.error, "Tick dispatch should succeed");
    };
    const auto checkReadout = [&](float milliseconds, std::uint64_t pulses) {
        const auto output = graph.inspectInput(readout, 0);
        check(output.value == openstone::green::Value{milliseconds} && output.triggers == pulses,
              "The Float readout should retain the last measured interval and receive each completed measurement");
    };

    checkReadout(0, 0);
    advance(0ms);
    checkReadout(0, 0);
    advance(12'500us);
    checkReadout(12.5F, 1);
    advance(20ms, TickWake::Configure);
    checkReadout(12.5F, 1);
    advance(25ms);
    checkReadout(12.5F, 2);
    advance(25ms);
    checkReadout(0, 3);
    advance(100ms);
    checkReadout(75, 4);
}

void indexerReportsEachInputWithoutRenumberingOtherLinks()
{
    using enum PrimitiveKind;
    using openstone::green::IntArray;
    using openstone::green::Type;
    Schematic graph;
    const auto add = [&](auto kind) { return graph.addNode(kind, 0, 0); };
    const int indexer = add(Indexer);
    const int first = add(Trigger);
    const int second = add(Trigger);
    const int third = add(Trigger);
    const int history = add(ArrayHistory);
    const int capacity = graph.addNode(Int, 0, 0, std::int32_t{8});
    const auto inputCount = [&] { return findNode(graph, indexer).inputPorts().size(); };
    check(inputCount() == 1 && graph.outputType(indexer) == Type::Int && integer(graph, indexer) == 0,
          "Indexer starts with one spare input and a zero Int output");

    graph.connect({first, indexer, 0}, LinkNotification::Silent);
    graph.connect({second, indexer, 1}, LinkNotification::Silent);
    graph.connect({third, indexer, 2}, LinkNotification::Silent);
    graph.connect({capacity, history, 0}, LinkNotification::Silent);
    graph.connect({indexer, history, 1}, LinkNotification::Silent);
    graph.connect({indexer, history, 2}, LinkNotification::Silent);
    check(inputCount() == 4 && graph.inputType(indexer, 3) == Type::Trigger,
          "Indexer offers a new Trigger input after the highest wired index");

    for (int source : {second, first, third, third}) graph.fireTrigger(source);
    check(std::get<IntArray>(graph.readValue(history)) == IntArray{2, 2, 0, 1} && integer(graph, indexer) == 2,
          "Each pulse publishes its zero-based index, including repeated pulses from the same input");
    graph.disconnectInput(indexer, 1);
    graph.fireTrigger(third);
    check(inputCount() == 4 && integer(graph, indexer) == 2,
          "Removing an earlier wire keeps the indices of the remaining wires");
    graph.disconnectInput(indexer, 2);
    check(inputCount() == 2 && graph.inspectOutput(indexer).triggers == 5,
          "Unwired trailing inputs shrink to one spare without emitting a new index");
}

void loopPublishesValueAndIterationInStepOrder()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    const auto checkRange = []<class Number>(Number start, std::optional<Number> increment, Number end,
                                           const std::vector<Number>& expected) {
        Schematic graph;
        const auto add = [&](auto kind) { return graph.addNode(kind, 0, 0); };
        const auto wire = [&](int source, int target, int input, int output = 0) {
            graph.connect({source, target, input, output}, LinkNotification::Silent);
        };
        const auto kind = std::is_same_v<Number, float> ? Float : Int;
        const int loop = add(Loop);
        const int run = add(Trigger);
        const int indexer = add(Indexer);
        const int capacity = graph.addNode(Int, 0, 0, static_cast<std::int32_t>(expected.size() + 1));
        check(!graph.inputType(loop, 0) && !graph.outputType(loop, 0), "Loop starts in Template mode");
        wire(graph.addNode(kind, 0, 0, start), loop, 0);
        if (increment) wire(graph.addNode(kind, 0, 0, *increment), loop, 1);
        wire(graph.addNode(kind, 0, 0, end), loop, 2);
        wire(run, loop, 4);

        const int values = add(ArrayHistory);
        const int iterations = add(ArrayHistory);
        const int events = add(ArrayHistory);
        wire(loop, values, 1, 0);
        wire(loop, iterations, 1, 1);
        wire(loop, values, 2, 2);
        wire(loop, iterations, 2, 2);
        wire(loop, indexer, 0, 2);
        wire(loop, indexer, 1, 3);
        wire(indexer, events, 1);
        wire(indexer, events, 2);
        for (int history : {values, iterations, events}) wire(capacity, history, 0);
        check(graph.inspectOutput(loop, 2).triggers == 0, "Only the Run input starts a loop");
        graph.fireTrigger(run);

        std::vector<Number> seen(expected.rbegin(), expected.rend());
        IntArray indices;
        for (auto i = expected.size(); i > 0; --i) indices.push_back(static_cast<std::int32_t>(i - 1));
        IntArray order(expected.size(), 0);
        if (!expected.empty()) order.insert(order.begin(), 1);
        check(graph.readValue(values) == Value{seen} && graph.readValue(iterations) == Value{indices},
              "The values and zero-based indices must be readable in order on each Step");
        check(graph.readValue(events) == Value{order} && graph.inspectOutput(loop, 2).triggers == expected.size() &&
              graph.inspectOutput(loop, 0).triggers == 0 && graph.inspectOutput(loop, 1).triggers == 0,
              "Step fires once per iteration, and Finished fires after all steps of a nonempty run");
        check(graph.readValue(loop) == Value{expected.empty() ? Number{} : expected.back()},
              "Loop retains the last iteration value or defaults to zero when no iterations ran");
    };

    checkRange.operator()<std::int32_t>(3, {}, 6, {3, 4, 5});
    checkRange.operator()<std::int32_t>(7, -2, 1, {7, 5, 3});
    checkRange.operator()<float>(0.5F, 0.25F, 1.5F, {0.5F, 0.75F, 1.0F, 1.25F});
    checkRange.operator()<std::int32_t>(2, 0, 6, {});
    checkRange.operator()<std::int32_t>(2, -1, 6, {});
    checkRange.operator()<std::int32_t>(2, {}, 2, {});
    const auto max = std::numeric_limits<std::int32_t>::max();
    checkRange.operator()<std::int32_t>(max - 1, 2, max, {max - 1});
    checkRange.operator()<float>(16'777'216.0F, 0.125F, 16'777'220.0F, {16'777'216.0F});
}

void advancedCounterStepsAndResetsWithinItsLimits()
{
    using enum PrimitiveKind;
    Schematic graph;
    const int counter = graph.addNode(CounterAdvanced, 180, 0);
    const int step = graph.addNode(Trigger, 0, 0);
    const int reset = graph.addNode(Trigger, 0, 80);
    const int maximum = graph.addNode(Int, 0, 160, std::int32_t{3});
    const int minimum = graph.addNode(Int, 0, 240, std::int32_t{-2});
    const int mode = graph.addNode(Int, 0, 320);
    const int start = graph.addNode(Int, 0, 400, std::int32_t{1});
    const std::array sources{step, reset, maximum, minimum, mode, start};
    for (int i = 0; i < std::ssize(sources); ++i) {
        graph.connect({sources[i], counter, i}, LinkNotification::Silent);
    }
    const auto expect = [&](int value, std::uint64_t changes, std::uint64_t highs, std::uint64_t lows) {
        check(integer(graph, counter) == value && graph.inspectOutput(counter, 0).triggers == changes &&
              graph.inspectOutput(counter, 1).triggers == highs && graph.inspectOutput(counter, 2).triggers == lows,
              "Counter Advanced should publish the expected value and limit pulses");
    };
    graph.fireTrigger(reset);
    expect(1, 1, 0, 0);
    graph.fireTrigger(reset);
    expect(1, 1, 0, 0);
    graph.fireTrigger(step);
    expect(2, 2, 0, 0);
    graph.fireTrigger(step);
    expect(3, 3, 1, 0);
    graph.fireTrigger(step);
    expect(3, 3, 1, 0);

    graph.setValue(mode, std::int32_t{1});
    expect(3, 3, 1, 0);
    for (int value = 2; value >= -2; --value) {
        graph.fireTrigger(step);
        expect(value, 6 - value, 1, value == -2 ? 1 : 0);
    }
    graph.fireTrigger(step);
    expect(-2, 8, 1, 1);

    graph.setValue(start, std::int32_t{4});
    expect(-2, 8, 1, 1);
    graph.fireTrigger(reset);
    expect(3, 9, 2, 1);
    graph.setValue(start, std::int32_t{-3});
    graph.fireTrigger(reset);
    expect(-2, 10, 2, 2);

    graph.setValue(start, std::int32_t{2});
    graph.fireTrigger(reset);
    expect(2, 11, 2, 2);
    graph.setValue(maximum, std::int32_t{1});
    expect(1, 12, 2, 2);
    graph.setValue(minimum, std::int32_t{0});
    graph.disconnectInput(counter, 5);
    graph.fireTrigger(reset);
    expect(0, 13, 2, 3);
    graph.setValue(minimum, std::int32_t{1});
    expect(1, 14, 2, 3);
}

void advancedCounterAlternatesAndDeliversLimitBeforeValue()
{
    using enum PrimitiveKind;
    using openstone::green::IntArray;
    Schematic graph;
    const auto add = [&](auto kind) { return graph.addNode(kind, 0, 0); };
    const auto wire = [&](int source, int target, int input, int output = 0) {
        graph.connect({source, target, input, output}, LinkNotification::Silent);
    };
    const int counter = add(CounterAdvanced);
    const int step = add(Trigger);
    const int reset = add(Trigger);
    const int start = graph.addNode(Int, 0, 0, std::int32_t{1});
    wire(step, counter, 0);
    wire(reset, counter, 1);
    wire(graph.addNode(Int, 0, 0, std::int32_t{2}), counter, 2);
    wire(graph.addNode(Int, 0, 0, std::int32_t{2}), counter, 4);
    wire(start, counter, 5);

    const int indexer = add(Indexer);
    const int events = add(ArrayHistory);
    wire(counter, indexer, 0, 1);
    wire(counter, indexer, 1, 2);
    wire(counter, indexer, 2, 0);
    wire(graph.addNode(Int, 0, 0, std::int32_t{12}), events, 0);
    wire(indexer, events, 1);
    wire(indexer, events, 2);
    for (const auto value : {1, 2, 1, 0, 1, 2, 1}) {
        graph.fireTrigger(step);
        check(integer(graph, counter) == value, "Up/Down should reverse without repeating either endpoint");
    }
    auto seen = std::get<IntArray>(graph.readValue(events));
    std::ranges::reverse(seen);
    check(seen == IntArray{2, 0, 2, 2, 1, 2, 2, 0, 2, 2},
          "Reached Max and Reached Min should pulse before Current Value on a limit step");

    graph.fireTrigger(reset);
    check(integer(graph, counter) == 1 && graph.inspectOutput(counter, 0).triggers == 7,
          "A reset to the existing value should stay silent");
    graph.fireTrigger(step);
    check(integer(graph, counter) == 2 && graph.inspectOutput(counter, 1).triggers == 3,
          "Reset restores the ascending direction even if the value was unchanged");
    graph.fireTrigger(step);
    graph.setValue(start, std::int32_t{2});
    graph.fireTrigger(reset);
    graph.fireTrigger(step);
    check(integer(graph, counter) == 2 && graph.inspectOutput(counter, 1).triggers == 4,
          "After reset to Max, the next ascending step only reverses direction");
    graph.fireTrigger(step);
    check(integer(graph, counter) == 1, "The following step counts down");
}

void advancedCounterStepsWithFlowStoneIntOverflow()
{
    using enum PrimitiveKind;
    Schematic graph;
    const int counter = graph.addNode(CounterAdvanced, 0, 0);
    const int step = graph.addNode(Trigger, 0, 0);
    const int reset = graph.addNode(Trigger, 0, 0);
    const int mode = graph.addNode(Int, 0, 0);
    constexpr auto maximum = std::numeric_limits<std::int32_t>::max();
    constexpr auto minimum = std::numeric_limits<std::int32_t>::min();
    const int maximumSource = graph.addNode(Int, 0, 0, maximum);
    const int minimumSource = graph.addNode(Int, 0, 0, minimum);
    check(graph.inspectInput(counter, 2).value == openstone::green::Value{maximum - 1},
          "The unwired Max avoids an overflowing increment at the ordinary upper limit");
    const std::array sources{step, reset, maximumSource, minimumSource, mode, maximumSource};
    for (int i = 0; i < std::ssize(sources); ++i) {
        graph.connect({sources[i], counter, i}, LinkNotification::Silent);
    }
    graph.fireTrigger(reset);
    graph.fireTrigger(step);
    check(integer(graph, counter) == minimum && graph.inspectOutput(counter, 1).triggers == 1 &&
          graph.inspectOutput(counter, 2).triggers == 0, "Up wraps before checking Max when Max is INT32_MAX");
    graph.setValue(mode, std::int32_t{1});
    graph.fireTrigger(step);
    check(integer(graph, counter) == maximum && graph.inspectOutput(counter, 1).triggers == 1 &&
          graph.inspectOutput(counter, 2).triggers == 0, "Down wraps before checking Min when Min is INT32_MIN");
}

void afterLoadFiresOnceWhenOpeningASchematic()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    openstone::Session session;
    for (int open = 0; open < 2; ++open) {
        auto project = std::make_unique<Schematic>();
        const int load = project->addNode(AfterLoad, 0, 0);
        const int duplicate = project->addNode(AfterDuplicate, 0, 90);
        const int count = project->addNode(TriggerCounter, 180, 0);
        const int module = project->addNode(Module, 180, 90);
        const int childCount = project->addNode(TriggerCounter, 360, 90);
        auto* child = project->module(module);
        const int childLoad = child->addNode(AfterLoad, 0, 0);
        const int output = child->addNode(ModuleOutput, 180, 0);
        child->connect({childLoad, output, 0});
        project->connect({module, childCount, 0}, LinkNotification::Silent);
        project->connect({load, count, 0});
        project->connect({duplicate, count, 0});
        check(project->outputType(load) == Type::Trigger && integer(*project, count) == 0 &&
              integer(*project, childCount) == 0, "Wiring After Load and After Duplicate leaves counters at zero");

        session.openSchematic(std::move(project));
        auto& graph = session.graph();
        check(!session.takeLifecycleError() && integer(graph, count) == 1 && integer(graph, childCount) == 1,
              "Each schematic open delivers one pulse from every After Load through its restored links");
        check(graph.inspectOutput(load).triggers == 1 && graph.inspectOutput(duplicate).triggers == 0 &&
              child->inspectOutput(childLoad).triggers == 1,
              "Opening fires After Load across module levels");
        check(session.enterModule(module) && session.navigate(0) && integer(graph, childCount) == 1,
              "Navigating between existing levels keeps the original load count");
    }
}

void fileUtilitiesFindNamesAndGenerateGuids()
{
    using namespace openstone;
    using namespace openstone::green;
    using enum PrimitiveKind;
    namespace fs = std::filesystem;
    struct Fixture {
        fs::path path = fs::temp_directory_path() /
            ("openstone-filenames-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ~Fixture() { std::error_code ignored; fs::remove_all(path, ignored); }
    } fixture;
    fs::create_directory(fixture.path);
    fs::create_directory(fixture.path / "folder.txt");
    for (const auto& path : {fs::path("b.txt"), fs::path("alpha.txt"), fs::path(u8"é.txt"),
                            fs::path("data.bin"), fs::path("README"), fs::path("folder.txt/nested.txt")}) {
        std::ofstream file(fixture.path / path);
        check(file.good(), "Write the Find Files test fixture");
    }

    Schematic graph;
    const int find = graph.addNode(FindFiles, 0, 0);
    const int guid = graph.addNode(GenerateGuid, 0, 0);
    const int input = graph.addNode(String, 0, 0, (fixture.path / "*.txt").string());
    const int trigger = graph.addNode(Trigger, 0, 0);
    graph.connect({input, find, 0});
    graph.connect({trigger, find, 1});
    graph.connect({trigger, guid, 0});
    const auto done = graph.inspectOutput(find, 2).triggers;
    graph.fireTrigger(trigger);
    check(graph.readValue(find, 0) == Value{StringArray{"alpha.txt", "b.txt", "é.txt"}} &&
          graph.readValue(find, 1) == Value{3} && graph.inspectOutput(find, 2).triggers == done + 1,
          "Find Files returns sorted basenames and one Done trigger, excluding folders and their contents");
    const auto first = std::get<std::string>(graph.readValue(guid));
    const auto validGuid = [](const std::string& token) {
        return token.size() == 38 && token.front() == '{' && token.back() == '}' &&
            token[9] == '-' && token[14] == '-' && token[19] == '-' && token[24] == '-' &&
            token[15] == '4' && std::string_view("89AB").contains(token[20]);
    };
    graph.setValue(input, (fixture.path / "?.txt").string());
    graph.fireTrigger(trigger);
    const auto second = std::get<std::string>(graph.readValue(guid));
    check(graph.readValue(find, 0) == Value{StringArray{"b.txt", "é.txt"}} &&
          validGuid(first) && validGuid(second) && first != second,
          "A wildcard matches a Unicode character and each Gen pulse produces a fresh UUIDv4");
    graph.setValue(input, (fixture.path / "*.*").string());
    graph.fireTrigger(trigger);
    check(graph.readValue(find, 1) == Value{5}, "The FlowStone all-files filter includes extensionless files");
    graph.setValue(input, (fixture.path / "absent" / "*.txt").string());
    graph.fireTrigger(trigger);
    check(graph.readValue(find, 0) == Value{StringArray{}} && graph.readValue(find, 1) == Value{0} &&
          graph.inspectOutput(find, 2).triggers == done + 4,
          "An empty result replaces the previous list and still completes");
}

void fileDialogRequestsReturnToTheNodeThatOpenedThem()
{
    using namespace openstone;
    using namespace openstone::green;
    using enum PrimitiveKind;
    Schematic graph;
    const int module = graph.addNode(Module, 0, 0);
    auto& child = *graph.module(module);
    const int file = child.addNode(FileDialog, 0, 0);
    const int folder = graph.addNode(FolderDialog, 0, 0);
    const int message = graph.addNode(MessageBox, 0, 0);
    const int openFile = child.addNode(Trigger, 0, 0);
    const int openFolder = graph.addNode(Trigger, 0, 0);
    const int openMessage = graph.addNode(Trigger, 0, 0);
    const int name = child.addNode(String, 0, 0, std::string{"test.wav"});
    const int save = child.addNode(Int, 0, 0, 0);
    const int okPath = child.addNode(SampleAndHold, 0, 0);
    const int icon = graph.addNode(Int, 0, 0, 3);
    const int type = graph.addNode(Int, 0, 0, 2);
    child.connect({openFile, file, 0});
    child.connect({name, file, 5});
    child.connect({save, file, 2});
    child.connect({file, okPath, 0});
    child.connect({file, okPath, 1, 1});
    graph.connect({openFolder, folder, 0});
    graph.connect({openMessage, message, 0});
    graph.connect({icon, message, 3});
    graph.connect({type, message, 4});

    check(!graph.pendingAppDialog(), "Wiring Open connectors does not show any dialogs");
    const auto ok = child.inspectOutput(file, 1).triggers;
    const auto cancel = child.inspectOutput(file, 2).triggers;
    child.fireTrigger(openFile);
    const auto request = *graph.pendingAppDialog();
    child.setValue(name, std::string{"other.wav"});
    child.fireTrigger(openFile);
    graph.fireTrigger(openFolder);
    check(request.kind == FileDialog && request.ownerPath == std::vector{module} &&
          request.type == 0 && graph.pendingAppDialog()->name == "test.wav",
          "File Dialog snapshots its inputs and queues requests by owner");
    check(graph.finishAppDialog(request.token, true, "/chosen/test.wav") &&
          child.readValue(okPath) == Value{std::string{"/chosen/test.wav"}} &&
          child.inspectOutput(file, 1).triggers == ok + 1 && child.inspectOutput(file, 2).triggers == cancel,
          "OK listeners can read the selected path immediately");
    const auto folderToken = graph.pendingAppDialog()->token;
    check(graph.pendingAppDialog()->kind == FolderDialog &&
          !graph.finishAppDialog(request.token, true, "/stale") &&
          graph.finishAppDialog(folderToken, true, "/chosen") &&
          graph.readValue(folder) == Value{std::string{"/chosen"}},
          "A stale response cannot complete the queued Folder Dialog");
    child.fireTrigger(openFile);
    check(graph.finishAppDialog(graph.pendingAppDialog()->token, false) &&
          child.readValue(file) == Value{std::string{"/chosen/test.wav"}} &&
          child.inspectOutput(file, 2).triggers == cancel + 1 && !graph.pendingAppDialog(),
          "Cancel has its own trigger and preserves the latest successful selection");

    graph.fireTrigger(openMessage);
    check(graph.pendingAppDialog()->icon == 3 && graph.pendingAppDialog()->type == 2 &&
          graph.finishAppDialog(graph.pendingAppDialog()->token, true) && graph.readValue(message) == Value{true},
          "An affirmative Message Box answer updates its Boolean output");
    graph.fireTrigger(openMessage);
    check(graph.finishAppDialog(graph.pendingAppDialog()->token, false) && graph.readValue(message) == Value{false},
          "A negative Message Box answer clears its Boolean output");
    graph.fireTrigger(openMessage);
    const auto removed = graph.pendingAppDialog()->token;
    graph.removeNode(message);
    check(!graph.pendingAppDialog() && !graph.finishAppDialog(removed, true),
          "Deleting the owner invalidates an outstanding request");
}

void schematicPathIsAvailableFromAfterLoad()
{
    using namespace openstone;
    using namespace openstone::green;
    using enum PrimitiveKind;
    Session session;
    auto replacement = std::make_unique<Schematic>();
    const int path = replacement->addNode(SchematicFilePath, 0, 0);
    const int load = replacement->addNode(AfterLoad, 0, 0);
    const int held = replacement->addNode(SampleAndHold, 0, 0);
    replacement->connect({path, held, 0, 1});
    replacement->connect({load, held, 1});
    check(replacement->readValue(path, 0) == Value{std::string{}} &&
          replacement->readValue(path, 1) == Value{std::string{}}, "A new schematic has both path outputs blank");
    const auto directory = std::filesystem::temp_directory_path();
    const auto target = directory / "test file.ostone";
    check(session.openSchematic(std::move(replacement), {}, target.string()), "Open the schematic with its file identity");
    auto& graph = session.graph();
    check(graph.readValue(held) == Value{std::string{"test file.ostone"}}, "After Load sees the reopened filename");
    check(graph.readValue(path, 0) == Value{target.parent_path().string()}, "The folder output omits the file name");
    const auto updates = graph.inspectOutput(path, 1).triggers;
    graph.setSchematicFilePath((directory / "renamed.ostone").string());
    check(graph.readValue(path, 1) == Value{std::string{"renamed.ostone"}} &&
          graph.inspectOutput(path, 1).triggers == updates + 1,
          "Changing the saved path sends one update");
}

void anAfterLoadErrorLeavesTheOpenedSchematicUsable()
{
    using enum PrimitiveKind;
    openstone::Session session;
    auto project = std::make_unique<Schematic>([](std::string_view) -> openstone::yellow::Bitmap {
        throw std::runtime_error("Image unavailable");
    });
    const int first = project->addNode(AfterLoad, 0, 0);
    const int path = project->addNode(String, 0, 90, std::string{"missing.png"});
    const int bitmap = project->addNode(Bitmap, 180, 0);
    project->connect({path, bitmap, 0});
    project->connect({first, bitmap, 1});
    const int second = project->addNode(AfterLoad, 0, 180);
    const int count = project->addNode(TriggerCounter, 180, 180);
    project->connect({second, count, 0});

    session.openSchematic(std::move(project));
    check(session.takeLifecycleError() && !session.takeLifecycleError() && integer(session.graph(), count) == 1,
          "Report a failing trigger handler once and still fire the remaining After Load nodes");
}

void afterDuplicateFiresOnlyInCopiedNodes()
{
    using enum PrimitiveKind;
    using openstone::SchematicEdit;
    openstone::Session session;
    const int duplicate = session.addNode(AfterDuplicate, 0, 0);
    const int load = session.addNode(AfterLoad, 0, 90);
    const int count = session.addNode(TriggerCounter, 180, 0);
    const int outside = session.addNode(TriggerCounter, 180, 90);
    session.connect({duplicate, count, 0});
    session.connect({duplicate, outside, 0});
    session.connect({load, count, 0});
    const std::array selection{duplicate, load, count};

    session.beginEdit(SchematicEdit::DuplicateNodes);
    const auto copies = session.duplicateNodes(selection, 24, 24);
    session.finishEdit();
    check(copies.size() == 3, "Duplicate all selected nodes");
    const auto verify = [&](std::span<const int> nodes) {
        const auto& graph = session.graph();
        check(graph.inspectOutput(nodes[0]).triggers == 1 && graph.inspectOutput(nodes[1]).triggers == 0 &&
              integer(graph, nodes[2]) == 1 && graph.inspectLink({nodes[0], nodes[2], 0}).triggers == 1,
              "Only the new After Duplicate fires, through the copied selection's internal link");
        check(graph.inspectOutput(duplicate).triggers == 0 && integer(graph, count) == 0 &&
              integer(graph, outside) == 0, "The original and its outgoing links remain quiet");
    };
    verify(copies);
    check(session.applyHistory(false, [](auto) {}) && session.applyHistory(true, [](auto) {}),
          "Undo and redo the duplication");
    verify(copies);
    session.copyNodes(selection);
    const auto pasted = session.pasteNodes(400, 0, false);
    check(pasted.size() == 3, "Paste all selected nodes");
    verify(pasted);
}

void afterDuplicateReachesNestedModuleCopies()
{
    using enum PrimitiveKind;
    openstone::Session session;
    const int module = session.addNode(Module, 0, 0);
    check(session.enterModule(module), "Enter the reusable module");
    const int innerModule = session.addNode(Module, 0, 0);
    check(session.enterModule(innerModule), "Enter the nested module");
    const int duplicate = session.addNode(AfterDuplicate, 0, 0);
    const int count = session.addNode(TriggerCounter, 180, 0);
    session.connect({duplicate, count, 0});
    check(session.navigate(0), "Return to the main schematic");
    const auto checkCopy = [&](int id) {
        const auto& graph = session.graph();
        const auto& copy = *graph.module(id)->module(innerModule);
        const auto& source = *graph.module(module)->module(innerModule);
        check(!session.takeLifecycleError() && integer(copy, count) == 1 && integer(source, count) == 0,
              "Only the new nested module receives the duplication pulse");
    };

    const auto copies = session.duplicateNodes(std::array{module}, 24, 24);
    check(copies.size() == 1, "Duplicate one reusable module");
    checkCopy(copies.front());
    session.copyNode(module);
    checkCopy(session.pasteNode(400, 0, true));
    session.saveModuleToToolbox(module);
    checkCopy(session.insertToolboxModule(0, 600, 0));
}

void aboutToCloseFiresOnceBeforeProjectReplacement()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    openstone::Session session;
    auto& graph = session.graph();
    const int closing = session.addNode(AboutToClose, 0, 0);
    const int count = session.addNode(TriggerCounter, 180, 0);
    session.connect({closing, count, 0});
    check(graph.inputType(closing, 0) == Type::Boolean && graph.inputType(closing, 1) == Type::Trigger &&
          graph.outputType(closing) == Type::Trigger && integer(graph, count) == 0,
          "About To Close exposes Hold and Done, and wiring its output leaves the counter at zero");
    check(!session.schematicReadyToClose(), "Closing requires a request");

    int replacements = 0;
    const auto beforeReplace = [&] {
        check(&session.graph() == &graph && integer(graph, count) == 1 &&
              graph.inspectOutput(closing).triggers == 1,
              "About To Close fires once while the old schematic and its connections are still available");
        session.requestSchematicClose();
        check(integer(graph, count) == 1, "A repeated close request leaves the count unchanged");
        ++replacements;
    };
    check(session.openSchematic(std::make_unique<Schematic>(), beforeReplace) && replacements == 1 &&
          session.graph().nodes().empty() && !session.schematicReadyToClose(),
          "With Hold off, opening a replacement completes and resets the close state");
}

void aboutToCloseWaitsForDoneInEveryHoldingModule()
{
    using enum PrimitiveKind;
    using enum openstone::TickWake;
    openstone::Session session;
    auto& root = session.graph();
    const int closing = root.addNode(AboutToClose, 0, 0);
    const int hold = root.addNode(Boolean, 0, 0, true);
    const int done = root.addNode(Trigger, 0, 0);
    const int count = root.addNode(TriggerCounter, 0, 0);
    root.connect({hold, closing, 0});
    root.connect({done, closing, 1});
    root.connect({closing, count, 0});
    root.fireTrigger(done);

    const int module = root.addNode(Module, 0, 0);
    auto& child = *root.module(module);
    const int childClosing = child.addNode(AboutToClose, 0, 0);
    const int childHold = child.addNode(Boolean, 0, 0, true);
    const int childDone = child.addNode(Tick, 0, 0);
    child.setTickRate(childDone, 10);
    child.connect({childHold, childClosing, 0});
    child.connect({childDone, childClosing, 1});

    auto replacement = std::make_unique<Schematic>();
    const int load = replacement->addNode(AfterLoad, 0, 0);
    const int loadCount = replacement->addNode(TriggerCounter, 0, 0);
    replacement->connect({load, loadCount, 0});
    int replacements = 0;
    check(!session.openSchematic(std::move(replacement), [&] { ++replacements; }),
          "Done before the close request cannot satisfy Hold");
    session.requestSchematicClose();
    check(!session.schematicReadyToClose() && !session.completeSchematicOpen() &&
          replacements == 0 && &session.graph() == &root && integer(root, count) == 1 &&
          child.inspectOutput(childClosing).triggers == 1, "A held close keeps the old graph live and fires each output once");

    root.setValue(hold, false);
    check(!session.schematicReadyToClose(), "Hold is sampled at close time and the pending request still needs Done");
    root.fireTrigger(done);
    check(!session.schematicReadyToClose() && !session.completeSchematicOpen(),
          "Done releases its own hold while the nested module is still waiting");

    const openstone::TickTime start{};
    check(!root.advanceTicks(start, Configure).fired, "Arm the nested timer");
    check(root.advanceTicks(start + std::chrono::seconds{1}, Timer).fired && session.schematicReadyToClose() &&
          &session.graph() == &root && replacements == 0, "A timer can deliver Done while closing is pending");
    check(session.completeSchematicOpen() && replacements == 1 && integer(session.graph(), loadCount) == 1 &&
          !session.completeSchematicOpen() && !session.takeLifecycleError(),
          "The replacement opens once after all Done pulses and fires its own After Load");
}

void aboutToCloseAcceptsSynchronousDoneFeedback()
{
    using enum PrimitiveKind;
    openstone::Session session;
    auto& graph = session.graph();
    const int first = graph.addNode(AboutToClose, 0, 0);
    const int second = graph.addNode(AboutToClose, 0, 0);
    const int hold = graph.addNode(Boolean, 0, 0, true);
    const int count = graph.addNode(TriggerCounter, 0, 0);
    graph.connect({hold, first, 0});
    graph.connect({hold, second, 0});
    graph.connect({first, second, 1});
    graph.connect({second, count, 0});
    graph.connect({count, first, 1});

    session.requestSchematicClose();
    check(session.schematicReadyToClose() && integer(graph, count) == 1 &&
          graph.inspectOutput(first).triggers == 1 && graph.inspectOutput(second).triggers == 1,
          "Both waiting nodes can receive Done during the close cascade without retriggering their outputs");
}

void triggerThreadDefersPulsesAndPullsCurrentValues()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    Schematic graph;
    const int pulse = graph.addNode(Trigger, 0, 0);
    const int thread = graph.addNode(TriggerThread, 0, 0);
    const int direct = graph.addNode(TriggerCounter, 0, 0);
    const int deferred = graph.addNode(TriggerCounter, 0, 0);
    const int value = graph.addNode(Int, 0, 0, std::int32_t{7});
    const int sample = graph.addNode(SampleAndHold, 0, 0);
    graph.connect({pulse, thread, 0}, LinkNotification::Silent);
    graph.connect({pulse, direct, 0}, LinkNotification::Silent);
    graph.connect({thread, deferred, 0}, LinkNotification::Silent);
    graph.connect({value, sample, 0}, LinkNotification::Silent);
    graph.connect({thread, sample, 1}, LinkNotification::Silent);
    check(graph.inputType(thread, 0) == Type::Trigger && graph.outputType(thread) == Type::Trigger,
          "Trigger Thread carries pulses on both connectors");

    for (int i = 0; i < 3; ++i) graph.fireTrigger(pulse);
    check(graph.hasDeferredTriggers() && graph.inspectInput(thread, 0).triggers == 3 &&
          graph.inspectOutput(thread).triggers == 0 && integer(graph, direct) == 3 && integer(graph, deferred) == 0,
          "The emitting cascade finishes while all three Thread outputs are still queued");
    graph.setValue(value, std::int32_t{9});
    const auto result = graph.dispatchDeferredTriggers();
    check(result.fired && !result.error && !graph.hasDeferredTriggers() &&
          graph.inspectOutput(thread).triggers == 3 && integer(graph, deferred) == 3 &&
          integer(graph, sample) == 9, "Deliver one pulse per input and let downstream nodes pull the current values");
}

void triggerThreadPreservesFifoAcrossModules()
{
    using enum PrimitiveKind;
    using openstone::green::IntArray;
    Schematic graph;
    const int rootSource = graph.addNode(Trigger, 0, 0);
    const int rootThread = graph.addNode(TriggerThread, 0, 0);
    const int module = graph.addNode(Module, 0, 0);
    auto& child = *graph.module(module);
    const int childSource = child.addNode(Trigger, 0, 0);
    const int childThread = child.addNode(TriggerThread, 0, 0);
    const int childOutput = child.addNode(ModuleOutput, 0, 0);
    child.connect({childSource, childThread, 0}, LinkNotification::Silent);
    child.connect({childThread, childOutput, 0}, LinkNotification::Silent);

    const int indexer = graph.addNode(Indexer, 0, 0);
    const int history = graph.addNode(ArrayHistory, 0, 0);
    const int capacity = graph.addNode(Int, 0, 0, std::int32_t{4});
    graph.connect({rootSource, rootThread, 0}, LinkNotification::Silent);
    graph.connect({rootThread, indexer, 0}, LinkNotification::Silent);
    graph.connect({module, indexer, 1}, LinkNotification::Silent);
    graph.connect({capacity, history, 0}, LinkNotification::Silent);
    graph.connect({indexer, history, 1}, LinkNotification::Silent);
    graph.connect({indexer, history, 2}, LinkNotification::Silent);

    child.fireTrigger(childSource);
    graph.fireTrigger(rootSource);
    child.fireTrigger(childSource);
    graph.fireTrigger(rootSource);
    const auto result = child.dispatchDeferredTriggers();
    check(result.fired && !result.error && !graph.hasDeferredTriggers() &&
          std::get<IntArray>(graph.readValue(history)) == IntArray{0, 1, 0, 1},
          "One FIFO delivers in enqueue order across modules, recorded newest first by Array History");
}

void triggerThreadFeedbackWaitsForTheNextTurn()
{
    using enum PrimitiveKind;
    Schematic graph;
    const int source = graph.addNode(Trigger, 0, 0);
    const int thread = graph.addNode(TriggerThread, 0, 0);
    const int count = graph.addNode(TriggerCounter, 0, 0);
    graph.connect({source, thread, 0}, LinkNotification::Silent);
    graph.connect({thread, count, 0}, LinkNotification::Silent);
    graph.connect({count, thread, 0}, LinkNotification::Silent);
    graph.fireTrigger(source);

    for (int expected = 1; expected <= 2; ++expected) {
        const auto result = graph.dispatchDeferredTriggers();
        check(result.fired && !result.error && graph.hasDeferredTriggers() && integer(graph, count) == expected,
              "Feedback through Thread starts one new cascade per turn");
    }
    graph.disconnectLink({count, thread, 0});
    check(graph.dispatchDeferredTriggers().fired && integer(graph, count) == 3 && !graph.hasDeferredTriggers(),
          "Disconnecting feedback leaves only the outstanding pulse to deliver");
}

void triggerThreadUsesTheLiveSchematic()
{
    using enum PrimitiveKind;
    openstone::Session session;
    auto& graph = session.graph();
    const int source = graph.addNode(Trigger, 0, 0);
    const int thread = graph.addNode(TriggerThread, 0, 0);
    const int count = graph.addNode(TriggerCounter, 0, 0);
    graph.connect({source, thread, 0}, LinkNotification::Silent);
    graph.connect({thread, count, 0}, LinkNotification::Silent);

    graph.fireTrigger(source);
    const int module = session.makeModule(std::array{thread, count});
    auto& child = *graph.module(module);
    const auto copies = session.duplicateNodes(std::array{module}, 24, 24);
    check(copies.size() == 1 && graph.dispatchDeferredTriggers().fired && integer(child, count) == 1 &&
          integer(*graph.module(copies.front()), count) == 0,
          "A queued pulse follows a grouped primitive and belongs to the original instance");

    graph.fireTrigger(source);
    session.removeNode(module);
    const auto canceled = graph.dispatchDeferredTriggers();
    check(!canceled.fired && !canceled.error && !graph.hasDeferredTriggers(),
          "Queued pulses for a deleted module are discarded safely");
}

void triggerThreadCanCompleteAHeldClose()
{
    using enum PrimitiveKind;
    openstone::Session session;
    auto& graph = session.graph();
    const int closing = graph.addNode(AboutToClose, 0, 0);
    const int hold = graph.addNode(Boolean, 0, 0, true);
    const int thread = graph.addNode(TriggerThread, 0, 0);
    graph.connect({hold, closing, 0}, LinkNotification::Silent);
    graph.connect({closing, thread, 0}, LinkNotification::Silent);
    graph.connect({thread, closing, 1}, LinkNotification::Silent);

    check(!session.openSchematic(std::make_unique<Schematic>()) && graph.hasDeferredTriggers() &&
          !session.schematicReadyToClose(), "Closing waits for the queued Done pulse");
    check(graph.dispatchDeferredTriggers().fired && session.schematicReadyToClose() &&
          session.completeSchematicOpen(), "Done from a fresh trigger cascade releases the hold");
}

void monoToFloatSamplesOnTick()
{
    using namespace std::chrono_literals;
    using enum PrimitiveKind;
    using enum openstone::TickWake;
    using openstone::TickTime;
    using openstone::green::Type;
    openstone::Session session;
    auto& graph = session.graph();
    const auto source = session.addNode(DspCode, 0, 0);
    graph.setDspDraft(source, "streamin gain; streamout ramp; float phase; ramp = phase * gain; phase += 1;");
    session.compileDsp(source);
    const auto gain = graph.addNode(Float, 0, 0, 1.0F);
    const auto sampler = session.addNode(MonoToFloat, 0, 0);
    const auto tick = session.addNode(Tick, 0, 0);
    const auto count = session.addNode(TriggerCounter, 0, 0);
    const auto value = [&] { return std::get<float>(graph.readValue(sampler)); };
    graph.connect({gain, source, 0}, LinkNotification::Silent);
    graph.connect({source, sampler, 0}, LinkNotification::Silent);
    graph.connect({tick, sampler, 1}, LinkNotification::Silent);
    graph.connect({sampler, count, 0}, LinkNotification::Silent);
    check(graph.inputType(sampler, 0) == Type::Mono && graph.outputType(sampler, 0) == Type::Float &&
          graph.outputType(source, 0) == Type::Mono && value() == 0 && integer(graph, count) == 0,
          "The Mono reader should resolve the DSP connector and wait for Get");

    const auto advance = [&](std::chrono::milliseconds time, openstone::TickWake wake) {
        const auto schedule = graph.advanceTicks(TickTime{} + time, wake);
        if (schedule.error) {
            std::rethrow_exception(schedule.error);
        }
        return schedule.fired;
    };
    check(!advance(0ms, Configure) && advance(40ms, Timer) && value() == 0 && integer(graph, count) == 1,
          "Get should publish the first sample even if it is equal to the initial value");
    check(!advance(60ms, Timer) && value() == 0 && integer(graph, count) == 1,
          "The Green Float should hold its value between ticks");
    check(advance(80ms, Timer) && value() == 1920 && integer(graph, count) == 2,
          "The 25 TPS clock should sample a running 48 kHz preview");
    session.setValue(gain, 2.0F);
    check(value() == 1920 && integer(graph, count) == 2 &&
          advance(120ms, Timer) && value() == 7680 && integer(graph, count) == 3,
          "Green controls should affect the next sample while preserving the DSP phase");
    graph.disconnectInput(sampler, 0);
    check(advance(160ms, Timer) && value() == 0 && integer(graph, count) == 4,
          "Get on an unwired Mono input should publish silence");
    graph.connect({source, sampler, 0}, LinkNotification::Silent);
    graph.fireTrigger(sampler);
    check(value() == 0 && integer(graph, count) == 5, "Manual Get should start the rewired signal preview");
}

void streamToGreenSamplesBooleanOnlyOnGet()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    using openstone::green::Value;
    openstone::Session session;
    auto& graph = session.graph();
    const int control = graph.addNode(Boolean, 0, 0, true);
    const int dsp = graph.addNode(DspCode, 0, 0);
    graph.setDspDraft(dsp, "streamboolin enabled; streamboolout gate; gate = enabled;");
    session.compileDsp(dsp);
    const int sampler = graph.addNode(StreamToGreen, 0, 0);
    const int clock = graph.addNode(Trigger, 0, 0);
    const int count = graph.addNode(TriggerCounter, 0, 0);
    graph.connect({control, dsp, 0}, LinkNotification::Silent);
    graph.connect({dsp, sampler, 0});
    graph.connect({clock, sampler, 1}, LinkNotification::Silent);
    graph.connect({sampler, count, 0}, LinkNotification::Silent);
    check(graph.outputType(dsp) == Type::MonoBoolean && graph.inputType(sampler, 0) == Type::MonoBoolean &&
          graph.outputType(sampler) == Type::Boolean && graph.readValue(sampler) == Value{false},
          "Generic StreamBoolean specializes to MonoBoolean and starts with a false Green sample");
    graph.fireTrigger(clock);
    check(graph.readValue(sampler) == Value{true} && integer(graph, count) == 1,
          "Get publishes true from a Boolean stream");
    graph.setValue(control, false);
    check(graph.readValue(sampler) == Value{true} && integer(graph, count) == 1,
          "A changed signal leaves the latched Green value alone until Get");
    graph.fireTrigger(clock);
    graph.fireTrigger(clock);
    check(graph.readValue(sampler) == Value{false} && integer(graph, count) == 3,
          "Each Get publishes one trigger even if the Boolean sample is unchanged");
    graph.setTemplateType(sampler, Type::Boolean);
    graph.disconnectInput(sampler, 0);
    graph.fireTrigger(clock);
    check(graph.readValue(sampler) == Value{false} && integer(graph, count) == 4,
          "An explicitly Boolean sampler with no stream publishes false");
    graph.setTemplateType(sampler, {});
    graph.setDspDraft(dsp, "monoout sample; sample = -0.25;");
    session.compileDsp(dsp);
    graph.connect({dsp, sampler, 0});
    graph.fireTrigger(clock);
    check(graph.outputType(sampler) == Type::Float && graph.readValue(sampler) == Value{-0.25F},
          "Rewiring the sampler to Mono switches the held output to Float");
}

void streamToGreenReadsTheLatestLiveBooleanSample()
{
    using enum PrimitiveKind;
    using openstone::green::Value;
    openstone::Session session;
    auto& graph = session.graph();
    const int input = graph.addNode(AudioIn, 0, 0);
    const int comparator = graph.addNode(DspCode, 0, 0);
    graph.setDspDraft(comparator, "monoin signal; monoboolout gate; gate = signal > 0;");
    session.compileDsp(comparator);
    const int monitor = graph.addNode(DspCode, 0, 0);
    graph.setDspDraft(monitor, "monoboolin gate; monoout sample; sample = gate;");
    session.compileDsp(monitor);
    const int output = graph.addNode(AudioOut, 0, 0);
    const int sampler = graph.addNode(StreamToGreen, 0, 0);
    graph.connect({input, comparator, 0});
    graph.connect({comparator, monitor, 0});
    graph.connect({monitor, output, 0});
    graph.connect({comparator, sampler, 0});
    auto patch = graph.prepareAudio(2);
    graph.setLiveAudio(&patch);
    std::array<float, 2> left{}, right{};
    const std::array positiveLast{-1.0F, 0.5F}, negativeLast{0.5F, -1.0F};
    check(patch.processor().process({positiveLast, {}}, {left, right}), "Render the live Boolean stream");
    graph.fireTrigger(sampler);
    check(graph.readValue(sampler) == Value{true}, "Get reads the latest true sample from live audio");
    check(patch.processor().process({negativeLast, {}}, {left, right}), "Render a false sample");
    check(graph.readValue(sampler) == Value{true}, "Audio processing keeps the sampled Green value until Get");
    graph.fireTrigger(sampler);
    check(graph.readValue(sampler) == Value{false}, "Get reads the latest false sample from live audio");
}

void matrixRoutesGreenValuesAndReplacesItsConnections()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    using openstone::green::Value;
    Schematic graph;
    const auto matrix = graph.addNode(Matrix, 0, 0);
    const auto map = graph.addNode(String, 0, 0, std::string{"1,1,2,0,0,1"});
    const auto a = graph.addNode(Float, 0, 0, 2.F);
    const auto b = graph.addNode(Float, 0, 0, 4.F);
    const auto c = graph.addNode(Float, 0, 0, 8.F);
    const auto x = graph.addNode(Float, 0, 0);
    const auto y = graph.addNode(Float, 0, 0);

    const auto& router = *std::ranges::find(graph.nodes(), matrix, &openstone::SchematicNode::id);
    check(router.inputPorts().size() == 2 &&
          router.inputPorts().front().name == "Connections" &&
          graph.inputType(matrix, 0) == Type::String && !graph.inputType(matrix, 1) &&
          router.outputPorts().size() == 1 && !graph.outputType(matrix),
          "Matrix starts with Connections and a generic spare on each side");
    graph.connect({map, matrix, 0});
    graph.connect({a, matrix, 1});
    graph.connect({b, matrix, 2});
    graph.connect({c, matrix, 3});
    graph.connect({matrix, x, 0, 0});
    graph.connect({matrix, y, 0, 1});
    check(router.inputPorts().size() == 5 && router.outputPorts().size() == 3 &&
          graph.outputType(matrix, 0) == Type::Float && graph.outputType(matrix, 1) == Type::Float,
          "Wiring a spare adds another connector in that direction");

    const auto expect = [&](float first, float second) {
        check(graph.readValue(x) == Value{first} && graph.readValue(y) == Value{second},
              "Matrix uses zero-based input/output pairs and combines Green values");
    };
    expect(8.F, 6.F);
    const auto before = graph.inspectOutput(matrix, 0).triggers;
    graph.setValue(a, 3.F);
    expect(8.F, 7.F);
    check(graph.inspectOutput(matrix, 0).triggers == before,
          "A value change pulses only the outputs that receive that input");

    const auto wires = graph.links().size();
    graph.setValue(map, std::string{" 0 , 0 , 0 , 1 , 3 , 0 , 0 , 2 , -1,0, bad,0, 2"});
    expect(3.F, 3.F);
    check(graph.links().size() == wires &&
          router.inputPorts().size() == 5 && router.outputPorts().size() == 3,
          "A new map replaces old routes without editing wires or turning spare ports into channels");
    graph.setValue(map, std::string{});
    expect(0.F, 0.F);
    graph.setValue(map, std::string{"2,0"});
    expect(8.F, 0.F);
    graph.disconnectInput(matrix, 0);
    expect(0.F, 0.F);
    graph.disconnectInput(matrix, 3);
    check(router.inputPorts().size() == 4, "An unused trailing signal input is reclaimed");
}

void matrixRoutesTriggerPulsesWithoutFiringOnReconfiguration()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    Schematic graph;
    const auto matrix = graph.addNode(Matrix, 0, 0);
    const auto map = graph.addNode(String, 0, 0, std::string{"0,0,0,1"});
    const auto a = graph.addNode(Trigger, 0, 0);
    const auto b = graph.addNode(Trigger, 0, 0);
    const auto x = graph.addNode(TriggerCounter, 0, 0);
    const auto y = graph.addNode(TriggerCounter, 0, 0);
    graph.connect({map, matrix, 0}, LinkNotification::Silent);
    graph.connect({a, matrix, 1}, LinkNotification::Silent);
    graph.connect({b, matrix, 2}, LinkNotification::Silent);
    graph.connect({matrix, x, 0, 0}, LinkNotification::Silent);
    graph.connect({matrix, y, 0, 1}, LinkNotification::Silent);
    check(graph.outputType(matrix, 1) == Type::Trigger, "A Trigger signal specializes the Matrix");
    graph.fireTrigger(a);
    graph.fireTrigger(b);
    check(integer(graph, x) == 1 && integer(graph, y) == 1, "A pulse fans out only through the selected routes");

    graph.setValue(map, std::string{"1,0"});
    check(integer(graph, x) == 1 && integer(graph, y) == 1, "Editing the map does not manufacture Trigger pulses");
    graph.fireTrigger(a);
    graph.fireTrigger(b);
    check(integer(graph, x) == 2 && integer(graph, y) == 1, "Only the new Trigger route remains");
}

void matrixChangesStreamRoutesOnTheNextBuffer()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    Schematic graph;
    const auto input = graph.addNode(AudioIn, 0, 0);
    const auto output = graph.addNode(AudioOut, 0, 0);
    const auto matrix = graph.addNode(Matrix, 0, 0);
    const auto delay = graph.addNode(DelayOneSample, 0, 0);
    const auto map = graph.addNode(String, 0, 0, std::string{});
    graph.connect({input, matrix, 1, 0});
    graph.connect({input, delay, 0, 1});
    graph.connect({delay, matrix, 2});
    graph.connect({matrix, output, 0, 0});
    graph.connect({matrix, output, 1, 1});
    graph.connect({map, matrix, 0});
    check(graph.inputType(matrix, 1) == Type::Mono && graph.outputType(matrix, 1) == Type::Mono,
          "Audio In specializes Matrix signal ports to Mono");

    const std::array left{1.F, 2.F, 3.F, 4.F, 5.F, 6.F, 7.F};
    const std::array right{-1.F, -1.F, -2.F, -3.F, -5.F, -8.F, -13.F};
    const std::array delayed{-13.F, -1.F, -1.F, -2.F, -3.F, -5.F, -8.F};
    const std::array sum{-12.F, 1.F, 2.F, 2.F, 2.F, 1.F, -1.F};
    const std::array<float, 7> silence{};
    for (const std::size_t block : {1, 3, 7}) {
        graph.setValue(map, std::string{"0,0"});
        auto patch = graph.prepareAudio(block);
        graph.setLiveAudio(&patch);
        std::array<float, 7> outLeft{}, outRight{};
        const auto render = [&](const auto& expectedLeft, const auto& expectedRight) {
            check(patch.processor().process({left, right}, {outLeft, outRight}) &&
                  outLeft == expectedLeft && outRight == expectedRight,
                  "The current buffer must use the latest published Matrix routes");
        };
        render(left, silence);

        graph.setValue(map, std::string{"1,0,0,1"});
        check(graph.refreshAudioControls(patch), "Reconfiguring Matrix keeps the same audio patch");
        render(delayed, left);
        const auto trace = graph.liveDspTrace(matrix, 0, true);
        check(trace && trace->size() >= delayed.size() &&
              std::ranges::equal(std::span{*trace}.last(delayed.size()), delayed),
              "The live monitor continues reading the same Matrix output after switching routes");

        graph.setValue(map, std::string{});
        check(graph.refreshAudioControls(patch), "An empty Matrix map keeps the audio patch");
        render(silence, silence);

        graph.setValue(map, std::string{"0,0,1,0,0,1"});
        check(graph.refreshAudioControls(patch), "A Matrix map can restore routes after silence");
        render(sum, left);
        graph.setLiveAudio(nullptr);
    }
}

void matrixPreservesMonoDoubleAndResolvesGenericStreams()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    openstone::Session session;
    auto& graph = session.graph();
    const auto addCode = [&](std::string text) {
        const auto id = session.addNode(DspCode, 0, 0);
        graph.setDspDraft(id, std::move(text));
        session.compileDsp(id);
        return id;
    };
    const auto unit = addCode("streamdoubleout y; y = 1;");
    const auto fraction = addCode("streamdoubleout y; y = 0.0000000000009094947017729282379150390625;");
    const auto unused = addCode("streamdoubleout y; y = 99;");
    const auto sink = addCode("monodoublein x; monodoubleout y; y = x;");
    const auto matrix = graph.addNode(Matrix, 0, 0);
    const auto map = graph.addNode(String, 0, 0, std::string{"0,0,1,0"});
    graph.connect({unit, matrix, 1});
    graph.connect({fraction, matrix, 2});
    graph.connect({unused, matrix, 3});
    graph.connect({matrix, sink, 0});
    graph.connect({map, matrix, 0});
    check(graph.outputType(unit) == Type::MonoDouble && graph.outputType(matrix) == Type::MonoDouble,
          "The fixed MonoDouble destination specializes the generic sources through Matrix");
    for (const std::size_t block : {1, 5, 13}) {
        check(graph.captureDsp(sink, 13, block) ==
                  std::vector<std::vector<double>>{std::vector<double>(13, 1.0 + 0x1p-40)},
              "The MonoDouble sum preserves precision at every buffer size");
        check(graph.dspTrace(unused, 0, true).empty(), "An unmapped source is not rendered");
    }
}

void audioEndpointsProcessHostBuffers()
{
    using enum PrimitiveKind;
    openstone::Session session;
    auto& graph = session.graph();
    const auto input = graph.addNode(AudioIn, 0, 0);
    const auto output = graph.addNode(AudioOut, 0, 0);
    graph.connect({input, output, 0, 0});
    graph.connect({input, output, 1, 1});
    auto patch = graph.prepareAudio(3);

    const std::array left{0.5F, 0.25F, -0.5F, 0.0F, 0.25F, 0.5F, -0.25F};
    const std::array right{-0.75F, -0.25F, 0.25F, -0.5F, 0.125F, 0.0F, 0.25F};
    std::array<float, left.size()> outLeft{}, outRight{};
    check(patch.processor().needsInput() &&
          patch.processor().process({left, right}, {outLeft, outRight}) &&
          outLeft == left && outRight == right,
          "Audio In and Audio Out should pass stereo channels through a partial final block");
    check(patch.processor().process({left, {}}, {outLeft, outRight}) &&
          outLeft == left && std::ranges::all_of(outRight, [](float sample) { return sample == 0; }),
          "An absent host input channel should be silent");
    check(!patch.processor().process({left, right}, {outLeft, std::span{outRight}.first(1)}),
          "A host buffer length mismatch should be rejected");

    graph.disconnectInput(output, 0);
    check(!graph.refreshAudioControls(patch), "A running host must replace a patch after rewiring");
    const auto gain = graph.addNode(Float, 0, 0, 0.5F);
    const auto dsp = graph.addNode(DspCode, 0, 0);
    graph.setDspDraft(dsp, "streamin signal; streamin gain; streamout result; result = signal * gain;");
    session.compileDsp(dsp);
    graph.connect({input, dsp, 0, 0});
    graph.connect({gain, dsp, 1});
    graph.connect({dsp, output, 0});
    graph.makeModule(std::array{dsp});
    patch = graph.prepareAudio(4);
    graph.setValue(gain, 0.25F);
    check(graph.refreshAudioControls(patch) &&
          patch.processor().process({left, right}, {outLeft, outRight}) && outRight == right,
          "Audio routing through a module should accept live Green control updates");
    for (std::size_t i = 0; i < outLeft.size(); ++i) {
        check(outLeft[i] == left[i] * 0.25F, "The Mono DSP block should apply the new gain at each sample");
    }

    graph.disconnectInput(output, 1);
    patch = graph.prepareAudio(5);
    check(patch.processor().process({left, right}, {outLeft, outRight}) &&
          std::ranges::all_of(outRight, [](float sample) { return sample == 0; }),
          "An unwired Audio Out channel should stay silent");
}

void liveAudioReadoutsObserveTheRunningGraph()
{
    using enum PrimitiveKind;
    using openstone::green::FloatArray;
    openstone::Session session;
    auto& graph = session.graph();
    const auto input = graph.addNode(AudioIn, 0, 0);
    const auto output = graph.addNode(AudioOut, 0, 0);
    const auto capture = graph.addNode(MonoToGraph4, 0, 0);
    const auto sampler = graph.addNode(MonoToFloat, 0, 0);
    const auto ns = graph.addNode(Int, 0, 0, std::int32_t{5});
    const auto count = graph.addNode(TriggerCounter, 0, 0);
    const auto dsp = graph.addNode(DspCode, 0, 0);
    graph.setDspDraft(dsp, "streamin in; streamout out; out = in * 2;");
    session.compileDsp(dsp);
    graph.connect({input, dsp, 0, 0});
    graph.connect({dsp, output, 0});
    graph.connect({input, output, 1, 1});
    graph.connect({input, capture, 0, 0});
    graph.connect({dsp, capture, 1});
    graph.connect({input, capture, 2, 1});
    graph.connect({dsp, capture, 3});
    graph.connect({input, capture, 3, 1});
    graph.connect({dsp, sampler, 0});
    graph.connect({input, sampler, 0, 1});
    graph.connect({ns, capture, 4}, LinkNotification::Silent);
    graph.connect({sampler, count, 0}, LinkNotification::Silent);
    const auto module = graph.makeModule(std::array{dsp});

    const auto idle = graph.addNode(DspCode, 0, 0);
    graph.setDspDraft(idle, "streamout value; value = 1;");
    session.compileDsp(idle);
    graph.setDspDraft(idle, "invalid draft on an idle Stream");
    auto patch = graph.prepareAudio(23);
    check(!graph.liveDspTrace(input, 0, true), "Prepared audio should have no live readout before activation");
    graph.setLiveAudio(&patch);
    const auto beforeSamples = graph.liveDspTrace(input, 0, true);
    check(beforeSamples && beforeSamples->empty() && !graph.liveDspTrace(idle, 0, true),
          "Only scheduled signals should have a live readout");

    std::array<float, 2> firstLeft{1, 2}, firstRight{-1, -2}, outLeft{}, outRight{};
    check(patch.processor().process({firstLeft, firstRight}, {outLeft, outRight}), "Initial stereo callback");
    graph.fireTrigger(capture);
    check(std::get<FloatArray>(graph.readValue(capture, 0)) == FloatArray{0, 0, 0, 1, 2} &&
          std::get<FloatArray>(graph.readValue(capture, 1)) == FloatArray{0, 0, 0, 2, 4} &&
          std::get<FloatArray>(graph.readValue(capture, 2)) == FloatArray{0, 0, 0, -1, -2} &&
          std::get<FloatArray>(graph.readValue(capture, 3)) == FloatArray{0, 0, 0, 1, 2},
          "Live Get should align its channels and pad the early samples with silence");

    // Advance past the ring capacity with uneven processing blocks.
    const auto frames = static_cast<std::size_t>(openstone::maxDspCaptureFrames) + 237;
    std::vector<float> left(frames), right(frames), renderedLeft(frames), renderedRight(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        left[i] = static_cast<float>(i) / 32768.0F;
        right[i] = -left[i];
    }
    check(patch.processor().process({left, right}, {renderedLeft, renderedRight}), "Render beyond history wrap");
    const auto swatch = graph.liveDspTrace(output, 0, false);
    check(swatch && swatch->size() == 128 &&
          std::ranges::equal(*swatch, std::span{renderedLeft}.last(128)) &&
          graph.liveDspTrace(module, 0, true) == swatch,
          "A live swatch should match the rendered channel through a module boundary");
    graph.setValue(ns, openstone::maxDspCaptureFrames);
    graph.fireTrigger(capture);
    check(std::ranges::equal(std::get<FloatArray>(graph.readValue(capture, 0)),
                            std::span{left}.last(openstone::maxDspCaptureFrames)) &&
          std::ranges::equal(std::get<FloatArray>(graph.readValue(capture, 1)),
                            std::span{renderedLeft}.last(openstone::maxDspCaptureFrames)) &&
          std::ranges::equal(std::get<FloatArray>(graph.readValue(capture, 2)),
                            std::span{right}.last(openstone::maxDspCaptureFrames)),
          "Get should read the latest NS live frames in chronological order after wrap");
    graph.fireTrigger(sampler);
    check(std::get<float>(graph.readValue(sampler)) == renderedLeft.back() + right.back() && integer(graph, count) == 1,
          "Mono to Float should publish the latest mixed sample on Get");
    graph.setLiveAudio(nullptr);
    check(!graph.liveDspTrace(input, 0, true), "Stopping audio should remove the live readout");
}

void nativeStreamMathAdaptsAndRendersInBothPrecisions()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    openstone::Session session;
    auto& graph = session.graph();
    const auto add = [&](PrimitiveKind kind) { return session.addNode(kind, 0, 0); };
    const auto code = [&](std::string source) {
        const auto id = add(DspCode);
        graph.setDspDraft(id, std::move(source));
        session.compileDsp(id);
        return id;
    };
    const auto capture = [&](int id, double sample) {
        const auto expected = std::vector<std::vector<double>>{std::vector<double>(13, sample)};
        check(graph.captureDsp(id, 13, 1) == expected && graph.captureDsp(id, 13, 5) == expected,
              "The native primitive graph produces the same samples across buffer sizes");
    };

    const auto source = code("monodoubleout out; out = 1.0 + 0.0000000000009094947017729282379150390625;");
    const auto sink = code("streamdoublein in; streamdoubleout out; out = in;");
    const auto addNode = add(Add);
    const auto multiply = add(Multiply);
    const auto subtract = add(Subtract);
    const auto narrow = add(DoubleToStream);
    const auto widen = add(StreamToDouble);
    const auto factor = add(Int);
    const auto unit = add(Float);
    graph.setValue(factor, std::int32_t{2});
    graph.setValue(unit, 1.0F);
    graph.connect({unit, addNode, 1});
    check(graph.outputType(addNode) == Type::Float, "The existing Add keeps its Green behavior");
    graph.connect({source, addNode, 0});
    graph.connect({addNode, multiply, 0});
    graph.connect({factor, multiply, 1});
    graph.connect({multiply, subtract, 0});
    graph.connect({unit, subtract, 1});
    graph.connect({subtract, sink, 0});
    const auto result = 3.0 + 0x1p-39;
    check(graph.outputType(addNode) == Type::MonoDouble && graph.inputType(multiply, 1) == Type::MonoDouble,
          "Double precision and Mono mode propagate across shared operators");
    capture(sink, result);

    graph.disconnectLink({subtract, sink, 0});
    graph.connect({subtract, narrow, 0});
    graph.connect({narrow, widen, 0});
    graph.connect({widen, sink, 0});
    check(graph.outputType(narrow) == Type::Mono && graph.outputType(widen) == Type::MonoDouble,
          "Format conversions keep the processing mode while changing the precision");
    capture(sink, 3.0);
    check(graph.dspTrace(subtract, 0, true).back() == result && graph.dspTrace(narrow, 0, true).back() == 3.0,
          "Swatches distinguish the samples before and after narrowing");
    const auto vacant = add(StreamToDouble);
    const auto wireCount = graph.links().size();
    const auto rejects = [&](SchematicLink link) {
        try {
            graph.connect(link);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    check(rejects({source, vacant, 0}) && graph.links().size() == wireCount,
          "The two precision families require a conversion");
    graph.connect({unit, multiply, 1});
    capture(sink, 5.0);
    check(graph.dspTrace(multiply, 1, false).back() == 3.0,
          "Two Green values wired to one DSP operand are summed as signals");
}

void nativeStreamMathSupportsUnaryAndBinaryInputsForLiveAudio()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    Schematic graph;
    const auto add = [&](PrimitiveKind kind) { return graph.addNode(kind, 0, 0); };
    const auto input = add(AudioIn);
    const auto out = add(AudioOut);
    const auto abs = add(Abs);
    const auto sqrt = add(StreamSquareRoot);
    const auto reciprocal = add(StreamReciprocal);
    const auto inverse = add(StreamInverse);
    const auto sign = add(StreamSign);
    const auto min = add(StreamMin);
    const auto max = add(StreamMax);
    const auto divide = add(Divide);
    const auto ceiling = add(Float);
    const auto divisor = add(Float);
    const auto idle = add(StreamReciprocal);
    graph.setValue(ceiling, 0.75F);
    graph.setValue(divisor, 2.F);
    graph.connect({input, abs, 0});
    graph.connect({abs, sqrt, 0});
    graph.connect({sqrt, reciprocal, 0});
    graph.connect({reciprocal, min, 0});
    graph.connect({ceiling, min, 1});
    graph.connect({min, max, 0});
    graph.connect({max, divide, 0});
    graph.connect({divisor, divide, 1});
    graph.connect({divide, out, 0});
    graph.connect({input, inverse, 0, 1});
    graph.connect({inverse, sign, 0});
    graph.connect({sign, out, 1});
    check(graph.outputType(divide) == Type::Mono && graph.outputType(idle) == Type::Stream,
          "Only the wired operators specialize from Stream to Mono");

    auto patch = graph.prepareAudio(2);
    graph.setLiveAudio(&patch);
    std::array<float, 3> left{-1, -4, 16}, right{4, -2, 0}, renderedLeft{}, renderedRight{};
    check(patch.processor().process({left, right}, {renderedLeft, renderedRight}) &&
          renderedLeft == std::array{0.375F, 0.25F, 0.125F} && renderedRight == std::array{-1.F, 1.F, 1.F},
          "Native operators render from live audio across internal buffer boundaries");
    const auto trace = graph.liveDspTrace(divide, 0, true);
    check(trace && std::ranges::equal(*trace, renderedLeft) && !graph.liveDspTrace(idle, 0, true),
          "A swatch observes the active patch and never schedules a disconnected Stream");
}

void streamRoutingPreservesStaggeredFeedbackInEveryLane()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    constexpr std::array amplitudes{1.0, -2.0, 4.0, 8.0};
    constexpr std::size_t frames = 19;
    for (const bool wide : {false, true}) {
        openstone::Session session;
        auto& graph = session.graph();
        const auto add = [&](PrimitiveKind kind) { return graph.addNode(kind, 0, 0); };
        const auto code = [&](std::string source) {
            const int id = add(DspCode);
            graph.setDspDraft(id, std::move(source));
            session.compileDsp(id);
            return id;
        };
        const std::string precision = wide ? "monodouble" : "mono";
        std::string source = "double phase; double impulse;";
        std::string tap;
        for (int lane = 0; lane < 4; ++lane) {
            const auto name = std::to_string(lane);
            source += precision + "out out" + name + ";";
            tap += precision + "in in" + name + ";" + precision + "out out" + name + ";";
        }
        source += wide ? "impulse = 1.0 + 0.0000000000009094947017729282379150390625;" : "impulse = 1;";
        for (std::size_t lane = 0; lane < amplitudes.size(); ++lane) {
            const auto name = std::to_string(lane);
            source += "out" + name + " = phase == " + name + " ? " + std::to_string(amplitudes[lane]) + " * impulse : 0;";
            tap += "out" + name + " = in" + name + ";";
        }
        source += "phase += 1;";
        const int impulses = code(source);
        const int sink = code(tap);
        const int pack = add(wide ? PackDouble : Pack);
        const int delay = add(wide ? DelayDoubleOneSample : DelayOneSample);
        const int feedback = add(Multiply);
        const int unpack = add(wide ? UnpackDouble : Unpack);
        const int half = graph.addNode(Float, 0, 0, 0.5f);
        for (int lane = 0; lane < 4; ++lane) {
            graph.connect({impulses, pack, lane, lane});
            graph.connect({unpack, sink, lane, lane});
        }
        check(graph.outputType(delay) == (wide ? Type::StreamDouble : Type::Stream),
              "The disconnected delay remains a generic stream");
        graph.connect({pack, delay, 0});
        graph.connect({delay, unpack, 0});
        graph.connect({delay, feedback, 0});
        graph.connect({half, feedback, 1});
        graph.connect({feedback, delay, 0});
        const auto packed = wide ? Type::Mono4Double : Type::Mono4;
        check(graph.inputType(delay, 0) == packed && graph.outputType(delay) == packed,
              "The delay resolves both ports to the packed stream type");

        std::vector<std::vector<double>> expected(amplitudes.size(), std::vector<double>(frames));
        for (std::size_t lane = 0; lane < amplitudes.size(); ++lane) {
            const double impulse = amplitudes[lane] * (wide ? 1.0 + 0x1p-40 : 1.0);
            for (std::size_t frame = lane + 1; frame < frames; ++frame) {
                expected[lane][frame] = std::ldexp(impulse, -static_cast<int>(frame - lane - 1));
            }
        }
        for (const std::size_t block : {1, 3, 8, 19}) {
            check(graph.captureDsp(sink, frames, block) == expected,
                  "Every lane retains the staggered impulse and exact decay across arbitrary block boundaries");
        }
        const int module = session.makeModule(std::array{delay});
        check(module >= 0 && graph.captureDsp(sink, frames, 5) == expected,
              "A module boundary preserves the one-sample feedback delay");
        graph.disconnectInput(pack, 1);
        std::ranges::fill(expected[1], 0.0);
        check(graph.captureDsp(sink, frames, 5) == expected,
              "An unconnected Pack input silences only its corresponding lane");
    }
}

void stagedDspCombAcceptsFeedbackAcrossAModuleInput(bool stageThreeOutput)
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    openstone::Session session;
    auto& graph = session.graph();
    const auto code = [&](std::string source) {
        const auto id = graph.addNode(DspCode, 0, 0);
        graph.setDspDraft(id, std::move(source));
        session.compileDsp(id);
        return id;
    };
    const auto impulse = code("monoout x; float n; x = n == 0 ? 1 : 0; n += 1;");
    const auto gain = graph.addNode(Multiply, 0, 0);
    const auto half = graph.addNode(Float, 0, 0, .5F);
    const auto pack = graph.addNode(Pack, 0, 0);
    const auto unpack = graph.addNode(Unpack, 0, 0);
    const auto readout = code("monoin x; monoout y; stage(2) { y = x; }");
    const auto delay = code(stageThreeOutput ? R"dsp(# COMPATIBILITY_3_0_8 // remove this line and check for errors
streamin in;
streamout out;
streamin delay;
float mem[44100];
float index;
stage(2)
{
    mem[index] = in;
}
stage(3)
{
    index = index + 1;
    index = (index<delay)&index;
    out = mem[index];
}
)dsp" : R"dsp(
        streamin in;
        streamout out;
        float buffer[3];
        int position;
        stage(0) { position = 0; }
        stage(1) { out = buffer[position]; }
        stage(3) { buffer[position] = in; position = (position + 1) % 3; }
    )dsp");
    graph.connect({delay, gain, 0});
    check(graph.outputType(delay) == Type::Stream && graph.canSuggestLink({gain, delay, 0}),
          "The editor allows feedback through staged DSP before the grey Stream ports resolve");
    graph.connect({gain, delay, 0});
    graph.connect({impulse, pack, 0});
    graph.connect({pack, delay, 0});
    if (stageThreeOutput) {
        const auto length = graph.addNode(Float, 0, 0, 3.F);
        graph.connect({length, delay, 1});
    }
    graph.connect({half, gain, 1});
    graph.connect({delay, unpack, 0});
    graph.connect({unpack, readout, 0});

    std::vector<double> expected(17);
    for (int i = 3; i < 17; i += 3) expected[i] = std::ldexp(1.0, 1 - i / 3);
    for (std::size_t block : {1, 2, 7, 17}) {
        check(graph.captureDsp(readout, expected.size(), block).front() == expected,
              "The staged comb produces the expected three-sample echoes across render blocks");
    }

    // Put both wires on the same exterior inlet, as in a reusable Fixed Delay module.
    graph.disconnectLink({gain, delay, 0});
    const auto module = session.makeModule(std::array{delay});
    graph.connect({gain, module, 0});
    check(graph.inputType(module, 0) == Type::Mono4 &&
          graph.captureDsp(readout, expected.size(), 7).front() == expected,
          "A staged comb still works with both sources wired to the same module input");

    auto& child = *graph.module(module);
    child.setDspDraft(delay, "streamin in; streamout out; out = in;");
    check(session.enterModule(module), "Open the delay module for editing");
    session.compileDsp(delay);
    const auto immediate = graph.captureDsp(readout, expected.size(), 7).front();
    const auto first = std::ranges::find(immediate, 1.0);
    check(std::distance(first, immediate.end()) >= 3 && *(first + 1) == 0.5 && *(first + 2) == 0.25,
          "Removing the explicit delay leaves a one-sample feedback recurrence");
}

void stageOneCanReadSummedEarlySignals()
{
    using enum PrimitiveKind;
    openstone::Session session;
    auto& graph = session.graph();
    const auto code = [&](std::string text) {
        const auto id = graph.addNode(DspCode, 0, 0);
        graph.setDspDraft(id, std::move(text));
        session.compileDsp(id);
        return id;
    };
    const auto ramp = code(R"dsp(
        monoout out;
        float n;
        stage(0) { n = 3; }
        stage(1) { out = n; }
        stage(3) { n += 1; }
    )dsp");
    const auto offset = graph.addNode(Float, 0, 0, 2.F);
    const auto mixer = code(R"dsp(
        monoin in;
        monoout early;
        monoout ordinary;
        monoout explicit;
        stage(1) { early = in; }
        ordinary = early * 2;
        stage(2) { explicit = early * 2; }
    )dsp");
    graph.connect({ramp, mixer, 0});
    graph.connect({offset, mixer, 0});
    for (std::size_t block : {1, 3, 5}) {
        const auto output = graph.captureDsp(mixer, 5, block);
        check(output[0] == std::vector<double>{5, 6, 7, 8, 9} &&
              output[1] == std::vector<double>{10, 12, 14, 16, 18} && output[2] == output[1],
              "Stage 1 can read fan-in and both implicit and explicit Stage 2 see the current result");
    }

    graph.connect({mixer, mixer, 0, 1});
    for (std::size_t block : {1, 3, 5}) {
        const auto output = graph.captureDsp(mixer, 5, block);
        check(output[0] == std::vector<double>{5, 16, 39, 86, 181},
              "Stage 1 fan-in reads the previous sample from its Stage 2 feedback wire");
    }
}

void packedAllpassCanFeedTheNextLaneWithoutAnExtraSample()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    openstone::Session session;
    auto& root = session.graph();
    const auto makeCode = [&](openstone::Schematic& graph, std::string source) {
        const int node = graph.addNode(DspCode, 0, 0);
        graph.setDspDraft(node, std::move(source));
        session.compileDsp(node);
        return node;
    };
    const int impulse = makeCode(root, "monoout y; float n; y = n == 0 ? 1 : 0; n += 1;");
    const int sink = makeCode(root, "monoin x; monoout y; y = x;");
    const int delayLengths = root.addNode(Pack, 0, 0);
    const int pack = root.addNode(Pack, 0, 0);
    const int unpack = root.addNode(Unpack, 0, 0);
    const int module = root.addNode(Module, 0, 0);
    const int half = root.addNode(Float, 0, 0, 0.5F);
    auto& allpass = *root.module(module);
    const int in = allpass.addNode(ModuleInput, 0, 0);
    const int delay = allpass.addNode(ModuleInput, 0, 100);
    const int feedback = allpass.addNode(ModuleInput, 0, 200);
    const int out = allpass.addNode(ModuleOutput, 700, 0);
    allpass.setModulePortType(in, Type::Mono4);
    allpass.setModulePortType(delay, Type::Mono4);
    allpass.setModulePortType(feedback, Type::Float);
    allpass.setModulePortType(out, Type::Mono4);
    const int dry = allpass.addNode(Multiply, 400, 300);
    const int wet = allpass.addNode(Multiply, 400, 0);
    const int gain = allpass.addNode(Float, 180, 0);
    const int inverse = allpass.addNode(Inverse, 180, 300);
    check(session.enterModule(module), "Open the allpass module");
    const int denormal = makeCode(allpass, "streamin in; streamout out; out = denorm(in);");
    const int delayModule = allpass.addNode(Module, 200, 100);
    auto& fixed = *allpass.module(delayModule);
    const int fixedIn = fixed.addNode(ModuleInput, 0, 0);
    const int fixedLength = fixed.addNode(ModuleInput, 0, 100);
    const int fixedOut = fixed.addNode(ModuleOutput, 450, 0);
    fixed.setModulePortType(fixedIn, Type::Mono4);
    fixed.setModulePortType(fixedLength, Type::Mono4);
    fixed.setModulePortType(fixedOut, Type::Mono4);
    check(session.enterModule(delayModule), "Open the nested Fixed Delay");
    const int fixedCode = makeCode(fixed, R"dsp(
        streamin in;
        streamout out;
        streamin delay;
        float mem[24];
        float index;
        stage(2) { mem[index] = in; }
        stage(3) {
            index += 1;
            index = (index < delay) & index;
            out = mem[index];
        }
    )dsp");
    fixed.connect({fixedIn, fixedCode, 0});
    fixed.connect({fixedLength, fixedCode, 1});
    fixed.connect({fixedCode, fixedOut, 0});
    check(session.navigate(1), "Return to the allpass module");

    allpass.connect({in, delayModule, 0});
    allpass.connect({denormal, delayModule, 0});
    allpass.connect({delay, delayModule, 1});
    allpass.connect({feedback, gain, 0});
    allpass.connect({feedback, inverse, 0});
    allpass.connect({inverse, dry, 1});
    allpass.connect({gain, wet, 0});
    allpass.connect({delayModule, wet, 1});
    allpass.connect({dry, wet, 1});
    allpass.connect({wet, denormal, 0});
    allpass.connect({delayModule, out, 0});
    allpass.connect({dry, out, 0});

    constexpr std::array lengths{2, 3, 5, 7};
    for (int lane = 0; lane < 4; ++lane) {
        const int length = root.addNode(Float, 0, 0, static_cast<float>(lengths[lane]));
        root.connect({length, delayLengths, lane});
    }
    root.connect({delayLengths, module, 1});
    root.connect({half, module, 2});
    root.connect({module, unpack, 0});
    root.connect({impulse, pack, 0});
    for (int lane = 0; lane < 3; ++lane) root.connect({unpack, pack, lane + 1, lane});
    root.connect({pack, module, 0});
    root.connect({unpack, sink, 0, 3});
    session.connect({in, dry, 0}); // The highlighted wire inside the already connected allpass.

    constexpr int frames = 24;
    std::array<std::array<float, frames>, 4> written{};
    std::vector<double> expected(frames);
    for (int n = 0; n < frames; ++n) {
        float signal = n == 0 ? 1.F : 0.F;
        for (int lane = 0; lane < 4; ++lane) {
            const auto delayed = n < lengths[lane] ? 0.F : written[lane][n - lengths[lane]];
            const float dry = -0.5F * signal;
            written[lane][n] = signal + 0.5F * (delayed + dry);
            signal = delayed + dry;
        }
        expected[n] = signal;
    }
    check(expected.front() == 0.0625, "The fourth lane receives the dry impulse at sample zero");
    for (std::size_t block : {1, 5, 9, 24}) {
        check(root.captureDsp(sink, frames, block).front() == expected,
              "The nested allpass matches an independent four-filter cascade across buffer sizes");
    }
}

void shortedMonoModulesFeedBackThePreviousSample()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    openstone::Session session;
    auto& graph = session.graph();
    const auto makePassthrough = [&] {
        const int module = graph.addNode(Module, 0, 0);
        auto& inner = *graph.module(module);
        const int in = inner.addNode(ModuleInput, 0, 0);
        const int out = inner.addNode(ModuleOutput, 200, 0);
        inner.setModulePortType(in, Type::Mono);
        inner.setModulePortType(out, Type::Mono);
        inner.connect({in, out, 0});
        return module;
    };
    const int first = makePassthrough();
    const int second = makePassthrough();
    const int impulse = graph.addNode(DspCode, 0, 0);
    const int sink = graph.addNode(DspCode, 0, 0);
    graph.setDspDraft(impulse, "monoout y; float n; y = n == 0 ? 1 : (n == 3 ? 1 : 0); n += 1;");
    graph.setDspDraft(sink, "monoin x; monoout y; y = x;");
    session.compileDsp(impulse);
    session.compileDsp(sink);
    session.connect({first, second, 0});
    session.connect({second, first, 0});
    session.connect({first, sink, 0});
    check(graph.captureDsp(sink, 7, 3).front() == std::vector<double>(7), "An undriven passthrough ring is silent");

    session.connect({impulse, second, 0});
    const std::vector<double> expected{1, 1, 1, 2, 2, 2, 2};
    for (std::size_t block : {1, 3, 7}) {
        check(graph.captureDsp(sink, expected.size(), block).front() == expected,
              "The two shorted modules sum the input with the previous sample");
    }

    const int green = graph.addNode(Float, 0, 0);
    check(!graph.canSuggestLink({green, green, 0}), "Green value feedback is still rejected");
}

void impulseFiresOnTheFirstSampleAcrossBufferSizes()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    Schematic graph;
    const int impulse = graph.addNode(Impulse, 0, 0);
    const int delay = graph.addNode(DelayOneSample, 0, 0);
    const int audio = graph.addNode(AudioOut, 0, 0);
    check(openstone::primitive(Impulse).inputs.empty() && graph.outputType(impulse) == Type::Stream,
          "Impulse has no inputs and a generic Stream output");
    graph.connect({impulse, audio, 0});
    graph.connect({impulse, delay, 0});
    graph.connect({delay, audio, 1});
    check(graph.outputType(impulse) == Type::Mono, "Audio Out specializes Impulse to Mono");

    constexpr std::size_t frames = 259;
    std::array<float, frames> direct{}, delayed{};
    direct[0] = 1;
    delayed[1] = 1;
    for (std::size_t block : {1, 2, 7, 16, 64, 129}) {
        auto patch = graph.prepareAudio(block);
        std::array<float, frames> left{}, right{};
        const auto render = [&](std::size_t start, std::size_t count) {
            check(patch.processor().process({}, {std::span{left}.subspan(start, count),
                                                 std::span{right}.subspan(start, count)}),
                  "Render the same audio patch over successive callbacks");
        };
        render(0, 1);
        render(1, frames - 1);
        check(left == direct && right == delayed,
              "Each fresh patch emits one unit impulse, and Delay1 moves it to sample one");
    }
}

void packedImpulseFiresInAllFourLanes()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    openstone::Session session;
    auto& graph = session.graph();
    const int impulse = graph.addNode(Impulse, 0, 0);
    const int unpack = graph.addNode(Unpack, 0, 0);
    const int tap = graph.addNode(DspCode, 0, 0);
    graph.setDspDraft(tap, R"(
        monoin a; monoin b; monoin c; monoin d;
        monoout w; monoout x; monoout y; monoout z;
        w = a; x = b; y = c; z = d;
    )");
    session.compileDsp(tap);
    graph.connect({impulse, unpack, 0});
    for (int lane = 0; lane < 4; ++lane) graph.connect({unpack, tap, lane, lane});
    check(graph.outputType(impulse) == Type::Mono4, "Unpack specializes Impulse to Mono4");

    constexpr int frames = 31;
    std::vector<double> pulse(frames);
    pulse.front() = 1;
    const std::vector<std::vector<double>> expected(4, pulse);
    for (std::size_t block : {1, 5, 16, frames}) {
        check(graph.captureDsp(tap, frames, block) == expected,
              "Each new capture starts with a unit impulse in every lane");
    }
}

void streamTrigonometryUsesTurnsAtBothPrecisions()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    constexpr double delta = 0.0000000000009094947017729282379150390625;
    constexpr std::array phases{0., .125, -.125, 1.125, -1.125, .375, -.375, .125 + delta, .5, 1.};
    struct Case {
        PrimitiveKind single;
        PrimitiveKind wide;
        double (*function)(double);
    };
    const std::array cases{
        Case{StreamSine, DoubleStreamSine, std::sin},
        Case{StreamCosine, DoubleStreamCosine, std::cos},
        Case{StreamTangent, DoubleStreamTangent, std::tan}
    };
    for (const bool wide : {false, true}) {
        openstone::Session session;
        auto& graph = session.graph();
        const auto add = [&](PrimitiveKind kind) { return graph.addNode(kind, 0, 0); };
        const auto code = [&](std::string source) {
            const int id = add(DspCode);
            graph.setDspDraft(id, std::move(source));
            session.compileDsp(id);
            return id;
        };
        const std::string stream = wide ? "streamdouble" : "stream";
        const std::string mono = wide ? "monodouble" : "mono";
        const int source = code(stream + R"(out phase;
            float n;
            phase = 0;
            phase = n == 1 ? .125 : phase;
            phase = n == 2 ? -.125 : phase;
            phase = n == 3 ? 1.125 : phase;
            phase = n == 4 ? -1.125 : phase;
            phase = n == 5 ? .375 : phase;
            phase = n == 6 ? -.375 : phase;
            phase = n == 7 ? .1250000000009094947017729282379150390625 : phase;
            phase = n == 8 ? .5 : phase;
            phase = n == 9 ? 1 : phase;
            n += 1;
        )");
        const int packedSource = code(stream + R"(out phase;
            phase = .125;
            phase{1} = -.375;
            phase{2} = .0625;
            phase{3} = -.1875;
        )");
        const int tap = code(mono + "in in0; " + mono + "out out0; out0 = in0;");
        const int packedTap = code(mono + "in a; " + mono + "in b; " + mono + "in c; " + mono + "in d; " +
                                   mono + "out w; " + mono + "out x; " + mono + "out y; " + mono + "out z; " +
                                   "w = a; x = b; y = c; z = d;");
        const auto generic = wide ? Type::StreamDouble : Type::Stream;
        for (const auto& test : cases) {
            const auto kind = wide ? test.wide : test.single;
            const int trig = add(kind);
            const auto& spec = openstone::primitive(kind);
            check(spec.inputs.size() == 1 && spec.outputs.size() == 1 &&
                  graph.inputType(trig, 0) == generic && graph.outputType(trig) == generic,
                  "Stream trigonometry exposes one input and output at the selected precision");
            graph.connect({source, trig, 0});
            graph.connect({trig, tap, 0});
            const auto expected = [&](double phase) {
                return test.function(2 * std::numbers::pi * std::fmod(phase, 1.));
            };
            const double tolerance = wide ? 2e-14 : 1e-6;
            const auto samples = graph.captureDsp(tap, phases.size(), 1).front();
            for (std::size_t i = 0; i < phases.size(); ++i) {
                check(std::abs(samples[i] - expected(phases[i])) < tolerance,
                      "The stream functions evaluate signed and wrapped phases in turns");
            }
            check(samples[1] == samples[3] && samples[2] == samples[4] &&
                  (wide ? std::abs(samples[7] - samples[1]) > 1e-13 : samples[7] == samples[1]),
                  "Whole turns repeat and only the double connector retains the sub-float phase change");
            for (const std::size_t block : {2, 3, 4, 7, 10}) {
                check(graph.captureDsp(tap, phases.size(), block).front() == samples,
                      "Trigonometry output is independent of buffer size");
            }
            graph.disconnectInput(tap, 0);

            const int packedTrig = add(kind);
            const int unpack = add(wide ? UnpackDouble : Unpack);
            graph.connect({packedSource, packedTrig, 0});
            graph.connect({packedTrig, unpack, 0});
            for (int lane = 0; lane < 4; ++lane) graph.connect({unpack, packedTap, lane, lane});
            constexpr std::array packedPhases{.125, -.375, .0625, -.1875};
            const auto packed = graph.captureDsp(packedTap, 11, 3);
            for (std::size_t lane = 0; lane < packedPhases.size(); ++lane) {
                check(std::ranges::all_of(packed[lane], [&](double sample) {
                    return std::abs(sample - expected(packedPhases[lane])) < tolerance;
                }), "Each packed lane computes its own trigonometric value");
                graph.disconnectInput(packedTap, static_cast<int>(lane));
            }
        }
    }
}

void streamComparisonsKeepPrecisionAndBooleanLanesAcrossBufferSizes()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    struct Comparison {
        PrimitiveKind single;
        PrimitiveKind wide;
        bool (*matches)(double, double);
    };
    const std::array comparisons{
        Comparison{StreamEqualTo, DoubleStreamEqualTo, [](double a, double b) { return a == b; }},
        Comparison{StreamLess, DoubleStreamLess, [](double a, double b) { return a < b; }},
        Comparison{StreamLessOrEqualTo, DoubleStreamLessOrEqualTo, [](double a, double b) { return a <= b; }},
        Comparison{StreamGreater, DoubleStreamGreater, [](double a, double b) { return a > b; }},
        Comparison{StreamGreaterOrEqualTo, DoubleStreamGreaterOrEqualTo, [](double a, double b) { return a >= b; }},
    };
    struct Operand {
        std::string_view expression;
        double value;
    };
    constexpr Operand negative{"-2", -2}, zero{"0", 0}, negativeZero{"-0.0", -0.0}, unit{"1", 1};
    constexpr Operand aboveUnit{"1.0000000000009094947017729282379150390625", 1 + 0x1p-40};
    constexpr Operand nan{"0 / (phase - phase)", std::numeric_limits<double>::quiet_NaN()};
    constexpr std::array operands{
        std::pair{negative, unit}, std::pair{zero, negativeZero}, std::pair{unit, negative},
        std::pair{aboveUnit, unit}, std::pair{unit, aboveUnit}, std::pair{nan, unit},
    };
    constexpr std::size_t frames = 11;
    for (const bool wide : {false, true}) {
        for (const bool packed : {false, true}) {
            openstone::Session session;
            auto& graph = session.graph();
            const auto add = [&](PrimitiveKind kind) { return graph.addNode(kind, 0, 0); };
            const auto code = [&](std::string source) {
                const int id = add(DspCode);
                graph.setDspDraft(id, std::move(source));
                session.compileDsp(id);
                return id;
            };
            const std::string signal = wide ? "streamdouble" : "stream";
            const Type signalType = wide ? Type::StreamDouble : Type::Stream;
            const std::string mono = wide ? "monodouble" : "mono";
            const std::string boolConnector = packed ? "streambool" : "monobool";
            const int lanes = packed ? 4 : 1;
            const auto pairAt = [&](std::size_t frame, int lane) {
                return operands[(frame + lane) % operands.size()];
            };
            std::string sourceCode = signal + "out a; " + signal + "out b; " +
                (wide ? "double phase;\n" : "float phase;\n");
            for (int lane = 0; lane < lanes; ++lane) {
                const std::string suffix = packed ? "{" + std::to_string(lane) + "}" : "";
                std::string assignA = "a" + suffix + " = ";
                std::string assignB = "b" + suffix + " = ";
                for (std::size_t phase = 0; phase < operands.size() - 1; ++phase) {
                    const auto [a, b] = pairAt(phase, lane);
                    const auto condition = "phase == " + std::to_string(phase) + " ? ";
                    assignA += condition + std::string(a.expression) + " : ";
                    assignB += condition + std::string(b.expression) + " : ";
                }
                const auto [lastA, lastB] = pairAt(operands.size() - 1, lane);
                sourceCode += assignA + std::string(lastA.expression) + ";\n";
                sourceCode += assignB + std::string(lastB.expression) + ";\n";
            }
            sourceCode += "phase = phase == " + std::to_string(operands.size() - 1) + " ? 0 : phase + 1;";
            const int source = code(std::move(sourceCode));
            const int decoder = code(boolConnector + "in match; " + (packed ? signal : mono) +
                                     "out value; value = match ? 1 : 0;");
            int tap = decoder;
            if (packed) {
                const int unpack = add(wide ? UnpackDouble : Unpack);
                graph.connect({decoder, unpack, 0});
                tap = code(mono + "in a; " + mono + "in b; " + mono + "in c; " + mono + "in d; " +
                           mono + "out w; " + mono + "out x; " + mono + "out y; " + mono + "out z; " +
                           "w = a; x = b; y = c; z = d;");
                for (int lane = 0; lane < lanes; ++lane) graph.connect({unpack, tap, lane, lane});
            }
            for (const auto& comparison : comparisons) {
                const auto kind = wide ? comparison.wide : comparison.single;
                const int node = add(kind);
                check(openstone::primitive(kind).inputs.size() == 2 && openstone::primitive(kind).outputs.size() == 1 &&
                      graph.inputType(node, 0) == signalType && graph.inputType(node, 1) == signalType &&
                      graph.outputType(node) == Type::StreamBoolean,
                      "Both precisions declare two numeric inputs and one StreamBoolean output");
                graph.connect({source, node, 0});
                graph.connect({source, node, 1, 1});
                graph.connect({node, decoder, 0});
                check(graph.outputType(node) == (packed ? Type::StreamBoolean : Type::MonoBoolean),
                      "The comparison result follows the stream mode");
                const auto atPrecision = [wide](double v) {
                    return wide ? v : static_cast<double>(static_cast<float>(v));
                };
                for (std::size_t block : {1, 2, 3, 5, 8, 11}) {
                    const auto captured = graph.captureDsp(tap, frames, block);
                    for (int lane = 0; lane < lanes; ++lane) {
                        for (std::size_t frame = 0; frame < frames; ++frame) {
                            const auto [a, b] = pairAt(frame, lane);
                            if (captured[lane][frame] != comparison.matches(atPrecision(a.value), atPrecision(b.value))) {
                                throw std::runtime_error(std::string(openstone::primitive(kind).name) +
                                    " gave the wrong result in lane " + std::to_string(lane + 1) +
                                    " at frame " + std::to_string(frame) + " with block size " + std::to_string(block));
                            }
                        }
                    }
                }
                graph.disconnectInput(decoder, 0);
            }
        }
    }
}

void floatArrayReadRoundsAndWrapsIndicesAcrossBufferSizes()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    constexpr std::size_t frames = 13;
    constexpr std::array indices{"0", "0.5", "1.5", "2.5", "-0.5", "-1", "-1.5", "2047.5", "2048", "2049",
                                 "-2049", "0 / (phase - phase)", "1 / (phase - phase)"};
    const std::vector<double> expected{1, 1, 3, 3, 1, 2048, 2047, 1, 1, 2, 2048, 1, 1};
    std::string array;
    for (int value = 1; value <= 2049; ++value) array += std::to_string(value) + "\n";
    for (const bool packed : {false, true}) {
        openstone::Session session;
        auto& graph = session.graph();
        const auto add = [&](PrimitiveKind kind) { return graph.addNode(kind, 0, 0); };
        const auto code = [&](std::string source) {
            const int id = add(DspCode);
            graph.setDspDraft(id, std::move(source));
            session.compileDsp(id);
            return id;
        };
        const int reader = add(FloatArrayRead);
        check(graph.inputType(reader, 0) == Type::FloatArray && graph.inputType(reader, 1) == Type::Stream &&
              graph.outputType(reader) == Type::Stream,
              "Float Array Read takes a Green Float Array and a sample-rate numeric index");
        std::string fixture = "streamout index; float phase; index = 0;";
        for (std::size_t frame = 1; frame < indices.size(); ++frame) {
            fixture += "index = phase == " + std::to_string(frame) + " ? " + indices[frame] + " : index;";
        }
        fixture += "index{1} = 1.5; index{2} = -1.5; index{3} = 2047.5; phase += 1;";
        const int source = code(std::move(fixture));
        graph.connect({source, reader, 1});
        int tap;
        if (packed) {
            const int unpack = add(Unpack);
            graph.connect({reader, unpack, 0});
            tap = code("monoin a; monoin b; monoin c; monoin d; "
                       "monoout w; monoout x; monoout y; monoout z; w = a; x = b; y = c; z = d;");
            for (int lane = 0; lane < 4; ++lane) graph.connect({unpack, tap, lane, lane});
        } else {
            tap = code("monoin value; monoout sample; sample = value;");
            graph.connect({reader, tap, 0});
        }
        check(graph.captureDsp(tap, frames, 5).front() == std::vector<double>(frames),
              "An unconnected array supplies a silent table");
        const int text = graph.addNode(MultilineString, 0, 0, array);
        graph.connect({text, reader, 0});
        for (const auto block : std::array<std::size_t, 6>{1, 2, 3, 5, 8, frames}) {
            const auto samples = graph.captureDsp(tap, frames, block);
            check(samples.front() == expected, "Float Array Read rounds ties to even and wraps to the first 2048 slots");
            if (packed) {
                check(samples.size() == 4 && samples[1] == std::vector<double>(frames, 3) &&
                      samples[2] == std::vector<double>(frames, 2047) && samples[3] == std::vector<double>(frames, 1),
                      "Every packed lane reads its own index from the shared table");
            }
        }
    }
}

void floatArrayReadPublishesEditsToRunningAudio()
{
    using enum PrimitiveKind;
    Schematic graph;
    const int text = graph.addNode(MultilineString, 0, 0, std::string{"2\n4\n8"});
    const int index = graph.addNode(AudioIn, 0, 0);
    const int reader = graph.addNode(FloatArrayRead, 0, 0);
    const int output = graph.addNode(AudioOut, 0, 0);
    graph.connect({text, reader, 0});
    graph.connect({index, reader, 1});
    graph.connect({reader, output, 0});
    auto patch = graph.prepareAudio(3);
    const std::array indices{0.F, 1.F, 2.F, 3.F, 2048.F};
    std::array<float, indices.size()> left{}, right{};
    const auto verify = [&](const auto& expected) {
        check(patch.processor().process({indices, {}}, {left, right}) && left == expected,
              "The running graph reads the current array consistently through buffer boundaries");
    };
    verify(std::array{2.F, 4.F, 8.F, 0.F, 2.F});
    graph.setValue(text, std::string{"1\n3\n5\n7"});
    check(graph.refreshAudioControls(patch), "Replacing the array keeps the prepared audio graph");
    graph.setValue(text, std::string{"9"});
    check(graph.refreshAudioControls(patch), "A newer array can replace a pending edit before it is rendered");
    verify(std::array{9.F, 0.F, 0.F, 0.F, 9.F});
    graph.setValue(text, std::string{});
    check(graph.refreshAudioControls(patch), "The array can be cleared without a graph rebuild");
    verify(std::array<float, indices.size()>{});
}

void streamSelectRoutesEachSampleAndLaneAtBothPrecisions()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    constexpr std::size_t frames = 13;
    for (const bool wide : {false, true}) {
        for (const bool packed : {false, true}) {
            openstone::Session session;
            auto& graph = session.graph();
            const auto add = [&](PrimitiveKind kind) { return graph.addNode(kind, 0, 0); };
            const auto code = [&](std::string source) {
                const int id = add(DspCode);
                graph.setDspDraft(id, std::move(source));
                session.compileDsp(id);
                return id;
            };
            const std::string signal = wide ? "streamdouble" : "stream";
            const std::string mono = wide ? "monodouble" : "mono";
            const auto type = wide ? Type::StreamDouble : Type::Stream;
            const auto kind = wide ? DoubleSelect : StreamSelect;
            const int select = add(kind);
            check(openstone::primitive(kind).inputs.size() == 3 && openstone::primitive(kind).outputs.size() == 1 &&
                  graph.inputType(select, 0) == type && graph.inputType(select, 1) == type &&
                  graph.inputType(select, 2) == Type::StreamBoolean && graph.outputType(select) == type,
                  "Both Stream Select variants declare two numeric inputs and a StreamBoolean selector");
            const int source = code(signal + "out low; " + signal + "out high; streamout control; " +
                (wide ? "double phase;" : "float phase;") + R"(
                low = phase + 1.0000000000009094947017729282379150390625;
                low{1} = low(0) + 16;
                low{2} = low(0) + 32;
                low{3} = low(0) + 48;
                high = -low;
                control = (phase % 4) >= 2 ? 1 : -1;
                control{1} = -control(0);
                control{2} = ((phase + 1) % 4) >= 2 ? 1 : -1;
                control{3} = -control(2);
                low = control > 0 ? 0 / (phase - phase) : low;
                high = control > 0 ? high : 0 / (phase - phase);
                phase += 1;
            )");
            const int mask = add(StreamGreater);
            graph.connect({source, select, 0});
            graph.connect({source, select, 1, 1});
            graph.connect({source, mask, 0, 2});
            int tap;
            if (packed) {
                const int unpack = add(wide ? UnpackDouble : Unpack);
                graph.connect({select, unpack, 0});
                tap = code(mono + "in a; " + mono + "in b; " + mono + "in c; " + mono + "in d; " +
                           mono + "out w; " + mono + "out x; " + mono + "out y; " + mono + "out z; " +
                           "w = a; x = b; y = c; z = d;");
                for (int lane = 0; lane < 4; ++lane) graph.connect({unpack, tap, lane, lane});
            } else {
                tap = code(mono + "in source; " + mono + "out sample; sample = source;");
                graph.connect({select, tap, 0});
            }
            const double offset = wide ? 0x1p-40 : 0.0;
            check(graph.captureDsp(tap, 1, 1).front().front() == 1 + offset,
                  "An unwired selector starts on the False input");
            graph.connect({mask, select, 2});
            check(graph.inputType(select, 2) == (packed ? Type::StreamBoolean : Type::MonoBoolean),
                  "The Boolean input follows the stream mode");
            for (const auto block : std::array<std::size_t, 6>{1, 2, 3, 5, 8, frames}) {
                const auto captured = graph.captureDsp(tap, frames, block);
                for (std::size_t lane = 0; lane < captured.size(); ++lane) {
                    for (std::size_t frame = 0; frame < frames; ++frame) {
                        const bool flag = (frame + lane / 2 + 2 * (lane % 2)) % 4 >= 2;
                        const double magnitude = frame + 1 + 16 * lane + offset;
                        const double expected = flag ? -magnitude : magnitude;
                        if (captured[lane][frame] != expected) {
                            throw std::runtime_error(std::string(openstone::primitive(kind).name) + " at frame " +
                                std::to_string(frame) + " lane " + std::to_string(lane + 1) + " with block size " +
                                std::to_string(block) + " gave " + std::to_string(captured[lane][frame]) +
                                " when " + std::to_string(expected) + " was selected");
                        }
                    }
                }
            }
        }
    }
}

void streamSampleAndHoldCapturesAtFrequencyOrZeroCrossings()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    constexpr std::size_t frames = 17;
    constexpr std::array frequencyFrame{0, 0, 0, 0, 4, 4, 4, 4, 8, 8, 8, 11, 11, 11, 11, 11, 16};
    constexpr std::array crossingFrame{0, 0, 0, 3, 3, 3, 3, 7, 7, 7, 7, 7, 7, 7, 7, 15, 15};
    for (const bool wide : {false, true}) {
        openstone::Session session;
        auto& graph = session.graph();
        const auto add = [&](PrimitiveKind kind) { return graph.addNode(kind, 0, 0); };
        const auto code = [&](std::string source) {
            const int id = add(DspCode);
            graph.setDspDraft(id, std::move(source));
            session.compileDsp(id);
            return id;
        };
        const auto kind = wide ? DoubleSampleAndHold : StreamSampleAndHold;
        const auto type = wide ? Type::StreamDouble : Type::Stream;
        const std::string signal = wide ? "streamdouble" : "stream";
        const std::string mono = wide ? "monodouble" : "mono";
        const std::string fixture = signal + "out value; " + signal + "out clock; " +
            (wide ? "double frame;" : "float frame;") +
            "value = frame + 1.0000000000009094947017729282379150390625;";
        const auto sample = [wide](int frame, int lane = 0) {
            return frame + 1 + lane * 32 + (wide ? 0x1p-40 : 0.0);
        };
        const int source = code(fixture + R"(
            clock = .25;
            clock = ((frame == 2) | (frame == 6) | (frame == 11) | (frame == 14)) ? -.5 : clock;
            clock = ((frame == 3) | (frame == 7) | (frame == 8) |
                     (frame == 10) | (frame == 13) | (frame == 15)) ? .5 : clock;
            clock = ((frame == 9) | (frame == 12)) ? 0 : clock;
            frame += 1;
        )");
        const int hold = add(kind);
        const int tap = code(mono + "in held; " + mono + "out sample; sample = held;");
        check(graph.inputType(hold, 0) == type && graph.inputType(hold, 1) == type &&
              graph.inputType(hold, 2) == Type::Boolean && graph.outputType(hold) == type,
              "Stream Sample and Hold declares two signals, a Green mode input, and a same-precision output");
        graph.connect({source, hold, 0});
        graph.connect({hold, tap, 0});
        check(graph.captureDsp(tap, frames, 5).front() == std::vector<double>(frames, sample(0)),
              "The default frequency mode captures the first sample and holds it at zero frequency");
        graph.connect({source, hold, 1, 1});

        const int packedSource = code(fixture + R"(
            value{1} = value(0) + 32;
            value{2} = value(0) + 64;
            value{3} = value(0) + 96;
            clock = .25;
            clock{1} = -.25;
            clock{2} = frame < 4 ? -.5 : .5;
            clock{3} = 0;
            frame += 1;
        )");
        const int packedHold = add(kind);
        const int unpack = add(wide ? UnpackDouble : Unpack);
        const int packedTap = code(mono + "in a; " + mono + "in b; " + mono + "in c; " + mono + "in d; " +
                                   mono + "out w; " + mono + "out x; " + mono + "out y; " + mono + "out z; " +
                                   "w = a; x = b; y = c; z = d;");
        graph.connect({packedSource, packedHold, 0});
        graph.connect({packedSource, packedHold, 1, 1});
        graph.connect({packedHold, unpack, 0});
        for (int lane = 0; lane < 4; ++lane) graph.connect({unpack, packedTap, lane, lane});
        const int zeroCross = graph.addNode(Boolean, 0, 0, false);
        graph.connect({zeroCross, hold, 2});
        graph.connect({zeroCross, packedHold, 2});

        for (const bool crossing : {false, true}) {
            graph.setValue(zeroCross, crossing);
            const auto& expectedFrame = crossing ? crossingFrame : frequencyFrame;
            for (const auto block : std::array<std::size_t, 6>{1, 2, 3, 5, 8, frames}) {
                const auto captured = graph.captureDsp(tap, frames, block).front();
                const auto packed = graph.captureDsp(packedTap, frames, block);
                for (int frame = 0; frame < frames; ++frame) {
                    check(captured[frame] == sample(expectedFrame[frame]),
                          "Capture must occur on the exact sample in both modes and preserve input precision");
                    const std::array expected = crossing
                        ? std::array{sample(0, 0), 0., frame < 4 ? 0. : sample(4, 2), 0.}
                        : std::array{sample(frame / 4 * 4, 0), sample(0, 1),
                                     sample(frame < 6 ? 0 : frame / 2 * 2, 2), sample(0, 3)};
                    for (int lane = 0; lane < 4; ++lane) {
                        check(packed[lane][frame] == expected[lane], "Each packed lane holds its own sample");
                    }
                }
            }
        }
    }
}

void streamPitchToFreqUsesTheSampleRateAtBothPrecisions()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    constexpr std::size_t frames = 11;
    constexpr std::array pitches{57.0, 69.0, 81.0, 69.0 + 0x1p-30};
    constexpr std::array laneOffsets{0.0, 12.0, -12.0, 0.5};
    for (const bool wide : {false, true}) {
        for (const bool packed : {false, true}) {
            openstone::Session session;
            auto& graph = session.graph();
            const auto add = [&](PrimitiveKind kind) { return graph.addNode(kind, 0, 0); };
            const auto code = [&](std::string source) {
                const int id = add(DspCode);
                graph.setDspDraft(id, std::move(source));
                session.compileDsp(id);
                return id;
            };
            const std::string signal = wide ? "streamdouble" : "stream";
            const std::string mono = wide ? "monodouble" : "mono";
            const auto type = wide ? Type::StreamDouble : Type::Stream;
            const auto kind = wide ? DoublePitchToFreq : StreamPitchToFreq;
            const int convert = add(kind);
            check(openstone::primitive(kind).inputs.size() == 1 && openstone::primitive(kind).outputs.size() == 1 &&
                  graph.inputType(convert, 0) == type && graph.outputType(convert) == type,
                  "Pitch to Freq has one input and one output at the declared stream precision");
            const int source = code(signal + "out pitch; " + (wide ? "double phase;" : "float phase;") + R"(
                pitch = phase == 3 ? 69.000000000931322574615478515625 : 57 + 12 * phase;
                pitch{1} = pitch(0) + 12;
                pitch{2} = pitch(0) - 12;
                pitch{3} = pitch(0) + .5;
                phase = phase == 3 ? 0 : phase + 1;
            )");
            graph.connect({source, convert, 0});
            int tap;
            if (packed) {
                const int unpack = add(wide ? UnpackDouble : Unpack);
                graph.connect({convert, unpack, 0});
                tap = code(mono + "in a; " + mono + "in b; " + mono + "in c; " + mono + "in d; " +
                           mono + "out w; " + mono + "out x; " + mono + "out y; " + mono + "out z; " +
                           "w = a; x = b; y = c; z = d;");
                for (int lane = 0; lane < 4; ++lane) graph.connect({unpack, tap, lane, lane});
            } else {
                tap = code(mono + "in freq; " + mono + "out sample; sample = freq;");
                graph.connect({convert, tap, 0});
            }
            const double relativeTolerance = wide ? 4e-15 : 4e-7;
            for (const double rate : {44100.0, 48000.0, 96000.0}) {
                graph.setSampleRate(rate);
                for (const auto block : std::array<std::size_t, 6>{1, 2, 3, 5, 8, frames}) {
                    const auto captured = graph.captureDsp(tap, frames, block);
                    for (std::size_t lane = 0; lane < captured.size(); ++lane) {
                        const auto& samples = captured[lane];
                        for (std::size_t frame = 0; frame < frames; ++frame) {
                            const double pitch = pitches[frame % pitches.size()] + laneOffsets[lane];
                            const double expected = 880.0 * std::exp2((pitch - 69.0) / 12.0) / rate;
                            check(std::abs(samples[frame] - expected) <= relativeTolerance * expected,
                                  "Pitch conversion follows the sample rate and fractional pitch in every lane");
                        }
                        check(wide ? samples[3] > samples[1] : samples[3] == samples[1],
                              "Double Pitch to Freq retains pitch changes below single-precision resolution");
                    }
                }
            }
        }
    }
}

void sampleRatePublishesChangesAndUpdatesRunningAudio()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    Schematic graph;
    const int rate = graph.addNode(SampleRate, 0, 0);
    const int module = graph.addNode(Module, 0, 0);
    auto& child = *graph.module(module);
    const int childRate = child.addNode(SampleRate, 0, 0);
    const auto value = [](Schematic& level, int id) { return std::get<float>(level.readValue(id)); };
    check(openstone::primitive(SampleRate).inputs.empty() && graph.outputType(rate) == Type::Float &&
          value(graph, rate) == 44100 && value(child, childRate) == 44100,
          "Sample Rate has no inputs and reads the root rate as a Green Float");
    const auto count = graph.addNode(TriggerCounter, 0, 0);
    graph.connect({rate, count, 0}, LinkNotification::Silent);
    graph.setSampleRate(48000);
    check(value(graph, rate) == 48000 && value(child, childRate) == 48000 &&
          integer(graph, count) == 1 && child.inspectOutput(childRate).triggers == 1,
          "The rate change publishes once at every schematic level");

    const int input = graph.addNode(AudioIn, 0, 0);
    const int convert = graph.addNode(StreamPitchToFreq, 0, 0);
    const int output = graph.addNode(AudioOut, 0, 0);
    graph.connect({input, convert, 0});
    graph.connect({convert, output, 0});
    auto patch = graph.prepareAudio(2);
    const std::array inputSamples{57.0F, 69.0F, 81.0F};
    std::array<float, inputSamples.size()> left{}, right{};
    const auto verifyAudio = [&](float sampleRate) {
        check(patch.processor().process({inputSamples, {}}, {left, right}) &&
              left == std::array{440.0F / sampleRate, 880.0F / sampleRate, 1760.0F / sampleRate},
              "The stream converter uses the host rate in a running audio patch");
    };
    verifyAudio(48000);
    child.setSampleRate(96000);
    child.setSampleRate(96000);
    check(value(graph, rate) == 96000 && value(child, childRate) == 96000 &&
          integer(graph, count) == 2 && child.inspectOutput(childRate).triggers == 2,
          "Setting the same rate twice emits one change and nested graphs use the root rate");
    check(graph.refreshAudioControls(patch), "Update the rate without rebuilding the audio patch");
    verifyAudio(96000);
}

void streamCountersStartAtZeroAndResetAcrossBufferSizes()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    openstone::Session session;
    auto& graph = session.graph();
    const auto add = [&](PrimitiveKind kind) { return graph.addNode(kind, 0, 0); };
    const int single = add(StreamCounter);
    const int wide = add(DoubleStreamCounter);
    const int resetSingle = add(Trigger);
    const int resetWide = add(Trigger);
    const int narrow = add(DoubleToStream);
    const int audio = add(AudioOut);
    check(openstone::primitive(StreamCounter).inputs.size() == 1 &&
          openstone::primitive(DoubleStreamCounter).inputs.size() == 1 &&
          openstone::primitive(StreamCounter).inputs[0].name == "Reset" &&
          openstone::primitive(DoubleStreamCounter).inputs[0].name == "Reset" &&
          graph.inputType(single, 0) == Type::Trigger && graph.inputType(wide, 0) == Type::Trigger &&
          graph.outputType(single) == Type::Stream && graph.outputType(wide) == Type::StreamDouble,
          "Both counters take one Green Reset and emit their own stream precision");
    graph.connect({resetSingle, single, 0}, LinkNotification::Silent);
    graph.connect({resetWide, wide, 0}, LinkNotification::Silent);
    graph.connect({single, audio, 0});
    graph.connect({wide, narrow, 0});
    graph.connect({narrow, audio, 1});
    check(graph.outputType(single) == Type::Mono && graph.outputType(wide) == Type::MonoDouble,
          "The counters resolve with the audio streams");

    const auto verifyAcrossBlocks = [&](float rightScale) {
        for (const std::size_t block : {1, 2, 5, 8, 64, 129}) {
            auto patch = graph.prepareAudio(block);
            const auto render = [&](std::size_t size, int leftStart, int rightStart) {
                std::vector<float> left(size), right(size);
                check(graph.refreshAudioControls(patch) && patch.processor().process({}, {left, right}),
                      "Green reset reaches the running patch without recompiling");
                for (std::size_t n = 0; n < size; ++n) {
                    check(left[n] == static_cast<float>(leftStart + n) && right[n] == rightScale * (rightStart + n),
                          "Each counter emits its current count before advancing once per sample");
                }
            };
            render(5, 0, 0);
            render(8, 5, 5);
            graph.fireTrigger(resetSingle);
            render(3, 0, 13);
            render(7, 3, 16);
            graph.fireTrigger(resetWide);
            graph.fireTrigger(resetWide);
            render(6, 10, 0);
            render(9, 16, 6);
        }
    };
    verifyAcrossBlocks(1.F);

    graph.disconnectInput(narrow, 0);
    const int unpack = add(UnpackDouble);
    const int sum = add(DspCode);
    graph.setDspDraft(sum, R"(
        monodoublein a; monodoublein b; monodoublein c; monodoublein d;
        monodoubleout y;
        y = (a == b) & (a == c) & (a == d) ? a + b + c + d : -1;
    )");
    session.compileDsp(sum);
    graph.connect({wide, unpack, 0});
    for (int lane = 0; lane < 4; ++lane) graph.connect({unpack, sum, lane, lane});
    graph.connect({sum, narrow, 0});
    check(graph.outputType(wide) == Type::Mono4Double, "Double Stream Counter supports packed output");
    verifyAcrossBlocks(4.F);
}

void biquadImpulseResponsesMatchAcrossBufferSizes()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    for (const bool wide : {false, true}) {
        openstone::Session session;
        auto& graph = session.graph();
        const auto code = [&](std::string source) {
            const int id = graph.addNode(DspCode, 0, 0);
            graph.setDspDraft(id, std::move(source));
            session.compileDsp(id);
            return id;
        };
        const auto kind = wide ? DoubleBiquadFilterCoeff : BiquadFilterCoeff;
        const auto generic = wide ? Type::StreamDouble : Type::Stream;
        const auto mono = wide ? Type::MonoDouble : Type::Mono;
        const int filter = graph.addNode(kind, 0, 0);
        const auto& spec = openstone::primitive(kind);
        constexpr std::array names{"In", "a0", "a1", "a2", "b1", "b2"};
        check(spec.inputs.size() == names.size() && graph.outputType(filter) == generic,
              "Biquad has six inputs and one generic Stream output at the selected precision");
        for (int i = 0; i < static_cast<int>(names.size()); ++i) {
            check(spec.inputs[i].name == names[i] && graph.inputType(filter, i) == generic,
                  "The biquad input order and types match the reference");
        }

        int input = graph.addNode(Impulse, 0, 0);
        if (wide) {
            const int convert = graph.addNode(StreamToDouble, 0, 0);
            graph.connect({input, convert, 0});
            input = convert;
        }
        const int a0 = code(wide ? "streamdoubleout k; k = 0.5000000000009094947017729282379150390625;" :
                                   "streamout k; k = 0.5;");
        const int tap = code(wide ? "monodoublein x; monodoubleout y; y = x;" :
                                   "monoin x; monoout y; y = x;");
        graph.connect({input, filter, 0});
        graph.connect({a0, filter, 1});
        constexpr std::array greenCoefficients{-.25f, .125f, -.75f, .125f};
        for (int i = 0; i < static_cast<int>(greenCoefficients.size()); ++i) {
            const int coefficient = graph.addNode(Float, 0, 0, greenCoefficients[i]);
            graph.connect({coefficient, filter, i + 2});
        }
        graph.connect({filter, tap, 0});
        check(graph.outputType(filter) == mono, "Biquad specializes to the destination's Mono precision");
        for (int i = 0; i < static_cast<int>(names.size()); ++i) {
            check(graph.inputType(filter, i) == mono, "Signal and coefficient inputs share the same stream mode");
        }

        constexpr int frames = 137;
        const double extra = wide ? std::ldexp(1., -40) : 0.;
        const auto samples = graph.captureDsp(tap, frames, 1).front();
        check(samples[0] == .5 + extra && samples[1] == .125 + .75 * extra,
              "The first two samples preserve the coefficient precision and feedback sign");
        for (int n = 1; n < frames; ++n) {
            // Poles at 1/2 and 1/4 give a closed-form response for this impulse.
            const double tail = std::ldexp(.5, 1 - n) - std::ldexp(.375, 2 - 2 * n);
            const double extraTail = extra * (std::ldexp(2., -n) - std::ldexp(1., -2 * n));
            const double expected = tail + extraTail;
            const double tolerance = wide ? 2e-15 * expected : 2e-6 * expected + 1e-24;
            check(std::abs(samples[n] - expected) <= tolerance, "The impulse follows the expected two-pole decay");
        }
        for (std::size_t block : {3, 7, 16, 64, frames}) {
            check(graph.captureDsp(tap, frames, block).front() == samples,
                  "Biquad state carries through each buffer boundary at either precision");
        }
    }
}

void packedBiquadsReadCoefficientsEverySample()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    for (const bool wide : {false, true}) {
        openstone::Session session;
        auto& graph = session.graph();
        const auto add = [&](PrimitiveKind kind) { return graph.addNode(kind, 0, 0); };
        const auto code = [&](std::string source) {
            const int id = add(DspCode);
            graph.setDspDraft(id, std::move(source));
            session.compileDsp(id);
            return id;
        };
        const std::string stream = wide ? "streamdouble" : "stream";
        const int coefficients = code(stream + "out a0; " + stream + R"(out b1;
            float n;
            a0 = 1; a0{1} = -2; a0{2} = .5; a0{3} = 4;
            b1 = -.5; b1{1} = n < 3 ? -.25 : .5; b1{2} = .25; b1{3} = 0;
            n += 1;
        )");
        int input = add(Impulse);
        if (wide) {
            const int convert = add(StreamToDouble);
            graph.connect({input, convert, 0});
            input = convert;
        }
        const int filter = add(wide ? DoubleBiquadFilterCoeff : BiquadFilterCoeff);
        const int unpack = add(wide ? UnpackDouble : Unpack);
        const int tap = code(wide ? R"(
            monodoublein a; monodoublein b; monodoublein c; monodoublein d;
            monodoubleout w; monodoubleout x; monodoubleout y; monodoubleout z;
            w = a; x = b; y = c; z = d;
        )" : R"(
            monoin a; monoin b; monoin c; monoin d;
            monoout w; monoout x; monoout y; monoout z;
            w = a; x = b; y = c; z = d;
        )");
        graph.connect({input, filter, 0});
        graph.connect({coefficients, filter, 1});
        graph.connect({coefficients, filter, 4, 1});
        graph.connect({filter, unpack, 0});
        for (int lane = 0; lane < 4; ++lane) graph.connect({unpack, tap, lane, lane});
        check(graph.outputType(filter) == (wide ? Type::Mono4Double : Type::Mono4),
              "Biquad supports four independent lanes at either precision");

        constexpr int frames = 29;
        std::vector<std::vector<double>> expected(4, std::vector<double>(frames));
        constexpr std::array gains{1., -2., .5, 4.};
        for (std::size_t lane = 0; lane < gains.size(); ++lane) expected[lane][0] = gains[lane];
        for (int n = 1; n < frames; ++n) {
            expected[0][n] = .5 * expected[0][n - 1];
            expected[1][n] = (n < 3 ? .25 : -.5) * expected[1][n - 1];
            expected[2][n] = -.25 * expected[2][n - 1];
        }
        for (std::size_t block : {1, 3, 8, frames}) {
            check(graph.captureDsp(tap, frames, block) == expected,
                  "Each lane responds immediately to its own coefficient changes and preserves its own history");
        }
    }
}

std::array<double, 5> expectedLowPassCoefficients(double cutoff, double q)
{
    if (q <= 0) return {};

    // Independently calculate the same low-pass from sin and cos at the
    // quantized cutoff. The coefficient generator uses the tangent form.
    const double bin = std::nearbyint(std::clamp(cutoff, .01, .997) * 16384);
    const double radians = std::numbers::pi * bin / 16384;
    const double cosine = std::cos(radians);
    const double alpha = std::sin(radians) / (2 * q);
    const double normalizer = 1 + alpha;
    const double a0 = (1 - cosine) / (2 * normalizer);
    return {a0, 2 * a0, a0, -2 * cosine / normalizer, (1 - alpha) / normalizer};
}

void generatedCoefficientsProduceLowPassImpulseResponses()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    for (const bool wide : {false, true}) {
        openstone::Session session;
        auto& graph = session.graph();
        const auto add = [&](PrimitiveKind kind) { return graph.addNode(kind, 0, 0); };
        const auto kind = wide ? DoubleFilterCoefficients : FilterCoefficients;
        const int coefficients = add(kind);
        const int cutoff = add(Float);
        const int resonance = add(Float);
        const int filter = add(wide ? DoubleBiquadFilterCoeff : BiquadFilterCoeff);
        const int tap = add(DspCode);
        graph.setDspDraft(tap, wide ? "monodoublein x; monodoubleout y; y = x;" :
                                     "monoin x; monoout y; y = x;");
        session.compileDsp(tap);

        const auto stream = wide ? Type::StreamDouble : Type::Stream;
        const auto mono = wide ? Type::MonoDouble : Type::Mono;
        const auto& spec = openstone::primitive(kind);
        check(spec.inputs.size() == 2 && spec.inputs[0].name == "Cut" && spec.inputs[1].name == "Res" &&
              graph.inputType(coefficients, 0) == stream && graph.inputType(coefficients, 1) == stream,
              "Filter Coefficients takes Cut and Res in the matching stream precision");
        constexpr std::array names{"a0", "a1", "a2", "b1", "b2"};
        check(spec.outputs.size() == names.size(), "The generator provides all five biquad coefficients");
        for (int port = 0; port < static_cast<int>(names.size()); ++port) {
            check(spec.outputs[port].name == names[port] && graph.outputType(coefficients, port) == stream,
                  "Coefficient output order and types match the FlowStone reference");
            graph.connect({coefficients, filter, port + 1, port});
        }
        graph.connect({cutoff, coefficients, 0});
        graph.connect({resonance, coefficients, 1});
        int input = add(Impulse);
        if (wide) {
            const int convert = add(StreamToDouble);
            graph.connect({input, convert, 0});
            input = convert;
        }
        graph.connect({input, filter, 0});
        graph.connect({filter, tap, 0});
        check(graph.inputType(coefficients, 0) == mono && graph.inputType(coefficients, 1) == mono &&
              graph.outputType(coefficients, 4) == mono, "Cut, Res, and coefficients resolve with the biquad");

        constexpr int frames = 137;
        for (const auto [cut, q] : {std::pair{.2503f, .625f}, std::pair{.375f, 1.5f}, std::pair{.125f, 1.25f}}) {
            graph.setValue(cutoff, cut);
            graph.setValue(resonance, q);
            const auto [a0, a1, a2, b1, b2] = expectedLowPassCoefficients(cut, q);
            std::vector<double> expected(frames);
            expected[0] = a0;
            expected[1] = a1 - b1 * expected[0];
            expected[2] = (a2 - b1 * expected[1]) - b2 * expected[0];
            for (int n = 3; n < frames; ++n) {
                expected[n] = -b1 * expected[n - 1] - b2 * expected[n - 2];
            }
            const auto samples = graph.captureDsp(tap, frames, 1).front();
            for (int n = 0; n < frames; ++n) {
                check(std::abs(samples[n] - expected[n]) < (wide ? 3e-14 : 3e-6),
                      "The generated coefficients give the expected low-pass impulse response");
            }
            for (const std::size_t block : {3, 4, 7, 64, frames}) {
                check(graph.captureDsp(tap, frames, block).front() == samples,
                      "The generated low-pass response is independent of buffer size");
            }
        }
    }
}

void lowPassCoefficientsTrackCutoffAndResonanceEveryFourSamples()
{
    using enum PrimitiveKind;
    for (const bool wide : {false, true}) {
        openstone::Session session;
        auto& graph = session.graph();
        const auto code = [&](std::string source) {
            const int id = graph.addNode(DspCode, 0, 0);
            graph.setDspDraft(id, std::move(source));
            session.compileDsp(id);
            return id;
        };
        const std::string stream = wide ? "streamdouble" : "stream";
        const std::string mono = wide ? "monodouble" : "mono";
        const int source = code(stream + "out cutoff; " + stream + R"(out q;
            float n;
            cutoff = n < 3 ? .25 : (n < 7 ? 0 : (n < 11 ? 1.25 : .25));
            q = n < 12 ? .625 : (n < 16 ? 0 : .6250000000009094947017729282379150390625);
            n += 1;
        )");
        const int coefficients = graph.addNode(wide ? DoubleFilterCoefficients : FilterCoefficients, 0, 0);
        graph.connect({source, coefficients, 0});
        graph.connect({source, coefficients, 1, 1});
        std::string tapCode;
        for (int i = 0; i < 5; ++i) {
            const auto port = std::to_string(i);
            tapCode += mono + "in x" + port + "; " + mono + "out y" + port + "; y" + port + " = x" + port + ";\n";
        }
        const int tap = code(std::move(tapCode));
        for (int port = 0; port < 5; ++port) graph.connect({coefficients, tap, port, port});

        const double extra = wide ? std::ldexp(1., -40) : 0.;
        const std::array expected{
            expectedLowPassCoefficients(.25, .625), expectedLowPassCoefficients(0, .625),
            expectedLowPassCoefficients(1.25, .625), expectedLowPassCoefficients(.25, 0),
            expectedLowPassCoefficients(.25, .625 + extra)
        };
        constexpr int frames = 20;
        const auto samples = graph.captureDsp(tap, frames, 1);
        for (int port = 0; port < 5; ++port) {
            for (int n = 0; n < frames; ++n) {
                check(std::abs(samples[port][n] - expected[n / 4][port]) < (wide ? 3e-14 : 3e-6),
                      "The four-sample update holds each coefficient and applies the cutoff limits and Q");
            }
        }
        const double change = samples[0][16] - samples[0][0];
        check(wide ? change > 1e-14 && change < 1e-12 : change == 0,
              "The double generator preserves a 2^-40 change in resonance");
        for (const std::size_t block : {3, 4, 7, frames}) {
            check(graph.captureDsp(tap, frames, block) == samples,
                  "The coefficient update cadence follows sample time across buffer boundaries");
        }
    }
}

void biquadResponsesMatchTheLowPassImpulseTransform()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    openstone::Session session;
    auto& graph = session.graph();
    const auto add = [&](PrimitiveKind kind) { return graph.addNode(kind, 0, 0); };
    const int magnitude = add(BiquadFrequencyResponse);
    const int phase = add(BiquadPhaseResponse);
    constexpr std::array names{"a0", "a1", "a2", "b0", "b1", "b2", "LowF", "HighF", "N", "SR"};
    for (const auto kind : {BiquadFrequencyResponse, BiquadPhaseResponse}) {
        const auto& spec = openstone::primitive(kind);
        const bool frequencyResponse = kind == BiquadFrequencyResponse;
        check(spec.inputs.size() == (frequencyResponse ? 12 : 11) && spec.outputs.size() == 1 &&
              spec.outputs[0].type == Type::FloatArray, "Biquad response uses Green connectors and a Float Array");
        for (std::size_t port = 0; port < names.size(); ++port) {
            check(spec.inputs[port].name == names[port] && spec.inputs[port].type == (port == 8 ? Type::Int : Type::Float),
                  "Coefficient and frequency grid connectors match the reference");
        }
        check(spec.inputs.back().name == "LogF" && spec.inputs.back().type == Type::Boolean &&
              (!frequencyResponse || (spec.inputs[10].name == "LogA" && spec.inputs[10].type == Type::Boolean)),
              "The frequency grid and decibel controls match the reference");
    }

    constexpr int points = 20;
    constexpr float rate = 48000;
    graph.setSampleRate(rate);
    const auto wireBoth = [&](int source, int port) {
        graph.connect({source, magnitude, port});
        graph.connect({source, phase, port});
    };
    wireBoth(graph.addNode(Float, 0, 0, 1.F), 3);
    const int lowest = graph.addNode(Float, 0, 0, 0.F);
    wireBoth(lowest, 6);
    wireBoth(graph.addNode(Float, 0, 0, 22000.F), 7);
    wireBoth(graph.addNode(Int, 0, 0, points), 8);
    wireBoth(graph.addNode(Float, 0, 0, rate), 9);
    const int logAmplitude = add(Boolean);
    const int logFrequency = add(Boolean);
    graph.connect({logAmplitude, magnitude, 10});
    graph.connect({logFrequency, magnitude, 11});
    graph.connect({logFrequency, phase, 10});

    const int generator = add(FilterCoefficients);
    const int filter = add(BiquadFilterCoeff);
    const int tap = add(DspCode);
    graph.setDspDraft(tap, "monoin x; monoout y; y = x;");
    session.compileDsp(tap);
    graph.connect({graph.addNode(Float, 0, 0, .25F), generator, 0});
    graph.connect({graph.addNode(Float, 0, 0, .75F), generator, 1});
    graph.connect({add(Impulse), filter, 0});
    graph.connect({filter, tap, 0});
    constexpr std::array responsePorts{0, 1, 2, 4, 5};
    for (int port = 0; port < 5; ++port) {
        graph.connect({generator, filter, port + 1, port});
        const int sampler = add(MonoToFloat);
        graph.connect({generator, sampler, 0, port});
        graph.fireTrigger(sampler);
        wireBoth(sampler, responsePorts[port]);
    }

    const auto impulse = graph.captureDsp(tap, 256, 17).front();
    constexpr double turn = 2 * std::numbers::pi;
    for (const bool logF : {false, true}) {
        const double low = logF ? 650. : 0.;
        constexpr double high = 22000.;
        graph.setValue(lowest, static_cast<float>(low));
        graph.setValue(logFrequency, logF);
        graph.setValue(logAmplitude, false);
        const auto power = std::get<FloatArray>(graph.readValue(magnitude));
        const auto angle = std::get<FloatArray>(graph.readValue(phase));
        graph.setValue(logAmplitude, true);
        const auto db = std::get<FloatArray>(graph.readValue(magnitude));
        check(power.size() == points && angle.size() == points && db.size() == points,
              "Both response nodes return N points");
        for (int index = 0; index < points; ++index) {
            const double t = static_cast<double>(index) / points;
            const double hz = logF ? low * std::pow(high / low, t) : (high - low) * t + low;
            std::complex<double> transform{};
            for (std::size_t n = 0; n < impulse.size(); ++n) {
                transform += impulse[n] * std::polar(1.0, -turn * hz * n / rate);
            }
            const double referencePower = std::norm(transform);
            check(std::abs(power[index] - referencePower) < 2e-6 &&
                  std::abs(db[index] - 10 * std::log10(referencePower)) < 3e-4,
                  "Linear power and decibels agree with the measured low-pass response");
            check(std::abs(std::remainder(angle[index] - std::arg(transform), turn)) < 2e-5,
                  "Principal phase in radians agrees with the measured low-pass response");
        }
    }
}

void biquadResponsesPublishChangesAndUseTheDenominatorGain()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    Schematic graph;
    const int response = graph.addNode(BiquadFrequencyResponse, 0, 0);
    const int numerator = graph.addNode(Float, 0, 0, .5F);
    const int denominator = graph.addNode(Float, 0, 0, 2.F);
    const int count = graph.addNode(Int, 0, 0, 4);
    const int rate = graph.addNode(Float, 0, 0, 48000.F);
    const int logF = graph.addNode(Boolean, 0, 0);
    const auto data = [&] { return std::get<FloatArray>(graph.readValue(response)); };
    graph.connect({numerator, response, 0});
    graph.connect({denominator, response, 3});
    graph.connect({count, response, 8});
    check(data().empty(), "The response is empty until a valid sample rate is supplied");
    graph.connect({rate, response, 9});
    graph.connect({logF, response, 11});
    check(data() == FloatArray(4, .0625F), "A0 / B0 produces flat power across the grid");
    const auto before = graph.inspectOutput(response).triggers;
    graph.setValue(denominator, 1.F);
    check(graph.inspectOutput(response).triggers == before + 1 && data() == FloatArray(4, .25F),
          "Changing a coefficient publishes the recalculated response");
    graph.setValue(logF, true);
    check(data().empty(), "A logarithmic sweep needs positive frequency bounds");
}

void rampStartsWrapsAndResetsOnTheRisingSyncEdge()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    openstone::Session session;
    auto& graph = session.graph();
    const auto code = [&](std::string source) {
        const int id = graph.addNode(DspCode, 0, 0);
        graph.setDspDraft(id, std::move(source));
        session.compileDsp(id);
        return id;
    };
    const int source = code(R"(
        streamout frequency;
        streamboolout sync;
        float n;
        frequency = n < 9 ? 0.5 : (n < 12 ? 1.0 : 0.0);
        sync = n < 3 ? false : (n < 6 ? true : (n < 8 ? false : n < 10));
        n += 1;
    )");
    const int ramp = graph.addNode(Ramp, 0, 0);
    const int tap = code("monoin x; monoout y; y = x;");
    check(graph.inputType(ramp, 0) == Type::Stream && graph.inputType(ramp, 1) == Type::StreamBoolean &&
          graph.outputType(ramp) == Type::Stream,
          "Ramp has a normalized Stream frequency, a Stream Boolean sync, and a Stream output");
    graph.connect({source, ramp, 0});
    graph.connect({source, ramp, 1, 1});
    graph.connect({ramp, tap, 0});
    check(graph.inputType(ramp, 0) == Type::Mono && graph.inputType(ramp, 1) == Type::MonoBoolean &&
          graph.outputType(source, 1) == Type::MonoBoolean && graph.outputType(ramp) == Type::Mono,
          "The Ramp sync specializes to Mono Boolean alongside the numeric Mono ports");

    const std::vector expected{0., .25, .5, 0., .25, .5, .75, 0., 0., .25, .75, .25, .75, .75};
    for (const std::size_t block : {1, 3, 4, 5, 8, 14}) {
        check(graph.captureDsp(tap, expected.size(), block).front() == expected,
              "Ramp starts at zero, wraps, resets on the sync edge, and advances while sync is high");
    }

    graph.disconnectInput(ramp, 1);
    const std::vector freeRunning{0., .25, .5, .75, 0., .25, .5, .75, 0., .25, .75, .25, .75, .75};
    check(graph.captureDsp(tap, freeRunning.size(), 7).front() == freeRunning,
          "An unconnected sync leaves the oscillator running freely");
    graph.disconnectInput(ramp, 0);
    check(graph.captureDsp(tap, 14, 5).front() == std::vector<double>(14),
          "An unconnected frequency leaves the new ramp at zero");
}

void packedRampKeepsIndependentPhasesAndSyncEdges()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    openstone::Session session;
    auto& graph = session.graph();
    const auto code = [&](std::string source) {
        const int id = graph.addNode(DspCode, 0, 0);
        graph.setDspDraft(id, std::move(source));
        session.compileDsp(id);
        return id;
    };
    const int source = code(R"(
        streamout frequency;
        streamboolout sync;
        float n;
        frequency = 0;
        frequency{0} = 1.0;
        frequency{1} = 0.5;
        frequency{2} = 0.25;
        sync = false;
        sync{0} = n == 5;
        sync{1} = n < 7 ? false : n < 10;
        sync{2} = n == 11;
        sync{3} = true;
        n += 1;
    )");
    const int ramp = graph.addNode(Ramp, 0, 0);
    const int unpack = graph.addNode(Unpack, 0, 0);
    const int tap = code(R"(
        monoin a; monoin b; monoin c; monoin d;
        monoout w; monoout x; monoout y; monoout z;
        w = a; x = b; y = c; z = d;
    )");
    graph.connect({source, ramp, 0});
    graph.connect({source, ramp, 1, 1});
    graph.connect({ramp, unpack, 0});
    for (int lane = 0; lane < 4; ++lane) graph.connect({unpack, tap, lane, lane});
    check(graph.outputType(ramp) == Type::Mono4, "Ramp resolves to Mono4 when unpacked");

    constexpr int frames = 29;
    constexpr std::array periods{2, 4, 8};
    constexpr std::array resets{5, 7, 11};
    std::vector<std::vector<double>> expected(4, std::vector<double>(frames));
    for (std::size_t lane = 0; lane < periods.size(); ++lane) {
        for (int n = 0; n < frames; ++n) {
            const int sinceReset = n < resets[lane] ? n : n - resets[lane];
            expected[lane][n] = static_cast<double>(sinceReset % periods[lane]) / periods[lane];
        }
    }
    for (const std::size_t block : {1, 5, 7, 11, frames}) {
        check(graph.captureDsp(tap, frames, block) == expected,
              "Each packed lane applies its own phase increment and sync edge across buffer boundaries");
    }
}

void oscillatorAppliesPhaseOffsetAndHardSyncAcrossBufferBoundaries(PrimitiveKind single, PrimitiveKind wideKind)
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    for (const bool wide : {false, true}) {
        openstone::Session session;
        auto& graph = session.graph();
        const auto code = [&](std::string source) {
            const int id = graph.addNode(DspCode, 0, 0);
            graph.setDspDraft(id, std::move(source));
            session.compileDsp(id);
            return id;
        };
        const std::string stream = wide ? "streamdouble" : "stream";
        const std::string mono = wide ? "monodouble" : "mono";
        const int source = code(stream + "out frequency; " + stream + R"(out offset;
            streamboolout sync;
            float n;
            frequency = n < 12 ? 0.25 : (n == 13 ? 1 : 0.5);
            offset = n < 3 ? 0.125 : (n < 6 ? 0.375 : (n < 8 ? 1.125 : 0.125));
            sync = n < 3 ? false : (n < 6 ? true : (n < 8 ? false : n < 10));
            n += 1;
        )");
        const int osc = graph.addNode(wide ? wideKind : single, 0, 0);
        const int tap = code(mono + "in x; " + mono + "out y; y = x;");
        const auto generic = wide ? Type::StreamDouble : Type::Stream;
        const auto scalar = wide ? Type::MonoDouble : Type::Mono;
        check(graph.inputType(osc, 0) == generic && graph.inputType(osc, 1) == generic &&
              graph.inputType(osc, 2) == Type::StreamBoolean && graph.outputType(osc) == generic,
              "The oscillator exposes Freq, Phase, and Stream Boolean Sync at both precisions");
        for (int port = 0; port < 3; ++port) graph.connect({source, osc, port, port});
        graph.connect({osc, tap, 0});
        check(graph.inputType(osc, 0) == scalar && graph.inputType(osc, 1) == scalar &&
              graph.inputType(osc, 2) == Type::MonoBoolean && graph.outputType(osc) == scalar,
              "The oscillator specializes all of its connectors together");

        constexpr std::array phases{.125, .25, .375, .375, .5, .625, .5, .625, .125, .25, .375, .5, .625, .875, .375};
        constexpr std::array harmonics{4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 2, 1, 2};
        const auto harmonicSum = [single](double phase, int count) {
            const double angle = 2 * std::numbers::pi * phase;
            if (single == Sine) return std::sin(angle);

            double value = 0;
            for (int harmonic = 1; harmonic <= count; ++harmonic) {
                if (single == Triangle) {
                    if (harmonic % 2 == 0) continue;
                    const double sign = harmonic % 4 == 1 ? 1. : -1.;
                    value += sign * std::sin(harmonic * angle) / (harmonic * harmonic);
                } else {
                    value += std::sin(harmonic * angle) / harmonic;
                }
            }
            return value;
        };
        const auto samples = graph.captureDsp(tap, phases.size(), 1).front();
        double scale = 1.;
        if (single == Triangle) scale = 1. / harmonicSum(.25, 1023);
        if (single == Sawtooth) {
            scale = samples.front() / harmonicSum(phases.front(), harmonics.front());
            check(scale > .5 && scale < 1., "Sawtooth normalizes the wavetable bank");
        }
        for (std::size_t i = 0; i < phases.size(); ++i) {
            const double expected = scale * harmonicSum(phases[i], harmonics[i]);
            check(std::abs(samples[i] - expected) < (wide ? 2e-14 : 2e-6),
                  "The oscillator wraps Phase, resets on rising sync, and limits harmonics to Nyquist");
        }
        for (const std::size_t block : {3, 4, 5, 8, 15}) {
            check(graph.captureDsp(tap, phases.size(), block).front() == samples,
                  "Phase and edge detection carry across block boundaries");
        }

        // With no frequency or phase offset the oscillator stays at the start of the cycle.
        graph.disconnectInput(osc, 0);
        graph.disconnectInput(osc, 1);
        check(graph.captureDsp(tap, phases.size(), 5).front() == std::vector<double>(phases.size()),
              "Unconnected Freq and Phase read zero");
    }
}

void packedOscillatorMatchesIndependentDoubleRuns(PrimitiveKind kind)
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    openstone::Session session;
    auto& graph = session.graph();
    const auto code = [&](std::string source) {
        const int id = graph.addNode(DspCode, 0, 0);
        graph.setDspDraft(id, std::move(source));
        session.compileDsp(id);
        return id;
    };
    const std::string declarations = "streamdoubleout frequency; streamdoubleout offset; streamboolout sync; double n;";
    constexpr std::array frequencies{"0.25", "0.25", "0.5", "0.125"};
    constexpr std::array offsets{"0.125", "0.1250000000009094947017729282379150390625", "-0.625", "1.25"};
    const auto connectOscillator = [&](int source) {
        const int id = graph.addNode(kind, 0, 0);
        for (int port = 0; port < 3; ++port) graph.connect({source, id, port, port});
        return id;
    };

    constexpr int frames = 31;
    std::vector<std::vector<double>> expected;
    auto packed = declarations;
    for (int lane = 0; lane < 4; ++lane) {
        const auto edge = std::to_string(3 + lane * 2);
        const auto assignLane = [&](const std::string& selector) {
            return "frequency" + selector + " = " + frequencies[lane] + "; offset" + selector + " = " +
                   offsets[lane] + "; sync" + selector + " = n < " + edge + " ? false : n < " +
                   std::to_string(6 + lane * 2) + ";";
        };
        const int osc = connectOscillator(code(declarations + assignLane("") + "n += 1;"));
        const int tap = code("monodoublein x; monodoubleout y; y = x;");
        graph.connect({osc, tap, 0});
        expected.push_back(graph.captureDsp(tap, frames, 1).front());
        packed += assignLane("{" + std::to_string(lane) + "}");
    }
    const double offsetDifference = std::abs(expected[0][0] - expected[1][0]);
    check(offsetDifference > 1e-13 && offsetDifference < 1e-9,
          "A 2^-40 phase offset change survives a double oscillator");

    const int osc = connectOscillator(code(packed + "n += 1;"));
    const int unpack = graph.addNode(UnpackDouble, 0, 0);
    graph.connect({osc, unpack, 0});
    const int tap = code(R"(
        monodoublein a; monodoublein b; monodoublein c; monodoublein d;
        monodoubleout w; monodoubleout x; monodoubleout y; monodoubleout z;
        w = a; x = b; y = c; z = d;
    )");
    for (int lane = 0; lane < 4; ++lane) graph.connect({unpack, tap, lane, lane});
    check(graph.outputType(osc) == Type::Mono4Double, "The oscillator supports a packed double stream");
    for (const std::size_t block : {1, 3, 5, 8, 13, frames}) {
        check(graph.captureDsp(tap, frames, block) == expected,
              "The packed double oscillator preserves each lane's frequency, phase precision, and sync edges");
    }
}

void glideRampsBothDirectionsAcrossBufferBoundaries()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    openstone::Session session;
    auto& graph = session.graph();
    const auto code = [&](std::string source) {
        const int id = graph.addNode(DspCode, 0, 0);
        graph.setDspDraft(id, std::move(source));
        session.compileDsp(id);
        return id;
    };
    const int source = code(R"(
        streamout target; streamout length;
        float n;
        target = n < 19 ? .25 : (n < 270 ? 1.25 : (n < 700 ? -.75 : .5));
        length = n < 150 ? 512 : (n < 370 ? 0 : (n < 700 ? 256 : 0));
        n += 1;
    )");
    const int glide = graph.addNode(Glide, 0, 0);
    const int tap = code("monoin x; monoout y; y = x;");
    check(graph.inputType(glide, 0) == Type::Stream && graph.inputType(glide, 1) == Type::Stream &&
          graph.outputType(glide) == Type::Stream,
          "Glide exposes a Stream target, a Stream length in samples, and a Stream output");
    graph.connect({source, glide, 0});
    graph.connect({source, glide, 1, 1});
    graph.connect({glide, tap, 0});
    check(graph.inputType(glide, 0) == Type::Mono && graph.inputType(glide, 1) == Type::Mono &&
          graph.outputType(glide) == Type::Mono,
          "The destination resolves all of Glide's Stream ports together");

    constexpr int frames = 804;
    std::vector<double> expected(frames);
    for (int n = 0; n < frames; ++n) {
        if (n < 384) expected[n] = .25 + std::max(0, n - 127) / 512.0;
        else if (n < 768) expected[n] = .75 - std::min(256, n - 383) * 3.0 / 512;
        else expected[n] = .5;
    }
    for (std::size_t block : {1, 17, 128, 257, frames}) {
        check(graph.captureDsp(tap, frames, block).front() == expected,
              "Glide adopts the first target and retargets a rising or falling ramp at an exact sample hop");
    }
    graph.setSampleRate(96000);
    check(graph.captureDsp(tap, frames, 31).front() == expected, "Glide lengths are measured in samples");

    graph.disconnectInput(glide, 1);
    const auto unwired = graph.captureDsp(tap, frames, 41).front();
    for (int n = 0; n < frames; ++n) {
        const double target = n < 128 ? .25 : n < 384 ? 1.25 : n < 768 ? -.75 : .5;
        check(unwired[n] == target, "An unconnected length snaps to the target at the next hop");
    }
}

void glideUsesEachPackedLanesLength()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    openstone::Session session;
    auto& graph = session.graph();
    const auto code = [&](std::string source) {
        const int id = graph.addNode(DspCode, 0, 0);
        graph.setDspDraft(id, std::move(source));
        session.compileDsp(id);
        return id;
    };
    const int source = code(R"(
        monoout a; monoout b; monoout c; monoout d;
        float n;
        a = n < 7 ? 0 : 1;
        b = n < 7 ? 0 : -2;
        c = n < 7 ? 0 : 4;
        d = n < 7 ? 0 : -8;
        n += 1;
    )");
    const int tap = code(R"(
        monoin a; monoin b; monoin c; monoin d;
        monoout x; monoout y; monoout z; monoout w;
        x = a; y = b; z = c; w = d;
    )");
    const int targets = graph.addNode(Pack, 0, 0);
    const int lengths = graph.addNode(Pack, 0, 0);
    const int glide = graph.addNode(Glide, 0, 0);
    const int unpack = graph.addNode(Unpack, 0, 0);
    constexpr std::array sizes{256.f, 256.f, 64.f, 0.f};
    for (int lane = 0; lane < 4; ++lane) {
        const int length = graph.addNode(Float, 0, 0, sizes[lane]);
        graph.connect({source, targets, lane, lane});
        graph.connect({length, lengths, lane});
        graph.connect({unpack, tap, lane, lane});
    }
    graph.connect({targets, glide, 0});
    graph.connect({lengths, glide, 1});
    graph.connect({glide, unpack, 0});
    check(graph.inputType(glide, 1) == Type::Mono4, "Glide accepts different lengths on each packed lane");

    constexpr int frames = 419;
    std::vector<std::vector<double>> expected(4, std::vector<double>(frames));
    for (int n = 0; n < frames; ++n) {
        expected[0][n] = std::clamp(n - 127, 0, 256) / 256.0;
        expected[1][n] = -2 * expected[0][n];
        expected[2][n] = std::clamp(n - 127, 0, 128) / 32.0;
        expected[3][n] = n < 128 ? 0.0 : -8.0;
    }
    for (std::size_t block : {3, 67, 128, 409}) {
        check(graph.captureDsp(tap, frames, block) == expected,
              "Each packed lane uses its own length, including a sub-hop glide and a zero-length jump");
    }
}

void streamDeZipperTracksEachLaneOnSampleHops()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    openstone::Session session;
    auto& graph = session.graph();
    graph.setSampleRate(64000);
    const auto add = [&](PrimitiveKind kind) { return graph.addNode(kind, 0, 0); };
    const auto code = [&](std::string source) {
        const int id = add(DspCode);
        graph.setDspDraft(id, std::move(source));
        session.compileDsp(id);
        return id;
    };
    const int source = code(R"(
        monoout a; monoout b; monoout c; monoout d;
        float frame;
        a = frame < 3 ? .25 : .75;
        b = frame < 3 ? -.25 : -.75;
        c = frame < 129 ? 0 : 1;
        d = .125;
        frame += 1;
    )");
    const int tap = code(R"(
        monoin a; monoin b; monoin c; monoin d;
        monoout x; monoout y; monoout z; monoout w;
        x = a; y = b; z = c; w = d;
    )");
    const int pack = add(Pack);
    const int unpack = add(Unpack);
    const int smoother = add(DeZipper);
    const int time = graph.addNode(Float, 0, 0, 4.0f);
    check(graph.inputType(smoother, 0) == Type::Stream && graph.inputType(smoother, 1) == Type::Float &&
          graph.inputType(smoother, 2) == Type::Trigger && graph.outputType(smoother) == Type::Stream &&
          graph.inspectInput(smoother, 1).value == openstone::green::Value{40.0f},
          "De-Zipper exposes the Alpha connectors and the default time");
    for (int lane = 0; lane < 4; ++lane) {
        graph.connect({source, pack, lane, lane});
        graph.connect({unpack, tap, lane, lane});
    }
    graph.connect({pack, smoother, 0});
    graph.connect({time, smoother, 1});
    graph.connect({smoother, unpack, 0});
    check(graph.inputType(smoother, 0) == Type::Mono4 && graph.outputType(smoother) == Type::Mono4,
          "The De-Zipper's Stream ports specialize to the packed type together");

    constexpr int frames = 544;
    std::vector<std::vector<double>> expected(4, std::vector<double>(frames));
    for (int frame = 0; frame < frames; ++frame) {
        expected[0][frame] = .25 + std::clamp(frame - 127, 0, 256) / 512.0;
        expected[1][frame] = -expected[0][frame];
        expected[2][frame] = std::clamp(frame - 255, 0, 256) / 256.0;
        expected[3][frame] = .125;
    }
    for (std::size_t block : {1, 17, 127, 512, frames}) {
        check(graph.captureDsp(tap, frames, block) == expected,
              "The step targets and 128-sample cadence stay independent of host buffer size and lane");
    }
    graph.setSampleRate(32000);
    graph.setValue(time, 8.0f);
    check(graph.captureDsp(tap, frames, 63) == expected,
          "Changing milliseconds and sample rate proportionally keeps the same response");
}

void floatDeZipperSmoothsGreenStepsAndResetsNextSample()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    for (std::size_t block : {1, 13, 127, 300}) {
        Schematic graph;
        graph.setSampleRate(6400);
        const int target = graph.addNode(Float, 0, 0, .25f);
        const int time = graph.addNode(Float, 0, 0, 0.0f);
        const int reset = graph.addNode(Trigger, 0, 0);
        const int smoother = graph.addNode(FloatDeZipper, 0, 0);
        const int out = graph.addNode(AudioOut, 0, 0);
        check(graph.inputType(smoother, 0) == Type::Float && graph.inputType(smoother, 1) == Type::Float &&
              graph.inputType(smoother, 2) == Type::Trigger && graph.outputType(smoother) == Type::Stream,
              "Float De-Zipper takes a Green Float and supplies a generic stream");
        graph.connect({target, smoother, 0});
        graph.connect({time, smoother, 1}); // A wired zero also selects 40 ms.
        graph.connect({reset, smoother, 2}, LinkNotification::Silent);
        graph.connect({smoother, out, 0});
        auto patch = graph.prepareAudio(block);
        const auto render = [&](std::size_t size) {
            std::vector<float> actual(size), right(size);
            check(graph.refreshAudioControls(patch) && patch.processor().process({}, {actual, right}),
                  "Render the live Green control without rebuilding the patch");
            return actual;
        };

        check(render(5) == std::vector<float>(5, .25f), "The first output uses the existing target value");
        graph.setValue(target, 1.25f);
        const auto rising = render(300);
        for (std::size_t n = 0; n < rising.size(); ++n) {
            check(rising[n] == .25f + std::min<std::size_t>(n + 1, 256) / 256.0f,
                  "A unit step reaches its target in 40 ms at every buffer size");
        }

        graph.setValue(target, -.75f);
        graph.setValue(time, 20.0f);
        const auto falling = render(21);
        for (std::size_t n = 0; n < falling.size(); ++n) {
            check(falling[n] == 1.25f - (n + 1) / 128.0f,
                  "A larger falling change uses the same speed per unit without overshoot");
        }
        graph.fireTrigger(reset);
        check(render(5) == std::vector<float>(5, -.75f), "Reset jumps to the target on the next sample");
        graph.setValue(target, -.5f);
        check(render(2) == std::vector<float>{-.75f + 1.0f / 128, -.75f + 2.0f / 128},
              "The Reset pulse is consumed, so later changes still smooth");
    }
}

void streamDeZipperResetsAtTheNextSampleHop()
{
    using enum PrimitiveKind;
    Schematic graph;
    graph.setSampleRate(6400);
    const int in = graph.addNode(AudioIn, 0, 0);
    const int out = graph.addNode(AudioOut, 0, 0);
    const int smoother = graph.addNode(DeZipper, 0, 0);
    const int reset = graph.addNode(Trigger, 0, 0);
    graph.connect({in, smoother, 0});
    graph.connect({reset, smoother, 2}, LinkNotification::Silent);
    graph.connect({smoother, out, 0});
    auto patch = graph.prepareAudio(13);
    const auto render = [&](float target, std::size_t size) {
        std::vector<float> input(size, target), actual(size), right(size);
        check(graph.refreshAudioControls(patch) && patch.processor().process({input, {}}, {actual, right}),
              "Render De-Zipper with live audio buffers");
        return actual;
    };
    check(render(0, 3) == std::vector<float>(3), "Start from the first stream sample");
    const auto before = render(1, 130);
    check(before[124] == 0 && before[125] == 1.0f / 256 && before.back() == 5.0f / 256,
          "A stream step waits for the next hop before ramping at the default time");
    graph.fireTrigger(reset);
    const auto after = render(1, 129);
    check(after.front() == 6.0f / 256 && after[122] == .5f && after[123] == 1 && after.back() == 1,
          "Stream Reset takes effect at the next 128-sample hop even with nonaligned buffers");
}

void variableDelayPrimitivesShiftAnImpulseByTheRequestedSamples()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    constexpr std::size_t frames = 64;
    for (const bool wide : {false, true}) {
        openstone::Session session;
        auto& graph = session.graph();
        const auto code = [&](std::string text) {
            const int id = graph.addNode(DspCode, 0, 0);
            graph.setDspDraft(id, std::move(text));
            session.compileDsp(id);
            return id;
        };
        const int source = code(wide ?
            "streamdoubleout x; double n; x = n == 0 ? 1.0 + 0.0000000000009094947017729282379150390625 : 0; n += 1;" :
            "streamout x; float n; x = n == 0 ? 1 : 0; n += 1;");
        const int tap = code(wide ? "monodoublein x; monodoubleout y; y = x;" : "monoin x; monoout y; y = x;");
        const int delay = graph.addNode(wide ? DoubleDelay : Delay, 0, 0);
        const int samples = graph.addNode(Int, 0, 0);
        graph.connect({source, delay, 0});
        graph.connect({delay, tap, 0});
        graph.connect({samples, delay, 1});
        const auto mono = wide ? Type::MonoDouble : Type::Mono;
        check(graph.outputType(source) == mono && graph.inputType(delay, 0) == mono &&
              graph.inputType(delay, 1) == Type::Int && graph.outputType(delay) == mono,
              "Variable delays expose Mono or MonoDouble and an integer sample count");

        const double impulse = wide ? 1.0 + 0x1p-40 : 1.0;
        const auto expected = [&](int length) {
            std::vector<double> output(frames);
            output[std::max(1, length)] = impulse;
            return output;
        };
        for (int length : {-7, 0, 1, 5, 17, 33}) {
            graph.setValue(samples, length);
            for (std::size_t block : {1, 4, 7, 64}) {
                check(graph.captureDsp(tap, frames, block).front() == expected(length),
                      "Variable delay matches the exact impulse and preserves precision across block sizes");
            }
        }
        graph.disconnectInput(delay, 1);
        check(graph.captureDsp(tap, frames, 7).front() == expected(0),
              "An unconnected NS port defaults to one sample");

        const int gain = graph.addNode(Multiply, 0, 0);
        const int half = graph.addNode(Float, 0, 0, .5f);
        graph.setValue(samples, 5);
        graph.connect({samples, delay, 1});
        graph.connect({delay, gain, 0});
        graph.connect({half, gain, 1});
        graph.connect({gain, delay, 0});
        auto echoes = expected(5);
        for (std::size_t n = 10; n < frames; n += 5) echoes[n] = echoes[n - 5] * .5;
        for (std::size_t block : {1, 4, 7, 64}) {
            check(graph.captureDsp(tap, frames, block).front() == echoes,
                  "A variable delay sums the impulse and feedback wired to the same connector");
        }
    }
}

void variableDelayUpdatesWithoutRestartingLiveAudio()
{
    using enum PrimitiveKind;
    Schematic graph;
    const int in = graph.addNode(AudioIn, 0, 0);
    const int out = graph.addNode(AudioOut, 0, 0);
    const int delay = graph.addNode(Delay, 0, 0);
    const int samples = graph.addNode(Int, 0, 0, 3);
    graph.connect({in, delay, 0});
    graph.connect({samples, delay, 1});
    graph.connect({delay, out, 0});
    const int pack = graph.addNode(Pack, 0, 0);
    const int unpack = graph.addNode(Unpack, 0, 0);
    graph.connect({delay, pack, 2});
    graph.connect({pack, unpack, 0});
    graph.connect({unpack, out, 1, 2});
    auto patch = graph.prepareAudio(3);
    const std::array first{1.f, 2.f, 3.f, 4.f}, second{5.f, 6.f, 7.f, 8.f}, third{9.f, 10.f, 11.f, 12.f};
    std::array<float, 4> delayed{}, tapped{};
    check(patch.processor().process({first, {}}, {delayed, tapped}) && delayed == std::array{0.f, 0.f, 0.f, 1.f} && tapped == delayed,
          "The live patch starts with the configured delay");
    graph.setValue(samples, 1);
    check(graph.refreshAudioControls(patch) &&
          patch.processor().process({second, {}}, {delayed, tapped}) && delayed == std::array{4.f, 5.f, 6.f, 7.f} && tapped == delayed,
          "Shortening the delay uses samples already in memory");
    graph.setValue(samples, 5);
    check(graph.refreshAudioControls(patch) &&
          patch.processor().process({third, {}}, {delayed, tapped}) && delayed == std::array{4.f, 5.f, 6.f, 7.f} && tapped == delayed,
          "Lengthening the delay keeps the earlier history and the existing audio patch");
}

void streamDelayPreservesScalarStateAcrossLiveAudioCalls()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    Schematic graph;
    const int input = graph.addNode(AudioIn, 0, 0);
    const int delay = graph.addNode(DelayOneSample, 0, 0);
    const int idle = graph.addNode(DelayOneSample, 0, 0);
    const int output = graph.addNode(AudioOut, 0, 0);
    graph.connect({input, delay, 0});
    graph.connect({delay, output, 0});
    check(graph.inputType(delay, 0) == Type::Mono && graph.outputType(delay) == Type::Mono,
          "The same delay specializes to Mono through audio endpoints");

    auto patch = graph.prepareAudio(2);
    graph.setLiveAudio(&patch);
    const std::array left{1.f, -2.f, 3.f};
    std::array<float, 3> delayed{}, right{};
    check(patch.processor().process({left, {}}, {delayed, right}) && delayed == std::array{0.f, 1.f, -2.f},
          "The first live audio buffer is shifted by exactly one sample");
    const std::array next{4.f, 5.f, 6.f};
    check(patch.processor().process({next, {}}, {delayed, right}) && delayed == std::array{3.f, 4.f, 5.f},
          "Delay state carries between live audio calls");
    check(graph.liveDspTrace(delay, 0, true).has_value() && !graph.liveDspTrace(idle, 0, true),
          "A live swatch reads the active delay and leaves the disconnected delay idle");
}

void dspCodeNodesCaptureAndKeepNamedLinks()
{
    using openstone::green::Type;
    openstone::Session session;
    auto& graph = session.graph();
    const auto source = session.addNode(PrimitiveKind::DspCode, 0, 0);
    const auto sink = session.addNode(PrimitiveKind::DspCode, 600, 0);
    const auto control = graph.addNode(PrimitiveKind::Int, 0, 100, std::int32_t{3});
    const auto apply = [&](int node, std::string text) {
        graph.setDspDraft(node, std::move(text));
        session.compileDsp(node);
    };
    apply(source, R"(
        monodoubleout impulse;
        double phase;
        impulse = (phase < 1) ? (1.0 + 0.0000000000009094947017729282379150390625) : 0.0;
        phase += 1;
    )");
    apply(sink, R"(
        monodoublein signal;
        monodoublein gain;
        monodoubleout result;
        result = signal * gain;
    )");
    session.connect({source, sink, 0, 0});
    session.connect({control, sink, 1, 0});
    check(graph.outputType(source) == Type::MonoDouble && graph.inputType(sink, 0) == Type::MonoDouble,
          "A DSP block should expose its declared connector types");

    constexpr double expected = (1.0 + 0x1p-40) * 3;
    const auto samples = graph.captureDsp(sink, 13, 1);
    check(samples.front().front() == expected && samples.front().at(1) == 0.0 &&
          graph.captureDsp(sink, 13, 5) == samples,
          "Captures should retain the double samples across irregular render block boundaries");

    apply(sink, R"(
        monodoublein gain;
        monodoublein signal;
        monodoubleout result;
        result = signal * gain;
    )");
    check(graph.links().at(0).input == 1 && graph.links().at(1).input == 0 &&
          graph.captureDsp(sink, 13, 5) == samples,
          "Reordering declarations should keep existing wires on the named inputs");

    bool diagnosed = false;
    graph.setDspDraft(sink, "monoout forgotten;\nforgotten = ;");
    try {
        session.compileDsp(sink);
    } catch (const openstone::stream::DspCompileError& error) {
        diagnosed = error.line == 2 && error.column > 0;
    }
    check(diagnosed && graph.outputType(sink) == Type::MonoDouble && graph.links().size() == 2,
          "A failed compile should report the source location and keep the applied ports and wires");

    apply(sink, graph.nodes().at(1).dspCode->source);
    session.copyNode(sink);
    const auto copy = session.pasteNode(1000, 0, false);
    check(graph.inputType(copy, 1) == Type::MonoDouble && graph.captureDsp(copy, 2, 1).front().front() == 0.0,
          "A copied DSP node should retain its program and default its disconnected inputs to zero");

    const int module = session.makeModule(std::array{source});
    check(module >= 0 && graph.captureDsp(sink, 13, 7) == samples,
          "Capture should follow stream links through module ports");
}

constexpr std::size_t flowStoneTraceFrames = 64;

std::filesystem::path flowStoneFixture(const char* name)
{
    return std::filesystem::path(OPENSTONE_TEST_FIXTURES) / "flowstone" / name;
}

std::string readFlowStoneFixture(const char* name)
{
    std::ifstream file(flowStoneFixture(name), std::ios::binary);
    if (!file) throw std::runtime_error(std::string("Cannot open FlowStone fixture ") + name);
    return std::string(std::istreambuf_iterator<char>(file), {});
}

template<class Word = std::uint32_t, std::size_t Frames = flowStoneTraceFrames>
std::array<Word, Frames> flowStoneReference(const char* name)
{
    const auto bytes = readFlowStoneFixture(name);
    std::array<Word, Frames> samples{};
    check(bytes.size() == sizeof(samples), "The FlowStone trace has the requested length and word width");
    for (std::size_t i = 0; i < samples.size(); ++i) {
        for (unsigned byte = 0; byte < sizeof(Word); ++byte) {
            samples[i] |= static_cast<Word>(static_cast<unsigned char>(bytes[i * sizeof(Word) + byte])) << (8 * byte);
        }
    }
    return samples;
}

void assemblerControlFlowMatchesFlowStone()
{
    using namespace openstone::stream;
    constexpr auto frames = flowStoneTraceFrames;
    const auto input = std::bit_cast<std::array<float, frames>>(flowStoneReference("assembler-jump-input.f32"));
    constexpr std::array cases{
        std::pair{"assembler-jmp-forward.asm", "assembler-jmp-forward.f32"},
        std::pair{"assembler-jnz-forward.asm", "assembler-jnz-forward.f32"},
        std::pair{"assembler-jmp-backward.asm", "assembler-jmp-backward.f32"},
        std::pair{"assembler-jnz-backward.asm", "assembler-jnz-backward.f32"},
        std::pair{"assembler-label-jmp.asm", "assembler-label-jmp.f32"},
        std::pair{"assembler-label-jnz.asm", "assembler-label-jnz.f32"},
        std::pair{"assembler-nested-calls.asm", "assembler-nested-calls.f32"},
        std::pair{"assembler-earlier-direct.asm", "assembler-earlier-calls.f32"},
        std::pair{"assembler-recursive-direct.asm", "assembler-recursive-calls.f32"},
        std::pair{"assembler-fibonacci-direct.asm", "assembler-fibonacci.f32"},
        std::pair{"assembler-fibonacci-early-return-direct.asm", "assembler-fibonacci-early-return.f32"},
        std::pair{"assembler-tail-calls.asm", "assembler-tail-calls.f32"},
        std::pair{"assembler-register-tail-control.asm", "assembler-register-tail-calls.f32"},
    };
    for (const auto& [source, trace] : cases) {
        const auto program = compileDspCode(readFlowStoneFixture(source), DspLanguage::Assembler);
        const auto expected = flowStoneReference(trace);
        for (auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);
            const auto block = graph.addDspCode(program);
            graph.connectDspInput(graph.addInput(), block.id, 0);
            for (std::size_t bufferSize : {1, 7, 64}) {
                auto runtime = graph.compile(block.outputs);
                std::array<float, frames> output{};
                for (std::size_t first = 0; first < frames; first += bufferSize) {
                    const auto count = std::min(bufferSize, frames - first);
                    check(runtime.process(std::array{InputBuffer{std::span{input}.subspan(first, count)}},
                                          std::array{OutputBuffer{std::span{output}.subspan(first, count)}}),
                          "Render Assembler control flow fixture");
                }
                for (std::size_t sample = 0; sample < frames; ++sample) {
                    if (std::bit_cast<std::uint32_t>(output[sample]) != expected[sample]) {
                        const auto engine = backend == DspBackend::LlvmOrc ? "LLVM ORC" : "interpreter";
                        throw std::runtime_error(std::string(source) + " differs on " + engine + " at sample " +
                                                 std::to_string(sample) + " for buffer size " + std::to_string(bufferSize));
                    }
                }
            }
        }
    }
}

void assemblerRegisterCallsMatchFlowStone()
{
    using namespace openstone::stream;
    constexpr auto frames = flowStoneTraceFrames;
    const auto input = std::bit_cast<std::array<float, frames>>(flowStoneReference("assembler-call-input.f32"));
    constexpr std::array cases{
        std::pair{"assembler-register-call.asm", "assembler-register-call.f32"},
        std::pair{"assembler-subroutine-loop.asm", "assembler-subroutine-loop.f32"},
        std::pair{"assembler-nested-register-calls.asm", "assembler-nested-calls.f32"},
        std::pair{"assembler-earlier-register.asm", "assembler-earlier-calls.f32"},
        std::pair{"assembler-recursive-register.asm", "assembler-recursive-calls.f32"},
        std::pair{"assembler-fibonacci-register.asm", "assembler-fibonacci.f32"},
        std::pair{"assembler-fibonacci-early-return-register.asm", "assembler-fibonacci-early-return.f32"},
        std::pair{"assembler-register-tail-calls.asm", "assembler-register-tail-calls.f32"},
        std::pair{"assembler-runtime-call.asm", "assembler-runtime-call.f32"},
        std::pair{"assembler-runtime-tail.asm", "assembler-runtime-tail.f32"},
        std::pair{"assembler-memory-call.asm", "assembler-memory-call.f32"},
        std::pair{"assembler-memory-tail.asm", "assembler-memory-tail.f32"},
        std::pair{"assembler-indirect-call.asm", "assembler-indirect-call.f32"},
        std::pair{"assembler-indirect-tail.asm", "assembler-indirect-tail.f32"},
        std::pair{"assembler-indexed-call.asm", "assembler-indexed-call.f32"},
        std::pair{"assembler-indexed-tail.asm", "assembler-indexed-tail.f32"},
        std::pair{"assembler-indexed-write-call.asm", "assembler-indexed-write-call.f32"},
        std::pair{"assembler-indexed-write-tail.asm", "assembler-indexed-write-tail.f32"},
        std::pair{"assembler-indexed-copy-call.asm", "assembler-indexed-copy-call.f32"},
        std::pair{"assembler-indexed-copy-tail.asm", "assembler-indexed-copy-tail.f32"},
        std::pair{"assembler-chained-copy-call.asm", "assembler-chained-copy-call.f32"},
        std::pair{"assembler-chained-copy-tail.asm", "assembler-chained-copy-tail.f32"},
        std::pair{"assembler-indexed-swap-call.asm", "assembler-indexed-swap-call.f32"},
        std::pair{"assembler-indexed-swap-tail.asm", "assembler-indexed-swap-tail.f32"},
    };
    for (const auto& [sourceName, traceName] : cases) {
        const auto expected = flowStoneReference(traceName);
        const auto eaxSource = readFlowStoneFixture(sourceName);
        auto raxSource = eaxSource;
        for (auto at = raxSource.find("eax"); at != std::string::npos; at = raxSource.find("eax", at + 3)) {
            // Numeric zeroing keeps its 32-bit form. Target transfers exercise the address-register alias.
            if (raxSource.compare(at, 6, "eax,0;") == 0) continue;
            raxSource.replace(at, 3, "rax");
        }

        for (const auto& source : {eaxSource, raxSource}) {
            const auto program = compileDspCode(source, DspLanguage::Assembler);
            for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
                if (!dspBackendAvailable(backend)) continue;
                Graph graph(backend);
                const auto block = graph.addDspCode(program);
                graph.connectDspInput(graph.addInput(), block.id, 0);
                for (std::size_t size : {1, 7, 64}) {
                    auto runtime = graph.compile(block.outputs);
                    if (program.inputs().size() > 1 && program.inputs()[1].type == DspPortType::MemRef) {
                        const openstone::green::Memory memory(std::vector<std::uint8_t>(64));
                        check(runtime.setDspMemRefInput(block.id, 1, memory), "Bind the saved-target Mem input");
                    }
                    std::array<float, frames> output{};
                    for (std::size_t frame = 0; frame < frames; frame += size) {
                        const auto count = std::min(size, frames - frame);
                        check(runtime.process(std::array{InputBuffer{std::span{input}.subspan(frame, count)}},
                                              std::array{OutputBuffer{std::span{output}.subspan(frame, count)}}),
                              "Render local register calls and returns");
                    }
                    for (std::size_t frame = 0; frame < frames; ++frame) {
                        if (std::bit_cast<std::uint32_t>(output[frame]) != expected[frame]) {
                            const auto engine = backend == DspBackend::LlvmOrc ? "LLVM ORC" : "interpreter";
                            const auto reg = source == eaxSource ? "eax" : "rax";
                            throw std::runtime_error(std::string(sourceName) + " using " + reg + " differs on " + engine + " at sample " +
                                                     std::to_string(frame) + " for buffer size " + std::to_string(size));
                        }
                    }
                }
            }
        }
    }
}

void assemblerIndirectMemoryMatchesFlowStone()
{
    using namespace openstone::stream;
    using namespace openstone::green;
    constexpr auto frames = flowStoneTraceFrames;
    constexpr std::array cases{
        std::pair{"assembler-indirect-packed.asm", "assembler-indirect-packed.f32"},
        std::pair{"assembler-indirect-scalar.asm", "assembler-indirect-scalar.f32"},
    };
    const auto input = std::bit_cast<std::array<float, frames>>(flowStoneReference("assembler-indirect-input.f32"));

    for (const auto& [source, reference] : cases) {
        const auto program = compileDspCode(readFlowStoneFixture(source), DspLanguage::Assembler);
        const auto expected = flowStoneReference(reference);
        for (auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);
            const auto block = graph.addDspCode(program);
            graph.connectDspInput(graph.addInput(), block.id, 0);
            for (std::size_t size : {1, 7, 64}) {
                auto runtime = graph.compile(block.outputs);
                const Memory memory(std::vector<std::uint8_t>(64));
                check(runtime.setDspMemRefInput(block.id, 1, memory), "Bind a writable 64-byte Mem to the Assembler case");
                std::array<float, frames> output{};
                for (std::size_t frame = 0; frame < frames; frame += size) {
                    const auto count = std::min(size, frames - frame);
                    check(runtime.process(std::array{InputBuffer{std::span{input}.subspan(frame, count)}},
                                          std::array{OutputBuffer{std::span{output}.subspan(frame, count)}}),
                          "Render the register-indirect Assembler case");
                }
                for (std::size_t frame = 0; frame < frames; ++frame) {
                    if (std::bit_cast<std::uint32_t>(output[frame]) != expected[frame]) {
                        const auto engine = backend == DspBackend::LlvmOrc ? "LLVM ORC" : "interpreter";
                        throw std::runtime_error(std::string(source) + " differs on " + engine + " at sample " +
                                                 std::to_string(frame) + " for buffer size " + std::to_string(size));
                    }
                }
                if (std::string_view(source).ends_with("packed.asm")) {
                    check(memory.sample(0) == input.back() + 0.5F && memory.sample(3) == input.back() + 4.0F,
                          "Packed indirect stores update the connected Mem");
                } else {
                    check(memory.wordAt(1) == std::bit_cast<std::uint32_t>(input.back()) && memory.word(2) == 7U,
                          "Unaligned scalar stores and indirect INC update the connected Mem");
                }
            }
        }
    }

    using enum PrimitiveKind;
    openstone::Session session;
    auto& graph = session.graph();
    const auto text = graph.addNode(String, 0, 0, std::string{"0\n0"});
    const auto buffer = graph.addNode(FloatArrayToFloatMem, 0, 0);
    const auto node = session.addNode(Assembler, 0, 0);
    graph.setDspDraft(node, "memrefin buffer; monoout result; mov eax,buffer[0]; "
                            "mov [eax+4],0x3f800000; fld [eax+4]; fstp result[0];");
    session.compileDsp(node);
    check(graph.inputType(node, 0) == Type::Mem, "Assembler presents a Green Mem inlet");
    graph.connect({text, buffer, 0});
    graph.connect({buffer, node, 0});
    check(graph.captureDsp(node, 3, 2).front() == std::vector<double>(3, 1.0) &&
          std::get<Memory>(graph.readValue(buffer)).sample(1) == 1.F,
          "The editor binds the connected Mem and exposes Assembler stores to Green");
}

void assemblerMultiSampleMatchesFlowStoneCaptures()
{
    using enum PrimitiveKind;
    using namespace openstone::stream;
    using namespace openstone::green;
    constexpr auto frames = flowStoneTraceFrames;
    const auto source = readFlowStoneFixture("multi-sample-bounds.asm");
    const auto empty = flowStoneReference("multi-sample-unwired.f32");
    const auto loaded = flowStoneReference("multi-sample-one-wave.f32");

    openstone::Session session;
    auto& schematic = session.graph();
    const auto stimulus = session.addNode(DspCode, 0, 0);
    const auto mask = session.addNode(Assembler, 0, 0);
    schematic.setDspDraft(stimulus, "monoout stimulus; float n; stimulus = n * 0.25 - 8.0; n = n + 1;");
    schematic.setDspDraft(mask, source);
    session.compileDsp(stimulus);
    session.compileDsp(mask);
    session.connect({stimulus, mask, 1});
    check(schematic.inputType(mask, 0) == Type::MemArray && schematic.outputType(mask) == Type::Mono,
          "Assembler exposes a MemArray inlet and keeps the signal wired as Mono");

    const auto checkSchematic = [&](const auto& expected) {
        for (std::size_t blockSize : {1, 7, 64}) {
            const auto result = schematic.captureDsp(mask, frames, blockSize);
            check(result.size() == 1 && result.front().size() == expected.size() &&
                  std::ranges::equal(result.front(), expected, {}, [](double sample) {
                      return std::bit_cast<std::uint32_t>(static_cast<float>(sample));
                  }), "The editor capture matches the FlowStone mask sample for sample");
        }
    };
    checkSchematic(empty);

    const auto waveArray = session.addNode(WaveFileArray, 0, 0);
    const auto waveCount = schematic.addNode(Int, 0, 0, std::int32_t{1});
    const auto filename = schematic.addNode(String, 0, 0, flowStoneFixture("test-wave.wav").string());
    const auto resize = session.addNode(Trigger, 0, 0);
    const auto load = session.addNode(Trigger, 0, 0);
    session.connect({waveCount, waveArray, 0});
    session.connect({resize, waveArray, 1});
    session.connect({filename, waveArray, 4});
    session.connect({load, waveArray, 5});
    session.connect({waveArray, mask, 0});
    schematic.fireTrigger(resize);
    schematic.fireTrigger(load);
    const auto wave = std::get<MemoryArray>(schematic.readValue(waveArray));
    check(wave.entries().size() == 1 && wave.entries()[0].size() > 0, "The Wave File Array loads the fixture");
    checkSchematic(loaded);

    const auto program = compileDspCode(source, DspLanguage::Assembler);
    for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto input = graph.addInput();
        const auto assembler = graph.addDspCode(program);
        graph.connectDspInput(input, assembler.id, 1);
        std::array<Mono4, frames> waveIndex{};
        for (std::size_t i = 0; i < frames; ++i) {
            for (std::size_t lane = 0; lane < 4; ++lane) {
                waveIndex[i][lane] = static_cast<float>((i + lane * 17) % frames) * 0.25F - 8.F;
            }
        }
        auto runtime = graph.compileMono4(assembler.outputs);
        const auto checkPacked = [&](const auto& expected) {
            for (std::size_t blockSize : {1, 7, 64}) {
                runtime.reset();
                std::array<Mono4, frames> output{};
                for (std::size_t frame = 0; frame < frames; frame += blockSize) {
                    const auto count = std::min(blockSize, frames - frame);
                    check(runtime.process(std::array{Mono4InputBuffer{waveIndex}.subspan(frame, count)},
                                          std::array{Mono4OutputBuffer{output}.subspan(frame, count)}),
                          "Render the unchanged Assembler on packed streams");
                }
                for (std::size_t i = 0; i < frames; ++i) {
                    for (std::size_t lane = 0; lane < 4; ++lane) {
                        check(std::bit_cast<std::uint32_t>(output[i][lane]) == expected[(i + lane * 17) % frames],
                              "The reference mask agrees on every packed lane");
                    }
                }
            }
        };
        checkPacked(empty);
        check(runtime.setDspMemArrayInput(assembler.id, 0, wave) &&
              !runtime.setDspArrayInput(assembler.id, 0, {}) &&
              !runtime.setDspMemArrayInput(assembler.id, 1, wave),
              "Only MemArray data belongs in the MemArray inlet");
        checkPacked(loaded);
        check(runtime.setDspMemArrayInput(assembler.id, 0, {}), "Clear the MemArray snapshot");
        checkPacked(empty);
    }
}

void assemblerMemArrayWaveData()
{
    using namespace openstone::stream;
    constexpr std::size_t frames = 64;
    const auto positions = std::bit_cast<std::array<float, frames>>(flowStoneReference("memarray-input.f32"));
    std::vector<float> mono, stereo;
    for (int i = 0; i < 16; ++i) {
        mono.push_back(((i % 7) - 3) * 2048 / 32768.F);
        stereo.push_back((8192 + i * 256) / 32768.F);
        stereo.push_back((-16384 + i * 128) / 32768.F);
    }
    const auto monoMemory = openstone::green::Memory::fromSamples(mono);
    const auto stereoMemory = openstone::green::Memory::fromSamples(stereo, 2);
    for (int test = 0; test < 3; ++test) {
        const bool isStereo = test == 1;
        const openstone::green::MemoryArray waves(test == 2
            ? std::vector{monoMemory, stereoMemory} : std::vector{isStereo ? stereoMemory : monoMemory});
        for (const bool right : {false, true}) {
            std::array<std::uint32_t, frames> reference{};
            if (test < 2) {
                const auto name = std::string("memarray-") + (isStereo ? "stereo-" : "mono-") + (right ? "right.f32" : "left.f32");
                reference = flowStoneReference(name.c_str());
            }
            const auto source = readFlowStoneFixture(right ? "memarray-wave-right.asm" : "memarray-wave-left.asm");
            const auto program = compileDspCode(source, DspLanguage::Assembler);
            for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
                if (!dspBackendAvailable(backend)) continue;
                Graph graph(backend);
                const auto block = graph.addDspCode(program);
                graph.connectDspInput(graph.addInput(), block.id, 1);
                graph.connectDspInput(graph.addInput(), block.id, 2);
                for (const std::size_t size : {1, 7, 64}) {
                    auto runtime = graph.compileMono4(std::span{block.outputs}.first(1));
                    check(runtime.setDspMemArrayInput(block.id, 0, waves), "Bind loaded wave data to the bundled reader");
                    std::array<Mono4, frames> selection{}, input{}, output{};
                    for (std::size_t i = 0; i < frames; ++i) for (std::size_t lane = 0; lane < 4; ++lane) {
                        input[i][lane] = positions[(i + lane * 17) % frames];
                        if (test == 2) selection[i][lane] = (static_cast<int>((i + lane * 11) % 7) - 2) * .5F;
                    }
                    for (std::size_t frame = 0; frame < frames; frame += size) {
                        const auto count = std::min(size, frames - frame);
                        check(runtime.process(std::array{Mono4InputBuffer{std::span{selection}.subspan(frame, count)},
                                                         Mono4InputBuffer{std::span{input}.subspan(frame, count)}},
                                              std::array{Mono4OutputBuffer{std::span{output}.subspan(frame, count)}}), "Render the bundled wave reader");
                    }
                    for (std::size_t i = 0; i < frames; ++i) for (std::size_t lane = 0; lane < 4; ++lane) {
                        const bool selectedStereo = isStereo || (test == 2 && selection[i][lane] > .5F);
                        const auto index = static_cast<std::size_t>(std::nearbyint(std::clamp(input[i][lane], 0.F, 15.F)));
                        const auto expected = selectedStereo ? stereo[index * 2 + (right ? 1 : 0)] : mono[index];
                        if (test < 2) check(std::bit_cast<std::uint32_t>(output[i][lane]) == reference[(i + lane * 17) % frames],
                            "The bundled MemArray reader matches every native FlowStone sample word on every lane");
                        if (output[i][lane] != expected) throw std::runtime_error("MemArray wave mismatch at frame " +
                            std::to_string(i) + " lane " + std::to_string(lane) + " actual " + std::to_string(output[i][lane]) +
                            " expected " + std::to_string(expected));
                    }
                }
            }
        }
    }
}

void assemblerX87MatchesFlowStoneCaptures()
{
    using namespace openstone::stream;
    constexpr auto frames = flowStoneTraceFrames;
    constexpr auto traceFrame = [](std::size_t frame, std::size_t lane) { return (frame + lane * 17) % frames; };
    constexpr std::array<std::pair<const char*, const char*>, 2> cases{{
        {"stream-power.asm", "stream-power.f32"},
        {"compressor-power.asm", "compressor-power.f32"},
    }};
    std::array<Mono4, frames> exponents{}, bases{};
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::size_t lane = 0; lane < 4; ++lane) {
            const auto n = static_cast<float>(traceFrame(frame, lane));
            exponents[frame][lane] = 2.F - n * 0.0078125F;
            bases[frame][lane] = 0.5F + n * 0.125F;
        }
    }

    for (const auto& [sourceName, referenceName] : cases) {
        const auto source = readFlowStoneFixture(sourceName);
        const auto expected = flowStoneReference(referenceName);
        const auto program = compileDspCode(source, DspLanguage::Assembler);
        for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);
            const auto exponent = graph.addInput();
            const auto base = graph.addInput();
            const auto block = graph.addDspCode(program);
            graph.connectDspInput(exponent, block.id, 0);
            graph.connectDspInput(base, block.id, 1);
            auto runtime = graph.compileMono4(block.outputs);

            for (const std::size_t blockSize : {1, 7, 64}) {
                runtime.reset();
                std::array<Mono4, frames> output{};
                for (std::size_t frame = 0; frame < frames; frame += blockSize) {
                    const auto count = std::min(blockSize, frames - frame);
                    check(runtime.process(std::array{Mono4InputBuffer{exponents}.subspan(frame, count),
                                                     Mono4InputBuffer{bases}.subspan(frame, count)},
                                          std::array{Mono4OutputBuffer{output}.subspan(frame, count)}),
                          "Render the bundled x87 Assembler on packed streams");
                }
                for (std::size_t frame = 0; frame < frames; ++frame) {
                    for (std::size_t lane = 0; lane < 4; ++lane) {
                        if (std::bit_cast<std::uint32_t>(output[frame][lane]) != expected[traceFrame(frame, lane)]) {
                            const auto engine = backend == DspBackend::LlvmOrc ? "LLVM ORC" : "interpreter";
                            throw std::runtime_error(std::string(sourceName) + " differs from " + referenceName + " on "
                                + engine + " at frame " + std::to_string(frame) + " lane " + std::to_string(lane)
                                + " with buffer size " + std::to_string(blockSize));
                        }
                    }
                }
            }
        }
    }
}

void assemblerCommunityX87ProbeHasDocumentedReadouts()
{
    using namespace openstone::stream;
    const auto program = compileDspCode(readFlowStoneFixture("x87-extended-probe.asm"), DspLanguage::Assembler);
    constexpr std::array expected{1.F, 1.F, 1.F, 0.F, 1.F, 4.F, 1.F, 0.F};
    for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto block = graph.addDspCode(program);
        for (const std::size_t size : {1, 7, 64}) {
            auto runtime = graph.compile(block.outputs);
            std::array<std::array<float, 64>, expected.size()> output{};
            for (std::size_t frame = 0; frame < 64; frame += size) {
                const auto count = std::min(size, 64 - frame);
                std::array<OutputBuffer, expected.size()> buffers;
                for (std::size_t channel = 0; channel < buffers.size(); ++channel) buffers[channel] = std::span{output[channel]}.subspan(frame, count);
                check(runtime.process({}, buffers), "Render the community x87 precision probe");
            }
            for (std::size_t channel = 0; channel < expected.size(); ++channel) check(std::ranges::all_of(output[channel],
                [&](float sample) { return sample == expected[channel]; }), "The community probe produces the documented full-precision readouts");
        }
    }
}

void assemblerDenormalMaskMatchesFlowStoneWithMeasuredRegisters()
{
    using namespace openstone::stream;
    constexpr auto frames = flowStoneTraceFrames;
    const auto original = readFlowStoneFixture("denormal-mask.asm");
    const auto stimulus = flowStoneReference("denormal-input.f32");
    using Words = decltype(flowStoneReference(""));
    const auto match = [&](const std::string& source, std::span<const Words> inputs, const char* referenceName,
                           const Words* entry = nullptr) {
        const auto expected = flowStoneReference(referenceName);
        const auto program = compileDspCode(source, DspLanguage::Assembler);
        std::vector<std::array<float, frames>> samples(inputs.size());
        std::vector<InputBuffer> buffers(inputs.size());
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            std::ranges::transform(inputs[i], samples[i].begin(), std::bit_cast<float, std::uint32_t>);
        }
        for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);
            const auto block = graph.addDspCode(program);
            for (std::size_t i = 0; i < inputs.size(); ++i) {
                graph.connectDspInput(graph.addInput(), block.id, i);
            }
            auto runtime = graph.compile(block.outputs);
            if (entry) {
                std::array<DspXmmRegister, frames> incoming;
                for (std::size_t n = 0; n < frames; ++n) incoming[n].fill((*entry)[n]);
                check(runtime.setDspIncomingXmm(block.id, 1, incoming), "Bind measured incoming xmm1 without changing the program");
            }
            for (std::size_t size : {1, 7, 64}) {
                runtime.reset();
                std::array<float, frames> output{};
                for (std::size_t frame = 0; frame < frames; frame += size) {
                    const auto count = std::min(size, frames - frame);
                    for (std::size_t i = 0; i < inputs.size(); ++i) {
                        buffers[i] = InputBuffer{samples[i]}.subspan(frame, count);
                    }
                    check(runtime.process(buffers, std::array{OutputBuffer{output}.subspan(frame, count)}),
                          "Render the FlowStone denormal regression");
                }
                const auto actual = std::bit_cast<Words>(output);
                if (actual != expected) {
                    const auto frame = std::ranges::mismatch(actual, expected).in1 - actual.begin();
                    throw std::runtime_error(std::string(referenceName) + " differs at sample " + std::to_string(frame));
                }
            }
        }
    };

    // Replay both captured register banks through the incoming-register API.
    const auto firstEntry = flowStoneReference("denormal-xmm1-original.f32");
    const auto secondEntry = flowStoneReference("denormal-xmm1-reordered.f32");
    const std::span singleInput{&stimulus, 1};
    match(original, singleInput, "denormal-mask-original.f32", &firstEntry);
    match(original, singleInput, "denormal-mask-reordered.f32", &secondEntry);
    match(original, singleInput, "denormal-mask-zeroed.f32");

    auto initialized = original;
    const auto selfOr = initialized.find("por xmm1,xmm1;");
    check(selfOr != std::string::npos, "Locate the self-OR in the unchanged fixture");
    initialized.replace(selfOr, std::string_view("por").size(), "pxor");
    match(initialized, singleInput, "denormal-mask-zeroed.f32");

    const std::string load = "streamin in; streamout out; movaps xmm0,in;";
    match(load + "movaps out,xmm0;", singleInput, "denormal-input.f32");
    match(load + "float factor=1; mulps xmm0,factor; movaps out,xmm0;", singleInput, "denormal-mul-one.f32");
    match(load + "float factor=0.5; mulps xmm0,factor; movaps out,xmm0;", singleInput, "denormal-mul-half.f32");
}

using DoubleTrace = std::array<double, flowStoneTraceFrames>;

DoubleTrace readFlowStoneDoubleTrace(const char* filename)
{
    return std::bit_cast<DoubleTrace>(flowStoneReference<std::uint64_t>(filename));
}

void checkDoubleAssemblerTrace(const openstone::stream::DspProgram& program, std::string_view label,
                               std::span<const DoubleTrace> inputs, std::span<const std::uint64_t> expected)
{
    using namespace openstone::stream;
    constexpr auto frames = flowStoneTraceFrames;
    const auto laneFrame = [](std::size_t frame, std::size_t lane) { return (frame + 17 * lane) % frames; };
    std::vector<std::array<MonoDouble4, frames>> packedInputs(inputs.size());
    for (std::size_t port = 0; port < inputs.size(); ++port) {
        for (std::size_t frame = 0; frame < frames; ++frame) {
            for (std::size_t lane = 0; lane < 4; ++lane) {
                packedInputs[port][frame][lane] = inputs[port][laneFrame(frame, lane)];
            }
        }
    }

    for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto block = graph.addDspCode(program);
        for (std::size_t port = 0; port < inputs.size(); ++port) {
            graph.connectDspInput(graph.addInput(), block.id, port);
        }
        const auto compare = [&]<class Sample>(auto runtime, std::span<const std::array<Sample, frames>> inputSamples,
                                              auto sampleAt, std::size_t lanes, std::size_t laneBase = 0) {
            std::vector<std::span<const Sample>> buffers(inputSamples.size());
            for (std::size_t size : {1, 7, 64}) {
                runtime.reset();
                std::array<Sample, frames> output{};
                for (std::size_t frame = 0; frame < frames; frame += size) {
                    const auto count = std::min(size, frames - frame);
                    for (std::size_t port = 0; port < inputSamples.size(); ++port) {
                        buffers[port] = std::span{inputSamples[port]}.subspan(frame, count);
                    }
                    check(runtime.process(buffers,
                                          std::array{std::span<Sample>{output}.subspan(frame, count)}),
                          "Render the double Assembler in buffer chunks");
                }
                for (std::size_t frame = 0; frame < frames; ++frame) {
                    for (std::size_t lane = 0; lane < lanes; ++lane) {
                        const auto bits = std::bit_cast<std::uint64_t>(sampleAt(output[frame], lane));
                        if (bits != expected[laneFrame(frame, laneBase + lane)]) {
                            const auto engine = backend == DspBackend::LlvmOrc ? "LLVM ORC" : "interpreter";
                            throw std::runtime_error(std::string(label) + " differs on " + engine + " at sample "
                                + std::to_string(frame) + ", lane " + std::to_string(laneBase + lane)
                                + ", buffer size " + std::to_string(size)
                                + " (actual word " + std::to_string(bits)
                                + ", reference " + std::to_string(expected[laneFrame(frame, laneBase + lane)]) + ")");
                        }
                    }
                    if (lanes == 2) {
                        check(std::bit_cast<std::uint64_t>(sampleAt(output[frame], 2)) == 0 &&
                              std::bit_cast<std::uint64_t>(sampleAt(output[frame], 3)) == 0,
                              "A single 128-bit Double store preserves the untouched upper packet window");
                    }
                }
            }
        };
        compare(graph.compileMonoDouble(block.outputs), inputs,
                [](double sample, std::size_t) { return sample; }, 1);
        // Captured bodies use one physical XMM window. Replay both pairs so
        // all four independent reference rotations remain covered unchanged.
        for (const std::size_t laneBase : {0, 2}) {
            auto windows = packedInputs;
            for (auto& channel : windows) for (auto& sample : channel) {
                std::rotate(sample.begin(), sample.begin() + laneBase, sample.end());
            }
            compare(graph.compileMonoDouble4(block.outputs), std::span<const std::array<MonoDouble4, frames>>{windows},
                    [](const MonoDouble4& sample, std::size_t lane) { return sample[lane]; }, 2, laneBase);
        }
    }
}

void assemblerDoubleMatchesFlowStoneCaptures()
{
    using namespace openstone::stream;
    const std::array<DoubleTrace, 1> singleInput{readFlowStoneDoubleTrace("assembler-double-input.f64")};
    const std::array pairedInputs{readFlowStoneDoubleTrace("assembler-double-math-lhs.f64"),
                                  readFlowStoneDoubleTrace("assembler-double-math-rhs.f64")};
    struct Case { std::string_view mnemonic; bool paired; };
    constexpr Case cases[]{
        {"addpd", false}, {"subpd", false}, {"mulpd", false},
        {"divpd", true}, {"minpd", true}, {"maxpd", true}, {"sqrtpd", true},
    };

    for (const auto& test : cases) {
        const auto opcode = std::string(test.mnemonic);
        const auto fixture = "assembler-" + opcode;
        const auto source = readFlowStoneFixture((fixture + ".asm").c_str());
        const auto expected = flowStoneReference<std::uint64_t>((fixture + ".f64").c_str());
        const auto inputs = test.paired ? std::span<const DoubleTrace>{pairedInputs} : std::span<const DoubleTrace>{singleInput};
        const auto program = compileDspCode(source, DspLanguage::Assembler);
        check(program.inputs().size() == inputs.size() &&
              std::ranges::all_of(program.inputs(), [](const auto& port) { return port.type == DspPortType::StreamDouble; }) &&
              program.outputs().size() == 1 && program.outputs().front().type == DspPortType::StreamDouble,
              "Double Assembler keeps its ports at binary64 precision");

        openstone::Session session;
        auto& schematic = session.graph();
        const auto generator = session.addNode(PrimitiveKind::DspCode, 0, 0);
        const auto assembly = session.addNode(PrimitiveKind::Assembler, 0, 0);
        const auto ramp = test.paired ? R"dsp(
            monodoubleout stimulus;
            monodoubleout rhs;
            double n;
            double epsilon = 0x3d70000000000000;
            stimulus = (n + 1.0) * 0.125 + (n + 1.0) * epsilon;
            rhs = (48.0 - n) * 0.0625 + 0.25 + (n + 2.0) * epsilon;
            n = n + 1.0;
        )dsp" : R"dsp(
            monodoubleout stimulus;
            double n;
            double epsilon = 0x3d70000000000000;
            stimulus = (n - 32.0) * 0.125 + (n + 1.0) * epsilon;
            n = n + 1.0;
        )dsp";
        schematic.setDspDraft(generator, ramp);
        schematic.setDspDraft(assembly, source);
        session.compileDsp(generator);
        session.compileDsp(assembly);
        for (int port = 0; port < static_cast<int>(inputs.size()); ++port) {
            session.connect({generator, assembly, port, port});
            check(schematic.inputType(assembly, port) == openstone::green::Type::MonoDouble,
                  "Double Assembler inputs resolve through ordinary MonoDouble links");
        }
        check(schematic.outputType(assembly) == openstone::green::Type::MonoDouble,
              "Double Assembler outputs preserve the inferred MonoDouble type");
        const auto captured = schematic.captureDsp(assembly, 8, 7);
        check(captured.size() == 1 && captured.front().size() == 8 &&
              std::ranges::equal(captured.front(), std::span{expected}.first(8), {},
                                 std::bit_cast<std::uint64_t, double>),
              "The editor captures the double Assembler result with the saved low bits intact");

        checkDoubleAssemblerTrace(program, opcode, inputs, expected);
    }
}

void assemblerFloatingPointEdgesMatchFlowStone()
{
    using namespace openstone::stream;
    constexpr auto frames = flowStoneTraceFrames;
    const std::array inputs{readFlowStoneDoubleTrace("assembler-fp64-lhs.f64"), readFlowStoneDoubleTrace("assembler-fp64-rhs.f64")};
    for (const auto* opcode : {"lhs", "rhs", "addpd", "subpd", "mulpd", "divpd", "sqrtpd", "minpd", "maxpd"}) {
        const auto fixture = std::string("assembler-fp64-") + opcode;
        const auto program = compileDspCode(readFlowStoneFixture((fixture + ".asm").c_str()), DspLanguage::Assembler);
        const auto expected = flowStoneReference<std::uint64_t>((fixture + ".f64").c_str());
        checkDoubleAssemblerTrace(program, fixture, inputs, expected);
    }
    const auto inputProgram = compileDspCode(readFlowStoneFixture("assembler-fp32-input.dsp"));
    const auto reader = compileDspCode(readFlowStoneFixture("assembler-fp32-read-bits.asm"), DspLanguage::Assembler);
    for (const auto* opcode : {"lhs", "rhs", "addps", "subps", "mulps", "divps", "sqrtps", "minps", "maxps", "cvtps2dq", "cvttps2dq"}) {
        const auto fixture = std::string("assembler-fp32-") + opcode;
        const auto program = compileDspCode(readFlowStoneFixture((fixture + ".asm").c_str()), DspLanguage::Assembler);
        const auto expected = flowStoneReference((fixture + ".f32").c_str());
        for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);
            const auto generator = graph.addDspCode(inputProgram), block = graph.addDspCode(program), bits = graph.addDspCode(reader);
            graph.connectDspInput(generator.outputs[0], block.id, 0);
            graph.connectDspInput(generator.outputs[1], block.id, 1);
            graph.connectDspInput(block.outputs[0], bits.id, 0);
            const auto compare = [&]<class Sample>(auto runtime, auto sampleAt, std::size_t lanes) {
                for (std::size_t size : {1, 7, 64}) {
                    runtime.reset();
                    std::array<std::array<Sample, frames>, 2> output{};
                    for (std::size_t frame = 0; frame < frames; frame += size) {
                        const auto count = std::min(size, frames - frame);
                        check(runtime.process({}, std::array{std::span{output[0]}.subspan(frame, count), std::span{output[1]}.subspan(frame, count)}),
                            "Render raw single-precision SSE result bits");
                    }
                    for (std::size_t frame = 0; frame < frames; ++frame) for (std::size_t lane = 0; lane < lanes; ++lane) {
                        const auto actual = static_cast<std::uint32_t>(sampleAt(output[0][frame], lane)) |
                            (static_cast<std::uint32_t>(sampleAt(output[1][frame], lane)) << 16);
                        const auto reference = expected[(frame + 17 * lane) % frames];
                        if (actual != reference) throw std::runtime_error(fixture + " differs on " +
                            (backend == DspBackend::LlvmOrc ? "LLVM ORC" : "interpreter") + " at sample " + std::to_string(frame) +
                            ", lane " + std::to_string(lane) + ", buffer " + std::to_string(size) +
                            ", actual " + std::to_string(actual) + ", reference " + std::to_string(reference));
                    }
                }
            };
            compare.template operator()<float>(graph.compile(bits.outputs), [](float sample, std::size_t) { return sample; }, 1);
            compare.template operator()<Mono4>(graph.compileMono4(bits.outputs), [](const Mono4& sample, std::size_t lane) { return sample[lane]; }, 4);
        }
    }
}

void assemblerDoubleComparisonsMatchFlowStoneMasks()
{
    using namespace openstone::stream;
    constexpr std::array aliases{"cmpeqpd", "cmpltpd", "cmplepd", "cmpunordpd", "cmpneqpd", "cmpnltpd", "cmpnlepd", "cmpordpd"};
    constexpr auto frames = flowStoneTraceFrames;
    const std::array inputs{readFlowStoneDoubleTrace("assembler-cmppd-lhs.f64"),
                            readFlowStoneDoubleTrace("assembler-cmppd-rhs.f64")};
    const auto masks = flowStoneReference<std::uint64_t, aliases.size() * frames>("assembler-cmppd-masks.f64");
    const std::string header = "streamdoublein in; streamdoublein rhs; streamdoubleout out; movapd xmm0,in; ";
    constexpr auto store = " movapd out,xmm0;";

    for (std::size_t predicate = 0; predicate < aliases.size(); ++predicate) {
        const auto expected = std::span{masks}.subspan(predicate * frames, frames);
        check(std::ranges::contains(expected, std::uint64_t{0}) &&
              std::ranges::contains(expected, std::numeric_limits<std::uint64_t>::max()),
              "The FlowStone fixture exercises both mask outcomes for each predicate");
        const auto direct = "cmppd xmm0,rhs," + std::to_string(predicate) + ";";
        checkDoubleAssemblerTrace(compileDspCode(header + direct + store, DspLanguage::Assembler),
                                  direct, inputs, expected);

        const std::string alias = aliases[predicate];
        const auto source = header + "movapd xmm1,rhs; " + alias + " xmm0,xmm1;" + store;
        checkDoubleAssemblerTrace(compileDspCode(source, DspLanguage::Assembler), alias, inputs, expected);
    }
}

void assemblerDoubleBitwiseMatchesFlowStoneCaptures()
{
    using namespace openstone::stream;
    const std::array inputs{readFlowStoneDoubleTrace("assembler-cmppd-lhs.f64"),
                            readFlowStoneDoubleTrace("assembler-cmppd-rhs.f64")};
    for (const std::string opcode : {"andpd", "andnpd", "orpd", "xorpd"}) {
        const auto fixture = "assembler-" + opcode;
        const auto program = compileDspCode(readFlowStoneFixture((fixture + ".asm").c_str()), DspLanguage::Assembler);
        const auto expected = flowStoneReference<std::uint64_t>((fixture + ".f64").c_str());
        checkDoubleAssemblerTrace(program, opcode, inputs, expected);
    }
}

void dspSwatchesObserveOnlyTheLastCapture()
{
    openstone::Session session;
    auto& graph = session.graph();
    const auto addCode = [&](std::string text) {
        const auto id = session.addNode(PrimitiveKind::DspCode, 0, 0);
        graph.setDspDraft(id, std::move(text));
        session.compileDsp(id);
        return id;
    };
    const int ramp = addCode("streamout ramp; float phase; ramp = phase; phase += 1;");
    const int sink = addCode(R"(
        streamin signal;
        streamin unused;
        streamout scaled;
        scaled = signal * 2;
    )");
    const int unused = addCode("streamout absent; absent = 11;");
    session.connect({ramp, sink, 0});
    session.connect({unused, sink, 1});
    graph.setDspDraft(unused, "A broken draft on an unused input");
    check(graph.dspTrace(ramp, 0, true).empty() && graph.dspTrace(sink, 0, false).empty(),
          "Inspecting an idle Stream should not start a render");

    const auto capture = graph.captureDsp(sink, 160, 17);
    const auto upstream = graph.dspTrace(ramp, 0, true);
    const auto input = graph.dspTrace(sink, 0, false);
    const auto output = graph.dspTrace(sink, 0, true);
    check(upstream.size() == 128 && upstream.front() == 32 && upstream.back() == 159 &&
          std::ranges::equal(input, upstream),
          "The input and source should share the same trace across block boundaries");
    check(std::ranges::equal(output, std::span{capture.front()}.last(128)),
          "The output trace should reflect the actual final samples of the capture");
    check(graph.dspTrace(unused, 0, true).empty() && graph.dspTrace(sink, 1, false).empty(),
          "A wire to an unused DSP input should never activate its source for inspection");

    const int module = session.makeModule(std::array{ramp});
    check(graph.dspTrace(sink, 0, false).empty(), "A graph rebuild should clear the stale trace");
    static_cast<void>(graph.captureDsp(sink, 6, 4));
    check(graph.dspTrace(module, 0, true).size() == 6 && graph.dspTrace(sink, 0, false).back() == 5,
          "The swatch should follow captured signals through module ports");

    const int poly = addCode("polyout velocity; velocity = 0.25;");
    static_cast<void>(graph.captureDsp(poly, 1, 1));
    const auto voice = graph.dspTrace(poly, 0, true);
    check(voice.size() == 1 && voice.front() == 0.25 && graph.dspTrace(sink, 0, true).empty(),
          "A Poly capture should publish its rendered lane and replace the previous capture");
}

void genericStreamsFollowTheirConnectedDspSection()
{
    using enum PrimitiveKind;
    using openstone::green::FloatArray;
    using openstone::green::Type;
    openstone::Session session;
    auto& graph = session.graph();
    const auto addCode = [&](std::string text) {
        const auto id = session.addNode(DspCode, 0, 0);
        graph.setDspDraft(id, std::move(text));
        session.compileDsp(id);
        return id;
    };
    const auto source = addCode("streamout signal; float phase; signal = phase; phase += 1;");
    const auto scale = addCode("streamin x; streamout y; y = x * 2;");
    const auto mono = session.addNode(MonoToGraph, 0, 0);
    const auto poly = session.addNode(SignalAnalyser, 0, 0);
    const auto ns = graph.addNode(Int, 0, 0, std::int32_t{5});
    session.connect({ns, mono, 1});
    session.connect({ns, poly, 1});
    session.connect({source, scale, 0});
    check(graph.outputType(source) == Type::Stream && graph.inputType(scale, 0) == Type::Stream &&
          graph.outputType(scale) == Type::Stream && graph.dspTrace(source, 0, true).empty(),
          "A generic chain without a typed anchor should stay grey and idle");

    session.connect({scale, mono, 0});
    check(graph.outputType(source) == Type::Mono && graph.inputType(scale, 0) == Type::Mono &&
          graph.outputType(scale) == Type::Mono && graph.dspTrace(source, 0, true).empty(),
          "A Mono destination should resolve the whole generic chain without rendering it");
    graph.fireTrigger(mono);
    check(std::get<FloatArray>(graph.readValue(mono)) == FloatArray{0, 2, 4, 6, 8},
          "Mono to Graph should capture a DSP Code section that uses streamout");
    const auto linkCount = graph.links().size();
    bool rejected = false;
    try {
        session.connect({scale, poly, 0});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected && graph.links().size() == linkCount && graph.outputType(scale) == Type::Mono,
          "Conflicting concrete destinations should leave existing links and types intact");

    const int module = session.makeModule(std::array{scale});
    const auto& child = *graph.module(module);
    graph.disconnectInput(mono, 0);
    check(graph.outputType(source) == Type::Stream && graph.inputType(module, 0) == Type::Stream &&
          graph.outputType(module) == Type::Stream && child.outputType(scale) == Type::Stream,
          "Removing the last typed anchor should revert generic connectors across module boundaries");
    session.connect({module, poly, 0});
    check(graph.outputType(source) == Type::Poly && graph.inputType(module, 0) == Type::Poly &&
          graph.outputType(module) == Type::Poly && child.outputType(scale) == Type::Poly,
          "A Poly destination should resolve the same patch on both sides of module boundaries");
    graph.fireTrigger(poly);
    check(std::get<FloatArray>(graph.readValue(poly)) == FloatArray{0, 2, 4, 6, 8},
          "Signal Analyser should capture the Poly preview through a generic module");

    const auto typed = addCode("monoout x; x = 7;");
    const auto generic = addCode("streamin x; streamout y; y = x;");
    session.connect({typed, generic, 0});
    check(graph.inputType(generic, 0) == Type::Mono && graph.outputType(generic) == Type::Mono,
          "An upstream concrete source should also resolve generic ports");

    const auto doubleSource = addCode("streamdoubleout x; x = 1.0 + 0.0000000000009094947017729282379150390625;");
    const auto doubleTap = addCode("monodoublein x; streamdoubleout y; y = x;");
    session.connect({doubleSource, doubleTap, 0});
    check(graph.outputType(doubleSource) == Type::MonoDouble &&
          graph.captureDsp(doubleTap, 2, 1).front().front() == 1.0 + 0x1p-40,
          "StreamDouble should adopt MonoDouble and preserve its precision");
    const auto doublePoly = addCode("polydoublein x; polydoubleout y; y = x;");
    graph.disconnectInput(doubleTap, 0);
    session.connect({doubleSource, doublePoly, 0});
    check(graph.outputType(doubleSource) == Type::PolyDouble, "StreamDouble should also adopt PolyDouble");

    rejected = false;
    try {
        session.connect({typed, doubleTap, 0});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "Generic inference should preserve signal precision on wires");

    const auto packed = session.addNode(Module, 0, 0);
    auto& packedChild = *graph.module(packed);
    const auto floatPort = packedChild.addNode(ModuleInput, 0, 0);
    const auto doublePort = packedChild.addNode(ModuleInput, 0, 60);
    const auto floatOut = packedChild.addNode(ModuleOutput, 100, 0);
    const auto doubleOut = packedChild.addNode(ModuleOutput, 100, 60);
    packedChild.setModulePortType(floatPort, Type::Mono4);
    packedChild.setModulePortType(doublePort, Type::Mono4Double);
    packedChild.connect({floatPort, floatOut, 0});
    packedChild.connect({doublePort, doubleOut, 0});

    const auto packedSource = addCode("streamout x; x = 0; x{1} = 19;");
    const auto packedTap = addCode("streamin x; monoin single; streamout y; monoout z; y = x(1); z = single(1);");
    const auto packedDoubleTap = addCode("streamdoublein x; streamdoubleout y; y = x(1);");
    graph.disconnectInput(doublePoly, 0);
    session.connect({packedSource, packed, 0});
    session.connect({doubleSource, packed, 1});
    session.connect({packed, packedTap, 0});
    session.connect({typed, packedTap, 1});
    session.connect({packed, packedDoubleTap, 0, 1});
    check(graph.outputType(packedSource) == Type::Mono4 && graph.outputType(packedTap) == Type::Mono4 &&
          graph.outputType(doubleSource) == Type::Mono4Double && graph.outputType(packedDoubleTap) == Type::Mono4Double,
          "Both template formats should resolve to the four-lane type through a module");
    const auto packedCapture = graph.captureDsp(packedTap, 5, 3);
    check(packedCapture[0] == std::vector<double>(5, 19) && packedCapture[1] == std::vector<double>(5, 0),
          "Packed inputs should carry all four lanes while scalar connections carry only lane zero");
    check(graph.captureDsp(packedDoubleTap, 5, 3).front() == std::vector<double>(5, 1.0 + 0x1p-40),
          "A packed double lane should keep its precision through generic links");
}

void monoPolyBridgesExposeOptionalVoiceIndex()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    openstone::Session session;
    auto& graph = session.graph();
    const auto addCode = [&](std::string text) {
        const auto id = session.addNode(DspCode, 0, 0);
        graph.setDspDraft(id, std::move(text));
        session.compileDsp(id);
        return id;
    };
    const auto source = addCode("monoout signal; float phase; signal = phase; phase += 1;");
    const auto toPoly = session.addNode(MonoToPoly, 0, 0);
    const auto toMono = session.addNode(PolyToMono, 0, 0);
    const auto sink = addCode("monoin signal; monoout result; result = signal;");
    const auto index = graph.addNode(Int, 0, 0, std::int32_t{0});
    const auto pulse = session.addNode(Trigger, 0, 0);
    check(graph.inputType(toPoly, 0) == Type::Mono && graph.outputType(toPoly) == Type::Poly &&
          graph.inputType(toMono, 0) == Type::Poly && graph.outputType(toMono) == Type::Mono &&
          graph.inputType(toPoly, 1) == Type::Int && graph.inputType(toMono, 1) == Type::Int,
          "Both converters should expose typed signal ports and an Int Index");
    session.connect({source, toPoly, 0});
    session.connect({toPoly, toMono, 0});
    session.connect({toMono, sink, 0});
    session.connect({pulse, toMono, 1});
    check(!graph.inspectInput(toMono, 1).value && !graph.inspectInput(toPoly, 1).value,
          "An unwired Index or a trigger-only link should leave selection optional");
    const std::vector<double> ramp{0, 1, 2, 3, 4};
    const std::vector<double> silence(ramp.size());
    const auto capture = [&] { return graph.captureDsp(sink, ramp.size(), 3).front(); };
    check(capture() == ramp && std::ranges::equal(graph.dspTrace(toPoly, 0, true), ramp) &&
          std::ranges::equal(graph.dspTrace(toMono, 0, true), ramp),
          "Capture should render both conversions and publish their swatches");

    session.connect({index, toMono, 1});
    check(graph.inspectInput(toMono, 1).value == openstone::green::Value{std::int32_t{0}} && capture() == ramp,
          "Slot zero should be an explicit selection");
    session.setValue(index, std::int32_t{1});
    check(capture() == silence && graph.dspTrace(source, 0, true).empty(),
          "Selecting a slot outside the preview should return silence and leave upstream DSP idle");
    session.disconnectInput(toMono, 1);
    session.connect({index, toPoly, 1});
    check(capture() == silence, "Mono to Poly should also honor the Index in a capture");
    session.setValue(index, std::int32_t{0});
    check(capture() == ramp, "Capturing again should use the current Index");

    const int module = session.makeModule(std::array{toPoly});
    check(module >= 0 && capture() == ramp, "Converter links should pass through generated module ports");
}

void signalCapturePrimitivesPublishOnTrigger()
{
    using enum PrimitiveKind;
    using openstone::green::FloatArray;
    using openstone::green::Type;
    openstone::Session session;
    auto& graph = session.graph();
    const auto addCode = [&](std::string text) {
        const auto id = session.addNode(DspCode, 0, 0);
        graph.setDspDraft(id, std::move(text));
        session.compileDsp(id);
        return id;
    };
    const auto readSamples = [&](int node, int output = 0) {
        return std::get<FloatArray>(graph.readValue(node, output));
    };
    const auto source = addCode(R"(
        monoout a;
        monoout b;
        float phase;
        a = phase;
        b = 100 + phase;
        phase += 1;
    )");
    const auto capture = session.addNode(MonoToGraph2, 0, 0);
    const auto ns = graph.addNode(Int, 0, 0, std::int32_t{259});
    const auto get = session.addNode(Trigger, 0, 0);
    const auto count = session.addNode(TriggerCounter, 0, 0);
    session.connect({source, capture, 0});
    session.connect({source, capture, 1, 1});
    session.connect({ns, capture, 2});
    graph.connect({capture, count, 0}, LinkNotification::Silent);
    check(graph.outputType(capture, 1) == Type::FloatArray && readSamples(capture).empty() &&
          graph.dspTrace(source, 0, true).empty(), "Connecting signals and NS should leave the capture idle");
    graph.connect({get, capture, 3}, LinkNotification::Silent);
    graph.fireTrigger(get);
    const auto a = readSamples(capture);
    const auto b = readSamples(capture, 1);
    check(a.size() == 259 && b.size() == a.size() && integer(graph, count) == 1,
          "A Get pulse should publish both Float Arrays");
    for (std::size_t i = 0; i < a.size(); ++i) {
        check(a[i] == i && b[i] == i + 100, "Mono taps should capture the same sample indices across a block boundary");
    }
    check(graph.dspTrace(capture, 0, false).back() == 258, "Capturing should publish the upstream waveform");
    session.setValue(ns, std::int32_t{0});
    check(readSamples(capture) == a, "Changing NS should keep the last successful capture");
    bool rejected = false;
    try {
        graph.fireTrigger(capture);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected && readSamples(capture) == a && integer(graph, count) == 1,
          "An invalid length should preserve the previous snapshot and emit no output pulse");
    session.setValue(ns, std::int32_t{5});
    graph.fireTrigger(capture);
    check(readSamples(capture) == FloatArray{0, 1, 2, 3, 4} && integer(graph, count) == 2,
          "Manual Get should also work and start a new offline render");
    session.disconnectInput(capture, 1);
    graph.fireTrigger(get);
    check(readSamples(capture, 1) == FloatArray(5), "An empty Mono input should give one channel of silence");

    check(openstone::primitive(MonoToGraph).outputs.size() == 1 &&
          openstone::primitive(MonoToGraph4).outputs.size() == 4 &&
          openstone::primitive(MonoToGraph8).outputs.size() == 8,
          "The library should also provide single, four, and eight-channel versions");
    const auto single = session.addNode(MonoToGraph, 0, 0);
    graph.fireTrigger(single);
    check(readSamples(single) == FloatArray(512), "NS should default to 512 when unwired");

    const auto poly = addCode("polyout wave; float phase; wave = phase * 0.25; phase += 1;");
    const auto analyser = session.addNode(SignalAnalyser, 0, 0);
    session.connect({poly, analyser, 0});
    session.connect({ns, analyser, 1});
    graph.connect({get, analyser, 2}, LinkNotification::Silent);
    graph.fireTrigger(get);
    const auto code = std::get<std::string>(graph.readValue(analyser, 1));
    const auto timings = readSamples(analyser, 2);
    check(readSamples(analyser) == FloatArray{0, 0.25f, 0.5f, 0.75f, 1.0f} && timings.size() == 5 &&
          std::ranges::all_of(timings, [](float elapsed) { return std::isfinite(elapsed) && elapsed >= 0; }),
          "Recalc should publish preview voice samples and one measured duration per sample");
    check(openstone::stream::defaultDspBackend() == openstone::stream::DspBackend::LlvmOrc
              ? code.contains("define") && code.contains("openstone_dsp_stage2")
              : code.contains("interpreter"),
          "Analyser Code should identify the runtime and expose the active LLVM kernel");
    check(graph.inspectOutput(analyser, 0).triggers == 1 && graph.inspectOutput(analyser, 1).triggers == 1 &&
          graph.inspectOutput(analyser, 2).triggers == 1, "Recalc should pulse all Analyser outputs once");
}

void valuePatch()
{
    Schematic schematic;
    const int text = schematic.addNode(PrimitiveKind::String, 0, 0, std::string("12 3"));
    const int number = schematic.addNode(PrimitiveKind::Int, 0, 100, std::int32_t{3});
    const int add = schematic.addNode(PrimitiveKind::Add, 200, 0);
    const int display = schematic.addNode(PrimitiveKind::Int, 400, 0);
    const int counter = schematic.addNode(PrimitiveKind::TriggerCounter, 400, 100);

    check(schematic.triggers() == 0, "Initial values should not emit triggers");
    schematic.setValue(display, std::int32_t{7});
    schematic.connect({text, add, 0}, LinkNotification::Silent);
    schematic.connect({number, add, 0}, LinkNotification::Silent);
    schematic.connect({add, display, 0}, LinkNotification::Silent);
    schematic.connect({display, counter, 0}, LinkNotification::Silent);
    check(integer(schematic, display) == 15 && !schematic.editable(display),
          "A connected Int display should read converted fan-in");

    schematic.setValue(text, std::string("11"));
    schematic.setValue(text, std::string("11"));
    check(integer(schematic, display) == 14 && integer(schematic, counter) == 1,
          "An edited source should update the sum and deliver one pulse");

    bool rejected = false;
    try {
        schematic.connect({display, add, 1});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected && integer(schematic, display) == 14, "Feedback should leave the patch intact");

    schematic.removeNode(text);
    check(integer(schematic, display) == 3 && integer(schematic, counter) == 1,
          "Removing a source should preserve the rest of the patch and counter state");
    schematic.disconnectInput(display, 0);
    check(integer(schematic, display) == 7 && schematic.editable(display),
          "Disconnecting an input should restore its local value");
}

void triggerPatch()
{
    Schematic schematic;
    const int trigger = schematic.addNode(PrimitiveKind::Trigger, 0, 0);
    const int counter = schematic.addNode(PrimitiveKind::TriggerCounter, 200, 0);
    schematic.connect({trigger, counter, 0});
    check(integer(schematic, counter) == 1, "Connecting a Trigger should deliver its first pulse");
    schematic.fireTrigger(trigger);
    schematic.fireTrigger(trigger);
    check(integer(schematic, counter) == 3, "Both manual triggers should reach the counter");
}

void connectingIntNodesDeliversOnlyOnTheNewLink()
{
    Schematic schematic;
    const int source = schematic.addNode(PrimitiveKind::Int, 0, 0);
    const int target = schematic.addNode(PrimitiveKind::Int, 200, 0);
    const int other = schematic.addNode(PrimitiveKind::Int, 200, 80);
    const int counter = schematic.addNode(PrimitiveKind::TriggerCounter, 400, 0);
    const SchematicLink newLink{source, target, 0};
    const SchematicLink existingLink{source, other, 0};
    schematic.connect(existingLink, LinkNotification::Silent);
    schematic.connect({target, counter, 0}, LinkNotification::Silent);

    schematic.connect(newLink);
    check(integer(schematic, target) == 0 && schematic.inspectLink(newLink).triggers == 1 &&
          schematic.inspectInput(target, 0).triggers == 1 && schematic.inspectOutput(target).triggers == 1 &&
          integer(schematic, counter) == 1,
          "Connecting two Int boxes with the same value should deliver and propagate one trigger");
    check(schematic.inspectLink(existingLink).triggers == 0 && schematic.inspectOutput(source).triggers == 0,
          "A new link should not fire the source or its other links");

    bool rejected = false;
    try {
        schematic.connect(newLink);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected && schematic.inspectLink(newLink).triggers == 1 && integer(schematic, counter) == 1,
          "A rejected link should not deliver another trigger");

    const int secondSource = schematic.addNode(PrimitiveKind::Int, 0, 160, std::int32_t{7});
    schematic.connect({secondSource, target, 0});
    check(integer(schematic, target) == 7 && schematic.inspectInput(target, 0).triggers == 2 &&
          integer(schematic, counter) == 2 && schematic.inspectLink(newLink).triggers == 1,
          "A second source should update the input and propagate once without retriggering the first link");
}

void inspectSignals()
{
    using openstone::green::Value;

    Schematic schematic;
    const int text = schematic.addNode(PrimitiveKind::String, 0, 0, std::string("12 3"));
    const int number = schematic.addNode(PrimitiveKind::Int, 0, 80, std::int32_t{3});
    const int add = schematic.addNode(PrimitiveKind::Add, 200, 0);
    schematic.setTemplateType(add, openstone::green::Type::Int);
    const int trigger = schematic.addNode(PrimitiveKind::Trigger, 0, 160);
    const SchematicLink converted{text, add, 0};
    const SchematicLink pulse{trigger, add, 0};

    check(schematic.inspectInput(number, 0).value == Value{std::int32_t{3}},
          "Unconnected input should report the node's local value");
    schematic.connect(converted, LinkNotification::Silent);
    schematic.connect({number, add, 0}, LinkNotification::Silent);
    schematic.connect(pulse, LinkNotification::Silent);
    check(schematic.inspectOutput(text).value == Value{std::string("12 3")} &&
          schematic.inspectLink(converted).value == Value{std::int32_t{12}} &&
          schematic.inspectInput(add, 0).value == Value{std::int32_t{15}},
          "Readout should distinguish the source, converted link, and combined input");

    schematic.setValue(text, std::string("7"));
    schematic.fireTrigger(trigger);
    const auto wire = schematic.inspectLink(pulse);
    const auto input = schematic.inspectInput(add, 0);
    check(!wire.value && wire.triggers == 1 && !schematic.inspectOutput(trigger).value &&
          schematic.inspectLink(converted).triggers == 1 && input.triggers == 2 &&
          input.value == Value{std::int32_t{10}} && schematic.inspectOutput(add).triggers == 2 &&
          schematic.inspectInput(add, 1).triggers == 0,
          "Pulses should count per link and port without contributing a value");

    schematic.removeNode(number);
    check(schematic.inspectLink(converted).triggers == 1 && schematic.inspectInput(add, 0).triggers == 2,
          "Removing a different link should preserve inspection counts");
    schematic.disconnectInput(add, 0);
    schematic.connect(converted);
    check(schematic.inspectLink(converted).triggers == 1 && schematic.inspectInput(add, 0).triggers == 3,
          "A reconnected link should count only its new pulse");
}

void arithmeticPrimitives()
{
    using openstone::green::Type;
    using enum PrimitiveKind;
    Schematic schematic;
    const int left = schematic.addNode(Int, 0, 0, std::int32_t{10});
    const int right = schematic.addNode(Int, 0, 80, std::int32_t{3});
    const auto connectOperator = [&](PrimitiveKind kind) {
        const int id = schematic.addNode(kind, 200, 0);
        check(!schematic.outputType(id), "A new operator should have template connectors");
        schematic.connect({left, id, 0});
        schematic.connect({right, id, 1});
        return id;
    };

    check(integer(schematic, connectOperator(Subtract)) == 7 &&
          integer(schematic, connectOperator(Multiply)) == 30,
          "Generic operators should infer Int and preserve operand order");
    const int divide = connectOperator(Divide);
    check(integer(schematic, divide) == 3, "The Int quotient should truncate");

    schematic.setValue(right, std::int32_t{0});
    check(integer(schematic, divide) == 0, "Integer division by zero should return zero");
    schematic.setValue(right, std::int32_t{-1});
    schematic.setValue(left, std::numeric_limits<std::int32_t>::min());
    check(integer(schematic, divide) == std::numeric_limits<std::int32_t>::min(),
          "Int division overflow should wrap without a crash");

    schematic.setValue(left, std::int32_t{10});
    schematic.setTemplateType(divide, Type::Float);
    schematic.setValue(right, std::int32_t{3});
    check(schematic.inputType(divide, 0) == Type::Float &&
          std::get<float>(schematic.readValue(divide)) == 10.0F / 3.0F,
          "Pinning the type should convert the same node and its existing links");
    schematic.setTemplateType(divide, std::nullopt);
    const int fractional = schematic.addNode(Float, 0, 100, 2.5F);
    schematic.disconnectInput(divide, 1);
    schematic.connect({fractional, divide, 1});
    check(schematic.inputType(divide, 0) == Type::Int && schematic.inputType(divide, 1) == Type::Float &&
          std::get<float>(schematic.readValue(divide)) == 4.0F,
          "An Int and a Float should retain their connector types and yield a Float");
    schematic.setValue(fractional, 0.0F);
    check(std::get<float>(schematic.readValue(divide)) == std::numeric_limits<float>::infinity(),
          "Float division by zero should return positive infinity");

    const int textA = schematic.addNode(String, 0, 0, std::string("Open"));
    const int textB = schematic.addNode(String, 0, 80, std::string("Stone"));
    const int addText = schematic.addNode(Add, 200, 0);
    schematic.connect({textA, addText, 0});
    schematic.connect({textB, addText, 1});
    check(std::get<std::string>(schematic.readValue(addText)) == "OpenStone",
          "The generic Add node should concatenate Strings in input order");

    const int array = schematic.addNode(TriggerBlocker, 100, 0);
    schematic.setTemplateType(array, Type::IntArray);
    schematic.setValue(textA, std::string("1\n2\n3"));
    schematic.connect({textA, array, 0});
    const int multiplyArray = schematic.addNode(Multiply, 200, 0);
    schematic.connect({array, multiplyArray, 0});
    schematic.connect({right, multiplyArray, 1});
    check(schematic.outputType(multiplyArray) == Type::IntArray &&
          std::get<openstone::green::IntArray>(schematic.readValue(multiplyArray)) ==
              openstone::green::IntArray{3, 6, 9},
          "A generic operator should broadcast a scalar over an array");
    bool rejected = false;
    try {
        schematic.setTemplateType(multiplyArray, Type::Float);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected && schematic.outputType(multiplyArray) == Type::IntArray &&
          schematic.inspectInput(multiplyArray, 0).triggers == 1,
          "An incompatible type change should preserve existing wire types and counters");
}

void triggerTools()
{
    using enum PrimitiveKind;
    Schematic schematic;
    const int source = schematic.addNode(Float, 0, 0, 440.0F);
    const int blocker = schematic.addNode(TriggerBlocker, 200, 0);
    const int counter = schematic.addNode(TriggerCounter, 400, 0);
    schematic.connect({blocker, counter, 0}, LinkNotification::Silent);
    schematic.connect({source, blocker, 0});
    check(std::get<float>(schematic.readValue(blocker)) == 440.0F && integer(schematic, counter) == 0,
          "A new input link should seed the blocker without pulsing downstream");
    schematic.setValue(source, 880.0F);
    check(std::get<float>(schematic.readValue(blocker)) == 880.0F &&
          schematic.inspectInput(blocker, 0).triggers == 2 &&
          schematic.inspectOutput(blocker).triggers == 0 && integer(schematic, counter) == 0,
          "Trigger Blocker should expose the latest value without forwarding either pulse");

    const int secondBlocker = schematic.addNode(TriggerBlocker, 300, 40);
    schematic.connect({blocker, secondBlocker, 0}, LinkNotification::Silent);
    schematic.setValue(source, 220.0F);
    check(std::get<float>(schematic.readValue(secondBlocker)) == 220.0F &&
          schematic.inspectInput(secondBlocker, 0).triggers == 0,
          "Chained Trigger Blockers pull current values even when no events reach the second blocker");
    schematic.setValue(source, 880.0F);

    const int pulse = schematic.addNode(Trigger, 0, 120);
    const int open = schematic.addNode(Boolean, 0, 200);
    const int gate = schematic.addNode(TriggerSwitch, 200, 120);
    const int gateCounter = schematic.addNode(TriggerCounter, 400, 120);
    schematic.connect({gate, gateCounter, 0}, LinkNotification::Silent);
    schematic.connect({pulse, gate, 0});
    schematic.connect({open, gate, 1});
    schematic.fireTrigger(pulse);
    check(integer(schematic, gateCounter) == 0, "A closed Trigger Switch should suppress incoming pulses");
    schematic.setValue(open, true);
    check(integer(schematic, gateCounter) == 0, "Opening the switch should not send a pulse");
    schematic.fireTrigger(pulse);
    check(integer(schematic, gateCounter) == 1, "An open switch should forward one pulse");
    schematic.setValue(open, false);
    schematic.fireTrigger(pulse);
    check(integer(schematic, gateCounter) == 1, "Closing the switch should suppress later pulses");

    // A Trigger output can notify an Int node without taking over its stored value.
    const int valueBox = schematic.addNode(Int, 400, 240, std::int32_t{42});
    schematic.connect({gate, valueBox, 0});
    check(schematic.editable(valueBox) && integer(schematic, valueBox) == 42,
          "A switch's trigger-only output should preserve the Int's local value");

    const int valueGate = schematic.addNode(TriggerSwitch, 200, 320);
    const int valueCounter = schematic.addNode(TriggerCounter, 400, 320);
    schematic.connect({valueGate, valueCounter, 0}, LinkNotification::Silent);
    schematic.connect({source, valueGate, 0});
    schematic.connect({open, valueGate, 1});
    check(schematic.outputType(valueGate) == openstone::green::Type::Float &&
          std::get<float>(schematic.readValue(valueGate)) == 880.0F && integer(schematic, valueCounter) == 0,
          "A generic switch should expose the value while blocking its pulse");
    schematic.setValue(open, true);
    schematic.setValue(source, 660.0F);
    check(std::get<float>(schematic.readValue(valueGate)) == 660.0F && integer(schematic, valueCounter) == 1,
          "An open typed switch should pass the value source's pulse");

    Schematic counters;
    const int increment = counters.addNode(Trigger, 0, 0);
    const int basic = counters.addNode(BasicCounter, 0, 0, std::numeric_limits<std::int32_t>::max());
    const int diagnostic = counters.addNode(TriggerCounter, 0, 0, std::numeric_limits<std::int32_t>::max());
    counters.connect({increment, basic, 0}, LinkNotification::Silent);
    counters.connect({increment, diagnostic, 0}, LinkNotification::Silent);
    counters.fireTrigger(increment);
    check(integer(counters, basic) == 0 && integer(counters, diagnostic) == std::numeric_limits<std::int32_t>::min(),
          "Counter resets a negative wrapped result to zero while the diagnostic Trigger Counter retains signed wrapping");
}

void greenMathAndComparisons()
{
    using namespace openstone::green;
    using enum PrimitiveKind;
    Schematic schematic;
    const int left = schematic.addNode(Int, 0, 0, std::int32_t{-10});
    const int right = schematic.addNode(Int, 0, 80, std::int32_t{3});

    const int remainder = schematic.addNode(Modulus, 180, 0);
    schematic.connect({left, remainder, 0});
    schematic.connect({right, remainder, 1});
    check(integer(schematic, remainder) == -1, "Int modulus should keep the dividend's sign");

    const int abs = schematic.addNode(Abs, 280, 0);
    schematic.connect({remainder, abs, 0});
    check(integer(schematic, abs) == 1, "Abs should infer Int from the remainder");
    schematic.setValue(left, std::numeric_limits<std::int32_t>::min());
    schematic.setValue(right, std::int32_t{-1});
    check(integer(schematic, remainder) == 0, "Int modulus must handle INT32_MIN and -1");
    schematic.setValue(right, std::int32_t{0});
    check(integer(schematic, remainder) == 0, "Int modulus by zero should return zero");
    schematic.setTemplateType(remainder, Type::Float);
    check(std::isnan(std::get<float>(schematic.readValue(remainder))),
          "Float modulus by zero should return NaN");

    schematic.setValue(left, std::int32_t{-10});
    schematic.setValue(right, std::int32_t{3});
    check(std::get<float>(schematic.readValue(remainder)) == -1.0F,
          "Float modulus should return the truncated remainder");

    const int point = schematic.addNode(PrimitiveKind::Point, 180, 80);
    const int fraction = schematic.addNode(Float, 0, 160, 2.5F);
    schematic.connect({left, point, 0});
    schematic.connect({fraction, point, 1});
    schematic.disconnectInput(abs, 0);
    schematic.connect({point, abs, 0});
    check(schematic.readValue(point) == Value{openstone::green::Point{-10, 2.5F}} &&
          schematic.readValue(abs) == Value{openstone::green::Point{10, 2.5F}},
          "Point should assemble Float coordinates and Abs should transform both");
    const int text = schematic.addNode(String, 0, 240, std::string("-1, 2\n3, -4"));
    schematic.disconnectInput(abs, 0);
    schematic.setTemplateType(abs, Type::PointArray);
    schematic.connect({text, abs, 0});
    check(schematic.readValue(abs) == Value{PointArray{{1, 2}, {3, 4}}},
          "A pinned Point Array Abs should transform every element after conversion");

    const int less = schematic.addNode(LessThan, 180, 200);
    const int greater = schematic.addNode(GreaterThan, 180, 280);
    const int counter = schematic.addNode(TriggerCounter, 400, 200);
    check(schematic.outputType(less) == Type::Boolean && !schematic.inputType(less, 0) &&
          !std::get<bool>(schematic.readValue(less)), "A fresh comparison should have a Boolean output");
    schematic.connect({less, counter, 0}, LinkNotification::Silent);
    for (const int comparison : {less, greater}) {
        schematic.connect({left, comparison, 0});
        schematic.connect({fraction, comparison, 1});
    }
    check(std::get<bool>(schematic.readValue(less)) && !std::get<bool>(schematic.readValue(greater)) &&
          integer(schematic, counter) == 2, "Both mixed numeric input links should propagate pulses");
    schematic.setTemplateType(less, Type::Int);
    schematic.setValue(left, std::int32_t{2});
    check(!std::get<bool>(schematic.readValue(less)) && schematic.outputType(less) == Type::Boolean,
          "The operand override should convert the Float to Int and keep the Boolean output");
    schematic.setTemplateType(less, std::nullopt);
    check(std::get<bool>(schematic.readValue(less)), "Template should restore mixed numeric comparison");
    schematic.setValue(left, std::int32_t{5});
    check(!std::get<bool>(schematic.readValue(less)) && std::get<bool>(schematic.readValue(greater)) &&
          integer(schematic, counter) == 4, "Editing either operand should update the result and deliver a pulse");

    const int stringLess = schematic.addNode(LessThan, 180, 360);
    const int firstText = schematic.addNode(String, 0, 400, std::string("10"));
    const int otherText = schematic.addNode(String, 0, 320, std::string("2"));
    schematic.connect({firstText, stringLess, 0});
    schematic.connect({otherText, stringLess, 1});
    check(std::get<bool>(schematic.readValue(stringLess)), "String comparison should compare text");
}

void genericComparisonsPublishBooleanResultsAndTriggers()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    Schematic graph;
    const int equal = graph.addNode(Equal, 180, 0);
    const int lessEqual = graph.addNode(LessThanOrEqual, 180, 80);
    const int greaterEqual = graph.addNode(GreaterThanOrEqual, 180, 160);
    const int left = graph.addNode(Int, 0, 0, std::int32_t{2});
    const int right = graph.addNode(Float, 0, 80, 2.0F);
    const int counter = graph.addNode(TriggerCounter, 360, 0);
    const auto result = [&](int id) { return std::get<bool>(graph.readValue(id)); };

    for (const int id : {equal, lessEqual, greaterEqual}) {
        check(!graph.inputType(id, 0) && !graph.inputType(id, 1) &&
              graph.outputType(id) == Type::Boolean && !result(id), "Unwired comparisons have Template inputs");

        graph.connect({left, id, 0});
        graph.connect({right, id, 1});
        check(graph.inputType(id, 0) == Type::Int && graph.inputType(id, 1) == Type::Float && result(id),
              "Mixed Green numeric comparison is inclusive at equality");
    }
    graph.connect({equal, counter, 0}, LinkNotification::Silent);
    graph.setValue(right, 2.75F);
    check(!result(equal) && result(lessEqual) && !result(greaterEqual), "The left operand is smaller");
    graph.setValue(left, std::int32_t{3});
    check(!result(equal) && !result(lessEqual) && result(greaterEqual) && integer(graph, counter) == 2,
          "Both operands deliver triggers, even for two consecutive False results");

    graph.setValue(left, std::int32_t{2});
    graph.setTemplateType(equal, Type::Int);
    check(graph.inputType(equal, 1) == Type::Int && result(equal), "Manual Int type converts Float before comparing");
    graph.setTemplateType(equal, std::nullopt);
    check(!result(equal), "Restoring Automatic uses the original Float value");

    graph.setValue(left, std::int32_t{16'777'217});
    graph.setValue(right, 16'777'216.0F);
    check(!result(equal) && !result(lessEqual) && result(greaterEqual),
          "Mixed numeric comparison preserves an Int beyond Float's exact range");
    graph.setValue(right, std::numeric_limits<float>::quiet_NaN());
    check(!result(equal) && !result(lessEqual) && !result(greaterEqual), "NaN is unordered");

    const int a = graph.addNode(String, 0, 160, std::string{"OpenStone"});
    const int b = graph.addNode(String, 0, 240, std::string{"OpenStone"});
    for (const int id : {equal, lessEqual, greaterEqual}) {
        graph.disconnectNode(id);
        graph.connect({a, id, 0});
        graph.connect({b, id, 1});
    }
    check(result(equal) && result(lessEqual) && result(greaterEqual), "Identical strings compare equal");
    graph.setValue(b, std::string{"openstone"});
    check(!result(equal) && result(lessEqual) && !result(greaterEqual), "String comparison is case sensitive");
}

void genericComparisonsOrderArrayContents()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    Schematic graph;
    const int textA = graph.addNode(MultilineString, 0, 0, std::string{"1\n4"});
    const int textB = graph.addNode(MultilineString, 0, 150, std::string{"1\n4"});
    const int a = graph.addNode(Array, 200, 0);
    const int b = graph.addNode(Array, 200, 230);
    const int equal = graph.addNode(Equal, 400, 0);
    const int lessEqual = graph.addNode(LessThanOrEqual, 400, 80);
    const int greaterEqual = graph.addNode(GreaterThanOrEqual, 400, 160);
    const int less = graph.addNode(LessThan, 400, 240);
    const int greater = graph.addNode(GreaterThan, 400, 320);
    const auto result = [&](int id) { return std::get<bool>(graph.readValue(id)); };
    graph.setTemplateType(a, Type::IntArray);
    graph.setTemplateType(b, Type::IntArray);
    graph.connect({textA, a, 4});
    graph.connect({textB, b, 4});
    for (const int id : {equal, lessEqual, greaterEqual, less, greater}) {
        graph.connect({a, id, 0});
        graph.connect({b, id, 1});
        check(graph.inputType(id, 0) == Type::IntArray && graph.outputType(id) == Type::Boolean,
              "Comparisons infer the linked array type");
    }
    check(result(equal) && result(lessEqual) && result(greaterEqual) && !result(less) && !result(greater),
          "Array equality requires the same elements in the same order");
    graph.setValue(textB, std::string{"1\n4\n0"});
    check(!result(equal) && result(lessEqual) && !result(greaterEqual) && result(less),
          "An equal prefix is ordered by length, including when the extra element is zero");
    graph.setValue(textB, std::string{"0\n99"});
    check(!result(equal) && !result(lessEqual) && result(greaterEqual) && result(greater),
          "The first differing element determines array order");

    graph.setValue(textA, std::string{"16777217"});
    graph.setTemplateType(b, Type::FloatArray);
    graph.setValue(textB, std::string{"16777216"});
    check(!result(equal) && !result(lessEqual) && result(greaterEqual),
          "Mixed numeric arrays compare without narrowing the Int elements");

    const int stringEqual = graph.addNode(Equal, 400, 400);
    graph.setTemplateType(stringEqual, Type::StringArray);
    graph.connect({textA, stringEqual, 0});
    graph.connect({textB, stringEqual, 1});
    graph.setValue(textA, std::string{"cat\ndog"});
    graph.setValue(textB, std::string{"cat\ndog"});
    check(result(stringEqual), "Matching String Arrays compare equal after connector conversion");
    graph.setValue(textB, std::string{"dog\ncat"});
    check(!result(stringEqual), "Changing the order of String Array elements breaks equality");

    const int scalar = graph.addNode(Int, 0, 300);
    const int incompatible = graph.addNode(Equal, 400, 480);
    graph.connect({a, incompatible, 0});
    const auto previousLinks = graph.links().size();
    bool rejected = false;
    try {
        graph.connect({scalar, incompatible, 1});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected && graph.links().size() == previousLinks && !result(incompatible),
          "Incompatible scalar and array operands are rejected before committing a link");
}

void genericTriggerComparisonsGateEveryIncomingPulse()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    Schematic graph;
    const int left = graph.addNode(Int, 0, 0, std::int32_t{-3});
    const int right = graph.addNode(Float, 0, 80, 2.0F);
    const int triggerA = graph.addNode(Trigger, 0, 160);
    const int triggerB = graph.addNode(Trigger, 0, 240);
    static constexpr std::array kinds{EqualTrigger, LessThanTrigger, LessThanOrEqualTrigger,
                                      GreaterThanTrigger, GreaterThanOrEqualTrigger};
    std::array<int, kinds.size()> gates;
    std::array<int, kinds.size()> counters;
    for (std::size_t i = 0; i < kinds.size(); ++i) {
        gates[i] = graph.addNode(kinds[i], 200, 80.0 * i);
        counters[i] = graph.addNode(TriggerCounter, 400, 80.0 * i);
        check(!graph.inputType(gates[i], 0) && !graph.inputType(gates[i], 1) &&
              graph.outputType(gates[i]) == Type::Trigger && !graph.inspectOutput(gates[i]).value,
              "Each gate has two Template inputs and one event-only output");
        graph.connect({gates[i], counters[i], 0}, LinkNotification::Silent);
        graph.connect({triggerA, gates[i], 0}, LinkNotification::Silent);
        graph.connect({triggerB, gates[i], 1}, LinkNotification::Silent);
    }

    const auto checkPulse = [&](const auto& action, const std::array<bool, kinds.size()>& passes) {
        std::array<std::int32_t, kinds.size()> before;
        for (std::size_t i = 0; i < kinds.size(); ++i) before[i] = integer(graph, counters[i]);
        action();
        for (std::size_t i = 0; i < kinds.size(); ++i) {
            check(integer(graph, counters[i]) == before[i] + passes[i],
                  "A matching input event produces exactly one output pulse");
        }
    };
    constexpr std::array blocked{false, false, false, false, false};
    constexpr std::array smaller{false, true, true, false, false};
    constexpr std::array same{true, false, true, false, true};
    constexpr std::array larger{false, false, false, true, true};
    checkPulse([&] { graph.fireTrigger(triggerA); }, blocked);

    for (const int gate : gates) {
        graph.connect({left, gate, 0}, LinkNotification::Silent);
        graph.connect({right, gate, 1}, LinkNotification::Silent);
        check(graph.inputType(gate, 0) == Type::Int && graph.inputType(gate, 1) == Type::Float,
              "Trigger versions infer mixed numeric operand types");
    }
    checkPulse([&] { graph.fireTrigger(triggerA); }, smaller);
    checkPulse([&] { graph.fireTrigger(triggerB); }, smaller);
    checkPulse([&] { graph.setValue(left, std::int32_t{2}); }, same);
    checkPulse([&] { graph.fireTrigger(triggerA); }, same);
    checkPulse([&] { graph.setValue(right, 1.5F); }, larger);
    checkPulse([&] { graph.fireTrigger(triggerB); }, larger);
    checkPulse([&] { graph.setValue(right, std::numeric_limits<float>::quiet_NaN()); }, blocked);
    checkPulse([&] { graph.fireTrigger(triggerA); }, blocked);
}

void genericTriggerComparisonsSampleTypedTextAndArrays()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    Schematic graph;
    const int equal = graph.addNode(EqualTrigger, 180, 0);
    const int left = graph.addNode(MultilineString, 0, 0, std::string{"cat\ndog"});
    const int right = graph.addNode(MultilineString, 0, 100, std::string{"cat\ndog"});
    const int counter = graph.addNode(TriggerCounter, 360, 0);
    graph.connect({equal, counter, 0}, LinkNotification::Silent);
    graph.setTemplateType(equal, Type::StringArray);
    graph.connect({left, equal, 0});
    check(integer(graph, counter) == 0, "The first connection compares with an empty array and is blocked");
    graph.connect({right, equal, 1});
    check(integer(graph, counter) == 1, "A new matching operand link delivers its connection trigger");
    graph.setValue(right, std::string{"dog\ncat"});
    check(integer(graph, counter) == 1, "Reordering an array blocks equality");
    graph.setValue(left, std::string{"dog\ncat"});
    check(integer(graph, counter) == 2, "Changing the other operand to a match passes its trigger");

    const int less = graph.addNode(LessThanTrigger, 180, 120);
    const int lexicalCounter = graph.addNode(TriggerCounter, 360, 120);
    graph.connect({less, lexicalCounter, 0}, LinkNotification::Silent);
    graph.connect({left, less, 0}, LinkNotification::Silent);
    graph.connect({right, less, 1}, LinkNotification::Silent);
    graph.setValue(left, std::string{"Dog\ncat"});
    check(graph.inputType(less, 0) == Type::String && integer(graph, lexicalCounter) == 1 &&
          integer(graph, counter) == 2, "Automatic String operands use the same case-sensitive ordering");
}

void greenLogicOperatorsWorkWithBooleansAndIntBits()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    Schematic graph;
    constexpr std::array kinds{And, Or, Xor};
    std::array<int, kinds.size()> nodes{};
    const int a = graph.addNode(Boolean, 0, 0, true);
    const int b = graph.addNode(Boolean, 0, 80, false);
    const int control = graph.addNode(Int, 0, 160, std::int32_t{6});
    const auto boolean = [&](int id) { return std::get<bool>(graph.readValue(id)); };

    for (std::size_t i = 0; i < kinds.size(); ++i) {
        const int id = graph.addNode(kinds[i], 180, 80.0 * i);
        nodes[i] = id;
        check(!graph.inputType(id, 0) && !graph.outputType(id), "Binary logic starts with Template connectors");
        graph.connect({a, id, 0});
        graph.connect({b, id, 1});
        check(graph.outputType(id) == Type::Boolean, "Boolean operands infer Boolean logic");
    }
    check(!boolean(nodes[0]) && boolean(nodes[1]) && boolean(nodes[2]), "Boolean operators follow their truth tables");
    graph.setValue(b, true);
    check(boolean(nodes[0]) && boolean(nodes[1]) && !boolean(nodes[2]), "XOr rejects two true inputs");

    const auto invert = graph.addNode(Not, 180, 240);
    graph.connect({b, invert, 0});
    check(!boolean(invert), "Not inverts its Boolean input");
    graph.setValue(b, false);
    check(boolean(invert), "Not follows value changes");

    const int other = graph.addNode(Int, 0, 240, std::int32_t{3});
    for (const int id : nodes) {
        graph.disconnectInput(id, 0);
        graph.disconnectInput(id, 1);
        graph.connect({control, id, 0});
        graph.connect({other, id, 1});
        check(graph.outputType(id) == Type::Int, "Int operands infer bitwise logic");
    }
    check(integer(graph, nodes[0]) == 2 && integer(graph, nodes[1]) == 7 && integer(graph, nodes[2]) == 5,
          "Int binary logic operates on the individual bits");
    graph.setTemplateType(nodes[0], Type::Boolean);
    check(boolean(nodes[0]), "A manual pin converts each Int to Boolean before applying And");

    const int intNot = graph.addNode(IntNot, 180, 320);
    const int shiftLeft = graph.addNode(ShiftLeft, 180, 400);
    const int shiftRight = graph.addNode(ShiftRight, 180, 480);
    graph.connect({control, intNot, 0});
    for (const int id : {shiftLeft, shiftRight}) {
        graph.connect({control, id, 0});
        graph.connect({other, id, 1});
    }
    graph.setValue(control, std::int32_t{-2});
    graph.setValue(other, std::int32_t{1});
    check(integer(graph, intNot) == 1 && integer(graph, shiftLeft) == -4 &&
          integer(graph, shiftRight) == std::numeric_limits<std::int32_t>::max(),
          "Right shift zero-fills and left shift discards bits past the 32-bit word");
    graph.setValue(other, std::int32_t{32});
    check(integer(graph, shiftLeft) == 0 && integer(graph, shiftRight) == 0, "Oversized shifts produce zero");
    graph.setValue(other, std::int32_t{-1});
    check(integer(graph, shiftLeft) == 0 && integer(graph, shiftRight) == 0, "Negative shift counts produce zero");
}

void greenLogicRoutesValuesAndEvaluatesConditionTables()
{
    using enum PrimitiveKind;
    Schematic graph;
    const int first = graph.addNode(Int, 0, 0, std::int32_t{10});
    const int second = graph.addNode(Int, 0, 80, std::int32_t{20});
    const int condition = graph.addNode(Boolean, 0, 160, false);
    const int select = graph.addNode(Select, 180, 0);
    const int last = graph.addNode(LastSwitch, 180, 100);
    for (const int id : {select, last}) {
        graph.connect({first, id, 0});
        graph.connect({second, id, 1});
    }
    graph.connect({condition, select, 2});
    const int conditionPulse = graph.addNode(Trigger, 0, 240);
    graph.connect({conditionPulse, select, 2}, LinkNotification::Silent);
    check(integer(graph, select) == 10 && integer(graph, last) == 20,
          "Select starts at the false branch and Last Switch follows the latest input pulse");
    const auto selectPulses = graph.inspectOutput(select).triggers;
    graph.setValue(second, std::int32_t{21});
    check(integer(graph, select) == 10 && graph.inspectOutput(select).triggers == selectPulses &&
          integer(graph, last) == 21, "Select suppresses events from its inactive input");
    graph.setValue(condition, true);
    const auto afterSelection = graph.inspectOutput(select).triggers;
    graph.fireTrigger(conditionPulse);
    check(graph.inspectOutput(select).triggers == afterSelection,
          "Select forwards a condition change but suppresses repeated identical condition notifications");
    graph.setValue(first, std::int32_t{11});
    check(integer(graph, select) == 21 && integer(graph, last) == 11, "The two routing primitives are independent");
    graph.disconnectInput(last, 0);
    check(integer(graph, last) == 21, "Last Switch falls back to the available input when its active link is removed");

    const int evaluate = graph.addNode(IfThenElse, 360, 0);
    const auto text = [&](PrimitiveKind kind, std::string value, int input) {
        const int id = graph.addNode(kind, 0, 80.0 * input, std::move(value));
        graph.connect({id, evaluate, input});
        return id;
    };
    const int mode = text(String, "int", 0);
    const int key = text(String, "10", 1);
    const int operation = text(String, ">", 2);
    const int conditions = text(MultilineString, "5\n2", 3);
    const int results = text(MultilineString, "first\nsecond", 4);
    text(String, "fallback", 5);
    const auto selected = [&] { return std::get<std::string>(graph.readValue(evaluate)); };
    check(selected() == "first", "An Int condition table returns the first matching row");
    graph.setValue(key, std::string{"3"});
    check(selected() == "second", "The table continues to the next condition");
    graph.setValue(results, std::string{"first"});
    check(selected() == "fallback", "A condition without a corresponding result is skipped");
    graph.setValue(mode, std::string{"float"});
    graph.setValue(key, std::string{"1.25"});
    graph.setValue(conditions, std::string{"1.2"});
    check(selected() == "first", "Floating-point conditions retain fractions");
    graph.setValue(mode, std::string{});
    graph.setValue(operation, std::string{});
    graph.setValue(key, std::string{"1.2"});
    check(selected() == "first", "The default condition compares strings for equality");
}

void indexSelectorPublishesSortedAndProgrammaticSelections()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    Schematic root;
    const int module = root.addNode(Module, 0, 0);
    auto& graph = *root.module(module);
    const int list = graph.addNode(MultilineString, 0, 0, std::string{" Zebra, Apricot\r\nMango\n"});
    const int sort = graph.addNode(Boolean, 0, 0, false);
    const int set = graph.addNode(String, 0, 0, std::string{});
    const int selector = graph.addNode(IndexSelector, 0, 0);
    const int indexCount = graph.addNode(TriggerCounter, 0, 0);
    const int textCount = graph.addNode(TriggerCounter, 0, 0);
    const auto text = [&] { return std::get<std::string>(graph.readValue(selector, 1)); };
    check(graph.inputType(selector, 0) == Type::String && graph.inputType(selector, 1) == Type::Boolean &&
          graph.inputType(selector, 2) == Type::String && graph.outputType(selector) == Type::Int &&
          graph.outputType(selector, 1) == Type::String, "Index Selector has the reference connector types");
    check(integer(graph, selector) == 0 && text().empty() && !graph.openIndexSelector(selector, {}),
          "An empty list defaults to zero and empty text and cannot open");
    graph.connect({list, selector, 0}, LinkNotification::Silent);
    graph.connect({sort, selector, 1}, LinkNotification::Silent);
    graph.connect({set, selector, 2}, LinkNotification::Silent);
    graph.connect({selector, indexCount, 0, 0}, LinkNotification::Silent);
    graph.connect({selector, textCount, 0, 1}, LinkNotification::Silent);
    check(integer(graph, selector) == 0 && text() == "Zebra",
          "The first comma-delimited option is selected before sorting");

    graph.setValue(set, std::string{"Mango"});
    check(integer(graph, selector) == 2 && text() == "Mango" &&
          integer(graph, indexCount) == 1 && integer(graph, textCount) == 1,
          "Set matches a newline-delimited option and publishes both outputs once");
    graph.setValue(set, std::string{"missing"});
    check(text() == "Mango" && integer(graph, indexCount) == 1 && integer(graph, textCount) == 1,
          "A missing option leaves the current selection and trigger counts alone");

    graph.setValue(set, std::string{"Mango"});
    graph.setValue(sort, true);
    check(integer(graph, selector) == 1 && text() == "Mango",
          "Set uses the zero-based index in the sorted list");
    check(graph.openIndexSelector(selector, {14, 20}) && root.pendingDropList()->checkedIndex == 1 &&
          root.pendingDropList()->contents.options == std::vector<std::string>{"Apricot", "Mango", "Zebra"},
          "The node menu exposes the sorted options and checks the active one");
    root.finishDropList(root.pendingDropList()->token, 2);
    check(integer(graph, selector) == 2 && text() == "Zebra" &&
          integer(graph, indexCount) == 3 && integer(graph, textCount) == 3,
          "Picking a row overrides the selection and publishes the matching index and text");
    check(graph.openIndexSelector(selector, {}), "Reopen the menu");
    root.finishDropList(root.pendingDropList()->token, std::nullopt);
    check(text() == "Zebra" && integer(graph, textCount) == 3, "Canceling changes nothing");

    check(graph.openIndexSelector(selector, {}), "Open the menu before changing the list");
    const auto stale = root.pendingDropList()->token;
    graph.setValue(list, std::string{"Only"});
    root.finishDropList(stale, 1);
    check(!root.pendingDropList() && integer(graph, selector) == 0 && text() == "Only" &&
          integer(graph, indexCount) == 4 && integer(graph, textCount) == 4,
          "A shorter list falls back to its first option and invalidates the old menu");
}

void pointToFloatSplitsCoordinatesAndForwardsPulses()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    using openstone::green::Value;
    Schematic graph;
    const int x = graph.addNode(Float, 0, 0, -3.25F);
    const int y = graph.addNode(Float, 0, 0, 18.5F);
    const int point = graph.addNode(Point, 0, 0);
    const int split = graph.addNode(PointToFloat, 0, 0);
    const int xCount = graph.addNode(TriggerCounter, 0, 0);
    const int yCount = graph.addNode(TriggerCounter, 0, 0);
    check(graph.inputType(split, 0) == Type::Point && graph.outputType(split, 0) == Type::Float &&
          graph.outputType(split, 1) == Type::Float && graph.readValue(split, 0) == Value{0.0F} &&
          graph.readValue(split, 1) == Value{0.0F}, "An unwired Point reads as zero on both Float outputs");
    graph.connect({x, point, 0}, LinkNotification::Silent);
    graph.connect({y, point, 1}, LinkNotification::Silent);
    graph.connect({split, xCount, 0, 0}, LinkNotification::Silent);
    graph.connect({split, yCount, 0, 1}, LinkNotification::Silent);
    graph.connect({point, split, 0});
    check(graph.readValue(split, 0) == Value{-3.25F} && graph.readValue(split, 1) == Value{18.5F} &&
          integer(graph, xCount) == 1 && integer(graph, yCount) == 1,
          "Link creation publishes the signed coordinates in X and Y order");
    graph.setValue(y, -4.125F);
    check(graph.readValue(split, 0) == Value{-3.25F} && graph.readValue(split, 1) == Value{-4.125F} &&
          integer(graph, xCount) == 2 && integer(graph, yCount) == 2,
          "A Point update forwards one pulse to each Float output");
}

void selectorRoutesTheIndexedInput()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    Schematic graph;
    const int index = graph.addNode(Int, 0, 0, std::int32_t{0});
    const int first = graph.addNode(Int, 0, 0, std::int32_t{10});
    const int second = graph.addNode(Float, 0, 0, 20.75F);
    const int third = graph.addNode(Int, 0, 0, std::int32_t{30});
    const int select = graph.addNode(Selector, 0, 0);

    graph.connect({index, select, 0});
    check(!graph.outputType(select) && findNode(graph, select).inputPorts().size() == 2,
          "The index leaves a single untyped spare data connector");
    graph.connect({first, select, 1});
    graph.connect({second, select, 2});
    graph.connect({third, select, 3});
    check(graph.outputType(select) == Type::Int && integer(graph, select) == 10 &&
          findNode(graph, select).inputPorts().size() == 5,
          "The Selector types its data sockets together and adds a spare after the third branch");

    const auto pulses = graph.inspectOutput(select).triggers;
    graph.setValue(second, 21.75F);
    check(graph.inspectOutput(select).triggers == pulses && integer(graph, select) == 10,
          "An inactive branch does not publish its changes");
    graph.setValue(index, std::int32_t{1});
    check(graph.inspectOutput(select).triggers == pulses + 1 && integer(graph, select) == 21,
          "Selecting a branch publishes its value with the inferred connector conversion");

    graph.disconnectInput(select, 2);
    check(integer(graph, select) == 0 && findNode(graph, select).inputPorts().size() == 5,
          "An unconnected intermediate branch reads zero without shifting other branches");
    graph.setValue(index, std::int32_t{2});
    check(integer(graph, select) == 30, "A later branch retains its index");
    graph.disconnectInput(select, 3);
    check(findNode(graph, select).inputPorts().size() == 3 && integer(graph, select) == 0,
          "Removing the highest branch trims the trailing spare and leaves an out-of-range selection at zero");
    graph.setValue(index, std::int32_t{std::numeric_limits<std::int32_t>::max()});
    check(integer(graph, select) == 0, "Even the largest index reads the typed default safely");
}

void multiplexerRoutesTheSelectedOutput()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    Schematic graph;
    const int index = graph.addNode(Int, 0, 0, std::int32_t{0});
    const int source = graph.addNode(String, 0, 0, std::string{"first"});
    const int route = graph.addNode(Multiplexer, 0, 0);
    const int first = graph.addNode(String, 0, 0);
    const int second = graph.addNode(String, 0, 0);
    const int third = graph.addNode(String, 0, 0);
    graph.connect({index, route, 0});
    graph.connect({source, route, 1});
    graph.connect({route, first, 0, 0});
    graph.connect({route, second, 0, 1});
    graph.connect({route, third, 0, 2});
    const auto read = [&](int id) { return std::get<std::string>(graph.readValue(id)); };
    check(findNode(graph, route).outputPorts().size() == 4 && graph.outputType(route, 3) == Type::String &&
          read(first) == "first" && read(second).empty() && read(third).empty(),
          "The third output grows a spare, and only the active output exposes the input value");

    const auto firstPulses = graph.inspectOutput(route, 0).triggers;
    const auto secondPulses = graph.inspectOutput(route, 1).triggers;
    const auto thirdPulses = graph.inspectOutput(route, 2).triggers;
    graph.setValue(index, std::int32_t{1});
    check(read(first).empty() && read(second) == "first" &&
          graph.inspectOutput(route, 0).triggers == firstPulses + 1 &&
          graph.inspectOutput(route, 1).triggers == secondPulses + 1 &&
          graph.inspectOutput(route, 2).triggers == thirdPulses,
          "Switching notifies the two outputs whose values are affected");
    graph.setValue(source, std::string{"next"});
    check(read(second) == "next" && graph.inspectOutput(route, 1).triggers == secondPulses + 2 &&
          graph.inspectOutput(route, 0).triggers == firstPulses + 1 &&
          graph.inspectOutput(route, 2).triggers == thirdPulses,
          "A data pulse reaches the selected branch alone");

    graph.setValue(index, std::int32_t{-1});
    graph.setValue(source, std::string{"parked"});
    check(read(first).empty() && read(second).empty() && read(third).empty() &&
          graph.inspectOutput(route, 1).triggers == secondPulses + 3,
          "An invalid index clears the old branch and subsequent data does not send a pulse");
    graph.disconnectInput(second, 0);
    check(findNode(graph, route).outputPorts().size() == 4, "An unwired branch does not renumber later links");
    graph.setValue(index, std::int32_t{2});
    check(read(third) == "parked", "The remaining third output still uses index two");
    graph.disconnectInput(third, 0);
    check(findNode(graph, route).outputPorts().size() == 2, "Removing trailing links trims unused outputs");

    const int sharedIndex = graph.addNode(Int, 0, 0, std::int32_t{0});
    const int selector = graph.addNode(Selector, 0, 0);
    graph.connect({sharedIndex, selector, 0});
    graph.connect({source, selector, 1});
    graph.connect({source, selector, 2});
    graph.disconnectInput(route, 1);
    graph.disconnectInput(route, 0);
    graph.connect({selector, route, 1});
    graph.connect({sharedIndex, route, 0});
    const auto oldBranchPulses = graph.inspectOutput(route, 0).triggers;
    graph.setValue(sharedIndex, std::int32_t{1});
    check(read(first).empty() && graph.inspectOutput(route, 0).triggers == oldBranchPulses + 1,
          "An earlier data pulse from the same index change does not hide the old branch's reset");
}

void indexedRoutersForwardTriggerSignals()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    Schematic graph;
    const int select = graph.addNode(Selector, 0, 0);
    const int route = graph.addNode(Multiplexer, 0, 0);
    const int index = graph.addNode(Int, 0, 0, std::int32_t{0});
    const int a = graph.addNode(Trigger, 0, 0);
    const int b = graph.addNode(Trigger, 0, 0);
    const int first = graph.addNode(TriggerCounter, 0, 0);
    const int second = graph.addNode(TriggerCounter, 0, 0);
    graph.connect({index, select, 0}, LinkNotification::Silent);
    graph.connect({index, route, 0}, LinkNotification::Silent);
    graph.connect({a, select, 1}, LinkNotification::Silent);
    graph.connect({b, select, 2}, LinkNotification::Silent);
    graph.connect({select, route, 1}, LinkNotification::Silent);
    graph.connect({route, first, 0, 0}, LinkNotification::Silent);
    graph.connect({route, second, 0, 1}, LinkNotification::Silent);
    check(graph.outputType(select) == Type::Trigger && graph.outputType(route, 1) == Type::Trigger,
          "The two routers infer trigger-only connectors through the signal path");
    graph.fireTrigger(a);
    graph.fireTrigger(b);
    check(integer(graph, first) == 1 && integer(graph, second) == 0, "Only the first trigger is routed");
    graph.setValue(index, std::int32_t{1});
    check(integer(graph, first) == 1 && integer(graph, second) == 0, "Changing the index never creates an event");
    graph.fireTrigger(a);
    graph.fireTrigger(b);
    check(integer(graph, first) == 1 && integer(graph, second) == 1, "Only the second trigger is now routed");
}

void indexedStreamRoutersPruneAndRebuildTheLiveGraph()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    openstone::Session session;
    auto& graph = session.graph();
    const auto add = [&](PrimitiveKind kind) { return graph.addNode(kind, 0, 0); };
    const auto code = [&](std::string source) {
        const auto node = add(DspCode);
        graph.setDspDraft(node, std::move(source));
        session.compileDsp(node);
        return node;
    };
    const std::string secondSource = "streamout out; float n; out = n + 10; n += 1;";
    const auto a = code("streamout out; float n; out = n + 1; n += 1;");
    const auto b = code(secondSource);
    const auto selector = add(Selector);
    const auto multiplexer = add(Multiplexer);
    const auto selection = add(Int);
    const auto destination = add(Int);
    const auto output = add(AudioOut);
    graph.connect({selection, selector, 0});
    graph.connect({destination, multiplexer, 0});
    graph.connect({a, selector, 1});
    graph.connect({b, selector, 2});
    graph.connect({selector, multiplexer, 1});
    check(graph.outputType(selector) == Type::Stream && graph.outputType(multiplexer) == Type::Stream,
          "Both routers support grey Stream before an audio endpoint specializes the section");
    graph.connect({multiplexer, output, 0, 0});
    graph.connect({multiplexer, output, 1, 1});
    check(graph.outputType(a) == Type::Mono && graph.outputType(b) == Type::Mono &&
          graph.inputType(selector, 0) == Type::Int && graph.outputType(multiplexer, 1) == Type::Mono,
          "Router data connectors specialize together while the index stays Int");

    // An invalid draft on an inactive branch would prevent audio preparation if that branch were compiled.
    graph.setDspDraft(b, "uncompiled source");
    auto patch = graph.prepareAudio(2);
    graph.setLiveAudio(&patch);
    const std::array<float, 3> silence{};
    const auto render = [&](std::array<float, 3> expectedLeft, std::array<float, 3> expectedRight) {
        std::array<float, 3> left{}, right{};
        check(graph.refreshAudioControls(patch) && patch.processor().process({}, {left, right}) &&
              left == expectedLeft && right == expectedRight,
              "The next buffer must render only the selected input and output");
    };
    const auto original = patch.processorHandle();
    render({1, 2, 3}, silence);
    render({4, 5, 6}, silence);
    check(patch.processorHandle() == original && !graph.liveDspTrace(b, 0, true),
          "With the index unchanged, the active patch continues and the inactive source has no trace");

    graph.setDspDraft(b, secondSource);
    graph.setValue(selection, std::int32_t{1});
    render({10, 11, 12}, silence);
    check(patch.processorHandle() != original && !graph.liveDspTrace(a, 0, true) && graph.liveDspTrace(b, 0, true),
          "Switching installs a patch where only the newly selected source runs");

    graph.setValue(destination, std::int32_t{1});
    render(silence, {10, 11, 12});
    graph.setValue(destination, std::int32_t{-1});
    graph.setDspDraft(b, "uncompiled and now disconnected");
    render(silence, silence);
    check(!graph.liveDspTrace(b, 0, true),
          "With no selected wired output, Multiplexer disconnects its upstream DSP");
    graph.setLiveAudio(nullptr);

    graph.setDspDraft(b, secondSource);
    graph.setValue(destination, std::int32_t{1});
    graph.setValue(selection, std::int32_t{2});
    render(silence, silence);
}

void streamRoutersPreservePackedDoubleLanes()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    openstone::Session session;
    auto& graph = session.graph();
    const auto add = [&](PrimitiveKind kind) { return graph.addNode(kind, 0, 0); };
    const auto code = [&](std::string source) {
        const auto node = add(DspCode);
        graph.setDspDraft(node, std::move(source));
        session.compileDsp(node);
        return node;
    };
    const auto source = code(R"(
        streamdoubleout out;
        out = 1.0000000000009094947017729282379150390625;
        out{1} = -2; out{2} = 4; out{3} = 8;
    )");
    const auto selector = add(Selector);
    const auto multiplexer = add(Multiplexer);
    const auto unpack = add(UnpackDouble);
    const auto sink = code(R"(
        monodoublein a; monodoublein b; monodoublein c; monodoublein d;
        monodoubleout w; monodoubleout x; monodoubleout y; monodoubleout z;
        w = a; x = b; y = c; z = d;
    )");
    graph.connect({source, selector, 1});
    graph.connect({selector, multiplexer, 1});
    graph.connect({multiplexer, unpack, 0});
    for (int lane = 0; lane < 4; ++lane) graph.connect({unpack, sink, lane, lane});
    check(graph.outputType(source) == Type::Mono4Double && graph.inputType(multiplexer, 1) == Type::Mono4Double,
          "A packed double constraint propagates backwards through both indexed routers");
    const std::vector<std::vector<double>> expected{
        std::vector<double>(5, 1.0 + 0x1p-40), std::vector<double>(5, -2),
        std::vector<double>(5, 4), std::vector<double>(5, 8)};
    for (const auto block : {1, 3, 5}) {
        check(graph.captureDsp(sink, 5, block) == expected, "Routing preserves all lanes and their double precision");
    }
}

void indexedRoutersPassBusesAndTheirNamedChannels()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    Schematic graph;
    const auto names = graph.addNode(String, 0, 0, std::string{"value"});
    const auto a = graph.addNode(BusCreate, 0, 0);
    const auto b = graph.addNode(BusCreate, 0, 0);
    const auto selector = graph.addNode(Selector, 0, 0);
    const auto multiplexer = graph.addNode(Multiplexer, 0, 0);
    const auto sink = graph.addNode(BusExtract, 0, 0);
    const auto index = graph.addNode(Int, 0, 0, std::int32_t{0});
    graph.connect({index, selector, 0});
    for (auto [bus, number] : {std::pair{a, 8}, std::pair{b, 19}}) {
        graph.connect({names, bus, 0});
        graph.connect({graph.addNode(Int, 0, 0, std::int32_t{number}), bus, 1});
    }
    graph.connect({a, selector, 1});
    graph.connect({b, selector, 2});
    graph.connect({selector, multiplexer, 1});
    graph.connect({names, sink, 0});
    graph.connect({multiplexer, sink, 1});
    check(graph.outputType(selector) == Type::Bus && graph.outputType(multiplexer) == Type::Bus &&
          graph.outputType(sink) == Type::Int && integer(graph, sink) == 8, "The routers pass named Bus channels");
    graph.setValue(index, std::int32_t{1});
    check(integer(graph, sink) == 19, "Selection redirects named Bus channel reads");
}

void intTransitionRequiresConsecutiveMatchingValues()
{
    using enum PrimitiveKind;
    Schematic graph;
    const int value = graph.addNode(Int, 0, 0, std::int32_t{-2});
    const int from = graph.addNode(Int, 0, 0, std::int32_t{-2});
    const int to = graph.addNode(Int, 0, 0, std::int32_t{5});
    const int repeat = graph.addNode(Trigger, 0, 0);
    const int transition = graph.addNode(IntTransition, 0, 0);
    const int count = graph.addNode(TriggerCounter, 0, 0);
    graph.connect({from, transition, 1}, LinkNotification::Silent);
    graph.connect({to, transition, 2}, LinkNotification::Silent);
    graph.connect({transition, count, 0}, LinkNotification::Silent);
    graph.connect({value, transition, 0}, LinkNotification::Silent);
    graph.connect({repeat, transition, 0}, LinkNotification::Silent);
    graph.setValue(value, std::int32_t{5});
    check(integer(graph, count) == 1, "A direct transition from -2 to 5 publishes once");
    graph.fireTrigger(repeat);
    graph.setValue(value, std::int32_t{-2});
    graph.setValue(value, std::int32_t{4});
    graph.setValue(value, std::int32_t{5});
    check(integer(graph, count) == 1, "Repeats, reverse order, and intervening values do not match");

    graph.setValue(from, std::int32_t{5});
    graph.setValue(to, std::int32_t{8});
    check(integer(graph, count) == 1, "Changing the From and To parameters emits no trigger");
    graph.setValue(value, std::int32_t{8});
    check(integer(graph, count) == 2, "The next step matches the updated settings");
}

void greenLogicDistinguishesTransitionsChangesAndSampling()
{
    using enum PrimitiveKind;
    Schematic graph;
    const int boolean = graph.addNode(Boolean, 0, 0, true);
    const int rise = graph.addNode(BoolToTrueTrigger, 180, 0);
    const int fall = graph.addNode(BoolToFalseTrigger, 180, 80);
    const int pulse = graph.addNode(Trigger, 0, 160);
    for (const int id : {rise, fall}) {
        graph.connect({pulse, id, 0}, LinkNotification::Silent);
        graph.connect({boolean, id, 0}, LinkNotification::Silent);
    }
    graph.fireTrigger(pulse);
    check(graph.inspectOutput(rise).triggers == 0 && graph.inspectOutput(fall).triggers == 0,
          "A silent link sets the baseline and an unchanged Boolean does not emit a transition");
    graph.setValue(boolean, false);
    graph.fireTrigger(pulse);
    graph.setValue(boolean, true);
    check(graph.inspectOutput(rise).triggers == 1 && graph.inspectOutput(fall).triggers == 1,
          "Each Boolean edge emits only its own transition trigger");

    const int source = graph.addNode(Int, 0, 240, std::int32_t{3});
    const int changed = graph.addNode(Changed, 180, 240);
    const int hold = graph.addNode(SampleAndHold, 360, 240);
    const int sample = graph.addNode(Trigger, 0, 320);
    graph.connect({source, changed, 0}, LinkNotification::Silent);
    graph.connect({pulse, changed, 0}, LinkNotification::Silent);
    graph.connect({changed, hold, 0}, LinkNotification::Silent);
    graph.connect({sample, hold, 1}, LinkNotification::Silent);
    graph.fireTrigger(pulse);
    check(graph.inspectOutput(changed).triggers == 0 && integer(graph, hold) == 0,
          "Changed suppresses repeated values and Sample and Hold starts at the type default");
    graph.setValue(source, std::int32_t{7});
    graph.fireTrigger(pulse);
    check(integer(graph, changed) == 7 && graph.inspectOutput(changed).triggers == 1 && integer(graph, hold) == 0,
          "Changed emits once per value change and input activity alone does not sample");
    graph.fireTrigger(sample);
    graph.setValue(source, std::int32_t{9});
    check(integer(graph, hold) == 7 && graph.inspectOutput(hold).triggers == 1,
          "The captured value persists until the next sample event");
    graph.fireTrigger(sample);
    check(integer(graph, hold) == 9 && graph.inspectOutput(hold).triggers == 2,
          "A new sample reads the latest input before emitting a trigger");
}

void genericMinMaxUseGreenNumericOperands()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    Schematic graph;
    const int minimum = graph.addNode(Min, 200, 0);
    const int maximum = graph.addNode(Max, 200, 80);
    const int a = graph.addNode(Int, 0, 0, std::numeric_limits<std::int32_t>::min());
    const int b = graph.addNode(Int, 0, 80, std::numeric_limits<std::int32_t>::max());
    const int fraction = graph.addNode(Float, 0, 160, -0.5F);
    const int counter = graph.addNode(TriggerCounter, 400, 0);

    check(!graph.inputType(minimum, 0) && !graph.outputType(minimum) &&
          !graph.inputType(maximum, 1) && !graph.outputType(maximum), "Both extrema start with Template connectors");
    graph.connect({minimum, counter, 0}, LinkNotification::Silent);
    for (const int node : {minimum, maximum}) {
        graph.connect({a, node, 0});
        graph.connect({b, node, 1});
    }
    check(integer(graph, minimum) == std::numeric_limits<std::int32_t>::min() &&
          integer(graph, maximum) == std::numeric_limits<std::int32_t>::max() && integer(graph, counter) == 2,
          "Int extrema preserve the full signed range and deliver a pulse per new input link");

    graph.setValue(a, std::int32_t{-3});
    for (const int node : {minimum, maximum}) {
        graph.disconnectInput(node, 1);
        graph.connect({fraction, node, 1});
    }
    check(graph.outputType(minimum) == Type::Float && graph.outputType(maximum) == Type::Float &&
          std::get<float>(graph.readValue(minimum)) == -3.0F && std::get<float>(graph.readValue(maximum)) == -0.5F,
          "Mixed Int and Float inputs resolve to Float");
    graph.setTemplateType(maximum, Type::Int);
    check(integer(graph, maximum) == 0, "A manual Int selection converts both inputs before comparison");
    graph.setTemplateType(maximum, std::nullopt);

    const auto pulses = graph.inspectOutput(minimum).triggers;
    graph.setValue(fraction, std::numeric_limits<float>::quiet_NaN());
    check(std::get<float>(graph.readValue(minimum)) == -3.0F &&
          std::get<float>(graph.readValue(maximum)) == -3.0F && graph.inspectOutput(minimum).triggers == pulses + 1,
          "Single NaNs select the numeric operand and a trigger still propagates when the result stays equal");
}

void genericMinMaxPairArrayElements()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    Schematic graph;
    const int left = graph.addNode(MultilineString, 0, 0, std::string{"-5\n9\n-3\n6"});
    const int right = graph.addNode(MultilineString, 0, 140, std::string{"-1\n-2"});
    const int a = graph.addNode(Array, 200, 0);
    const int b = graph.addNode(Array, 200, 200);
    const int scalar = graph.addNode(Float, 0, 280, 0.5F);
    const int minimum = graph.addNode(Min, 400, 0);
    const int maximum = graph.addNode(Max, 400, 100);
    graph.setTemplateType(a, Type::IntArray);
    graph.setTemplateType(b, Type::IntArray);
    graph.connect({left, a, 4});
    graph.connect({right, b, 4});

    for (const int node : {minimum, maximum}) {
        graph.connect({a, node, 0});
        graph.connect({b, node, 1});
    }
    check(graph.outputType(minimum) == Type::IntArray && graph.outputType(maximum) == Type::IntArray &&
          std::get<IntArray>(graph.readValue(minimum)) == IntArray{-5, -2, -3, 0} &&
          std::get<IntArray>(graph.readValue(maximum)) == IntArray{-1, 9, 0, 6},
          "Min and Max compare elements by index with the same zero padding as Green arithmetic");

    graph.disconnectInput(minimum, 1);
    graph.connect({scalar, minimum, 1});
    graph.disconnectInput(maximum, 0);
    graph.disconnectInput(maximum, 1);
    graph.connect({scalar, maximum, 0});
    graph.connect({a, maximum, 1});
    check(graph.outputType(minimum) == Type::FloatArray && graph.outputType(maximum) == Type::FloatArray &&
          std::get<FloatArray>(graph.readValue(minimum)) == FloatArray{-5, 0.5F, -3, 0.5F} &&
          std::get<FloatArray>(graph.readValue(maximum)) == FloatArray{0.5F, 9, 0.5F, 6},
          "A Float scalar compares with every element, on either side, and widens an Int Array");

    const int pointMin = graph.addNode(Min, 500, 0);
    const int pointMax = graph.addNode(Max, 500, 80);
    graph.setValue(left, std::string{"1, -2\n-5, 8"});
    graph.setValue(right, std::string{"-4, 6"});
    for (const int node : {pointMin, pointMax}) {
        graph.setTemplateType(node, Type::PointArray);
        graph.connect({left, node, 0});
        graph.connect({right, node, 1});
    }
    check(std::get<PointArray>(graph.readValue(pointMin)) == PointArray{{-4, -2}, {-5, 0}} &&
          std::get<PointArray>(graph.readValue(pointMax)) == PointArray{{1, 6}, {0, 8}},
          "Point Array compares X and Y independently after conversion");
}

void genericMinMaxSpecializeToStreams()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    openstone::Session session;
    auto& graph = session.graph();
    const auto code = [&](std::string source) {
        const auto id = session.addNode(DspCode, 0, 0);
        graph.setDspDraft(id, std::move(source));
        session.compileDsp(id);
        return id;
    };

    const int source = code("monodoubleout out; out = 1.0 + 0.0000000000009094947017729282379150390625;");
    const int sink = code("streamdoublein in; streamdoubleout out; out = in;");
    const int minimum = graph.addNode(Min, 0, 0);
    const int maximum = graph.addNode(Max, 0, 80);
    const int upper = graph.addNode(Float, 0, 160, 2.0F);
    const int lower = graph.addNode(Int, 0, 240, std::int32_t{1});
    graph.connect({upper, minimum, 1});
    check(graph.outputType(minimum) == Type::Float, "Min initially uses Green when wired to a Green value");
    graph.connect({source, minimum, 0});
    graph.connect({minimum, maximum, 0});
    graph.connect({lower, maximum, 1});
    graph.connect({maximum, sink, 0});
    check(graph.outputType(minimum) == Type::MonoDouble && graph.outputType(maximum) == Type::MonoDouble,
          "Both operators follow the connected double-precision stream family");

    const auto expected = std::vector<std::vector<double>>{std::vector<double>(13, 1.0 + 0x1p-40)};
    check(graph.captureDsp(sink, 13, 1) == expected && graph.captureDsp(sink, 13, 5) == expected,
          "Generic Min and Max preserve double precision across capture buffer sizes");
}

void genericPowerUsesGreenOperands()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    Schematic graph;
    const int power = graph.addNode(Power, 180, 0);
    const int base = graph.addNode(Int, 0, 0, std::int32_t{-2});
    const int exponent = graph.addNode(Int, 0, 80, std::int32_t{3});
    const int counter = graph.addNode(TriggerCounter, 360, 0);
    check(!graph.inputType(power, 0) && !graph.inputType(power, 1) && !graph.outputType(power),
          "Power starts with Template connectors");
    graph.connect({power, counter, 0}, LinkNotification::Silent);
    graph.connect({base, power, 0});
    graph.connect({exponent, power, 1});
    check(graph.outputType(power) == Type::Int && integer(graph, power) == -8 && integer(graph, counter) == 2,
          "Int Power preserves operand order and triggers on both new links");

    graph.setValue(exponent, std::int32_t{-3});
    check(integer(graph, power) == 0, "An Int reciprocal truncates to zero for bases outside -1 and 1");
    graph.setValue(base, std::int32_t{-1});
    check(integer(graph, power) == -1, "A negative odd exponent preserves the sign of base -1");
    graph.setValue(exponent, std::numeric_limits<std::int32_t>::min());
    check(integer(graph, power) == 1, "The smallest signed exponent is even");
    graph.setValue(base, std::int32_t{3});
    graph.setValue(exponent, std::int32_t{39});
    check(integer(graph, power) == -1201670133, "Int Power wraps without losing low bits to floating-point rounding");

    const int fraction = graph.addNode(Float, 0, 160, -3.0F);
    graph.disconnectInput(power, 1);
    graph.connect({fraction, power, 1});
    graph.setValue(base, std::int32_t{2});
    const auto value = [&] { return std::get<float>(graph.readValue(power)); };
    check(graph.outputType(power) == Type::Float && value() == 0.125F,
          "Mixed numeric Power preserves a fractional result");
    graph.setValue(base, std::int32_t{9});
    graph.setValue(fraction, 0.5F);
    check(value() == 3.0F, "A fractional exponent can take a root");
    graph.setValue(base, std::int32_t{-9});
    check(std::isnan(value()), "A negative base with a fractional exponent yields NaN");
    graph.setValue(base, std::int32_t{0});
    graph.setValue(fraction, -1.0F);
    check(value() == std::numeric_limits<float>::infinity(), "A zero Float base can produce positive infinity");
}

void genericPowerCombinesArraysInOperandOrder()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    Schematic graph;
    const int base = graph.addNode(Array, 180, 0);
    const int exponent = graph.addNode(Array, 180, 100);
    const int baseText = graph.addNode(MultilineString, 0, 0, std::string{"2\n3\n4"});
    const int exponentText = graph.addNode(MultilineString, 0, 100, std::string{"3\n2"});
    graph.setTemplateType(base, Type::IntArray);
    graph.setTemplateType(exponent, Type::IntArray);
    graph.connect({baseText, base, 4});
    graph.connect({exponentText, exponent, 4});
    const int power = graph.addNode(Power, 360, 0);
    graph.connect({base, power, 0});
    graph.connect({exponent, power, 1});
    check(graph.outputType(power) == Type::IntArray && std::get<IntArray>(graph.readValue(power)) == IntArray{8, 9, 1},
          "Array Power combines corresponding elements and uses arithmetic zero padding");

    const int scalar = graph.addNode(Float, 0, 200, 2.0F);
    graph.disconnectInput(power, 1);
    graph.connect({scalar, power, 1});
    check(graph.outputType(power) == Type::FloatArray &&
          std::get<FloatArray>(graph.readValue(power)) == FloatArray{4, 9, 16},
          "An array base broadcasts the scalar exponent");
    graph.disconnectInput(power, 0);
    graph.disconnectInput(power, 1);
    graph.connect({scalar, power, 0});
    graph.connect({exponent, power, 1});
    check(graph.outputType(power) == Type::FloatArray &&
          std::get<FloatArray>(graph.readValue(power)) == FloatArray{8, 4},
          "A scalar base broadcasts across an array of exponents");
}

void genericPowerSpecializesToStreams()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    openstone::Session session;
    auto& graph = session.graph();
    const auto code = [&](std::string source) {
        const int id = session.addNode(DspCode, 0, 0);
        graph.setDspDraft(id, std::move(source));
        session.compileDsp(id);
        return id;
    };
    const int source = code("monodoubleout out; out = 1.0 + 0.0000000000009094947017729282379150390625;");
    const int sink = code("streamdoublein in; streamdoubleout out; out = in;");
    const int exponent = graph.addNode(Int, 0, 80, std::int32_t{2});
    const int power = graph.addNode(Power, 180, 0);
    graph.connect({source, power, 0});
    graph.connect({exponent, power, 1});
    graph.connect({power, sink, 0});
    check(graph.inputType(power, 1) == Type::MonoDouble && graph.outputType(power) == Type::MonoDouble,
          "Power specializes to the connected stream and accepts Green control values");
    const std::vector<std::vector<double>> expected{std::vector<double>(13, 1.0 + 0x1p-39)};
    check(graph.captureDsp(sink, 13, 1) == expected && graph.captureDsp(sink, 13, 5) == expected,
          "Power preserves the low bits of double streams across block sizes");

    const int audioIn = graph.addNode(AudioIn, 0, 180);
    const int audioOut = graph.addNode(AudioOut, 360, 180);
    const int livePower = graph.addNode(Power, 180, 180);
    graph.connect({audioIn, livePower, 0, 0});
    graph.connect({audioIn, livePower, 1, 1});
    graph.connect({livePower, audioOut, 0});
    auto patch = graph.prepareAudio(2);
    const std::array<float, 3> left{-2.F, 9.F, 2.F}, right{3.F, 0.5F, -3.F};
    std::array<float, 3> result{}, unused{};
    check(patch.processor().process({left, right}, {result, unused}) &&
          result == std::array{-8.F, 3.F, 0.125F},
          "Live audio reads a separate base and exponent at each sample");
}

void genericLogarithmUsesGreenOperands()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    Schematic graph;
    const int logarithm = graph.addNode(Logarithm, 180, 0);
    const int value = graph.addNode(Int, 0, 0, std::int32_t{1000});
    const int base = graph.addNode(Int, 0, 80, std::int32_t{10});
    check(!graph.inputType(logarithm, 0) && !graph.inputType(logarithm, 1) && !graph.outputType(logarithm),
          "Logarithm starts with Template connectors");
    graph.connect({value, logarithm, 0});
    graph.connect({base, logarithm, 1});
    check(graph.outputType(logarithm) == Type::Int && integer(graph, logarithm) == 3,
          "Int Logarithm returns exact results at powers of its base");
    graph.setValue(value, std::int32_t{999});
    check(integer(graph, logarithm) == 2 && graph.inspectOutput(logarithm).triggers == 3,
          "Changing an Int value truncates its logarithm and propagates a trigger");
    graph.setValue(base, std::int32_t{1});
    check(integer(graph, logarithm) == 0, "Invalid Int logarithms yield zero");

    const int floatBase = graph.addNode(Float, 0, 160, 4.0F);
    graph.disconnectInput(logarithm, 1);
    graph.connect({floatBase, logarithm, 1});
    const auto result = [&] { return std::get<float>(graph.readValue(logarithm)); };
    graph.setValue(value, std::int32_t{8});
    check(graph.outputType(logarithm) == Type::Float && std::abs(result() - 1.5F) < 1e-6F,
          "Mixed numeric Logarithm preserves a fractional result");
    graph.setValue(floatBase, 0.5F);
    graph.setValue(value, std::int32_t{4});
    check(std::abs(result() + 2.0F) < 1e-6F, "Logarithm accepts a positive base below one");
    graph.setValue(value, std::int32_t{0});
    check(result() == std::numeric_limits<float>::infinity(), "A base below one reverses the sign at zero");
    graph.setValue(floatBase, 2.0F);
    check(result() == -std::numeric_limits<float>::infinity(), "Logarithm of zero has a pole");
    graph.setValue(value, std::int32_t{-1});
    check(std::isnan(result()), "A negative value has no real logarithm");
    graph.setValue(value, std::int32_t{8});
    for (const float invalid : {0.0F, 1.0F, -2.0F, std::numeric_limits<float>::infinity()}) {
        graph.setValue(floatBase, invalid);
        check(std::isnan(result()), "Invalid Float bases yield NaN");
    }

    const int valuesText = graph.addNode(MultilineString, 0, 240, std::string{"8\n16\n32"});
    const int basesText = graph.addNode(MultilineString, 0, 320, std::string{"2\n4"});
    graph.disconnectInput(logarithm, 0);
    graph.disconnectInput(logarithm, 1);
    graph.setTemplateType(logarithm, Type::IntArray);
    graph.connect({valuesText, logarithm, 0});
    graph.connect({basesText, logarithm, 1});
    check(std::get<IntArray>(graph.readValue(logarithm)) == IntArray{3, 2, 0},
          "Int Array Logarithm pairs values with bases and handles a missing base");
}

void genericLogarithmSpecializesToStreams()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    openstone::Session session;
    auto& graph = session.graph();
    const auto code = [&](std::string source) {
        const int id = session.addNode(DspCode, 0, 0);
        graph.setDspDraft(id, std::move(source));
        session.compileDsp(id);
        return id;
    };
    const int source = code("monodoubleout out; out = 1.0 + 0.0000000000009094947017729282379150390625;");
    const int sink = code("streamdoublein in; streamdoubleout out; out = in;");
    const int base = graph.addNode(Int, 0, 80, std::int32_t{2});
    const int logarithm = graph.addNode(Logarithm, 180, 0);
    graph.connect({source, logarithm, 0});
    graph.connect({base, logarithm, 1});
    graph.connect({logarithm, sink, 0});
    check(graph.inputType(logarithm, 1) == Type::MonoDouble && graph.outputType(logarithm) == Type::MonoDouble,
          "A Green base can control a double-precision stream");
    const auto captured = graph.captureDsp(sink, 13, 1);
    const double expected = std::log1p(0x1p-40) / std::log(2.0);
    check(captured.size() == 1 && captured.front().size() == 13 && graph.captureDsp(sink, 13, 5) == captured &&
          std::ranges::all_of(captured.front(), [=](double sample) { return std::abs(sample - expected) < 1e-25; }),
          "Logarithm preserves double precision across capture buffer sizes");

    const int audioIn = graph.addNode(AudioIn, 0, 180);
    const int audioOut = graph.addNode(AudioOut, 360, 180);
    const int live = graph.addNode(Logarithm, 180, 180);
    graph.connect({audioIn, live, 0, 0});
    graph.connect({audioIn, live, 1, 1});
    graph.connect({live, audioOut, 0});
    auto patch = graph.prepareAudio(2);
    const std::array<float, 4> values{8, 1000, 4, 8}, bases{2, 10, 0.5F, 1};
    std::array<float, 4> result{}, unused{};
    check(patch.processor().process({values, bases}, {result, unused}) &&
          std::abs(result[0] - 3.0F) < 1e-6F && std::abs(result[1] - 3.0F) < 1e-6F &&
          std::abs(result[2] + 2.0F) < 1e-6F && std::isnan(result[3]),
          "Live audio uses the base at each sample and propagates invalid bases as NaN");
}

void genericFixedLogarithmsPreserveGreenTypes()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    Schematic graph;
    constexpr std::array kinds{Ln, Log2, Log10};
    std::array<int, kinds.size()> nodes{};
    const int integerValue = graph.addNode(Int, 0, 0, std::int32_t{1000});
    const int floatValue = graph.addNode(Float, 0, 80, 100.0F);
    constexpr std::array integerResults{6, 9, 3};
    constexpr std::array floatResults{4.605170F, 6.643856F, 2.0F};

    for (std::size_t i = 0; i < kinds.size(); ++i) {
        const int id = graph.addNode(kinds[i], 180, 80.0 * i);
        nodes[i] = id;
        check(!graph.inputType(id, 0) && !graph.outputType(id) && primitive(kinds[i]).inputs.size() == 1,
              "Fixed-base logarithms start with one Template input and output");
        graph.connect({integerValue, id, 0}, LinkNotification::Silent);
        check(graph.outputType(id) == Type::Int && integer(graph, id) == integerResults[i],
              "Int results truncate toward zero, including exact decimal powers");
    }

    graph.setValue(integerValue, std::int32_t{999});
    check(integer(graph, nodes[2]) == 2 && graph.inspectOutput(nodes[2]).triggers == 1,
          "Crossing a decimal power updates the Int result and passes a trigger");
    graph.setValue(integerValue, std::numeric_limits<std::int32_t>::max());
    check(integer(graph, nodes[1]) == 30, "Log2 preserves the exact Int boundary below 2^31");
    graph.setValue(integerValue, std::int32_t{-1});
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const int id = nodes[i];
        check(integer(graph, id) == 0, "Nonpositive Int inputs yield zero");
        graph.disconnectInput(id, 0);
        graph.connect({floatValue, id, 0});
        check(graph.outputType(id) == Type::Float &&
              std::abs(std::get<float>(graph.readValue(id)) - floatResults[i]) < 1e-6F,
              "Float results keep fractional logarithms in the selected base");
    }

    graph.setValue(floatValue, 0.0F);
    for (const int id : nodes) {
        check(std::get<float>(graph.readValue(id)) == -std::numeric_limits<float>::infinity(),
              "Floating-point logarithms of zero return negative infinity");
    }
    graph.setValue(floatValue, -1.0F);
    for (const int id : nodes) {
        check(std::isnan(std::get<float>(graph.readValue(id))), "Negative Float inputs yield NaN");
    }

    const int array = graph.addNode(MultilineString, 0, 160, std::string{"0.5\n8\n0\n-4"});
    graph.disconnectInput(nodes[1], 0);
    graph.setTemplateType(nodes[1], Type::FloatArray);
    graph.connect({array, nodes[1], 0});
    const auto results = std::get<FloatArray>(graph.readValue(nodes[1]));
    check(results.size() == 4 && results[0] == -1.0F && results[1] == 3.0F &&
          results[2] == -std::numeric_limits<float>::infinity() && std::isnan(results[3]),
          "Fixed-base logarithms transform each array element");
}

void genericFixedLogarithmsPreserveStreamPrecision()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    openstone::Session session;
    auto& graph = session.graph();
    const auto code = [&](std::string source) {
        const int id = session.addNode(DspCode, 0, 0);
        graph.setDspDraft(id, std::move(source));
        session.compileDsp(id);
        return id;
    };
    const int source = code("monodoubleout out; out = 1.0 + 0.0000000000009094947017729282379150390625;");
    const int sink = code(R"(
        streamdoublein natural;
        streamdoublein binary;
        streamdoublein decimal;
        streamdoubleout a;
        streamdoubleout b;
        streamdoubleout c;
        a = natural;
        b = binary;
        c = decimal;
    )");
    constexpr std::array kinds{Ln, Log2, Log10};
    for (int i = 0; i < static_cast<int>(kinds.size()); ++i) {
        const int id = graph.addNode(kinds[i], 180, 80.0 * i);
        graph.connect({source, id, 0});
        graph.connect({id, sink, i});
        check(graph.outputType(id) == Type::MonoDouble, "Logarithms specialize to the incoming stream precision");
    }
    const auto captured = graph.captureDsp(sink, 13, 1);
    check(captured.size() == kinds.size() && graph.captureDsp(sink, 13, 5) == captured,
          "Changing the buffer size leaves all logarithm outputs equal");

    const double natural = std::log1p(0x1p-40);
    const std::array expected{natural, natural / std::log(2.0), natural / std::log(10.0)};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        check(captured[i].size() == 13 && std::ranges::all_of(captured[i], [&](double sample) {
            return std::abs(sample - expected[i]) < 1e-25;
        }), "Logarithms retain changes smaller than Float precision");
    }
}

void greenTrigonometryUsesFloatConnectors()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    constexpr auto pi = std::numbers::pi_v<float>;
    struct Example {
        PrimitiveKind kind;
        float input;
        float expected;
    };
    constexpr std::array examples{
        Example{Sin, pi / 6.0F, 0.5F},
        Example{Cos, pi / 3.0F, 0.5F},
        Example{Tan, -pi / 4.0F, -1.0F},
        Example{SinInverse, -0.5F, -pi / 6.0F},
        Example{CosInverse, 0.5F, pi / 3.0F},
        Example{TanInverse, 1.0F, pi / 4.0F},
        Example{Sinh, -1.0F, -1.1752012F},
        Example{Cosh, -1.0F, 1.5430806F},
        Example{Tanh, -1.0F, -0.7615942F},
    };

    Schematic graph;
    const int input = graph.addNode(Float, 0, 0);
    const int output = graph.addNode(Float, 300, 0);
    for (const auto& example : examples) {
        graph.setValue(input, example.input);
        const int math = graph.addNode(example.kind, 150, 0);
        check(graph.inputType(math, 0) == Type::Float && graph.outputType(math) == Type::Float &&
              !Schematic::supportsTemplateType(example.kind, Type::Mono),
              "Green trigonometry declares fixed Float connectors");
        graph.connect({math, output, 0}, LinkNotification::Silent);
        graph.connect({input, math, 0});
        check(std::abs(std::get<float>(graph.readValue(output)) - example.expected) < 1e-6F &&
              graph.inspectOutput(math).triggers == 1,
              "Each trigonometric primitive computes the expected value and forwards the connection pulse");

        if (example.kind == SinInverse || example.kind == CosInverse) {
            graph.setValue(input, 2.0F);
            check(std::isnan(std::get<float>(graph.readValue(output))) && graph.inspectOutput(math).triggers == 2,
                  "Out-of-domain inverse trigonometry forwards NaN and the input pulse");
        } else if (example.kind == Sinh || example.kind == Cosh) {
            graph.setValue(input, 88.5F);
            check(std::get<float>(graph.readValue(output)) == std::numeric_limits<float>::infinity(),
                  "Hyperbolic growth follows the Green Float limit");
        }
        graph.disconnectInput(output, 0);
    }
}

void greenAtan2UsesBothQuadrantsAndNormalFanIn()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    constexpr auto pi = std::numbers::pi_v<float>;
    Schematic graph;
    const int y = graph.addNode(Int, 0, 0, std::int32_t{1});
    const int offset = graph.addNode(String, 0, 80, std::string{"0.5"});
    const int x = graph.addNode(Float, 0, 160, 1.5F);
    const int angle = graph.addNode(Atan2, 150, 0);
    const int sink = graph.addNode(TriggerCounter, 300, 0);
    check(graph.inputType(angle, 0) == Type::Float && graph.inputType(angle, 1) == Type::Float &&
          graph.outputType(angle) == Type::Float, "Atan2 declares two Float inputs and one Float output");
    graph.connect({angle, sink, 0}, LinkNotification::Silent);
    graph.connect({y, angle, 0});
    graph.connect({offset, angle, 0});
    graph.connect({x, angle, 1});
    const auto checkAngle = [&](float expected) {
        check(std::abs(std::get<float>(graph.readValue(angle)) - expected) < 1e-6F,
              "Atan2 measures the angle of (X, Y) in radians");
    };
    checkAngle(pi / 4.0F);
    graph.setValue(x, -1.5F);
    checkAngle(3.0F * pi / 4.0F);
    graph.setValue(y, std::int32_t{-2});
    checkAngle(-3.0F * pi / 4.0F);
    graph.setValue(x, 1.5F);
    checkAngle(-pi / 4.0F);
    graph.setValue(x, 0.0F);
    checkAngle(-pi / 2.0F);
    check(integer(graph, sink) == 7, "Atan2 forwards each pulse from either operand");

    const int audio = graph.addNode(AudioIn, 0, 240);
    bool rejected = false;
    try {
        graph.connect({audio, angle, 0});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected && graph.inputType(angle, 0) == Type::Float,
          "A stream cannot specialize a Green Atan2 connector");
    checkAngle(-pi / 2.0F);
}

void greenHypotenuseSupportsFloatScalarsAndArrays()
{
    using enum PrimitiveKind;
    using openstone::green::FloatArray;
    using openstone::green::Type;
    Schematic graph;
    const int length = graph.addNode(Hypotenuse, 300, 0);
    const int left = graph.addNode(Float, 0, 0, -3.0F);
    const int right = graph.addNode(Float, 0, 60, 4.0F);
    check(!graph.inputType(length, 0) && !graph.inputType(length, 1) && !graph.outputType(length) &&
          !Schematic::supportsTemplateType(Hypotenuse, Type::Int) &&
          !Schematic::supportsTemplateType(Hypotenuse, Type::Mono), "Hypotenuse starts as a Green Template");
    graph.connect({left, length, 0});
    graph.connect({right, length, 1});
    check(graph.outputType(length) == Type::Float && std::get<float>(graph.readValue(length)) == 5.0F &&
          graph.inspectOutput(length).triggers == 2, "Hypotenuse uses both Float legs and forwards each pulse");
    graph.setValue(left, 1e20F);
    check(std::isinf(std::get<float>(graph.readValue(length))), "Hypotenuse squares its operands in Float precision");

    const int leftText = graph.addNode(MultilineString, 0, 120, std::string{"3\n-4\n12"});
    const int rightText = graph.addNode(MultilineString, 0, 200, std::string{"4\n3"});
    const int leftArray = graph.addNode(Array, 150, 120);
    const int rightArray = graph.addNode(Array, 150, 200);
    graph.setTemplateType(leftArray, Type::FloatArray);
    graph.setTemplateType(rightArray, Type::FloatArray);
    graph.connect({leftText, leftArray, 4});
    graph.connect({rightText, rightArray, 4});
    graph.disconnectInput(length, 0);
    graph.connect({leftArray, length, 0});
    check(graph.inputType(length, 1) == Type::Float && graph.outputType(length) == Type::FloatArray,
          "Hypotenuse detects an array and keeps an explicitly wired scalar operand");
    const auto result = [&] { return std::get<FloatArray>(graph.readValue(length)); };
    check(result() == FloatArray{5.0F, std::sqrt(32.0F), std::sqrt(160.0F)}, "Hypotenuse broadcasts a Float leg");
    graph.disconnectInput(length, 1);
    graph.connect({rightArray, length, 1});
    check(result() == FloatArray{5.0F, 5.0F, 12.0F}, "Uneven Float Arrays use the Green zero padding rule");
}

void greenReciprocalAndSignPreserveTheirConnectorTypes()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    constexpr float maxFloat = std::numeric_limits<float>::max();
    Schematic graph;
    const int reciprocal = graph.addNode(Reciprocal, 300, 0);
    const int sign = graph.addNode(Sign, 300, 60);
    check(!graph.inputType(reciprocal, 0) && !graph.outputType(reciprocal) &&
          !graph.inputType(sign, 0) && !graph.outputType(sign), "Reciprocal and Sign start as Templates");
    check(!Schematic::supportsTemplateType(Reciprocal, Type::Int) &&
          !Schematic::supportsTemplateType(Reciprocal, Type::IntArray) &&
          !Schematic::supportsTemplateType(Sign, Type::PointArray), "The type selectors omit unsupported variants");

    const int source = graph.addNode(Float, 0, 0, -4.0F);
    const int readout = graph.addNode(Float, 450, 0);
    graph.connect({source, reciprocal, 0});
    graph.connect({source, sign, 0});
    graph.connect({reciprocal, readout, 0});
    const auto floatResult = [&](int id) { return std::get<float>(graph.readValue(id)); };
    check(floatResult(readout) == -0.25F && floatResult(sign) == -1.0F, "Float operands preserve fractions and signs");
    graph.setValue(source, 4.0F);
    check(floatResult(readout) == 0.25F && floatResult(sign) == 1.0F, "Float operands can change sign");
    graph.setValue(source, 0.0F);
    check(floatResult(reciprocal) == maxFloat && std::isinf(floatResult(readout)) && floatResult(sign) == 1.0F &&
          graph.inspectOutput(sign).triggers == 3, "Zero uses Alpha's reciprocal sentinel and positive Sign");
    graph.setValue(source, -4.0F);
    graph.setValue(source, -0.0F);
    check(std::signbit(floatResult(source)) && floatResult(reciprocal) == maxFloat && floatResult(sign) == 1.0F,
          "Negative zero follows the same rule");
    graph.disconnectInput(readout, 0);

    const int intValue = graph.addNode(Int, 0, 120, std::numeric_limits<std::int32_t>::min());
    graph.disconnectInput(sign, 0);
    graph.connect({intValue, sign, 0});
    check(graph.outputType(sign) == Type::Int && integer(graph, sign) == -1, "Sign preserves the Int type at INT32_MIN");
    graph.setValue(intValue, std::int32_t{0});
    check(integer(graph, sign) == 1, "Int zero also produces +1");

    const int text = graph.addNode(MultilineString, 0, 200, std::string{"-4\n0\n8"});
    const int array = graph.addNode(Array, 150, 200);
    graph.setTemplateType(array, Type::FloatArray);
    graph.connect({text, array, 4});
    graph.disconnectInput(reciprocal, 0);
    graph.disconnectInput(sign, 0);
    graph.connect({array, reciprocal, 0});
    graph.connect({array, sign, 0});
    check(std::get<FloatArray>(graph.readValue(reciprocal)) == FloatArray{-0.25F, maxFloat, 0.125F} &&
          std::get<FloatArray>(graph.readValue(sign)) == FloatArray{-1.0F, 1.0F, 1.0F},
          "Reciprocal and Sign evaluate each Float Array element independently");
    const int intArray = graph.addNode(Array, 150, 280);
    graph.setTemplateType(intArray, Type::IntArray);
    graph.connect({text, intArray, 4});
    graph.disconnectInput(sign, 0);
    graph.connect({intArray, sign, 0});
    check(std::get<IntArray>(graph.readValue(sign)) == IntArray{-1, 1, 1}, "Sign preserves Int Array elements");
}

void genericReciprocalAndSignUseTheExistingStreamOperators()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    Schematic graph;
    const int input = graph.addNode(AudioIn, 0, 0);
    const int reciprocal = graph.addNode(Reciprocal, 150, 0);
    const int sign = graph.addNode(Sign, 150, 80);
    const int output = graph.addNode(AudioOut, 300, 0);
    graph.connect({input, reciprocal, 0});
    graph.connect({input, sign, 0});
    graph.connect({reciprocal, output, 0});
    graph.connect({sign, output, 1});
    check(graph.outputType(reciprocal) == Type::Mono && graph.outputType(sign) == Type::Mono,
          "The Template unary primitives specialize to Mono when wired to audio");
    auto patch = graph.prepareAudio(2);
    const std::array<float, 3> inputSamples{-4.0F, 0.0F, 2.0F}, silence{};
    std::array<float, 3> reciprocals{}, signs{};
    check(patch.processor().process({inputSamples, silence}, {reciprocals, signs}) &&
          reciprocals == std::array{-0.25F, std::numeric_limits<float>::infinity(), 0.5F} &&
          signs == std::array{-1.0F, 1.0F, 1.0F}, "The generic nodes reuse the existing stream kernels");
}

void generalProcessingConversionsUseTheirConnectorFamilies()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    Schematic graph;
    const int hz = graph.addNode(Float, 0, 0, 440.0F);
    const int toPitch = graph.addNode(HzToPitch, 0, 0);
    const int toHz = graph.addNode(PitchToHz, 0, 0);
    const int toFrequency = graph.addNode(HzToFrequency, 0, 0);
    const int frequencyToHz = graph.addNode(FrequencyToHz, 0, 0);
    const int frequencyToPitch = graph.addNode(FrequencyToPitch, 0, 0);
    const int pitchToFrequency = graph.addNode(PitchToFrequency, 0, 0);
    graph.connect({hz, toPitch, 0});
    graph.connect({toPitch, toHz, 0});
    graph.connect({hz, toFrequency, 0});
    graph.connect({toFrequency, frequencyToHz, 0});
    graph.connect({toFrequency, frequencyToPitch, 0});
    graph.connect({frequencyToPitch, pitchToFrequency, 0});
    const auto value = [&](int id) { return std::get<float>(graph.readValue(id)); };
    check(value(toPitch) == 69 && value(toHz) == 440 && value(frequencyToPitch) == 69 &&
          std::abs(value(pitchToFrequency) - 880.0F / 44100.0F) < 1e-8F &&
          std::abs(value(frequencyToHz) - 440.0F) < 1e-4F,
          "The Hz and normalized-frequency pitch pairs round trip their inputs");
    graph.setSampleRate(48000);
    check(value(toFrequency) == 880.0F / 48000.0F &&
          std::abs(value(frequencyToHz) - 440.0F) < 1e-4F,
          "The normalized-frequency conversion tracks the graph sample rate");
    graph.setValue(hz, 220.0F);
    check(value(toPitch) == 57 && value(frequencyToPitch) == 57 &&
          std::abs(value(pitchToFrequency) - 440.0F / 48000.0F) < 1e-8F,
          "The normalized pitch formula retains the Alpha offset");

    const int linear = graph.addNode(Float, 0, 0, 2.0F);
    const int toDecibel = graph.addNode(ToDecibel, 0, 0);
    const int fromDecibel = graph.addNode(FromDecibel, 0, 0);
    graph.connect({linear, toDecibel, 0});
    graph.connect({toDecibel, fromDecibel, 0});
    check(std::abs(value(toDecibel) - 6.0206F) < 1e-5F && std::abs(value(fromDecibel) - 2) < 1e-6F,
          "The decibel conversions round-trip a linear amplitude");
    graph.setValue(linear, 0.0F);
    check(value(toDecibel) == -std::numeric_limits<float>::infinity() && value(fromDecibel) == 0,
          "Zero amplitude maps to negative infinity");

    const auto minInt = std::numeric_limits<std::int32_t>::min();
    const int number = graph.addNode(Int, 0, 0, minInt);
    const int inverse = graph.addNode(Inverse, 0, 0);
    graph.connect({number, inverse, 0});
    check(graph.outputType(inverse) == Type::Int && integer(graph, inverse) == minInt,
          "Int Inverse preserves two's-complement wrapping");
    graph.setValue(number, std::int32_t{7});
    check(integer(graph, inverse) == -7, "Inverse negates Int values");
    const int text = graph.addNode(MultilineString, 0, 0, std::string{"1\n0.5"});
    const int array = graph.addNode(HzToFrequency, 0, 0);
    graph.setTemplateType(array, Type::FloatArray);
    graph.connect({text, array, 0});
    check(std::get<FloatArray>(graph.readValue(array)) == FloatArray{2.0F / 48000.0F, 1.0F / 48000.0F},
          "The Green frequency conversion maps every Float Array element");
}

void generalProcessingStreamConversionsUseTheHostSampleRate()
{
    using enum PrimitiveKind;
    Schematic graph;
    const int input = graph.addNode(AudioIn, 0, 0);
    const int hz = graph.addNode(FrequencyToHz, 0, 0);
    const int inverse = graph.addNode(Inverse, 0, 0);
    const int output = graph.addNode(AudioOut, 0, 0);
    graph.connect({input, hz, 0});
    graph.connect({hz, inverse, 0});
    graph.connect({inverse, output, 0});
    graph.setSampleRate(48000);
    auto patch = graph.prepareAudio(2);
    const std::array inputSamples{0.0F, 0.5F, 1.0F};
    std::array<float, inputSamples.size()> left{}, right{};
    check(patch.processor().process({inputSamples, {}}, {left, right}) &&
          left == std::array{0.0F, -12000.0F, -24000.0F}, "The audio graph uses the configured sample rate");
    graph.setSampleRate(96000);
    check(graph.refreshAudioControls(patch) &&
          patch.processor().process({inputSamples, {}}, {left, right}) &&
          left == std::array{0.0F, -24000.0F, -48000.0F}, "A host rate change updates the running patch");
}

void generalProcessingEvaluatesFormulasAndConvertsNoteNames()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    Schematic graph;
    const int note = graph.addNode(Int, 0, 0, std::int32_t{60});
    const int toName = graph.addNode(IntToNoteName, 0, 0);
    const int readout = graph.addNode(String, 0, 0);
    const int fromName = graph.addNode(NoteNameToInt, 0, 0);
    graph.connect({toName, readout, 0}, LinkNotification::Silent);
    check(graph.inputType(toName, 0) == Type::Int && graph.outputType(toName) == Type::String,
          "A typed output link can specialize an unconnected note converter");
    graph.connect({note, toName, 0});
    graph.connect({readout, fromName, 0});
    check(std::get<std::string>(graph.readValue(readout)) == "C4" && integer(graph, fromName) == 60,
          "A MIDI note round-trips through its name");
    const int names = graph.addNode(MultilineString, 0, 0, std::string{"A4\nDb4\ninvalid"});
    const int namesToNotes = graph.addNode(NoteNameToInt, 0, 0);
    graph.setTemplateType(namesToNotes, Type::StringArray);
    graph.connect({names, namesToNotes, 0});
    check(std::get<IntArray>(graph.readValue(namesToNotes)) == IntArray{69, 61, -1},
          "Note arrays support accidentals and preserve indices for invalid names");

    const int formula = graph.addNode(String, 0, 0, std::string{"x^2 + y + sin(pi/2)"});
    const int x = graph.addNode(Float, 0, 0, 3.0F);
    const int y = graph.addNode(Float, 0, 0, 2.0F);
    const int eval = graph.addNode(Evaluator, 0, 0);
    graph.connect({formula, eval, 0});
    graph.connect({x, eval, 1});
    graph.connect({y, eval, 2});
    const auto result = [&] { return std::get<float>(graph.readValue(eval)); };
    check(result() == 12, "Evaluator can evaluate powers and radian sine with wired variables");
    graph.setValue(formula, std::string{"x; out = 2"});
    check(std::isnan(result()), "Evaluator rejects a program in the expression input");
    graph.setValue(formula, std::string{"y / x"});
    check(result() == 2.0F / 3, "A corrected expression is evaluated with the same connector values");
}

void generalProcessingDispatchesRandomSlideAndGoalEvents()
{
    using namespace std::chrono_literals;
    using enum PrimitiveKind;
    using enum openstone::TickWake;
    Schematic graph;
    const int random = graph.addNode(RandomNumber, 0, 0);
    const int seed = graph.addNode(Int, 0, 0, std::int32_t{1});
    const int get = graph.addNode(Trigger, 0, 0);
    graph.connect({seed, random, 1});
    graph.connect({get, random, 0}, LinkNotification::Silent);
    for (const int expected : {41, 18467, 6334}) {
        graph.fireTrigger(get);
        check(integer(graph, random) == expected, "A fixed seed produces the matching repeatable sequence");
    }
    graph.setValue(seed, std::int32_t{2});
    graph.setValue(seed, std::int32_t{1});
    graph.fireTrigger(get);
    check(integer(graph, random) == 41, "A seed update restarts the sequence");

    const int slide = graph.addNode(FloatSlide, 0, 0);
    const int target = graph.addNode(Float, 0, 0, 5.0F);
    const int rate = graph.addNode(Int, 0, 0, std::int32_t{10});
    const int step = graph.addNode(Float, 0, 0, 2.0F);
    graph.connect({target, slide, 0});
    graph.connect({rate, slide, 1});
    graph.connect({step, slide, 2});
    const auto advance = [&](std::chrono::milliseconds time, openstone::TickWake wake) {
        const auto schedule = graph.advanceTicks(openstone::TickTime{} + time, wake);
        if (schedule.error) std::rethrow_exception(schedule.error);
        return schedule;
    };
    const auto result = [&] { return std::get<float>(graph.readValue(slide)); };
    check(!advance(0ms, Configure).fired && !advance(9ms, Timer).fired && result() == 0,
          "Slide waits for its next scheduled step");
    check(advance(10ms, Timer).fired && result() == 2 &&
          advance(20ms, Timer).fired && result() == 4 &&
          !advance(31ms, Timer).deadline && result() == 5,
          "Slide stops at the target without overshooting it");
    const int jump = graph.addNode(Float, 0, 0, 7.0F);
    graph.connect({jump, slide, 3});
    check(result() == 7, "Set changes the slide output immediately");

    const int goal = graph.addNode(TeachingGoal, 0, 0);
    const int progress = graph.addNode(GoalProgress, 0, 0);
    const int achieve = graph.addNode(Trigger, 0, 0);
    const int reset = graph.addNode(Trigger, 0, 0);
    auto& child = *graph.module(graph.addNode(Module, 0, 0));
    const int nestedGoal = child.addNode(TeachingGoal, 0, 0);
    const int nestedAchieve = child.addNode(Trigger, 0, 0);
    graph.connect({achieve, goal, 0}, LinkNotification::Silent);
    graph.connect({reset, progress, 0}, LinkNotification::Silent);
    child.connect({nestedAchieve, nestedGoal, 0}, LinkNotification::Silent);
    const auto complete = [&] { return std::get<std::int32_t>(graph.readValue(progress, 1)); };
    check(integer(graph, progress) == 2 && complete() == 0, "Goal Progress counts nested goals");
    graph.fireTrigger(achieve);
    graph.fireTrigger(achieve);
    check(complete() == 1 && graph.inspectOutput(progress, 1).triggers == 1,
          "A Teaching Goal completes and publishes once per reset");
    child.fireTrigger(nestedAchieve);
    check(complete() == 2, "Nested completion reaches the root report");
    graph.fireTrigger(reset);
    check(complete() == 0 && integer(graph, progress) == 2, "Reset clears completion throughout the schematic");
}

void genericRoundingPreservesGreenConnectorTypes()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    Schematic graph;
    const int round = graph.addNode(Round, 180, 0);
    const int floor = graph.addNode(Floor, 180, 80);
    const int ceiling = graph.addNode(Ceiling, 180, 160);
    const int truncate = graph.addNode(Truncate, 180, 240);
    const int value = graph.addNode(Float, 0, 0, 2.5F);
    const int counter = graph.addNode(TriggerCounter, 360, 80);

    for (const int node : {round, floor, ceiling, truncate}) {
        check(!graph.inputType(node, 0) && !graph.outputType(node), "Rounding starts with Template connectors");
        graph.connect({value, node, 0}, LinkNotification::Silent);
        check(graph.inputType(node, 0) == Type::Float && graph.outputType(node) == Type::Float,
              "Rounding infers Float on both connectors");
    }
    graph.connect({floor, counter, 0}, LinkNotification::Silent);

    const auto result = [&](int id) { return std::get<float>(graph.readValue(id)); };
    check(result(round) == 3 && result(floor) == 2 && result(ceiling) == 3 && result(truncate) == 2,
          "Rounding primitives handle positive halves according to their mode");
    graph.setValue(value, -2.5F);
    check(result(round) == -3 && result(floor) == -3 && result(ceiling) == -2 && result(truncate) == -2,
          "Rounding primitives handle negative halves according to their mode");
    graph.setValue(value, -2.25F);
    check(result(floor) == -3 && integer(graph, counter) == 2 &&
          result(truncate) == -2 && graph.inspectOutput(truncate).triggers == 2,
          "Green triggers pass through even when the rounded result stays equal");
    graph.setValue(value, -0.25F);
    for (const int node : {round, ceiling, truncate}) {
        check(result(node) == 0 && std::signbit(result(node)), "Rounding preserves the sign of a result rounded to zero");
    }
    graph.setValue(value, std::numeric_limits<float>::infinity());
    check(std::isinf(result(round)) && std::isinf(result(floor)) && std::isinf(result(ceiling)) &&
          std::isinf(result(truncate)), "Rounding keeps infinity in the Float family");
    graph.setValue(value, std::numeric_limits<float>::quiet_NaN());
    check(std::isnan(result(round)) && std::isnan(result(floor)) && std::isnan(result(ceiling)) &&
          std::isnan(result(truncate)), "Rounding propagates NaN");

    const auto largeInt = std::numeric_limits<std::int32_t>::max();
    const int integerSource = graph.addNode(Int, 0, 80, largeInt);
    for (const int node : {round, floor, ceiling, truncate}) {
        graph.disconnectInput(node, 0);
        graph.connect({integerSource, node, 0});
        check(graph.outputType(node) == Type::Int && integer(graph, node) == largeInt,
              "Rewiring to Int preserves the full signed precision");
    }

    const int text = graph.addNode(MultilineString, 0, 160, std::string{"2.5\n-2.5\n0.25\n-0.25"});
    graph.disconnectInput(round, 0);
    graph.setTemplateType(round, Type::FloatArray);
    graph.connect({text, round, 0});
    check(std::get<FloatArray>(graph.readValue(round)) == FloatArray{3, -3, 0, 0},
          "A pinned Float Array rounds every parsed element");
    graph.disconnectInput(truncate, 0);
    graph.setTemplateType(truncate, Type::FloatArray);
    graph.connect({text, truncate, 0});
    check(std::get<FloatArray>(graph.readValue(truncate)) == FloatArray{2, -2, 0, 0},
          "Truncate rounds mixed-sign Float Array elements toward zero");
    graph.disconnectInput(ceiling, 0);
    graph.setTemplateType(ceiling, Type::PointArray);
    graph.setValue(text, std::string{"2.5, -2.5\n0.25, -0.25"});
    graph.connect({text, ceiling, 0});
    check(std::get<PointArray>(graph.readValue(ceiling)) == PointArray{{3, -2}, {1, 0}},
          "Ceiling in Point Array mode applies to both coordinates of each element");
}

void genericRoundingPreservesDoubleStreamPrecision()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    openstone::Session session;
    auto& graph = session.graph();
    const auto code = [&](std::string source) {
        const int id = session.addNode(DspCode, 0, 0);
        graph.setDspDraft(id, std::move(source));
        session.compileDsp(id);
        return id;
    };
    const int source = code(R"(
        monodoubleout half;
        monodoubleout below;
        monodoubleout above;
        double tiny = 0.0000000000009094947017729282379150390625;
        half = -2.5 + tiny;
        below = 3 - tiny;
        above = -3 + tiny;
    )");
    const int sink = code(R"(
        streamdoublein nearest;
        streamdoublein lower;
        streamdoublein higher;
        streamdoublein truncated;
        streamdoubleout a;
        streamdoubleout b;
        streamdoubleout c;
        streamdoubleout d;
        a = nearest;
        b = lower;
        c = higher;
        d = truncated;
    )");

    constexpr std::array kinds{Round, Floor, Ceiling, Truncate};
    constexpr std::array sourcePorts{0, 1, 2, 2};
    for (int i = 0; i < static_cast<int>(kinds.size()); ++i) {
        const int node = graph.addNode(kinds[i], 180, 80.0 * i);
        graph.connect({source, node, 0, sourcePorts[i]});
        graph.connect({node, sink, i});
        check(graph.inputType(node, 0) == Type::MonoDouble && graph.outputType(node) == Type::MonoDouble,
              "Rounding specializes to the connected stream precision and processing mode");
    }
    const std::vector expected{std::vector<double>(13, -2), std::vector<double>(13, 2),
                               std::vector<double>(13, -2), std::vector<double>(13, -2)};
    check(graph.captureDsp(sink, 13, 1) == expected && graph.captureDsp(sink, 13, 5) == expected,
          "Rounding preserves near-integer and near-half distinctions in double precision across blocks");
}

void areaPrimitivesConvertFloatAndPointGeometry()
{
    using enum PrimitiveKind;
    using Coordinate = openstone::green::Point;
    using Rectangle = openstone::yellow::Area;

    Schematic graph;
    const int x = graph.addNode(Float, 0, 0, 5.0F);
    const int y = graph.addNode(Float, 0, 0, 7.0F);
    const int width = graph.addNode(Float, 0, 0, -4.0F);
    const int height = graph.addNode(Float, 0, 0, -6.0F);
    const int fromFloats = graph.addNode(Area, 0, 0);
    const int toFloats = graph.addNode(AreaToFloat, 0, 0);
    const int toPoints = graph.addNode(AreaToPoints, 0, 0);
    const int fromPoints = graph.addNode(AreaFromPoints, 0, 0);
    const int center = graph.addNode(AreaCenterPoint, 0, 0);
    const int around = graph.addNode(AreaAroundCenterPoint, 0, 0);
    const int size = graph.addNode(Float, 0, 0, 4.0F);
    const SchematicLink links[]{
        {x, fromFloats, 0}, {y, fromFloats, 1}, {width, fromFloats, 2}, {height, fromFloats, 3},
        {fromFloats, toFloats, 0}, {fromFloats, toPoints, 0}, {toPoints, fromPoints, 0, 1},
        {toPoints, fromPoints, 1, 0}, {fromPoints, center, 0}, {center, around, 0}, {size, around, 1},
    };
    for (const auto link : links) {
        graph.connect(link, LinkNotification::Silent);
    }

    const auto rectangle = [&](int id) { return std::get<Rectangle>(graph.readValue(id)); };
    const auto corner = [&](int id, int port = 0) { return std::get<Coordinate>(graph.readValue(id, port)); };
    check(rectangle(fromFloats) == Rectangle{5, 7, -4, -6}, "F2A should store signed dimensions");
    const std::array expected{5.0F, 7.0F, -4.0F, -6.0F};
    for (int output = 0; output < 4; ++output) {
        check(std::get<float>(graph.readValue(toFloats, output)) == expected[output],
              "A2F should return the raw X, Y, Width and Height in order");
    }
    check(corner(toPoints) == Coordinate{1, 1} && corner(toPoints, 1) == Coordinate{5, 7},
          "A2P should locate the geometric corners of a signed Area");
    check(rectangle(fromPoints) == Rectangle{1, 1, 4, 6} && corner(center) == Coordinate{3, 4},
          "P2A should accept reverse corner order and Center should read the midpoint");
    check(rectangle(around) == Rectangle{1, 2, 4, 4}, "C2A should use the connected width as the square size");
    graph.connect({height, around, 2}, LinkNotification::Silent);
    check(rectangle(around) == Rectangle{1, 7, 4, -6}, "C2A should honor independently wired signed dimensions");
    graph.disconnectInput(around, 1);
    check(rectangle(around) == Rectangle{6, 7, -6, -6}, "C2A should also use Height alone as the square size");

    graph.setValue(x, 9.0F);
    for (int output = 0; output < 4; ++output) {
        check(graph.inspectOutput(toFloats, output).triggers == 1, "Each A2F output should pass the Area trigger");
    }
    check(graph.inspectOutput(toPoints, 0).triggers == 1 && graph.inspectOutput(toPoints, 1).triggers == 1,
          "Each A2P output should pass the Area trigger");
}

void areaPrimitivesAlignAndFindUnion()
{
    using enum PrimitiveKind;
    using Rectangle = openstone::yellow::Area;

    Schematic graph;
    const int reference = graph.addNode(String, 0, 0, std::string{"10,20,12,8"});
    const int source = graph.addNode(String, 0, 0, std::string{"30,35,4,2"});
    const int horizontal = graph.addNode(String, 0, 0, std::string{" Middle "});
    const int vertical = graph.addNode(String, 0, 0, std::string{"FAR"});
    const int align = graph.addNode(AlignArea, 0, 0);
    const int covered = graph.addNode(AreaUnion, 0, 0);
    const SchematicLink links[]{
        {source, align, 1}, {horizontal, align, 2}, {vertical, align, 3}, {source, covered, 1},
    };
    for (const auto link : links) {
        graph.connect(link, LinkNotification::Silent);
    }

    const auto rectangle = [&](int id) { return std::get<Rectangle>(graph.readValue(id)); };
    check(rectangle(align) == Rectangle{30, 35, 4, 2} && rectangle(covered) == Rectangle{30, 35, 4, 2},
          "An unwired reference or union operand should leave the connected Area intact");
    graph.connect({reference, align, 0}, LinkNotification::Silent);
    graph.connect({reference, covered, 0}, LinkNotification::Silent);
    check(rectangle(align) == Rectangle{14, 26, 4, 2}, "Align should center horizontally and align to the far edge");
    check(rectangle(covered) == Rectangle{10, 20, 24, 17}, "Union should bound both disjoint Areas");
    graph.setValue(horizontal, std::string{"right"});
    graph.setValue(vertical, std::string{"near"});
    check(rectangle(align) == Rectangle{18, 20, 4, 2}, "Align should interpret directional aliases per axis");
}

void areaPrimitivesTestRectangleAndPolygonHits()
{
    using enum PrimitiveKind;
    Schematic graph;
    const int area = graph.addNode(String, 0, 0, std::string{"6,6,-6,-6"});
    const int polygon = graph.addNode(String, 0, 0, std::string{"0,0\n6,0\n6,6\n4,6\n4,2\n2,2\n2,6\n0,6"});
    const int sample = graph.addNode(Point, 0, 0);
    const int x = graph.addNode(Float, 0, 0, 3.0F);
    const int y = graph.addNode(Float, 0, 0, 4.0F);
    const int areaHit = graph.addNode(PointInsideArea, 0, 0);
    const int polygonHit = graph.addNode(PointInsidePolygon, 0, 0);
    const SchematicLink links[]{
        {x, sample, 0}, {y, sample, 1}, {area, areaHit, 0}, {sample, areaHit, 1}, {polygon, polygonHit, 0}, {sample, polygonHit, 1},
    };
    for (const auto link : links) {
        graph.connect(link, LinkNotification::Silent);
    }

    const auto hit = [&](int id) { return std::get<bool>(graph.readValue(id)); };
    check(hit(areaHit) && !hit(polygonHit), "A point in a concave notch should be outside the polygon");
    graph.setValue(x, 1.0F);
    check(hit(areaHit) && hit(polygonHit), "A point inside both shapes should report True");
    graph.setValue(x, 0.0F);
    check(hit(areaHit) && hit(polygonHit), "The implicit closing edge participates in the crossing rule");
    graph.setValue(x, 6.0F);
    check(hit(areaHit) && !hit(polygonHit), "Area edges are inclusive and the polygon crossing uses a strict comparison");
    graph.setValue(x, 7.0F);
    check(!hit(areaHit) && !hit(polygonHit), "A point outside both shapes should report False");
    graph.setValue(polygon, std::string{"0,0\n1,1"});
    graph.setValue(x, 0.0F);
    graph.setValue(y, 0.0F);
    check(!hit(polygonHit), "A polygon with fewer than three points has no interior");
}

void yellowPanelAndArea()
{
    using openstone::green::Type;
    using openstone::green::Value;
    using openstone::yellow::View;
    using enum PrimitiveKind;
    Schematic root;
    const int module = root.addNode(Module, 0, 0);
    auto& schematic = *root.module(module);
    const int parent = schematic.addNode(ModuleGui, 0, 0);
    const int child = schematic.addNode(ModuleGui, 200, 0);
    const int other = schematic.addNode(ModuleGui, 0, 200);
    check(schematic.inputType(child, 0) == Type::View && schematic.inputType(child, 1) == Type::Boolean &&
          schematic.inputType(child, 2) == Type::Boolean && schematic.outputType(child) == Type::View,
          "MGUI should expose its three Alpha inputs and a View output");
    const auto standalone = schematic.readModulePanel(child);
    check(standalone.view.panel == child && standalone.view.parentPanel == 0 &&
          standalone.view.bounds.width > 0 && standalone.view.bounds.height > 0 &&
          !standalone.view.mouseMoves && standalone.borderVisible,
          "An independent MGUI should have a panel, with hover off and its editor border on");

    const SchematicLink route{parent, child, 0};
    schematic.connect(route);
    const auto routed = schematic.readModulePanel(child).view;
    check(routed.panel == child && routed.parentPanel == parent && schematic.inspectLink(route).triggers == 1,
          "A View link should carry the parent context and deliver a trigger");
    bool rejected = false;
    try {
        schematic.connect({other, child, 0});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected && schematic.links().size() == 1 && schematic.readModulePanel(child).view == routed,
          "A second View source must leave the existing parent intact");

    const int toggle = schematic.addNode(Boolean, 0, 320);
    schematic.connect({toggle, child, 2});
    check(!schematic.readModulePanel(child).borderVisible &&
          schematic.readModulePanel(child).view == routed && schematic.inspectOutput(child).triggers == 1,
          "The editor border must stay outside the View data and trigger path");
    schematic.disconnectInput(child, 2);
    check(schematic.readModulePanel(child).borderVisible, "Removing the border source should restore the default");
    schematic.connect({toggle, child, 1});
    schematic.setValue(toggle, true);
    check(schematic.readModulePanel(child).view.mouseMoves && schematic.inspectOutput(child).triggers == 3,
          "Mouse toggles should update the View and pass their Green triggers");
    schematic.removeNode(parent);
    check(schematic.readModulePanel(child).view.parentPanel == 0,
          "Removing the parent should leave the child as an independent drawing target");

    const int area = schematic.addNode(Area, 400, 0);
    const int x = schematic.addNode(Float, 0, 400, -2.5F);
    const int width = schematic.addNode(Int, 0, 480, std::int32_t{-12});
    const int areaSwitch = schematic.addNode(TriggerSwitch, 600, 0);
    schematic.connect({area, areaSwitch, 0});
    schematic.connect({toggle, areaSwitch, 1});
    schematic.connect({x, area, 0});
    schematic.connect({width, area, 2});
    check(schematic.outputType(areaSwitch) == Type::Area &&
          schematic.readValue(areaSwitch) == Value{openstone::yellow::Area{-2.5F, 0, -12, 0}} &&
          schematic.inspectOutput(areaSwitch).triggers == 2,
          "Floating grid coordinates and signed sizes should survive a Yellow link and its pulses");
}

void yellowShapesFollowViewWiring()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    using openstone::yellow::DrawCommand;
    using openstone::yellow::LineStyle;
    using openstone::yellow::PenStroke;
    using openstone::yellow::ShapeKind;
    using openstone::yellow::SolidBrush;
    using openstone::yellow::View;
    Schematic root;
    const int module = root.addNode(Module, 0, 0);
    auto& graph = *root.module(module);
    const int mgui = graph.addNode(ModuleGui, 0, 0);
    const int arc = graph.addNode(Arc, 200, 0);
    const int ellipse = graph.addNode(Ellipse, 200, 100);
    const int line = graph.addNode(Line, 400, 100);
    const int shapeArea = graph.addNode(String, 0, 100, std::string{"2,3,8,5"});
    const int lineArea = graph.addNode(String, 0, 200, std::string{"10,3,-8,0"});
    const int start = graph.addNode(Float, 0, 300, 0.0F);
    const int sweep = graph.addNode(Float, 0, 400, -90.0F);
    const int pen = graph.addNode(Pen, 0, 500);
    const int solid = graph.addNode(Solid, 0, 600);
    const int dash = graph.addNode(String, 0, 700, std::string{"dash"});
    const int color = graph.addNode(Color, 0, 800, openstone::yellow::Color{0x8844AACC});
    graph.connect({dash, pen, 2});
    graph.connect({color, pen, 0});
    graph.connect({color, solid, 0});
    graph.connect({shapeArea, arc, 1});
    graph.connect({shapeArea, ellipse, 1});
    graph.connect({lineArea, line, 1});
    graph.connect({pen, arc, 2});
    graph.connect({solid, ellipse, 2});
    graph.connect({pen, line, 2});
    graph.connect({start, arc, 3});
    graph.connect({sweep, arc, 4});
    const auto view = graph.frontPanel()->view;
    check(root.panelDrawings(view).empty(), "A drawing needs a connected View");
    graph.connect({mgui, ellipse, 0});
    graph.connect({mgui, arc, 0});
    graph.connect({ellipse, line, 0});
    check(graph.outputType(line) == Type::View && std::get<View>(graph.readValue(line)).isSamePanel(view),
          "The View output should route to the original panel");

    auto drawings = root.panelDrawings(view);
    const auto shapeAt = [&](std::size_t index) -> const DrawCommand& {
        return std::get<DrawCommand>(drawings.at(index).command);
    };
    check(drawings.size() == 3 && shapeAt(0).shape == ShapeKind::Ellipse &&
          shapeAt(1).shape == ShapeKind::Line && shapeAt(2).shape == ShapeKind::Arc,
          "A chained line should paint before the next branch, regardless of node or link creation time");
    check(std::get<SolidBrush>(shapeAt(0).tool).color.argb == 0x8844AACC &&
          shapeAt(1).area == openstone::yellow::Area{10, 3, -8, 0} &&
          std::get<PenStroke>(shapeAt(1).tool).style == LineStyle::Dash &&
          shapeAt(2).sweepAngle == -90,
          "The draw commands should retain alpha, dash style, signed line size, and arc direction");

    const auto previous = graph.inspectOutput(arc).triggers;
    graph.setValue(sweep, 135.0F);
    check(std::get<DrawCommand>(root.panelDrawings(view).back().command).sweepAngle == 135 &&
          graph.inspectOutput(arc).triggers == previous + 1,
          "Changing the sweep should update the drawing and pass the value change along the View chain");
    graph.disconnectInput(ellipse, 0);
    drawings = root.panelDrawings(view);
    check(drawings.size() == 1 && shapeAt(0).shape == ShapeKind::Arc,
          "Disconnecting the first shape should remove its downstream drawings too");

    const int childModule = graph.addNode(Module, 600, 0);
    auto& child = *graph.module(childModule);
    const int port = child.addNode(ModuleInput, 0, 0);
    const int childLine = child.addNode(Line, 200, 0);
    const int childArea = child.addNode(String, 0, 100, std::string{"2,2,3,1"});
    child.connect({port, childLine, 0});
    child.connect({childArea, childLine, 1});
    graph.connect({arc, childModule, 0});
    drawings = root.panelDrawings(view);
    check(drawings.size() == 2 && shapeAt(1).shape == ShapeKind::Line,
          "A View flowing through a module connector should still draw on the source panel in chain order");
}

void yellowPieRectAndTextShareTheViewChain()
{
    using enum PrimitiveKind;
    using openstone::yellow::DrawCommand;
    using openstone::yellow::ShapeKind;
    using openstone::yellow::SolidBrush;
    using openstone::yellow::TextCommand;
    using openstone::yellow::View;
    Schematic root;
    auto& graph = *root.module(root.addNode(Module, 0, 0));
    const int mgui = graph.addNode(ModuleGui, 0, 0);
    const int rect = graph.addNode(Rect, 100, 0);
    const int pie = graph.addNode(Pie, 200, 0);
    const int text = graph.addNode(Text, 300, 0);
    const int area = graph.addNode(String, 0, 100, std::string{"2,3,8,6"});
    const int color = graph.addNode(Color, 100, 100, openstone::yellow::Color{0xAABBCCDD});
    const int brush = graph.addNode(Solid, 200, 100);
    const int angles = graph.addNode(Float, 300, 100, 90.0F);
    const int corner = graph.addNode(Float, 400, 100, 9.0F);
    const int message = graph.addNode(String, 0, 200, std::string{"First\nSecond"});
    const int font = graph.addNode(Font, 100, 200);
    const int size = graph.addNode(Float, 200, 200, 1.5F);
    const int format = graph.addNode(StringFormat, 300, 200);
    const int flags = graph.addNode(String, 400, 200, std::string{"vertical nowrap righttoleft"});
    graph.connect({mgui, rect, 0});
    graph.connect({rect, pie, 0});
    graph.connect({pie, text, 0});
    for (const int drawing : {rect, pie, text}) {
        graph.connect({area, drawing, 1});
    }
    graph.connect({color, brush, 0});
    graph.connect({brush, rect, 2});
    graph.connect({brush, pie, 2});
    graph.connect({corner, rect, 3});
    graph.connect({angles, pie, 3});
    graph.connect({angles, pie, 4});
    graph.connect({color, text, 2});
    graph.connect({message, text, 3});
    graph.connect({size, font, 1});
    graph.connect({font, text, 4});
    graph.connect({flags, format, 0});
    graph.connect({format, text, 5});

    const auto view = graph.frontPanel()->view;
    check(std::get<View>(graph.readValue(text)).isSamePanel(view),
          "Text should pass the same drawing target through its View output");
    auto drawings = graph.panelDrawings(view);
    check(drawings.size() == 3, "All three commands should draw on the same panel");
    const auto& rectangle = std::get<DrawCommand>(drawings.at(0).command);
    const auto& sector = std::get<DrawCommand>(drawings.at(1).command);
    const auto& label = std::get<TextCommand>(drawings.at(2).command);
    check(rectangle.shape == ShapeKind::Rect && rectangle.cornerRadius == 3.0F &&
          sector.shape == ShapeKind::Pie && sector.startAngle == 90 && sector.sweepAngle == 90 &&
          std::get<SolidBrush>(sector.tool).color.argb == 0xAABBCCDD,
          "The rectangle should clamp its radius and the pie should retain both angles and brush color");
    check(label.color.argb == 0xAABBCCDD && label.text == "First\nSecond" && label.font.size == 1.5F &&
          label.format.verticalText && label.format.rightToLeft && !label.format.wrap,
          "The Text command should retain its content, color, font, and formatting");

    graph.setValue(angles, 0.0F);
    graph.setValue(message, std::string{"Updated"});
    drawings = graph.panelDrawings(view);
    check(drawings.size() == 2 && std::get<TextCommand>(drawings.at(1).command).text == "Updated",
          "A zero sweep should hide the pie while preserving the View route to live text");
}

void yellowBitmapDrawingCropsTransformsAndKeepsViewOrder()
{
    using enum PrimitiveKind;
    using openstone::yellow::BitmapDrawCommand;
    using openstone::yellow::DrawCommand;
    using openstone::yellow::View;
    namespace ui = openstone::yellow;
    const ui::Bitmap pixels(4, 1, {255, 0, 0, 255, 128, 0, 0, 0, 32, 64, 128, 128, 255, 255, 255, 255});
    Schematic root([&](std::string_view) { return pixels; });
    auto& graph = *root.module(root.addNode(Module, 0, 0));
    const int mgui = graph.addNode(ModuleGui, 0, 0);
    const int box = graph.addNode(Rect, 100, 0);
    const int crop = graph.addNode(BitmapArea, 200, 0);
    const int draw = graph.addNode(BitmapDraw, 300, 0);
    const int transform = graph.addNode(BitmapDrawTransform, 400, 0);
    const int image = graph.addNode(Bitmap, 0, 100);
    const int source = graph.addNode(String, 100, 100, std::string{"-1,0,3,1"});
    const int area = graph.addNode(String, 200, 100, std::string{"2,3,9,6"});
    const int alpha = graph.addNode(Int, 300, 100, std::int32_t{0});
    const int matrix = graph.addNode(ColorMatrixSet, 400, 100);
    const int row = graph.addNode(Int, 500, 100, std::int32_t{4});
    const int col = graph.addNode(Int, 600, 100, std::int32_t{3});
    const int bias = graph.addNode(Float, 700, 100, 0.5F);

    graph.connect({mgui, box, 0});
    graph.connect({box, crop, 0});
    graph.connect({crop, draw, 0});
    graph.connect({draw, transform, 0});
    graph.connect({area, box, 1});
    graph.connect({area, crop, 3});
    graph.connect({area, draw, 1});
    graph.connect({area, transform, 1});
    graph.connect({image, crop, 1});
    graph.connect({image, draw, 2});
    graph.connect({image, transform, 2});
    graph.connect({source, crop, 2});
    graph.connect({row, matrix, 1});
    graph.connect({col, matrix, 2});
    graph.connect({bias, matrix, 3});
    graph.connect({matrix, transform, 3});

    const auto view = graph.frontPanel()->view;
    check(graph.panelDrawings(view).size() == 1 && std::get<View>(graph.readValue(transform)).isSamePanel(view),
          "Empty bitmap nodes should forward the View without drawing");
    graph.loadBitmap(image, "test.png");
    auto drawings = graph.panelDrawings(view);
    check(drawings.size() == 4 && std::holds_alternative<DrawCommand>(drawings.at(0).command),
          "Loading the bitmap should make all three bitmap commands paint after the shape");
    const auto& cropCommand = std::get<BitmapDrawCommand>(drawings.at(1).command);
    const auto& drawCommand = std::get<BitmapDrawCommand>(drawings.at(2).command);
    const auto transformCommand = std::get<BitmapDrawCommand>(drawings.at(3).command);
    check(cropCommand.sourcePixels == ui::Area{0, 0, 2, 1} && cropCommand.area == ui::Area{5, 3, 6, 6} &&
          cropCommand.matrix == ui::ColorMatrix{},
          "Clipping the source should clip the matching region of the destination without rescaling the crop");
    check(drawCommand.sourcePixels == ui::Area{0, 0, 4, 1} && drawCommand.area == ui::Area{2, 3, 9, 6} &&
          drawCommand.alpha == 255 && drawCommand.bitmap.rgba().data() == pixels.rgba().data(),
          "Bitmap Draw should use the whole image, share its storage, and default to full opacity");
    const auto tinted = ui::applyBitmapColorMatrix(transformCommand.matrix, transformCommand.bitmap);
    check(tinted.width() == 4 && tinted.height() == 1 &&
          tinted.rgba()[3] == 255 && tinted.rgba()[4] == 128 && tinted.rgba()[7] == 128 && tinted.rgba()[11] == 255 &&
          pixels.rgba()[7] == 0,
          "The matrix should preserve straight RGB and clamp the new alpha without touching the original image");

    const auto previous = graph.inspectOutput(transform).triggers;
    graph.connect({alpha, draw, 3});
    const auto drawAlpha = [&] {
        return std::get<BitmapDrawCommand>(graph.panelDrawings(view).at(2).command).alpha;
    };
    check(drawAlpha() == 0, "A wired zero alpha should make the bitmap invisible");
    graph.setValue(alpha, std::int32_t{127});
    check(drawAlpha() == 127 && graph.inspectOutput(transform).triggers > previous,
          "An alpha change should update the command and pass a pulse to the next View node");
    graph.setValue(alpha, std::int32_t{300});
    check(drawAlpha() == 255, "Alpha above 255 should clamp to 255");
    graph.setValue(alpha, std::int32_t{-1});
    check(drawAlpha() == 0, "Negative alpha should clamp to zero");

    graph.connect({matrix, crop, 4});
    graph.setValue(source, std::string{"1,0,2,1"});
    drawings = graph.panelDrawings(view);
    const auto& updatedCrop = std::get<BitmapDrawCommand>(drawings.at(1).command);
    check(updatedCrop.sourcePixels == ui::Area{1, 0, 2, 1} && updatedCrop.area == ui::Area{2, 3, 9, 6} &&
          updatedCrop.matrix == transformCommand.matrix,
          "Changing the pixel crop and connecting Matrix should update the current draw command");
    graph.setValue(source, std::string{"5,0,3,1"});
    check(graph.panelDrawings(view).size() == 3 && std::get<View>(graph.readValue(transform)).isSamePanel(view),
          "An out-of-bounds crop should leave the rest of the View chain active");
}

void yellowGraphsPlotLiveSamplesOnTheViewChain()
{
    using enum PrimitiveKind;
    using openstone::yellow::GraphDrawCommand;
    using openstone::yellow::GraphStyle;
    using openstone::yellow::SolidBrush;
    using openstone::yellow::View;
    Schematic root;
    auto& graph = *root.module(root.addNode(Module, 0, 0));
    const int mgui = graph.addNode(ModuleGui, 0, 0);
    const int dots = graph.addNode(GraphDots, 100, 0);
    const int line = graph.addNode(LineGraph, 200, 0);
    const int area = graph.addNode(String, 0, 100, std::string{"2,3,8,6"});
    const int data = graph.addNode(String, 100, 100, std::string{"1\n0\nnan\n-1\n0"});
    const int brush = graph.addNode(Solid, 200, 100);
    const int logX = graph.addNode(Boolean, 300, 100, false);
    const int curve = graph.addNode(Boolean, 400, 100, false);
    const int top = graph.addNode(Float, 500, 100, 3.0F);
    const int bottom = graph.addNode(Float, 600, 100, -3.0F);
    const int radius = graph.addNode(Float, 700, 100, 0.5F);
    graph.connect({mgui, dots, 0});
    graph.connect({dots, line, 0});
    for (const int plot : {dots, line}) {
        graph.connect({area, plot, 1});
        graph.connect({data, plot, 3});
        graph.connect({logX, plot, 6});
    }
    graph.connect({brush, dots, 2});
    graph.connect({radius, dots, 7});
    graph.connect({curve, line, 7});

    const auto view = graph.frontPanel()->view;
    auto drawings = graph.panelDrawings(view);
    check(drawings.size() == 2, "Both graphs should draw on the same panel");
    const auto& points = std::get<GraphDrawCommand>(drawings.front().command);
    const auto& trace = std::get<GraphDrawCommand>(drawings.back().command);
    check(points.style == GraphStyle::Dots && points.radius == 0.5F &&
          std::holds_alternative<SolidBrush>(points.tool) && trace.style == GraphStyle::Lines,
          "GraphDots should preserve the brush and radius and paint before LineGraph");
    const auto& samples = trace.vertices;
    check(samples.size() == 4 &&
          samples[0].x == 0 && samples[0].y == 0 && samples[0].startsRun &&
          samples[1].x == 2 && samples[1].y == 3 && !samples[1].startsRun &&
          samples[2].x == 6 && samples[2].y == 6 && samples[2].startsRun &&
          samples[3].x == 8 && samples[3].y == 3 && !samples[3].startsRun,
          "Samples should map to the default range, leaving a gap at the missing sample index");

    const auto previous = graph.inspectOutput(line).triggers;
    graph.setValue(logX, true);
    graph.setValue(curve, true);
    graph.connect({top, line, 4});
    graph.connect({bottom, line, 5});
    drawings = graph.panelDrawings(view);
    const auto& smooth = std::get<GraphDrawCommand>(drawings.back().command);
    const auto& loggedDots = std::get<GraphDrawCommand>(drawings.front().command);
    check(smooth.style == GraphStyle::Curve && smooth.vertices.front().y == 2 && smooth.vertices[2].y == 4 &&
          smooth.vertices.front().x == 0 && smooth.vertices.back().x == 8 &&
          std::abs(smooth.vertices[1].x - 3.445412464587145) < 0.000001 &&
          loggedDots.vertices[1].x == smooth.vertices[1].x,
          "Log X should expand early samples for both graphs while custom Y bounds rescale LineGraph");
    check(graph.inspectOutput(line).triggers > previous && std::get<View>(graph.readValue(line)).isSamePanel(view),
          "Changing graph settings should propagate a trigger and preserve the View");
    graph.setValue(data, std::string{"0.5"});
    drawings = graph.panelDrawings(view);
    check(drawings.size() == 1 && std::get<GraphDrawCommand>(drawings.front().command).vertices.front().x == 4,
          "A single sample should draw a centered dot and no line");
    graph.setValue(data, std::string{"1\n0"});
    graph.setValue(top, -3.0F);
    check(graph.panelDrawings(view).size() == 1 && std::get<View>(graph.readValue(line)).isSamePanel(view),
          "An equal top and bottom should skip the line without severing the chain");
    graph.setValue(radius, 0.0F);
    check(graph.panelDrawings(view).empty(), "A zero radius should skip dots");
}

void yellowDotsUseExplicitCoordinatesAndAxisBounds()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    using openstone::yellow::GraphDrawCommand;
    using openstone::yellow::GraphStyle;
    using openstone::yellow::View;
    Schematic root;
    auto& graph = *root.module(root.addNode(Module, 0, 0));
    const int mgui = graph.addNode(ModuleGui, 0, 0);
    const int dots = graph.addNode(Dots, 100, 0);
    const int area = graph.addNode(String, 0, 100, std::string{"2,3,8,6"});
    const int data = graph.addNode(String, 100, 100, std::string{"-0.5,0.5\n0.5,-0.5\n-0.5,-1\nnan,0"});
    const int left = graph.addNode(Float, 200, 100, 4.0F);
    const int right = graph.addNode(Float, 300, 100, 0.0F);
    const int top = graph.addNode(Float, 400, 100, 6.0F);
    const int bottom = graph.addNode(Float, 500, 100, 0.0F);
    graph.connect({mgui, dots, 0});
    graph.connect({area, dots, 1});
    graph.connect({data, dots, 3});

    const auto view = graph.frontPanel()->view;
    const auto initial = graph.panelDrawings(view);
    check(graph.inputType(dots, 3) == Type::PointArray && initial.size() == 1,
          "Dots should accept a Point Array and draw on the wired View");
    const auto& plot = std::get<GraphDrawCommand>(initial.front().command);
    check(plot.style == GraphStyle::Dots && plot.radius == 0.15F && plot.vertices.size() == 3 &&
          plot.vertices[0].x == 2 && plot.vertices[0].y == 1.5 &&
          plot.vertices[1].x == 6 && plot.vertices[1].y == 4.5 &&
          plot.vertices[2].x == 2 && plot.vertices[2].y == 6,
          "Dots should preserve repeated X coordinates, map the default bounds, and skip non-finite points");

    graph.connect({left, dots, 4});
    graph.connect({right, dots, 5});
    graph.connect({top, dots, 6});
    graph.connect({bottom, dots, 7});
    const auto pulses = graph.inspectOutput(dots).triggers;
    graph.setValue(data, std::string{"1,2"});
    const auto updated = graph.panelDrawings(view);
    const auto& point = std::get<GraphDrawCommand>(updated.at(0).command).vertices.at(0);
    check(point.x == 6 && point.y == 4 && graph.inspectOutput(dots).triggers > pulses,
          "Custom bounds should rescale live data and reversed X bounds should flip its position");
    graph.setValue(right, 4.0F);
    check(graph.panelDrawings(view).empty() && std::get<View>(graph.readValue(dots)).isSamePanel(view),
          "Equal X bounds should skip the dots and keep the View connected");
}

void yellowPointLinesFollowArrayOrderAcrossVerticalSegments()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    using openstone::yellow::GraphDrawCommand;
    using openstone::yellow::GraphStyle;
    using openstone::yellow::View;
    Schematic root;
    auto& graph = *root.module(root.addNode(Module, 0, 0));
    const int mgui = graph.addNode(ModuleGui, 0, 0);
    const int lines = graph.addNode(DrawPointLines, 100, 0);
    const int area = graph.addNode(String, 0, 100, std::string{"2,3,8,6"});
    const int data = graph.addNode(String, 100, 100, std::string{"0.5,0.5\n0.5,-0.5\n0.5,-0.5\n-0.5,0\nnan,0\n0,0\n0.5,0"});
    const int curve = graph.addNode(Boolean, 200, 100, true);
    const int right = graph.addNode(Float, 300, 100, 2.0F);
    graph.connect({mgui, lines, 0});
    graph.connect({area, lines, 1});
    graph.connect({data, lines, 3});

    const auto view = graph.frontPanel()->view;
    const auto straight = graph.panelDrawings(view);
    check(graph.inputType(lines, 3) == Type::PointArray && graph.inputType(lines, 8) == Type::Boolean &&
          straight.size() == 1, "Draw Point Lines should use the screenshot's Data and Curve ports");
    const auto& plot = std::get<GraphDrawCommand>(straight.front().command);
    const auto& vertices = plot.vertices;
    check(plot.style == GraphStyle::Lines && vertices.size() == 5 &&
          vertices[0].x == 6 && vertices[0].y == 1.5 &&
          vertices[1].x == 6 && vertices[1].y == 4.5 && !vertices[1].startsRun &&
          vertices[2].x == 2 && vertices[2].y == 3 && !vertices[2].startsRun &&
          vertices[3].startsRun && !vertices[4].startsRun,
          "Vertical and reversing segments should keep their order, and a non-finite point should create a gap");

    graph.connect({curve, lines, 8});
    graph.connect({right, lines, 5});
    const auto smoothed = graph.panelDrawings(view);
    const auto& path = std::get<GraphDrawCommand>(smoothed.at(0).command);
    check(path.style == GraphStyle::PointCurve && path.vertices[0].x == 4 && path.vertices[1].x == 4 &&
          path.vertices[2].x < path.vertices[1].x && path.vertices[3].startsRun,
          "Point curves should retain path order with the updated axis bounds");

    graph.setValue(data, std::string{"0,0\n0,0"});
    check(graph.panelDrawings(view).empty() && std::get<View>(graph.readValue(lines)).isSamePanel(view),
          "A path with no nonzero segment should still forward its View");
}

void yellowPolygonClosesAValidContour()
{
    using enum PrimitiveKind;
    using openstone::yellow::GraphDrawCommand;
    using openstone::yellow::GraphStyle;
    using openstone::yellow::SolidBrush;
    using openstone::yellow::View;
    Schematic root;
    auto& graph = *root.module(root.addNode(Module, 0, 0));
    const int mgui = graph.addNode(ModuleGui, 0, 0);
    const int polygon = graph.addNode(DrawPolygon, 100, 0);
    const int area = graph.addNode(String, 0, 100, std::string{"2,3,8,6"});
    const int data = graph.addNode(String, 100, 100, std::string{"-1,1\n1,1\n1,-1\n-1,1"});
    const int brush = graph.addNode(Solid, 200, 100);
    const int curve = graph.addNode(Boolean, 300, 100, true);
    graph.connect({mgui, polygon, 0});
    graph.connect({area, polygon, 1});
    graph.connect({brush, polygon, 2});
    graph.connect({data, polygon, 3});

    const auto view = graph.frontPanel()->view;
    const auto straight = graph.panelDrawings(view);
    check(straight.size() == 1, "Draw Polygon should accept a Solid brush");
    const auto& contour = std::get<GraphDrawCommand>(straight.front().command);
    check(contour.closed && contour.style == GraphStyle::Lines && std::holds_alternative<SolidBrush>(contour.tool) &&
          contour.vertices.size() == 3 && contour.vertices[0].x == 0 && contour.vertices[0].y == 0 &&
          contour.vertices[2].x == 8 && contour.vertices[2].y == 6,
          "A polygon should close without duplicating the starting vertex and use the default axis bounds");

    graph.connect({curve, polygon, 8});
    const auto curved = graph.panelDrawings(view);
    const auto& smoothed = std::get<GraphDrawCommand>(curved.at(0).command);
    check(smoothed.closed && smoothed.style == GraphStyle::PointCurve && smoothed.vertices.size() == 3,
          "Curve should smooth the closed contour");

    graph.setValue(data, std::string{"0,0\n1,1\n0,0\n1,1"});
    check(graph.panelDrawings(view).empty(), "Two distinct vertices are insufficient for a polygon");
    graph.setValue(data, std::string{"0,0\n1,1\nnan,0\n-1,1"});
    check(graph.panelDrawings(view).empty() && std::get<View>(graph.readValue(polygon)).isSamePanel(view),
          "Invalid vertices should skip the polygon without disconnecting the View");
}

void dropListSelectionAndCancellationPulseDifferentOutputs()
{
    using enum PrimitiveKind;
    using openstone::DropListLayout;
    Schematic root;
    auto& graph = *root.module(root.addNode(Module, 0, 0));
    const int mgui = graph.addNode(ModuleGui, 0, 0);
    const int menu = graph.addNode(DropList, 100, 0);
    const int list = graph.addNode(String, 0, 100, std::string{"Alpha,Beta,Alpha"});
    const int trigger = graph.addNode(Trigger, 100, 100);
    const int checkIndex = graph.addNode(Int, 200, 100, std::int32_t{1});
    const int style = graph.addNode(String, 300, 100, std::string{"2"});
    const int x = graph.addNode(Float, 400, 100, 2.5F);
    const int selected = graph.addNode(TriggerCounter, 0, 200);
    const int canceled = graph.addNode(TriggerCounter, 100, 200);
    graph.connect({mgui, menu, 0});
    graph.connect({x, menu, 1});
    graph.connect({list, menu, 3});
    graph.connect({trigger, menu, 4}, LinkNotification::Silent);
    graph.connect({checkIndex, menu, 5});
    graph.connect({style, menu, 6});
    graph.connect({menu, selected, 0, 0}, LinkNotification::Silent);
    graph.connect({menu, canceled, 0, 3}, LinkNotification::Silent);
    check(!root.pendingDropList() && std::get<std::int32_t>(graph.readValue(menu, 2)) == 3 &&
          std::get<std::int32_t>(graph.readValue(menu, 1)) == -1,
          "A list edit should expose its count without opening or selecting");

    graph.fireTrigger(trigger);
    const auto& pending = root.pendingDropList();
    check(pending && pending->view.isSamePanel(graph.frontPanel()->view) && pending->position.x == 2.5F &&
          pending->layout == DropListLayout::FixedColumns && pending->rowsPerColumn == 2 &&
          pending->checkedIndex == 1 && pending->contents.options[1] == "Beta",
          "Open should snapshot the options, check mark, layout, and target View");
    const auto selectionToken = pending->token;
    graph.setValue(list, std::string{"Different,Names"});
    root.finishDropList(selectionToken, 1);
    check(!root.pendingDropList() && std::get<std::string>(graph.readValue(menu, 0)) == "Beta" &&
          std::get<std::int32_t>(graph.readValue(menu, 1)) == 1 && integer(graph, selected) == 1 &&
          graph.inspectOutput(menu, 1).triggers == 1 && integer(graph, canceled) == 0 &&
          std::get<std::int32_t>(graph.readValue(menu, 2)) == 2,
          "Selecting should commit the displayed option, pulse both selection outputs, and keep the live count");

    graph.setValue(style, std::string{"Scroll"});
    graph.fireTrigger(trigger);
    check(root.pendingDropList() && root.pendingDropList()->layout == DropListLayout::Scroll,
          "Scroll should request a single scrolling column");
    root.finishDropList(selectionToken, std::nullopt);
    check(root.pendingDropList().has_value(), "A stale response should leave the current request active");
    root.finishDropList(root.pendingDropList()->token, std::nullopt);
    check(!root.pendingDropList() && integer(graph, selected) == 1 && integer(graph, canceled) == 1 &&
          std::get<std::string>(graph.readValue(menu, 0)) == "Beta",
          "Dismissing the popup should only pulse Canceled and retain the last selection");

    graph.setValue(list, std::string{
        "Item 1, Item 2,>>, Menu 1, <<, Item 3, Item 4, >>, -----, Item 5, >>, Menu 2, <<, Item 6, Item 7"});
    check(std::get<std::int32_t>(graph.readValue(menu, 2)) == 7,
          "Only selectable items should count, including those in submenus");
    graph.fireTrigger(trigger);
    const auto& menus = root.pendingDropList().value().contents.menus;
    check(menus.size() == 3 && menus[0].size() == 6 && menus[0][2].text == "Menu 1" && menus[0][2].submenu == 1 &&
          menus[0][3].optionIndex < 0 && menus[0][3].submenu < 0 && menus[0][4].text == "Item 5" &&
          menus[0][5].text == "Menu 2" && menus[0][5].submenu == 2,
          "The sample should produce two submenus with a separator before Item 5");
    check(menus[1].size() == 2 && menus[1][0].text == "Item 3" && menus[1][1].text == "Item 4" &&
          menus[2].size() == 2 && menus[2][0].text == "Item 6" && menus[2][1].text == "Item 7",
          "Each submenu should contain only its own options with trimmed labels");
    root.finishDropList(root.pendingDropList()->token, menus[2][0].optionIndex);
    check(std::get<std::string>(graph.readValue(menu, 0)) == "Item 6" &&
          std::get<std::int32_t>(graph.readValue(menu, 1)) == 5 && integer(graph, selected) == 2 &&
          integer(graph, canceled) == 1,
          "Selecting in a submenu should report the leaf's index across the whole list");

    graph.setValue(list, std::string{"A, >>, B, <<, >>, C, <<, D, >>, E, >>, F"});
    graph.fireTrigger(trigger);
    const auto& nested = root.pendingDropList().value().contents.menus;
    check(nested.size() == 3 && nested[0].size() == 3 && nested[0][2].text == "F" &&
          nested[1].size() == 2 && nested[1][0].submenu == 2 && nested[1][1].text == "E" &&
          nested[2].size() == 1 && nested[2][0].text == "D",
          "Opening a nested submenu and closing each level should restore the correct parent menu");
    root.finishDropList(root.pendingDropList()->token, std::nullopt);

    graph.setValue(list, std::string{});
    graph.fireTrigger(trigger);
    check(!root.pendingDropList() && std::get<std::int32_t>(graph.readValue(menu, 2)) == 0,
          "An empty list should have zero options and no popup");
}

void mouseMessagesFilterEventsAndDeliverEachMatchingPulse()
{
    using enum PrimitiveKind;
    using openstone::yellow::MouseButton;
    using openstone::yellow::MouseEventKind;
    using openstone::yellow::MouseMessage;
    Schematic root;
    const int module = root.addNode(Module, 0, 0);
    auto& graph = *root.module(module);
    const int view = graph.addNode(ModuleGui, 0, 0);
    const int target = graph.addNode(MouseArea, 0, 0);
    const int area = graph.addNode(Area, 0, 0);
    const int size = graph.addNode(Float, 0, 0, 6.0F);
    graph.connect({size, area, 2});
    graph.connect({size, area, 3});
    graph.connect({area, target, 1});
    check(!graph.sendMouseMessage(target, {MouseEventKind::ButtonDown, {2, 3}, MouseButton::Left}),
          "A Mouse Area with no View cannot publish mouse messages");
    graph.connect({view, target, 0});

    const auto panel = graph.frontPanel()->view;
    const auto drawing = root.panelDrawings(panel).at(0);
    check(std::get<openstone::yellow::MouseAreaCommand>(drawing.command).id.ownerPath == std::vector{module} &&
          graph.outputType(target) == openstone::green::Type::MouseMessage &&
          openstone::primitive(MouseArea).outputs.size() == 1,
          "Rectangular Mouse Area should attach to the View and expose only Mouse Messages");

    const int pass = graph.addNode(TriggerSwitch, 0, 0);
    const int enabled = graph.addNode(Boolean, 0, 0, true);
    graph.connect({target, pass, 0});
    graph.connect({enabled, pass, 1});

    struct Parser {
        PrimitiveKind kind;
        MouseButton button;
        MouseEventKind event;
    };
    const std::array parsers{
        Parser{MouseLDbl, MouseButton::Left, MouseEventKind::DoubleClick},
        Parser{MouseLDown, MouseButton::Left, MouseEventKind::ButtonDown},
        Parser{MouseLUp, MouseButton::Left, MouseEventKind::ButtonUp},
        Parser{MouseRDbl, MouseButton::Right, MouseEventKind::DoubleClick},
        Parser{MouseRDown, MouseButton::Right, MouseEventKind::ButtonDown},
        Parser{MouseRUp, MouseButton::Right, MouseEventKind::ButtonUp},
    };
    std::vector<int> nodes;
    for (const auto& parser : parsers) {
        nodes.push_back(graph.addNode(parser.kind, 0, 0));
        graph.connect({pass, nodes.back(), 0});
    }
    const int wheel = graph.addNode(MouseWheel, 0, 0);
    graph.connect({target, wheel, 0});
    check(!graph.sendMouseMessage(target, {MouseEventKind::ButtonDown, {7, 3}, MouseButton::Left}) &&
          !graph.sendMouseMessage(target, {MouseEventKind::Move, {2, 3}}),
          "Mouse Area should reject presses outside its extent and hover with Mouse disabled");

    for (std::size_t index = 0; index < parsers.size(); ++index) {
        const auto& parser = parsers[index];
        const openstone::green::Point position{2, static_cast<float>(index)};
        const MouseMessage event{parser.event, position, parser.button};
        check(graph.sendMouseMessage(target, event) && graph.sendMouseMessage(target, event),
              "Mouse Area should forward repeated equal messages");
        check(std::get<openstone::green::Point>(graph.readValue(nodes[index])) == position,
              "The matching button parser should store the event position");
    }
    for (const auto id : nodes) {
        check(graph.inspectOutput(id).triggers == 2, "Each parser should pulse only for its own button event");
    }
    check(graph.sendMouseMessage(target, {MouseEventKind::Wheel, {2, 3}, MouseButton::None, {120, -60}}) &&
          graph.sendMouseMessage(target, {MouseEventKind::Wheel, {2, 3}, MouseButton::None, {0, -60}}) &&
          graph.sendMouseMessage(target, {MouseEventKind::Wheel, {2, 3}, MouseButton::None, {120, 0}}) &&
          std::get<float>(graph.readValue(wheel)) == -60 && graph.inspectOutput(wheel).triggers == 2,
          "Wheel should deliver signed repeated vertical deltas and ignore horizontal-only messages");

    graph.sendMouseMessage(target, {MouseEventKind::ButtonUp, {8, 3}, MouseButton::Right});
    check(std::get<openstone::green::Point>(graph.readValue(nodes.back())) == openstone::green::Point{8, 3},
          "A grabbed release may be outside the Mouse Area");
    const int newParser = graph.addNode(MouseRUp, 0, 0);
    graph.connect({target, newParser, 0});
    const int unrelated = graph.addNode(Trigger, 0, 0);
    graph.connect({unrelated, newParser, 0});
    graph.fireTrigger(unrelated);
    check(graph.inspectInput(newParser, 0).triggers == 3 && graph.inspectOutput(newParser).triggers == 0,
          "Link creation and unrelated triggers must not replay the last mouse event");
}

void polygonalMouseAreaTestsTheContourInViewCoordinates()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    using openstone::yellow::MouseAreaCommand;
    using openstone::yellow::MouseButton;
    using openstone::yellow::MouseEventKind;
    using openstone::yellow::MousePolygon;
    Schematic root;
    auto& graph = *root.module(root.addNode(Module, 0, 0));
    const int view = graph.addNode(ModuleGui, 0, 0);
    const int mouse = graph.addNode(PolygonalMouseArea, 0, 0);
    const int points = graph.addNode(String, 0, 0, std::string{"2,1\n12,1\n12,3\n6,3\n6,6\n2,6"});
    const int down = graph.addNode(MouseLDown, 0, 0);
    const int up = graph.addNode(MouseLUp, 0, 0);
    graph.connect({view, mouse, 0});
    graph.connect({mouse, down, 0});
    graph.connect({mouse, up, 0});
    const auto panel = graph.frontPanel()->view;
    check(graph.panelDrawings(panel).empty(), "An empty Point Array should not create a hit region");
    graph.connect({points, mouse, 1});

    const auto drawings = graph.panelDrawings(panel);
    const auto& polygon = std::get<MousePolygon>(std::get<MouseAreaCommand>(drawings.at(0).command).region);
    check(graph.inputType(mouse, 1) == Type::PointArray && graph.outputType(mouse) == Type::MouseMessage &&
          openstone::primitive(PolygonalMouseArea).outputs.size() == 1 &&
          polygon.minimum == openstone::green::Point{2, 1} && polygon.maximum == openstone::green::Point{12, 6},
          "Polygonal Mouse Area should describe its Point Array and expose only Mouse Messages");

    const auto press = [&](openstone::green::Point position) {
        return graph.sendMouseMessage(mouse, {MouseEventKind::ButtonDown, position, MouseButton::Left});
    };
    check(!press({9, 5}) && !press({1, 3}) && graph.inspectOutput(down).triggers == 0,
          "Neither the concave cutout nor positions outside the bounds should send presses");
    check(press({4, 5}) && press({6, 5}) && press({2, 4}) && graph.inspectOutput(down).triggers == 3 &&
          std::get<openstone::green::Point>(graph.readValue(down)) == openstone::green::Point{2, 4},
          "Interior presses and boundary presses should keep View coordinates, including on the closing edge");
    check(graph.sendMouseMessage(mouse, {MouseEventKind::ButtonUp, {9, 5}, MouseButton::Left}) &&
          std::get<openstone::green::Point>(graph.readValue(up)) == openstone::green::Point{9, 5},
          "A release grabbed in the polygon can finish in its cutout");

    graph.setValue(points, std::string{"2,6\n12,6\n2,1\n2,6"});
    check(!press({9, 2}) && press({9, 5.5F}) && graph.inspectOutput(down).triggers == 4,
          "Rewiring points should update the hit region even with reversed winding and a repeated closing vertex");
    for (const std::string invalid : {"2,1\n12,1", "2,1\n4,2\n6,3\n2,1", "2,1\n12,1\n6,6\nnan,0"}) {
        graph.setValue(points, invalid);
        check(graph.panelDrawings(panel).empty() && !press({4, 3}) && graph.inspectOutput(down).triggers == 4,
              "Short, collinear, and non-finite polygons must not capture input");
    }
}

void mouseInfoResolvesCursorNamesAndIds()
{
    using openstone::yellow::MouseCursor;
    using openstone::yellow::parseMouseCursor;

    for (unsigned int id = 0; id <= 23; ++id) {
        check(parseMouseCursor(std::to_string(id)) == static_cast<MouseCursor>(id),
              "Mouse Info should preserve the numbered cursor IDs");
    }
    check(parseMouseCursor("  IBEAM ") == MouseCursor::IBeam &&
          parseMouseCursor("arrow") == MouseCursor::Arrow && parseMouseCursor("pointer") == MouseCursor::Arrow &&
          parseMouseCursor("sizewsne") == MouseCursor::ResizeNorthEastSouthWest &&
          parseMouseCursor("splitsn") == MouseCursor::SplitVertical &&
          parseMouseCursor("none") == MouseCursor::Hidden && parseMouseCursor("hand") == MouseCursor::Hand,
          "Cursor aliases should match the Alpha names");
    check(!parseMouseCursor("") && !parseMouseCursor("unknown") && !parseMouseCursor("24") &&
          !parseMouseCursor("-1") && !parseMouseCursor("12px"),
          "Empty, unknown, and out-of-range cursor strings should use the inherited cursor");
}

void mouseInfoFollowsItsSourceAndRefreshesWithoutPointerEvents()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    using openstone::yellow::MouseAreaCommand;
    using openstone::yellow::MouseCursor;
    using openstone::yellow::MouseHoverInfo;
    Schematic root;
    const int module = root.addNode(Module, 0, 0);
    auto& graph = *root.module(module);
    const int view = graph.addNode(ModuleGui, 0, 0);
    const int rectangle = graph.addNode(MouseArea, 0, 0);
    const int polygon = graph.addNode(PolygonalMouseArea, 0, 0);
    const int area = graph.addNode(String, 0, 0, std::string{"1,1,5,5"});
    const int points = graph.addNode(String, 0, 0, std::string{"1,1\n5,1\n1,5"});
    const int child = graph.addNode(Module, 0, 0);
    auto& listener = *graph.module(child);
    const int input = listener.addNode(ModuleInput, 0, 0);
    const int info = listener.addNode(MouseInfo, 0, 0);
    const int cursor = listener.addNode(Int, 0, 0, std::int32_t{19});
    const int tooltip = listener.addNode(String, 0, 0, std::string{"A tooltip"});
    const int unrelated = listener.addNode(Trigger, 0, 0);
    graph.connect({view, rectangle, 0});
    graph.connect({area, rectangle, 1});
    graph.connect({view, polygon, 0});
    graph.connect({points, polygon, 1});
    listener.connect({unrelated, info, 0});
    listener.connect({input, info, 0});
    listener.connect({cursor, info, 1});
    listener.connect({tooltip, info, 2});
    graph.connect({rectangle, child, 0});
    const auto panel = graph.frontPanel()->view;
    const auto current = [&](int id) -> MouseHoverInfo {
        for (const auto& drawing : root.panelDrawings(panel)) {
            if (const auto* command = std::get_if<MouseAreaCommand>(&drawing.command); command && command->id.node == id) {
                return command->hover;
            }
        }
        throw std::runtime_error("Missing mouse area");
    };

    check(listener.inputType(info, 0) == Type::MouseMessage && listener.inputType(info, 1) == Type::String &&
          listener.inputType(info, 2) == Type::String && openstone::primitive(MouseInfo).outputs.empty(),
          "Mouse Info should have three inputs and no outputs");
    check(!panel.mouseMoves && current(rectangle).cursor == MouseCursor::SplitVertical &&
          current(rectangle).tooltip == "A tooltip" && !current(polygon).cursor && current(polygon).tooltip.empty(),
          "Mouse Info should follow the Mouse Messages wire across module boundaries without needing hover events");
    listener.setValue(cursor, std::int32_t{23});
    listener.setValue(tooltip, std::string{});
    check(current(rectangle).cursor == MouseCursor::Hand && current(rectangle).tooltip.empty(),
          "Cursor and tooltip should refresh when their Green inputs change");

    graph.disconnectInput(child, 0);
    graph.connect({polygon, child, 0});
    listener.setValue(tooltip, std::string{"Polygon only"});
    check(!current(rectangle).cursor && current(rectangle).tooltip.empty() &&
          current(polygon).cursor == MouseCursor::Hand && current(polygon).tooltip == "Polygon only",
          "Rewiring Mouse Info should clear its previous region and attach it to the new one");
    const auto before = graph.inspectOutput(polygon).triggers;
    (void)current(polygon);
    check(graph.inspectOutput(polygon).triggers == before,
          "Reading hover information should not send Mouse Messages");
}

void showCursorSamplesHoldOnlyWhenHiding()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    Schematic root;
    const int module = root.addNode(Module, 0, 0);
    auto& graph = *root.module(module);
    const int control = graph.addNode(ShowCursor, 0, 0);
    const int show = graph.addNode(Boolean, 0, 0, true);
    const int hold = graph.addNode(Boolean, 0, 0, true);
    const int repeat = graph.addNode(Trigger, 0, 0);

    check(graph.inputType(control, 0) == Type::Boolean && graph.inputType(control, 1) == Type::Boolean &&
          openstone::primitive(ShowCursor).outputs.empty() &&
          std::get<bool>(*graph.inspectInput(control, 0).value) && !root.cursorVisibility().hidden,
          "Show Cursor should accept two Booleans and default to visible");
    graph.connect({hold, control, 1});
    graph.connect({repeat, control, 0});
    graph.connect({show, control, 0});
    check(root.cursorVisibility().revision == 0, "Visible Show and changes to Hold should leave the cursor visible");

    graph.setValue(show, false);
    const auto hidden = root.cursorVisibility();
    check(hidden.hidden && hidden.hold && graph.cursorVisibility().revision == hidden.revision,
          "A Show pulse in a nested module should hide the cursor for the whole schematic and sample Hold");
    graph.setValue(hold, false);
    graph.fireTrigger(repeat);
    check(root.cursorVisibility().hidden && root.cursorVisibility().hold &&
          root.cursorVisibility().revision == hidden.revision,
          "Changing Hold and repeating the hide pulse should preserve the current hide interval");

    graph.setValue(show, true);
    check(!root.cursorVisibility().hidden && root.cursorVisibility().revision == hidden.revision + 1,
          "Showing the cursor should end the hide interval");
    graph.setValue(show, false);
    const auto unheld = root.cursorVisibility();
    check(unheld.hidden && !unheld.hold, "A new hide interval should sample the current Hold value");

    graph.disconnectInput(control, 0);
    check(!root.cursorVisibility().hidden && root.cursorVisibility().revision == unheld.revision,
          "Disconnecting Show should release the cursor without emitting an explicit show transition");
    graph.connect({show, control, 0}, openstone::LinkNotification::Silent);
    check(!root.cursorVisibility().hidden, "A silent reconnect should leave the cursor visible");
    graph.connect({repeat, control, 0});
    check(root.cursorVisibility().hidden, "A pulse should sample the reconnected Show input");
    root.removeNode(module);
    check(!root.cursorVisibility().hidden, "Removing the owning module should release the cursor");
}

void mouseDragPublishesCapturedPointsAndDragState()
{
    using enum PrimitiveKind;
    using openstone::MouseDragEffects;
    using openstone::green::Type;
    using openstone::yellow::MouseButton;
    using openstone::yellow::MouseCursor;
    using openstone::yellow::MouseEventKind;
    using Position = openstone::green::Point;
    Schematic root;
    const int module = root.addNode(Module, 0, 0);
    auto& graph = *root.module(module);
    const int view = graph.addNode(ModuleGui, 0, 0);
    const int area = graph.addNode(String, 0, 0, std::string{"1,2,4,4"});
    const int mouse = graph.addNode(MouseArea, 0, 0);
    const int other = graph.addNode(MouseArea, 0, 0);
    const int drag = graph.addNode(MouseDrag, 0, 0);
    const int gate = graph.addNode(TriggerSwitch, 0, 0);
    const int open = graph.addNode(Boolean, 0, 0, true);
    const int cursor = graph.addNode(Int, 0, 0, std::int32_t{14});
    const int reset = graph.addNode(Boolean, 0, 0, true);
    for (const int source : {mouse, other}) {
        graph.connect({view, source, 0});
        graph.connect({area, source, 1});
    }
    graph.connect({mouse, gate, 0});
    graph.connect({open, gate, 1});
    graph.connect({gate, drag, 0});
    graph.connect({cursor, drag, 1});
    graph.connect({reset, drag, 2});

    const auto point = [&](int pin) { return std::get<Position>(graph.readValue(drag, pin)); };
    const auto pulses = [&](int pin) { return graph.inspectOutput(drag, pin).triggers; };
    const auto dragging = [&] { return std::get<bool>(graph.readValue(drag, 3)); };
    MouseDragEffects effects;
    const auto send = [&](MouseEventKind kind, Position point, MouseButton button = MouseButton::Left) {
        return graph.sendMouseMessage(mouse, {kind, point, button}, &effects);
    };
    check(graph.inputType(drag, 0) == Type::MouseMessage && graph.inputType(drag, 1) == Type::String &&
          graph.inputType(drag, 2) == Type::Boolean && graph.outputType(drag, 2) == Type::Point &&
          graph.outputType(drag, 3) == Type::Boolean && !dragging() && pulses(3) == 0,
          "Mouse Drag should expose the requested connectors and begin at rest");
    check(send(MouseEventKind::ButtonDown, {2, 3}, MouseButton::Right) && !dragging() && !effects.started &&
          send(MouseEventKind::Drag, {4, 5}) && !dragging() && pulses(1) == 0,
          "Other mouse buttons and uncaptured motion should not start a drag");

    check(send(MouseEventKind::ButtonDown, {2, 3}) && dragging() && effects.started &&
          effects.cursor == MouseCursor::Hidden && point(0) == Position{2, 3} && point(1) == Position{2, 3} &&
          pulses(0) == 1 && pulses(1) == 0 && pulses(3) == 1,
          "Press should capture and publish Down and Dragging with the configured cursor");
    check(send(MouseEventKind::Drag, {9, 10}) && dragging() && point(1) == Position{9, 10} && pulses(1) == 1,
          "Movement outside the Mouse Area should still publish View coordinates");
    graph.sendMouseMessage(other, {MouseEventKind::ButtonUp, {2, 3}, MouseButton::Left});
    check(dragging() && pulses(2) == 0, "Release from another area must not end the capture");
    check(send(MouseEventKind::ButtonUp, {10, 11}) && !dragging() && effects.resetPointer &&
          pulses(2) == 1 && pulses(3) == 2 && point(2) == Position{10, 11} && point(1) == Position{9, 10},
          "Release should publish the actual Up position and request a pointer reset separately");

    send(MouseEventKind::ButtonDown, {3, 4});
    check(root.cancelMouseDrag({{module}, mouse}) && !dragging() && pulses(3) == 4 && pulses(2) == 1 &&
          !root.cancelMouseDrag({{module}, mouse}),
          "Cancellation should publish Dragging=false once without synthesizing Up");
    send(MouseEventKind::ButtonDown, {3, 4});
    graph.setValue(open, false);
    check(send(MouseEventKind::ButtonUp, {10, 11}) && !dragging() && !effects.resetPointer && pulses(2) == 1 &&
          pulses(3) == 6, "A route that closed during capture should still end Dragging");
}

void mouseDragAccumulateUsesClampedGestureBaselines()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    using MousePoint = openstone::green::Point;
    using openstone::MouseDragEffects;
    using openstone::yellow::MouseButton;
    using openstone::yellow::MouseEventKind;

    Schematic root;
    const int module = root.addNode(Module, 0, 0);
    auto& graph = *root.module(module);
    const int view = graph.addNode(ModuleGui, 0, 0);
    const int mouse = graph.addNode(MouseArea, 0, 0);
    const int bounds = graph.addNode(String, 0, 0, std::string{"0,0,12,10"});
    const int drag = graph.addNode(MouseDragAccumulate, 0, 0);
    const int set = graph.addNode(Point, 0, 0);
    const int setX = graph.addNode(Float, 0, 0, 0.5F);
    const int setY = graph.addNode(Float, 0, 0, 0.4F);
    const int scale = graph.addNode(Point, 0, 0);
    const int scaleX = graph.addNode(Float, 0, 0, 0.1F);
    const int scaleY = graph.addNode(Float, 0, 0, -0.2F);
    const int reset = graph.addNode(Boolean, 0, 0, true);

    graph.connect({view, mouse, 0});
    graph.connect({bounds, mouse, 1});
    graph.connect({mouse, drag, 0});
    graph.connect({setX, set, 0});
    graph.connect({setY, set, 1});
    graph.connect({set, drag, 4});
    graph.connect({reset, drag, 2});
    graph.connect({scaleX, scale, 0});
    graph.connect({scaleY, scale, 1});

    const auto point = [&](int output, float x, float y) {
        const auto value = std::get<MousePoint>(graph.readValue(drag, output));
        return std::abs(value.x - x) < 1e-6F && std::abs(value.y - y) < 1e-6F;
    };
    const auto pulses = [&](int output) { return graph.inspectOutput(drag, output).triggers; };
    MouseDragEffects effects;
    const auto send = [&](MouseEventKind kind, MousePoint position) {
        return graph.sendMouseMessage(mouse, {kind, position, MouseButton::Left}, &effects);
    };
    check(graph.inputType(drag, 3) == Type::Point && graph.inputType(drag, 4) == Type::Point &&
          std::get<MousePoint>(*graph.inspectInput(drag, 3).value) == MousePoint{0.05F, 0.05F} &&
          point(0, 0, 0) && point(1, 0.5F, 0.4F) && pulses(1) == 0,
          "Drag Accumulate should have Point inputs, a default scale per axis, and a silent Set");

    check(send(MouseEventKind::ButtonDown, {3, 5}) && effects.started && effects.accumulating &&
          point(0, 0.5F, 0.4F) && pulses(0) == 1 && pulses(1) == 1 &&
          std::get<bool>(graph.readValue(drag, 3)),
          "Press should snapshot the accumulator and publish both Down and Position");
    check(send(MouseEventKind::Drag, {7, 1}) && point(1, 0.7F, 0.2F),
          "Default scale should apply to signed View-local displacement on each axis");
    check(send(MouseEventKind::Drag, {30, -20}) && point(1, 1, 0) &&
          send(MouseEventKind::Drag, {29, -19}) && point(1, 1, 0),
          "Clamping should preserve the gesture baseline when the pointer reverses beyond the limits");
    graph.setValue(setX, 0.2F);
    graph.setValue(setY, 0.8F);
    check(send(MouseEventKind::Drag, {7, 1}) && point(1, 0.7F, 0.2F),
          "Set should be ignored for the duration of the drag");

    graph.connect({scale, drag, 3});
    check(send(MouseEventKind::Drag, {8, 2}) && point(1, 0.7F, 0.2F) &&
          send(MouseEventKind::Drag, {7, 1}) && point(1, 0.6F, 0.4F),
          "Changing Scale should rebase on the next move and allow either axis to invert");
    const auto beforeRecenter = pulses(1);
    root.rebaseMouseDrag({{module}, mouse}, {3, 5});
    check(pulses(1) == beforeRecenter && send(MouseEventKind::Drag, {4, 6}) && point(1, 0.7F, 0.2F),
          "Recentering should silently rebase the captured value at the new pointer position");
    check(send(MouseEventKind::ButtonUp, {10, 8}) && effects.resetPointer && point(2, 0.7F, 0.2F) &&
          point(0, 0.5F, 0.4F) && !std::get<bool>(graph.readValue(drag, 3)),
          "Up should publish the final accumulator before desktop pointer reset");
    check(send(MouseEventKind::ButtonDown, {3, 5}) && point(0, 0.7F, 0.2F) &&
          root.cancelMouseDrag({{module}, mouse}) && !std::get<bool>(graph.readValue(drag, 3)) && pulses(2) == 1,
          "A later gesture should continue from the held accumulator and cancel without an Up pulse");

    const auto beforeSet = pulses(1);
    graph.setValue(setX, -2.0F);
    graph.setValue(setY, 2.0F);
    check(point(1, 0, 1) && point(2, 0.7F, 0.2F) && pulses(1) == beforeSet,
          "Idle Set should clamp the current value and leave the Up snapshot and trigger count intact");
}

void dragFilesRequestsAndPublishesNativeOutcomes()
{
    using enum PrimitiveKind;
    using openstone::FileDragMode;
    using openstone::MouseDragEffects;
    using openstone::green::StringArray;
    using openstone::green::Type;
    using openstone::yellow::MouseButton;
    using openstone::yellow::MouseEventKind;

    Schematic root;
    const int module = root.addNode(Module, 0, 0);
    auto& graph = *root.module(module);
    const int view = graph.addNode(ModuleGui, 0, 0);
    const int mouse = graph.addNode(MouseArea, 0, 0);
    const int bounds = graph.addNode(String, 0, 0, std::string{"0,0,12,10"});
    const int drag = graph.addNode(DragFiles, 0, 0);
    const int paths = graph.addNode(String, 0, 0, std::string{"/tmp/first file.png\r\n/tmp/second.txt "});
    const int mode = graph.addNode(String, 0, 0, std::string{"LiNk"});

    graph.connect({view, mouse, 0});
    graph.connect({bounds, mouse, 1});
    graph.connect({mouse, drag, 0});
    graph.connect({paths, drag, 1});

    const auto pulses = [&](int output) { return graph.inspectOutput(drag, output).triggers; };
    const auto flag = [&](int output) { return std::get<bool>(graph.readValue(drag, output)); };
    MouseDragEffects effects;
    const auto send = [&](MouseEventKind kind, MouseButton button) {
        return graph.sendMouseMessage(mouse, {kind, {3, 5}, button}, &effects);
    };
    const auto press = [&] { return send(MouseEventKind::ButtonDown, MouseButton::Left); };

    check(graph.inputType(drag, 0) == Type::MouseMessage && graph.inputType(drag, 1) == Type::StringArray &&
          graph.inputType(drag, 2) == Type::String && graph.outputType(drag, 0) == Type::Boolean &&
          graph.outputType(drag, 1) == Type::Boolean &&
          std::get<std::string>(*graph.inspectInput(drag, 2).value) == "copy" &&
          !flag(0) && !flag(1) && pulses(0) == 0 && pulses(1) == 0,
          "Drag Files should expose the native source inputs and two idle Boolean outputs");
    check(send(MouseEventKind::ButtonDown, MouseButton::Right) && !effects.fileDrag &&
          send(MouseEventKind::Click, MouseButton::Left) && !effects.fileDrag,
          "Only a left-button press should request a file drag");
    check(press() && effects.fileDrag && effects.fileDrag->mode == FileDragMode::Copy &&
          effects.fileDrag->paths == StringArray{"/tmp/first file.png", "/tmp/second.txt "} && !flag(0),
          "The press should snapshot the path list and default mode before the native drag starts");

    const auto copyToken = effects.fileDrag->token;
    check(!root.beginDragFiles(copyToken + 1) && root.beginDragFiles(copyToken) &&
          !root.beginDragFiles(copyToken) && flag(0) && !flag(1) && pulses(0) == 1,
          "The adapter should start the matching request once and publish Dragging");
    check(press() && !effects.fileDrag, "A nested press should leave the current native drag in flight");
    root.finishDragFiles(copyToken + 1, true);
    check(flag(0) && pulses(1) == 0, "An unrelated completion should not interrupt the current drag");
    root.finishDragFiles(copyToken, true);
    check(!flag(0) && flag(1) && pulses(0) == 2 && pulses(1) == 1,
          "An accepted drop should commit both final states and pulse both outputs");

    graph.connect({mode, drag, 2});
    check(press() && effects.fileDrag && effects.fileDrag->mode == FileDragMode::Link && !flag(1) &&
          root.beginDragFiles(effects.fileDrag->token),
          "Modes should be case insensitive, and starting a new attempt should reset the previous result");
    root.finishDragFiles(effects.fileDrag->token, false);
    check(!flag(0) && !flag(1) && pulses(0) == 4 && pulses(1) == 2,
          "A canceled native drag should report false and end Dragging");

    graph.setValue(mode, std::string{"move"});
    check(press() && effects.fileDrag && effects.fileDrag->mode == FileDragMode::Move,
          "Move mode should be carried through the native request");
    root.finishDragFiles(effects.fileDrag->token, false);
    graph.setValue(paths, std::string{});
    check(press() && !effects.fileDrag && pulses(0) == 4 && pulses(1) == 4 && !flag(1),
          "An unusable payload or empty path list should publish failure without starting Dragging");

    graph.setValue(paths, std::string{"/tmp/file.png"});
    check(press() && effects.fileDrag, "A later valid press should request a new drag");
    const auto stale = effects.fileDrag->token;
    graph.removeNode(drag);
    check(!root.beginDragFiles(stale), "Removing the source before execution should discard its native request");
    root.finishDragFiles(stale, true);
}

void viewTransformsComposeInPanelCoordinates()
{
    using enum PrimitiveKind;
    using openstone::green::Value;
    using openstone::yellow::View;
    using MousePoint = openstone::green::Point;
    using PanelArea = openstone::yellow::Area;
    using openstone::yellow::MouseButton;
    using openstone::yellow::MouseEventKind;

    Schematic root;
    auto& graph = *root.module(root.addNode(Module, 0, 0));
    const int panel = graph.addNode(ModuleGui, 0, 0);
    const int section = graph.addNode(ViewSection, 0, 0);
    const int scale = graph.addNode(ViewScale, 0, 0);
    const int size = graph.addNode(ViewSize, 0, 0);
    const int area = graph.addNode(PrimitiveKind::ViewArea, 0, 0);
    const int guiArea = graph.addNode(GuiArea, 0, 0);
    const int ratio = graph.addNode(Float, 0, 0, -2.0F);
    const int offset = graph.addNode(Float, 0, 0, 2.0F);
    const int extent = graph.addNode(Float, 0, 0, 6.0F);
    const int shape = graph.addNode(Rect, 0, 0);
    const int mouse = graph.addNode(MouseArea, 0, 0);
    const int move = graph.addNode(MouseMove, 0, 0);
    const int mouseEnabled = graph.addNode(Boolean, 0, 0, true);
    const int rectangle = graph.addNode(String, 0, 0, std::string{"0,0,8,8"});
    const SchematicLink links[]{
        {panel, section, 0}, {offset, section, 1}, {offset, section, 2},
        {extent, section, 3}, {mouseEnabled, panel, 1}, {section, scale, 0}, {ratio, scale, 1},
        {scale, size, 0}, {scale, area, 0}, {scale, guiArea, 0}, {scale, shape, 0}, {rectangle, shape, 1},
        {scale, mouse, 0}, {rectangle, mouse, 1}, {scale, move, 0},
    };
    for (const auto link : links) {
        graph.connect(link, LinkNotification::Silent);
    }
    const auto base = graph.frontPanel()->view;
    const auto mapped = std::get<View>(graph.readValue(scale));
    check(mapped.isSamePanel(base) && mapped.bounds == PanelArea{0, 0, 3, base.bounds.height / 2} &&
          mapped.mapping.toPanel({1, 2}) == MousePoint{4, 6} && mapped.mapping.fromPanel({4, 6}) == MousePoint{1, 2},
          "Section should translate and inherit an unwired height, and Scale should use the magnitude of its ratio");
    check(graph.readValue(size) == Value{3.0F} && graph.readValue(size, 1) == Value{base.bounds.height / 2} &&
          graph.readValue(area) == Value{mapped.bounds} && graph.readValue(guiArea) == Value{base.bounds},
          "View Size and View Area should read transformed extents independently of GUI Area");

    const auto drawings = graph.panelDrawings(base);
    check(drawings.size() == 2 && drawings.front().mapping == mapped.mapping &&
          std::get<openstone::yellow::DrawCommand>(drawings.front().command).area == PanelArea{0, 0, 8, 8},
          "Drawings should retain their own units and carry the Section and Scale mapping to the panel");
    check(graph.movePanelPointer(panel, {4, 6}) && graph.readValue(move) == Value{MousePoint{1, 2}} &&
          !graph.movePanelPointer(panel, {1, 6}),
          "Mouse Move should report View-local coordinates and ignore positions outside the section");
    check(graph.sendMouseMessage(mouse, {MouseEventKind::ButtonDown, {1, 2}, MouseButton::Left}) &&
          !graph.sendMouseMessage(mouse, {MouseEventKind::ButtonDown, {4, 2}, MouseButton::Left}),
          "Hits in the drawn shape but outside of the clip should be rejected");

    graph.setValue(ratio, 0.0F);
    check(graph.readValue(size) == Value{6.0F} && std::get<View>(graph.readValue(scale)).mapping.scale == 1.0F,
          "A zero ratio should leave the View usable at its previous scale");
}

void windowOwnsAnIndependentViewAndReportsNativeState()
{
    using enum PrimitiveKind;
    using openstone::green::Value;
    using PanelArea = openstone::yellow::Area;
    using openstone::yellow::View;
    using openstone::WindowType;

    Schematic root;
    const int module = root.addNode(Module, 0, 0);
    auto& graph = *root.module(module);
    const auto add = [&](PrimitiveKind kind, std::optional<Value> initial = {}) {
        return graph.addNode(kind, 0, 0, std::move(initial));
    };
    const int window = add(Window);
    const int open = add(Boolean, false);
    const int width = add(Int, std::int32_t{400});
    const int height = add(Int, std::int32_t{240});
    const int mode = add(Int, std::int32_t{1});
    const int title = add(String, std::string{"Mixer"});
    const int viewArea = add(ViewArea);
    const int box = add(Rect);
    const int area = add(String, std::string{"1,1,8,6"});
    const int move = add(MouseMove);
    const int resize = add(SetGuiArea);
    const int resizeTrigger = add(Trigger);
    const int resizeArea = add(String, std::string{"0,0,50,25"});
    const int status = add(TriggerCounter);
    const SchematicLink links[]{
        {open, window, 0}, {width, window, 1}, {height, window, 2}, {mode, window, 3}, {title, window, 4},
        {window, status, 0, 1}, {window, viewArea, 0}, {window, box, 0}, {area, box, 1}, {window, move, 0},
        {window, resize, 0}, {resizeArea, resize, 1}, {resizeTrigger, resize, 2},
    };
    for (const auto link : links) {
        graph.connect(link, LinkNotification::Silent);
    }

    check(root.requestedWindows().empty() && graph.readValue(window, 1) == Value{false},
          "A Window starts closed and allocates no native surface until requested");
    graph.setValue(open, true);
    const auto requested = root.requestedWindows();
    check(requested.size() == 1 && requested[0].width == 400 && requested[0].height == 240 &&
          requested[0].type == WindowType::Resizable && requested[0].title == "Mixer" &&
          requested[0].panel.view.schematicPath == std::vector{module} && !graph.frontPanel(),
          "A nested Window request should carry its own dimensions and stable identity without becoming the module panel");
    const auto view = std::get<View>(graph.readValue(window));
    check(view.bounds == PanelArea{0, 0, 50, 30} && graph.readValue(viewArea) == Value{view.bounds} &&
          root.renderPanel(view).size() == 1 && graph.readValue(window, 1) == Value{false},
          "The Window should expose a drawing surface at eight logical pixels per grid unit before presentation");

    check(graph.reportWindowVisible(window, true) && !graph.reportWindowVisible(window, true) &&
          graph.readValue(window, 1) == Value{true} && integer(graph, status) == 1,
          "An adapter acknowledgement should publish one Open change");
    check(graph.reportWindowSize(window, 512, 320) && graph.readValue(viewArea) == Value{PanelArea{0, 0, 64, 40}},
          "Native client resizing should immediately update the same View");
    graph.setValue(width, std::int32_t{480});
    check(graph.readValue(viewArea) == Value{PanelArea{0, 0, 60, 40}},
          "Changing one size input should preserve the native size along the other axis");
    graph.fireTrigger(resizeTrigger);
    check(graph.readValue(viewArea) == Value{PanelArea{0, 0, 50, 25}} &&
          graph.movePanelPointer(window, {5, 3}),
          "Set GUI Area and mouse move should target the native Window surface");

    check(graph.reportWindowVisible(window, false) && root.requestedWindows().empty() &&
          graph.readValue(window, 1) == Value{false} && integer(graph, status) == 2,
          "Closing the native window should reset Open regardless of the last input value");
    graph.setValue(open, false);
    graph.setValue(open, true);
    check(root.requestedWindows().size() == 1 && root.requestedWindows().front().width == 480 &&
          !std::get<bool>(graph.readValue(window, 1)),
          "Reopening should request a new native window at the configured client size");
    graph.reportWindowVisible(window, true);
    graph.setValue(open, false);
    check(root.requestedWindows().empty() && graph.readValue(window, 1) == Value{false} && integer(graph, status) == 4 &&
          !graph.reportWindowVisible(window, true),
          "A false input should close the window and ignore an obsolete show notification");
}

void viewRotateUsesALocalPivotAndSmoothOnlyPassesView()
{
    using enum PrimitiveKind;
    using openstone::green::Value;
    using Coordinate = openstone::green::Point;
    using openstone::yellow::View;

    Schematic root;
    auto& graph = *root.module(root.addNode(Module, 0, 0));
    const auto add = [&](PrimitiveKind kind, std::optional<Value> initial = {}) {
        return graph.addNode(kind, 0, 0, std::move(initial));
    };
    const int gui = add(ModuleGui);
    const int scale = add(ViewScale);
    const int rotate = add(ViewRotate);
    const int smooth = add(ViewSmooth);
    const int clip = add(Clip);
    const int rectangle = add(Rect);
    const int move = add(MouseMove);
    const int enable = add(Boolean, true);
    const int ratio = add(Float, 2.0F);
    const int pivot = add(Float, 2.0F);
    const int angle = add(Float, 450.0F);
    const int area = add(String, std::string{"1,1,2,2"});
    const SchematicLink links[]{
        {enable, gui, 1}, {gui, scale, 0}, {ratio, scale, 1},
        {scale, rotate, 0}, {pivot, rotate, 1}, {pivot, rotate, 2}, {angle, rotate, 3},
        {rotate, smooth, 0}, {enable, smooth, 1}, {enable, smooth, 2},
        {smooth, clip, 0}, {area, clip, 1}, {clip, rectangle, 0}, {area, rectangle, 1}, {clip, move, 0},
    };
    for (const auto link : links) {
        graph.connect(link, LinkNotification::Silent);
    }
    const auto base = graph.frontPanel()->view;
    const auto scaled = std::get<View>(graph.readValue(scale));
    const auto out = std::get<View>(graph.readValue(smooth));
    check(out.isSamePanel(base) && out.bounds == scaled.bounds &&
          out.mapping.rotation == 90 && out.mapping.toPanel({2, 2}) == Coordinate{4, 4} &&
          out.mapping.toPanel({3, 2}) == Coordinate{4, 6} && out.mapping.fromPanel({4, 6}) == Coordinate{3, 2},
          "Rotate must keep the local pivot fixed under Scale and map positive angles clockwise");
    check(out.mapping == std::get<View>(graph.readValue(rotate)).mapping,
          "Smooth should pass the View mapping through with both flags set");
    check(graph.movePanelPointer(gui, {4, 5}) && graph.readValue(move) == Value{Coordinate{2.5F, 2}},
          "Mouse Move should use the inverse of the drawing transform");

    const auto before = graph.inspectOutput(smooth).triggers;
    graph.setValue(angle, -315.0F);
    const auto drawing = graph.panelDrawings(base);
    check(graph.inspectOutput(smooth).triggers == before + 1 && drawing.size() == 1 &&
          drawing.front().mapping.rotation == 45,
          "A rotation change must keep the drawing routed through Smooth");
    const auto rotated = std::get<View>(graph.readValue(clip)).mapping;
    check(rotated.contains({2, 2}) && !rotated.contains(rotated.fromPanel({1.5F, 1.5F})),
          "A Clip wired after Rotate must follow its geometry instead of admitting the whole bounding box");

    const int compatibility = add(Boolean, false);
    graph.disconnectInput(smooth, 1);
    graph.connect({compatibility, smooth, 1}, LinkNotification::Silent);
    const auto compatibilityCount = graph.inspectOutput(smooth).triggers;
    graph.setValue(compatibility, true);
    check(graph.inspectOutput(smooth).triggers == compatibilityCount,
          "Changing an ignored Smooth flag should leave the output alone");

    graph.setValue(angle, std::numeric_limits<float>::infinity());
    check(std::get<View>(graph.readValue(smooth)).mapping == scaled.mapping,
          "A non-finite angle should leave the incoming View usable");
}

void viewLoopDrawsEachStepInLayerOrder()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    using openstone::yellow::DrawCommand;
    using openstone::yellow::ShapeKind;

    Schematic root;
    auto& graph = *root.module(root.addNode(Module, 0, 0));
    const int gui = graph.addNode(ModuleGui, 0, 0);
    const int loop = graph.addNode(ViewLoop, 0, 0);
    const int start = graph.addNode(Int, 0, 0, std::int32_t{2});
    const int end = graph.addNode(Int, 0, 0, std::int32_t{5});
    const int step = graph.addNode(Int, 0, 0, std::int32_t{-2});
    const int x = graph.addNode(Area, 0, 0);
    const int extent = graph.addNode(Int, 0, 0, std::int32_t{1});
    const int marker = graph.addNode(Rect, 0, 0);
    const int top = graph.addNode(Ellipse, 0, 0);
    const int outside = graph.addNode(Rect, 0, 0);
    const int outsideArea = graph.addNode(String, 0, 0, std::string{"10,0,1,1"});
    const int steps = graph.addNode(TriggerCounter, 0, 0);
    const int finishes = graph.addNode(TriggerCounter, 0, 0);
    const SchematicLink links[]{
        {gui, loop, 0}, {gui, outside, 0}, {outsideArea, outside, 1},
        {start, loop, 1}, {end, loop, 3}, {loop, x, 0, 1}, {steps, x, 1},
        {extent, x, 2}, {extent, x, 3}, {loop, marker, 0}, {loop, top, 0},
        {x, marker, 1}, {x, top, 1}, {loop, steps, 0, 3}, {loop, finishes, 0, 4},
    };
    for (auto link : links) {
        graph.connect(link, LinkNotification::Silent);
    }
    const auto view = graph.frontPanel()->view;
    check(graph.outputType(loop, 1) == Type::Int && graph.inputType(loop, 2) == Type::Int,
          "Start, Step, End, and Value should infer the same type");
    check(graph.panelDrawings(view).size() == 7 && integer(graph, steps) == 0,
          "Inspecting the panel should plan repeated drawing without firing triggers");
    const auto drawings = graph.renderPanel(view);
    check(drawings.size() == 7 && integer(graph, steps) == 3 && integer(graph, finishes) == 1 &&
          std::get<std::int32_t>(graph.readValue(loop, 1)) == 4 &&
          std::get<std::int32_t>(graph.readValue(loop, 2)) == 2,
          "The pass should exclude End and publish one Step per iteration and one Finished");
    for (int i = 0; i < 3; ++i) {
        const auto& first = std::get<DrawCommand>(drawings.at(i * 2).command);
        const auto& second = std::get<DrawCommand>(drawings.at(i * 2 + 1).command);
        check(first.shape == ShapeKind::Rect && second.shape == ShapeKind::Ellipse &&
              first.area.x == 2 + i && first.area.y == 1 + i && first.area == second.area,
              "Each iteration should draw every downstream node in order using the current Value and Step effects");
    }
    check(std::get<DrawCommand>(drawings.back().command).area.x == 10, "A sibling branch should draw once");

    graph.connect({step, loop, 2}, LinkNotification::Silent);
    graph.setValue(end, std::int32_t{-3});
    const auto descending = graph.renderPanel(view);
    check(descending.size() == 7 && std::get<DrawCommand>(descending[4].command).area.x == -2,
          "A negative Int step should draw the descending range");
    graph.setValue(step, std::int32_t{0});
    check(graph.renderPanel(view).size() == 1 && integer(graph, steps) == 6,
          "A zero step should leave the branch empty");

    graph.setValue(start, std::numeric_limits<std::int32_t>::max() - 1);
    graph.setValue(end, std::numeric_limits<std::int32_t>::max());
    graph.setValue(step, std::numeric_limits<std::int32_t>::max());
    check(graph.renderPanel(view).size() == 3 && integer(graph, steps) == 7,
          "Overshooting End at the Int limit should complete without wrapping");
}

void viewCacheReusesDrawingUntilInvalidated()
{
    using enum PrimitiveKind;
    using openstone::yellow::CachedDrawCommand;
    using openstone::yellow::DrawCommand;

    Schematic root;
    const int module = root.addNode(Module, 0, 0);
    auto& graph = *root.module(module);
    const int gui = graph.addNode(ModuleGui, 0, 0);
    const int cache = graph.addNode(ViewCache, 0, 0);
    const int clear = graph.addNode(Trigger, 0, 0);
    const int loop = graph.addNode(ViewLoop, 0, 0);
    const int end = graph.addNode(Int, 0, 0, std::int32_t{2});
    const int steps = graph.addNode(TriggerCounter, 0, 0);
    const int cachedRect = graph.addNode(Rect, 0, 0);
    const int sibling = graph.addNode(Rect, 0, 0);
    const int mouse = graph.addNode(MouseArea, 0, 0);
    const int area = graph.addNode(String, 0, 0, std::string{"1,1,1,1"});

    const SchematicLink links[]{
        {gui, cache, 0}, {clear, cache, 1}, {cache, loop, 0}, {end, loop, 3}, {loop, steps, 0, 3},
        {loop, cachedRect, 0}, {area, cachedRect, 1}, {gui, sibling, 0}, {area, sibling, 1}, {cache, mouse, 0}, {area, mouse, 1},
    };
    for (auto link : links) {
        graph.connect(link, LinkNotification::Silent);
    }
    auto view = graph.frontPanel()->view;
    const auto cached = [](const auto& drawings) {
        return std::get<CachedDrawCommand>(drawings.at(0).command);
    };
    const auto cachedX = [&](const auto& drawings) {
        return std::get<DrawCommand>(cached(drawings).drawings->at(0).command).area.x;
    };

    check(cached(graph.panelDrawings(view)).drawings->size() == 2 && integer(graph, steps) == 0,
          "A View Cache preview should leave the first render and its triggers pending");
    const auto first = graph.renderPanel(view);
    check(first.size() == 3 && cached(first).drawings->size() == 2 && cachedX(first) == 1 &&
          integer(graph, steps) == 2,
          "The first render should capture downstream drawing and keep the mouse region live");
    graph.setValue(area, std::string{"3,1,1,1"});
    const auto repeated = graph.renderPanel(view);
    check(cached(repeated).drawings == cached(first).drawings && integer(graph, steps) == 2 &&
          std::get<DrawCommand>(repeated.back().command).area.x == 3,
          "A sibling redraw should show its new value and reuse the cached branch without running Step");

    graph.fireTrigger(clear);
    check(integer(graph, steps) == 2, "Clear should wait for the next draw");
    check(cachedX(graph.panelDrawings(view)) == 3 && integer(graph, steps) == 2,
          "A preview after Clear should still leave the next render pending");
    const auto refreshed = graph.renderPanel(view);
    check(cachedX(refreshed) == 3 && cached(refreshed).revision != cached(first).revision && integer(graph, steps) == 4,
          "The next pass should rebuild the branch with current input values");

    graph.setValue(area, std::string{"5,1,1,1"});
    const auto zoomed = graph.renderPanel(view, 2);
    check(cachedX(zoomed) == 5 && integer(graph, steps) == 6, "A raster scale change should refresh the cache");

    root.resizePanel(module, 12, 8);
    view = graph.frontPanel()->view;
    const auto resized = graph.renderPanel(view, 2);
    check(cached(resized).bounds == view.bounds && integer(graph, steps) == 8,
          "A resize should draw a new View Cache for the new panel bounds");

    graph.disconnectInput(cachedRect, 0);
    const auto disconnected = graph.renderPanel(view, 2);
    check(disconnected.size() == 2 && integer(graph, steps) == 10,
          "A removed drawing link should invalidate the snapshot and remove its graphics");
}

void viewEffectsCaptureOnlyTheWiredDrawing()
{
    using enum PrimitiveKind;
    namespace ui = openstone::yellow;

    Schematic root;
    const int module = root.addNode(Module, 0, 0);
    auto& graph = *root.module(module);
    const auto add = [&](PrimitiveKind kind, std::optional<openstone::green::Value> initial = {}) {
        return graph.addNode(kind, 0, 0, std::move(initial));
    };
    const int gui = add(ModuleGui);
    const int section = add(ViewSection);
    const int scale = add(ViewScale);
    const int blur = add(ViewBlur);
    const int shadow = add(ViewShadow);
    const int glow = add(ViewGlow);
    const int bloom = add(ViewBloom);
    const int shader = add(ViewShader);
    const int rect = add(Rect);
    const int sibling = add(Rect);
    const int mouse = add(MouseArea);
    const int x = add(Float, 2.0F);
    const int extent = add(Float, 8.0F);
    const int ratio = add(Float, 2.0F);
    const int area = add(String, std::string{"1,1,2,2"});
    const int size = add(Point);
    const int sx = add(Float, 4.0F);
    const int sy = add(Float, 100.0F);
    const int channels = add(String, std::string{"r B"});
    const int mix = add(String, std::string{" SuB "});
    const int only = add(Boolean, true);
    const int alpha = add(Float, 0.75F);
    const int threshold = add(Float, 0.85F);
    const int path = add(String, std::string{"/tmp/view.frag.qsb"});
    const int time = add(Float, 2.5F);
    const ui::Color color{0x80FF00FF};
    const int swatch = add(Color, color);

    const SchematicLink links[]{
        {gui, section, 0}, {x, section, 1}, {extent, section, 3}, {extent, section, 4},
        {section, scale, 0}, {ratio, scale, 1}, {scale, blur, 0}, {size, blur, 1},
        {sx, size, 0}, {sy, size, 1}, {channels, blur, 3},
        {blur, shadow, 0}, {alpha, shadow, 3}, {swatch, shadow, 4}, {mix, shadow, 5}, {only, shadow, 6},
        {shadow, glow, 0}, {swatch, glow, 2}, {glow, bloom, 0}, {threshold, bloom, 2},
        {bloom, shader, 0}, {path, shader, 1}, {time, shader, 2},
        {shader, rect, 0}, {gui, sibling, 0}, {area, rect, 1}, {area, sibling, 1},
        {shader, mouse, 0}, {area, mouse, 1},
    };
    for (const auto link : links) {
        graph.connect(link, LinkNotification::Silent);
    }
    const auto view = graph.frontPanel()->view;
    const auto rendered = graph.renderPanel(view);
    check(rendered.size() == 3 && std::holds_alternative<ui::MouseAreaCommand>(rendered[1].command) &&
          std::holds_alternative<ui::DrawCommand>(rendered[2].command),
          "The sibling drawing and mouse hit region should stay outside the effect capture");

    const auto nested = [](const ui::EffectDrawCommand& command) -> const ui::EffectDrawCommand& {
        check(command.drawings->size() == 1, "Every effect should have one child in this chain");
        return std::get<ui::EffectDrawCommand>(command.drawings->front().command);
    };
    const auto& blurCommand = std::get<ui::EffectDrawCommand>(rendered[0].command);
    const auto& blurSettings = std::get<ui::ViewBlur>(blurCommand.effect);
    check(blurCommand.bounds == ui::Area{2, 0, 8, 8} && blurCommand.viewScale == 2 &&
          rendered[0].mapping.clip == ui::ClipRegion{std::vector{ui::Area{2, 0, 8, 8}}} &&
          blurSettings.size == openstone::green::Point{5, 65} && blurSettings.channels == 5,
          "View Blur should inherit the Section bounds and Scale, with normalized kernels and a channel mask");

    const auto& shadowCommand = nested(blurCommand);
    const auto& shadowSettings = std::get<ui::ViewShadow>(shadowCommand.effect);
    check(shadowSettings.alpha == 0.75F && shadowSettings.color == color &&
          shadowSettings.mix == ui::ShadowMix::Subtract && shadowSettings.shadowOnly,
          "View Shadow should preserve the live color, opacity, mix mode and Shadow Only setting");
    const auto& glowCommand = nested(shadowCommand);
    const auto& bloomCommand = nested(glowCommand);
    const auto& shaderCommand = nested(bloomCommand);
    check(std::get<ui::ViewGlow>(glowCommand.effect).color == color &&
          std::get<ui::ViewBloom>(bloomCommand.effect).threshold == 0.85F &&
          std::get<ui::ViewShader>(shaderCommand.effect).path == "/tmp/view.frag.qsb" &&
          std::get<ui::ViewShader>(shaderCommand.effect).time == 2.5F &&
          std::holds_alternative<ui::DrawCommand>(shaderCommand.drawings->front().command),
          "Glow, Bloom and Shader should capture the remaining branch in View order with their own parameters");

    const auto pulses = graph.inspectOutput(rect).triggers;
    graph.setValue(channels, std::string{"A"});
    const auto next = graph.renderPanel(view);
    check(graph.inspectOutput(rect).triggers == pulses + 1 &&
          std::get<ui::ViewBlur>(std::get<ui::EffectDrawCommand>(next[0].command).effect).channels == 8,
          "Changing an input should update the effect and pass one trigger through the View chain");
}

void redrawAreaRepaintsOnlyTheMappedRegion()
{
    using enum PrimitiveKind;
    using openstone::yellow::DrawCommand;
    using openstone::yellow::SolidBrush;
    using openstone::yellow::View;

    Schematic root;
    auto& graph = *root.module(root.addNode(Module, 0, 0));
    const int childModule = graph.addNode(Module, 0, 0);
    auto& child = *graph.module(childModule);
    const int input = child.addNode(ModuleInput, 0, 0);
    const int redrawArea = child.addNode(RedrawArea, 0, 0);
    const int area = child.addNode(String, 0, 0, std::string{"1,-1,3,3"});
    const int partial = child.addNode(Trigger, 0, 0);
    child.connect({input, redrawArea, 0}, LinkNotification::Silent);
    child.connect({area, redrawArea, 1}, LinkNotification::Silent);
    child.connect({partial, redrawArea, 2}, LinkNotification::Silent);

    const int gui = graph.addNode(ModuleGui, 0, 0);
    const int section = graph.addNode(ViewSection, 0, 0);
    const int x = graph.addNode(Float, 0, 0, 4.0F);
    const int y = graph.addNode(Float, 0, 0, 1.0F);
    const int extent = graph.addNode(Float, 0, 0, 6.0F);
    const int scale = graph.addNode(ViewScale, 0, 0);
    const int ratio = graph.addNode(Float, 0, 0, 2.0F);
    const int redraw = graph.addNode(Redraw, 0, 0);
    const int full = graph.addNode(Trigger, 0, 0);
    const int rect = graph.addNode(Rect, 0, 0);
    const int rectArea = graph.addNode(String, 0, 0, std::string{"0,0,16,10"});
    const int solid = graph.addNode(Solid, 0, 0);
    constexpr openstone::yellow::Color red{0xffee2030};
    constexpr openstone::yellow::Color blue{0xff2468df};
    const int color = graph.addNode(Color, 0, 0, red);

    const SchematicLink links[]{
        {gui, section, 0}, {x, section, 1}, {y, section, 2}, {extent, section, 3}, {extent, section, 4},
        {section, scale, 0}, {ratio, scale, 1}, {scale, childModule, 0}, {gui, redraw, 0}, {full, redraw, 1},
        {redraw, rect, 0}, {rectArea, rect, 1}, {color, solid, 0}, {solid, rect, 2},
    };
    for (auto link : links) {
        graph.connect(link, LinkNotification::Silent);
    }

    const auto view = graph.frontPanel()->view;
    const auto colorAt = [](const auto& drawing) {
        return std::get<SolidBrush>(std::get<DrawCommand>(drawing.command).tool).color;
    };
    check(std::get<View>(child.readValue(redrawArea)).isSamePanel(view),
          "Redraw Area should pass the View through its module input");
    auto paint = graph.renderPanel(view);
    check(paint.size() == 1 && colorAt(paint.front()) == red, "The initial paint should fill the panel");
    graph.setValue(color, blue);
    child.setValue(area, std::string{"100,100,1,1"});
    child.fireTrigger(partial);
    paint = graph.renderPanel(view);
    check(paint.size() == 1 && colorAt(paint.front()) == red,
          "An out-of-bounds request and a color change should keep the previous pixels");

    child.setValue(area, std::string{"1,-1,3,3"});
    child.fireTrigger(partial);
    paint = graph.renderPanel(view);
    const openstone::yellow::Area damage{6, 1, 4, 4};
    check(paint.size() == 2 && colorAt(paint[0]) == red && colorAt(paint[1]) == blue &&
          paint[1].mapping.clip == openstone::yellow::ClipRegion{std::vector{damage}} && paint[0].mapping.clip &&
          !paint[0].mapping.contains({7, 3}) && paint[0].mapping.contains({5, 3}),
          "Redraw Area should transform and clip the damage while preserving the rest of the previous drawing");
    child.fireTrigger(partial);
    check(graph.renderPanel(view).size() == 2, "Repeated updates of the same area should replace its previous draw");

    graph.fireTrigger(full);
    paint = graph.renderPanel(view);
    check(paint.size() == 1 && !paint[0].mapping.clip && colorAt(paint[0]) == blue,
          "Redraw should replace the whole panel even after an earlier partial redraw");
}

void redrawInvalidatesOnlyTheCachesOnItsViewRoute()
{
    using enum PrimitiveKind;
    using openstone::yellow::CachedDrawCommand;

    Schematic root;
    auto& graph = *root.module(root.addNode(Module, 0, 0));
    const int gui = graph.addNode(ModuleGui, 0, 0);
    const int firstCache = graph.addNode(ViewCache, 0, 0);
    const int secondCache = graph.addNode(ViewCache, 0, 0);
    const int redraw = graph.addNode(Redraw, 0, 0);
    const int outsideRedraw = graph.addNode(RedrawArea, 0, 0);
    const int inside = graph.addNode(Trigger, 0, 0);
    const int outside = graph.addNode(Trigger, 0, 0);
    const int firstRect = graph.addNode(Rect, 0, 0);
    const int secondRect = graph.addNode(Rect, 0, 0);
    const int area = graph.addNode(String, 0, 0, std::string{"1,1,5,4"});
    const SchematicLink links[]{
        {gui, firstCache, 0}, {gui, secondCache, 0}, {firstCache, redraw, 0}, {inside, redraw, 1},
        {gui, outsideRedraw, 0}, {area, outsideRedraw, 1}, {outside, outsideRedraw, 2}, {redraw, firstRect, 0}, {secondCache, secondRect, 0},
        {area, firstRect, 1}, {area, secondRect, 1},
    };
    for (auto link : links) {
        graph.connect(link, LinkNotification::Silent);
    }
    const auto view = graph.frontPanel()->view;
    const auto first = graph.renderPanel(view);
    const auto revision = [](const auto& drawings, std::size_t index) {
        return std::get<CachedDrawCommand>(drawings.at(index).command).revision;
    };
    graph.fireTrigger(outside);
    const auto external = graph.renderPanel(view);
    check(external.size() == 2 && revision(external, 0) == revision(first, 0) && revision(external, 1) == revision(first, 1),
          "An external Redraw Area should reuse both textures as whole layers");
    graph.fireTrigger(inside);
    const auto internal = graph.renderPanel(view);
    check(revision(internal, 0) != revision(first, 0) && revision(internal, 1) == revision(first, 1),
          "A Redraw routed through a View Cache should refresh only that cache");
}

void viewCacheWithinLoopKeepsSeparateIterations()
{
    using enum PrimitiveKind;
    using openstone::yellow::CachedDrawCommand;
    using openstone::yellow::TextCommand;

    Schematic root;
    auto& graph = *root.module(root.addNode(Module, 0, 0));
    const int gui = graph.addNode(ModuleGui, 0, 0);
    const int loop = graph.addNode(ViewArrayLoop, 0, 0);
    const int lines = graph.addNode(String, 0, 0, std::string{"first\nsecond"});
    const int cache = graph.addNode(ViewCache, 0, 0);
    const int text = graph.addNode(Text, 0, 0);
    const int area = graph.addNode(String, 0, 0, std::string{"0,0,5,2"});
    const SchematicLink links[]{
        {gui, loop, 0}, {lines, loop, 1}, {loop, cache, 0}, {cache, text, 0}, {area, text, 1}, {loop, text, 3, 1},
    };
    for (auto link : links) {
        graph.connect(link, LinkNotification::Silent);
    }

    const auto view = graph.frontPanel()->view;
    const auto first = graph.renderPanel(view);
    check(first.size() == 2, "A View Cache inside a loop should have a distinct snapshot for each iteration");
    const auto& firstCache = std::get<CachedDrawCommand>(first[0].command);
    const auto& secondCache = std::get<CachedDrawCommand>(first[1].command);
    check(std::get<TextCommand>(firstCache.drawings->front().command).text == "first" &&
          std::get<TextCommand>(secondCache.drawings->front().command).text == "second",
          "A cache should preserve each iteration's drawing inputs");
    const auto repeated = graph.renderPanel(view);
    check(std::get<CachedDrawCommand>(repeated[0].command).drawings == firstCache.drawings &&
          std::get<CachedDrawCommand>(repeated[1].command).drawings == secondCache.drawings,
          "Each iteration should reuse its own snapshot on the next redraw");
}

void viewArrayLoopRepeatsNestedDrawingInReverse()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    using openstone::yellow::TextCommand;

    Schematic root;
    auto& graph = *root.module(root.addNode(Module, 0, 0));
    const int gui = graph.addNode(ModuleGui, 0, 0);
    const int outer = graph.addNode(ViewLoop, 0, 0);
    const int end = graph.addNode(Float, 0, 0, 1.0F);
    const int half = graph.addNode(Float, 0, 0, 0.5F);
    const int section = graph.addNode(ViewSection, 0, 0);
    const int array = graph.addNode(ViewArrayLoop, 0, 0);
    const int lines = graph.addNode(String, 0, 0, std::string{"alpha\nbeta\ngamma"});
    const int reverse = graph.addNode(Boolean, 0, 0, true);
    const int area = graph.addNode(Area, 0, 0);
    const int extent = graph.addNode(Float, 0, 0, 1.0F);
    const int text = graph.addNode(Text, 0, 0);
    const int finishes = graph.addNode(TriggerCounter, 0, 0);
    const SchematicLink links[]{
        {gui, outer, 0}, {end, outer, 3}, {half, outer, 2}, {outer, section, 0}, {outer, section, 1, 1},
        {section, array, 0}, {lines, array, 1}, {reverse, array, 2}, {array, text, 0}, {array, text, 3, 1},
        {array, area, 1, 2}, {extent, area, 2}, {extent, area, 3}, {area, text, 1}, {array, finishes, 0, 4},
    };
    for (auto link : links) {
        graph.connect(link, LinkNotification::Silent);
    }
    const auto view = graph.frontPanel()->view;
    const auto drawings = graph.renderPanel(view);
    check(graph.inputType(array, 1) == Type::StringArray && graph.outputType(array, 1) == Type::String &&
          drawings.size() == 6 && integer(graph, finishes) == 2,
          "An array loop should repeat once per outer iteration and infer the String element type");
    for (int i = 0; i < 6; ++i) {
        const auto& drawing = drawings[i];
        const auto& label = std::get<TextCommand>(drawing.command);
        const std::array<std::string_view, 3> reverseLabels{"gamma", "beta", "alpha"};
        check(label.text == reverseLabels[i % 3] && label.area.y == 2 - (i % 3) &&
              drawing.mapping.origin.x == (i / 3) * 0.5F,
              "Reverse should retain source indices and use each outer iteration's View transform");
    }
    graph.setValue(lines, std::string{});
    check(graph.renderPanel(view).empty() && integer(graph, finishes) == 4, "An empty array should draw nothing");

    graph.disconnectInput(text, 3);
    graph.setTemplateType(array, Type::PointArray);
    graph.setValue(lines, std::string{"1,2\n3,4"});
    check(graph.renderPanel(view).empty() && graph.outputType(array, 1) == Type::Point &&
          std::get<openstone::green::Point>(graph.readValue(array, 1)) == openstone::green::Point{1, 2} &&
          integer(graph, finishes) == 6,
          "Selecting a Point Array should expose Point values and run with no attached drawings");
}

void clipsCombineRectanglesWithoutChangingTheViewSize()
{
    using enum PrimitiveKind;
    using openstone::yellow::View;
    Schematic root;
    auto& graph = *root.module(root.addNode(Module, 0, 0));
    const int panel = graph.addNode(ModuleGui, 0, 0);
    const int first = graph.addNode(Clip, 0, 0);
    const int second = graph.addNode(Clip, 0, 0);
    const int a = graph.addNode(String, 0, 0, std::string{"2,2,6,6"});
    const int b = graph.addNode(String, 0, 0, std::string{"5,5,5,5"});
    const int mode = graph.addNode(Int, 0, 0, std::int32_t{0});
    const SchematicLink links[]{{panel, first, 0}, {first, second, 0}, {a, first, 1}, {b, second, 1},
                                {mode, second, 2}};
    for (const auto link : links) {
        graph.connect(link, LinkNotification::Silent);
    }

    constexpr bool expected[][3]{
        {false, false, true}, // Intersect
        {true,  true,  true}, // Union
        {false, true,  false}, // Complement
        {true,  false, false}, // Exclude
        {true,  true,  false}, // Xor
    };
    const auto source = graph.frontPanel()->view;
    for (std::int32_t operation = 0; operation < 5; ++operation) {
        graph.setValue(mode, operation);
        const auto out = std::get<View>(graph.readValue(second));
        check(out.bounds == source.bounds && out.isSamePanel(source) &&
              out.mapping.contains({3, 3}) == expected[operation][0] &&
              out.mapping.contains({9, 9}) == expected[operation][1] &&
              out.mapping.contains({6, 6}) == expected[operation][2] && !out.mapping.contains({12, 12}),
              "Clip must apply the documented region operation while preserving the View dimensions");
    }
    check(!source.mapping.clip && std::get<View>(graph.readValue(first)).mapping.contains({6, 6}),
          "Clip should affect only the downstream path");
    graph.setValue(b, std::string{"2,2,6,6"});
    const auto xorIdentical = std::get<View>(graph.readValue(second));
    check(xorIdentical.mapping.fullyClipped(),
          "A fully clipped View must remain distinct from an unclipped View");
}

void setGuiAreaChangesThePanelOnTheSetTrigger()
{
    using enum PrimitiveKind;
    using openstone::green::Value;
    using PanelArea = openstone::yellow::Area;

    Schematic root;
    const int outer = root.addNode(Module, 0, 0);
    auto& parent = *root.module(outer);
    const int parentView = parent.addNode(ModuleGui, 0, 0);
    const int module = parent.addNode(Module, 0, 0);
    auto& child = *parent.module(module);
    const int view = child.addNode(ModuleGui, 0, 0);
    const int input = child.addNode(ModuleInput, 0, 0);
    child.connect({input, view, 0}, LinkNotification::Silent);
    parent.connect({parentView, module, 0}, LinkNotification::Silent);

    const int setter = child.addNode(SetGuiArea, 0, 0);
    const int guiArea = child.addNode(GuiArea, 0, 0);
    const int area = child.addNode(PrimitiveKind::ViewArea, 0, 0);
    const int text = child.addNode(String, 0, 0, std::string{"3,4,10,8"});
    const int trigger = child.addNode(Trigger, 0, 0);
    const SchematicLink links[]{{view, setter, 0}, {text, setter, 1}, {trigger, setter, 2},
                                {setter, guiArea, 0}, {setter, area, 0}};
    for (const auto link : links) {
        child.connect(link, LinkNotification::Silent);
    }

    const auto initial = child.frontPanel()->view.bounds;
    const auto revision = root.panelGeometryRevision();
    child.setValue(text, std::string{"2,5,11,9"});
    check(child.readValue(guiArea) == Value{initial} && root.panelGeometryRevision() == revision,
          "Editing the Area input should only stage the requested position and size");
    child.fireTrigger(trigger);
    check(child.readValue(guiArea) == Value{PanelArea{2, 5, 11, 9}} &&
          child.readValue(area) == Value{PanelArea{0, 0, 11, 9}} &&
          parent.frontPanel()->view.bounds == initial && root.panelGeometryRevision() == revision + 1 &&
          child.inspectOutput(guiArea).triggers == 1,
          "Set must move and resize the View's own panel, notify readers, and leave the ancestor panel alone");
    child.fireTrigger(trigger);
    child.setValue(text, std::string{"0,0,0,3"});
    child.fireTrigger(trigger);
    check(root.panelGeometryRevision() == revision + 1, "An identical or invalid Area should leave geometry in place");

    root.movePanelItem(outer, std::array{module}, {6, 7});
    parent.resizePanel(module, 12, 10);
    check(child.readValue(guiArea) == Value{PanelArea{6, 7, 12, 10}} &&
          child.inspectOutput(guiArea).triggers == 3,
          "Moving and resizing from the editor should notify GUI Area through the same View route");
}

void mouseMoveFollowsTheWiredViewAndItsMouseToggle()
{
    using enum PrimitiveKind;
    Schematic root;
    const int firstModule = root.addNode(Module, 0, 0);
    const int secondModule = root.addNode(Module, 0, 0);
    auto& first = *root.module(firstModule);
    auto& second = *root.module(secondModule);
    const int firstView = first.addNode(ModuleGui, 0, 0);
    const int secondView = second.addNode(ModuleGui, 0, 0);
    check(firstView == secondView, "Test views should have colliding local IDs");
    const int firstEnabled = first.addNode(Boolean, 0, 0, false);
    const int secondEnabled = second.addNode(Boolean, 0, 0, true);
    first.connect({firstEnabled, firstView, 1});
    second.connect({secondEnabled, secondView, 1});
    const int move = first.addNode(MouseMove, 0, 0);
    const int secondMove = second.addNode(MouseMove, 0, 0);
    first.connect({firstView, move, 0});
    second.connect({secondView, secondMove, 0});

    check(!first.movePanelPointer(firstView, {1, 2}) && first.inspectOutput(move).triggers == 0,
          "Move must be silent until the MGUI Mouse input is enabled");
    first.setValue(firstEnabled, true);
    check(first.movePanelPointer(firstView, {1, 2}) && first.movePanelPointer(firstView, {2, 2}) &&
          !first.movePanelPointer(firstView, {2, 2}) && !first.movePanelPointer(firstView, {1, 11}) &&
          std::get<openstone::green::Point>(first.readValue(move)) == openstone::green::Point{2, 2} && first.inspectOutput(move).triggers == 2 &&
          second.inspectOutput(secondMove).triggers == 0,
          "Move should track real position changes inside its own View only");
    check(second.movePanelPointer(secondView, {3, 4}) && std::get<openstone::green::Point>(second.readValue(secondMove)) == openstone::green::Point{3, 4},
          "A separate View should publish its own movement");
    first.setValue(firstEnabled, false);
    check(!first.movePanelPointer(firstView, {3, 4}) && first.inspectOutput(move).triggers == 2,
          "Turning off Mouse should stop further movement pulses");
}

void dropFilesPreviewsAndCommitsAcceptedBatches()
{
    using enum PrimitiveKind;
    using openstone::DropFile;
    using openstone::FileDropPhase;
    using openstone::green::StringArray;
    using openstone::yellow::DropFilesCommand;
    Schematic root;
    const int module = root.addNode(Module, 0, 0);
    auto& graph = *root.module(module);
    const int view = graph.addNode(ModuleGui, 0, 0);
    const int area = graph.addNode(Area, 0, 0);
    const int size = graph.addNode(Float, 0, 0, 6.0F);
    const int target = graph.addNode(DropFiles, 0, 0);
    const int extensions = graph.addNode(String, 0, 0, std::string{" .PNG, *.wav "});
    const int directories = graph.addNode(Boolean, 0, 0, false);
    const int counter = graph.addNode(TriggerCounter, 0, 0);
    graph.connect({view, target, 0});
    graph.connect({size, area, 0});
    graph.connect({size, area, 2});
    graph.connect({size, area, 3});
    graph.connect({area, target, 1});
    graph.connect({extensions, target, 2});
    graph.connect({directories, target, 3});
    graph.connect({target, counter, 0, 4}, LinkNotification::Silent);

    const auto panel = graph.frontPanel()->view;
    const auto command = root.panelDrawings(panel).at(0);
    check(std::get<DropFilesCommand>(command.command).id.ownerPath == std::vector{module},
          "The panel's drop target should refer to the primitive's owning schematic");

    const DropFile picture{"/folder/My Picture.pNg", "My Picture.pNg", false};
    const DropFile sound{"/folder/clip.WAV", "clip.WAV", false};
    const DropFile document{"/folder/notes.txt", "notes.txt", false};
    const DropFile directory{"/folder/subdir", "subdir", true};
    const std::array accepted{picture, sound};
    const std::array mixed{picture, document};
    check(!graph.receiveFileDrop(target, accepted, {7, 2}, FileDropPhase::Preview) &&
          !graph.receiveFileDrop(target, std::span{&picture, 1}, {5, 2}, FileDropPhase::Preview) &&
          !graph.receiveFileDrop(target, std::span{&directory, 1}, {7, 2}, FileDropPhase::Preview),
          "The default one-file limit, drop bounds, and directory filter should reject invalid drags");

    const int max = graph.addNode(Int, 0, 0, std::int32_t{2});
    graph.connect({max, target, 4});
    check(!graph.receiveFileDrop(target, mixed, {7, 2}, FileDropPhase::Preview) &&
          graph.receiveFileDrop(target, accepted, {7, 2}, FileDropPhase::Preview) &&
          std::get<StringArray>(graph.readValue(target)) == StringArray{picture.path, sound.path} &&
          std::get<bool>(graph.readValue(target, 3)) && integer(graph, counter) == 0,
          "The whole batch should match case-insensitive extensions before its paths become readable");
    check(graph.moveFileDrag(target, {8, 3}) && std::get<float>(graph.readValue(target, 1)) == 8 &&
          std::get<float>(graph.readValue(target, 2)) == 3 && integer(graph, counter) == 0,
          "Moving the preview should update the View coordinates without firing the drop trigger");
    check(graph.receiveFileDrop(target, accepted, {9, 4}, FileDropPhase::Drop) &&
          !std::get<bool>(graph.readValue(target, 3)) && integer(graph, counter) == 1 &&
          std::get<float>(graph.readValue(target, 1)) == 9,
          "A successful drop should clear the drag state and pulse once with the final coordinates");

    graph.setValue(directories, true);
    check(graph.receiveFileDrop(target, std::span{&directory, 1}, {7, 2}, FileDropPhase::Preview) &&
          std::get<StringArray>(graph.readValue(target)) == StringArray{directory.path} &&
          graph.leaveFileDrag(target) && std::get<StringArray>(graph.readValue(target)) == StringArray{picture.path, sound.path} &&
          !std::get<bool>(graph.readValue(target, 3)) && integer(graph, counter) == 1,
          "Leaving with a valid directory should restore the prior committed drop without pulsing");
}

void popupListCommitsTheIndexAndTextTogether()
{
    using enum PrimitiveKind;
    using openstone::DropListLayout;
    using openstone::yellow::PopupListCommand;
    using openstone::yellow::View;
    Schematic root;
    const int module = root.addNode(Module, 0, 0);
    auto& graph = *root.module(module);
    const int view = graph.addNode(ModuleGui, 0, 0);
    const int popup = graph.addNode(PopupList, 0, 0);
    const int area = graph.addNode(Area, 0, 0);
    const int size = graph.addNode(Float, 0, 0, 4.0F);
    const int list = graph.addNode(String, 0, 0, std::string{"Apple, Orange, Banana, Pear"});
    const int set = graph.addNode(Int, 0, 0, std::int32_t{1});
    graph.connect({size, area, 2});
    graph.connect({size, area, 3});
    graph.connect({view, popup, 0});
    graph.connect({area, popup, 1});
    graph.connect({list, popup, 5});
    graph.connect({set, popup, 7});

    const auto panel = std::get<View>(graph.readValue(popup));
    const auto drawings = root.panelDrawings(panel);
    const auto& control = std::get<PopupListCommand>(drawings.at(0).command);
    check(control.id.ownerPath == std::vector{module} && control.hasOptions && control.text == "Orange",
          "The panel should show the option selected through Set");
    check(graph.openPopupList(popup) && root.pendingDropList()->layout == DropListLayout::Scroll &&
          root.pendingDropList()->rowsPerColumn == 8 && root.pendingDropList()->checkedIndex == 1,
          "The press should open a scrolling list with up to eight rows and the current selection checked");
    const auto textPulses = graph.inspectOutput(popup, 1).triggers;
    const auto indexPulses = graph.inspectOutput(popup, 2).triggers;
    root.finishDropList(root.pendingDropList()->token, 3);
    check(!root.pendingDropList() && std::get<std::string>(graph.readValue(popup, 1)) == "Pear" &&
          std::get<std::int32_t>(graph.readValue(popup, 2)) == 3 &&
          graph.inspectOutput(popup, 1).triggers == textPulses + 1 &&
          graph.inspectOutput(popup, 2).triggers == indexPulses + 1,
          "Release should update and pulse the selected String and Int outputs together");

    graph.setValue(set, std::int32_t{9});
    check(std::get<std::int32_t>(graph.readValue(popup, 2)) == 3,
          "An out-of-range Set value should keep the current selection");
    check(graph.openPopupList(popup), "A new press should open the popup again");
    const auto stale = root.pendingDropList()->token;
    graph.setValue(list, std::string{"Other, Values"});
    root.finishDropList(stale, 1);
    check(!root.pendingDropList() && std::get<std::string>(graph.readValue(popup, 1)) == "Other" &&
          std::get<std::int32_t>(graph.readValue(popup, 2)) == 0,
          "Changing options during a press should discard the old menu and reset an unavailable selection");
    graph.setValue(list, std::string{});
    check(!graph.openPopupList(popup) && std::get<std::string>(graph.readValue(popup, 1)).empty() &&
          std::get<std::int32_t>(graph.readValue(popup, 2)) == -1,
          "An empty list should have no selection and should stay closed");
}

void editControlUsesViewAndPublishesEdits()
{
    using enum PrimitiveKind;
    using openstone::yellow::EditControlCommand;
    using openstone::yellow::View;
    Schematic root;
    const int module = root.addNode(Module, 0, 0);
    auto& graph = *root.module(module);
    const int view = graph.addNode(ModuleGui, 0, 0);
    const int area = graph.addNode(Area, 0, 0);
    const int extent = graph.addNode(Float, 0, 0, 4.0F);
    const int edit = graph.addNode(EditControl, 0, 0);
    const int overlay = graph.addNode(Rect, 0, 0);
    const int source = graph.addNode(String, 0, 0, std::string{"Initial"});
    const int finishOnly = graph.addNode(Boolean, 0, 0, true);
    const int trigger = graph.addNode(Trigger, 0, 0);
    graph.connect({view, edit, 0});
    graph.connect({extent, area, 2});
    graph.connect({extent, area, 3});
    graph.connect({area, edit, 1});
    graph.connect({area, overlay, 1});
    graph.connect({edit, overlay, 0});
    graph.connect({source, edit, 5});
    graph.connect({finishOnly, edit, 6});
    graph.connect({trigger, edit, 7}, LinkNotification::Silent);

    const auto panel = std::get<View>(graph.readValue(edit));
    const auto drawings = root.panelDrawings(panel);
    check(drawings.size() == 2 && std::holds_alternative<EditControlCommand>(drawings.front().command),
          "Edit Control should draw in View order, below connected drawing primitives");
    const auto& control = std::get<EditControlCommand>(drawings.front().command);
    check(control.id.ownerPath == std::vector{module} && control.id.node == edit &&
          control.area.width == 4.0F && control.text == "Initial",
          "The View should describe the control's owner, Area, and current text");

    graph.fireTrigger(trigger);
    check(root.takeEditControlStart() == control.id && !root.takeEditControlStart() &&
          !std::get<bool>(graph.readValue(edit, 2)),
          "Start should request focus once without claiming focus in the runtime");
    const auto token = graph.beginEditControl(edit);
    const auto initialPulses = graph.inspectOutput(edit, 1).triggers;
    check(token && std::get<bool>(graph.readValue(edit, 2)) &&
          graph.changeEditControl(edit, token, "First") && graph.changeEditControl(edit, token, "Final") &&
          std::get<std::string>(graph.readValue(edit, 1)) == "Final" &&
          graph.inspectOutput(edit, 1).triggers == initialPulses,
          "Finish-only mode should make live text readable without pulsing it");
    check(graph.finishEditControl(edit, token) && !graph.finishEditControl(edit, token) &&
          !std::get<bool>(graph.readValue(edit, 2)) && graph.inspectOutput(edit, 2).triggers == 2 &&
          graph.inspectOutput(edit, 1).triggers == initialPulses + 1,
          "Finishing should publish a single text pulse and clear Is Editing");

    graph.setValue(finishOnly, false);
    graph.setValue(source, std::string{"Reset"});
    const auto next = graph.beginEditControl(edit);
    check(next != token && !graph.changeEditControl(edit, token, "Stale") &&
          std::get<std::string>(graph.readValue(edit, 1)) == "Reset" &&
          graph.changeEditControl(edit, next, "Live") &&
          graph.inspectOutput(edit, 1).triggers == initialPulses + 3 &&
          graph.finishEditControl(edit, next) && graph.inspectOutput(edit, 1).triggers == initialPulses + 3,
          "Set should replace the text, and live mode should pulse on change without a second pulse on finish");
}

void colorBoxesPreserveArgbAndTriggers()
{
    using openstone::green::Value;
    using openstone::yellow::Color;
    Schematic schematic;
    const Color local{0xFF102030};
    const Color transparent{0x0040C8C3};
    const Color translucent{0x8040C8C3};
    const int source = schematic.addNode(PrimitiveKind::Color, 0, 0, transparent);
    const int display = schematic.addNode(PrimitiveKind::Color, 200, 0, local);
    const int counter = schematic.addNode(PrimitiveKind::TriggerCounter, 400, 0);
    schematic.connect({display, counter, 0}, LinkNotification::Silent);
    schematic.connect({source, display, 0});
    check(schematic.readValue(display) == Value{transparent} && !schematic.editable(display) &&
          integer(schematic, counter) == 1,
          "A wired Color box should forward all four bytes, including RGB at zero alpha");

    schematic.setValue(source, translucent);
    schematic.setValue(source, translucent);
    check(schematic.inspectLink({source, display, 0}).value == Value{translucent} &&
          integer(schematic, counter) == 2,
          "Changing alpha alone should update the value and emit exactly one trigger");
    schematic.disconnectInput(display, 0);
    check(schematic.editable(display) && schematic.readValue(display) == Value{local},
          "Disconnecting should restore the Color box's local swatch");
}

void argbPrimitivesPreserveStraightColorBytes()
{
    using enum PrimitiveKind;
    using PackedColor = openstone::yellow::Color;

    Schematic graph;
    const int alpha = graph.addNode(Int, 0, 0, std::int32_t{256});
    const int red = graph.addNode(Int, 0, 0, std::int32_t{-1});
    const int green = graph.addNode(Int, 0, 0, std::int32_t{260});
    const int blue = graph.addNode(Int, 0, 0, std::int32_t{514});
    const int packed = graph.addNode(ArgbToColor, 0, 0);
    const int split = graph.addNode(ColorToArgb, 0, 0);
    const SchematicLink links[]{
        {alpha, packed, 0}, {red, packed, 1}, {green, packed, 2}, {blue, packed, 3}, {packed, split, 0},
    };
    for (const auto link : links) {
        graph.connect(link, LinkNotification::Silent);
    }

    check(std::get<PackedColor>(graph.readValue(packed)) == PackedColor{0x00FF0402},
          "ARGB should wrap each source to a byte and preserve RGB under zero alpha");
    const std::array<std::int32_t, 4> expected{0, 255, 4, 2};
    for (int output = 0; output < 4; ++output) {
        check(std::get<std::int32_t>(graph.readValue(split, output)) == expected[output],
              "Color to ARGB should expose alpha, red, green and blue in order");
    }
    graph.setValue(alpha, std::int32_t{128});
    check(std::get<PackedColor>(graph.readValue(packed)) == PackedColor{0x80FF0402},
          "Changing alpha should not premultiply or otherwise modify the RGB channels");
    for (int output = 0; output < 4; ++output) {
        check(graph.inspectOutput(split, output).triggers == 1, "Every ARGB output should receive the Color trigger");
    }
}

void ahsvPrimitivesUseDegreesAndIntegerComponents()
{
    using enum PrimitiveKind;
    using PackedColor = openstone::yellow::Color;

    Schematic graph;
    const int swatch = graph.addNode(Color, 0, 0, PackedColor{0x7940C8C3});
    const int split = graph.addNode(ColorToAhsv, 0, 0);
    const int alpha = graph.addNode(Int, 0, 0, std::int32_t{121});
    const int hue = graph.addNode(Int, 0, 0, std::int32_t{30});
    const int sat = graph.addNode(Int, 0, 0, std::int32_t{255});
    const int val = graph.addNode(Int, 0, 0, std::int32_t{255});
    const int packed = graph.addNode(AhsvToColor, 0, 0);
    const SchematicLink links[]{
        {swatch, split, 0}, {alpha, packed, 0}, {hue, packed, 1}, {sat, packed, 2}, {val, packed, 3},
    };
    for (const auto link : links) {
        graph.connect(link, LinkNotification::Silent);
    }

    const auto verifySplit = [&](const std::array<std::int32_t, 4>& expected) {
        for (int output = 0; output < 4; ++output) {
            check(std::get<std::int32_t>(graph.readValue(split, output)) == expected[output],
                  "Color to AHSV should return alpha, rounded hue, rounded saturation and value in order");
        }
    };
    verifySplit({121, 178, 173, 200});
    graph.setValue(swatch, PackedColor{0x00202020});
    verifySplit({0, 0, 0, 32});
    graph.setValue(swatch, PackedColor{0x22FF0001});
    verifySplit({34, 360, 255, 255});

    const auto color = [&] { return std::get<PackedColor>(graph.readValue(packed)); };
    check(color() == PackedColor{0x79FF7F00}, "AHSV should truncate a fractional RGB byte");
    const std::array<std::uint32_t, 6> hues{
        0x79FF0000, 0x79FFFF00, 0x7900FF00, 0x7900FFFF, 0x790000FF, 0x79FF00FF,
    };
    for (std::int32_t sector = 0; sector < 6; ++sector) {
        graph.setValue(hue, sector * 60);
        check(color() == PackedColor{hues[sector]}, "Each 60-degree boundary should map to its primary or secondary hue");
    }
    graph.setValue(hue, std::int32_t{-60});
    check(color() == PackedColor{hues.back()}, "Negative hue should wrap around the color wheel");
    graph.setValue(hue, std::int32_t{360});
    check(color() == PackedColor{hues.front()}, "360 degrees should produce the same Color as zero");
    graph.setValue(sat, std::int32_t{0});
    graph.setValue(val, std::int32_t{120});
    check(color() == PackedColor{0x79787878}, "Zero saturation should produce grayscale with the supplied alpha");
    graph.setValue(val, std::int32_t{0});
    check(color() == PackedColor{0x79000000}, "Zero value should produce black with the supplied alpha");
}

void colorMatrixWiringAndChannelOrder()
{
    using openstone::green::Value;
    using openstone::yellow::Color;
    using openstone::yellow::ColorMatrix;
    Schematic schematic;
    const Color sample{0xFF401080};
    const int source = schematic.addNode(PrimitiveKind::Color, 0, 0, sample);
    const int first = schematic.addNode(PrimitiveKind::ColorMatrixSet, 200, 0);
    const int second = schematic.addNode(PrimitiveKind::ColorMatrixSet, 400, 0);
    const int transform = schematic.addNode(PrimitiveKind::ApplyColorMatrix, 600, 0);
    const int counter = schematic.addNode(PrimitiveKind::TriggerCounter, 800, 0);
    check(schematic.readValue(first) == Value{ColorMatrix{}}, "An unconnected Set should start at identity");

    const int greenColumn = schematic.addNode(PrimitiveKind::Int, 0, 100, std::int32_t{1});
    const int alphaIndex = schematic.addNode(PrimitiveKind::Int, 0, 200, std::int32_t{3});
    const int factor = schematic.addNode(PrimitiveKind::Float, 0, 300, 0.5F);
    const SchematicLink links[]{
        {greenColumn, first, 2}, {factor, first, 3}, {first, second, 0},
        {alphaIndex, second, 1}, {alphaIndex, second, 2}, {factor, second, 3},
        {source, transform, 0}, {second, transform, 1}, {transform, counter, 0},
    };
    for (const auto link : links) {
        schematic.connect(link, LinkNotification::Silent);
    }
    check(schematic.readValue(transform) == Value{Color{0x80403080}},
          "Row 0, column 1 should add red into green, and row 3, column 3 should scale alpha");
    auto expected = ColorMatrix{};
    expected.coefficients[0][1] = 0.5F;
    check(schematic.readValue(first) == Value{expected}, "A downstream Set must preserve the upstream matrix");

    schematic.setValue(alphaIndex, std::int32_t{5});
    check(schematic.readValue(second) == Value{expected} &&
          schematic.readValue(transform) == Value{Color{0xFF403080}} && integer(schematic, counter) == 2,
          "Invalid indices should pass the input matrix through and deliver both input triggers");

    auto bias = ColorMatrix{};
    bias.coefficients[4][0] = 1.0F;
    bias.coefficients[4][1] = -1.0F;
    bias.coefficients[4][3] = -0.25F;
    check(openstone::yellow::applyColorMatrix(bias, sample) == Color{0xBFFF0080},
          "The last row should add channel offsets and clamp the result");
}

void paintToolsPreserveStrokesAndFills()
{
    using openstone::green::Type;
    using openstone::green::Value;
    using openstone::yellow::LineStyle;
    using openstone::yellow::PaintTool;
    using openstone::yellow::PenStroke;
    using openstone::yellow::SolidBrush;

    Schematic schematic;
    const openstone::yellow::Color translucent{0x8040C8C3};
    const int color = schematic.addNode(PrimitiveKind::Color, 0, 0, translucent);
    const int width = schematic.addNode(PrimitiveKind::Float, 0, 100, 0.25F);
    const int style = schematic.addNode(PrimitiveKind::String, 0, 200, std::string(" DashDotDot "));
    const int pen = schematic.addNode(PrimitiveKind::Pen, 200, 0);
    const int solid = schematic.addNode(PrimitiveKind::Solid, 200, 200);
    const int gate = schematic.addNode(PrimitiveKind::TriggerSwitch, 400, 0);
    const int enabled = schematic.addNode(PrimitiveKind::Boolean, 200, 350, true);
    const int counter = schematic.addNode(PrimitiveKind::TriggerCounter, 600, 0);
    const SchematicLink links[]{
        {color, pen, 0}, {width, pen, 1}, {style, pen, 2}, {color, solid, 0},
        {pen, gate, 0}, {enabled, gate, 1}, {gate, counter, 0},
    };
    for (const auto link : links) {
        schematic.connect(link, LinkNotification::Silent);
    }
    check(schematic.outputType(gate) == Type::PaintTool &&
          schematic.readValue(gate) == Value{PaintTool{PenStroke{translucent, 0.25F, LineStyle::DashDotDot}}},
          "A Paint Tool link should preserve stroke width, style, and color alpha");

    schematic.setValue(width, 0.0F);
    schematic.setValue(style, std::string("unknown"));
    check(schematic.readValue(gate) == Value{PaintTool{PenStroke{translucent, 0.0F, LineStyle::Solid}}} &&
          integer(schematic, counter) == 2,
          "Hairline and style changes should reach a downstream Paint Tool and forward each trigger");

    schematic.disconnectInput(gate, 0);
    schematic.connect({solid, gate, 0});
    check(schematic.outputType(gate) == Type::PaintTool &&
          schematic.readValue(gate) == Value{PaintTool{SolidBrush{translucent}}},
          "A solid brush should use the same connector and keep its own value");
}

void yellowBrushesCarrySettingsThroughDrawings()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    using openstone::green::Value;
    namespace ui = openstone::yellow;

    const ui::Bitmap pixels(1, 1, {27, 64, 128, 212});
    Schematic root{[&](std::string_view) { return pixels; }};
    const int module = root.addNode(Module, 0, 0);
    auto& graph = *root.module(module);
    const int mgui = graph.addNode(ModuleGui, 0, 0);
    const auto add = [&](PrimitiveKind kind, std::optional<Value> initial = {}) {
        return graph.addNode(kind, 0, 0, std::move(initial));
    };
    const int linear = add(LinearGradientBrush);
    const int radial = add(RadialGradientBrush);
    const int hatch = add(HatchBrush);
    const int texture = add(TextureBrush);
    const int rect = add(Rect);
    const int ellipse = add(Ellipse);
    const int pie = add(Pie);
    const int polygon = add(DrawPolygon);
    const int line = add(Line);

    check(graph.readValue(radial) == Value{ui::PaintTool{ui::RadialGradientBrush{}}} &&
          graph.readValue(texture) == Value{ui::PaintTool{ui::TextureBrush{}}},
          "Radial gradients should default to an ellipse with an automatic center, and textures should start empty");

    constexpr ui::Color first{0xA0F02040};
    constexpr ui::Color second{0xDD3366AA};
    const int col1 = add(Color, first);
    const int col2 = add(Color, second);
    const int area = add(String, std::string{"2,1,8,6"});
    const int angle = add(Float, 450.0F);
    const int sweep = add(Float, 90.0F);
    const int wrap = add(String, std::string{" FLipXy "});
    const int shape = add(String, std::string{"Rect"});
    const int centerX = add(Float, 5.0F);
    const int focusX = add(Float, 0.4F);
    const int focusY = add(Float, 2.0F);
    const int style = add(String, std::string{" Wave "});
    const int bitmap = add(Bitmap);
    const int points = add(String, std::string{"-1,1\n1,1\n1,-1"});

    const SchematicLink links[]{
        {col1, linear, 0}, {col2, linear, 1}, {area, linear, 2}, {angle, linear, 3}, {wrap, linear, 4},
        {col1, radial, 0}, {col2, radial, 1}, {area, radial, 2}, {shape, radial, 3},
        {centerX, radial, 4}, {focusX, radial, 6}, {focusY, radial, 7},
        {col1, hatch, 0}, {col2, hatch, 1}, {style, hatch, 2}, {bitmap, texture, 0}, {wrap, texture, 1},
        {mgui, rect, 0}, {area, rect, 1}, {linear, rect, 2},
        {rect, ellipse, 0}, {area, ellipse, 1}, {radial, ellipse, 2},
        {ellipse, pie, 0}, {area, pie, 1}, {hatch, pie, 2}, {sweep, pie, 4},
        {pie, polygon, 0}, {area, polygon, 1}, {texture, polygon, 2}, {points, polygon, 3},
        {polygon, line, 0}, {area, line, 1}, {texture, line, 2},
    };
    for (const auto link : links) {
        graph.connect(link, LinkNotification::Silent);
    }
    check(graph.outputType(linear) == Type::PaintTool && graph.outputType(texture) == Type::PaintTool,
          "All brush constructors should use the existing Paint Tool connector");
    graph.loadBitmap(bitmap, "texture.png");
    const auto view = graph.frontPanel()->view;
    const auto drawings = root.panelDrawings(view);
    check(drawings.size() == 4, "Brushes should fill closed shapes and leave the open line undrawn");

    const ui::Area bounds{2, 1, 8, 6};
    const auto toolAt = [&](int index) -> const ui::PaintTool& {
        return std::get<ui::DrawCommand>(drawings.at(index).command).tool;
    };
    check(toolAt(0) == ui::PaintTool{ui::LinearGradientBrush{first, second, bounds, 90, ui::BrushWrap::FlipXY}},
          "Linear gradient should preserve bounds, transparency, clockwise angle, and the wrap mode");
    check(toolAt(1) == ui::PaintTool{ui::RadialGradientBrush{first, second, bounds, ui::RadialShape::Rectangle,
                                                         openstone::green::Point{5, 0}, {0.4F, 1.0F}}},
          "Radial gradient should preserve the optional center and clamp the focus values");
    check(toolAt(2) == ui::PaintTool{ui::HatchBrush{first, second, ui::HatchStyle::Wave}},
          "Hatch should preserve the two colors and parse the style by name");
    const auto& polygonBrush = std::get<ui::GraphDrawCommand>(drawings.at(3).command).tool;
    check(polygonBrush == ui::PaintTool{ui::TextureBrush{pixels, ui::BrushWrap::FlipXY}},
          "A polygon should retain the shared Bitmap and texture wrapping");

    const auto pulses = graph.inspectOutput(rect).triggers;
    graph.setValue(wrap, std::string{"4"});
    check(std::get<ui::LinearGradientBrush>(std::get<ui::PaintTool>(graph.readValue(linear))).wrap == ui::BrushWrap::Clamp &&
          graph.inspectOutput(rect).triggers == pulses + 1,
          "A wrap change should reach the drawing with one trigger");
    check(ui::parseHatchStyle("50") == ui::HatchStyle::LargeCheckerBoard &&
          ui::parseHatchStyle("largegrid") == ui::HatchStyle::Cross &&
          ui::parseHatchStyle("unknown") == ui::HatchStyle::Horizontal,
          "Hatch should support numeric IDs, aliases, and a stable default");
}

void fontAttributesWireAndTriggerIndependently()
{
    using openstone::green::Type;
    using openstone::green::Value;
    using openstone::yellow::Font;
    using openstone::yellow::FontStyle;

    Schematic schematic;
    const int font = schematic.addNode(PrimitiveKind::Font, 0, 0);
    const int attrs = schematic.addNode(PrimitiveKind::FontAttr, 200, 0);
    const int style = schematic.addNode(PrimitiveKind::String, -200, 0, std::string{"Italic BOLD underline-strikeout"});
    const int size = schematic.addNode(PrimitiveKind::Float, -200, 80, 1.25F);
    const int face = schematic.addNode(PrimitiveKind::String, -200, 160, std::string{"Requested Face"});
    const int alternate = schematic.addNode(PrimitiveKind::String, -200, 240, std::string{"Fallback Face"});
    const int text = schematic.addNode(PrimitiveKind::String, 400, 0);
    const int sizeGate = schematic.addNode(PrimitiveKind::TriggerSwitch, 400, 80);

    check(schematic.readValue(font) == Value{Font{}}, "An unconnected Font should use its defaults");
    schematic.connect({face, font, 0});
    schematic.connect({size, font, 1});
    schematic.connect({style, font, 2});
    schematic.connect({alternate, font, 3});
    schematic.connect({font, attrs, 0});

    const Font expected{"Requested Face", 1.25F, FontStyle{true, true, true, true}, "Fallback Face"};
    check(schematic.inspectLink({font, attrs, 0}).value == Value{expected},
          "A Font link should preserve both faces, the size and all style flags");

    const SchematicLink faceLink{attrs, text, 0, 0};
    const SchematicLink styleLink{attrs, text, 0, 2};
    const SchematicLink sizeLink{attrs, sizeGate, 0, 1};
    schematic.connect(faceLink);
    schematic.connect(styleLink);
    schematic.connect(sizeLink);
    check(schematic.outputType(sizeGate) == Type::Float && schematic.readValue(sizeGate) == Value{1.25F},
          "Template inference should use the FontAttr Size output's Float type");
    check(schematic.readValue(text) == Value{std::string{"Requested Facebolditalicunderlinestrike"}},
          "Face and Style should coexist as separate links to the same String input");
    check(schematic.inspectLink(faceLink).triggers == 1 && schematic.inspectLink(styleLink).triggers == 1,
          "Adding a link from one output should pulse only the new link");

    const auto priorTriggers = schematic.inspectOutput(attrs, 1).triggers;
    schematic.setValue(size, 2.0F);
    check(schematic.readValue(sizeGate) == Value{2.0F} &&
          schematic.inspectOutput(attrs, 1).triggers == priorTriggers + 1 &&
          schematic.inspectLink(sizeLink).triggers == 2 && schematic.inspectLink(faceLink).triggers == 2 &&
          schematic.inspectLink(styleLink).triggers == 2,
          "An updated Font should deliver one pulse through each FontAttr output");
}

void stringFormatRoundTrip()
{
    using openstone::green::Value;
    using openstone::yellow::HorizontalAlignment;
    using openstone::yellow::StringFormat;
    using openstone::yellow::VerticalAlignment;

    Schematic schematic;
    const int format = schematic.addNode(PrimitiveKind::StringFormat, 200, 0);
    const int attributes = schematic.addNode(PrimitiveKind::StrFmtAttr, 400, 0);
    const int copy = schematic.addNode(PrimitiveKind::StringFormat, 600, 0);
    check(schematic.readValue(format) == Value{StringFormat{}},
          "String Format should default to left/top alignment with wrapping and horizontal LTR text");

    const int style = schematic.addNode(PrimitiveKind::String, 0, 0, std::string{"Vertical Nowrap-RightToLeft"});
    const int horizontal = schematic.addNode(PrimitiveKind::String, 0, 80, std::string{" Right "});
    const int vertical = schematic.addNode(PrimitiveKind::String, 0, 160, std::string{"Bottom"});
    schematic.connect({style, format, 0});
    schematic.connect({horizontal, format, 1});
    schematic.connect({vertical, format, 2});
    schematic.connect({format, attributes, 0});
    schematic.connect({attributes, copy, 0, 0});
    schematic.connect({attributes, copy, 1, 1});
    schematic.connect({attributes, copy, 2, 2});

    const StringFormat expected{HorizontalAlignment::Right, VerticalAlignment::Bottom, false, true, true};
    check(schematic.readValue(format) == Value{expected} && schematic.readValue(copy) == Value{expected},
          "StrFmtAttr should preserve all format settings when wired into another String Format");

    schematic.setValue(style, std::string{"righttoleft"});
    auto updated = expected;
    updated.wrap = true;
    updated.verticalText = false;
    check(schematic.inspectOutput(copy).value == Value{updated},
          "Right-to-left reading order should work independently of wrapping and vertical text");
    check(schematic.inspectLink({attributes, copy, 2, 2}).triggers == 2,
          "Changes should propagate an event through the Vert output");
}

void measureTextPullsInputsAndForwardsTriggers()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    using openstone::green::Value;
    using Bounds = openstone::yellow::Area;
    using Typeface = openstone::yellow::Font;
    using Format = openstone::yellow::StringFormat;
    int calls = 0;
    std::string measuredText;
    Bounds limit;
    Typeface typeface;
    Format format;
    Schematic root({}, {}, [&](std::string_view text, Bounds area, const Typeface& font, const Format& options) {
        ++calls;
        measuredText = text;
        limit = area;
        typeface = font;
        format = options;
        return Bounds{area.x + 1, area.y + 2, font.size, 3};
    });
    const int module = root.addNode(Module, 0, 0);
    auto& graph = *root.module(module);
    const auto add = [&](PrimitiveKind kind, std::optional<Value> value = {}) { return graph.addNode(kind, 0, 0, value); };
    const int measure = add(MeasureText);
    const int text = add(String, std::string{"first"});
    const int area = add(String, std::string{"3,5,20,10"});
    const int font = add(Font);
    const int size = add(Float, 2.0F);
    const int options = add(StringFormat);
    const int style = add(String, std::string{"nowrap vertical righttoleft"});
    const int right = add(String, std::string{"right"});

    check(graph.inputType(measure, 0) == Type::String && graph.inputType(measure, 1) == Type::Area &&
          graph.inputType(measure, 2) == Type::Font && graph.inputType(measure, 3) == Type::StringFormat &&
          graph.outputType(measure, 0) == Type::Area, "Measure Text must expose the reference connectors");
    graph.connect({text, measure, 0});
    check(graph.readValue(measure) == Value{Bounds{}} && calls == 0, "Missing MaxArea should give an empty Area");
    graph.connect({size, font, 1});
    graph.connect({style, options, 0});
    graph.connect({right, options, 1});
    graph.connect({area, measure, 1});
    graph.connect({font, measure, 2});
    graph.connect({options, measure, 3});
    const auto pulses = graph.inspectOutput(measure).triggers;
    check(calls == 1 && measuredText == "first" && limit == Bounds{3, 5, 20, 10} && typeface.size == 2 &&
          format.verticalText && format.rightToLeft && !format.wrap &&
          format.horizontal == openstone::yellow::HorizontalAlignment::Right,
          "A pull should pass all layout inputs to the root text measurement backend");
    check(graph.readValue(measure) == Value{Bounds{4, 7, 2, 3}},
          "The measured Area should pass through the output unchanged");

    graph.setValue(text, std::string{"second"});
    graph.setValue(area, std::string{"4,5,20,10"});
    graph.setValue(size, 4.0F);
    graph.setValue(style, std::string{});
    check(calls == 2, "Input changes should forward triggers without measuring until a value is read");
    const auto updated = graph.inspectOutput(measure);
    check(updated.triggers == pulses + 4 && updated.value == Value{Bounds{5, 7, 4, 3}} &&
          measuredText == "second" && format.wrap && !format.verticalText,
          "A new pull should reflect updated text, bounds, font, and format");
    graph.setValue(text, std::string{});
    check(graph.readValue(measure) == Value{Bounds{}} && calls == 3,
          "Empty text should produce an empty Area without calling the backend");
}

void bitmapLoadsOnDemandAndOwnsItsPixels()
{
    using openstone::green::Value;
    using openstone::yellow::Bitmap;

    const Bitmap image(2, 1, {64, 200, 195, 128, 255, 0, 0, 255});
    int loads = 0;
    Schematic schematic([&](std::string_view path) {
        ++loads;
        if (path != "image.png") {
            throw std::runtime_error("Invalid image");
        }
        return image;
    });
    const int bitmap = schematic.addNode(PrimitiveKind::Bitmap, 200, 0);
    const int file = schematic.addNode(PrimitiveKind::String, 0, 0, std::string{"image.png"});
    const int load = schematic.addNode(PrimitiveKind::Trigger, 0, 80);
    const int clear = schematic.addNode(PrimitiveKind::Trigger, 0, 160);
    const int width = schematic.addNode(PrimitiveKind::Int, 400, 0);
    schematic.connect({file, bitmap, 0});
    schematic.connect({load, bitmap, 1}, LinkNotification::Silent);
    schematic.connect({clear, bitmap, 2}, LinkNotification::Silent);
    schematic.connect({bitmap, width, 0, 1});

    check(loads == 0 && std::get<Bitmap>(schematic.readValue(bitmap)).empty(),
          "Connecting Path should leave the Bitmap empty until Load fires");

    schematic.fireTrigger(load);
    auto snapshot = std::get<Bitmap>(schematic.readValue(bitmap));
    check(loads == 1 && snapshot == image && schematic.readValue(width) == Value{std::int32_t{2}} &&
          schematic.readValue(bitmap, 2) == Value{std::int32_t{1}},
          "Load should commit the pixels and both dimension outputs");

    const auto beforeFailure = schematic.inspectOutput(bitmap).triggers;
    schematic.setValue(file, std::string{"invalid.png"});
    check(loads == 1, "Changing Path should leave the loaded bitmap in place");
    bool failed = false;
    try {
        schematic.fireTrigger(load);
    } catch (const std::runtime_error&) {
        failed = true;
    }
    check(failed && loads == 2 && schematic.readValue(bitmap) == Value{image} &&
          schematic.inspectOutput(bitmap).triggers == beforeFailure,
          "A failed Load should keep the image and emit no Bitmap output pulse");

    schematic.fireTrigger(clear);
    check(std::get<Bitmap>(schematic.readValue(bitmap)).empty() &&
          schematic.readValue(width) == Value{std::int32_t{0}} &&
          schematic.readValue(bitmap, 2) == Value{std::int32_t{0}} && snapshot.rgba()[3] == 128,
          "Clear should reset the node and dimensions while existing copies retain their pixels");
}

void bitmapArrayLoadsAndReplacesIndexedImages()
{
    using openstone::green::Type;
    using openstone::green::Value;
    using openstone::yellow::Bitmap;
    using openstone::yellow::BitmapArray;

    const Bitmap first(1, 1, {64, 200, 195, 128});
    const Bitmap second(2, 1, {255, 0, 0, 255, 0, 0, 255, 255});
    int attempts = 0;
    Schematic schematic([&](std::string_view path) -> Bitmap {
        ++attempts;
        if (path == "first.png") {
            return first;
        }
        if (path == "second.png") {
            return second;
        }
        throw std::runtime_error("Invalid image");
    });
    const int array = schematic.addNode(PrimitiveKind::BitmapArrayFromFile, 200, 0);
    const int path = schematic.addNode(PrimitiveKind::String, 0, 0, std::string{"first.png"});
    const int index = schematic.addNode(PrimitiveKind::Int, 0, 80, std::int32_t{3});
    const int load = schematic.addNode(PrimitiveKind::Trigger, 0, 160);
    const int clear = schematic.addNode(PrimitiveKind::Trigger, 0, 240);
    const int counter = schematic.addNode(PrimitiveKind::TriggerCounter, 400, 0);
    schematic.connect({path, array, 0});
    schematic.connect({index, array, 1});
    schematic.connect({load, array, 2}, LinkNotification::Silent);
    schematic.connect({clear, array, 3}, LinkNotification::Silent);
    schematic.connect({array, counter, 0, 1}, LinkNotification::Silent);

    const auto readArray = [&] { return std::get<BitmapArray>(schematic.readValue(array)); };
    check(schematic.outputType(array) == Type::BitmapArray && readArray().size() == 0 && attempts == 0,
          "Wiring Path and Index alone should leave the Bitmap Array empty");

    schematic.fireTrigger(load);
    const auto snapshot = readArray();
    check(snapshot.size() == 4 && snapshot.entries().size() == 1 && snapshot.at(0).empty() &&
          snapshot.at(3) == first && schematic.readValue(counter) == Value{std::int32_t{1}},
          "Loading index 3 should leave earlier slots empty and send Load complete");

    schematic.setValue(index, std::int32_t{0});
    schematic.loadBitmap(array, "second.png");
    schematic.setValue(index, std::int32_t{3});
    schematic.setValue(path, std::string{"second.png"});
    schematic.fireTrigger(load);
    const auto loaded = readArray();
    check(loaded.size() == 4 && loaded.entries().size() == 2 && loaded.at(0) == second &&
          loaded.at(3) == second && snapshot.at(3) == first && snapshot.at(0).empty(),
          "Loading an existing index should replace only that slot and preserve older snapshots");

    schematic.setValue(path, std::string{"invalid.png"});
    bool failed = false;
    try {
        schematic.fireTrigger(load);
    } catch (const std::runtime_error&) {
        failed = true;
    }
    schematic.setValue(index, std::int32_t{-1});
    bool invalidIndex = false;
    try {
        schematic.loadBitmap(array, "first.png");
    } catch (const std::invalid_argument&) {
        invalidIndex = true;
    }
    check(failed && invalidIndex && attempts == 4 && readArray() == loaded &&
          schematic.readValue(counter) == Value{std::int32_t{3}},
          "Failed loads must keep the array and suppress Load complete, and invalid indices must skip decoding");

    schematic.fireTrigger(clear);
    check(readArray().size() == 0 && schematic.inspectOutput(array).triggers == 4 &&
          schematic.readValue(counter) == Value{std::int32_t{3}},
          "Clear must emit an empty array without firing Load complete");
}

void bitmapToolsReadAndStoreTriggeredSnapshots()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    using openstone::green::Value;
    using Image = openstone::yellow::Bitmap;
    using openstone::yellow::BitmapArray;

    const Image source(2, 2, {255, 0, 0, 255, 0, 0, 255, 0, 255, 0, 0, 255, 0, 0, 255, 0});
    const Image replacement(1, 1, {42, 64, 200, 160});
    int saves = 0;
    Schematic root([&](std::string_view path) { return path == "source.png" ? source : replacement; },
        {.save = [&](std::string_view path, std::string_view encoding, const Image& bitmap) {
            if (path == "invalid.png") {
                throw std::runtime_error("Write failed");
            }
            check(path == "result.png" && encoding == "png" && bitmap == source,
                  "Bitmap Save should pass the full path, encoder, and immutable pixels to the backend");
            ++saves;
        }});
    const int module = root.addNode(Module, 0, 0);
    auto& graph = *root.module(module);
    const auto add = [&](PrimitiveKind kind, std::optional<Value> value = {}) { return graph.addNode(kind, 0, 0, value); };
    const auto link = [&](int from, int to, int input) { graph.connect({from, to, input}, LinkNotification::Silent); };
    const int loader = add(PrimitiveKind::Bitmap);
    const int resize = add(BitmapResize);
    const int sample = add(BitmapSampleAndHold);
    const int size = add(BitmapSize);
    const int bounds = add(BitmapBounds);
    const int pixel = add(BitmapGetPixel);
    const int save = add(BitmapSave);
    const int array = add(BitmapArrayFromBitmap);
    const int capture = add(Trigger);
    const int clear = add(Trigger);
    const int scale = add(Float, 0.5F);
    const int index = add(Int, std::int32_t{3});
    const int x = add(Int, std::int32_t{1});
    const int path = add(String, std::string{"result.png"});
    const int encoder = add(String, std::string{"png"});

    for (int dest : {resize, sample, size, bounds, pixel, save, array}) {
        link(loader, dest, 0);
    }
    link(scale, resize, 1);
    link(index, array, 1);
    link(x, pixel, 1);
    link(path, save, 1);
    link(encoder, save, 3);
    link(capture, resize, 2);
    link(capture, sample, 1);
    link(capture, pixel, 3);
    link(capture, save, 2);
    link(capture, array, 2);
    link(clear, array, 4);
    check(graph.inputType(array, 3) == Type::String &&
          graph.inputType(graph.addNode(BitmapArrayFromFile, 0, 0), 4) == Type::String,
          "Both array builders should expose an encoder input");

    graph.loadBitmap(loader, "source.png");
    check(graph.readValue(size, 0) == Value{std::int32_t{2}} &&
          graph.readValue(size, 1) == Value{std::int32_t{2}} &&
          graph.readValue(bounds) == Value{openstone::yellow::Area{0, 0, 2, 2}},
          "Bitmap Size and Bitmap Bounds should reflect the currently wired pixel dimensions");
    check(std::get<Image>(graph.readValue(sample)).empty() && graph.inspectOutput(resize, 1).triggers == 0 &&
          graph.inspectOutput(pixel).triggers == 0 && saves == 0,
          "Changing the source image should leave triggered operations idle");

    graph.fireTrigger(capture);
    const auto snapshot = std::get<BitmapArray>(graph.readValue(array));
    check(graph.readValue(resize) == Value{Image(1, 1, {255, 0, 0, 128})} &&
          graph.inspectOutput(resize, 1).triggers == 1 &&
          graph.readValue(sample) == Value{source} && graph.inspectOutput(sample, 1).triggers == 1,
          "Resize must preserve straight alpha colors and Sample must capture the image before pulsing");
    check(graph.readValue(pixel) == Value{openstone::yellow::Color{0x000000FF}} && saves == 1 &&
          graph.inspectOutput(save).triggers == 1 && snapshot.size() == 4 &&
          snapshot.at(2).empty() && snapshot.at(3) == source && graph.inspectOutput(array, 1).triggers == 1,
          "Get Pixel should read straight ARGB and Bitmap Array should insert into a sparse index");

    graph.loadBitmap(loader, "replacement.png");
    check(graph.readValue(size, 0) == Value{std::int32_t{1}} && graph.readValue(sample) == Value{source},
          "Changing the source should update Size and leave the captured bitmap intact");
    graph.fireTrigger(clear);
    check(std::get<BitmapArray>(graph.readValue(array)).size() == 0 && snapshot.at(3) == source &&
          graph.inspectOutput(array, 1).triggers == 1,
          "Clear should empty the array without sending a Set completion pulse");

    graph.setValue(path, std::string{"invalid.png"});
    bool failed = false;
    try {
        graph.fireTrigger(capture);
    } catch (const std::runtime_error&) {
        failed = true;
    }
    check(failed && graph.readValue(pixel) == Value{openstone::yellow::Color{}} &&
          graph.inspectOutput(save).triggers == 1,
          "Out-of-range pixels should read as transparent and failed writes should suppress Saved");
}

void bitmapGetAtCopiesTheSelectedSlotAndReportsCompletion()
{
    using enum PrimitiveKind;
    using openstone::green::Value;
    using Image = openstone::yellow::Bitmap;
    using openstone::yellow::BitmapArray;
    using Shade = openstone::yellow::Color;
    const Image source(1, 1, {1, 2, 3, 255});
    Schematic graph([&](std::string_view) { return source; });
    const auto add = [&](PrimitiveKind kind, std::optional<Value> value = {}) { return graph.addNode(kind, 0, 0, value); };
    const auto link = [&](int from, int to, int input) { graph.connect({from, to, input}, LinkNotification::Silent); };
    const int array = add(BitmapArrayFromFile);
    const int index = add(Int, std::int32_t{3});
    const int get = add(Trigger);
    const int extract = add(BitmapGetAt);
    const int set = add(Trigger);
    const int edit = add(BitmapSetPixel);
    const int color = add(Color, Shade{0x00C84A28});
    const auto result = [&] { return std::get<Image>(graph.readValue(extract)); };
    link(index, array, 1);
    link(array, extract, 0);
    link(index, extract, 1);
    link(get, extract, 2);
    link(extract, edit, 0);
    link(color, edit, 3);
    link(set, edit, 4);

    graph.loadBitmap(array, "source.png");
    check(result().empty() && graph.inspectOutput(extract, 1).triggers == 0,
          "Loading the array must leave Bitmap Get At idle until Get fires");
    graph.fireTrigger(get);
    check(result() == source && graph.inspectOutput(extract).triggers == 1 &&
          graph.inspectOutput(extract, 1).triggers == 1,
          "Get must publish the selected bitmap and pulse Done");
    graph.fireTrigger(set);
    check(result().pixelArgb(0, 0) == 0x00C84A28 &&
          std::get<BitmapArray>(graph.readValue(array)).at(3) == source &&
          graph.inspectOutput(extract).triggers == 1 && graph.inspectOutput(extract, 1).triggers == 1,
          "Editing a retrieved bitmap must preserve stored pixels and both output trigger counts");

    std::uint64_t pulses = 1;
    for (const std::int32_t invalid : {2, -1, 4}) {
        graph.setValue(index, std::int32_t{3});
        graph.fireTrigger(get);
        ++pulses;
        check(result() == source, "Getting the same slot again must return the original pixels");
        graph.setValue(index, invalid);
        check(result() == source, "Changing Index must leave the last result available");
        graph.fireTrigger(get);
        ++pulses;
        check(result().empty() && graph.inspectOutput(extract).triggers == pulses &&
              graph.inspectOutput(extract, 1).triggers == pulses,
              "A gap or invalid index must publish an empty bitmap and pulse Done");
    }
}

void setPixelUpdatesLiveWiresAndPreservesImageSnapshots()
{
    using enum PrimitiveKind;
    using openstone::green::Value;
    using Image = openstone::yellow::Bitmap;
    using Shade = openstone::yellow::Color;
    using openstone::yellow::BitmapDrawCommand;
    const Image source(2, 2, {1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255, 10, 11, 12, 255});
    openstone::Session session([&](std::string_view) { return source; });
    const int module = session.graph().addNode(Module, 0, 0);
    check(session.enterModule(module), "Enter a module to render its bitmap");
    auto& graph = session.graph();
    const auto add = [&](PrimitiveKind kind, std::optional<Value> value = {}) { return graph.addNode(kind, 0, 0, value); };
    const auto link = [&](int from, int to, int input) { graph.connect({from, to, input}, LinkNotification::Silent); };
    const int loader = add(Bitmap);
    const int edit = add(BitmapSetPixel);
    const int read = add(BitmapGetPixel);
    const int sample = add(BitmapSampleAndHold);
    const int set = add(Trigger);
    const int get = add(Trigger);
    const int x = add(Int, std::int32_t{1});
    const int y = add(Int, std::int32_t{1});
    const int color = add(Color, Shade{0x80AABBCC});
    const int mgui = add(ModuleGui);
    const int draw = add(BitmapDraw);
    const int area = add(String, std::string{"0,0,2,2"});
    for (const auto node : {edit, read, sample}) {
        link(loader, node, 0);
    }
    link(set, edit, 4);
    link(get, read, 3);
    link(get, sample, 1);
    for (const auto node : {edit, read}) {
        link(x, node, 1);
        link(y, node, 2);
    }
    link(mgui, draw, 0);
    link(area, draw, 1);
    link(loader, draw, 2);
    graph.loadBitmap(loader, "source.png");
    graph.fireTrigger(get);
    check(graph.readValue(read) == Value{Shade{0xFF0A0B0C}}, "Get Pixel should read the original color");
    graph.fireTrigger(set);
    check(graph.readValue(loader) == Value{source}, "Set Pixel needs a wired Color value");

    link(color, edit, 3);
    session.copyNode(loader);
    const auto view = graph.frontPanel()->view;
    const auto paint = [&] { return std::get<BitmapDrawCommand>(graph.panelDrawings(view).at(0).command).bitmap; };
    const auto originalFrame = paint();
    const auto live = std::get<Image>(graph.readValue(loader));
    const auto loaderPulses = graph.inspectOutput(loader).triggers;
    const auto drawPulses = graph.inspectOutput(draw).triggers;
    graph.fireTrigger(set);
    const auto firstEdit = paint();
    check(live.pixelArgb(1, 1) == 0x80AABBCC && live.pixelArgb(0, 0) == 0xFF010203 &&
          firstEdit == live && originalFrame == source && graph.readValue(sample) == Value{source},
          "Set Pixel should replace just one straight-ARGB pixel while retaining earlier snapshots");
    check(graph.inspectOutput(loader).triggers == loaderPulses && graph.inspectOutput(draw).triggers == drawPulses &&
          graph.readValue(read) == Value{Shade{0xFF0A0B0C}},
          "Set Pixel must not pulse the bitmap source, View, or triggered reader");
    graph.fireTrigger(get);
    check(graph.readValue(read) == Value{Shade{0x80AABBCC}},
          "A later Get Pixel on another wire should read the edit");

    graph.setValue(color, Shade{0xFF112233});
    check(firstEdit == live, "Changing Color alone must not write another pixel");
    graph.setValue(x, std::int32_t{-1});
    graph.fireTrigger(set);
    graph.setValue(x, std::int32_t{1});
    graph.setValue(y, std::int32_t{2});
    graph.fireTrigger(set);
    check(firstEdit == live, "Out-of-range coordinates must preserve the image");
    graph.setValue(y, std::int32_t{1});
    graph.fireTrigger(set);
    check(live.pixelArgb(1, 1) == 0xFF112233 && firstEdit.pixelArgb(1, 1) == 0x80AABBCC,
          "A second write must preserve the previously published drawing");
    const int pasted = session.pasteNode(0, 0, false);
    check(graph.readValue(pasted) == Value{source}, "Pasting a bitmap must restore the copied pixels");
}

void bitmapCanvasesRenderThroughTheViewChain()
{
    using enum PrimitiveKind;
    using openstone::green::Value;
    using Bounds = openstone::yellow::Area;
    using Image = openstone::yellow::Bitmap;
    using openstone::yellow::DrawCommand;
    using openstone::yellow::PanelDrawing;
    using openstone::yellow::View;
    const Image source(2, 2, std::vector<std::uint8_t>(16, 255));
    const Image pixels(2, 2, std::vector<std::uint8_t>(16, 64));
    int renders = 0;
    bool failRender = false;

    Schematic graph([&](std::string_view) { return source; },
        {.render = [&](const openstone::BitmapCanvasState& state, const View& view, const Image& background,
                       const std::vector<PanelDrawing>& drawings) -> Image {
            check(drawings.size() == 1 && std::get<DrawCommand>(drawings.front().command).area == Bounds{0, 0, 1, 1},
                  "Only the shapes wired to the active bitmap View should reach the renderer");
            check(view.bounds == Bounds{0, 0, 1, 1} && state.width == 2 && state.height == 2 && state.gridStep == 2,
                  "Grid Step should map pixels to the bitmap View's grid dimensions");
            check(renders < 3 ? background.empty() : background == source,
                  "Create and Redraw should start transparent and Draw to Bitmap should provide the source image");
            ++renders;
            return failRender ? Image{} : pixels;
        }});
    const auto add = [&](PrimitiveKind kind, std::optional<Value> value = {}) { return graph.addNode(kind, 0, 0, value); };
    const auto link = [&](int from, int to, int input, int output = 0) {
        graph.connect({from, to, input, output}, LinkNotification::Silent);
    };
    const int create = add(BitmapCreate);
    const int drawTo = add(DrawToBitmap);
    const int loader = add(PrimitiveKind::Bitmap);
    const int dimension = add(Int, std::int32_t{2});
    const int sizeInGrid = add(Float, 2.0F);
    const int area = add(String, std::string{"0,0,1,1"});
    const int createPulse = add(Trigger);
    const int redrawPulse = add(Trigger);
    const int drawPulse = add(Trigger);

    for (const int canvas : {create, drawTo}) {
        const int rect = add(Rect);
        link(canvas, rect, 0, 1);
        link(area, rect, 1);
    }
    link(dimension, create, 0);
    link(dimension, create, 1);
    link(sizeInGrid, create, 2);
    link(createPulse, create, 3);
    link(redrawPulse, create, 4);
    link(loader, drawTo, 0);
    link(sizeInGrid, drawTo, 1);
    link(drawPulse, drawTo, 2);

    graph.fireTrigger(redrawPulse);
    check(renders == 0 && std::get<Image>(graph.readValue(create)).empty(), "Redraw must wait for Create");
    graph.fireTrigger(createPulse);
    check(renders == 1 && graph.readValue(create) == Value{pixels} && graph.inspectOutput(create, 2).triggers == 1,
          "Create should publish the first painted image before Created");
    graph.setValue(dimension, std::int32_t{4});
    graph.fireTrigger(redrawPulse);
    check(renders == 2 && graph.readValue(create) == Value{pixels} && graph.inspectOutput(create).triggers == 2 &&
          graph.inspectOutput(create, 2).triggers == 1,
          "Redraw should reuse the created size and refresh the bitmap while Created is reserved for Create");
    failRender = true;
    bool failed = false;
    try {
        graph.fireTrigger(redrawPulse);
    } catch (const std::runtime_error&) {
        failed = true;
    }
    check(failed && graph.readValue(create) == Value{pixels} && graph.inspectOutput(create).triggers == 2,
          "A failed render should retain the last successful output");
    failRender = false;
    graph.loadBitmap(loader, "source.png");
    graph.fireTrigger(drawPulse);
    check(renders == 4 && graph.readValue(drawTo) == Value{pixels} && graph.readValue(loader) == Value{source} &&
          graph.inspectOutput(drawTo, 2).triggers == 1, "Draw to Bitmap should publish a separate image and a completion pulse");

    Schematic blank;
    const auto emptyCanvas = blank.addNode(BitmapCreate, 0, 0);
    const auto extent = blank.addNode(Int, 0, 0, std::int32_t{4});
    const auto pulse = blank.addNode(Trigger, 0, 0);
    blank.connect({extent, emptyCanvas, 0});
    blank.connect({extent, emptyCanvas, 1});
    blank.connect({pulse, emptyCanvas, 3});
    check(blank.readValue(emptyCanvas) == Value{Image(4, 4, std::vector<std::uint8_t>(64))},
          "Create Bitmap should produce transparent pixels without a drawing backend when the View is empty");
}

void svgRescalesTheLoadedDocumentAndSeparatesOutcomes()
{
    using enum PrimitiveKind;
    using openstone::SvgDocument;
    using openstone::SvgRasterLayout;
    using openstone::green::Type;
    using openstone::green::Value;
    using Image = openstone::yellow::Bitmap;

    int loads = 0;
    int renders = 0;
    SvgRasterLayout last{};
    Schematic root({}, {.loadSvg = [&](std::string_view path) -> SvgDocument {
        ++loads;
        if (path != "art.svg") {
            throw std::runtime_error("Invalid SVG");
        }
        return {8, 4, [&](const SvgRasterLayout& layout) {
            ++renders;
            last = layout;
            return Image(layout.width, layout.height, std::vector<std::uint8_t>(layout.width * layout.height * 4, 127));
        }};
    }});
    const auto module = root.addNode(Module, 0, 0);
    auto& graph = *root.module(module);
    const auto add = [&](PrimitiveKind kind, std::optional<Value> value = {}) { return graph.addNode(kind, 0, 0, value); };
    const int svg = add(Svg);
    const int path = add(String, std::string{"art.svg"});
    const int load = add(Trigger);
    const int width = add(Float, 24.25F);
    const int height = add(Float, 20.0F);
    const int xMode = add(String, std::string{"fit"});
    const int yMode = add(String, std::string{"aspect"});
    const int size = add(BitmapSize);
    const auto image = [&] { return std::get<Image>(graph.readValue(svg)); };
    const auto pulses = [&](int output) { return graph.inspectOutput(svg, output).triggers; };
    graph.connect({path, svg, 0});
    graph.connect({svg, size, 0});
    graph.connect({load, svg, 5}, LinkNotification::Silent);
    check(graph.inputType(svg, 1) == Type::Float && graph.inputType(svg, 2) == Type::Float &&
          graph.outputType(svg, 1) == Type::Trigger && graph.outputType(svg, 2) == Type::Trigger &&
          loads == 0 && image().empty(), "SVG should stay empty until Load while exposing the reference connectors");

    graph.fireTrigger(load);
    check(loads == 1 && renders == 1 && image().width() == 8 && image().height() == 4 &&
          pulses(0) == 1 && pulses(1) == 1 && pulses(2) == 0 && std::get<std::int32_t>(graph.readValue(size)) == 8,
          "Load should publish the natural-size image and then fire Success");
    graph.connect({width, svg, 1});
    graph.connect({height, svg, 2});
    check(image().width() == 25 && image().height() == 20 && last.x == 8.125 && last.y == 8 &&
          last.scaleX == 1 && last.scaleY == 1, "Center should keep native size with rounded-up bitmap dimensions");
    graph.connect({xMode, svg, 3});
    graph.connect({yMode, svg, 4});
    check(last.x == 0 && last.y == 3.9375 && last.scaleX == 3.03125 && last.scaleY == last.scaleX &&
          loads == 1 && renders == 5 && pulses(1) == 1, "Y Aspect should inherit X Fit without reloading the file");

    graph.setValue(xMode, std::string{"aspect"});
    check(last.scaleX == 1 && last.scaleY == 1 && last.x == 8.125 && last.y == 8,
          "Two Aspect modes should use the native scale");
    graph.setValue(yMode, std::string{"FIT"});
    check(last.x == -7.875 && last.y == 0 && last.scaleX == 5 && last.scaleY == 5,
          "X Aspect should inherit Y Fit and center the cropped artwork");
    graph.setValue(path, std::string{"missing.svg"});
    const auto beforeFailure = image();
    const auto bitmapPulses = pulses(0);
    graph.fireTrigger(load);
    check(loads == 2 && pulses(0) == bitmapPulses && pulses(2) == 1 && pulses(1) == 1 && image() == beforeFailure,
          "An invalid replacement should fire only Error and preserve the rendered image");
    graph.setValue(width, -2.0F);
    check(loads == 2 && image().width() == 1 && pulses(0) == bitmapPulses + 1 && pulses(1) == 1,
          "Size changes after a failed load should still use the last successful document");
    graph.setValue(width, 1e6F);
    check(image().width() == 1 && pulses(2) == 2 && pulses(0) == bitmapPulses + 1,
          "An oversized raster should fire only Error and preserve the output");
}

void busRoutesValuesAndPulsesByName()
{
    using openstone::green::Bus;
    using openstone::green::Type;
    using openstone::green::Value;

    Schematic schematic;
    const int createNames = schematic.addNode(PrimitiveKind::String, 0, 0, std::string{"number, text, pulse"});
    const int extractNames = schematic.addNode(PrimitiveKind::String, 0, 80, std::string{"pulse,text,number"});
    const int selection = schematic.addNode(PrimitiveKind::String, 0, 160, std::string{"text"});
    const int number = schematic.addNode(PrimitiveKind::Int, 0, 240, std::int32_t{7});
    const int text = schematic.addNode(PrimitiveKind::String, 0, 320, std::string{"hello"});
    const int pulse = schematic.addNode(PrimitiveKind::Trigger, 0, 400);
    const int create = schematic.addNode(PrimitiveKind::BusCreate, 200, 0);
    const int extract = schematic.addNode(PrimitiveKind::BusExtract, 400, 0);
    const int other = schematic.addNode(PrimitiveKind::BusExtract, 400, 240);
    const int counter = schematic.addNode(PrimitiveKind::TriggerCounter, 600, 0);
    const int display = schematic.addNode(PrimitiveKind::Int, 600, 80);

    schematic.connect({createNames, create, 0}, LinkNotification::Silent);
    schematic.connect({extractNames, extract, 0});
    schematic.connect({selection, other, 0});
    schematic.connect({number, create, 1});
    schematic.connect({text, create, 2});
    schematic.connect({pulse, create, 3});
    schematic.connect({create, extract, 1});
    schematic.connect({create, other, 1});
    schematic.connect({extract, counter, 0, 0}, LinkNotification::Silent);
    schematic.connect({extract, display, 0, 2});

    const Bus expected{{{"number", Type::Int}, {"text", Type::String}, {"pulse", Type::Trigger}}};
    check(schematic.inspectLink({create, extract, 1}).value == Value{expected} &&
          schematic.readValue(display) == Value{std::int32_t{7}} && schematic.readValue(other) == Value{std::string{"hello"}},
          "One bus should expose mixed channel types and let separate Extracts select them by name");
    check(schematic.readValue(counter) == Value{std::int32_t{0}},
          "Connecting a Bus should not synthesize a trigger on a Trigger channel");

    const auto numberPulses = schematic.inspectOutput(extract, 2).triggers;
    schematic.setValue(text, std::string{"updated"});
    check(schematic.inspectOutput(extract, 2).triggers == numberPulses &&
          schematic.readValue(counter) == Value{std::int32_t{0}} &&
          schematic.readValue(other) == Value{std::string{"updated"}},
          "Changing a value channel must only pulse matching Extract outputs");
    schematic.fireTrigger(pulse);
    check(schematic.readValue(counter) == Value{std::int32_t{1}} &&
          schematic.inspectOutput(extract, 2).triggers == numberPulses,
          "A trigger-only channel must deliver just its own pulse");

    schematic.setValue(createNames, std::string{"text, number, pulse"});
    schematic.setValue(extractNames, std::string{"number, pulse, text"});
    schematic.setValue(number, std::int32_t{8});
    schematic.fireTrigger(pulse);
    check(schematic.readValue(display) == Value{std::int32_t{8}} &&
          schematic.readValue(counter) == Value{std::int32_t{2}} &&
          schematic.inspectOutput(extract, 0).triggers == numberPulses + 1,
          "Reordering Create and Extract names must preserve each wire and pulse count");
    schematic.setValue(extractNames, std::string{"pulse,text"});
    check(schematic.editable(display) && schematic.outputType(extract, 0) == Type::Trigger,
          "Removing a named output should disconnect just that channel");
}

void modulesOwnTheirSchematicsAndViewports()
{
    using enum PrimitiveKind;
    openstone::Session session;
    auto& main = session.graph();
    bool rootRejected = false;
    try {
        main.addNode(ModuleGui, 0, 0);
    } catch (const std::invalid_argument&) {
        rootRejected = true;
    }
    check(rootRejected && main.nodes().empty(), "Main should reject MGUI without adding a node");

    const int left = main.addNode(Module, 10, 20);
    const int right = main.addNode(Module, 100, 20);
    main.setModuleName(left, "Instrument");
    check(main.nodes().front().inputPorts().empty() && main.nodes().front().outputPorts().empty(),
          "An empty Module has no external connectors");
    session.centerOn(5, 8);
    check(session.enterModule(left) && session.levelName(1) == "Instrument" && session.graph().nodes().empty(),
          "Entering a Module should open its own empty schematic and named breadcrumb");

    const int mgui = session.graph().addNode(ModuleGui, 0, 0);
    const int nested = session.graph().addNode(Module, 80, 100);
    session.centerOn(90, 30);
    session.zoomAt(2, 0, 0);
    check(!session.enterModule(mgui) && session.enterModule(nested) &&
          session.graph().canPlace(ModuleGui) && session.levelName(1) == "Instrument",
          "A nested Module should allow MGUI and preserve its parent breadcrumb");
    check(session.navigate(0) && session.activeViewport().x == 5 && session.enterModule(right) &&
          session.graph().nodes().empty() && session.activeViewport().zoom == 1,
          "Main and sibling Modules should keep independent contents and viewports");
    check(session.navigate(0) && session.enterModule(left) &&
          session.graph().readModulePanel(mgui).view.panel == mgui &&
          session.activeViewport().x == 90 && session.activeViewport().zoom == 2,
          "Re-entering a Module should restore its contents and viewport");
    check(session.navigate(0), "Main should still be reachable");
    session.removeNode(left);
    check(!session.enterModule(left) && session.enterModule(right),
          "Removing a Module should remove its subtree and leave sibling Modules accessible");
}

void wirelessRoutingAcrossModules()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    using openstone::green::Value;
    openstone::Schematic main;
    const int payload = main.addNode(String, 0, 0, std::string{"static"});
    const int sender = main.addNode(WirelessOutput, 0, 0);
    const int wrongLevel = main.addNode(WirelessInput, 0, 0);
    main.setWirelessLabel(sender, "Label");
    main.setWirelessLabel(wrongLevel, "Label");
    main.connect({payload, sender, 0});
    main.setTemplateType(wrongLevel, Type::String);
    check(!main.wirelessConnected(wrongLevel), "A sender should not connect at the same level");

    const int parentId = main.addNode(Module, 0, 0);
    auto& parent = *main.module(parentId);
    const int name = parent.addNode(String, 0, 0, std::string{"Label"});
    const int rx = parent.addNode(WirelessReceiver, 0, 0);
    const int display = parent.addNode(String, 0, 0);
    parent.connect({name, rx, 0});
    parent.connect({rx, display, 0});
    check(main.wirelessConnected(sender) && parent.wirelessConnected(rx) &&
          parent.readValue(display) == Value{std::string{"static"}},
          "A String-named receiver should match a label-named sender");

    const int grandchildId = parent.addNode(Module, 0, 0);
    auto& grandchild = *parent.module(grandchildId);
    const int nestedRx = grandchild.addNode(WirelessInput, 0, 0);
    const int nestedDisplay = grandchild.addNode(String, 0, 0);
    grandchild.setWirelessLabel(nestedRx, "Label");
    grandchild.connect({nestedRx, nestedDisplay, 0});
    check(grandchild.readValue(nestedDisplay) == Value{std::string{"static"}},
          "Wireless links should reach through intermediate modules");

    const int nearerName = parent.addNode(String, 0, 0, std::string{"Label"});
    const int nearerData = parent.addNode(String, 0, 0, std::string{"dynamic"});
    const int nearer = parent.addNode(WirelessLink, 0, 0);
    parent.connect({nearerName, nearer, 1});
    parent.connect({nearerData, nearer, 0});
    check(parent.readValue(display) == Value{std::string{"static"}} &&
          grandchild.readValue(nestedDisplay) == Value{std::string{"dynamic"}},
          "The dynamic sender should shadow an identically named ancestor only below its own level");
    const auto nestedPulses = grandchild.inspectOutput(nestedRx).triggers;
    parent.setValue(nearerData, std::string{"updated"});
    check(grandchild.readValue(nestedDisplay) == Value{std::string{"updated"}} &&
          grandchild.inspectOutput(nestedRx).triggers == nestedPulses + 1,
          "A matching publisher should carry each data pulse");
    parent.setValue(nearerName, std::string{"label"});
    check(grandchild.readValue(nestedDisplay) == Value{std::string{"static"}} &&
          grandchild.inspectOutput(nestedRx).triggers == nestedPulses + 2,
          "Renaming a dynamic sender should rebind to the ancestor and deliver the new value");

    parent.setValue(name, std::string{"label"});
    check(!parent.wirelessConnected(rx) && parent.readValue(display) == Value{std::string{}},
          "Changing the receiver name should drop a route with different casing");
    parent.setValue(name, std::string{"Label"});
    main.removeNode(sender);
    check(!parent.wirelessConnected(rx) && !grandchild.wirelessConnected(nestedRx) &&
          grandchild.readValue(nestedDisplay) == Value{std::string{}},
          "Removing a publisher should clear its routes");

    const int trigger = main.addNode(Trigger, 0, 0);
    const int triggerSender = main.addNode(WirelessOutput, 0, 0);
    const int triggerRx = parent.addNode(WirelessInput, 0, 0);
    const int counter = parent.addNode(TriggerCounter, 0, 0);
    main.setWirelessLabel(triggerSender, "Label");
    parent.setWirelessLabel(triggerRx, "Label");
    main.connect({trigger, triggerSender, 0}, openstone::LinkNotification::Silent);
    parent.connect({triggerRx, counter, 0}, openstone::LinkNotification::Silent);
    check(parent.outputType(triggerRx) == Type::Trigger && !parent.wirelessConnected(rx),
          "Names shared by incompatible connector types must use separate routes");
    main.fireTrigger(trigger);
    main.fireTrigger(trigger);
    check(parent.readValue(counter) == Value{std::int32_t{2}},
          "Wireless Trigger routes must preserve repeated pulses");
}

void moduleWirelessOutputsPublishAtTheModuleLevel()
{
    using enum PrimitiveKind;
    using openstone::green::Value;
    Schematic root;
    check(!root.canPlace(ModuleWirelessOutput) && !root.canPlace(ModuleWirelessLink),
          "Module wireless outputs belong inside a module");
    auto& scope = *root.module(root.addNode(Module, 0, 0));
    auto& source = *scope.module(scope.addNode(Module, 0, 0));
    auto& target = *scope.module(scope.addNode(Module, 0, 0));
    auto& outside = *root.module(root.addNode(Module, 0, 0));
    const int fixed = source.addNode(ModuleWirelessOutput, 0, 0);
    const int dynamic = source.addNode(ModuleWirelessLink, 0, 0);
    const int name = source.addNode(String, 0, 0, std::string{"dynamic"});
    const int value = source.addNode(Float, 0, 0, 0.5F);
    source.setWirelessLabel(fixed, "fixed");
    source.connect({name, dynamic, 1});
    source.connect({value, fixed, 0});
    source.connect({value, dynamic, 0});

    const auto receiver = [](Schematic& graph, std::string label) {
        const int input = graph.addNode(WirelessInput, 0, 0);
        graph.setWirelessLabel(input, std::move(label));
        graph.setTemplateType(input, openstone::green::Type::Float);
        return input;
    };
    const int fixedTarget = receiver(target, "fixed");
    const int dynamicTarget = receiver(target, "dynamic");
    const int outsideTarget = receiver(outside, "dynamic");
    check(source.wirelessConnected(fixed) && source.wirelessConnected(dynamic) &&
          target.readValue(fixedTarget) == Value{0.5F} && target.readValue(dynamicTarget) == Value{0.5F} &&
          !outside.wirelessConnected(outsideTarget), "Both variants publish to siblings below the containing level");
    const auto previous = target.inspectOutput(dynamicTarget).triggers;
    source.setValue(value, 0.75F);
    check(target.readValue(dynamicTarget) == Value{0.75F} && target.inspectOutput(dynamicTarget).triggers == previous + 1,
          "Changes in the transmitter module deliver a pulse and the new value");
    source.setValue(name, std::string{"renamed"});
    check(!target.wirelessConnected(dynamicTarget) && target.readValue(fixedTarget) == Value{0.75F},
          "Changing the dynamic name rebinds only that transmitter");
}

void moduleFrontPanelLayout()
{
    using enum PrimitiveKind;
    openstone::Schematic main;
    const int parentId = main.addNode(Module, 0, 0);
    auto& parent = *main.module(parentId);
    check(!parent.frontPanel(), "A Module without MGUI has no front panel");

    const int parentMgui = parent.addNode(ModuleGui, 0, 0);
    const int childId = parent.addNode(Module, 70, 110);
    auto& child = *parent.module(childId);
    const int childMgui = child.addNode(ModuleGui, 0, 0);
    const auto panel = parent.frontPanel();
    check(!main.nodes().front().panelHidden && panel && panel->view.bounds == openstone::defaultPanelBounds,
          "MGUI provides a front panel that starts visible at the default size");

    const auto initialPulses = child.inspectOutput(childMgui).triggers;
    const auto parentPulses = parent.inspectOutput(parentMgui).triggers;
    parent.resizePanel(childId, 8.5F, 6.0F);
    parent.resizePanel(childId, 8.5F, 6.0F);
    const auto childPanel = child.frontPanel();
    check(childPanel && childPanel->view.bounds == openstone::yellow::Area{0, 0, 8.5F, 6} &&
          child.inspectOutput(childMgui).triggers == initialPulses + 1,
          "Resizing should update the View and trigger only on a change");
    check(parent.readModulePanel(parentMgui).view.bounds == openstone::defaultPanelBounds &&
          parent.inspectOutput(parentMgui).triggers == parentPulses,
          "Resizing a child Module should only update that Module's MGUI");
    const int childPath[]{childId};
    bool rejected = false;
    try {
        main.movePanelItem(parentId, childPath, {2.5F, 3.0F});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected && !childPanel->view.attachesTo(panel->view),
          "A child with MGUI but without an incoming View must stay off the parent panel");

    const int viewTx = parent.addNode(WirelessOutput, 0, 0);
    const int viewRx = child.addNode(WirelessInput, 0, 0);
    parent.setWirelessLabel(viewTx, "View");
    child.setWirelessLabel(viewRx, "View");
    parent.connect({parentMgui, viewTx, 0});
    child.connect({viewRx, childMgui, 0});
    check(child.readModulePanel(childMgui).view.attachesTo(panel->view) &&
          child.readModulePanel(childMgui).view.panel == panel->view.panel,
          "A matching View should attach the child even when both MGUI IDs are the same");
    main.movePanelItem(parentId, childPath, {2.5F, 3.0F});
    main.setPanelHidden(parentId, true);
    check(main.nodes().front().panelHidden, "Hiding a front panel should be recorded on its Module node");
    main.setPanelHidden(parentId, false);
    const auto& entry = parent.nodes()[1];
    check(!main.nodes().front().panelHidden && entry.panelPosition == openstone::green::Point{2.5F, 3} &&
          entry.x == 70 && entry.y == 110 && child.readModulePanel(childMgui).view.bounds == childPanel->view.bounds,
          "Showing the front panel should restore child panel layout independently of schematic positions");
    child.disconnectInput(childMgui, 0);
    check(!child.readModulePanel(childMgui).view.attachesTo(panel->view),
          "Unplugging the receiver should detach the panel without removing its MGUI");
}

void modulePropertiesUseAnIndependentEditorSurface()
{
    using enum PrimitiveKind;
    using enum openstone::ModulePanelKind;
    using openstone::PropertiesVisibility;
    using openstone::green::Type;
    using openstone::green::Value;
    using Position = openstone::green::Point;
    using PanelArea = openstone::yellow::Area;
    openstone::Session session;
    auto& root = session.graph();
    check(!root.canPlace(ModulePropertiesGui), "Props belongs inside a Module");
    const int outer = root.addNode(Module, 0, 0);
    auto& graph = *root.module(outer);
    const int props = graph.addNode(ModulePropertiesGui, 0, 0);
    check(!graph.frontPanel() && graph.modulePanel(Properties)->view.panel == props &&
          root.nodes().front().propertiesVisibility == PropertiesVisibility::Closed &&
          graph.inputType(props, 0) == Type::View && graph.inputType(props, 1) == Type::Boolean &&
          graph.outputType(props) == Type::View,
          "Props alone should provide a closed properties panel with View and Mouse inputs");
    const int front = graph.addNode(ModuleGui, 0, 0);
    const int rect = graph.addNode(Rect, 0, 0);
    const int area = graph.addNode(String, 0, 0, std::string{"0,0,2,2"});
    const int mouse = graph.addNode(Boolean, 0, 0, false);
    const int move = graph.addNode(MouseMove, 0, 0);
    graph.connect({props, rect, 0});
    graph.connect({area, rect, 1});
    graph.connect({mouse, props, 1});
    graph.connect({props, move, 0});
    const auto mainView = graph.frontPanel()->view;
    const auto propertiesView = graph.modulePanel(Properties)->view;
    check(!mainView.isSamePanel(propertiesView) && graph.panelDrawings(mainView).empty() &&
          graph.panelDrawings(propertiesView).size() == 1,
          "Properties drawing commands must stay on their own View");
    check(!graph.movePanelPointer(props, {1, 1}), "Props suppresses hover motion by default");
    graph.setValue(mouse, true);
    check(graph.movePanelPointer(props, {1, 1}) && graph.readValue(move) == Value{Position{1, 1}},
          "Props Mouse should enable ordinary pointer move handling");

    const int control = graph.addNode(Module, 0, 0);
    auto& child = *graph.module(control);
    const int childFront = child.addNode(ModuleGui, 0, 0);
    const int childProps = child.addNode(ModulePropertiesGui, 0, 0);
    const int sender = graph.addNode(WirelessOutput, 0, 0);
    const int receiver = child.addNode(WirelessInput, 0, 0);
    graph.setWirelessLabel(sender, "Properties");
    child.setWirelessLabel(receiver, "Properties");
    graph.connect({front, sender, 0});
    child.connect({receiver, childFront, 0});
    child.connect({receiver, childProps, 0});
    check(child.frontPanel()->view.attachesTo(mainView) &&
          !child.modulePanel(Properties)->view.attachesTo(mainView),
          "A properties panel can attach to another properties View");
    graph.disconnectInput(sender, 0);
    graph.connect({props, sender, 0});
    check(child.frontPanel()->view.attachesTo(propertiesView) &&
          child.modulePanel(Properties)->view.attachesTo(propertiesView),
          "Both kinds of child panel should follow a matching Properties wireless link");
    const int childArea = child.addNode(GuiArea, 0, 0);
    child.connect({childProps, childArea, 0});
    session.movePanelItem(outer, std::array{control}, {2, 3}, Properties, Properties);
    session.movePanelItem(outer, std::array{control}, {5, 6}, Properties);
    check(child.readValue(childArea) == Value{PanelArea{2, 3, 16, 10}} &&
          child.frontPanel()->position == Position{5, 6},
          "A nested child keeps individual positions for its two panels");

    root.setPropertiesVisibility(outer, PropertiesVisibility::Pinned);
    session.copyNode(outer);
    const int copy = session.pasteNode(100, 100, true);
    session.resizePanel(outer, 12, 8, Properties);
    check(root.nodes().front().propertiesVisibility == PropertiesVisibility::Pinned &&
          root.nodes().back().propertiesVisibility == PropertiesVisibility::Closed &&
          graph.modulePanel(Properties)->view.bounds == PanelArea{0, 0, 12, 8} &&
          root.module(copy)->modulePanel(Properties)->view.bounds == PanelArea{0, 0, 12, 8} &&
          graph.frontPanel()->view.bounds == mainView.bounds,
          "Properties geometry should synchronize while each instance keeps its own open state and front panel");
}

void moduleActionsKeepReusablePanelsAndAnchorMargins()
{
    using enum PrimitiveKind;
    using enum openstone::ModulePanelKind;
    using openstone::AnchorBottom;
    using openstone::AnchorLeft;
    using openstone::AnchorRight;
    using openstone::green::Value;
    using Position = openstone::green::Point;
    using Bounds = openstone::yellow::Area;
    openstone::Session session;
    auto& root = session.graph();
    const int module = session.addNode(Module, 0, 0);
    session.renameNode(module, "Panel template");
    session.enableModulePanel(module, Front);
    session.enableModulePanel(module, Properties);
    session.enableModulePanel(module, Front);
    auto& graph = *root.module(module);
    check(graph.nodes().size() == 2 && graph.frontPanel() && graph.modulePanel(Properties),
          "Enabling each panel should add its own provider once");

    const int front = graph.frontPanel()->view.panel;
    const int sender = graph.addNode(WirelessOutput, 0, 0);
    graph.setWirelessLabel(sender, "Panel");
    graph.connect({front, sender, 0});
    const int control = graph.addNode(Module, 0, 0);
    auto& child = *graph.module(control);
    const int receiver = child.addNode(WirelessInput, 0, 0);
    child.setWirelessLabel(receiver, "Panel");
    const int childGui = child.addNode(ModuleGui, 0, 0);
    const int measure = child.addNode(GuiArea, 0, 0);
    child.connect({receiver, childGui, 0});
    child.connect({childGui, measure, 0});
    graph.resizePanel(control, 4, 3);
    const std::array path{control};
    session.movePanelItem(module, path, {2, 3});
    const auto mask = AnchorLeft | AnchorRight | AnchorBottom;
    session.setPanelItemAnchors(module, path, Front, Front, mask);
    check(child.readValue(measure) == Value{Bounds{2, 3, 4, 3}},
          "Changing anchors should rebase at the current panel position");
    session.resizePanel(module, 20, 12);
    check(child.readValue(measure) == Value{Bounds{2, 5, 8, 3}},
          "Opposite anchors should stretch width while a bottom anchor follows that edge");
    session.resizePanel(module, 2, 12);
    check(child.frontPanel()->view.bounds.width > 0, "Shrinking the parent should keep the anchored child usable");
    session.resizePanel(module, 20, 12);
    check(child.readValue(measure) == Value{Bounds{2, 5, 8, 3}},
          "Returning to a previous size should restore the same anchor geometry");

    check(session.enterModule(module), "Enter the panel's schematic");
    session.setPanelShownInParent(control, false);
    check(!graph.nodes().back().showPanelInParent && child.frontPanel()->position == Position{2, 5},
          "Parent visibility should preserve the local panel and its layout");
    session.navigate(0);
    session.saveModuleToToolbox(module);
    session.removeNode(module);
    const int inserted = session.insertToolboxModule(0, 100, 100);
    const auto& restored = *root.module(inserted);
    auto* restoredChild = restored.module(control);
    check(session.toolboxNames() == std::vector<std::string>{"Panel template"} && restoredChild &&
          restoredChild->frontPanel()->anchors == mask && restoredChild->frontPanel()->position == Position{2, 5} &&
          restoredChild->frontPanel()->view.bounds.width == 8 && root.nodes().back().moduleName == "Panel template",
          "A toolbox snapshot should recreate its nested wiring and panel layout after deleting the source");
    const int wrapped = session.makeModule(std::array{inserted});
    check(root.module(wrapped)->module(inserted) != nullptr,
          "Make Module should allow wrapping one selected module");
}

void synchronizedModulesShareEditsAndKeepIndependentState()
{
    using enum PrimitiveKind;
    openstone::Session session;
    const int original = session.addNode(Module, 0, 0);
    check(session.enterModule(original), "Open the original");
    const int trigger = session.addNode(Trigger, 0, 0);
    const int counter = session.addNode(TriggerCounter, 120, 0);
    session.connect({trigger, counter, 0});
    session.graph().fireTrigger(trigger);
    const int nested = session.addNode(Module, 20, 70);
    check(session.enterModule(nested), "Open the nested module");
    const int value = session.addNode(Int, 30, 15);
    session.setValue(value, std::int32_t{5});
    check(session.navigate(0), "Return to Main");

    session.copyNode(original);
    const int independent = session.pasteNode(180, 20, false);
    const int synchronized = session.pasteNode(360, 20, true);
    check(session.syncCount(original) == 1 && session.syncCount(synchronized) == 1 &&
          session.syncCount(independent) == 0,
          "Paste Synchronize should include the live source and leave ordinary copies independent");
    auto* copied = session.graph().module(synchronized);
    check(copied->links().size() == 1 &&
          copied->readValue(counter) == openstone::green::Value{std::int32_t{0}},
          "The pasted module should keep its wiring and start with fresh runtime state");
    copied->fireTrigger(trigger);
    check(copied->readValue(counter) == openstone::green::Value{std::int32_t{1}} &&
          session.graph().module(original)->readValue(counter) == openstone::green::Value{std::int32_t{2}},
          "Pulses inside a synchronized module should stay in that instance");
    check(session.enterModule(synchronized) && !session.canPasteSynchronized(),
          "A linked copy cannot be pasted into its own definition");
    check(session.enterModule(nested), "Open the nested module in the synchronized copy");
    session.setValue(value, std::int32_t{42});
    session.renameNode(value, "Depth");
    session.moveNode(value, 69, 12);
    session.copyNode(value);
    const int second = session.pasteNode(210, 18, false);
    session.connect({value, second, 0});
    check(session.navigate(0), "Return to Main after editing the synchronized copy");
    const auto* changed = session.graph().module(original)->module(nested);
    check(changed->nodes()[0].label == "Depth" && changed->nodes()[0].x == 69 &&
          changed->links().size() == 1 && changed->readValue(second) == openstone::green::Value{std::int32_t{42}} &&
          session.graph().module(independent)->module(nested)->nodes().size() == 1,
          "Nested edits in a copy should update the original and preserve an ordinary copy");
    check(session.followSync(original, false) == synchronized && session.followSync(synchronized, true) == original,
          "Following a group should work in both directions");
    session.unsynchronize(synchronized, false);
    check(session.syncCount(original) == 0 && session.enterModule(synchronized) && session.enterModule(nested),
          "Detach a synchronized copy");
    session.removeNode(second);
    check(changed->nodes().size() == 2, "Detached copies should keep their own future changes");
}

void matchingModulesCanBeSynchronizedAcrossLevels()
{
    using enum PrimitiveKind;
    openstone::Session session;
    const int original = session.addNode(Module, 0, 0);
    const int container = session.addNode(Module, 130, 0);
    session.renameNode(container, "Container");
    check(session.enterModule(original), "Open the original");
    const int discarded = session.addNode(Trigger, 30, 30);
    session.removeNode(discarded);
    const int originalValue = session.addNode(Int, 0, 0);
    check(session.navigate(0) && session.enterModule(container), "Go inside the container");
    const int matching = session.addNode(Module, 15, 50);
    check(session.enterModule(matching), "Create a match with different local IDs");
    const int matchingValue = session.addNode(Int, 0, 0);
    check(session.navigate(0), "Return to Main");
    session.synchronizeAll(original);
    check(session.syncCount(original) == 1 && session.followSync(original, false) == matching &&
          session.activeLevel() == 1, "Synchronize All and navigation should find a match across levels");
    session.unsynchronize(matching, true);
    check(session.syncCount(matching) == 0 && session.navigate(0), "Unsynchronize All should dissolve the group");

    session.startSyncPainter(original);
    check(session.enterModule(container) && session.syncPainterActive(),
          "The painter should follow the user across module navigation");
    session.paintSync(matching);
    check(session.syncCount(matching) == 1, "Paint a matching module");
    session.paintSync(matching);
    check(session.syncCount(matching) == 0, "Painting a linked copy again should detach it");
    session.paintSync(matching);
    check(session.enterModule(matching), "Open the linked copy again");
    session.setValue(matchingValue, std::int32_t{11});
    check(!session.syncPainterActive() && session.navigate(0) &&
          session.graph().module(original)->readValue(originalValue) == openstone::green::Value{std::int32_t{11}},
          "The painted module should synchronize edits even when local IDs differ");
}

void makeModulePreservesConversionsFanOutAndState()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    using openstone::green::Value;
    Schematic graph;
    const int a = graph.addNode(Float, 0, 0, 1.75F);
    const int b = graph.addNode(Float, 0, 95, 2.75F);
    const int input = graph.addNode(Int, 200, 40);
    const int output = graph.addNode(Int, 390, 100);
    const int display = graph.addNode(Int, 630, 0);
    const int counter = graph.addNode(TriggerCounter, 630, 95);
    graph.connect({a, input, 0});
    graph.connect({b, input, 0});
    graph.connect({input, output, 0});
    graph.connect({output, display, 0});
    graph.connect({output, counter, 0});
    const auto pulses = graph.readValue(counter);
    const int selection[]{output, input};
    const int wrapper = graph.makeModule(selection);
    auto& child = *graph.module(wrapper);
    const auto& node = graph.nodes().back();
    check(node.moduleInputs.size() == 2 && node.moduleOutputs.size() == 2 &&
          graph.inputType(wrapper, 0) == Type::Float && graph.outputType(wrapper, 0) == Type::Int,
          "Each crossing wire should get its own port with the original source type");
    check(child.nodes()[0].x == 0 && child.nodes()[0].y == 0 &&
          child.nodes()[1].x == 190 && child.nodes()[1].y == 60 &&
          std::ranges::contains(child.links(), SchematicLink{input, output, 0}),
          "Grouping should retain the selected nodes' relative layout and internal link");
    check(graph.readValue(display) == Value{std::int32_t{3}} && graph.readValue(counter) == pulses,
          "Grouping must preserve per-link conversion and add no synthetic pulses");
    graph.setValue(a, 5.75F);
    check(graph.readValue(display) == Value{std::int32_t{7}} &&
          graph.readValue(counter) == Value{std::get<std::int32_t>(pulses) + 1},
          "An incoming value and pulse should travel through the module once per outgoing wire");
}

void makeModuleHandlesPathsThatLeaveAndReenter()
{
    using enum PrimitiveKind;
    using openstone::green::Value;
    Schematic graph;
    const int first = graph.addNode(Int, 0, 0, std::int32_t{8});
    const int outside = graph.addNode(Int, 200, 0);
    const int last = graph.addNode(Int, 400, 0);
    const int display = graph.addNode(Int, 600, 0);
    graph.connect({first, outside, 0});
    graph.connect({outside, last, 0});
    graph.connect({last, display, 0});
    const int selection[]{first, last};
    const int wrapper = graph.makeModule(selection);
    check(graph.readValue(display) == Value{std::int32_t{8}},
          "An existing acyclic path can exit and re-enter the new module");

    const int secondDisplay = graph.addNode(Int, 600, 90);
    graph.connect({wrapper, secondDisplay, 0, 1});
    graph.module(wrapper)->setValue(first, std::int32_t{13});
    check(graph.readValue(secondDisplay) == Value{std::int32_t{13}},
          "The path should survive local type rebuilding and pulse delivery");
    bool rejected = false;
    try {
        graph.connect({wrapper, outside, 0, 1});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "Connecting the second output back to its own input path should still be rejected");
}

void makeModulePortsReinferAfterRewiring()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    using openstone::green::Value;
    Schematic graph;
    const int floats = graph.addNode(Float, 0, 0, -2.75F);
    const int ints = graph.addNode(Int, 0, 100, std::int32_t{-6});
    const int first = graph.addNode(Abs, 200, 0);
    const int second = graph.addNode(Abs, 400, 0);
    const int sink = graph.addNode(Float, 600, 0);
    graph.connect({floats, first, 0});
    graph.connect({first, second, 0});
    graph.connect({second, sink, 0});

    const int selection[]{first, second};
    const int wrapper = graph.makeModule(selection);
    auto& child = *graph.module(wrapper);
    const auto& moduleNode = graph.nodes().back();
    const int in = moduleNode.moduleInputs.front().endpoint;
    const int out = moduleNode.moduleOutputs.front().endpoint;
    check(child.outputType(in) == Type::Float && child.inputType(out, 0) == Type::Float &&
          graph.readValue(sink) == Value{2.75F},
          "Grouped ports should initially carry the original Float signal");

    graph.disconnectInput(wrapper, 0);
    graph.connect({ints, wrapper, 0});
    check(graph.inputType(wrapper, 0) == Type::Int && graph.outputType(wrapper) == Type::Int &&
          graph.readValue(sink) == Value{6.0F},
          "Rewiring a grouped input should retype both the input and the output that follows it");

    child.setTemplateType(out, Type::Int);
    graph.disconnectInput(wrapper, 0);
    graph.connect({floats, wrapper, 0});
    check(child.outputType(in) == Type::Float && child.inputType(out, 0) == Type::Int &&
          graph.readValue(sink) == Value{2.0F},
          "Pinning the inferred output type should keep that type across rewiring");
    child.setTemplateType(out, std::nullopt);
    check(graph.outputType(wrapper) == Type::Float && graph.readValue(sink) == Value{2.75F},
          "Restoring Template should use the current source and retain the wire");
}

void makeModuleKeepsUnresolvedPortsAsTemplate()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    Schematic graph;
    const int abs = graph.addNode(Abs, 100, 0);
    const int count = graph.addNode(TriggerCounter, 300, 0);
    graph.connect({abs, count, 0}, LinkNotification::Silent);
    const int selection[]{abs, graph.addNode(Int, 100, 100)};
    const int wrapper = graph.makeModule(selection);
    check(!graph.outputType(wrapper) && !graph.inspectLink({wrapper, count, 0}).value,
          "Grouping an unresolved source should leave the new port untyped");
    auto& child = *graph.module(wrapper);
    child.connect({child.addNode(Float, 0, 0, -1.25F), abs, 0});
    check(graph.outputType(wrapper) == Type::Float,
          "An unresolved grouped port should infer the type when its source resolves");
}

void makeModuleRoutesBusSignalsThroughNestedBoundaries()
{
    using enum PrimitiveKind;
    using openstone::green::Value;
    using openstone::green::Type;
    Schematic graph;
    const int sourceNames = graph.addNode(String, 0, 0, std::string{"data,pulse"});
    const int sinkNames = graph.addNode(String, 0, 80, std::string{"pulse,data"});
    const int data = graph.addNode(Int, 20, 200, std::int32_t{5});
    const int pulse = graph.addNode(Trigger, 20, 280);
    const int create = graph.addNode(BusCreate, 240, 100);
    const int extract = graph.addNode(BusExtract, 460, 0);
    const int display = graph.addNode(Int, 650, 0);
    const int counter = graph.addNode(TriggerCounter, 650, 95);
    graph.connect({sourceNames, create, 0});
    graph.connect({data, create, 1});
    graph.connect({pulse, create, 2});
    graph.connect({sinkNames, extract, 0});
    graph.connect({create, extract, 1});
    graph.connect({extract, display, 0, 1});
    graph.connect({extract, counter, 0}, LinkNotification::Silent);

    const int outputSide[]{create, data};
    const int sender = graph.makeModule(outputSide);
    const int inputSide[]{sinkNames, extract};
    const int receiver = graph.makeModule(inputSide);
    check(graph.outputType(sender, 0) == Type::Bus && graph.inputType(receiver, 0) == Type::Bus &&
          graph.readValue(display) == Value{std::int32_t{5}},
          "Bus values should resolve their channel types across both boundaries");
    const int combined[]{sender, receiver};
    const int nested = graph.makeModule(combined);
    check(graph.readValue(counter) == Value{std::int32_t{0}},
          "Creating nested boundaries should not manufacture a Bus channel pulse");
    graph.fireTrigger(pulse);
    graph.module(nested)->module(sender)->setValue(data, std::int32_t{19});
    graph.setValue(sourceNames, std::string{"pulse,data"});
    graph.fireTrigger(pulse);
    check(graph.readValue(display) == Value{std::int32_t{19}} &&
          graph.readValue(counter) == Value{std::int32_t{2}},
          "Nested Bus routes should preserve channel-specific pulses through name reordering");
}

void makeModuleCopiesWithItsInterface()
{
    using enum PrimitiveKind;
    using openstone::green::Value;
    openstone::Session session;
    const int original = session.addNode(Module, 0, 0);
    check(session.enterModule(original), "Enter the source module");
    const int number = session.addNode(Int, 0, 0);
    const int input = session.addNode(Int, 200, 0);
    const int output = session.addNode(Int, 400, 0);
    const int display = session.addNode(Int, 600, 0);
    session.setValue(number, std::int32_t{17});
    session.connect({number, input, 0});
    session.connect({input, output, 0});
    session.connect({output, display, 0});
    check(session.navigate(0), "Return to the main canvas");
    session.copyNode(original);
    const int peer = session.pasteNode(350, 0, true);
    check(session.enterModule(peer), "Enter the linked copy");
    const int selection[]{input, output};
    const int wrapped = session.makeModule(selection);
    check(session.graph().module(wrapped) && session.graph().readValue(display) == Value{std::int32_t{17}} &&
          session.navigate(0) && session.enterModule(original) &&
          session.graph().nodes().size() == 3 && session.graph().readValue(display) == Value{std::int32_t{17}},
          "Grouping should propagate the same working interface to synchronized copies");
    check(session.navigate(0), "Return to Main before copying the grouped schematic");
    session.copyNode(original);
    const int copy = session.pasteNode(700, 0, false);
    check(session.enterModule(copy) && session.graph().readValue(display) == Value{std::int32_t{17}},
          "Copy and paste should preserve generated module connectors and internal proxy nodes");
}

void propertyPortsReadOutputsAndSupplyUnwiredInputs()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    using openstone::green::Value;
    openstone::Session session;
    auto& root = session.graph();
    check(!root.canPlace(ModuleInputProperty) && !root.canPlace(ModuleOutputProperty),
          "Property connectors need an enclosing module");
    const int module = root.addNode(Module, 0, 0);
    check(session.enterModule(module), "Enter the module to author its property wiring");
    auto& child = session.graph();
    const int input = child.addNode(ModuleInput, 0, 0);
    const int output = child.addNode(ModuleOutput, 0, 0);
    const int read = child.addNode(ModuleInputProperty, 0, 0);
    const int write = child.addNode(ModuleOutputProperty, 0, 0);
    child.setModulePortName(input, "Set");
    child.setModulePortName(output, "Value");
    session.renameNode(read, "Value");
    session.renameNode(write, "Set");
    check(findNode(root, module).inputPorts().size() == 1 && findNode(root, module).outputPorts().size() == 1,
          "Property endpoints share the ordinary connectors without creating new exterior connectors");
    const int editor = child.addNode(Int, 0, 0, std::int32_t{17});
    const int readout = child.addNode(Int, 0, 0);
    const int count = child.addNode(TriggerCounter, 0, 0);
    child.connect({input, output, 0}, LinkNotification::Silent);
    child.connect({read, readout, 0}, LinkNotification::Silent);
    child.connect({read, count, 0}, LinkNotification::Silent);
    child.connect({editor, write, 0});
    check(root.inputType(module, 0) == Type::Int && child.outputType(read) == Type::Int &&
          root.inspectInput(module, 0).value == Value{std::int32_t{17}} && integer(child, readout) == 17 &&
          integer(child, count) == 1, "Property output supplies the input and property input reads the result");
    child.setValue(editor, std::int32_t{25});
    check(integer(root, module) == 25 && integer(child, readout) == 25 && integer(child, count) == 2,
          "After writing a property the module and its readout share the updated value and pulse");
    const int wired = root.addNode(Int, 0, 0, std::int32_t{50});
    root.connect({wired, module, 0});
    child.setValue(editor, std::int32_t{33});
    check(integer(root, module) == 50 && integer(child, readout) == 50 && integer(child, count) == 3,
          "An exterior value link controls the input and supplies updates to the property reader");
    root.disconnectInput(module, 0);
    check(integer(root, module) == 33 && integer(child, readout) == 33,
          "Disconnecting the exterior value returns control to the property output");
    session.renameNode(write, "Unbound");
    child.connect({read, write, 0}, LinkNotification::Silent);
    bool rejected = false;
    try {
        session.renameNode(write, "Set");
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected && findNode(child, write).label == "Unbound",
          "Renaming a property port rolls back a binding that would create feedback");
}

void propertyPortsCarryTriggerOnlyConnections()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    openstone::Session session;
    auto& root = session.graph();
    const int module = root.addNode(Module, 0, 0);
    check(session.enterModule(module), "Enter a module to wire its trigger properties");
    auto& child = session.graph();
    const int in = child.addNode(ModuleInput, 0, 0);
    const int out = child.addNode(ModuleOutput, 0, 0);
    const int read = child.addNode(ModuleInputProperty, 0, 0);
    const int write = child.addNode(ModuleOutputProperty, 0, 0);
    const int get = child.addNode(Trigger, 0, 0);
    const int count = child.addNode(TriggerCounter, 0, 0);
    child.setModulePortName(in, "Fire");
    child.setModulePortName(out, "Fired");
    session.renameNode(write, "Fire");
    session.renameNode(read, "Fired");
    child.connect({get, write, 0}, LinkNotification::Silent);
    child.connect({in, out, 0}, LinkNotification::Silent);
    child.connect({read, count, 0}, LinkNotification::Silent);
    check(root.inputType(module, 0) == Type::Trigger && root.outputType(module) == Type::Trigger &&
          child.outputType(read) == Type::Trigger && integer(child, count) == 0,
          "Trigger types propagate through both property variants");
    child.fireTrigger(get);
    check(integer(child, count) == 1 && root.inspectOutput(module).triggers == 1,
          "A trigger from a property control reaches the module output and property reader once");
}

void manualPortsCarryValuesAndTriggers()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    using openstone::green::Value;
    Schematic graph;
    check(!graph.canPlace(ModuleInput) && !graph.canPlace(ModuleOutput),
          "A top-level schematic has no enclosing module to expose ports on");
    const int wrapper = graph.addNode(Module, 200, 0);
    auto& child = *graph.module(wrapper);
    const int in = child.addNode(ModuleInput, 0, 0);
    const int out = child.addNode(ModuleOutput, 200, 0);
    child.setModulePortName(in, "Gain");
    child.setModulePortName(out, "Level");
    const int source = graph.addNode(Float, 0, 0, 8.75F);
    const int sink = graph.addNode(Float, 400, 0);
    const int count = graph.addNode(TriggerCounter, 400, 90);
    child.connect({in, out, 0});
    graph.connect({source, wrapper, 0});
    graph.connect({wrapper, sink, 0});
    graph.connect({wrapper, count, 0});
    check(graph.readValue(sink) == Value{8.75F}, "An outside Float source should type both ends of a passthrough");

    const auto pulses = graph.readValue(count);
    child.setModulePortType(out, Type::Int);
    check(graph.readValue(sink) == Value{8.0F} && graph.outputType(wrapper) == Type::Int,
          "A manual choice overrides the source type and uses the normal link conversion");
    child.setModulePortType(out, std::nullopt);
    const auto owner = std::ranges::find(graph.nodes(), wrapper, &openstone::SchematicNode::id);
    check(owner->inputPorts().at(0).name == "Gain" && owner->outputPorts().at(0).name == "Level" &&
          graph.inputType(wrapper, 0) == Type::Float && graph.outputType(wrapper, 0) == Type::Float &&
          child.outputType(in) == Type::Float && child.inputType(out, 0) == Type::Float,
          "The enclosing module should expose the internal names and types on the correct sides");
    check(graph.readValue(sink) == Value{8.75F} && graph.readValue(count) == pulses,
          "Retyping should retain compatible links without emitting extra triggers");
    graph.setValue(source, 3.25F);
    check(graph.readValue(sink) == Value{3.25F} &&
          graph.readValue(count) == Value{std::get<std::int32_t>(pulses) + 1},
          "Values and their triggers should flow both ways across the boundary");

    const int triggerIn = child.addNode(ModuleInput, 0, 100);
    const int triggerOut = child.addNode(ModuleOutput, 200, 100);
    child.connect({triggerIn, triggerOut, 0}, LinkNotification::Silent);
    check(!child.outputType(triggerIn) && !graph.outputType(wrapper, 1),
          "A chain with no concrete connector should remain Template");
    const int trigger = graph.addNode(Trigger, 0, 100);
    graph.connect({trigger, wrapper, 1}, LinkNotification::Silent);
    graph.connect({wrapper, count, 0, 1}, LinkNotification::Silent);
    graph.fireTrigger(trigger);
    check(graph.outputType(wrapper, 1) == Type::Trigger &&
          graph.readValue(count) == Value{std::get<std::int32_t>(pulses) + 2} &&
          !graph.inspectOutput(wrapper, 1).value,
          "Manually placed Trigger endpoints carry pulses with no value");
}

void templateModulePortsInferFromEitherSide()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    using openstone::green::Value;
    Schematic graph;
    const int wrapper = graph.addNode(Module, 200, 0);
    auto& child = *graph.module(wrapper);
    const int nested = child.addNode(Module, 200, 0);
    auto& inner = *child.module(nested);
    const int input = child.addNode(ModuleInput, 0, 0);
    const int output = child.addNode(ModuleOutput, 400, 0);
    const int innerInput = inner.addNode(ModuleInput, 0, 0);
    const int innerOutput = inner.addNode(ModuleOutput, 200, 0);
    child.connect({input, nested, 0});
    inner.connect({innerInput, innerOutput, 0});
    child.connect({nested, output, 0});
    const int color = graph.addNode(Color, 500, 0);
    graph.connect({wrapper, color, 0});
    check(graph.inputType(wrapper, 0) == Type::Color && child.outputType(nested) == Type::Color &&
          inner.outputType(innerInput) == Type::Color && inner.inputType(innerOutput, 0) == Type::Color,
          "A downstream connector should type an otherwise untyped chain through nested modules");

    const int incompatible = graph.addNode(Int, 0, 0);
    bool rejected = false;
    try {
        graph.connect({incompatible, wrapper, 0});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected && graph.links().size() == 1 && graph.outputType(wrapper) == Type::Color,
          "An incompatible link should leave the wire and inferred types that were already present");
    graph.disconnectInput(color, 0);
    check(!graph.inputType(wrapper, 0) && !child.outputType(nested) && !inner.outputType(innerInput),
          "Disconnecting the last typed neighbor should restore Template across the chain");
    const int source = graph.addNode(Color, 0, 120, openstone::yellow::Color{0xFF19EEAA});
    graph.connect({source, wrapper, 0});
    graph.connect({wrapper, color, 0});
    check(graph.readValue(color) == Value{openstone::yellow::Color{0xFF19EEAA}},
          "The same chain should pick up a Yellow value type and carry its value");
}

void templateModulePortsCarryBusChannels()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    using openstone::green::Value;
    Schematic graph;
    const int wrapper = graph.addNode(Module, 200, 0);
    auto& child = *graph.module(wrapper);
    const int input = child.addNode(ModuleInput, 0, 0);
    const int output = child.addNode(ModuleOutput, 200, 0);
    child.connect({input, output, 0});
    const int bus = graph.addNode(BusCreate, 0, 0);
    const int extract = graph.addNode(BusExtract, 400, 0);
    const int names = graph.addNode(String, 0, 100, std::string{"data"});
    const int data = graph.addNode(Float, 0, 200, 2.75F);
    graph.connect({names, bus, 0});
    graph.connect({names, extract, 0});
    graph.connect({wrapper, extract, 1});
    check(graph.inputType(wrapper, 0) == Type::Bus && graph.outputType(wrapper) == Type::Bus,
          "The Bus on the outside should type the entire module route");
    graph.connect({data, bus, 1});
    graph.connect({bus, wrapper, 0});
    check(graph.outputType(extract) == Type::Float && graph.readValue(extract) == Value{2.75F},
          "Bus extraction should resolve the original channel type and value through automatic ports");
}

struct MidiSource {
    explicit MidiSource(Schematic& graph)
        : graph(graph), event(graph.addNode(PrimitiveKind::MidiEvent, 0, 0)),
          send(graph.addNode(PrimitiveKind::Trigger, 0, 0))
    {
        for (int port = 0; port < 4; ++port) {
            fields[port] = graph.addNode(PrimitiveKind::Int, 0, 0);
            graph.connect({fields[port], event, port});
        }
        graph.connect({send, event, 4}, LinkNotification::Silent);
    }

    void emit(int status, int channel, int data1 = 0, int data2 = 0) const
    {
        const std::array<std::int32_t, 4> bytes{status, channel, data1, data2};
        for (int port = 0; port < 4; ++port) graph.setValue(fields[port], bytes[port]);
        graph.fireTrigger(send);
    }

    Schematic& graph;
    int event;
    int send;
    std::array<int, 4> fields;
};

void assemblerPackedPolyMatchesFlowStone()
{
    using namespace openstone::stream;
    using enum PrimitiveKind;
    constexpr auto frames = flowStoneTraceFrames;
    const auto source = readFlowStoneFixture("poly-voice-reverse.asm");
    const auto program = compileDspCode(source, DspLanguage::Assembler);
    for (const std::size_t voiceCount : {4, 8}) {
        const auto expected = flowStoneReference(voiceCount == 4 ? "poly-voice-four.f32" : "poly-voice-eight.f32");
        for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);
            const auto pitch = graph.addInput();
            const auto block = graph.addDspCode(program);
            graph.connectDspInput(pitch, block.id, 0);
            const std::array bindings{PolyInput{PolyInputKind::Pitch}};
            std::vector<openstone::stream::MidiNoteEvent> notes;
            for (std::size_t voice = 0; voice < voiceCount; ++voice) notes.push_back({0, 0x90, static_cast<std::uint8_t>(60 + voice), 127});
            for (const std::size_t size : {1, 7, 64}) {
                PolyRuntime runtime(graph, bindings, block.outputs, {.maxVoices = voiceCount});
                // Run three periods, including wraparound across buffer boundaries.
                std::array<float, frames * 3> output{};
                for (std::size_t frame = 0; frame < output.size(); frame += size) {
                    const auto count = std::min(size, output.size() - frame);
                    check(runtime.processMidi({}, std::array{OutputBuffer{std::span{output}.subspan(frame, count)}},
                        frame == 0 ? std::span<const openstone::stream::MidiNoteEvent>{notes} : std::span<const openstone::stream::MidiNoteEvent>{}),
                        "Render the packed Poly Assembler on either backend");
                }
                for (std::size_t frame = 0; frame < output.size(); ++frame) {
                    check(std::bit_cast<std::uint32_t>(output[frame]) == expected[frame % frames],
                        "Packed Poly matches every native FlowStone sample word across buffers and packet boundaries");
                }
            }
        }
        // Also exercise the editor's actual MIDI primitives, generic Stream specialization,
        // voice bank, Poly-to-Mono bridge and Audio Out render path.
        for (const std::size_t size : {1, 7, 64}) {
            openstone::Session session;
            auto& graph = session.graph();
            const auto add = [&](PrimitiveKind kind) { return session.addNode(kind, 0, 0); };
            const MidiSource sender(graph);
            const int notes = add(MidiToNotes), voices = add(Voices), assign = add(NotesToVoices);
            const int signals = add(VoicesToPoly), assembler = add(Assembler), mix = add(PolyToMono), output = add(AudioOut);
            graph.setDspDraft(assembler, source);
            session.compileDsp(assembler);
            graph.connect({sender.event, notes, 0});
            graph.connect({voices, assign, 0});
            graph.connect({notes, assign, 1});
            graph.connect({assign, signals, 0});
            graph.connect({signals, assembler, 0, 1});
            graph.connect({assembler, mix, 0});
            graph.connect({mix, output, 0});
            check(graph.outputType(assembler) == openstone::green::Type::Poly, "Generic Assembler output specializes to Poly");
            auto patch = graph.prepareAudio(size);
            for (std::size_t voice = 0; voice < voiceCount; ++voice) sender.emit(0x90, 1, 60 + voice, 127);
            std::array<float, frames> left{}, right{};
            for (std::size_t frame = 0; frame < frames; frame += size) {
                const auto count = std::min(size, frames - frame);
                check(patch.processor().process({}, {std::span{left}.subspan(frame, count), std::span{right}.subspan(frame, count)}),
                    "Render packed Poly through the editor audio path");
            }
            for (std::size_t frame = 0; frame < frames; ++frame) check(std::bit_cast<std::uint32_t>(left[frame]) == expected[frame],
                "The editor audio path matches the FlowStone packed Poly capture");
        }
    }
}

void midiSequencerReplaysRecordedEventsInTimeOrder()
{
    using enum PrimitiveKind;
    using enum openstone::stream::SequenceMode;
    using namespace std::chrono_literals;
    auto time = openstone::TickTime{};
    Schematic graph({}, {}, {}, {}, {.monotonicClock = [&] { return time; }});
    const MidiSource input(graph);
    const int track = graph.addNode(MidiSequencerTrack, 0, 0);
    graph.connect({input.event, track, 0});
    graph.setSequencerMode(Recording, time);
    time += 20ms;
    input.emit(0x90, 2, 65, 127);
    time += 30ms;
    input.emit(0x80, 2, 65);
    time += 10ms;
    graph.setSequencerMode(Stopped, time);
    const auto count = graph.inspectOutput(track).triggers;
    check(count == 2, "The MIDI track monitors incoming events as it records them");
    graph.setSequencerMode(Playing, time);
    check(!graph.advanceSequencer(time + 19ms) && graph.inspectOutput(track).triggers == count,
          "A replay waits until the recorded time");
    check(graph.advanceSequencer(time + 21ms) &&
          std::get<openstone::green::MidiMessage>(graph.readValue(track)).bytes == std::vector<std::uint8_t>{0x91,65,127},
          "The next replay tick emits the first event with the original channel and pitch");
    check(graph.advanceSequencer(time + 51ms) &&
          std::get<openstone::green::MidiMessage>(graph.readValue(track)).bytes == std::vector<std::uint8_t>{0x81,65,0},
          "The replay emits note-off at its recorded time");
    check(graph.advanceSequencer(time + 61ms) && graph.sequencerMode() == Stopped &&
          graph.inspectOutput(track).triggers == count + 2,
          "Reaching the end stops the shared transport");
}

void matrixSwitchesPolyRoutesWithoutResettingVoices()
{
    using enum PrimitiveKind;
    openstone::Session session;
    auto& graph = session.graph();
    const auto add = [&](PrimitiveKind kind) { return session.addNode(kind, 0, 0); };
    const MidiSource sender(graph);
    const int notes = add(MidiToNotes);
    const int voices = add(Voices);
    const int assign = add(NotesToVoices);
    const int signals = add(VoicesToPoly);
    const int ramp = add(DspCode);
    const int matrix = add(Matrix);
    const int mix = add(PolyToMono);
    const int output = add(AudioOut);
    const int map = graph.addNode(String, 0, 0, std::string{"0,0"});
    graph.setDspDraft(ramp, "polyin v; polyout y; float phase; y = v * phase; phase += 1;");
    session.compileDsp(ramp);

    graph.connect({sender.event, notes, 0});
    graph.connect({voices, assign, 0});
    graph.connect({notes, assign, 1});
    graph.connect({assign, signals, 0});
    graph.connect({signals, ramp, 0, 2});
    graph.connect({signals, matrix, 1, 2});
    graph.connect({ramp, matrix, 2});
    graph.connect({matrix, mix, 0});
    graph.connect({mix, output, 0});
    graph.connect({map, matrix, 0});

    auto patch = graph.prepareAudio(2);
    sender.emit(0x90, 1, 60, 127);
    sender.emit(0x90, 1, 64, 127);
    std::array<float, 3> left{}, right{};
    check(patch.processor().process({}, {left, right}) && left == std::array{2.F, 2.F, 2.F},
          "The initial Matrix map routes the velocity of both held voices");
    graph.setValue(map, std::string{"1,0"});
    check(graph.refreshAudioControls(patch) && patch.processor().process({}, {left, right}) &&
          left == std::array{6.F, 8.F, 10.F},
          "The next buffer routes the new signal with both voices' DSP state intact");
}

void midiMessagesMergeAcrossModulesAndSplitAtomically()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    Schematic graph;
    const MidiSource a(graph);
    const MidiSource b(graph);
    const int wrapper = graph.addNode(Module, 0, 0);
    auto& child = *graph.module(wrapper);
    const int input = child.addNode(ModuleInput, 0, 0);
    child.setModulePortType(input, Type::MIDI);
    const int split = child.addNode(MidiSplit, 0, 0);
    child.connect({input, split, 0});
    graph.connect({a.event, wrapper, 0});
    graph.connect({b.event, wrapper, 0});

    const int pulse = graph.addNode(Trigger, 0, 0);
    graph.connect({pulse, wrapper, 0}, LinkNotification::Silent);
    const int hold = child.addNode(SampleAndHold, 0, 0);
    child.connect({split, hold, 0, 3});
    child.connect({split, hold, 1, 4}, LinkNotification::Silent);
    const auto checkSplit = [&](std::array<int, 4> fields, int deliveries) {
        for (int out = 0; out < 4; ++out) {
            check(integer(child, split, out) == fields[out] && child.inspectOutput(split, out).triggers == 0,
                  "Splitter updates all Ints without triggering the individual value outputs");
        }
        check(integer(child, hold) == fields.back() && child.inspectOutput(split, 4).triggers == deliveries,
              "A single Changed trigger exposes the complete split message");
    };

    a.emit(0x90, 16, 60, 93);
    check(std::get<MidiMessage>(graph.readValue(a.event)).bytes == std::vector<std::uint8_t>{0x9F, 60, 93},
          "Event combines the status high nibble with a one-based channel");
    checkSplit({0x90, 16, 60, 93}, 1);
    b.emit(0xC0, 2, 14);
    checkSplit({0xC0, 2, 14, 0}, 2);
    graph.fireTrigger(pulse);
    b.emit(0x90, 0, 10, 1);
    b.emit(0x90, 2, 128, 1);
    checkSplit({0xC0, 2, 14, 0}, 2);
    b.emit(0xF8, 1);
    checkSplit({0xF8, 0, 0, 0}, 3);

    const int addedLater = child.addNode(MidiSplit, 0, 0);
    child.connect({input, addedLater, 0});
    check(child.inspectOutput(addedLater, 4).triggers == 0, "Connecting a new MIDI input does not replay an old message");
    a.emit(0x80, 16, 60, 0);
    checkSplit({0x80, 16, 60, 0}, 4);
    check(child.inspectOutput(addedLater, 4).triggers == 1, "The new input sees the next event");
}

void midiFiltersExtractAndBufferMessagesInOrder()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    Schematic graph;
    const MidiSource source(graph);
    const int channel = graph.addNode(MidiChannelFilter, 0, 0);
    const int status = graph.addNode(MidiStatusFilter, 0, 0);
    const int note = graph.addNode(MidiNoteFilter, 0, 0);
    const int cc = graph.addNode(MidiCC, 0, 0);
    const int aftertouch = graph.addNode(MidiAftertouch, 0, 0);
    const int bend = graph.addNode(MidiPitchBend, 0, 0);
    const int buffer = graph.addNode(MidiBuffer, 0, 0);
    const int split = graph.addNode(MidiSplit, 0, 0);
    const int flush = graph.addNode(Trigger, 0, 0);
    const int clear = graph.addNode(Trigger, 0, 0);
    const int ccNumber = graph.addNode(Int, 0, 0, std::int32_t{64});
    const int history = graph.addNode(ArrayHistory, 0, 0);
    const int size = graph.addNode(Int, 0, 0, std::int32_t{8});
    const int flushed = graph.addNode(TriggerCounter, 0, 0);
    for (int node : {channel, status, note, cc, aftertouch, bend, buffer}) graph.connect({source.event, node, 0});
    graph.connect({ccNumber, cc, 1});
    graph.connect({buffer, split, 0});
    graph.connect({buffer, flushed, 0, 1}, LinkNotification::Silent);
    const int oldestFirst = graph.addNode(Boolean, 0, 0, true);
    graph.connect({oldestFirst, history, 3});
    graph.connect({size, history, 0});
    graph.connect({split, history, 1, 2});
    graph.connect({split, history, 2, 4}, LinkNotification::Silent);
    graph.connect({flush, buffer, 1}, LinkNotification::Silent);
    graph.connect({clear, buffer, 2}, LinkNotification::Silent);

    check(integer(graph, bend) == 8192, "Pitch bend defaults to center");
    source.emit(0x90, 1, 60, 100);
    source.emit(0x90, 2, 61, 100);
    source.emit(0x80, 1, 60);
    source.emit(0xB0, 1, 64, 127);
    source.emit(0xA0, 1, 66, 77);
    source.emit(0xD0, 1, 82);
    source.emit(0xE0, 2, 127, 127);
    check(graph.inspectOutput(channel).triggers == 5 && graph.inspectOutput(channel, 1).triggers == 2 &&
          graph.inspectOutput(status).triggers == 2 && graph.inspectOutput(status, 1).triggers == 5 &&
          graph.inspectOutput(note).triggers == 2 && graph.inspectOutput(note, 1).triggers == 5,
          "Filters route each message once to either Match or Remaining");
    check(integer(graph, cc) == 127 && graph.inspectOutput(cc, 1).triggers == 1 &&
          integer(graph, aftertouch) == 82 && graph.inspectOutput(aftertouch).triggers == 1 &&
          integer(graph, bend) == 16383 && graph.inspectOutput(bend).triggers == 1,
          "Decoders distinguish CC, channel pressure, and 14-bit pitch bend");
    check(graph.inspectOutput(split, 4).triggers == 0, "Pending MIDI messages are silent before Flush");
    graph.fireTrigger(flush);
    check(std::get<IntArray>(graph.readValue(history)) == IntArray{60, 61, 60, 64, 66, 82, 127} &&
          integer(graph, flushed) == 1, "Flush sends messages in arrival order then sends Flushed");
    source.emit(0x90, 2, 99, 100);
    graph.fireTrigger(clear);
    graph.fireTrigger(flush);
    check(graph.inspectOutput(split, 4).triggers == 7 && integer(graph, flushed) == 2,
          "Clear discards queued messages and an empty Flush still completes");
}

void midiStreamControlsHoldAndUpdateAudioParameters()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    Schematic graph;
    const MidiSource source(graph);
    const int cc = graph.addNode(MidiCCStream, 0, 0);
    const int pressure = graph.addNode(MidiAftertouchStream, 0, 0);
    const int bend = graph.addNode(MidiPitchBendStream, 0, 0);
    const int number = graph.addNode(Int, 0, 0, std::int32_t{74});
    const int minimum = graph.addNode(Float, 0, 0, -0.5F);
    const int fallback = graph.addNode(Float, 0, 0, 0.25F);
    const int capture = graph.addNode(MonoToGraph, 0, 0);
    const int output = graph.addNode(AudioOut, 0, 0);
    check(graph.outputType(cc) == Type::Poly, "MIDI CC Stream has a Poly output");
    for (const auto node : {cc, pressure, bend}) {
        check(graph.inputType(node, 0) == Type::MIDI, "MIDI stream controls consume triggered MIDI");
        graph.connect({source.event, node, 0});
    }
    graph.connect({number, cc, 1});
    graph.connect({minimum, cc, 2});
    graph.connect({fallback, cc, 4});
    const auto notes = graph.addNode(MidiToNotes, 0, 0);
    const auto bank = graph.addNode(Voices, 0, 0);
    const auto allocator = graph.addNode(NotesToVoices, 0, 0);
    const auto mono = graph.addNode(PolyToMono, 0, 0);
    graph.connect({source.event, notes, 0});
    graph.connect({bank, allocator, 0});
    graph.connect({notes, allocator, 1});
    graph.connect({cc, mono, 0});
    graph.connect({mono, output, 0});
    graph.connect({bend, output, 1});
    graph.connect({pressure, capture, 0});

    auto patch = graph.prepareAudio(2);
    std::array<float, 5> left{}, right{};
    const auto checkLive = [&](float expectedCc, float expectedBend, const char* message) {
        check(patch.processor().process({}, {left, right}) &&
              std::ranges::all_of(left, [=](float sample) { return std::abs(sample - expectedCc) < 1e-6F; }) &&
              std::ranges::all_of(right, [=](float sample) { return sample == expectedBend; }), message);
    };
    const auto refresh = [&] {
        check(graph.refreshAudioControls(patch), "MIDI and control changes should reuse the prepared audio graph");
    };
    const auto checkPressure = [&](float expected, const char* message) {
        graph.fireTrigger(capture);
        const auto samples = std::get<FloatArray>(graph.readValue(capture));
        check(samples == FloatArray(512, expected), message);
    };

    checkLive(0.0F, 0.0F, "With no voice playing, Poly CC mixes down to silence");
    source.emit(0x90, 3, 60, 127);
    refresh();
    checkLive(0.25F, 0.0F, "A new voice uses the configured CC default and bend starts centered");
    checkPressure(0.0F, "Channel pressure should start at zero");
    source.emit(0xB0, 1, 7, 127);
    source.emit(0xA0, 1, 60, 127);
    refresh();
    checkLive(0.25F, 0.0F, "An unrelated controller should leave the default and bend alone");
    checkPressure(0.0F, "Polyphonic key pressure should leave channel pressure alone");

    source.emit(0xB0, 3, 74, 0);
    source.emit(0xD0, 4, 95);
    source.emit(0xE0, 5, 0, 0);
    refresh();
    checkLive(-0.5F, -1.0F, "The minimum CC and 14-bit bend should map independently");
    checkPressure(95.0F / 127.0F, "Channel pressure should normalize the last valid message");

    source.emit(0xB0, 3, 74, 127);
    source.emit(0xE0, 5, 127, 127);
    refresh();
    checkLive(1.0F, 1.0F, "The maximum controller and bend should reach their upper endpoints");
    source.emit(0xE0, 5, 0, 64);
    source.emit(0xB0, 3, 74, 64);
    source.emit(0xB0, 3, 7, 127);
    refresh();
    checkLive(0.2559055F, 0.0F, "CC should interpolate and hold its value when unrelated controllers arrive");
    graph.setValue(minimum, 1.0F);
    graph.setValue(fallback, 0.125F);
    refresh();
    checkLive(1.0F, 0.0F, "Range edits should remap the held CC and center should decode from 0, 64");

    graph.setValue(number, std::int32_t{1});
    source.emit(0xB0, 3, 74, 0);
    refresh();
    checkLive(0.125F, 0.0F, "Selecting another CC should restore the default until it receives a value");
    graph.setValue(number, std::int32_t{74});
    refresh();
    checkLive(0.125F, 0.0F, "Reselecting the previous CC should still wait for a fresh message");
}

void midiCcStreamTracksEachVoiceChannel()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    Schematic graph;
    const MidiSource source(graph);
    const auto add = [&](PrimitiveKind kind) { return graph.addNode(kind, 0, 0); };
    const auto cc = add(MidiCCStream);
    const auto notes = add(MidiToNotes);
    const auto bank = add(Voices);
    const auto allocate = add(NotesToVoices);
    const auto selector = add(Selector);
    const auto first = add(PolyToMono);
    const auto second = add(PolyToMono);
    const auto output = add(AudioOut);
    const auto number = graph.addNode(Int, 0, 0, std::int32_t{74});
    const auto fallback = graph.addNode(Float, 0, 0, 0.25F);
    const auto slot0 = graph.addNode(Int, 0, 0, std::int32_t{0});
    const auto slot1 = graph.addNode(Int, 0, 0, std::int32_t{1});
    graph.connect({source.event, cc, 0});
    graph.connect({number, cc, 1});
    graph.connect({fallback, cc, 4});
    graph.connect({source.event, notes, 0});
    graph.connect({bank, allocate, 0});
    graph.connect({notes, allocate, 1});
    graph.connect({cc, selector, 1});
    graph.connect({selector, first, 0});
    graph.connect({selector, second, 0});
    graph.connect({slot0, first, 1});
    graph.connect({slot1, second, 1});
    graph.connect({first, output, 0});
    graph.connect({second, output, 1});
    check(graph.outputType(cc) == Type::Poly && graph.outputType(selector) == Type::Poly,
          "MIDI CC Stream routes through a Poly selector without a conversion");

    source.emit(0xB0, 1, 74, 127);
    source.emit(0x90, 1, 60, 127);
    source.emit(0x90, 16, 60, 127);
    auto patch = graph.prepareAudio(3);
    const auto processor = patch.processorHandle();
    const auto render = [&](float expectedFirst, float expectedSecond) {
        std::array<float, 11> left{}, right{};
        check(graph.refreshAudioControls(patch) && patch.processorHandle() == processor &&
              patch.processor().process({}, {left, right}) &&
              std::ranges::all_of(left, [=](float value) { return value == expectedFirst; }) &&
              std::ranges::all_of(right, [=](float value) { return value == expectedSecond; }),
              "A Poly voice reads and holds the CC value of its own MIDI channel");
    };
    render(1.0F, 0.25F);
    source.emit(0xB0, 16, 74, 0);
    source.emit(0xB0, 1, 7, 0);
    render(1.0F, 0.0F);
    source.emit(0xB0, 1, 74, 64);
    render(64.0F / 127.0F, 0.0F);
    graph.setValue(number, std::int32_t{10});
    render(0.25F, 0.25F);
}

void notesFiltersRememberRoutingAndTagLayers()
{
    using enum PrimitiveKind;
    using openstone::green::Notes;
    Schematic graph;
    const auto add = [&](PrimitiveKind kind) { return graph.addNode(kind, 0, 0); };
    const auto text = [&](const char* string) { return graph.addNode(String, 0, 0, std::string{string}); };
    const auto number = [&](int value) { return graph.addNode(Int, 0, 0, std::int32_t{value}); };
    const auto flag = graph.addNode(Boolean, 0, 0, true);
    const MidiSource sender(graph);
    const int notes = add(MidiToNotes);
    const int pitch = add(NotesPitchFilter);
    const int velocity = add(NotesVelocityFilter);
    const int channel = add(NotesChannelFilter);
    const int low = text("60\n55");
    graph.connect({sender.event, notes, 0});
    graph.connect({notes, pitch, 0});
    graph.connect({pitch, velocity, 0});
    graph.connect({velocity, channel, 0});
    graph.connect({low, pitch, 1});
    graph.connect({text("70\n72"), pitch, 2});
    graph.connect({number(0), pitch, 3});
    graph.connect({flag, pitch, 6});
    graph.connect({text("0\n0.75"), velocity, 1});
    graph.connect({text("1\n1"), velocity, 2});
    graph.connect({number(0), velocity, 5});
    graph.connect({text("1"), channel, 1});
    graph.connect({text("1"), channel, 2});

    const auto events = [&](int node, int output = 0) { return std::get<Notes>(graph.readValue(node, output)).events; };
    sender.emit(0x90, 2, 60, 64);
    check(events(pitch).size() == 2 && events(pitch)[0].tags[0] == 0 && events(pitch)[1].tags[0] == 1,
          "Overlapping pitch layers should duplicate and tag matching attacks");
    check(events(velocity).size() == 1 && events(velocity, 1).size() == 1 &&
          events(channel).size() == 1 && events(channel).front().channel == 1,
          "Velocity should use the tagged layer and channel matching should use the wire channel");
    graph.setValue(low, std::string{"61\n61"});
    sender.emit(0x80, 2, 60, 0);
    check(events(pitch).size() == 2 && events(velocity).size() == 1 &&
          events(channel).size() == 1 && !events(channel).front().noteOn,
          "Note Off should reuse the original attack's layer and tags despite range or velocity changes");
    sender.emit(0x90, 1, 60, 127);
    check(events(pitch).empty() && events(pitch, 1).size() == 1,
          "Nonmatching notes should go to Remaining");
}

void polyImpulseStartsWithEachNewVoice()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    Schematic graph;
    const auto add = [&](PrimitiveKind kind) { return graph.addNode(kind, 0, 0); };
    const MidiSource sender(graph);
    const int notes = add(MidiToNotes);
    const int voices = add(Voices);
    const int assign = add(NotesToVoices);
    const int signals = add(VoicesToPoly);
    const int impulse = add(Impulse);
    const int multiply = add(Multiply);
    const int mix = add(PolyToMono);
    const int audio = add(AudioOut);
    graph.connect({sender.event, notes, 0});
    graph.connect({voices, assign, 0});
    graph.connect({notes, assign, 1});
    graph.connect({assign, signals, 0});
    graph.connect({signals, multiply, 0, 2}); // Velocity anchors the patch to the voice bank.
    graph.connect({impulse, multiply, 1});
    graph.connect({multiply, mix, 0});
    graph.connect({mix, audio, 0});
    check(graph.outputType(impulse) == Type::Poly, "Impulse specializes to Poly in the voice section");

    auto patch = graph.prepareAudio(7);
    const auto render = [&](float firstSample) {
        check(graph.refreshAudioControls(patch), "Changing notes keeps the active patch");
        std::array<float, 15> left{}, right{}, expected{};
        expected.front() = firstSample;
        check(patch.processor().process({}, {left, right}) && left == expected,
              "Only the first sample of a newly started voice carries the impulse");
    };
    render(0);
    sender.emit(0x90, 1, 60, 127);
    render(1);
    render(0);
    sender.emit(0x90, 1, 64, 64);
    render(64.0f / 127);
    render(0);
}

void floatArrayReadSharesLiveEditsAcrossPolyVoices()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    Schematic graph;
    const auto add = [&](PrimitiveKind kind) { return graph.addNode(kind, 0, 0); };
    const MidiSource sender(graph);
    const int notes = add(MidiToNotes);
    const int voices = add(Voices);
    const int assign = add(NotesToVoices);
    const int signals = add(VoicesToPoly);
    const int reader = add(FloatArrayRead);
    const int mix = add(PolyToMono);
    const int output = add(AudioOut);
    std::string array;
    for (int i = 0; i < 60; ++i) array += "0\n";
    const int text = graph.addNode(MultilineString, 0, 0, array + "0.25\n0\n0\n0\n0.75");
    graph.connect({sender.event, notes, 0});
    graph.connect({voices, assign, 0});
    graph.connect({notes, assign, 1});
    graph.connect({assign, signals, 0});
    graph.connect({text, reader, 0});
    graph.connect({signals, reader, 1, 1});
    graph.connect({reader, mix, 0});
    graph.connect({mix, output, 0});
    check(graph.outputType(reader) == Type::Poly, "Float Array Read specializes to Poly for a voice index");
    auto patch = graph.prepareAudio(3);
    const auto verify = [&](float expected) {
        std::array<float, 11> left{}, right{};
        check(graph.refreshAudioControls(patch) && patch.processor().process({}, {left, right}) &&
              std::ranges::all_of(left, [expected](float sample) { return sample == expected; }),
              "Poly voices read independent indices and share the current array");
    };
    sender.emit(0x90, 1, 60, 127);
    verify(0.25F);
    sender.emit(0x90, 1, 64, 127);
    verify(1.0F);
    graph.setValue(text, array + "0.5\n0\n0\n0\n0.125");
    verify(0.625F);
}

void streamSelectRoutesPolyVoicesIndependently()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    for (const bool wide : {false, true}) {
        Schematic graph;
        const auto add = [&](PrimitiveKind kind) { return graph.addNode(kind, 0, 0); };
        const MidiSource sender(graph);
        const int notes = add(MidiToNotes);
        const int voiceBank = add(Voices);
        const int assign = add(NotesToVoices);
        const int signals = add(VoicesToPoly);
        const int threshold = graph.addNode(Float, 0, 0, 62.0F);
        const int low = graph.addNode(Float, 0, 0, 1.0F);
        const int high = graph.addNode(Float, 0, 0, 4.0F);
        const int mask = add(StreamGreater);
        const int select = add(wide ? DoubleSelect : StreamSelect);
        const int mix = add(PolyToMono);
        const int audio = add(AudioOut);
        graph.connect({sender.event, notes, 0});
        graph.connect({voiceBank, assign, 0});
        graph.connect({notes, assign, 1});
        graph.connect({assign, signals, 0});
        graph.connect({signals, mask, 0, 1});
        graph.connect({threshold, mask, 1});
        graph.connect({low, select, 0});
        graph.connect({high, select, 1});
        graph.connect({mask, select, 2});
        check(graph.inputType(select, 2) == Type::PolyBoolean &&
              graph.outputType(select) == (wide ? Type::PolyDouble : Type::Poly),
              "Select adopts the Poly mode of its Boolean input");
        if (wide) {
            const int narrow = add(DoubleToStream);
            graph.connect({select, narrow, 0});
            graph.connect({narrow, mix, 0});
        } else {
            graph.connect({select, mix, 0});
        }
        graph.connect({mix, audio, 0});
        auto patch = graph.prepareAudio(3);
        const auto verify = [&](float expected) {
            std::array<float, 11> left{}, right{};
            check(graph.refreshAudioControls(patch) && patch.processor().process({}, {left, right}) &&
                  std::ranges::all_of(left, [expected](float sample) { return sample == expected; }),
                  "Each held voice chooses its own branch before the Poly signals are summed");
        };
        verify(0);
        sender.emit(0x90, 1, 60, 127);
        verify(1);
        sender.emit(0x90, 1, 64, 127);
        verify(5);
    }
}

void envelopeControlTracksPolyphonicStages()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    openstone::Session session;
    auto& graph = session.graph();
    const auto add = [&](PrimitiveKind kind) { return graph.addNode(kind, 0, 0); };
    const MidiSource sender(graph);
    const int notes = add(MidiToNotes), voices = add(Voices), assign = add(NotesToVoices);
    const int envelope = add(EnvelopeControl), mix = add(PolyToMono), output = add(AudioOut);
    const int hold = graph.addNode(Boolean, 0, 0, true);
    const int end = add(DspCode);
    const int finish = graph.addNode(Float, 0, 0, 0.F);
    graph.setDspDraft(end, "polyin finish; polyboolout done; done = finish > 0;");
    session.compileDsp(end);
    graph.connect({sender.event, notes, 0});
    graph.connect({voices, assign, 0});
    graph.connect({notes, assign, 1});
    graph.connect({hold, envelope, 1});
    graph.connect({finish, end, 0});
    graph.connect({end, envelope, 0});
    graph.connect({envelope, mix, 0});
    graph.connect({mix, output, 0});
    auto patch = graph.prepareAudio(2);
    const auto render = [&](std::initializer_list<float> expected) {
        check(graph.refreshAudioControls(patch), "Envelope controls update without recompiling");
        std::vector<float> l(expected.size()), r(expected.size());
        check(patch.processor().process({}, {l, r}) && std::ranges::equal(l, expected),
              "Envelope Control emits the voice's expected stage");
    };
    render({0});
    sender.emit(0x90, 1, 60, 127);
    render({1, 3, 3});
    sender.emit(0x80, 1, 60);
    render({4, 4, 4});
    graph.setValue(finish, 1.F);
    render({4, 0, 0});
    graph.setValue(finish, 0.F);
    graph.setValue(hold, false);
    sender.emit(0x90, 1, 62, 127);
    render({1, 3});
    sender.emit(0x80, 1, 62);
    render({0, 0});
}

void midiVoicesRenderIndependentReleaseTails()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    openstone::Session session;
    auto& graph = session.graph();
    const auto add = [&](PrimitiveKind kind) { return session.addNode(kind, 0, 0); };
    const MidiSource sender(graph);
    const int notes = add(MidiToNotes);
    const int voices = add(Voices);
    const int assign = add(NotesToVoices);
    const int signals = add(VoicesToPoly);
    const int envelope = add(DspCode);
    const int mix = add(PolyToMono);
    const int output = add(AudioOut);
    const auto limit = graph.addNode(Int, 0, 0, std::int32_t{2});
    graph.setDspDraft(envelope, R"(
        polyboolin gate;
        polyin velocity;
        polyout signal;
        polyboolout finished;
        float level = 0;
        level = velocity & gate + (level * 0.5) & (gate ~ (1 == 1));
        signal = level;
        finished = level <= 0.25;
    )");
    session.compileDsp(envelope);
    graph.connect({limit, voices, 0});
    graph.connect({sender.event, notes, 0});
    graph.connect({voices, assign, 0});
    graph.connect({notes, assign, 1});
    graph.connect({assign, signals, 0});
    graph.connect({signals, envelope, 0, 3});
    graph.connect({signals, envelope, 1, 2});
    graph.connect({envelope, signals, 1, 1});
    graph.connect({envelope, mix, 0});
    graph.connect({mix, output, 0});

    auto patch = graph.prepareAudio(2);
    const auto render = [&](std::initializer_list<float> expected) {
        check(graph.refreshAudioControls(patch), "Changing note state should keep the live audio patch");
        std::vector<float> left(expected.size()), right(expected.size());
        check(patch.processor().process({}, {left, right}) && std::ranges::equal(left, expected),
              "Audio should mix independent voice states and release tails");
    };

    render({0, 0, 0});
    sender.emit(0x90, 1, 60, 127);
    render({1, 1, 1});
    sender.emit(0x90, 2, 60, 127);
    render({2});
    sender.emit(0x80, 1, 60);
    render({1.5F, 1.25F, 1, 1});
    sender.emit(0x80, 2, 60);
    render({0.5F, 0.25F, 0});
    graph.setValue(limit, std::int32_t{3});
    check(!graph.refreshAudioControls(patch), "Changing the voice count should rebuild the prepared layout");
}

void polyMetadataAndMixersFollowVoiceGroups()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    openstone::Session session;
    auto& graph = session.graph();
    const auto add = [&](PrimitiveKind kind) { return session.addNode(kind, 0, 0); };
    const MidiSource sender(graph);
    const int notes = add(MidiToNotes);
    const int filter = add(NotesChannelFilter);
    const int voices = add(Voices);
    const int assign = add(NotesToVoices);
    const int signals = add(VoicesToPoly);
    const int channel = add(PolyChannelId);
    const int slot = add(PolyVoiceId);
    const int note = add(PolyNoteId);
    const int tag = add(PolyTag);
    const int data = add(DspCode);
    const int capture = add(MonoToGraph4);
    const int ns = graph.addNode(Int, 0, 0, std::int32_t{5});
    const int hop = graph.addNode(Int, 0, 0, std::int32_t{2});
    const int group = graph.addNode(Int, 0, 0, std::int32_t{1});
    const int tagIndex = graph.addNode(Int, 0, 0, std::int32_t{0});
    const int tagValue = graph.addNode(String, 0, 0, std::string{"5"});
    graph.setDspDraft(data, R"(
        polyin pitch;
        polyin channel;
        polyin slot;
        polyin note;
        polyin tag;
        polyout signal;
        float phase;
        signal = pitch + channel + slot + note + tag + phase;
        phase += 1;
    )");
    session.compileDsp(data);
    graph.connect({sender.event, notes, 0});
    graph.connect({notes, filter, 0});
    graph.connect({tagIndex, filter, 3});
    graph.connect({tagValue, filter, 4});
    graph.connect({voices, assign, 0});
    graph.connect({filter, assign, 1});
    graph.connect({assign, signals, 0});
    graph.connect({signals, data, 0, 1});
    for (const auto [node, port] : std::array{std::pair{channel, 1}, std::pair{slot, 2}, std::pair{note, 3}, std::pair{tag, 4}}) {
        graph.connect({node, data, port});
    }
    const std::array mixers{add(PolyToMonoMin), add(PolyToMonoMax), add(PolyToMonoHop), add(PolyToMonoSelective)};
    graph.connect({hop, mixers[2], 1});
    graph.connect({group, mixers[3], 1});
    for (int i = 0; i < static_cast<int>(mixers.size()); ++i) {
        graph.connect({data, mixers[i], 0});
        graph.connect({mixers[i], capture, i});
    }
    graph.connect({ns, capture, 4});
    for (int i = 0; i < 5; ++i) sender.emit(0x90, i + 1, 60 + i, 127);
    graph.fireTrigger(capture);
    const auto samples = [&](int output) { return std::get<FloatArray>(graph.readValue(capture, output)); };
    check(samples(0) == FloatArray{66, 67, 68, 69, 70}, "Minimum should reduce real active voices");
    check(samples(1) == FloatArray{82, 83, 84, 85, 86}, "Maximum should reduce real active voices");
    check(samples(2) == FloatArray{370, 370, 380, 380, 390}, "Hop should hold the last reduction while DSP keeps advancing");
    check(samples(3) == FloatArray{82, 83, 84, 85, 86}, "Selective should collect the requested four-slot group");
}

void voiceBankReturnsHeldStolenNotes()
{
    using openstone::green::NoteEvent;
    using namespace openstone::stream;
    VoiceBank bank;
    bank.configure({.capacity = 1, .stolenPerSlot = 1, .returnStolen = true, .retriggerReturn = true});
    std::array<VoiceSlot, 1> slots{}, scratch{};
    const auto apply = [&](NoteEvent event) {
        auto result = bank.apply(std::array{event});
        check(result.size() == 1, "Every paired event should be delivered once");
        bank.read(slots, scratch);
        return result.front().id;
    };

    const auto first = apply({0, 60, 127, true});
    const auto generation = slots[0].generation;
    const auto second = apply({1, 60, 64, true});
    check(slots[0].id == second && slots[0].generation == generation && slots[0].triggerCount == 1,
          "A legato steal should preserve DSP state");
    check(apply({1, 60, 0, false}) == second && slots[0].id == first &&
          slots[0].generation != generation && slots[0].triggerCount == 2,
          "Release should return the stolen held note and retrigger when requested");
    check(apply({0, 60, 0, false}) == first && !slots[0].note.noteOn,
          "The returned note should release independently");
    bank.configure({.capacity = 2});
    bank.read(slots, scratch);
    check(!slots[0].id, "A new bank layout should clear the old slots");
}

void hostMidiPortsRouteWithoutDeviceBackends()
{
    using enum PrimitiveKind;
    using openstone::green::MidiMessage;
    Schematic graph;
    const auto input = graph.addNode(VstMidiIn, 0, 0);
    const auto output = graph.addNode(VstMidiOut, 0, 0);
    graph.connect({input, output, 0}, LinkNotification::Silent);
    std::vector<MidiMessage> received;
    graph.setHostMidiOutput([&](const auto& message) { received.push_back(message); });
    const MidiMessage note{{0x92, 60, 64}}, sysex{{0xf0, 0x7e, 0xf7}};
    graph.receiveHostMidi(note);
    graph.receiveHostMidi(sysex);
    graph.receiveHostMidi({{0x90, 60}});
    check(received == std::vector{note, sysex} && graph.inspectOutput(input).triggers == 2,
          "Host messages should cross the graph in order with their bytes intact");
}

void midiNotesRespectChannelsAndDeferredRelease()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    Schematic graph;
    const MidiSource a(graph);
    const MidiSource b(graph);
    const int notes = graph.addNode(MidiToNotes, 0, 0);
    const int mono = graph.addNode(MidiNote, 0, 0);
    graph.connect({a.event, notes, 0});
    graph.connect({b.event, notes, 0});
    graph.connect({a.event, mono, 0});
    graph.connect({b.event, mono, 0});
    const auto last = [&] { return std::get<Notes>(graph.readValue(notes)).events; };

    a.emit(0x90, 1, 60, 100);
    b.emit(0x90, 2, 69, 82);
    check(last() == std::vector<NoteEvent>{{1, 69, 82, true}} && integer(graph, mono) == 69 &&
          integer(graph, mono, 1) == 2 &&
          std::abs(std::get<float>(graph.readValue(mono, 2)) - 880.0 / graph.sampleRate()) < 1e-8,
          "Midi Note reports the latest held key, count, and Nyquist-normalized frequency");
    a.emit(0xB0, 1, 64, 127);
    const auto previous = graph.inspectOutput(notes).triggers;
    a.emit(0x90, 1, 60, 0);
    check(graph.inspectOutput(notes).triggers == previous && integer(graph, mono, 1) == 1,
          "Velocity-zero Note On releases the key and sustain defers the Notes release");
    b.emit(0xB0, 2, 121);
    check(graph.inspectOutput(notes).triggers == previous, "Controller reset on another channel cannot release the deferred note");
    a.emit(0xB0, 1, 121);
    check(last() == std::vector<NoteEvent>{{0, 60, 0, false}} && graph.inspectOutput(notes).triggers == previous + 1,
          "Controller reset releases only the deferred note on its channel");

    a.emit(0x90, 1, 60, 99);
    a.emit(0xB0, 1, 64, 127);
    a.emit(0x80, 1, 60);
    a.emit(0x90, 1, 60, 81);
    const auto beforeLift = graph.inspectOutput(notes).triggers;
    a.emit(0xB0, 1, 64, 0);
    check(graph.inspectOutput(notes).triggers == beforeLift + 1 &&
          last() == std::vector<NoteEvent>{{0, 60, 0, false}} && integer(graph, mono, 1) == 2,
          "Pedal lift emits the deferred release while the newer note at that pitch stays held");
    a.emit(0x80, 1, 60, 40);
    check(last() == std::vector<NoteEvent>{{0, 60, 40, false}} && integer(graph, mono) == 69,
          "An explicit Note Off retains release velocity and restores the earlier held pitch");

    a.emit(0x90, 1, 60, 99);
    a.emit(0x90, 1, 60, 81);
    a.emit(0x80, 1, 60, 40);
    check(last() == std::vector<NoteEvent>{{0, 60, 40, false}} && integer(graph, mono, 1) == 2 &&
          integer(graph, mono) == 60, "One Note Off consumes the oldest matching Note On");
    a.emit(0x80, 1, 60, 41);
    check(last() == std::vector<NoteEvent>{{0, 60, 41, false}} && integer(graph, mono, 1) == 1,
          "The second Note Off consumes the newer Note On on the same pitch");
}

void midiNoteEventSchedulesOriginalKeyAndSysexRoundTrips()
{
    using namespace std::chrono_literals;
    using enum PrimitiveKind;
    using namespace openstone::green;
    using openstone::TickTime;
    using openstone::TickWake;
    TickTime now{};
    Schematic graph({}, {}, {}, {}, {.monotonicClock = [&] { return now; }});
    const int note = graph.addNode(MidiNoteEvent, 0, 0);
    const int send = graph.addNode(Trigger, 0, 0);
    const int key = graph.addNode(Int, 0, 0, std::int32_t{64});
    const int duration = graph.addNode(Int, 0, 0, std::int32_t{100});
    const int convert = graph.addNode(MidiToNotes, 0, 0);
    graph.connect({send, note, 4}, LinkNotification::Silent);
    graph.connect({key, note, 1});
    graph.connect({duration, note, 3});
    graph.connect({note, convert, 0});
    graph.fireTrigger(send);
    check(std::get<Notes>(graph.readValue(convert)).events == std::vector<NoteEvent>{{0, 64, 100, true}} &&
          graph.advanceTicks(now, TickWake::Configure).deadline == now + 100ms, "Note Event starts immediately and arms Note Off");
    graph.setValue(key, std::int32_t{67});
    graph.setValue(duration, std::int32_t{200});
    now += 25ms;
    graph.fireTrigger(send);
    now = TickTime{} + 99ms;
    check(!graph.advanceTicks(now, TickWake::Timer).fired, "Note Off never fires early");
    now += 2ms;
    check(graph.advanceTicks(now, TickWake::Timer).fired &&
          std::get<Notes>(graph.readValue(convert)).events == std::vector<NoteEvent>{{0, 64, 0, false}},
          "Editing the controls leaves the scheduled event's original key and deadline intact");
    now = TickTime{} + 226ms;
    check(graph.advanceTicks(now, TickWake::Timer).fired &&
          std::get<Notes>(graph.readValue(convert)).events == std::vector<NoteEvent>{{0, 67, 0, false}},
          "An overlapping note keeps its own deadline");

    const int encode = graph.addNode(MidiStringToSysex, 0, 0);
    const int decode = graph.addNode(MidiSysexToString, 0, 0);
    const int hex = graph.addNode(String, 0, 0, std::string{"7e 7f 09 01"});
    const int sendHex = graph.addNode(Trigger, 0, 0);
    graph.connect({hex, encode, 0});
    graph.connect({sendHex, encode, 1}, LinkNotification::Silent);
    graph.connect({encode, decode, 0});
    graph.connect({note, decode, 0});
    graph.fireTrigger(sendHex);
    check(std::get<MidiMessage>(graph.readValue(encode)).bytes == std::vector<std::uint8_t>{0xF0, 0x7E, 0x7F, 9, 1, 0xF7} &&
          std::get<std::string>(graph.readValue(decode)) == "F0 7E 7F 09 01 F7" &&
          graph.inspectOutput(decode, 1).triggers == 1, "SysEx conversion preserves the framed bytes");
    for (const auto invalid : {"F0 7E", "F0 80 F7", "F0 7 F7", "zz"}) {
        graph.setValue(hex, std::string{invalid});
        graph.fireTrigger(sendHex);
    }
    graph.fireTrigger(send);
    check(graph.inspectOutput(decode, 1).triggers == 1, "Malformed SysEx and ordinary MIDI messages leave the string unchanged");
}

class TestMidiBackend final : public openstone::MidiDeviceBackend {
public:
    using Message = openstone::green::MidiMessage;
    using Direction = openstone::MidiDirection;
    using Info = openstone::MidiDeviceInfo;

    struct Input final : openstone::MidiInputPort {
        Input(TestMidiBackend& backend, std::string id) : backend(backend), id(std::move(id)) {}
        ~Input() override
        {
            ++backend.closedInputs[id];
            backend.incoming[id].clear();
        }

        std::optional<Message> receive() override
        {
            auto& queue = backend.incoming[id];
            if (queue.empty()) return {};
            auto message = std::move(queue.front());
            queue.pop_front();
            return message;
        }

        TestMidiBackend& backend;
        std::string id;
    };

    struct Output final : openstone::MidiOutputPort {
        Output(TestMidiBackend& backend, std::string id) : backend(backend), id(std::move(id)) {}
        ~Output() override { ++backend.closedOutputs[id]; }

        void send(const Message& message) override
        {
            backend.outgoing[id].push_back(message);
        }

        TestMidiBackend& backend;
        std::string id;
    };

    std::vector<Info> list(Direction direction) override
    {
        return direction == Direction::Input ? inputs : outputs;
    }

    std::unique_ptr<openstone::MidiInputPort> openInput(std::string_view id) override
    {
        if (id == blockedInput) throw std::runtime_error("Test device is busy");
        ++openedInputs[std::string{id}];
        return std::make_unique<Input>(*this, std::string{id});
    }

    std::unique_ptr<openstone::MidiOutputPort> openOutput(std::string_view id) override
    {
        ++openedOutputs[std::string{id}];
        return std::make_unique<Output>(*this, std::string{id});
    }

    std::vector<Info> inputs{{"keys", "Keyboard"}, {"pads", "Pads"}};
    std::vector<Info> outputs{{"rack", "Rack"}, {"synth", "Synth"}};
    std::string blockedInput;
    std::map<std::string, int> openedInputs, closedInputs, openedOutputs, closedOutputs;
    std::map<std::string, std::deque<Message>> incoming;
    std::map<std::string, std::vector<Message>> outgoing;
};

struct SerialFixture {
    std::string incoming;
    std::string queued;
    std::string written;
    std::size_t writeLimit = 2;
    bool connected = false;
    bool disconnected = false;
};

class FakeSerialPort final : public openstone::SerialDevicePort {
public:
    explicit FakeSerialPort(SerialFixture& fixture) : fixture_(fixture) { fixture_.connected = true; }
    ~FakeSerialPort() override
    {
        fixture_.connected = false;
        fixture_.queued.clear();
        fixture_.incoming.clear();
    }

    std::string read(std::size_t maximum) override
    {
        if (fixture_.disconnected) throw std::runtime_error("Device disconnected");
        auto bytes = fixture_.incoming.substr(0, maximum);
        fixture_.incoming.erase(0, bytes.size());
        return bytes;
    }

    std::size_t write(std::string_view bytes) override
    {
        const auto count = std::min(fixture_.writeLimit, bytes.size());
        fixture_.queued.append(bytes.substr(0, count));
        return count;
    }

    std::size_t pendingWrites() const override { return fixture_.queued.size(); }
    void clearWrites() override { fixture_.queued.clear(); }

private:
    SerialFixture& fixture_;
};

class FakeSerialBackend final : public openstone::SerialDeviceBackend {
public:
    std::vector<openstone::SerialDeviceInfo> list() override { return devices; }

    std::unique_ptr<openstone::SerialDevicePort> open(std::string_view id,
                                                     const openstone::SerialPortSettings& settings) override
    {
        selected = id;
        applied = settings;
        if (fixture.connected) throw std::runtime_error("Port is already in use");
        return std::make_unique<FakeSerialPort>(fixture);
    }

    void drain() { fixture.written += std::exchange(fixture.queued, {}); }

    std::vector<openstone::SerialDeviceInfo> devices{{8, "port-8"}, {2, "port-2"}};
    std::string selected;
    openstone::SerialPortSettings applied;
    SerialFixture fixture;
};

void comPortsRefreshTheListAcrossModules()
{
    using namespace openstone;
    using namespace openstone::green;
    using enum PrimitiveKind;
    Session session;
    auto backend = std::make_shared<FakeSerialBackend>();
    auto& root = session.graph();
    root.setSerialDeviceBackend(backend);
    const int list = session.addNode(ComPorts, 0, 0);
    const int module = session.addNode(Module, 0, 0);
    auto& inside = *root.module(module);
    const int port = inside.addNode(ComPort, 0, 0);
    const int refresh = root.addNode(Trigger, 0, 0);
    root.connect({refresh, list, 0});
    check(root.readValue(list) == Value{IntArray{2, 8}} && inside.readValue(port, 4) == Value{IntArray{2, 8}},
          "Both COM primitives read sorted positive port numbers from the same backend");
    const auto listUpdates = root.inspectOutput(list).triggers;
    const auto portUpdates = inside.inspectOutput(port, 4).triggers;
    backend->devices = {{9, "port-9"}, {9, "duplicate"}, {0, "invalid"}, {2, "port-2"}};
    root.fireTrigger(refresh);
    check(root.readValue(list) == Value{IntArray{2, 9}} &&
          inside.readValue(port, 4) == Value{IntArray{2, 9}} &&
          root.inspectOutput(list).triggers == listUpdates + 1 &&
          inside.inspectOutput(port, 4).triggers == portUpdates + 1,
          "Refresh publishes the changed list to both primitives across schematic levels");

    auto replacement = std::make_unique<Schematic>();
    const int reload = replacement->addNode(AfterLoad, 0, 0);
    const int newList = replacement->addNode(ComPorts, 0, 0);
    replacement->connect({reload, newList, 0});
    check(session.openSchematic(std::move(replacement)) &&
          session.graph().readValue(newList) == Value{IntArray{2, 9}} &&
          session.graph().inspectOutput(newList).triggers == 1,
          "The host backend remains available to After Load on project replacement");
}

void comPortTransfersTextAndHexAndClearsPendingWrites()
{
    using namespace openstone;
    using namespace openstone::green;
    using enum PrimitiveKind;
    auto backend = std::make_shared<FakeSerialBackend>();
    Schematic graph;
    graph.setSerialDeviceBackend(backend);
    const int port = graph.addNode(ComPort, 0, 0);
    const auto input = [&](PrimitiveKind kind, Value value, int connector) {
        const int node = graph.addNode(kind, 0, 0, std::move(value));
        graph.connect({node, port, connector});
        return node;
    };
    const auto button = [&](int connector) {
        const int node = graph.addNode(Trigger, 0, 0);
        graph.connect({node, port, connector});
        return node;
    };
    const int number = input(Int, 8, 0);
    input(Int, 115200, 1);
    input(Int, 2, 2);
    input(Int, 1, 3);
    input(Int, 7, 4);
    const int data = input(String, std::string{"hi"}, 7);
    const int terminator = input(Int, 0, 8);
    const int hex = input(Boolean, false, 10);
    input(Boolean, true, 11);
    const int noNull = input(Boolean, false, 13);
    const int open = button(5), close = button(6), send = button(9), clearLog = button(12), clearSends = button(15);
    check(!backend->fixture.connected && !graph.hasOpenSerialPorts(), "Wiring Open does not open hardware");
    graph.fireTrigger(open);
    check(backend->selected == "port-8" && backend->applied == SerialPortSettings{115200, 2, 1, 7} &&
          graph.readValue(port, 0) == Value{true} && graph.readValue(port, 5) == Value{false} &&
          graph.inspectOutput(port, 5).triggers == 2, "Open passes the selected device and line settings to the backend");

    const auto sent = graph.inspectOutput(port, 2).triggers;
    graph.fireTrigger(send);
    check(graph.readValue(port, 6) == Value{3}, "A text send queues its final NUL");
    static_cast<void>(graph.pollSerialDevices());
    check(backend->fixture.queued == "hi" && graph.inspectOutput(port, 2).triggers == sent,
          "A partial device write keeps the unsent data and waits for completion");
    backend->drain();
    static_cast<void>(graph.pollSerialDevices());
    check(backend->fixture.queued == std::string(1, '\0') && graph.readValue(port, 6) == Value{1},
          "The remaining byte is written without duplicating the partial send");
    backend->drain();
    check(graph.pollSerialDevices() && backend->fixture.written == std::string("hi\0", 3) &&
          graph.readValue(port, 6) == Value{0} && graph.inspectOutput(port, 2).triggers == sent + 1,
          "Out pulses once when the complete write drains");

    const auto received = graph.inspectOutput(port, 1).triggers;
    backend->fixture.incoming = "rea";
    check(!graph.pollSerialDevices() && graph.inspectOutput(port, 1).triggers == received,
          "An incomplete NUL-delimited frame stays buffered across polls");
    backend->fixture.incoming = std::string("dy\0ok\0", 6);
    check(graph.pollSerialDevices() && graph.readValue(port, 1) == Value{std::string{"ok"}} &&
          graph.inspectOutput(port, 1).triggers == received + 2,
          "Two complete frames emit in order as separate input events");

    graph.setValue(hex, true);
    graph.setValue(terminator, -1);
    graph.setValue(data, std::string{"00 ff A5"});
    graph.fireTrigger(send);
    for (int i = 0; i < 3; ++i) {
        static_cast<void>(graph.pollSerialDevices());
        backend->drain();
    }
    check(backend->fixture.written == std::string("hi\0\x00\xFF\xA5", 6) &&
          graph.inspectOutput(port, 2).triggers == sent + 2, "Hex sends literal bytes without a trailing NUL");
    backend->fixture.incoming = std::string("\x00\xFF", 2);
    check(graph.pollSerialDevices() && graph.readValue(port, 1) == Value{std::string{"FF"}} &&
          graph.inspectOutput(port, 1).triggers == received + 4,
          "Single-byte mode delivers one hex event per byte including NUL");

    graph.setValue(data, std::string{"QZ"});
    graph.fireTrigger(send);
    check(graph.readValue(port, 6) == Value{0} && graph.inspectOutput(port, 2).triggers == sent + 2 &&
          std::get<std::string>(graph.readValue(port, 3)).contains("hexadecimal"),
          "Invalid hex keeps the send queue empty and reports the error to Log");
    graph.fireTrigger(clearLog);
    check(graph.readValue(port, 3) == Value{std::string{}}, "ClearLog resets the log");
    graph.setValue(hex, false);
    graph.setValue(noNull, true);
    graph.setValue(data, std::string{"bye"});
    graph.fireTrigger(send);
    check(graph.readValue(port, 6) == Value{3}, "NoNull keeps the raw text length");
    static_cast<void>(graph.pollSerialDevices());
    graph.fireTrigger(clearSends);
    check(backend->fixture.queued.empty() && graph.readValue(port, 6) == Value{0} &&
          graph.inspectOutput(port, 2).triggers == sent + 2, "ClearSends cancels both backend and unsent bytes");

    graph.fireTrigger(close);
    check(!backend->fixture.connected && !graph.hasOpenSerialPorts() && graph.readValue(port, 0) == Value{false},
          "Close releases the hardware handle");
    graph.setValue(number, 99);
    graph.fireTrigger(open);
    check(graph.readValue(port, 0) == Value{false} && graph.readValue(port, 5) == Value{false} &&
          std::get<std::string>(graph.readValue(port, 3)).contains("unavailable"),
          "Opening a missing device leaves both status outputs false and reports an error");
    graph.setValue(number, 8);
    graph.fireTrigger(open);
    backend->fixture.disconnected = true;
    check(graph.pollSerialDevices() && !graph.hasOpenSerialPorts() &&
          std::get<std::string>(graph.readValue(port, 3)).contains("disconnected"),
          "A device fault during polling closes the handle and publishes an error");
}

void comPortCopiesAndUndoDoNotHoldDeletedHardware()
{
    using namespace openstone;
    using namespace openstone::green;
    using enum PrimitiveKind;
    auto backend = std::make_shared<FakeSerialBackend>();
    Session session;
    auto& graph = session.graph();
    graph.setSerialDeviceBackend(backend);
    const int port = session.addNode(ComPort, 0, 0);
    const int select = graph.addNode(Int, 0, 0, 2);
    const int open = session.addNode(Trigger, 0, 0);
    graph.connect({select, port, 0});
    graph.connect({open, port, 5});
    graph.fireTrigger(open);
    check(backend->applied == SerialPortSettings{}, "Unconnected line settings use 9600 baud and 8N1");

    session.copyNode(port);
    const int copy = session.pasteNode(100, 0, false);
    check(graph.readValue(port, 0) == Value{true} && graph.readValue(copy, 0) == Value{false},
          "An open COM Port remains live while its copy starts closed");
    check(session.beginEdit(SchematicEdit::DeleteNodes), "Record the node deletion");
    session.removeNode(port);
    session.finishEdit();
    check(!backend->fixture.connected, "Undo history does not retain the deleted device handle");
    check(session.applyHistory(false, [](auto) {}) &&
          session.graph().readValue(port, 0) == Value{false}, "Undo restores the node with its hardware closed");
    session.graph().fireTrigger(open);
    check(session.graph().readValue(port, 0) == Value{true}, "A restored node can open using the same backend");
}

void midiDevicesRouteToSelectedHardware()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    Schematic graph;
    auto backend = std::make_shared<TestMidiBackend>();
    graph.setMidiDeviceBackend(backend);
    const int input = graph.addNode(MidiIn, 0, 0);
    const int inputs = graph.addNode(MidiInDevices, 0, 0);
    const int outputs = graph.addNode(MidiOutDevices, 0, 0);
    const int output = graph.addNode(MidiOut, 0, 0);
    const int splitter = graph.addNode(MidiSplit, 0, 0);
    check(graph.refreshMidiDevices() && graph.watchesMidiDevices() && !graph.listensToMidi(),
          "Listing does not claim or open hardware");
    check(integer(graph, inputs) == 2 && std::get<StringArray>(graph.readValue(inputs, 1)) == StringArray{"Keyboard", "Pads"} &&
          std::get<IntArray>(graph.readValue(outputs, 2)) == IntArray{1, 1},
          "Device listing exposes counts, stable order, names and availability");

    graph.connect({input, splitter, 0});
    graph.connect({input, output, 0});
    graph.setMidiDeviceSelected(input, "keys", true);
    graph.setMidiDeviceSelected(input, "pads", true);
    graph.setMidiDeviceSelected(output, "synth", true);
    graph.setMidiDeviceSelected(output, "rack", true);
    check(graph.refreshMidiDevices(false) && graph.listensToMidi() &&
          std::get<IntArray>(graph.readValue(inputs, 2)) == IntArray{0, 0}, "Selection opens each chosen port");

    const MidiMessage first{{0x91, 65, 99}};
    const MidiMessage second{{0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7}};
    backend->incoming["keys"] = {first, second};
    check(graph.pollMidiDevices() && !graph.pollMidiDevices() &&
          backend->outgoing["synth"] == std::vector<MidiMessage>{first, second} &&
          backend->outgoing["rack"] == std::vector<MidiMessage>{first, second} &&
          graph.inspectOutput(splitter, 4).triggers == 2,
          "Incoming MIDI and SysEx enter the trigger graph and reach both selected outputs in order");

    const int late = graph.addNode(MidiOut, 0, 0);
    graph.connect({input, late, 0});
    check(backend->outgoing["synth"].size() == 2, "A new MIDI wire never replays a previous hardware message");
    graph.setMidiDeviceSelected(input, "keys", false);
    graph.setMidiDeviceSelected(output, "rack", false);
    check(graph.refreshMidiDevices(false) && backend->closedInputs["keys"] == 1 &&
          backend->closedOutputs["rack"] == 1, "Deselecting closes each connection");
    backend->incoming["pads"].push_back(first);
    check(graph.pollMidiDevices() && backend->outgoing["rack"].size() == 2 &&
          backend->outgoing["synth"].size() == 3, "Remaining selected hardware keeps working");

    graph.removeNode(input);
    graph.removeNode(output);
    check(graph.refreshMidiDevices(false) && !graph.listensToMidi() &&
          backend->closedInputs["pads"] == 1 && backend->closedOutputs["synth"] == 1,
          "Removing endpoint nodes releases the hardware");
}

void indexedMidiDevicesHandleBusyPortsAndHotplug()
{
    using enum PrimitiveKind;
    using namespace openstone::green;
    Schematic graph;
    auto backend = std::make_shared<TestMidiBackend>();
    graph.setMidiDeviceBackend(backend);
    const int module = graph.addNode(Module, 0, 0);
    auto& child = *graph.module(module);
    const int device = child.addNode(MidiInSelect, 0, 0);
    const int index = child.addNode(Int, 0, 0, std::int32_t{0});
    const int split = child.addNode(MidiSplit, 0, 0);
    const int contender = graph.addNode(MidiInSelect, 0, 0);
    const int other = graph.addNode(Int, 0, 0, std::int32_t{0});
    const int output = graph.addNode(MidiOutSelect, 0, 0);
    const int outIndex = graph.addNode(Int, 0, 0, std::int32_t{1});
    const MidiSource source(graph);
    static_cast<void>(graph.refreshMidiDevices());
    child.connect({device, split, 0});
    check(integer(child, device, 1) == -1 && integer(graph, output) == -1,
          "Select primitives start disconnected with no index input");

    backend->blockedInput = "keys";
    child.connect({index, device, 0});
    check(integer(child, device, 1) == -1 && child.inspectOutput(device, 2).triggers == 1,
          "A busy backend produces a single Unavailable trigger");
    check(!graph.refreshMidiDevices() && child.inspectOutput(device, 2).triggers == 1,
          "Polling a still-busy port does not repeatedly fire Unavailable");
    backend->blockedInput.clear();
    check(graph.refreshMidiDevices() && integer(child, device, 1) == 0, "A selected port connects when it becomes available");

    graph.connect({other, contender, 0});
    check(integer(graph, contender, 1) == -1 && graph.inspectOutput(contender, 2).triggers == 1 &&
          backend->openedInputs["keys"] == 1, "One node owns each opened port in a given direction");
    graph.connect({outIndex, output, 1});
    graph.connect({source.event, output, 0});
    source.emit(0x90, 1, 70, 80);
    check(integer(graph, output) == 1 && backend->outgoing["synth"] == std::vector<MidiMessage>{{{0x90, 70, 80}}},
          "MIDI Out Select sends the next trigger-driven message to its selected device");

    backend->inputs.erase(backend->inputs.begin());
    std::ranges::reverse(backend->outputs);
    check(graph.refreshMidiDevices() && !graph.listensToMidi() && integer(child, device, 1) == -1 &&
          integer(graph, output) == 0 && backend->closedInputs["keys"] == 1 &&
          backend->openedInputs["pads"] == 0, "Port identity survives a reorder and prevents redirecting to another device");
    backend->inputs.push_back({"keys", "Keyboard"});
    check(graph.refreshMidiDevices() && graph.listensToMidi() && backend->openedInputs["keys"] == 2,
          "The intended hardware reconnects when it is listed again");
    graph.setValue(other, std::int32_t{-1});
    if (integer(child, device, 1) == -1) {
        static_cast<void>(graph.refreshMidiDevices());
    }
    check(integer(child, device, 1) == 1, "The available nested owner follows the updated enumeration index");
    backend->incoming["keys"].push_back({{0x92, 63, 100}});
    check(graph.pollMidiDevices() && integer(child, split, 1) == 3,
          "Input routing continues while the selected module is outside the editor viewport");
    child.setValue(index, std::int32_t{-1});
    graph.setValue(outIndex, std::int32_t{-1});
    check(!graph.listensToMidi() && integer(graph, output) == -1 && backend->closedOutputs["synth"] == 1,
          "Minus one immediately disconnects both programmatic selectors");
}

void alphaTypesRouteThroughModuleEndpoints()
{
    using enum openstone::green::Type;
    constexpr std::array types{
        Mem, MemArray, Stream, Poly, Mono, Mono4, StreamBoolean, PolyBoolean, MonoBoolean,
        StreamDouble, PolyDouble, MonoDouble, Mono4Double, MIDI, Notes, Voices, Preset,
    };
    for (const auto type : types) {
        Schematic graph;
        const int source = graph.addNode(PrimitiveKind::Module, 0, 0);
        const int target = graph.addNode(PrimitiveKind::Module, 240, 0);
        auto& producer = *graph.module(source);
        auto& consumer = *graph.module(target);
        const int output = producer.addNode(PrimitiveKind::ModuleOutput, 0, 0);
        const int input = consumer.addNode(PrimitiveKind::ModuleInput, 0, 0);
        const int through = consumer.addNode(PrimitiveKind::ModuleOutput, 240, 0);
        producer.setModulePortType(output, type);

        const auto initialTriggers = graph.triggers();
        const SchematicLink link{source, target, 0};
        graph.connect(link);
        const auto wire = graph.inspectLink(link);
        consumer.connect({input, through, 0});
        check(graph.outputType(source) == type && graph.inputType(target, 0) == type &&
              consumer.outputType(input) == type && graph.outputType(target) == type,
              "Module boundaries must preserve all Alpha connector types");
        const auto readout = graph.inspectOutput(target);
        if (openstone::green::isDspType(type)) {
            check(!wire.value && !readout.value && wire.triggers == 0 &&
                  graph.triggers() == initialTriggers,
                  "DSP links must stay out of the Green value and trigger runtime");
        } else {
            const auto pulses = openstone::green::deliversDiscreteEvents(type) ? 0 : 1;
            check(wire.value && wire.triggers == pulses && readout.value &&
                  openstone::green::typeOf(*readout.value) == type && readout.triggers == pulses,
                  "Value-bearing Alpha connectors and triggers must cross a module boundary");
        }

        const int integer = graph.addNode(PrimitiveKind::Int, 480, 0);
        bool rejected = false;
        try {
            graph.connect({target, integer, 0});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        check(rejected && graph.links().size() == 1 && graph.outputType(target) == type,
              "An incompatible Alpha connector link must leave the graph intact");
    }
}

void templateModuleOutputsFollowGenericSources()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    using openstone::green::Value;
    Schematic graph;
    const int wrapper = graph.addNode(Module, 200, 0);
    auto& child = *graph.module(wrapper);
    const int input = child.addNode(ModuleInput, 0, 0);
    const int output = child.addNode(ModuleOutput, 400, 0);
    const int add = child.addNode(Add, 200, 0);
    const int offset = child.addNode(Int, 0, 100, std::int32_t{1});
    child.connect({offset, add, 0});
    child.connect({input, add, 1});
    child.connect({add, output, 0});
    check(graph.outputType(wrapper) == Type::Int, "The output should follow the Add's initial Int type");
    const int source = graph.addNode(Float, 0, 0, 2.5F);
    graph.connect({source, wrapper, 0});
    check(child.outputType(add) == Type::Float && graph.outputType(wrapper) == Type::Float &&
          graph.readValue(wrapper) == Value{3.5F},
          "The output should update when the incoming Float changes the Add's type");
}

void manualPortOrderFollowsLayoutAndKeepsWires()
{
    using enum PrimitiveKind;
    using openstone::green::Value;
    Schematic graph;
    const int wrapper = graph.addNode(Module, 200, 0);
    auto& child = *graph.module(wrapper);
    const int rightInput = child.addNode(ModuleInput, 100, 0);
    const int rightOutput = child.addNode(ModuleOutput, 500, 0);
    child.connect({rightInput, rightOutput, 0});
    const int source = graph.addNode(Int, 0, 0, std::int32_t{3});
    const int sink = graph.addNode(Int, 400, 0);
    graph.connect({source, wrapper, 0});
    graph.connect({wrapper, sink, 0});
    const auto pulses = graph.inspectLink({source, wrapper, 0}).triggers;

    const int leftInput = child.addNode(ModuleInput, 0, 0);
    const int leftOutput = child.addNode(ModuleOutput, 300, 0);
    child.connect({leftInput, leftOutput, 0});
    check(std::ranges::contains(graph.links(), SchematicLink{source, wrapper, 1}) &&
          std::ranges::contains(graph.links(), SchematicLink{wrapper, sink, 0, 1}) &&
          graph.readValue(sink) == Value{std::int32_t{3}},
          "Inserting ports to the left should move both existing ports without moving their wires");
    child.moveNode(rightInput, 100, -100);
    child.moveNode(rightOutput, 500, -100);
    child.setModulePortName(rightInput, "New name");
    check(graph.inspectLink({source, wrapper, 0}).triggers == pulses &&
          graph.inspectInput(wrapper, 0).triggers == pulses &&
          graph.readValue(sink) == Value{std::int32_t{3}},
          "Moving ports above their neighbors and renaming them should preserve connections and counts");
    const int otherSource = graph.addNode(Int, 0, 100, std::int32_t{7});
    const int otherSink = graph.addNode(Int, 400, 100);
    graph.connect({otherSource, wrapper, 1});
    graph.connect({wrapper, otherSink, 0, 1});
    child.removeNode(rightInput);
    child.removeNode(rightOutput);
    check(graph.links() == std::vector<SchematicLink>{{otherSource, wrapper, 0}, {wrapper, otherSink, 0}} &&
          child.links() == std::vector<SchematicLink>{{leftInput, leftOutput, 0}} &&
          graph.readValue(otherSink) == Value{std::int32_t{7}},
          "Removing an endpoint should remove only its own wires and keep its neighbors connected");
}

void manualPortsSynchronizeAndRejectIncompatibleTypes()
{
    using enum PrimitiveKind;
    using openstone::green::Type;
    openstone::Session session;
    const int original = session.addNode(Module, 0, 0);
    session.copyNode(original);
    const int peer = session.pasteNode(200, 0, true);
    check(session.enterModule(original), "Enter original");
    const int input = session.addNode(ModuleInput, 0, 0);
    const int output = session.addNode(ModuleOutput, 240, 0);
    session.renameNode(input, "Sample");
    session.renameNode(output, "Result");
    check(session.navigate(0), "Return to Main");
    const int source = session.addNode(Int, 0, 200);
    session.connect({source, peer, 0});
    check(session.enterModule(original), "Open original");
    bool rejected = false;
    try {
        // Only the peer has an outside wire, which cannot feed a Color input.
        session.setModulePortType(input, Type::Color);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected && !session.graph().outputType(input) &&
          session.graphAt(0).inputType(peer, 0) == Type::Int,
          "An incompatible external wire on a synchronized peer must roll back the type edit everywhere");
    session.setModulePortType(input, Type::Float);
    const int inside = session.addNode(Int, 400, 100);
    session.connect({inside, output, 0});
    rejected = false;
    try {
        session.setModulePortType(output, Type::Color);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected && session.graph().inputType(output, 0) == Type::Int &&
          std::ranges::contains(session.graph().links(), SchematicLink{inside, output, 0}),
          "An incompatible internal wire must also survive a rejected type change");
    check(session.navigate(0), "Return to Main after edits");
    for (const auto& entry : session.graph().nodes()) {
        if (entry.id == original || entry.id == peer) {
            check(entry.moduleInputs.front().name == "Sample" && entry.moduleInputs.front().type == Type::Float &&
                  entry.moduleOutputs.front().name == "Result",
                  "Addition, names and accepted types should propagate to synchronized copies");
        }
    }
    session.copyNode(original);
    const int copy = session.pasteNode(450, 0, false);
    check(session.graph().inputType(copy, 0) == Type::Float && session.graph().outputType(copy, 0) == Type::Int,
          "Copying a module should preserve its manual interface");
    check(session.enterModule(original), "Enter original to remove a synced port");
    session.setTemplateType(input, std::nullopt);
    check(!session.graph().outputType(input) && session.graphAt(0).inputType(peer, 0) == Type::Int &&
          session.graphAt(0).inputType(copy, 0) == Type::Float,
          "Template mode is synchronized while each copy infers from its own outside wiring");
    session.removeNode(input);
    check(session.navigate(0) && session.graph().links().empty() &&
          session.graph().module(original)->nodes().size() == 2 &&
          session.graph().module(peer)->nodes().size() == 2 &&
          session.graph().inputType(copy, 0) == Type::Float,
          "Deleting a synchronized endpoint should remove the peer's outside wire but leave unsynced copies alone");
}

}

int main()
{
    try {
        editHistoryReplaysMovesAndLinksAsGestures();
        editHistoryRestoresGroupedBoundaryWires();
        editHistoryRestoresReorderedModuleInputs();
        copySelectionPreservesOnlyInternalWires();
        cutSelectionRestoresOriginalWiring();
        duplicateSelectionPreservesSpacingWiringAndClipboard();
        ticksUseTheManualClockOrDisplayFrames();
        triggerDivCountsPulsesAndRestartsOnDivisorEvents();
        triggerDeltaMeasuresSuccessivePulsesInMilliseconds();
        indexerReportsEachInputWithoutRenumberingOtherLinks();
        loopPublishesValueAndIterationInStepOrder();
        advancedCounterStepsAndResetsWithinItsLimits();
        advancedCounterAlternatesAndDeliversLimitBeforeValue();
        advancedCounterStepsWithFlowStoneIntOverflow();
        afterLoadFiresOnceWhenOpeningASchematic();
        anAfterLoadErrorLeavesTheOpenedSchematicUsable();
        fileUtilitiesFindNamesAndGenerateGuids();
        fileDialogRequestsReturnToTheNodeThatOpenedThem();
        schematicPathIsAvailableFromAfterLoad();
        afterDuplicateFiresOnlyInCopiedNodes();
        afterDuplicateReachesNestedModuleCopies();
        aboutToCloseFiresOnceBeforeProjectReplacement();
        aboutToCloseWaitsForDoneInEveryHoldingModule();
        aboutToCloseAcceptsSynchronousDoneFeedback();
        triggerThreadDefersPulsesAndPullsCurrentValues();
        triggerThreadPreservesFifoAcrossModules();
        triggerThreadFeedbackWaitsForTheNextTurn();
        triggerThreadUsesTheLiveSchematic();
        triggerThreadCanCompleteAHeldClose();
        valuePatch();
        nativeStreamMathAdaptsAndRendersInBothPrecisions();
        nativeStreamMathSupportsUnaryAndBinaryInputsForLiveAudio();
        streamRoutingPreservesStaggeredFeedbackInEveryLane();
        stagedDspCombAcceptsFeedbackAcrossAModuleInput(false);
        stagedDspCombAcceptsFeedbackAcrossAModuleInput(true);
        stageOneCanReadSummedEarlySignals();
        packedAllpassCanFeedTheNextLaneWithoutAnExtraSample();
        shortedMonoModulesFeedBackThePreviousSample();
        impulseFiresOnTheFirstSampleAcrossBufferSizes();
        packedImpulseFiresInAllFourLanes();
        streamTrigonometryUsesTurnsAtBothPrecisions();
        streamComparisonsKeepPrecisionAndBooleanLanesAcrossBufferSizes();
        floatArrayReadRoundsAndWrapsIndicesAcrossBufferSizes();
        floatArrayReadPublishesEditsToRunningAudio();
        streamSelectRoutesEachSampleAndLaneAtBothPrecisions();
        streamSampleAndHoldCapturesAtFrequencyOrZeroCrossings();
        streamPitchToFreqUsesTheSampleRateAtBothPrecisions();
        sampleRatePublishesChangesAndUpdatesRunningAudio();
        streamCountersStartAtZeroAndResetAcrossBufferSizes();
        biquadImpulseResponsesMatchAcrossBufferSizes();
        packedBiquadsReadCoefficientsEverySample();
        generatedCoefficientsProduceLowPassImpulseResponses();
        lowPassCoefficientsTrackCutoffAndResonanceEveryFourSamples();
        biquadResponsesMatchTheLowPassImpulseTransform();
        biquadResponsesPublishChangesAndUseTheDenominatorGain();
        rampStartsWrapsAndResetsOnTheRisingSyncEdge();
        packedRampKeepsIndependentPhasesAndSyncEdges();
        using enum PrimitiveKind;
        oscillatorAppliesPhaseOffsetAndHardSyncAcrossBufferBoundaries(Sawtooth, DoubleSawtooth);
        oscillatorAppliesPhaseOffsetAndHardSyncAcrossBufferBoundaries(Sine, DoubleSine);
        oscillatorAppliesPhaseOffsetAndHardSyncAcrossBufferBoundaries(Triangle, DoubleTriangle);
        packedOscillatorMatchesIndependentDoubleRuns(DoubleSawtooth);
        packedOscillatorMatchesIndependentDoubleRuns(DoubleSine);
        packedOscillatorMatchesIndependentDoubleRuns(DoubleTriangle);
        glideRampsBothDirectionsAcrossBufferBoundaries();
        glideUsesEachPackedLanesLength();
        streamDeZipperTracksEachLaneOnSampleHops();
        floatDeZipperSmoothsGreenStepsAndResetsNextSample();
        streamDeZipperResetsAtTheNextSampleHop();
        variableDelayPrimitivesShiftAnImpulseByTheRequestedSamples();
        variableDelayUpdatesWithoutRestartingLiveAudio();
        streamDelayPreservesScalarStateAcrossLiveAudioCalls();
        dspCodeNodesCaptureAndKeepNamedLinks();
        assemblerControlFlowMatchesFlowStone();
        assemblerRegisterCallsMatchFlowStone();
        assemblerIndirectMemoryMatchesFlowStone();
        openstone::tests::withReferenceSources("assemblerMultiSampleMatchesFlowStoneCaptures", [&] { assemblerMultiSampleMatchesFlowStoneCaptures(); });
        openstone::tests::withReferenceSources("assemblerMemArrayWaveData", [&] { assemblerMemArrayWaveData(); });
        assemblerPackedPolyMatchesFlowStone();
        openstone::tests::withReferenceSources("assemblerX87MatchesFlowStoneCaptures", [&] { assemblerX87MatchesFlowStoneCaptures(); });
        assemblerCommunityX87ProbeHasDocumentedReadouts();
        openstone::tests::withReferenceSources("assemblerDenormalMaskMatchesFlowStoneWithMeasuredRegisters", [&] { assemblerDenormalMaskMatchesFlowStoneWithMeasuredRegisters(); });
        assemblerDoubleMatchesFlowStoneCaptures();
        assemblerFloatingPointEdgesMatchFlowStone();
        assemblerDoubleComparisonsMatchFlowStoneMasks();
        assemblerDoubleBitwiseMatchesFlowStoneCaptures();
        dspSwatchesObserveOnlyTheLastCapture();
        genericStreamsFollowTheirConnectedDspSection();
        monoPolyBridgesExposeOptionalVoiceIndex();
        signalCapturePrimitivesPublishOnTrigger();
        monoToFloatSamplesOnTick();
        streamToGreenSamplesBooleanOnlyOnGet();
        streamToGreenReadsTheLatestLiveBooleanSample();
        matrixRoutesGreenValuesAndReplacesItsConnections();
        matrixRoutesTriggerPulsesWithoutFiringOnReconfiguration();
        matrixChangesStreamRoutesOnTheNextBuffer();
        matrixPreservesMonoDoubleAndResolvesGenericStreams();
        audioEndpointsProcessHostBuffers();
        liveAudioReadoutsObserveTheRunningGraph();
        triggerPatch();
        connectingIntNodesDeliversOnlyOnTheNewLink();
        inspectSignals();
        arithmeticPrimitives();
        triggerTools();
        greenMathAndComparisons();
        genericComparisonsPublishBooleanResultsAndTriggers();
        genericComparisonsOrderArrayContents();
        genericTriggerComparisonsGateEveryIncomingPulse();
        genericTriggerComparisonsSampleTypedTextAndArrays();
        greenLogicOperatorsWorkWithBooleansAndIntBits();
        greenLogicRoutesValuesAndEvaluatesConditionTables();
        indexSelectorPublishesSortedAndProgrammaticSelections();
        pointToFloatSplitsCoordinatesAndForwardsPulses();
        selectorRoutesTheIndexedInput();
        multiplexerRoutesTheSelectedOutput();
        indexedRoutersForwardTriggerSignals();
        indexedStreamRoutersPruneAndRebuildTheLiveGraph();
        streamRoutersPreservePackedDoubleLanes();
        indexedRoutersPassBusesAndTheirNamedChannels();
        intTransitionRequiresConsecutiveMatchingValues();
        greenLogicDistinguishesTransitionsChangesAndSampling();
        genericMinMaxUseGreenNumericOperands();
        genericMinMaxPairArrayElements();
        genericMinMaxSpecializeToStreams();
        genericPowerUsesGreenOperands();
        genericPowerCombinesArraysInOperandOrder();
        genericPowerSpecializesToStreams();
        genericLogarithmUsesGreenOperands();
        genericLogarithmSpecializesToStreams();
        genericFixedLogarithmsPreserveGreenTypes();
        genericFixedLogarithmsPreserveStreamPrecision();
        greenTrigonometryUsesFloatConnectors();
        greenAtan2UsesBothQuadrantsAndNormalFanIn();
        greenHypotenuseSupportsFloatScalarsAndArrays();
        greenReciprocalAndSignPreserveTheirConnectorTypes();
        genericReciprocalAndSignUseTheExistingStreamOperators();
        generalProcessingConversionsUseTheirConnectorFamilies();
        generalProcessingStreamConversionsUseTheHostSampleRate();
        generalProcessingEvaluatesFormulasAndConvertsNoteNames();
        generalProcessingDispatchesRandomSlideAndGoalEvents();
        genericRoundingPreservesGreenConnectorTypes();
        genericRoundingPreservesDoubleStreamPrecision();
        greenArrayPrimitivesPreserveTypesValuesAndTriggers();
        arrayBuilderPortsFollowTopologyEdits();
        greenArrayTransformsSampleOnTrigger();
        greenArrayTransformsSupportNonIntegerArrays();
        indexedArrayStoresValuesOnActionTriggers();
        arrayHistoryKeepsLatestValues();
        arrayQueueAndStackExposePopOrder();
        arrayInterpolationAndResamplingRespectModesAndBoundaries();
        arrayLoopAndSplitterPreserveTypesAndIndices();
        arrayDrawInterpolatesBetweenDragSamples();
        arrayExtremaNormalizationAndCoordinateConversions();
        arrayReductionsAndSortUseGreenTypes();
        spectralArrayToolsPublishPairedResultsOnCalc();
        stringPrimitivesReadCurrentValuesAndForwardTriggers();
        stringArrayPrimitivesSplitJoinAndFind();
        stringConversionsPreserveCharacterCodesAndBytes();
        stringCharsetConvertsNamesAndCodePages();
        formatStringUsesOneNumericInputAndPropagatesTriggers();
        textLoadPublishesOnlyAfterReadingTheFile();
        textSaveWritesTheCurrentPathAndTextOnSave();
        systemFoldersUsesItsIntInputForTheCustomPath();
        systemFontsSharesOneSnapshotAcrossItsOutputs();
        memoryCreateReadWriteSectionAndCopy();
        memoryArraysConvertSplitAndFeedDsp();
        waveArraysConvertChannelsAndGenerateMipmaps();
        timeSamplesAndHoldsTheLocalCalendarFields();
        timerRepeatsAndReschedulesOnGreenTriggers();
        yellowPanelAndArea();
        areaPrimitivesConvertFloatAndPointGeometry();
        areaPrimitivesAlignAndFindUnion();
        areaPrimitivesTestRectangleAndPolygonHits();
        yellowShapesFollowViewWiring();
        yellowPieRectAndTextShareTheViewChain();
        yellowBitmapDrawingCropsTransformsAndKeepsViewOrder();
        yellowGraphsPlotLiveSamplesOnTheViewChain();
        yellowDotsUseExplicitCoordinatesAndAxisBounds();
        yellowPointLinesFollowArrayOrderAcrossVerticalSegments();
        yellowPolygonClosesAValidContour();
        dropListSelectionAndCancellationPulseDifferentOutputs();
        editControlUsesViewAndPublishesEdits();
        popupListCommitsTheIndexAndTextTogether();
        dropFilesPreviewsAndCommitsAcceptedBatches();
        mouseMessagesFilterEventsAndDeliverEachMatchingPulse();
        polygonalMouseAreaTestsTheContourInViewCoordinates();
        mouseInfoResolvesCursorNamesAndIds();
        mouseInfoFollowsItsSourceAndRefreshesWithoutPointerEvents();
        showCursorSamplesHoldOnlyWhenHiding();
        mouseDragPublishesCapturedPointsAndDragState();
        mouseDragAccumulateUsesClampedGestureBaselines();
        dragFilesRequestsAndPublishesNativeOutcomes();
        viewTransformsComposeInPanelCoordinates();
        viewRotateUsesALocalPivotAndSmoothOnlyPassesView();
        windowOwnsAnIndependentViewAndReportsNativeState();
        viewLoopDrawsEachStepInLayerOrder();
        viewCacheReusesDrawingUntilInvalidated();
        viewEffectsCaptureOnlyTheWiredDrawing();
        redrawAreaRepaintsOnlyTheMappedRegion();
        redrawInvalidatesOnlyTheCachesOnItsViewRoute();
        viewCacheWithinLoopKeepsSeparateIterations();
        viewArrayLoopRepeatsNestedDrawingInReverse();
        clipsCombineRectanglesWithoutChangingTheViewSize();
        setGuiAreaChangesThePanelOnTheSetTrigger();
        mouseMoveFollowsTheWiredViewAndItsMouseToggle();
        modulesOwnTheirSchematicsAndViewports();
        makeModulePreservesConversionsFanOutAndState();
        makeModuleHandlesPathsThatLeaveAndReenter();
        makeModulePortsReinferAfterRewiring();
        makeModuleKeepsUnresolvedPortsAsTemplate();
        makeModuleRoutesBusSignalsThroughNestedBoundaries();
        makeModuleCopiesWithItsInterface();
        propertyPortsReadOutputsAndSupplyUnwiredInputs();
        propertyPortsCarryTriggerOnlyConnections();
        manualPortsCarryValuesAndTriggers();
        templateModulePortsInferFromEitherSide();
        templateModulePortsCarryBusChannels();
        templateModuleOutputsFollowGenericSources();
        alphaTypesRouteThroughModuleEndpoints();
        midiSequencerReplaysRecordedEventsInTimeOrder();
        matrixSwitchesPolyRoutesWithoutResettingVoices();
        midiMessagesMergeAcrossModulesAndSplitAtomically();
        midiFiltersExtractAndBufferMessagesInOrder();
        midiStreamControlsHoldAndUpdateAudioParameters();
        midiCcStreamTracksEachVoiceChannel();
        notesFiltersRememberRoutingAndTagLayers();
        polyImpulseStartsWithEachNewVoice();
        floatArrayReadSharesLiveEditsAcrossPolyVoices();
        streamSelectRoutesPolyVoicesIndependently();
        midiVoicesRenderIndependentReleaseTails();
        envelopeControlTracksPolyphonicStages();
        polyMetadataAndMixersFollowVoiceGroups();
        voiceBankReturnsHeldStolenNotes();
        presetsRecallTypedProgramsAndRespectLock();
        keyDownPublishesOneCoherentRecordPerMatchingPress();
        keyUpTracksReleasesInNestedModulesAndHonorsTheFilter();
        isKeyPressedReadsHeldStateAndPulsesOnlyOnCheck();
        presentationControlsTrackTheFocusedPanelAndHostReports();
        gridConversionsRoundTripFloatsAndSignedAreas();
        moduleTooltipUsesItsLocalTitleAndParagraphs();
        redrawLimiterTracksTheClockAndHostSuspension();
        presetRecallSuspendsRedrawsDuringParameterChanges();
        presetTextCodecReadsReferenceTablesAndPreservesStrings();
        presetTextTransfersScopedProgramsAndRejectsBrokenTables();
        legacyPresetCodecReadsParameterProgramsAndBanks();
        legacyPresetPrimitivesTransferFilesAndRejectInvalidBanks();
        pluginModuleRoutesHostAudioAndReportsAuxChannels();
        startFolderUsesTheInstalledDirectoryAndEditorFallbacks();
        settingsReportsTheSelectedSystemValue();
        sfzParserPublishesAlignedRegionColumns();
        networkNodesRouteBytesAndLifecycleTriggers();
        streamSequencerRecordsAndReplaysMono4AcrossBlocks();
        mediaPrimitivesPublishFramesAndStereoWhileAudioIsPrepared();
        mouseCaptureDeliversOutsideEventsUntilStopped();
        httpRequestsPublishOnlyTheCurrentReply();
        remainingSystemActionsUseTheCurrentInputsOnTrigger();
        remainingDrawingPrimitivesPublishTypedResults();
        fontEmbedRestoresBytesInCopiedModules();
        stateMachineFollowsTheMapAndResets();
        stateMachinePrimesItsMapWhenOpeningASchematic();
        greenTransportPublishesChangedHostValues();
        greenTransportSurvivesSchematicReplacementAndUndo();
        hostTransportAndMonoOutFollowCallbackTiming();
        clearAudioRestartsTheRunningStream();
        waveWritingReadingAndFilesRoundTrip();
        waveReadHopAndWaveTableKeepTheirPhaseAcrossBuffers();
        audioDeviceControlsAndMultichannelEndpoints();
        monoToFramePublishesAlignedFrames();
        presetOrderAndBypassMatchHostParameters();
        pluginHostControlsAreAvailableThroughTheControlThread();
        hostMidiPortsRouteWithoutDeviceBackends();
        midiNotesRespectChannelsAndDeferredRelease();
        midiNoteEventSchedulesOriginalKeyAndSysexRoundTrips();
        midiDevicesRouteToSelectedHardware();
        comPortsRefreshTheListAcrossModules();
        comPortTransfersTextAndHexAndClearsPendingWrites();
        comPortCopiesAndUndoDoNotHoldDeletedHardware();
        indexedMidiDevicesHandleBusyPortsAndHotplug();
        manualPortOrderFollowsLayoutAndKeepsWires();
        manualPortsSynchronizeAndRejectIncompatibleTypes();
        moduleFrontPanelLayout();
        modulePropertiesUseAnIndependentEditorSurface();
        moduleActionsKeepReusablePanelsAndAnchorMargins();
        synchronizedModulesShareEditsAndKeepIndependentState();
        matchingModulesCanBeSynchronizedAcrossLevels();
        wirelessRoutingAcrossModules();
        moduleWirelessOutputsPublishAtTheModuleLevel();
        colorBoxesPreserveArgbAndTriggers();
        argbPrimitivesPreserveStraightColorBytes();
        ahsvPrimitivesUseDegreesAndIntegerComponents();
        colorMatrixWiringAndChannelOrder();
        paintToolsPreserveStrokesAndFills();
        yellowBrushesCarrySettingsThroughDrawings();
        fontAttributesWireAndTriggerIndependently();
        stringFormatRoundTrip();
        measureTextPullsInputsAndForwardsTriggers();
        bitmapLoadsOnDemandAndOwnsItsPixels();
        bitmapArrayLoadsAndReplacesIndexedImages();
        bitmapToolsReadAndStoreTriggeredSnapshots();
        bitmapGetAtCopiesTheSelectedSlotAndReportsCompletion();
        setPixelUpdatesLiveWiresAndPreservesImageSnapshots();
        bitmapCanvasesRenderThroughTheViewChain();
        svgRescalesTheLoadedDocumentAndSeparatesOutcomes();
        busRoutesValuesAndPulsesByName();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
