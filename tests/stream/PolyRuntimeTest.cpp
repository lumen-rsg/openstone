#include "stream/PolyRuntime.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

// Count allocations made by the normal C++ containers used in the render path.
namespace allocationCheck {
bool enabled = false;
std::size_t count = 0;
}

void* operator new(std::size_t size)
{
    if (allocationCheck::enabled) {
        ++allocationCheck::count;
    }
    if (auto* pointer = std::malloc(size == 0 ? 1 : size)) {
        return pointer;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

namespace {

using namespace openstone::stream;

void check(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void rejects(auto&& operation, std::string_view message)
{
    try {
        operation();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error(std::string(message));
}

struct FeedbackPatch {
    Graph graph;
    NodeId pulse = graph.addInput();
    NodeId velocity = graph.addInput();
    NodeId excite = graph.addMultiply();
    NodeId sum = graph.addSum();
    NodeId delay = graph.addDelay();
    NodeId gain = graph.addGain(0.5);
    std::array<NodeId, 3> taps{sum, delay, gain};
    std::array<PolyInput, 2> bindings{{{PolyInputKind::NoteOn}, {PolyInputKind::Velocity}}};

    FeedbackPatch()
    {
        graph.connect(pulse, excite);
        graph.connect(velocity, excite);
        graph.connect(excite, sum);
        graph.connect(gain, sum);
        graph.connect(sum, delay);
        graph.connect(delay, gain);
    }
};

constexpr std::size_t frames = 640;
using Samples = std::array<float, frames>;
using Response = std::array<Samples, 3>;

void monoPolyBridgesRouteByVoiceSlot()
{
    Graph graph;
    const auto velocity = graph.addInput();
    const auto mono = graph.addInput();
    const auto routed = graph.addInput();
    const auto scaled = graph.addMultiply();
    graph.connect(mono, scaled);
    graph.connect(velocity, scaled);
    constexpr std::array bindings{
        PolyInput{PolyInputKind::Velocity},
        PolyInput{PolyInputKind::Mono},
        PolyInput{.kind = PolyInputKind::Mono, .voiceIndex = 0},
    };
    const std::array taps{scaled, scaled, scaled, scaled, routed};
    PolyRuntime runtime(graph, bindings, taps, {.maxVoices = 3, .holdAfterNoteOff = true});
    check(runtime.setOutputVoiceIndex(1, 0) && runtime.setOutputVoiceIndex(2, 1) &&
          runtime.setOutputVoiceIndex(3, 2), "Could not select voice slots");
    check(!runtime.setMonoInputVoiceIndex(0, 0) && !runtime.setOutputVoiceIndex(taps.size(), 0),
          "Route controls must refer to Mono bindings and valid output taps");

    constexpr std::array input{2.0f, 4.0f, 6.0f, 8.0f};
    constexpr std::array<float, input.size()> silence{};
    std::array<std::array<float, input.size()>, taps.size()> output{};
    std::array<OutputBuffer, taps.size()> outputs;
    for (std::size_t i = 0; i < output.size(); ++i) {
        outputs[i] = output[i];
    }
    constexpr std::array events{
        PolyEvent{0, PolyEventKind::NoteOn, 1, 60, 0.5f},
        PolyEvent{0, PolyEventKind::NoteOn, 2, 60, 1.0f},
        PolyEvent{1, PolyEventKind::NoteOff, 1},
        PolyEvent{2, PolyEventKind::EndVoice, 1},
        PolyEvent{3, PolyEventKind::NoteOn, 3, 72, 0.25f},
    };
    check(runtime.process(std::array{InputBuffer{input}}, outputs, events), "Could not render voice routes");
    check(output[0] == std::array{3.0f, 6.0f, 6.0f, 10.0f}, "Unwired Index should broadcast and sum");
    check(output[1] == std::array{1.0f, 2.0f, 0.0f, 2.0f}, "Slot zero should include release and follow slot reuse");
    check(output[2] == input && output[3] == silence,
          "Poly to Mono should isolate the selected voice and silence empty slots");
    check(output[4] == std::array{2.0f, 4.0f, 0.0f, 8.0f}, "Mono to Poly should feed only the selected voice");

    check(runtime.setMonoInputVoiceIndex(1, 1) && runtime.setMonoInputVoiceIndex(2, std::nullopt) &&
          runtime.setOutputVoiceIndex(3, -1), "Could not change voice routing");
    check(runtime.process(std::array{InputBuffer{input}}, outputs), "Could not render updated voice routes");
    check(output[0] == input && output[1] == silence && output[2] == input &&
          output[3] == silence && output[4] == std::array{4.0f, 8.0f, 12.0f, 16.0f},
          "Index changes should take effect without restarting voices or changing other bindings");

    check(runtime.setMonoInputVoiceIndex(1, -1) && runtime.setOutputVoiceIndex(4, 100),
          "Could not set unmatched voice indices");
    check(runtime.process(std::array{InputBuffer{input}}, outputs) &&
          std::ranges::all_of(output, [&silence](const auto& channel) { return channel == silence; }) &&
          runtime.activeVoiceCount() == 2, "Unmatched slots should give silence without stopping voices");
}

void overlappingVoicesMatchIndependentMono()
{
    FeedbackPatch patch;
    // Eight voices deliberately cross the four-lane SIMD boundary. All have
    // the same pitch, proving that pitch does not alias note state.
    constexpr std::array<std::size_t, 8> starts{0, 1, 3, 7, 31, 63, 127, 257};
    constexpr std::array<float, 8> velocities{1, 0.5f, 0.25f, 0.75f, 0.125f, 0.625f, 0.875f, 1};
    std::vector<PolyEvent> events;
    Response expected{};
    for (std::size_t voice = 0; voice < starts.size(); ++voice) {
        const auto start = starts[voice];
        const auto end = 500 + voice * 7;
        const auto id = static_cast<NoteId>(voice + 1);
        events.push_back({start, PolyEventKind::NoteOn, id, 60, velocities[voice]});
        events.push_back({start + 8, PolyEventKind::NoteOff, id});
        events.push_back({end, PolyEventKind::EndVoice, id});

        auto mono = patch.graph.compile(patch.taps);
        Samples pulse{};
        Samples velocity{};
        pulse[start] = 1;
        velocity.fill(velocities[voice]);
        Response independent{};
        const std::array<InputBuffer, 2> inputs{
            std::span{pulse}.subspan(start, end - start),
            std::span{velocity}.subspan(start, end - start),
        };
        const std::array<OutputBuffer, 3> outputs{
            std::span{independent[0]}.subspan(start, end - start),
            std::span{independent[1]}.subspan(start, end - start),
            std::span{independent[2]}.subspan(start, end - start),
        };
        check(mono.process(inputs, outputs), "Independent Mono failed");
        check(independent[0][start] == velocities[voice] && independent[1][start] == 0,
              "Independent impulse must start with cleared delay state");
        for (std::size_t channel = 0; channel < expected.size(); ++channel) {
            for (std::size_t frame = 0; frame < frames; ++frame) {
                expected[channel][frame] += independent[channel][frame];
            }
        }
    }
    std::stable_sort(events.begin(), events.end(), [](const auto& a, const auto& b) { return a.frame < b.frame; });

    const auto render = [&](std::span<const std::size_t> blockSizes, std::span<const NodeId> taps) {
        PolyRuntime runtime(patch.graph, patch.bindings, taps, {8, 48000, true});
        Response result{};
        std::size_t cursor = 0;
        std::vector<PolyEvent> blockEvents;
        blockEvents.reserve(events.size());
        for (std::size_t frame = 0, block = 0; frame < frames; ++block) {
            const auto size = std::min(blockSizes[block % blockSizes.size()], frames - frame);
            blockEvents.clear();
            while (cursor < events.size() && events[cursor].frame < frame + size) {
                blockEvents.push_back(events[cursor++]);
                blockEvents.back().frame -= frame;
            }
            std::array<OutputBuffer, 3> outputs{
                std::span{result[0]}.subspan(frame, size),
                std::span{result[1]}.subspan(frame, size),
                std::span{result[2]}.subspan(frame, size),
            };
            allocationCheck::enabled = true;
            const bool rendered = runtime.process({}, std::span{outputs}.first(taps.size()), blockEvents);
            allocationCheck::enabled = false;
            check(rendered, "Poly feedback rejected valid event block");
            frame += size;
        }
        check(runtime.activeVoiceCount() == 0, "Explicit ends must retire all released voices");
        return result;
    };
    constexpr std::array<std::size_t, 8> blockSizes{1, 3, 7, 16, 31, 64, 257, frames};
    for (const auto size : blockSizes) {
        check(render(std::array{size}, patch.taps) == expected,
              "Poly mix differs sample-for-sample from eight independent Mono runs");
    }
    check(render(std::array<std::size_t, 6>{0, 3, 1, 17, 0, 64}, patch.taps) == expected,
          "Poly changed across variable buffers or zero-frame calls");
    for (std::size_t tap = 0; tap < patch.taps.size(); ++tap) {
        check(render(std::array<std::size_t, 1>{31}, std::span{patch.taps}.subspan(tap, 1))[0] == expected[tap],
              "Adding or removing an observer changed Poly feedback");
    }
    check(allocationCheck::count == 0, "Poly process allocated memory");
    check(expected[0][1] == 1, "Overlapping voices should sum without averaging");
}

void lifecycleAndControls()
{
    Graph graph;
    std::array<NodeId, 6> taps;
    for (auto& tap : taps) {
        tap = graph.addInput();
    }
    const std::array<PolyInput, 6> bindings{{
        {PolyInputKind::Gate}, {PolyInputKind::NoteOn}, {PolyInputKind::Stage},
        {PolyInputKind::Velocity}, {PolyInputKind::Pitch}, {PolyInputKind::Frequency},
    }};
    PolyRuntime runtime(graph, bindings, taps, {1, 880, true});
    std::array<std::array<float, 7>, 6> result{};
    std::array<OutputBuffer, 6> outputs;
    for (std::size_t channel = 0; channel < result.size(); ++channel) {
        outputs[channel] = result[channel];
    }
    const std::array events{
        PolyEvent{1, PolyEventKind::NoteOn, 1, 69, 0.25f},
        PolyEvent{2, PolyEventKind::EndVoice, 1}, // End cannot cut a key-held voice.
        PolyEvent{3, PolyEventKind::NoteOff, 1},
        PolyEvent{5, PolyEventKind::EndVoice, 1},
    };
    check(runtime.process({}, outputs, events), "Lifecycle process failed");
    check(result[0] == std::array<float, 7>{0, 1, 1, 0, 0, 0, 0}, "Wrong gate lifecycle");
    check(result[1] == std::array<float, 7>{0, 1, 0, 0, 0, 0, 0}, "NoteOn must pulse for one sample");
    check(result[2] == std::array<float, 7>{0, 1, 3, 4, 4, 0, 0}, "Wrong envelope stage sequence");
    for (std::size_t frame = 1; frame < 5; ++frame) {
        check(result[3][frame] == 0.25f && result[4][frame] == 69 && result[5][frame] == 1,
              "Velocity, pitch, or normalized Nyquist frequency changed during release");
    }
    check(runtime.voiceInfo(0).stage == VoiceStage::Off, "Voice not retired");

    PolyRuntime immediate(graph, bindings, taps, {1, 880, false});
    check(immediate.process({}, outputs, events), "Immediate note-off process failed");
    check(result[2] == std::array<float, 7>{0, 1, 3, 0, 0, 0, 0}, "Unheld note must end on note-off");
    runtime.reset();
    check(runtime.process({}, outputs, events), "Reset must restart note ID sequence");
}

void releaseEndComesFromEachVoiceGraph()
{
    Graph graph;
    const auto gate = graph.addInput();
    const auto stage = graph.addInput();
    const auto delayedGate = graph.addDelay();
    graph.connect(gate, delayedGate);
    const std::array<PolyInput, 2> bindings{{{PolyInputKind::Gate}, {PolyInputKind::Stage}}};
    // Using a delayed gate as End is deliberately simple: true while held is
    // ignored, then true on the release sample ends just that voice afterward.
    PolyRuntime runtime(graph, bindings, std::array{stage}, {2, 48000, true}, delayedGate);
    std::array<float, 7> result{};
    const std::array events{
        PolyEvent{0, PolyEventKind::NoteOn, 1},
        PolyEvent{1, PolyEventKind::NoteOn, 2},
        PolyEvent{3, PolyEventKind::NoteOff, 1},
        PolyEvent{5, PolyEventKind::NoteOff, 2},
    };
    check(runtime.process({}, std::array<OutputBuffer, 1>{result}, events), "Graph end process failed");
    check(result == std::array<float, 7>{1, 4, 6, 7, 3, 4, 0}, "End tap must retire only its own released voice");
    check(runtime.activeVoiceCount() == 0, "End tap left released voices active");
}

void stealingAndSlotReuseClearHistory()
{
    FeedbackPatch patch;
    PolyRuntime runtime(patch.graph, patch.bindings, patch.taps, {2, 48000, true});
    std::array<std::array<float, 8>, 3> result{};
    const std::array<OutputBuffer, 3> outputs{result[0], result[1], result[2]};
    const std::array events{
        PolyEvent{0, PolyEventKind::NoteOn, 1, 60, 1},
        PolyEvent{1, PolyEventKind::NoteOn, 2, 60, 0.5f},
        PolyEvent{2, PolyEventKind::NoteOn, 3, 60, 0.25f}, // Steal oldest slot (1).
        PolyEvent{3, PolyEventKind::NoteOff, 1},          // Stale events must not kill 3.
        PolyEvent{3, PolyEventKind::EndVoice, 1},
        PolyEvent{4, PolyEventKind::NoteOff, 2},
        PolyEvent{4, PolyEventKind::EndVoice, 2},
        PolyEvent{5, PolyEventKind::NoteOn, 4, 60, 1},    // Reuse freed slot (2).
        PolyEvent{6, PolyEventKind::AllSoundOff},
        PolyEvent{7, PolyEventKind::NoteOn, 5, 60, 0.5f},
    };
    allocationCheck::enabled = true;
    const auto rendered = runtime.process({}, outputs, events);
    allocationCheck::enabled = false;
    check(rendered, "Voice stealing process failed");
    check(result[0] == std::array<float, 8>{1, 1, 0.5f, 0.25f, 0.0625f, 1.03125f, 0, 0.5f},
          "FIFO stealing, stale event handling, or freed voice reset failed");
    check(result[1] == std::array<float, 8>{0, 1, 0.5f, 0.5f, 0.125f, 0.0625f, 0, 0},
          "New voice inherited another voice's delay history");
    check(runtime.activeVoiceCount() == 1 && runtime.voiceInfo(0).id == 5, "Wrong voice owner after reuse");
    check(allocationCheck::count == 0, "Voice allocation during render allocated heap memory");
}

void sharedMonoAndSameFrameOrdering()
{
    Graph graph;
    const auto mono = graph.addInput();
    const auto gate = graph.addInput();
    const auto multiply = graph.addMultiply();
    graph.connect(mono, multiply);
    graph.connect(gate, multiply);
    const std::array<PolyInput, 2> bindings{{{PolyInputKind::Mono, 0}, {PolyInputKind::Gate}}};
    PolyRuntime original(graph, bindings, std::array{multiply}, {2});
    auto runtime = std::move(original); // Preallocated buffer views must survive moves.
    std::array<float, 5> samples{2, 3, 4, 5, 6};
    const std::array events{
        PolyEvent{0, PolyEventKind::NoteOn, 1},
        PolyEvent{0, PolyEventKind::NoteOn, 2},
        PolyEvent{1, PolyEventKind::NoteOff, 1},
        PolyEvent{2, PolyEventKind::NoteOff, 2},
        PolyEvent{2, PolyEventKind::NoteOn, 3},
        PolyEvent{3, PolyEventKind::NoteOn, 4},
        PolyEvent{3, PolyEventKind::NoteOff, 4},
        PolyEvent{4, PolyEventKind::AllSoundOff},
    };
    check(runtime.process(std::array<InputBuffer, 1>{samples}, std::array<OutputBuffer, 1>{samples}, events),
          "Shared Mono in-place process failed");
    check(samples == std::array<float, 5>{4, 3, 4, 5, 0}, "In-place shared input or equal-frame event order failed");
}

void invalidBlocksAreAtomic()
{
    FeedbackPatch patch;
    PolyRuntime runtime(patch.graph, patch.bindings, patch.taps, {1, 48000, true});
    std::array<std::array<float, 2>, 3> result{};
    for (auto& channel : result) {
        channel.fill(42);
    }
    const auto untouched = result;
    const std::array<OutputBuffer, 3> outputs{result[0], result[1], result[2]};
    const auto invalid = [&](std::span<const PolyEvent> events) {
        check(!runtime.process({}, outputs, events), "Accepted invalid Poly events");
        check(result == untouched && runtime.activeVoiceCount() == 0, "Invalid block partially mutated output/state");
    };
    const auto on = PolyEvent{0, PolyEventKind::NoteOn, 1};
    invalid(std::array{on, PolyEvent{2, PolyEventKind::NoteOff, 1}});
    invalid(std::array{PolyEvent{1, PolyEventKind::NoteOn, 1}, PolyEvent{0, PolyEventKind::NoteOff, 1}});
    invalid(std::array{on, PolyEvent{1, PolyEventKind::NoteOn, 1}});
    invalid(std::array{on, PolyEvent{1, PolyEventKind::NoteOn, 2, 128}});
    invalid(std::array{on, PolyEvent{1, PolyEventKind::NoteOn, 2, 60, std::numeric_limits<float>::quiet_NaN()}});
    invalid(std::array{PolyEvent{0, PolyEventKind::NoteOn, 0}});
    invalid(std::array{PolyEvent{0, PolyEventKind::NoteOff, 0}});
    invalid(std::array{PolyEvent{0, static_cast<PolyEventKind>(99), 1}});
    check(!runtime.process({}, std::span{outputs}.first(2), std::array{on}), "Accepted wrong output count");
    const std::array<OutputBuffer, 3> wrongLengths{result[0], std::span{result[1]}.first(1), result[2]};
    check(!runtime.process({}, wrongLengths, std::array{on}), "Accepted unequal output buffers");
    const std::array<OutputBuffer, 3> empty{};
    check(!runtime.process({}, empty, std::array{on}), "Accepted events in an empty block");
    check(result == untouched && runtime.activeVoiceCount() == 0, "Shape error changed state or buffers");
    check(runtime.process({}, outputs, std::array{on}), "Rejected valid note after invalid blocks");
    check(result[0] == std::array<float, 2>{1, 0.5f}, "Rejected event consumed note ID or changed delay");
    check(runtime.process({}, empty), "Zero frame block rejected");
    const auto afterFirstBlock = result;
    check(!runtime.process({}, outputs, std::array{on}), "Reused NoteOn identity was accepted");
    check(result == afterFirstBlock && runtime.activeVoiceCount() == 1, "Rejected ID modified active voice");
    check(runtime.process({}, outputs), "Continuation failed");
    check(result[0] == std::array<float, 2>{0.25f, 0.125f}, "Empty or rejected block advanced voice state");
    runtime.reset();
    check(runtime.process({}, outputs, std::array{on}), "Reset didn't allow IDs to restart");
    check(result[0] == std::array<float, 2>{1, 0.5f}, "Reset didn't clear history");

    Graph monoGraph;
    const auto input = monoGraph.addInput();
    const std::array<PolyInput, 1> monoBinding{{{PolyInputKind::Mono, 0}}};
    PolyRuntime mono(monoGraph, monoBinding, std::array{input});
    check(!mono.process({}, std::span{outputs}.first(1)), "Accepted missing shared Mono input");
    check(!mono.process(std::array<InputBuffer, 1>{std::span{result[0]}.first(1)}, std::span{outputs}.first(1)),
          "Accepted wrong shared Mono input length");
}

struct MidiReleasePatch {
    Graph graph;
    const NodeId gate = graph.addInput();
    const NodeId velocity = graph.addInput();
    const NodeId end = graph.addInput();
    const std::array<PolyInput, 3> bindings{{{PolyInputKind::Gate}, {PolyInputKind::Velocity},
                                            {PolyInputKind::Mono, 0}}};
    const std::array<NodeId, 1> taps{graph.addSum()};

    MidiReleasePatch()
    {
        const auto held = graph.addMultiply();
        graph.connect(gate, held);
        graph.connect(velocity, held);

        // The previous two samples give each voice a short, deterministic
        // release tail after its gate drops.
        const auto previous = graph.addDelay();
        const auto older = graph.addDelay();
        graph.connect(held, previous);
        graph.connect(previous, older);
        for (const auto [source, weight] : std::array{
                 std::pair{held, 0.5}, std::pair{previous, 0.25}, std::pair{older, 0.25}}) {
            const auto gain = graph.addGain(weight);
            graph.connect(source, gain);
            graph.connect(gain, taps.front());
        }
    }
};

void midiNotesReleaseByChannelAndPitch()
{
    const MidiReleasePatch patch;
    constexpr float b = 64.0f / 127.0f;

    struct TestCase {
        std::uint8_t bStatus;
        std::uint8_t bPitch;
    };
    for (const auto [bStatus, bPitch] : std::array{
             TestCase{0x90, 60}, TestCase{0x91, 60}, TestCase{0x90, 61}}) {
        PolyRuntime runtime(patch.graph, patch.bindings, patch.taps, {2, 48000, true}, patch.end);
        std::array<float, 14> signal{};
        std::array<float, 14> endRelease{};
        endRelease[8] = 1;
        endRelease[12] = 1;
        const auto render = [&](std::size_t start, std::size_t count, const auto& events) {
            const std::array<InputBuffer, 1> inputs{std::span{endRelease}.subspan(start, count)};
            const std::array<OutputBuffer, 1> outputs{std::span{signal}.subspan(start, count)};
            allocationCheck::enabled = true;
            const bool ok = runtime.processMidi(inputs, outputs, events);
            allocationCheck::enabled = false;
            check(ok, "MIDI render rejected a valid note block");
        };
        render(0, 6, std::array{MidiNoteEvent{0, 0x90, 60, 127}, MidiNoteEvent{3, bStatus, bPitch, 64}});
        check(runtime.activeVoiceCount() == 2 && runtime.voiceInfo(0).id != runtime.voiceInfo(1).id,
              "Overlapping MIDI Note Ons must create distinct voices");
        render(6, 3, std::array{MidiNoteEvent{0, 0x80, 60, 42}});
        check(runtime.voiceInfo(0).stage == VoiceStage::Off && runtime.voiceInfo(1).stage == VoiceStage::Execute,
              "First Note Off must retire A and leave B held, even on the same channel and pitch");
        render(9, 5, std::array{MidiNoteEvent{1, bStatus, bPitch, 0}});
        check(runtime.activeVoiceCount() == 0, "A zero-velocity Note On must finish the remaining note");

        const std::array expected{
            0.5f, 0.75f, 1.0f, 1 + 0.5f * b, 1 + 0.75f * b, 1 + b,
            b + 0.5f, b + 0.25f, b, b, 0.5f * b, 0.25f * b, 0.0f, 0.0f,
        };
        for (std::size_t frame = 0; frame < expected.size(); ++frame) {
            check(std::abs(signal[frame] - expected[frame]) < 0.000001f,
                  "MIDI mix did not follow the expected held levels and release tails");
        }
    }
    check(allocationCheck::count == 0, "MIDI process allocated memory");
}

void midiSustainReleasesOnlyKeyUpVoicesOnItsChannel()
{
    const MidiReleasePatch patch;
    PolyRuntime runtime(patch.graph, patch.bindings, patch.taps, {3, 48000, true}, patch.end);
    std::array<float, 17> signal{};
    std::array<float, 17> endRelease{};
    for (const auto frame : {3, 9, 13, 16}) {
        endRelease[frame] = 1;
    }
    const auto render = [&](std::size_t start, std::size_t count, const auto& events) {
        const std::array<InputBuffer, 1> inputs{std::span{endRelease}.subspan(start, count)};
        const std::array<OutputBuffer, 1> outputs{std::span{signal}.subspan(start, count)};
        allocationCheck::enabled = true;
        const bool ok = runtime.processMidi(inputs, outputs, events);
        allocationCheck::enabled = false;
        check(ok, "MIDI render rejected a valid sustain block");
    };

    // B overlaps A on the same channel and pitch; C shares their pitch
    // on a different channel. A and C receive Note Off while sustained.
    render(0, 6, std::array{MidiNoteEvent{0, 0x90, 60, 127}, MidiNoteEvent{0, 0x91, 60, 32},
                            MidiNoteEvent{2, 0x90, 60, 64},
                            MidiNoteEvent{3, 0xb0, 64, 64}, MidiNoteEvent{3, 0xb1, 64, 127},
                            MidiNoteEvent{3, 0x80, 60, 0}, MidiNoteEvent{3, 0x91, 60, 0}});
    check(runtime.voiceInfo(0).stage == VoiceStage::Execute && runtime.voiceInfo(1).stage == VoiceStage::Execute
              && runtime.voiceInfo(2).stage == VoiceStage::Execute,
          "Note Off changed voice stage while sustain was down");
    render(6, 4, std::array{MidiNoteEvent{1, 0xb0, 64, 63}});
    check(runtime.voiceInfo(0).stage == VoiceStage::Off && runtime.voiceInfo(1).stage == VoiceStage::Execute
              && runtime.voiceInfo(2).stage == VoiceStage::Execute,
          "Channel 1 pedal-up must release only A and keep C sustained and B held");
    render(10, 4, std::array{MidiNoteEvent{1, 0xb1, 64, 0}});
    check(runtime.voiceInfo(1).stage == VoiceStage::Off && runtime.voiceInfo(2).stage == VoiceStage::Execute,
          "Channel 2 pedal-up must release C and keep B held");
    render(14, 3, std::array{MidiNoteEvent{0, 0x80, 60, 0}});
    check(runtime.activeVoiceCount() == 0, "B must release when its own Note Off arrives");

    constexpr float b = 64.0f / 127.0f;
    constexpr float c = 32.0f / 127.0f;
    constexpr std::array expected{
        0.5f * (1 + c), 0.75f * (1 + c), 1 + c + 0.5f * b, 1 + c + 0.75f * b, 1 + b + c, 1 + b + c,
        1 + b + c, 0.5f + b + c, 0.25f + b + c, b + c,
        b + c, b + 0.5f * c, b + 0.25f * c, b, 0.5f * b, 0.25f * b, 0.0f,
    };
    for (std::size_t frame = 0; frame < expected.size(); ++frame) {
        check(std::abs(signal[frame] - expected[frame]) < 0.000001f,
              "Sustain changed the mix before the expected release frame or produced the wrong tail");
    }
    check(allocationCheck::count == 0, "MIDI sustain render allocated memory");
}

void midiSustainClearsOnVoiceReuseAndReset()
{
    Graph graph;
    const auto gate = graph.addInput();
    const std::array bindings{PolyInput{PolyInputKind::Gate}};
    PolyRuntime runtime(graph, bindings, std::array{gate}, {1});
    std::array<float, 3> signal{};
    const std::array<OutputBuffer, 1> outputs{signal};
    constexpr MidiNoteEvent pedalDown{0, 0xbf, 64, 127};
    constexpr MidiNoteEvent on{0, 0x9f, 60, 127};
    constexpr MidiNoteEvent off{1, 0x8f, 60, 0};

    check(runtime.processMidi({}, outputs, std::array{pedalDown, on, off})
              && signal == std::array{1.0f, 1.0f, 1.0f},
          "Sustain did not defer immediate stop in a patch without a release tail");
    check(runtime.processMidi({}, outputs, std::array{on, MidiNoteEvent{1, 0xbf, 64, 0}})
              && signal == std::array{1.0f, 1.0f, 1.0f} && runtime.voiceInfo(0).id == 2,
          "A voice that stole a sustained slot was released by the previous owner's pedal-up");
    check(runtime.processMidi({}, outputs, std::array{pedalDown, off}), "Could not sustain the replacement voice");
    runtime.reset();
    check(runtime.processMidi({}, outputs, std::array{on, off}) && signal == std::array{1.0f, 0.0f, 0.0f},
          "Reset must restore pedal-up state");
}

void midiNoteOffConsumesStolenInstanceBeforeNewVoice()
{
    Graph graph;
    const auto gate = graph.addInput();
    const std::array bindings{PolyInput{PolyInputKind::Gate}};
    PolyRuntime runtime(graph, bindings, std::array{gate}, {1});
    std::array<float, 3> signal{};
    const std::array<OutputBuffer, 1> outputs{signal};
    constexpr MidiNoteEvent on{0, 0x90, 60, 127};
    constexpr MidiNoteEvent off{0, 0x80, 60, 0};

    check(runtime.processMidi({}, outputs, std::array{on, MidiNoteEvent{1, 0x90, 60, 64}})
              && runtime.voiceInfo(0).id == 2,
          "Second Note On must steal the first voice at capacity");
    check(runtime.processMidi({}, outputs, std::array{off, MidiNoteEvent{2, 0x90, 60, 0}})
              && signal == std::array{1.0f, 1.0f, 0.0f},
          "First Note Off must consume the stolen note; only the second releases the new voice");

    check(runtime.processMidi({}, outputs, std::array{on}), "Could not start a new note before reset");
    runtime.reset();
    check(runtime.processMidi({}, outputs, std::array{on, MidiNoteEvent{1, 0x80, 60, 0}})
              && signal == std::array{1.0f, 0.0f, 0.0f},
          "Reset must clear outstanding Note Ons");
}

void midiAllNotesOffRespectsSustain()
{
    const MidiReleasePatch patch;
    PolyRuntime runtime(patch.graph, patch.bindings, patch.taps, {5, 48000, true}, patch.end);
    std::array<float, 18> signal{};
    std::array<float, 18> endRelease{};
    for (const auto frame : {11, 14, 17}) {
        endRelease[frame] = 1;
    }
    const std::array<InputBuffer, 1> inputs{endRelease};
    const std::array<OutputBuffer, 1> outputs{signal};
    constexpr std::array events{
        MidiNoteEvent{0, 0x90, 60, 127}, MidiNoteEvent{0, 0x90, 60, 127},
        MidiNoteEvent{0, 0x90, 61, 127}, MidiNoteEvent{0, 0x91, 60, 127},
        MidiNoteEvent{1, 0xb0, 64, 127},
        MidiNoteEvent{3, 0x80, 60, 0},  // One voice already sustained.
        MidiNoteEvent{4, 0xb0, 123, 0}, // Defer remaining channel 1 notes.
        MidiNoteEvent{6, 0x90, 60, 127}, // A new note stays held at pedal-up.
        MidiNoteEvent{9, 0xb0, 64, 0},
        MidiNoteEvent{12, 0x80, 60, 0},  // Matches the new note after CC123.
        MidiNoteEvent{15, 0xb1, 123, 0}, // Pedal up on channel 2.
    };

    allocationCheck::enabled = true;
    const bool ok = runtime.processMidi(inputs, outputs, events);
    allocationCheck::enabled = false;
    check(ok, "MIDI rejected a valid All Notes Off block");
    check(allocationCheck::count == 0, "All Notes Off allocated memory");

    constexpr std::array expected{
        2.0f, 3.0f, 4.0f, 4.0f, 4.0f, 4.0f, // CC123 keeps sustained voices sounding.
        4.5f, 4.75f, 5.0f, 3.5f, 2.75f, 2.0f, // Pedal-up releases three voices.
        1.5f, 1.25f, 1.0f, 0.5f, 0.25f, 0.0f, // New note and channel 2 then release.
    };
    check(signal == expected && runtime.activeVoiceCount() == 0,
          "All Notes Off must release only the addressed notes when their pedal is up");
}

void midiAllSoundOffCutsOnlyItsChannel()
{
    const MidiReleasePatch patch;
    PolyRuntime runtime(patch.graph, patch.bindings, patch.taps, {4, 48000, true}, patch.end);
    std::array<float, 12> signal{};
    std::array<float, 12> endRelease{};
    endRelease[11] = 1;
    const std::array<InputBuffer, 1> inputs{endRelease};
    const std::array<OutputBuffer, 1> outputs{signal};
    constexpr std::array events{
        MidiNoteEvent{0, 0x9f, 60, 127}, MidiNoteEvent{0, 0x9f, 61, 127},
        MidiNoteEvent{0, 0x9f, 62, 127}, MidiNoteEvent{0, 0x90, 60, 127},
        MidiNoteEvent{3, 0x8f, 61, 0}, // Starts a release tail before the pedal.
        MidiNoteEvent{4, 0xbf, 64, 127}, MidiNoteEvent{4, 0x8f, 62, 0},
        MidiNoteEvent{4, 0xbf, 120, 0}, // Cuts held, sustained, and releasing voices.
        MidiNoteEvent{6, 0x9f, 60, 127},
        MidiNoteEvent{7, 0x8f, 60, 0}, // Matches the silenced note.
        MidiNoteEvent{8, 0x8f, 60, 0}, // The new note sustains until pedal-up.
        MidiNoteEvent{9, 0xbf, 64, 0},
    };

    allocationCheck::enabled = true;
    const bool ok = runtime.processMidi(inputs, outputs, events);
    allocationCheck::enabled = false;
    check(ok, "MIDI rejected a valid All Sound Off block");
    check(allocationCheck::count == 0, "All Sound Off allocated memory");
    constexpr std::array expected{2.0f, 3.0f, 4.0f, 3.5f, 1.0f, 1.0f, 1.5f, 1.75f, 2.0f, 1.5f, 1.25f, 1.0f};
    check(signal == expected && runtime.activeVoiceCount() == 1 && runtime.voiceInfo(3).id == 4,
          "All Sound Off must cut its channel immediately and preserve the other voice and pedal state");
}

void midiResetAllControllersRestoresSustainOnItsChannel()
{
    const MidiReleasePatch patch;
    PolyRuntime runtime(patch.graph, patch.bindings, patch.taps, {3, 48000, true}, patch.end);
    std::array<float, 15> signal{};
    std::array<float, 15> endRelease{};
    for (const auto frame : {7, 11, 14}) {
        endRelease[frame] = 1;
    }
    const std::array<InputBuffer, 1> inputs{endRelease};
    const std::array<OutputBuffer, 1> outputs{signal};
    constexpr std::array events{
        MidiNoteEvent{0, 0x90, 60, 127}, MidiNoteEvent{0, 0x90, 60, 64},
        MidiNoteEvent{0, 0x91, 60, 32},
        MidiNoteEvent{1, 0xb0, 64, 127}, MidiNoteEvent{1, 0xb1, 64, 127},
        MidiNoteEvent{3, 0x80, 60, 0}, MidiNoteEvent{3, 0x81, 60, 0},
        MidiNoteEvent{5, 0xb0, 121, 0}, // Releases the first channel 1 note.
        MidiNoteEvent{8, 0xb0, 121, 0},
        MidiNoteEvent{9, 0x80, 60, 0},  // The second note can now release without pedal-up.
        MidiNoteEvent{12, 0xb1, 64, 0}, // Channel 2 was still sustained.
    };

    allocationCheck::enabled = true;
    const bool ok = runtime.processMidi(inputs, outputs, events);
    allocationCheck::enabled = false;
    check(ok, "MIDI rejected a valid Reset All Controllers block");
    check(allocationCheck::count == 0, "Reset All Controllers allocated memory");

    constexpr float held = 64.0f / 127.0f;
    constexpr float otherChannel = 32.0f / 127.0f;
    constexpr float all = 1 + held + otherChannel;
    constexpr float remaining = held + otherChannel;
    constexpr std::array expected{
        0.5f * all, 0.75f * all, all, all, all,
        0.5f + remaining, 0.25f + remaining, remaining, remaining,
        0.5f * held + otherChannel, 0.25f * held + otherChannel, otherChannel,
        0.5f * otherChannel, 0.25f * otherChannel, 0.0f,
    };
    for (std::size_t frame = 0; frame < expected.size(); ++frame) {
        check(std::abs(signal[frame] - expected[frame]) < 0.000001f,
              "Reset All Controllers must release only its channel's sustained notes with keys up");
    }
    check(runtime.activeVoiceCount() == 0, "Reset All Controllers left an unretired voice");
}

void midiPitchBendChangesOnlyItsChannel()
{
    Graph graph;
    const auto frequency = graph.addInput();
    const auto pitch = graph.addInput();
    const std::array taps{frequency, pitch};
    const std::array bindings{PolyInput{PolyInputKind::Frequency}, PolyInput{PolyInputKind::Pitch}};
    PolyRuntime runtime(graph, bindings, taps, {2, 880});
    std::array<float, 9> frequencies{};
    std::array<float, 9> pitches{};
    const std::array<OutputBuffer, 2> outputs{frequencies, pitches};
    constexpr std::array events{
        MidiNoteEvent{0, 0x90, 69, 127}, MidiNoteEvent{0, 0x9f, 81, 127},
        MidiNoteEvent{1, 0xe0, 127, 127}, // Channel 1 full up.
        MidiNoteEvent{2, 0xef, 0, 0},     // Channel 16 full down.
        MidiNoteEvent{3, 0xe0, 1, 64},    // One step above center uses the LSB.
        MidiNoteEvent{4, 0xb0, 121, 0},   // Centers channel 1 while both notes are held.
        MidiNoteEvent{5, 0x80, 69, 0},
        MidiNoteEvent{6, 0x9f, 69, 127}, // New note starts with channel 16's bend.
        MidiNoteEvent{7, 0xef, 0, 64},   // Center via the wheel message.
        MidiNoteEvent{8, 0xef, 0, 0},
    };
    allocationCheck::enabled = true;
    const bool ok = runtime.processMidi({}, outputs, events);
    allocationCheck::enabled = false;
    check(ok, "MIDI rejected a valid Pitch Bend block");
    check(allocationCheck::count == 0, "Pitch Bend allocated memory");

    // At 880 Hz sample rate, MIDI keys 69 and 81 are 1 and 2 in Nyquist units.
    const double up = std::exp2(2.0 / 12.0);
    const double down = 2.0 * std::exp2(-2.0 / 12.0);
    const double oneStep = std::exp2(2.0 / (8191.0 * 12.0));
    const std::array expected{
        3.0, up + 2, up + down, oneStep + down, 1 + down, down, 1.5 * down, 3.0, 1.5 * down,
    };
    for (std::size_t frame = 0; frame < expected.size(); ++frame) {
        check(std::abs(frequencies[frame] - expected[frame]) < 0.000001,
              "Pitch Bend or CC121 changed the wrong channel or sample");
    }
    check(pitches == std::array{150.0f, 150.0f, 150.0f, 150.0f, 150.0f, 81.0f, 150.0f, 150.0f, 150.0f},
          "Pitch Bend changed the MIDI key number or Note Off matching");

    runtime.reset();
    check(runtime.processMidi({}, outputs, std::array{MidiNoteEvent{0, 0x9f, 81, 127}})
              && std::ranges::all_of(frequencies, [](float f) { return f == 2.0f; }),
          "Runtime reset must center the bend for subsequent notes");
}

void midiPitchBendSensitivityIsPerChannelAndSurvivesControllerReset()
{
    Graph graph;
    const auto frequency = graph.addInput();
    const std::array bindings{PolyInput{PolyInputKind::Frequency}};
    PolyRuntime runtime(graph, bindings, std::array{frequency}, {2, 880});
    std::array<float, 12> signal{};
    const std::array<OutputBuffer, 1> outputs{signal};
    constexpr std::array events{
        MidiNoteEvent{0, 0x90, 69, 127}, MidiNoteEvent{0, 0x91, 81, 127},
        MidiNoteEvent{0, 0xe0, 127, 127}, MidiNoteEvent{0, 0xe1, 127, 127},
        MidiNoteEvent{1, 0xb0, 101, 0}, MidiNoteEvent{1, 0xb0, 6, 3}, // Incomplete selection.
        MidiNoteEvent{2, 0xb1, 100, 0}, MidiNoteEvent{2, 0xb1, 6, 9},
        MidiNoteEvent{3, 0xb0, 100, 0}, MidiNoteEvent{3, 0xb0, 6, 12},
        MidiNoteEvent{4, 0xb0, 38, 50}, // Twelve semitones and 50 cents.
        MidiNoteEvent{5, 0xb0, 121, 0},
        MidiNoteEvent{6, 0xb0, 6, 7}, MidiNoteEvent{6, 0xe0, 127, 127}, // Reapply bend after reset.
        MidiNoteEvent{7, 0xb1, 101, 0}, MidiNoteEvent{7, 0xb1, 6, 0},  // A zero range disables bend.
        MidiNoteEvent{8, 0xb0, 101, 0}, MidiNoteEvent{8, 0xb0, 100, 0}, MidiNoteEvent{8, 0xb0, 38, 0},
        MidiNoteEvent{9, 0xb0, 101, 127}, MidiNoteEvent{9, 0xb0, 100, 127}, MidiNoteEvent{9, 0xb0, 6, 3},
        MidiNoteEvent{10, 0xb0, 101, 0}, MidiNoteEvent{10, 0xb0, 100, 0},
        MidiNoteEvent{10, 0xb0, 99, 0}, MidiNoteEvent{10, 0xb0, 98, 0}, MidiNoteEvent{10, 0xb0, 6, 3},
        MidiNoteEvent{11, 0xb0, 101, 0}, MidiNoteEvent{11, 0xb0, 100, 3}, MidiNoteEvent{11, 0xb0, 6, 3},
    };

    allocationCheck::enabled = true;
    const bool ok = runtime.processMidi({}, outputs, events);
    allocationCheck::enabled = false;
    check(ok, "MIDI rejected a valid Pitch Bend Sensitivity block");
    check(allocationCheck::count == 0, "Pitch Bend Sensitivity allocated memory");

    const double defaultRatio = std::exp2(2.0 / 12.0);
    const double octaveAnd50Cents = 2.0 * std::exp2(0.5 / 12.0);
    const std::array expected{
        3 * defaultRatio, 3 * defaultRatio, 3 * defaultRatio,
        2 + 2 * defaultRatio, octaveAnd50Cents + 2 * defaultRatio,
        1 + 2 * defaultRatio, octaveAnd50Cents + 2 * defaultRatio,
        octaveAnd50Cents + 2, 4.0, 4.0, 4.0, 4.0,
    };
    for (std::size_t frame = 0; frame < expected.size(); ++frame) {
        check(std::abs(signal[frame] - expected[frame]) < 0.000001,
              "Pitch Bend Sensitivity changed the wrong channel, range, or sample");
    }
}

void midiChannelFineTuningUsesBothDataBytes()
{
    Graph graph;
    const auto frequency = graph.addInput();
    const std::array bindings{PolyInput{PolyInputKind::Frequency}};
    PolyRuntime runtime(graph, bindings, std::array{frequency}, {2, 880});
    std::array<float, 8> signal{};
    const std::array<OutputBuffer, 1> outputs{signal};
    constexpr std::array events{
        MidiNoteEvent{0, 0x90, 69, 127}, MidiNoteEvent{0, 0x91, 81, 127},
        MidiNoteEvent{1, 0xb0, 101, 0}, MidiNoteEvent{1, 0xb0, 100, 1}, MidiNoteEvent{1, 0xb0, 6, 96},
        MidiNoteEvent{2, 0xb0, 38, 64},
        MidiNoteEvent{3, 0xe0, 0, 0}, MidiNoteEvent{3, 0xe1, 127, 127},
        MidiNoteEvent{4, 0xb0, 121, 0},
        MidiNoteEvent{5, 0xb0, 6, 64}, MidiNoteEvent{5, 0xb0, 38, 0}, // CC121 cleared selection.
        MidiNoteEvent{6, 0xb1, 101, 0}, MidiNoteEvent{6, 0xb1, 100, 1}, MidiNoteEvent{6, 0xb1, 6, 0},
        MidiNoteEvent{7, 0xb0, 101, 0}, MidiNoteEvent{7, 0xb0, 100, 1},
        MidiNoteEvent{7, 0xb0, 38, 127}, MidiNoteEvent{7, 0xb0, 6, 127},
    };

    allocationCheck::enabled = true;
    const bool ok = runtime.processMidi({}, outputs, events);
    allocationCheck::enabled = false;
    check(ok, "MIDI rejected a valid Channel Fine Tuning block");
    check(allocationCheck::count == 0, "Channel Fine Tuning allocated memory");

    const auto ratio = [](double semitones) { return std::exp2(semitones / 12.0); };
    constexpr double fine = 0.5 + 64.0 / 8192.0;
    const double channel2Bent = 2 * ratio(2);
    const double channel2BentAndTuned = 2 * ratio(1); // Full bend (+2) and minimum fine tuning (-1).
    const std::array expected{
        3.0,
        ratio(0.5) + 2,
        ratio(fine) + 2,
        ratio(fine - 2) + channel2Bent,
        ratio(fine) + channel2Bent,
        ratio(fine) + channel2Bent,
        ratio(fine) + channel2BentAndTuned,
        ratio(8191.0 / 8192.0) + channel2BentAndTuned,
    };
    for (std::size_t frame = 0; frame < expected.size(); ++frame) {
        check(std::abs(signal[frame] - expected[frame]) < 0.000001,
              "Channel Fine Tuning changed the wrong channel, frequency, or sample");
    }
}

void midiChannelCoarseTuningCombinesWithFineAndBend()
{
    Graph graph;
    const auto frequency = graph.addInput();
    const std::array bindings{PolyInput{PolyInputKind::Frequency}};
    PolyRuntime runtime(graph, bindings, std::array{frequency}, {2, 880});
    std::array<float, 11> signal{};
    const std::array<OutputBuffer, 1> outputs{signal};
    constexpr std::array events{
        MidiNoteEvent{0, 0x90, 69, 127}, MidiNoteEvent{0, 0x91, 81, 127},
        MidiNoteEvent{1, 0xb0, 101, 0}, MidiNoteEvent{1, 0xb0, 100, 1}, MidiNoteEvent{1, 0xb0, 6, 32},
        MidiNoteEvent{2, 0xb0, 100, 2}, MidiNoteEvent{2, 0xb0, 6, 76},
        MidiNoteEvent{3, 0xb0, 38, 127}, // Coarse tuning uses only the MSB.
        MidiNoteEvent{4, 0xe0, 127, 127}, MidiNoteEvent{4, 0xe1, 0, 0},
        MidiNoteEvent{5, 0xb0, 121, 0},
        MidiNoteEvent{6, 0xb1, 101, 0}, MidiNoteEvent{6, 0xb1, 100, 2}, MidiNoteEvent{6, 0xb1, 6, 52},
        MidiNoteEvent{7, 0xb1, 121, 0},
        MidiNoteEvent{8, 0x80, 69, 0},
        MidiNoteEvent{9, 0x81, 81, 0},
        MidiNoteEvent{10, 0x90, 69, 127},
    };

    allocationCheck::enabled = true;
    const bool ok = runtime.processMidi({}, outputs, events);
    allocationCheck::enabled = false;
    check(ok, "MIDI rejected a valid Channel Coarse Tuning block");
    check(allocationCheck::count == 0, "Channel Coarse Tuning allocated memory");

    const auto ratio = [](double semitones) { return std::exp2(semitones / 12.0); };
    const double channel1Tuned = ratio(12 - 0.5);
    const double channel2Bent = 2 * ratio(-2);
    const std::array expected{
        3.0,
        ratio(-0.5) + 2,
        channel1Tuned + 2,
        channel1Tuned + 2,
        ratio(12 - 0.5 + 2) + channel2Bent,
        channel1Tuned + channel2Bent,
        channel1Tuned + 2 * ratio(-12 - 2),
        channel1Tuned + 1,
        1.0,
        0.0,
        channel1Tuned,
    };
    for (std::size_t frame = 0; frame < expected.size(); ++frame) {
        check(std::abs(signal[frame] - expected[frame]) < 0.000001,
              "Channel Coarse Tuning changed the wrong channel, frequency, or sample");
    }

    runtime.reset();
    check(runtime.processMidi({}, outputs, std::array{events[0], events[1]})
              && std::ranges::all_of(signal, [](float value) { return value == 3.0f; }),
          "Runtime reset must restore default tuning for subsequent notes");
}

void midiContinuousControllersDriveOnlyTheirChannel()
{
    Graph graph;
    const std::array taps{graph.addInput(), graph.addInput(), graph.addInput(), graph.addInput()};
    const std::array bindings{
        PolyInput{PolyInputKind::Modulation}, PolyInput{PolyInputKind::ChannelVolume},
        PolyInput{PolyInputKind::Pan}, PolyInput{PolyInputKind::Expression},
    };
    PolyRuntime runtime(graph, bindings, taps, {2});
    std::array<std::array<float, 4>, 4> samples{};
    const std::array<OutputBuffer, 4> outputs{samples[0], samples[1], samples[2], samples[3]};
    constexpr MidiNoteEvent firstNote{0, 0x90, 60, 127};
    constexpr std::array firstBlock{
        firstNote,
        MidiNoteEvent{1, 0xb0, 1, 127}, MidiNoteEvent{1, 0xb0, 7, 64},
        MidiNoteEvent{1, 0xb0, 10, 0}, MidiNoteEvent{1, 0xb0, 11, 32},
        // Set channel 16's controls before starting its voice.
        MidiNoteEvent{2, 0xbf, 1, 32}, MidiNoteEvent{2, 0xbf, 7, 96},
        MidiNoteEvent{2, 0xbf, 10, 127}, MidiNoteEvent{2, 0xbf, 11, 64},
        MidiNoteEvent{3, 0x9f, 60, 127},
    };
    constexpr std::array secondBlock{
        MidiNoteEvent{0, 0xb0, 121, 0},
        MidiNoteEvent{1, 0x80, 60, 0},
        MidiNoteEvent{2, 0xbf, 121, 0},
        MidiNoteEvent{3, 0xbf, 10, 64}, MidiNoteEvent{3, 0xbf, 7, 0}, MidiNoteEvent{3, 0xbf, 11, 0},
    };
    constexpr std::array midiDefaults{0.0f, 100.0f / 127, 0.0f, 1.0f};
    const auto checkFrame = [&](std::size_t frame, const std::array<float, 4>& expected) {
        for (std::size_t control = 0; control < expected.size(); ++control) {
            check(std::abs(samples[control][frame] - expected[control]) < 0.000001f,
                  "Continuous MIDI control changed the wrong channel, value, or sample");
        }
    };

    allocationCheck::enabled = true;
    const bool firstOk = runtime.processMidi({}, outputs, firstBlock);
    allocationCheck::enabled = false;
    check(firstOk, "MIDI rejected valid continuous controllers");
    checkFrame(0, midiDefaults);
    checkFrame(1, {1, 64.0f / 127, -1, 32.0f / 127});
    checkFrame(2, {1, 64.0f / 127, -1, 32.0f / 127});
    checkFrame(3, {1 + 32.0f / 127, 160.0f / 127, 0, 96.0f / 127});

    allocationCheck::enabled = true;
    const bool secondOk = runtime.processMidi({}, outputs, secondBlock);
    allocationCheck::enabled = false;
    check(secondOk, "MIDI rejected valid controller resets");
    check(allocationCheck::count == 0, "Continuous MIDI controllers allocated memory");
    checkFrame(0, {32.0f / 127, 160.0f / 127, 0, 1 + 64.0f / 127});
    checkFrame(1, {32.0f / 127, 96.0f / 127, 1, 64.0f / 127});
    checkFrame(2, {0, 96.0f / 127, 1, 1});
    checkFrame(3, {0, 0, 0, 0});

    // A direct Poly event has no MIDI channel. Channel 16 still contributes
    // the zero values from the last frame above.
    check(runtime.process({}, outputs, std::array{PolyEvent{0, PolyEventKind::NoteOn, 3}}),
          "Poly rejected a direct NoteOn with controller bindings");
    checkFrame(0, {0, 1, 0, 1});

    runtime.reset();
    check(runtime.processMidi({}, outputs, std::array{MidiNoteEvent{0, 0x9f, 60, 127}}),
          "MIDI rejected a Note On after runtime reset");
    checkFrame(0, midiDefaults);
}

void midiCCInputsReadControllerNumbersAndPairs()
{
    Graph graph;
    const std::array taps{graph.addInput(), graph.addInput(), graph.addInput(), graph.addInput()};
    const std::array bindings{
        PolyInput{.kind = PolyInputKind::MidiCC, .controller = 74},
        PolyInput{.kind = PolyInputKind::MidiCC14, .controller = 1},
        PolyInput{.kind = PolyInputKind::MidiCC, .controller = 33},
        PolyInput{.kind = PolyInputKind::MidiCC, .controller = 63},
    };
    PolyRuntime runtime(graph, bindings, taps, {2});
    std::array<std::array<float, 8>, 4> signal{};
    const std::array<OutputBuffer, 4> outputs{signal[0], signal[1], signal[2], signal[3]};
    constexpr std::array events{
        MidiNoteEvent{0, 0x90, 60, 127},
        MidiNoteEvent{1, 0xb0, 74, 65}, MidiNoteEvent{1, 0xb0, 1, 12}, MidiNoteEvent{1, 0xb0, 33, 34},
        MidiNoteEvent{1, 0xb0, 63, 64},
        MidiNoteEvent{2, 0xbf, 74, 41}, MidiNoteEvent{2, 0xbf, 1, 2}, MidiNoteEvent{2, 0xbf, 33, 5},
        MidiNoteEvent{3, 0x9f, 60, 127},
        MidiNoteEvent{4, 0xb0, 1, 17}, // MSB clears its old LSB.
        MidiNoteEvent{5, 0xb0, 121, 0},
        MidiNoteEvent{6, 0xbf, 127, 0}, // Applies the All Notes Off part on channel 16.
        MidiNoteEvent{7, 0x9f, 60, 127},
    };
    allocationCheck::enabled = true;
    const bool ok = runtime.processMidi({}, outputs, events);
    allocationCheck::enabled = false;
    check(ok, "MIDI rejected valid CC inputs or a Poly Mode message");
    check(allocationCheck::count == 0, "MIDI CC readout allocated memory");
    constexpr std::array<std::array<float, 8>, 4> expected{{
        {0, 65, 65, 106, 106, 106, 65, 106},
        {0, 1570, 1570, 1831, 2437, 261, 0, 261},
        {0, 34, 34, 39, 5, 5, 0, 5},
        {0, 64, 64, 64, 64, 64, 64, 64},
    }};
    check(signal == expected, "MIDI CC inputs changed the wrong byte, channel, or sample");
}

void midiSostenutoCapturesOnlyHeldKeys()
{
    Graph graph;
    const auto gate = graph.addInput();
    const std::array bindings{PolyInput{PolyInputKind::Gate}};
    PolyRuntime runtime(graph, bindings, std::array{gate}, {3});
    std::array<float, 11> signal{};
    const std::array<OutputBuffer, 1> outputs{signal};
    constexpr std::array events{
        MidiNoteEvent{0, 0x90, 60, 127}, MidiNoteEvent{0, 0x91, 60, 127},
        MidiNoteEvent{1, 0xb0, 66, 64}, MidiNoteEvent{1, 0xb1, 66, 127}, MidiNoteEvent{1, 0x81, 60, 0},
        MidiNoteEvent{2, 0x90, 61, 127}, MidiNoteEvent{2, 0xb0, 66, 127}, // Still down.
        MidiNoteEvent{3, 0x80, 60, 0}, MidiNoteEvent{3, 0x80, 61, 0},
        MidiNoteEvent{4, 0xb0, 64, 127},
        MidiNoteEvent{5, 0xb0, 66, 63}, // Sustain still holds the first note.
        MidiNoteEvent{6, 0x90, 62, 127}, MidiNoteEvent{6, 0xb0, 66, 127}, // Capture only key 62.
        MidiNoteEvent{7, 0xb0, 64, 0},
        MidiNoteEvent{8, 0xb0, 123, 0}, // Sostenuto keeps key 62 sounding.
        MidiNoteEvent{9, 0xb0, 121, 0},
        MidiNoteEvent{10, 0xb1, 66, 0},
    };
    allocationCheck::enabled = true;
    const bool ok = runtime.processMidi({}, outputs, events);
    allocationCheck::enabled = false;
    check(ok, "MIDI rejected a valid sostenuto block");
    check(allocationCheck::count == 0, "Sostenuto allocated memory");
    constexpr std::array<float, 11> expected{2, 2, 3, 2, 2, 2, 3, 2, 2, 1, 0};
    check(signal == expected && runtime.activeVoiceCount() == 0,
          "Sostenuto captured or released the wrong voice");
}

void midiRpnDataIncrementAndDecrementUseParameterSteps()
{
    Graph graph;
    const auto frequency = graph.addInput();
    const std::array bindings{PolyInput{PolyInputKind::Frequency}};
    PolyRuntime runtime(graph, bindings, std::array{frequency}, {2, 880});
    std::array<float, 11> signal{};
    const std::array<OutputBuffer, 1> outputs{signal};
    constexpr std::array events{
        MidiNoteEvent{0, 0x90, 69, 127}, MidiNoteEvent{0, 0x91, 81, 127},
        MidiNoteEvent{1, 0xe0, 127, 127},
        MidiNoteEvent{1, 0xb0, 101, 0}, MidiNoteEvent{1, 0xb0, 100, 0}, MidiNoteEvent{1, 0xb0, 96, 127},
        MidiNoteEvent{2, 0xb0, 97, 0},
        MidiNoteEvent{3, 0xb0, 100, 1}, MidiNoteEvent{3, 0xb0, 6, 63}, MidiNoteEvent{3, 0xb0, 38, 127},
        MidiNoteEvent{4, 0xb0, 96, 0}, // Carry across the low byte to reach center.
        MidiNoteEvent{5, 0xb0, 97, 0},
        MidiNoteEvent{6, 0xb0, 100, 2}, MidiNoteEvent{6, 0xb0, 96, 0},
        MidiNoteEvent{7, 0xb0, 97, 0},
        MidiNoteEvent{8, 0xb0, 6, 127}, MidiNoteEvent{8, 0xb0, 96, 0},
        MidiNoteEvent{9, 0xb0, 101, 127}, MidiNoteEvent{9, 0xb0, 100, 127}, MidiNoteEvent{9, 0xb0, 97, 0},
        MidiNoteEvent{10, 0xb0, 121, 0},
    };
    allocationCheck::enabled = true;
    const bool ok = runtime.processMidi({}, outputs, events);
    allocationCheck::enabled = false;
    check(ok, "MIDI rejected a valid RPN step");
    check(allocationCheck::count == 0, "RPN steps allocated memory");
    constexpr double fine = -1.0 / 8192;
    constexpr std::array semitones{0.0, 2.01, 2.0, 2 + fine, 2.0, 2 + fine, 3 + fine, 2 + fine,
                                   65 + fine, 65 + fine, 63 + fine};
    for (std::size_t frame = 0; frame < semitones.size(); ++frame) {
        const double expected = 2 + std::exp2(semitones[frame] / 12);
        check(std::abs(signal[frame] - expected) < expected * 0.0000002,
              "Data Increment or Decrement used the wrong step, channel, or bound");
    }
}

void invalidMidiBlocksAreAtomic()
{
    Graph graph;
    const auto gate = graph.addInput();
    const std::array bindings{PolyInput{PolyInputKind::Gate}};
    const std::array taps{gate};
    PolyRuntime runtime(graph, bindings, taps, {1});
    std::array signal{42.0f, 42.0f};
    const std::array<OutputBuffer, 1> outputs{signal};
    constexpr MidiNoteEvent on{0, 0x9f, 127, 1};
    for (const auto invalid : std::array{MidiNoteEvent{1, 0x7f, 127, 0}, MidiNoteEvent{1, 0xa0, 127, 0},
                                        MidiNoteEvent{1, 0xbf, 128, 64}, MidiNoteEvent{1, 0xbf, 64, 128},
                                        MidiNoteEvent{1, 0xbf, 11, 128},
                                        MidiNoteEvent{1, 0xbf, 120, 1}, MidiNoteEvent{1, 0xbf, 121, 1},
                                        MidiNoteEvent{1, 0xbf, 122, 64}, MidiNoteEvent{1, 0xbf, 123, 1},
                                        MidiNoteEvent{1, 0xbf, 126, 17}, MidiNoteEvent{1, 0xbf, 127, 1},
                                        MidiNoteEvent{1, 0xbf, 101, 128}, MidiNoteEvent{1, 0xbf, 38, 128},
                                        MidiNoteEvent{1, 0xef, 128, 64}, MidiNoteEvent{1, 0xef, 0, 128},
                                        MidiNoteEvent{1, 0x8f, 128, 0}, MidiNoteEvent{1, 0x8f, 127, 128},
                                        MidiNoteEvent{2, 0x8f, 127, 0}}) {
        check(!runtime.processMidi({}, outputs, std::array{on, MidiNoteEvent{0, 0xbf, 64, 127}, invalid}),
              "Accepted an invalid MIDI block");
        check(runtime.activeVoiceCount() == 0 && signal == std::array{42.0f, 42.0f},
              "Rejected MIDI block changed voice state or output");
    }
    check(runtime.processMidi({}, outputs, std::array{on}), "MIDI rejected a valid boundary channel and pitch");
    check(runtime.voiceInfo(0).id == 1 && signal == std::array{1.0f, 1.0f},
          "Rejected MIDI block consumed a note ID");
    check(runtime.processMidi({}, outputs, std::array{MidiNoteEvent{1, 0x8f, 127, 0}}),
          "MIDI rejected a later block carrying a matching Note Off");
    check(signal == std::array{1.0f, 0.0f}, "A rejected MIDI block must not change the sustain state");
}

void assemblerShufflesBetweenPackedVoices()
{
    const auto program = compileDspCode(R"asm(streamin pitch;
streamout out;
float base=59;
float step=0.015625;
float phase;
movaps xmm1,pitch;
subps xmm1,base;
movaps xmm0,phase;
addps xmm0,xmm1;
shufps xmm0,xmm0,27;
mulps xmm0,xmm1;
movaps out,xmm0;
movaps xmm2,xmm1;
mulps xmm2,step;
addps xmm2,phase;
movaps xmm3,xmm2;
cmpltps xmm3,xmm1;
andps xmm2,xmm3;
movaps phase,xmm2;
)asm", DspLanguage::Assembler);
    for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto pitch = graph.addInput();
        const auto block = graph.addDspCode(program);
        graph.connectDspInput(pitch, block.id, 0);
        constexpr std::array bindings{PolyInput{PolyInputKind::Pitch}};
        for (const std::size_t voiceCount : {4, 6, 8}) for (const std::size_t size : {1, 7, 64}) {
            std::vector<NodeId> taps(voiceCount + 1, block.outputs[0]);
            std::vector<PolyEvent> notes;
            for (std::size_t voice = 0; voice < voiceCount; ++voice) notes.push_back({0, PolyEventKind::NoteOn, voice + 1, static_cast<std::uint8_t>(60 + voice)});
            PolyRuntime runtime(graph, bindings, taps, {.maxVoices = voiceCount});
            for (std::size_t lane = 0; lane < voiceCount; ++lane) check(runtime.setOutputVoiceIndex(lane, lane), "Select each packed voice lane");
            std::vector<std::array<float, 64>> output(voiceCount + 1);
            for (std::size_t frame = 0; frame < 64; frame += size) {
                const auto count = std::min(size, 64 - frame);
                std::vector<OutputBuffer> channels(voiceCount + 1);
                for (std::size_t channel = 0; channel < channels.size(); ++channel) channels[channel] = std::span{output[channel]}.subspan(frame, count);
                allocationCheck::count = 0;
                allocationCheck::enabled = true;
                const bool ok = runtime.process({}, channels, frame == 0 ? std::span{notes} : std::span<const PolyEvent>{});
                allocationCheck::enabled = false;
                check(ok && allocationCheck::count == 0, "Packed DSP processes voice lanes without allocating");
            }
            for (std::size_t frame = 0; frame < 64; ++frame) {
                const float scale = 1.F + frame / 64.F;
                float mixed = 0;
                for (std::size_t voice = 0; voice < voiceCount; ++voice) {
                    const auto peer = (voice / 4) * 4 + 3 - voice % 4;
                    const float expected = peer < voiceCount ? (voice + 1) * (peer + 1) * scale : 0;
                    check(output[voice][frame] == expected, "SHUFPS crosses voice lanes within each packet and masks inactive lanes");
                    mixed += expected;
                }
                check(output[voiceCount][frame] == mixed, "Mix all packed voices after their shuffle");
            }
        }
    }
}

void packedDspInitializesOnlyNewVoices()
{
    const auto program = compileDspCode(R"(
        polyin pitch;
        polyout out;
        float phase;
        stage(0) { phase = 100; }
        phase = phase + 1;
        out = phase + pitch;
    )");
    for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto pitch = graph.addInput();
        const auto block = graph.addDspCode(program);
        graph.connectDspInput(pitch, block.id, 0);
        constexpr std::array bindings{PolyInput{PolyInputKind::Pitch}};
        const std::array taps{block.outputs[0], block.outputs[0], block.outputs[0]};
        constexpr std::array events{
            PolyEvent{0, PolyEventKind::NoteOn, 1, 60},
            PolyEvent{2, PolyEventKind::NoteOn, 2, 61},
            PolyEvent{4, PolyEventKind::NoteOff, 1},
            PolyEvent{5, PolyEventKind::NoteOn, 3, 62},
        };
        for (const std::size_t size : {1, 3, 8}) {
            PolyRuntime runtime(graph, bindings, taps, {.maxVoices = 6});
            check(runtime.setOutputVoiceIndex(0, 0) && runtime.setOutputVoiceIndex(1, 1), "Select reused voice lanes");
            std::array<std::array<float, 8>, 3> output{};
            for (std::size_t frame = 0; frame < 8; frame += size) {
                const auto count = std::min(size, 8 - frame);
                std::vector<PolyEvent> local;
                for (auto event : events) if (event.frame >= frame && event.frame < frame + count) {
                    event.frame -= frame;
                    local.push_back(event);
                }
                std::array<OutputBuffer, 3> channels;
                for (std::size_t channel = 0; channel < channels.size(); ++channel) channels[channel] = std::span{output[channel]}.subspan(frame, count);
                check(runtime.process({}, channels, local), "Render staggered voice initialization");
            }
            check(output[0] == std::array{161.F, 162.F, 163.F, 164.F, 0.F, 163.F, 164.F, 165.F}, "Reusing a lane clears only that voice's DSP state");
            check(output[1] == std::array{0.F, 0.F, 162.F, 163.F, 164.F, 165.F, 166.F, 167.F}, "Stage zero does not restart held neighbors");
            for (std::size_t frame = 0; frame < 8; ++frame) check(output[2][frame] == output[0][frame] + output[1][frame], "Inactive lanes do not enter the mix");
            runtime.reset();
            std::array<float, 1> resetSample{};
            const std::array<OutputBuffer, 3> resetOutputs{resetSample, resetSample, resetSample};
            check(runtime.process({}, resetOutputs, std::span{events}.first(1)) && resetSample[0] == 161.F, "Reset clears packet and voice state");
        }
    }
}

void packedDspStagesRunInGraphOrder()
{
    const auto early = compileDspCode(R"(
        streamin pitch; streamout shuffled;
        stage(1) {
            movaps xmm0,pitch;
            shufps xmm0,xmm0,27;
            movaps shuffled,xmm0;
        }
    )", DspLanguage::Assembler);
    const auto late = compileDspCode(R"(
        polyin shuffled; polyin pitch; polyout weighted;
        float previous;
        weighted = previous * pitch;
        stage(3) { previous = shuffled; }
    )");
    for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto pitch = graph.addInput();
        const auto first = graph.addDspCode(early), second = graph.addDspCode(late);
        graph.connectDspInput(pitch, first.id, 0);
        graph.connectDspInput(first.outputs[0], second.id, 0);
        graph.connectDspInput(pitch, second.id, 1);
        const std::array bindings{PolyInput{PolyInputKind::Pitch}};
        constexpr std::array notes{
            PolyEvent{0, PolyEventKind::NoteOn, 1, 60}, PolyEvent{0, PolyEventKind::NoteOn, 2, 61},
            PolyEvent{0, PolyEventKind::NoteOn, 3, 62}, PolyEvent{0, PolyEventKind::NoteOn, 4, 63},
        };
        for (const std::size_t size : {1, 7, 16}) {
            PolyRuntime runtime(graph, bindings, second.outputs, {.maxVoices = 4});
            std::array<float, 16> output{};
            for (std::size_t frame = 0; frame < output.size(); frame += size) {
                const auto count = std::min(size, output.size() - frame);
                check(runtime.process({}, std::array{OutputBuffer{std::span{output}.subspan(frame, count)}},
                    frame == 0 ? std::span{notes} : std::span<const PolyEvent>{}), "Render packed early and late stages");
            }
            check(output[0] == 0 && std::ranges::all_of(std::span{output}.subspan(1), [](float value) { return value == 15124; }),
                "Stage 1 shuffles feed later DSP blocks, and stage 3 writes become visible on the next sample");
        }
    }
}

void packedX87RenderingAllocatesNothing()
{
    std::string source = "polyin pitch; polyout out; double big=0x43e0000000000000;";
    for (std::size_t lane = 0; lane < 4; ++lane) source += "dfld big; fld pitch[" + std::to_string(lane) + "]; fadd; dfld big; fsub; fstp out[" + std::to_string(lane) + "];";
    const auto program = compileDspCode(source, DspLanguage::Assembler);
    for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto block = graph.addDspCode(program);
        graph.connectDspInput(graph.addInput(), block.id, 0);
        constexpr std::array bindings{PolyInput{PolyInputKind::Pitch}};
        std::array<PolyEvent, 8> notes;
        for (std::size_t voice = 0; voice < 8; ++voice) notes[voice] = {0, PolyEventKind::NoteOn, voice + 1, static_cast<int>(60 + voice)};
        for (const std::size_t size : {1, 7, 64}) {
            PolyRuntime runtime(graph, bindings, block.outputs, {.maxVoices = 8});
            std::array<float, 64> output{};
            for (std::size_t frame = 0; frame < output.size(); frame += size) {
                const auto count = std::min(size, output.size() - frame);
                allocationCheck::count = 0;
                allocationCheck::enabled = true;
                const auto ok = runtime.process({}, std::array{OutputBuffer{std::span{output}.subspan(frame, count)}},
                    frame == 0 ? std::span<const PolyEvent>{notes} : std::span<const PolyEvent>{});
                allocationCheck::enabled = false;
                check(ok && allocationCheck::count == 0, "Packed software x87 rendering allocates nothing");
            }
            check(std::ranges::all_of(output, [](float value) { return value == 508; }), "Each packed voice retains extended x87 precision");
        }
    }
}

void invalidConfigurationsFailBeforeRendering()
{
    Graph graph;
    const auto input = graph.addInput();
    const std::array<PolyInput, 1> bindings{{{PolyInputKind::Gate}}};
    const std::array taps{input};
    rejects([&] { PolyRuntime runtime(graph, bindings, taps, {0}); }, "Accepted zero voices");
    rejects([&] { PolyRuntime runtime(graph, bindings, taps, {1, 0}); }, "Accepted zero sample rate");
    rejects([&] { PolyRuntime runtime(graph, bindings, taps, {1, std::numeric_limits<double>::infinity()}); },
            "Accepted nonfinite sample rate");
    rejects([&] { PolyRuntime runtime(graph, bindings, taps, {1, 1e-40}); },
            "Accepted sample rate too small for the pitch table");
    rejects([&] { PolyRuntime runtime(graph, {}, taps); }, "Accepted missing binding");
    rejects([&] { PolyRuntime runtime(graph, bindings, {}); }, "Accepted no taps");
    const std::array<PolyInput, 1> sparse{{{PolyInputKind::Mono, 2}}};
    rejects([&] { PolyRuntime runtime(graph, sparse, taps); }, "Accepted sparse Mono input indices");
    const std::array<PolyInput, 1> unknown{{{static_cast<PolyInputKind>(99)}}};
    rejects([&] { PolyRuntime runtime(graph, unknown, taps); }, "Accepted unknown input kind");
    for (const auto binding : std::array{
             PolyInput{.kind = PolyInputKind::MidiCC, .controller = 128},
             PolyInput{.kind = PolyInputKind::MidiCC14, .controller = 32},
             PolyInput{.kind = PolyInputKind::MidiCC14, .controller = 6},
         }) {
        rejects([&] { PolyRuntime runtime(graph, std::array{binding}, taps); }, "Accepted invalid MIDI CC binding");
    }
    const auto sum = graph.addSum();
    graph.connect(sum, sum);
    rejects([&] { PolyRuntime runtime(graph, bindings, std::array{sum}); }, "Poly accepted algebraic feedback");
}

}

int main()
{
    try {
        monoPolyBridgesRouteByVoiceSlot();
        overlappingVoicesMatchIndependentMono();
        lifecycleAndControls();
        releaseEndComesFromEachVoiceGraph();
        stealingAndSlotReuseClearHistory();
        sharedMonoAndSameFrameOrdering();
        invalidBlocksAreAtomic();
        midiNotesReleaseByChannelAndPitch();
        midiSustainReleasesOnlyKeyUpVoicesOnItsChannel();
        midiSustainClearsOnVoiceReuseAndReset();
        midiNoteOffConsumesStolenInstanceBeforeNewVoice();
        midiAllNotesOffRespectsSustain();
        midiAllSoundOffCutsOnlyItsChannel();
        midiResetAllControllersRestoresSustainOnItsChannel();
        midiPitchBendChangesOnlyItsChannel();
        midiPitchBendSensitivityIsPerChannelAndSurvivesControllerReset();
        midiChannelFineTuningUsesBothDataBytes();
        midiChannelCoarseTuningCombinesWithFineAndBend();
        midiContinuousControllersDriveOnlyTheirChannel();
        midiCCInputsReadControllerNumbersAndPairs();
        midiSostenutoCapturesOnlyHeldKeys();
        midiRpnDataIncrementAndDecrementUseParameterSteps();
        invalidMidiBlocksAreAtomic();
        assemblerShufflesBetweenPackedVoices();
        packedDspInitializesOnlyNewVoices();
        packedDspStagesRunInGraphOrder();
        packedX87RenderingAllocatesNothing();
        invalidConfigurationsFailBeforeRendering();
        std::cout << "Poly: eight independent voices, buffer invariance, release/steal/reset, controls, and allocation checks passed\n";
    } catch (const std::exception& error) {
        allocationCheck::enabled = false;
        std::cerr << error.what() << '\n';
        return 1;
    }
}
