#include "stream/PolyRuntime.hpp"
#include "stream/StreamCounter.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <cfenv>
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <xmmintrin.h>
#endif
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace openstone::stream;

void check(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template<typename Sample>
void feedbackIsIndependentOfBlockSize(Sample amplitude, auto compileRuntime)
{
    Graph graph;
    const auto sum = graph.addSum();
    const auto gain = graph.addGain(0.5);
    const auto input = graph.addInput();
    const auto delay = graph.addDelay();
    graph.connect(input, sum);
    graph.connect(gain, sum);
    graph.connect(sum, delay);
    graph.connect(delay, gain);

    constexpr std::size_t frameCount = 640;
    struct FeedbackResponse {
        std::array<Sample, frameCount> sum{};
        std::array<Sample, frameCount> delayed{};

        bool operator==(const FeedbackResponse&) const = default;
    };
    const std::array<Sample, frameCount> impulse{amplitude};
    const auto render = [&](std::span<const std::size_t> blockSizes) {
        auto runtime = (graph.*compileRuntime)(std::array{sum, delay});
        FeedbackResponse response;
        for (std::size_t frame = 0, block = 0; frame < frameCount; ++block) {
            const auto size = std::min(blockSizes[block % blockSizes.size()], frameCount - frame);
            const std::array inputs{std::span{impulse}.subspan(frame, size)};
            const std::array outputs{
                std::span{response.sum}.subspan(frame, size),
                std::span{response.delayed}.subspan(frame, size),
            };
            check(runtime.process(inputs, outputs), "Feedback render rejected a valid block");
            frame += size;
        }
        return response;
    };
    const auto reference = render(std::array<std::size_t, 1>{1});
    // Compare the analytical decay through normal values and the entire render across block sizes.
    constexpr auto normalFrames = std::min(static_cast<int>(frameCount), 2 - std::numeric_limits<Sample>::min_exponent);
    for (int frame = 0; frame < normalFrames; ++frame) {
        check(reference.sum[frame] == std::ldexp(amplitude, -frame), "Unexpected impulse response");
    }
    // The FlowStone capture taps Delay1, which shows the same decay one sample later.
    check(reference.delayed.front() == Sample{}, "Delay tap must start at zero");
    check(std::ranges::equal(std::span{reference.delayed}.subspan(1),
                            std::span{reference.sum}.first(frameCount - 1)),
          "Delay tap must lag the sum by exactly one sample");
    constexpr std::array<std::size_t, 6> fixedBlockSizes{7, 16, 31, 64, 257, frameCount};
    for (const auto size : fixedBlockSizes) {
        check(render(std::array{size}) == reference, "Feedback response changed with block size");
    }
    check(render(std::array<std::size_t, 6>{0, 3, 1, 17, 0, 64}) == reference,
          "Feedback response changed with varying block sizes");
}

void monoDoubleMultipliersPreservePrecision()
{
    Graph graph;
    const auto input = graph.addInput();
    const auto factorInput = graph.addInput();
    constexpr MonoDouble multiplier = 1.0 + 0x1p-40;
    const auto gain = graph.addGain(multiplier);
    const auto multiply = graph.addMultiply();
    graph.connect(input, gain);
    graph.connect(input, multiply);
    graph.connect(factorInput, multiply);
    auto runtime = graph.compileMonoDouble(std::array{gain, multiply});

    constexpr std::array signal{0.5};
    constexpr std::array factor{multiplier};
    std::array<MonoDouble, 1> gainResponse{};
    std::array<MonoDouble, 1> multiplyResponse{};
    const std::array inputs{MonoDoubleInputBuffer{signal}, MonoDoubleInputBuffer{factor}};
    const std::array outputs{MonoDoubleOutputBuffer{gainResponse}, MonoDoubleOutputBuffer{multiplyResponse}};
    check(runtime.process(inputs, outputs), "MonoDouble multiply render failed");
    constexpr std::array expected{0.5 + 0x1p-41};
    check(gainResponse == expected, "MonoDouble fixed gain must retain double precision");
    check(multiplyResponse == expected, "MonoDouble stream multiplier must retain double precision");
}

void delaysReadTheirOldStateBeforeWriting()
{
    Graph graph;
    const auto second = graph.addDelay();
    const auto first = graph.addDelay();
    const auto input = graph.addInput();
    graph.connect(input, first);
    graph.connect(first, second);
    auto runtime = graph.compile(std::array{second, first});

    const std::array<float, 4> signal{1, -2, 3, 4};
    std::array<float, 4> secondOutput{};
    std::array<float, 4> firstOutput{};
    const std::array inputs{InputBuffer{signal}};
    const std::array outputs{OutputBuffer{secondOutput}, OutputBuffer{firstOutput}};
    check(runtime.process(inputs, outputs), "Two-output delay render failed");
    check(secondOutput == std::array{0.0F, 0.0F, 1.0F, -2.0F} &&
          firstOutput == std::array{0.0F, 1.0F, -2.0F, 3.0F},
          "Cascaded delays must introduce one sample each and preserve tap order");

    const std::array next{InputBuffer{signal}.last(1)};
    const std::array mismatched{outputs[0].first(1), outputs[1].first(2)};
    check(!runtime.process(next, mismatched) && firstOutput == std::array{0.0F, 1.0F, -2.0F, 3.0F},
          "Rejecting a malformed block must leave the output buffers alone");
    const std::array valid{outputs[0].first(1), outputs[1].first(1)};
    check(runtime.process(next, valid) && secondOutput[0] == 3.0F && firstOutput[0] == 4.0F,
          "Rejecting a malformed block must preserve delay history");

    runtime.reset();
    check(runtime.process(inputs, outputs) && secondOutput == std::array{0.0F, 0.0F, 1.0F, -2.0F} &&
          firstOutput == std::array{0.0F, 1.0F, -2.0F, 3.0F},
          "Reset must restore the initial silence in all delays");
}

void streamCountersKeepTheirOwnPrecision()
{
    const auto single = makeStreamCounter(SamplePrecision::Float32);
    const auto wide = makeStreamCounter(SamplePrecision::Float64);
    std::array resets{0.};
    constexpr int singleLimit = 1 << 24;
    double narrowSample{}, wideSample{};
    for (int n = 0; n <= singleLimit + 1; ++n) {
        narrowSample = single->process(resets);
        wideSample = wide->process(resets);
    }
    check(narrowSample == singleLimit && wideSample == singleLimit + 1,
          "The double counter still advances by one at the single-precision limit");
    resets[0] = 1;
    check(single->process(resets) == 0 && wide->process(resets) == 0,
          "Both precisions reset on the next sample after a changed trigger count");
    check(single->process(resets) == 1 && wide->process(resets) == 1,
          "A delivered reset only affects one sample");
}

void variableDelayClampsToTheMaximumAndResetsHistory()
{
    constexpr std::size_t limit = 262144;
    Graph graph;
    const auto in = graph.addInput();
    const auto length = graph.addInput();
    const auto delay = graph.addVariableDelay(length, SamplePrecision::Float64);
    graph.connect(in, delay);
    auto runtime = graph.compileMonoDouble(std::array{delay});
    constexpr double impulse = 1.0 + 0x1p-40;
    std::vector<double> samples(limit + 5);
    samples.front() = impulse;
    samples[3] = -.5;
    samples.back() = .25;
    const std::vector<double> lengths(samples.size(), static_cast<double>(limit) + 1000);
    std::vector<double> response(samples.size(), 99);
    for (std::size_t offset = 0; offset < samples.size();) {
        const auto count = std::min<std::size_t>(317, samples.size() - offset);
        check(runtime.process(
            std::array{MonoDoubleInputBuffer{samples}.subspan(offset, count),
                       MonoDoubleInputBuffer{lengths}.subspan(offset, count)},
            std::array{MonoDoubleOutputBuffer{response}.subspan(offset, count)}),
            "Maximum delay render rejected a valid block");
        offset += count;
    }
    std::vector<double> expected(samples.size());
    expected[limit] = impulse;
    expected[limit + 3] = -.5;
    check(response == expected, "The delay clamps at 262144 samples and keeps playing across the ring wrap");

    constexpr std::array<double, 2> silence{};
    constexpr std::array<double, 2> oneSample{1, 1};
    std::array<double, 2> afterReset{};
    runtime.reset();
    check(runtime.process(std::array{MonoDoubleInputBuffer{silence}, MonoDoubleInputBuffer{oneSample}},
                          std::array{MonoDoubleOutputBuffer{afterReset}}) && afterReset == silence,
          "A reset clears all previous samples");
}

template<std::size_t frames, typename Scalar>
void packedFeedbackMatchesFourScalarStreams(
    const std::array<Scalar, 4>& amplitudes, auto compilePacked, auto compileScalar,
    std::span<const std::size_t> blockSizes)
{
    Graph graph;
    const auto input = graph.addInput();
    const auto factors = graph.addInput();
    const auto sum = graph.addSum();
    const auto delay = graph.addDelay();
    const auto multiply = graph.addMultiply();
    graph.connect(input, sum);
    graph.connect(sum, delay);
    graph.connect(delay, multiply);
    graph.connect(factors, multiply);
    const std::array taps{sum, delay, multiply};

    using PackedSample = std::array<Scalar, 4>;
    using PackedFrames = std::array<PackedSample, frames>;
    constexpr Scalar gain = 0.5;
    PackedFrames packedInput{};
    PackedFrames packedFactors;
    for (std::size_t lane = 0; lane < amplitudes.size(); ++lane) {
        packedInput[lane][lane] = amplitudes[lane];
    }
    packedFactors.fill(PackedSample{gain, gain, gain, gain});

    const auto render = [&]<typename Sample, std::size_t tapCount>(
                            auto compile, const std::array<Sample, frames>& signal,
                            const std::array<Sample, frames>& factors, const std::array<NodeId, tapCount>& outputs,
                            std::span<const std::size_t> sizes) {
        auto runtime = (graph.*compile)(outputs);
        std::array<std::array<Sample, frames>, tapCount> response{};
        for (std::size_t frame = 0, block = 0; frame < frames; ++block) {
            const auto size = std::min(sizes[block % sizes.size()], frames - frame);
            const std::array inputs{
                std::span{signal}.subspan(frame, size),
                std::span{factors}.subspan(frame, size),
            };
            std::array<std::span<Sample>, tapCount> blockOutputs{};
            for (std::size_t tap = 0; tap < tapCount; ++tap) {
                blockOutputs[tap] = std::span{response[tap]}.subspan(frame, size);
            }
            check(runtime.process(inputs, blockOutputs), "Staggered impulse render failed");
            frame += size;
        }
        return response;
    };

    const auto [openSum, openDelay, openMultiply] =
        render(compilePacked, packedInput, packedFactors, taps, std::array{frames});
    check(openSum == packedInput, "An open loop must pass the staggered inputs through Add");
    check(openDelay.front() == PackedSample{} &&
              std::ranges::equal(std::span{openDelay}.subspan(1), std::span{openSum}.first(frames - 1)),
          "Packed Delay1 must lag Add by one sample");
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::size_t lane = 0; lane < amplitudes.size(); ++lane) {
            check(openMultiply[frame][lane] == gain * openDelay[frame][lane],
                  "Packed Multiply must scale Delay1 on the same sample");
        }
    }

    graph.connect(multiply, sum);

    std::array<Scalar, frames> monoFactors;
    monoFactors.fill(gain);
    std::array<PackedFrames, taps.size()> expected{};
    for (std::size_t lane = 0; lane < amplitudes.size(); ++lane) {
        std::array<Scalar, frames> inputLane{};
        inputLane[lane] = amplitudes[lane];
        const auto scalar = render(compileScalar, inputLane, monoFactors, taps, std::array{frames});
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const auto expectedSum = frame < lane ? Scalar{} :
                std::ldexp(amplitudes[lane], -static_cast<int>(frame - lane));
            const auto expectedDelay = frame <= lane ? Scalar{} :
                std::ldexp(amplitudes[lane], -static_cast<int>(frame - lane - 1));
            check(scalar[0][frame] == expectedSum && scalar[1][frame] == expectedDelay &&
                      scalar[2][frame] == gain * expectedDelay,
                  "Scalar reference must preserve the exact amplitude and decay");
            for (std::size_t tap = 0; tap < taps.size(); ++tap) {
                expected[tap][frame][lane] = scalar[tap][frame];
            }
        }
    }

    const auto compareTaps = [&](std::span<const std::size_t> sizes) {
        const auto response = render(compilePacked, packedInput, packedFactors, taps, sizes);
        check(response[0] == expected[0], "Packed Add must match all four independent scalar renders");
        check(response[1] == expected[1], "Packed Delay1 must match all four independent scalar renders");
        check(response[2] == expected[2], "Packed Multiply must match all four independent scalar renders");
        check(render(compilePacked, packedInput, packedFactors, std::array{delay}, sizes).front() == expected[1],
              "Capturing Delay1 alone must preserve packed feedback timing");
        check(render(compilePacked, packedInput, packedFactors, std::array{multiply}, sizes).front() == expected[2],
              "Capturing Multiply alone must preserve packed feedback timing");
    };
    for (const auto size : blockSizes) {
        compareTaps(std::array{size});
    }
    compareTaps(std::array<std::size_t, 6>{0, 3, 1, 17, 0, 64});
}

void sumUsesLinkOrderAndSupportsInPlaceBuffers()
{
    Graph graph;
    const auto input1 = graph.addInput();
    const auto input2 = graph.addInput();
    const auto input3 = graph.addInput();
    const auto sum = graph.addSum();
    const auto gain = graph.addGain(2.0F);
    graph.connect(input1, sum);
    graph.connect(input2, sum);
    graph.connect(input3, sum);
    graph.connect(sum, gain);
    auto runtime = graph.compile(std::array{gain});

    std::array<float, 2> first{1.0e20F, 2.0F};
    const std::array<float, 2> second{-1.0e20F, -1.0F};
    const std::array<float, 2> third{1.0F, 3.0F};
    const std::array inputs{InputBuffer{first}, InputBuffer{second}, InputBuffer{third}};
    check(runtime.process(inputs, std::array{OutputBuffer{first}}) && first == std::array{2.0F, 8.0F},
          "Fan-in must sum in link order before applying gain, including in-place renders");
}

void mono4MultiplyMatchesFlowStone()
{
    Graph graph;
    const auto signal = graph.addInput();
    const auto factors = graph.addInput();
    const auto gain = graph.addGain(0.5F);
    const auto packedOutput = graph.addSum();
    const auto multiply = graph.addMultiply();
    graph.connect(signal, gain);
    graph.connect(signal, multiply);
    graph.connect(factors, multiply);
    graph.connect(multiply, packedOutput);
    auto runtime = graph.compileMono4(std::array{gain, packedOutput});

    constexpr Mono4 sample{1, -2, 4, 8};
    const std::array signalFrames{sample, sample, sample};
    const std::array factorFrames{
        Mono4{0.5F, 0.5F, 0.5F, 0.5F},
        Mono4{0.5F}, // Unconnected Pack inputs contribute positive zero.
        Mono4{2.0F, 3.0F, -0.25F, -2.0F},
    };
    std::array<Mono4, signalFrames.size()> broadcast{};
    std::array<Mono4, signalFrames.size()> packed{};
    const std::array inputs{Mono4InputBuffer{signalFrames}, Mono4InputBuffer{factorFrames}};
    const std::array outputs{Mono4OutputBuffer{broadcast}, Mono4OutputBuffer{packed}};
    check(runtime.process(inputs, outputs), "Mono4 multiply render failed");
    for (const auto& frame : broadcast) {
        check(frame == Mono4{0.5F, -1.0F, 2.0F, 4.0F}, "Scalar gain must apply to every lane");
    }
    check(packed[0] == broadcast[0], "An explicit pack of four equal factors must match scalar gain");
    check(packed[1] == Mono4{0.5F, 0.0F, 0.0F, 0.0F} && std::signbit(packed[1][1]) &&
          !std::signbit(packed[1][2]) && !std::signbit(packed[1][3]),
          "A negative sample multiplied by an unwired Pack lane must retain negative zero");
    check(packed[2] == Mono4{2.0F, -6.0F, -1.0F, -16.0F}, "Packed multiplication must use each lane's factor");
}

void dspCodeFeedbackUsesGlobalStages()
{
    const auto program = compileDspCode(R"(
        streamin write;
        streamout previous;
        float memory;
        stage(0) { memory = 1; }
        stage(1) { previous = memory; }
        stage(3) { memory = write; }
    )");
    check(program.inputs().size() == 1 && program.inputs()[0].name == "write"
          && program.outputs().size() == 1 && program.outputs()[0].name == "previous"
          && program.outputs()[0].type == DspPortType::Stream, "DSP port declarations were not preserved");

    Graph graph;
    const auto sum = graph.addSum();
    const auto source = graph.addInput();
    const auto delay = graph.addDspCode(program);
    const auto half = graph.addGain(0.5);
    graph.connect(delay.outputs[0], half);
    graph.connect(source, sum);
    graph.connect(half, sum);
    graph.connectDspInput(sum, delay.id, 0);

    auto runtime = graph.compile(std::array{delay.outputs[0], sum});
    constexpr std::array<float, 6> samples{0, 1, 0, 0, 0, 0};
    constexpr std::array<float, 6> expected{1, 0.5F, 1.25F, 0.625F, 0.3125F, 0.15625F};
    constexpr std::array<float, 6> expectedSum{0.5F, 1.25F, 0.625F, 0.3125F, 0.15625F, 0.078125F};
    for (const std::size_t blockSize : {1, 2, 5, 6}) {
        runtime.reset();
        std::array<float, samples.size()> delayed{};
        std::array<float, samples.size()> summed{};
        check(runtime.process(std::array{InputBuffer{}}, std::array{OutputBuffer{}, OutputBuffer{}}),
              "Zero-frame DSP Code render failed");
        for (std::size_t start = 0; start < samples.size(); start += blockSize) {
            const auto length = std::min(samples.size() - start, blockSize);
            check(runtime.process(std::array{InputBuffer{samples}.subspan(start, length)},
                      std::array{OutputBuffer{delayed}.subspan(start, length),
                                 OutputBuffer{summed}.subspan(start, length)}),
                  "Feedback with a DSP Code delay failed to render");
        }
        check(delayed == expected && summed == expectedSum,
              "DSP Code initialization or global stage order changed across blocks");
    }
}

void dspCodeUsesEachLaneAndPreservesDoublePrecision()
{
    Graph graph;
    const auto source = graph.addInput();
    const auto program = compileDspCode(R"(
        streamdoublein value;
        streamdoubleout original;
        streamdoubleout positive;
        double epsilon = 9.094947017729282379150390625e-13;
        original = value + epsilon;
        positive = original & (value > 0);
    )");
    const auto code = graph.addDspCode(program);
    graph.connectDspInput(source, code.id, 0);
    auto runtime = graph.compileMonoDouble4(code.outputs);
    constexpr auto epsilon = 0x1p-40;
    constexpr std::array<MonoDouble4, 1> input{MonoDouble4{1, -2, 0, 4}};
    std::array<MonoDouble4, 1> original{};
    std::array<MonoDouble4, 1> positive{};
    check(runtime.process(std::array{MonoDouble4InputBuffer{input}},
                          std::array{MonoDouble4OutputBuffer{original}, MonoDouble4OutputBuffer{positive}}),
          "Double precision DSP Code failed to render on four lanes");
    check(original[0] == MonoDouble4{1 + epsilon, -2 + epsilon, epsilon, 4 + epsilon}
          && positive[0] == MonoDouble4{1 + epsilon, 0, 0, 4 + epsilon},
          "DSP Code lost double precision or shared comparison masks between lanes");
}

void dspCodeCanShapeIndependentMidiVoices()
{
    Graph graph;
    const auto gate = graph.addInput();
    const auto velocity = graph.addInput();
    const auto code = graph.addDspCode(compileDspCode(R"(
        polyboolin gate;
        polyin velocity;
        polyout signal;
        polyboolout finished;
        float level = 0;
        level = velocity & gate + (level * 0.5) & (gate ~ (1 == 1));
        signal = level;
        finished = level <= 0.25;
    )"));
    graph.connectDspInput(gate, code.id, 0);
    graph.connectDspInput(velocity, code.id, 1);
    const std::array bindings{PolyInput{PolyInputKind::Gate}, PolyInput{PolyInputKind::Velocity}};
    PolyRuntime runtime(graph, bindings, std::array{code.outputs[0]},
                        {.maxVoices = 2, .holdAfterNoteOff = true}, code.outputs[1]);
    constexpr std::array events{
        MidiNoteEvent{0, 0x90, 60, 127}, MidiNoteEvent{2, 0x91, 60, 127},
        MidiNoteEvent{3, 0x80, 60, 0}, MidiNoteEvent{5, 0x81, 60, 0},
    };
    std::array<float, 8> mixed{};
    check(runtime.processMidi({}, std::array{OutputBuffer{mixed}}, events),
          "Poly could not render MIDI voices through DSP Code");
    check(mixed == std::array{1.0F, 1.0F, 2.0F, 1.5F, 1.25F, 0.5F, 0.25F, 0.0F},
          "DSP Code release state or Gate leaked between voices");
    check(runtime.activeVoiceCount() == 0, "DSP Code Boolean release tap did not retire its voices");
}

void dspCodeReportsSourceErrorsAndRejectsInstantaneousFeedback()
{
    bool rejected = false;
    try {
        static_cast<void>(compileDspCode("streamout sample;\n  sample = missing;"));
    } catch (const DspCompileError& error) {
        rejected = error.line == 2 && error.column == 12
            && std::string_view(error.what()).find("missing") != std::string_view::npos;
    }
    check(rejected, "DSP compile errors should point to the offending token");

    Graph graph;
    const auto code = graph.addDspCode(compileDspCode("streamin x; streamout y; y = x;"));
    graph.connectDspInput(code.outputs[0], code.id, 0);
    rejected = false;
    try {
        static_cast<void>(graph.compile(code.outputs));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "DSP Code must not turn a zero-delay feedback loop into an implicit delay");
}

void dspAlphaPreservesIntegerPortsAndMixedPrecision()
{
    Graph graph;
    const auto upstream = graph.addDspCode(compileDspCode(R"(
        polyintout raw;
        int bits = 0x80000005;
        raw = bits;
    )"));
    const auto block = graph.addDspCode(compileDspCode(R"(
        polyintin raw;
        polydoubleout value;
        polyout status;
        double epsilon = 0x3d70000000000000;
        float unit = 0x3f800000;
        int recovered;
        int counter;
        bool valid = false;

        recovered = raw ~ 0x80000000;
        counter = int(2.75);
        counter ^= 3;
        counter ~= 1;
        counter &= 0x0f;
        counter |= 2;
        counter *= 6;
        counter /= 2;
        counter %= 7;
        counter -= 4;
        valid = (recovered == 5) & (counter == 1) & ((raw >>> 31) == -1)
            & ((raw >> 31) == 1) & ((recovered << 2) == 20)
            & (2 ^ 3 ^ 2 == 512);
        value = valid ? double(unit) + epsilon : -1;
        status = 3 & !false;
    )"));
    graph.connectDspInput(upstream.outputs[0], block.id, 0);
    auto runtime = graph.compileMonoDouble(block.outputs);
    std::array<double, 1> value{}, status{};
    check(runtime.process({}, std::array{MonoDoubleOutputBuffer{value}, MonoDoubleOutputBuffer{status}}),
          "Alpha DSP could not render mixed precision and PolyInt ports");
    check(value[0] == 1.0 + 0x1p-40 && status[0] == 3,
          "Alpha DSP lost raw integer bits, power, comparison masks, or precision");
}

void dspAlphaArraysAndLaneSelectionPreserveState()
{
    const auto program = compileDspCode(R"(
        memin memory[3];
        floatarrayin additions[3];
        streamout masked;
        streamout broadcast;
        streamout partial;
        streamout result;
        float x = {1,2,3,4};
        float selected = {9,9,9,9};
        int i = {0,1,2,3};
        float table[4] = {10,20,30,40};

        x{0,2} = 4;
        masked = x{1,3}(0);
        broadcast(1) = x + 5;
        selected{1} = x{1};
        partial = selected;
        table[i] += additions[i];
        result = table[i] + memory[i];
        table[99] = 100;
    )");
    check(program.inputs()[0].type == DspPortType::Mem && program.inputs()[0].arraySize == 3
          && program.inputs()[1].type == DspPortType::FloatArray, "DSP array ports lost their declared types");

    Graph graph;
    const auto block = graph.addDspCode(program);
    auto runtime = graph.compileMono4(block.outputs);
    check(runtime.setDspArrayInput(block.id, 0, std::array{3.0F, 4.0F, 5.0F, 123.0F})
          && runtime.setDspArrayInput(block.id, 1, std::array{1.0F, 2.0F})
          && !runtime.setDspArrayInput(block.id, 2, {}), "DSP array input rejected valid metadata or accepted an invalid port");

    std::array<Mono4, 1> masked{}, broadcast{}, partial{}, result{};
    const std::array outputs{Mono4OutputBuffer{masked}, Mono4OutputBuffer{broadcast},
                             Mono4OutputBuffer{partial}, Mono4OutputBuffer{result}};
    check(runtime.process({}, outputs), "DSP packed array processing failed");
    check(masked[0] == Mono4{0, 4, 0, 4} && broadcast[0] == Mono4{7, 7, 7, 7}
          && partial[0] == Mono4{9, 2, 9, 9}, "DSP lane mask or broadcast updated the wrong lanes");
    check(result[0] == Mono4{14, 26, 35, 40}, "DSP indexed update or input bounds were wrong");
    check(runtime.setDspArrayInput(block.id, 1, std::array{-1.0F})
          && runtime.process({}, outputs) && result[0] == Mono4{13, 26, 35, 40},
          "DSP array updates retained old entries or reset program state");
    runtime.reset();
    check(runtime.process({}, outputs) && result[0] == Mono4{12, 24, 35, 40},
          "DSP reset lost external inputs or retained local array state");
}

void dspAlphaHopAndNestedLoopsFollowSampleTime()
{
    Graph graph;
    const auto block = graph.addDspCode(compileDspCode(R"(
        streamout value;
        int counter;
        stage(0x0) { counter = 0; }
        hop(4) {
            loop(0x2) {
                loop(3) { counter += 1; }
            }
        }
        loop(0) { counter += 100; }
        value = float(counter);
    )"));
    auto runtime = graph.compile(block.outputs);
    std::array<float, 11> output{};
    for (const auto count : {1, 3, 7, 11}) {
        runtime.reset();
        for (std::size_t i = 0; i < output.size(); i += count) {
            const auto buffer = std::span{output}.subspan(i, std::min<std::size_t>(count, output.size() - i));
            check(runtime.process({}, std::array{OutputBuffer{buffer}}), "DSP hop render rejected a valid block");
        }
        check(output == std::array{6.F, 6.F, 6.F, 6.F, 12.F, 12.F, 12.F, 12.F, 18.F, 18.F, 18.F},
              "DSP hop restarted at a block boundary or nested loops counted incorrectly");
    }
}

void dspAlphaMathAndInitializersRunThroughTheInterpreter()
{
    Graph graph;
    const auto block = graph.addDspCode(compileDspCode(R"(
        streamdoubleout result;
        streamdoubleout random;
        double shortList[4] = {7};
        bool flags[4] = {true, false, true, false};
        double samples[5] = rand(5,10);
        int integers[2] = rand(5);
        float positive[1] = rand();
        float subnormal = 0x00000001;
        result = shortList[0] + shortList[1] + bool(flags[0]) + bool(flags[1])
            + pow(3,2) + sign(-2) + abs(-2) + exp(0) + sqrt(16) + log10(100) + log2(8)
            + ln(1) + rndint(2.1) + round(2.7) + int(-3.2) + trunc(2.7) + floor(-2.7) + ceil(-2.7)
            + sin1(0) + cos1(0) + tan1(0) + max(3,5) + min(3,5) + double(bool(denorm(subnormal)))
            + ((integers[0] >= 0) & (integers[0] < 5) & (positive[0] >= 0) & (positive[0] < 1) ? 1 : 100);
        random = samples[0];
    )"));
    auto runtime = graph.compileMonoDouble4(block.outputs);
    std::array<MonoDouble4, 1> output{}, random{};
    const std::array outputs{MonoDouble4OutputBuffer{output}, MonoDouble4OutputBuffer{random}};
    check(runtime.process({}, outputs) && output[0] == MonoDouble4{37, 37, 37, 37},
          "DSP math functions, casts, or array initializers returned unexpected results");
    check(std::ranges::all_of(random[0], [](auto value) { return value >= 5 && value < 10; })
          && random[0][0] != random[0][1], "DSP random initializer produced invalid or shared lane values");
    const auto first = random;
    runtime.reset();
    check(runtime.process({}, outputs) && random == first, "DSP random array reset was not reproducible");
}

void dspStageOneReadsReadySourcesInDependencyOrder()
{
    Graph graph;
    const auto copy = compileDspCode("streamin x; streamout y; stage(0x1) { y = x; }");
    const auto downstream = graph.addDspCode(copy);
    const auto upstream = graph.addDspCode(copy);
    const auto input = graph.addInput();
    graph.connectDspInput(input, upstream.id, 0);
    graph.connectDspInput(upstream.outputs[0], downstream.id, 0);
    auto runtime = graph.compile(downstream.outputs);
    constexpr std::array inputValues{2.0F, 4.0F, 6.0F};
    std::array<float, inputValues.size()> output{};
    check(runtime.process(std::array{InputBuffer{inputValues}}, std::array{OutputBuffer{output}})
          && output == inputValues, "Stage 1 did not process upstream values before their consumers");
}

template<typename Sample, typename Compile>
void packedLaneShiftDoesNotAddLatency(Compile compile)
{
    Graph graph;
    const auto input = graph.addInput();
    const auto pack = graph.addPack(std::same_as<typename Sample::value_type, float>
                                    ? SamplePrecision::Float32 : SamplePrecision::Float64);
    const auto twice = graph.addGain(2);
    graph.connect(pack, twice);
    graph.connect(input, pack);
    for (std::size_t lane = 0; lane < 3; ++lane) {
        const auto unpack = graph.addUnpack(lane);
        graph.connect(twice, unpack);
        graph.connect(unpack, pack);
    }
    auto runtime = (graph.*compile)(std::array{twice});
    using Scalar = typename Sample::value_type;
    const auto impulse = std::same_as<Scalar, float> ? Scalar{1} : static_cast<Scalar>(1.0 + 0x1p-40);
    const std::array source{Sample{impulse}, Sample{-impulse}, Sample{}};
    std::array<Sample, source.size()> rendered{};
    check(runtime.process(std::array{std::span<const Sample>{source}}, std::array{std::span<Sample>{rendered}}) &&
          rendered == std::array{Sample{2 * impulse, 4 * impulse, 8 * impulse, 16 * impulse},
                                 Sample{-2 * impulse, -4 * impulse, -8 * impulse, -16 * impulse}, Sample{}},
          "Pack and Unpack carry each lane forward on the same sample and preserve its precision");
}

void stageThreeOutputsAdvanceTogether()
{
    const auto late = compileDspCode("streamin in; streamout out; stage(3) { out = in; }");
    const auto early = compileDspCode("streamin in; streamout out; stage(1) { out = in; }");
    constexpr std::array input{2.F, 4.F, 6.F, 8.F, 10.F, 12.F, 14.F};
    constexpr std::array expectedFirst{0.F, 2.F, 4.F, 6.F, 8.F, 10.F, 12.F};
    constexpr std::array expectedSecond{0.F, 0.F, 2.F, 4.F, 6.F, 8.F, 10.F};

    for (auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto inlet = graph.addInput();
        const auto first = graph.addDspCode(late);
        const auto second = graph.addDspCode(late);
        const auto readout = graph.addDspCode(early);
        graph.connectDspInput(inlet, first.id, 0);
        graph.connectDspInput(first.outputs[0], second.id, 0);
        graph.connectDspInput(second.outputs[0], readout.id, 0);
        auto runtime = graph.compile(std::array{first.outputs[0], second.outputs[0], readout.outputs[0]});

        for (std::size_t blockSize : {1, 3, 7}) {
            runtime.reset();
            std::array<float, input.size()> firstSamples{}, secondSamples{}, earlySamples{};
            for (std::size_t frame = 0; frame < input.size(); frame += blockSize) {
                const auto size = std::min(blockSize, input.size() - frame);
                check(runtime.process(std::array{InputBuffer{input}.subspan(frame, size)},
                                      std::array{OutputBuffer{firstSamples}.subspan(frame, size),
                                                 OutputBuffer{secondSamples}.subspan(frame, size),
                                                 OutputBuffer{earlySamples}.subspan(frame, size)}),
                      "Render chained Stage 3 outputs");
            }
            check(firstSamples == expectedFirst && secondSamples == expectedSecond && earlySamples == expectedSecond,
                  "Stage 3 outlets each delay one sample, including when read by Stage 1");
        }
    }
}

void assemblerRunsPackedStagesAcrossBufferSizes()
{
    const auto program = compileDspCode(R"asm(streamin input;
streamout output;
streamout delayed;
float factor = 0.5;
float state;
float initial = {8, 6, 4, 2};
stage(0) {
    movaps xmm0, initial;
    movaps state, xmm0;
}
stage(1) {
    movaps xmm0, state;
    movaps delayed, xmm0;
}
MOVAPS XMM0, input;
MULPS XMM0,factor;
MOVAPS output,xmm0;
stage(3) {
    movaps xmm0,output;
    movaps state,xmm0;
}
)asm", DspLanguage::Assembler);
    check(program.inputs().size() == 1 && program.inputs()[0].name == "input" &&
          program.outputs().size() == 2 && program.outputs()[1].name == "delayed" &&
          !program.outputDependsOnInput(1, 0) && program.outputDependsOnInput(0, 0),
          "Assembler uses FlowStone-style ports and annotates dependencies by stage");

    constexpr std::array input{Mono4{2, -4, 8, -16}, Mono4{1, 3, 5, 7}, Mono4{}, Mono4{3, -2, 7, -4}, Mono4{}};
    constexpr std::array output{Mono4{1, -2, 4, -8}, Mono4{.5, 1.5, 2.5, 3.5}, Mono4{}, Mono4{1.5, -1, 3.5, -2}, Mono4{}};
    constexpr std::array delayed{Mono4{8, 6, 4, 2}, output[0], output[1], output[2], output[3]};
    for (auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto inlet = graph.addInput();
        const auto node = graph.addDspCode(program);
        graph.connectDspInput(inlet, node.id, 0);
        auto runtime = graph.compileMono4(node.outputs);
        for (std::size_t block : {1, 2, 5}) {
            runtime.reset();
            std::array<Mono4, input.size()> samples{}, history{};
            for (std::size_t n = 0; n < input.size(); n += block) {
                const auto size = std::min(block, input.size() - n);
                check(runtime.process(std::array{Mono4InputBuffer{input}.subspan(n, size)},
                                      std::array{Mono4OutputBuffer{samples}.subspan(n, size),
                                                 Mono4OutputBuffer{history}.subspan(n, size)}),
                      "Process Assembler with each backend");
            }
            check(samples == output && history == delayed, "Assembler gain and stored state survive all block sizes");
        }
    }
}

void assemblerPreservesPackedBits()
{
    const auto program = compileDspCode(R"asm(
        streamout shuffled;
        streamout filtered;
        streamout chosen;
        streamout converted;
        float a = {1, -0, 0x7fc01234, 8};
        float b = {4, 0, 6, 0x7fc05678};
        float one = 1;
        float fractions = {1.5, -2.5, 7.75, -1.2};
        int increment = 1;

        movaps xmm0,a;
        shufps xmm0,b,0x27;
        movaps shuffled,xmm0;

        movaps xmm1,a;
        cmpps xmm1,b,3;
        andps xmm1,one;
        movaps filtered,xmm1;

        movaps xmm2,b;
        minps xmm2,a;
        movaps chosen,xmm2;

        cvtps2dq xmm3,fractions;
        paddd xmm3,increment;
        cvtdq2ps xmm3,xmm3;
        movaps converted,xmm3;

        pxor xmm15,xmm15;
        andnps xmm15,one;
    )asm", DspLanguage::Assembler);

    for (auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto node = graph.addDspCode(program);
        auto runtime = graph.compileMono4(node.outputs);
        std::array<Mono4, 1> shuffled{}, filtered{}, chosen{}, converted{};
        check(runtime.process({}, std::array{Mono4OutputBuffer{shuffled}, Mono4OutputBuffer{filtered},
                                             Mono4OutputBuffer{chosen}, Mono4OutputBuffer{converted}}),
              "Render Assembler bit patterns");
        check(shuffled[0] == Mono4{8, -0.0F, 6, 4} && std::signbit(shuffled[0][1]),
              "SHUFPS selects two lanes from the destination and two from the source");
        check(filtered[0] == Mono4{0, 0, 1, 1}, "CMPPS creates a raw mask that ANDPS can apply");
        check(chosen[0][0] == 1 && std::signbit(chosen[0][1]) &&
              std::bit_cast<std::uint32_t>(chosen[0][2]) == 0x7fc01234U && chosen[0][3] == 8,
              "MINPS keeps the second operand bits on equal or unordered lanes");
        check(converted[0] == Mono4{3, -1, 9, 0}, "Packed integer operations and SSE conversions preserve register bits");
    }
}

void assemblerCmpTestsRawWords()
{
    const auto program = compileDspCode(R"asm(
        streamin in;
        streamout out;
        float equal = 1;
        float different = 2;
        movaps xmm0,equal;
        movaps out,xmm0;
        cmp in,-2147483648;
        jz done;
        movaps xmm0,different;
        movaps out,xmm0;
        done:
    )asm", DspLanguage::Assembler);
    for (auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto block = graph.addDspCode(program);
        graph.connectDspInput(graph.addInput(), block.id, 0);
        auto runtime = graph.compile(block.outputs);
        const std::array input{-0.0F, 0.0F, 1.F, -0.0F};
        std::array<float, input.size()> output{};
        check(runtime.process(std::array{InputBuffer{input}}, std::array{OutputBuffer{output}}) &&
              output == std::array{1.F, 2.F, 2.F, 1.F},
              "CMP uses raw integer bits, so JZ distinguishes negative zero from positive zero");
    }
}

void assemblerRunsIntegerControlFlow()
{
    const auto program = compileDspCode(R"asm(
        streamin in;
        streamout out;
        mov eax,in;
        mov ebx,1;
        add eax,ebx;
        jnz nonzero;
        jmp finish;
        nonzero:
        cmp eax,0;
        jl negative;
        jg positive;
        jmp finish;
        negative:
        mov ecx,3;
        jmp classified;
        positive:
        mov ecx,7;
        classified:
        cmp eax,0;
        jng nonpositive;
        mov edx,4;
        jmp adjust;
        nonpositive:
        mov edx,2;
        adjust:
        cmp eax,0;
        jnl transform;
        add ecx,1;
        transform:
        push ecx;
        pop eax;
        inc eax;
        and eax,7;
        or eax,16;
        shl eax,2;
        shr eax,1;
        sub eax,edx;
        call update;
        mov out,eax;
        ret;
        update:
        add eax,8;
        ret;
        finish:
        mov out,10;
    )asm", DspLanguage::Assembler);
    constexpr std::array input{-1.0F, std::bit_cast<float>(0xffffffffU), 0.0F, 1.0F, 2.0F};
    constexpr std::array<std::uint32_t, input.size()> expected{48, 10, 36, 36, 36};
    for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto block = graph.addDspCode(program);
        graph.connectDspInput(graph.addInput(), block.id, 0);
        for (const auto frames : {1U, 3U, 5U}) {
            auto runtime = graph.compile(block.outputs);
            std::array<float, input.size()> output{};
            for (std::size_t offset = 0; offset < input.size(); offset += frames) {
                const auto count = std::min<std::size_t>(frames, input.size() - offset);
                check(runtime.process(std::array{InputBuffer{std::span(input).subspan(offset, count)}},
                                      std::array{OutputBuffer{std::span(output).subspan(offset, count)}}),
                      "Render integer Assembler control flow across buffer boundaries");
            }
            check(std::bit_cast<decltype(expected)>(output) == expected,
                  "Integer registers, branches, and local CALL use the current sample's raw words");
        }
    }
}

void assemblerReadsMonotonicTimestamp()
{
    const auto program = compileDspCode(R"asm(
        streamout low;
        streamout midLow;
        streamout midHigh;
        streamout high;
        rdtsc;
        mov ebx,eax;
        mov ecx,edx;
        and eax,65535;
        shr ebx,16;
        and ecx,65535;
        shr edx,16;
        movd xmm0,eax;
        movd xmm1,ebx;
        movd xmm2,ecx;
        movd xmm3,edx;
        movss low,xmm0;
        movss midLow,xmm1;
        movss midHigh,xmm2;
        movss high,xmm3;
    )asm", DspLanguage::Assembler);
    for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto block = graph.addDspCode(program);
        auto runtime = graph.compile(block.outputs);
        std::array<float, 8> low{}, midLow{}, midHigh{}, high{};
        const auto start = std::chrono::steady_clock::now().time_since_epoch().count();
        check(runtime.process({}, std::array{OutputBuffer{low}, OutputBuffer{midLow},
                                             OutputBuffer{midHigh}, OutputBuffer{high}}), "Render RDTSC");
        const auto end = std::chrono::steady_clock::now().time_since_epoch().count();
        auto previous = static_cast<std::uint64_t>(start);
        for (std::size_t i = 0; i < low.size(); ++i) {
            const auto timestamp = std::bit_cast<std::uint32_t>(low[i]) |
                                   (std::uint64_t{std::bit_cast<std::uint32_t>(midLow[i])} << 16) |
                                   (std::uint64_t{std::bit_cast<std::uint32_t>(midHigh[i])} << 32) |
                                   (std::uint64_t{std::bit_cast<std::uint32_t>(high[i])} << 48);
            check(timestamp >= previous && timestamp <= static_cast<std::uint64_t>(end),
                  "RDTSC writes both words of a monotonic host timestamp");
            previous = timestamp;
        }
    }
}

void assemblerConvertsAndMovesSimdLanes()
{
    const auto program = compileDspCode(R"asm(
        streamdoubleout fromInt;
        streamdoubleout fromFloat;
        streamout fromDouble;
        streamout scalar;
        streamout copied;
        streamout word;
        streamout product;
        streamout shifted;
        streamout reciprocal;
        int integers = {-3, 16777217, 12, 13};
        float floats = {0.5, -2, 3, 4};
        double doubles = {1.000000059604644775390625, -2, 3, 4};
        int lhs = {0x80000003, 5, 0x80000021, 7};
        int rhs = {4, 99, 2, 99};
        float divisor = {4, -2, 0, 0x80000000};
        cvtdq2pd xmm0,integers;
        movapd fromInt,xmm0;
        cvtps2pd xmm1,floats;
        movapd fromFloat,xmm1;
        cvtpd2ps xmm2,doubles;
        movaps fromDouble,xmm2;
        movss xmm3,floats;
        movaps scalar,xmm3;
        movaps xmm4,floats;
        movss xmm4,xmm2;
        movaps copied,xmm4;
        movss copied,xmm3;
        mov eax,0x40400000;
        movd xmm5,eax;
        movaps word,xmm5;
        movaps xmm6,lhs;
        pmuludq xmm6,rhs;
        movaps product,xmm6;
        pslld xmm6,2;
        psrld xmm6,1;
        movaps shifted,xmm6;
        rcpps xmm7,divisor;
        movaps reciprocal,xmm7;
    )asm", DspLanguage::Assembler);
    for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto block = graph.addDspCode(program);
        auto runtime = graph.compileMonoDouble4(block.outputs);
        std::array<std::array<MonoDouble4, 1>, 9> samples{};
        std::array<MonoDouble4OutputBuffer, samples.size()> outputs{};
        for (std::size_t i = 0; i < samples.size(); ++i) outputs[i] = MonoDouble4OutputBuffer{samples[i]};
        check(runtime.process({}, outputs), "Render packed Assembler conversions and moves");
        check(samples[0][0] == MonoDouble4{-3, 16777217, 0, 0}, "CVTDQ2PD widens two signed integer lanes");
        check(samples[1][0] == MonoDouble4{0.5, -2, 0, 0}, "CVTPS2PD widens two float lanes");
        check(samples[2][0] == MonoDouble4{1, -2, 0, 0}, "CVTPD2PS rounds two doubles and clears upper lanes");
        check(samples[3][0] == MonoDouble4{0.5, 0, 0, 0} && samples[4][0] == MonoDouble4{0.5, -2, 3, 4},
              "MOVSS clears or preserves the upper lanes according to its operands");
        check(samples[5][0] == MonoDouble4{3, 0, 0, 0}, "MOVD transfers one raw word and clears upper lanes");
        const auto bits = [](const MonoDouble4& sample) {
            std::array<std::uint32_t, 4> lanes{};
            std::ranges::transform(sample, lanes.begin(), [](double lane) {
                return std::bit_cast<std::uint32_t>(static_cast<float>(lane));
            });
            return lanes;
        };
        check(bits(samples[6][0]) == std::array{12U, 2U, 66U, 1U}, "PMULUDQ packs two unsigned 64-bit products");
        check(bits(samples[7][0]) == std::array{24U, 4U, 132U, 2U}, "PSLLD and PSRLD shift each packed word");
        constexpr auto inf = std::numeric_limits<double>::infinity();
        check(samples[8][0] == MonoDouble4{0.25, -0.5, inf, -inf}, "RCPPS preserves the sign of reciprocals");
    }
}

void assemblerPreservesRawFloatBitsAndIsolatesHostEnvironment()
{
    using namespace openstone::stream;
    struct Restore {
        std::fenv_t saved;
        Restore() { std::fegetenv(&saved); }
        ~Restore() { std::fesetenv(&saved); }
    } restore;
    const auto program = compileDspCode(R"(
        streamin in;
        streamin raw;
        streamin rounding;
        streamout sum;
        streamout identity;
        streamout rounded;
        float halfUlp = 0x33800000;
        movaps xmm0,in;
        addps xmm0,halfUlp;
        movaps sum,xmm0;
        movaps xmm1,raw;
        movaps identity,xmm1;
        cvtps2dq xmm2,rounding;
        movaps rounded,xmm2;
    )", DspLanguage::Assembler);
    const std::array<Mono4, 1> input{{{std::bit_cast<float>(0x3f800000U), std::bit_cast<float>(0x3f800001U),
                                      std::bit_cast<float>(0xbf800000U), std::bit_cast<float>(0xbf800001U)}}};
    const std::array<Mono4, 1> raw{{{std::bit_cast<float>(0x00000001U), std::bit_cast<float>(0x807fffffU),
                                    std::bit_cast<float>(0x7f812345U), std::bit_cast<float>(0xffc23456U)}}};
    const std::array<Mono4, 1> rounding{{{2.5F, 3.5F, -2.5F, -3.5F}}};
    for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto block = graph.addDspCode(program);
        for (std::size_t port = 0; port < 3; ++port) graph.connectDspInput(graph.addInput(), block.id, port);
        auto runtime = graph.compileMono4(block.outputs);
        for (const auto mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
            runtime.reset();
            check(std::fesetround(mode) == 0, "Set the caller's rounding mode");
#if defined(__aarch64__) || defined(__arm64__)
            std::uint64_t control;
            __asm__ volatile("mrs %0, fpcr" : "=r"(control));
            control |= (std::uint64_t{1} << 24) | (std::uint64_t{1} << 25); // Host flush/default-NaN modes.
            __asm__ volatile("msr fpcr, %0" : : "r"(control));
#elif defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
            const auto control = _mm_getcsr() | 0x8040U;
            _mm_setcsr(control);
#endif
            std::feclearexcept(FE_ALL_EXCEPT);
            std::feraiseexcept(FE_DIVBYZERO);
            const auto flags = std::fetestexcept(FE_ALL_EXCEPT);
            std::array<Mono4, 1> sum{}, identity{}, rounded{};
            check(runtime.process(std::array{Mono4InputBuffer{input}, Mono4InputBuffer{raw}, Mono4InputBuffer{rounding}},
                std::array{Mono4OutputBuffer{sum}, Mono4OutputBuffer{identity}, Mono4OutputBuffer{rounded}}), "Render with hostile host floating-point settings");
            check(std::fegetround() == mode && std::fetestexcept(FE_ALL_EXCEPT) == flags, "DSP restores the caller's rounding and exception flags");
#if defined(__aarch64__) || defined(__arm64__)
            std::uint64_t after;
            __asm__ volatile("mrs %0, fpcr" : "=r"(after));
            check(after == control, "DSP restores the host's ARM flush and default-NaN controls");
#elif defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
            check((_mm_getcsr() & ~0x3fU) == (control & ~0x3fU), "DSP restores the host's SSE controls");
#endif
            constexpr std::array expected{0x3f800000U, 0x3f800002U, 0xbf7fffffU, 0xbf800000U};
            constexpr std::array integers{2U, 4U, 0xfffffffeU, 0xfffffffcU};
            for (std::size_t lane = 0; lane < 4; ++lane) {
                check(std::bit_cast<std::uint32_t>(sum[0][lane]) == expected[lane], "SSE addition rounds ties to even independently of the caller");
                check(std::bit_cast<std::uint32_t>(identity[0][lane]) == std::bit_cast<std::uint32_t>(raw[0][lane]), "MOVAPS preserves raw subnormal and signaling-NaN words at graph boundaries");
                check(std::bit_cast<std::uint32_t>(rounded[0][lane]) == integers[lane], "CVTPS2DQ rounds positive and negative ties to even");
            }
        }
    }
}

void assemblerRunsX87MathAndStores()
{
    const auto program = compileDspCode(R"asm(
        streamout sine;
        streamout cosine;
        streamout pairCos;
        streamout pairSin;
        streamout unit;
        streamout tangent;
        streamout rem;
        streamout remAgain;
        streamout nearest;
        double angle = 0.75;
        double tie = 2.5;
        int dividend = -7;
        int modulus = 4;
        int saved;
        int savedAgain;
        int rounded;
        dfld angle;
        fsin;
        fst sine;
        fstp st(0);
        dfld angle;
        fcos;
        fstp cosine;
        dfld angle;
        fsincos;
        fstp pairCos;
        fstp pairSin;
        dfld angle;
        fptan;
        fstp unit;
        fstp tangent;
        fild modulus;
        fild dividend;
        fprem;
        fist saved;
        fistp savedAgain;
        fstp st(0);
        dfld tie;
        fistp rounded;
        cvtdq2ps xmm0,saved;
        movaps rem,xmm0;
        cvtdq2ps xmm0,savedAgain;
        movaps remAgain,xmm0;
        cvtdq2ps xmm0,rounded;
        movaps nearest,xmm0;
    )asm", DspLanguage::Assembler);
    for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto block = graph.addDspCode(program);
        auto runtime = graph.compile(block.outputs);
        std::array<std::array<float, 1>, 9> samples{};
        std::array<OutputBuffer, samples.size()> outputs{};
        for (std::size_t i = 0; i < samples.size(); ++i) outputs[i] = OutputBuffer{samples[i]};
        check(runtime.process({}, outputs), "Render x87 trigonometry and integer stores");
        const auto sine = static_cast<float>(std::sin(0.75));
        const auto cosine = static_cast<float>(std::cos(0.75));
        check(samples[0][0] == sine && samples[1][0] == cosine &&
              samples[2][0] == cosine && samples[3][0] == sine &&
              samples[4][0] == 1.0F && samples[5][0] == static_cast<float>(std::tan(0.75)),
              "x87 trigonometry uses radians and leaves FSINCOS and FPTAN results in stack order");
        check(samples[6][0] == -3.0F && samples[7][0] == -3.0F && samples[8][0] == 2.0F,
              "FPREM and integer x87 stores preserve the remainder and round ties to even");
    }
}

void assemblerKeepsExtendedX87Precision()
{
    struct Case { const char* name; const char* body; int result; };
    // result: 0=n, 1=3*n, 2=4*n, 3=nearest even multiple of 2,
    // 4=n/2, 5=n*2, 6=(2^63+n)%5, 7=-(2^63+n)%5, 8=rounded n+0.5.
    constexpr Case cases[]{
        {"extended local call", "dfld big63; call extended; jmp finished; extended: dfld in; fadd; ret; finished: dfld big63; fsub; dfstp out;", 0},
        {"53-bit cancellation", "dfld big53; dfld in; fadd; dfld big53; fsub; dfstp out;", 0},
        {"64-bit significand", "dfld big63; dfld in; fadd; dfld big63; fsub; dfstp out;", 0},
        {"64-bit precision boundary", "dfld big64; dfld in; fadd; dfld big64; fsub; dfstp out;", 3},
        {"extended multiplication", "dfld big53; dfld in; fadd; dfld three; fmul; dfld threeBig53; fsub; dfstp out;", 1},
        {"division and recovery", "dfld big63; dfld in; fadd; dfld two; fdivp; dfld two; fmul; dfld big63; fsub; dfstp out;", 0},
        {"double division alias", "dfld big63; dfld in; fadd; dfld two; dfdivp; dfld two; fmul; dfld big63; fsub; dfstp out;", 0},
        {"temporary overflow", "dfld in; dfld huge; fmul; dfld two; fmul; dfld half; fmul; dfld huge; fdivp; dfstp out;", 0},
        {"temporary underflow", "dfld in; dfld tiny; fmul; dfld tiny; fmul; dfld huge; fmul; dfld huge; fmul; dfstp out;", 2},
        {"non-popping double store", "dfld big53; dfld in; fadd; dfst dmem; dfld big53; fsub; dfstp out;", 0},
        {"rounded double memory", "dfld big53; dfld in; fadd; dfstp dmem; dfld dmem; dfld big53; fsub; dfstp out;", 3},
        {"non-popping float store", "dfld big24; dfld in; fadd; fst fmem; dfld big24; fsub; dfstp out;", 0},
        {"rounded float memory", "dfld big24; dfld in; fadd; fstp fmem; fld fmem; dfld big24; fsub; dfstp out;", 3},
        {"stack copy and exchange", "dfld big63; dfld in; fadd; fld1; fxch st(1); fld st(0); fstp st(1); fxch st(1); fstp st(0); dfld big63; fsub; dfstp out;", 0},
        {"integral rounding", "dfld big53; dfld in; fadd; dfld half; fadd; frndint; dfld big53; fsub; dfstp out;", 8},
        {"integer store rounding", "dfld in; dfld half; fadd; fistp imem; fild imem; dfstp out;", 8},
        {"extended scale and recovery", "dfld up; dfld in; fscale; dfld down; fxch st(1); fscale; dfstp out; fstp st(0); fstp st(0);", 0},
        {"fractional scale", "dfld fraction; dfld in; fscale; dfstp out; fstp st(0);", 5},
        {"negative fractional scale", "dfld negFraction; dfld in; fscale; dfstp out; fstp st(0);", 4},
        {"positive remainder", "dfld five; dfld big63; dfld in; fadd; fprem; dfstp out; fstp st(0);", 6},
        {"negative remainder", "dfld five; dfld big63; dfld in; fadd; dfld negOne; fmul; fprem; dfstp out; fstp st(0);", 7},
        {"scale overflow", "dfld upHuge; dfld in; fscale; dfstp out; fstp st(0);", 9},
        {"scale underflow", "dfld downHuge; dfld in; fscale; dfstp out; fstp st(0);", 10},
        {"signed scale underflow", "dfld downHuge; dfld in; dfld negOne; fmul; fscale; dfstp out; fstp st(0);", 11},
        {"minimum extended subnormal recovery", "dfld minimumScale; dfld in; fscale; dfld undoMinimum; fxch st(1); fscale; dfstp out; fstp st(0); fstp st(0);", 0},
        {"large scale from extended tiny", "dfld lowerEdge; dfld in; fscale; dfld upHuge; fxch st(1); fscale; dfld lowerEdge; fxch st(1); fscale; dfstp out; fstp st(0); fstp st(0); fstp st(0);", 9},
        {"large scale from extended huge", "dfld upperEdge; dfld in; fscale; dfld downHuge; fxch st(1); fscale; dfld upperEdge; fxch st(1); fscale; dfstp out; fstp st(0); fstp st(0); fstp st(0);", 10},
        {"invalid infinite scale", "dfld infinity; dfld zero; fscale; dfstp out; fstp st(0);", 12},
    };
    const std::string declarations = R"(
        streamdoublein in; streamdoubleout out;
        double big24=0x4170000000000000;
        double big53=0x4340000000000000;
        double big63=0x43e0000000000000;
        double big64=0x43f0000000000000;
        double threeBig53=0x4358000000000000;
        double huge=0x7fe0000000000000;
        double tiny=0x0010000000000000;
        double two=2; double three=3; double five=5; double half=0.5; double negOne=-1;
        double up=1024; double down=-1024; double fraction=1.75; double negFraction=-1.75;
        double upHuge=50000; double downHuge=-50000; double zero=0; double infinity=0x7ff0000000000000;
        double lowerEdge=-16390; double upperEdge=16376; double minimumScale=-16445; double undoMinimum=16445;
        double dmem; float fmem; int imem;
    )";
    for (const auto& test : cases) for (const bool packed : {false, true}) {
        std::string source = declarations;
        for (std::size_t lane = 0; lane < (packed ? 4 : 1); ++lane) {
            std::string body = test.body;
            const auto replace = [&](std::string_view name, std::string_view replacement) {
                for (std::size_t at = 0; (at = body.find(name, at)) != std::string::npos; at += replacement.size()) body.replace(at, name.size(), replacement);
            };
            replace("extended", "extended" + std::to_string(lane));
            replace("finished", "finished" + std::to_string(lane));
            replace("dfld in;", "dfld in[" + std::to_string(lane) + "];");
            replace("dfstp out;", "dfstp out[" + std::to_string(lane) + "];");
            source += body;
        }
        const auto program = compileDspCode(source, DspLanguage::Assembler);
        for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);
            const auto block = graph.addDspCode(program);
            graph.connectDspInput(graph.addInput(), block.id, 0);
            const auto checkSamples = [&]<class Sample>(auto runtime, auto set, auto get, std::size_t lanes) {
                for (const std::size_t size : {1, 7, 64}) {
                    runtime.reset();
                    std::array<Sample, 64> input{}, output{};
                    for (std::size_t frame = 0; frame < 64; ++frame) for (std::size_t lane = 0; lane < lanes; ++lane) set(input[frame], lane, 1 + (frame + 17 * lane) % 64);
                    for (std::size_t frame = 0; frame < 64; frame += size) {
                        const auto count = std::min(size, 64 - frame);
                        check(runtime.process(std::array{std::span<const Sample>{input}.subspan(frame, count)},
                            std::array{std::span<Sample>{output}.subspan(frame, count)}), "Render 80-bit x87 arithmetic");
                    }
                    for (std::size_t frame = 0; frame < 64; ++frame) for (std::size_t lane = 0; lane < lanes; ++lane) {
                        const int n = 1 + (frame + 17 * lane) % 64;
                        const int even = 2 * (n / 2 + (n % 2 && (n / 2) % 2));
                        const std::array expected{double(n), 3.0*n, 4.0*n, double(even), n*0.5, n*2.0,
                            double((3+n)%5), -double((3+n)%5), double(n+(n%2)), std::numeric_limits<double>::infinity(),
                            0.0, -0.0, std::bit_cast<double>(0xfff8000000000000ULL)};
                        if (std::bit_cast<std::uint64_t>(get(output[frame], lane)) != std::bit_cast<std::uint64_t>(expected[test.result])) throw std::runtime_error(std::string(test.name) +
                            " failed on " + (backend == DspBackend::LlvmOrc ? "LLVM ORC" : "interpreter") + ", sample " + std::to_string(frame) +
                            ", lane " + std::to_string(lane) + ", buffer " + std::to_string(size) + ", actual " + std::to_string(get(output[frame], lane)));
                    }
                }
                if (backend == DspBackend::LlvmOrc) check(runtime.compiledDspCode().find("openstone_dsp_x87_") != std::string::npos,
                    "LLVM ORC emits native x87 software-arithmetic calls");
            };
            if (packed) checkSamples.template operator()<MonoDouble4>(graph.compileMonoDouble4(block.outputs),
                [](auto& sample, std::size_t lane, double value) { sample[lane] = value; }, [](const auto& sample, std::size_t lane) { return sample[lane]; }, 4);
            else checkSamples.template operator()<double>(graph.compileMonoDouble(block.outputs),
                [](auto& sample, std::size_t, double value) { sample = value; }, [](const auto& sample, std::size_t) { return sample; }, 1);
        }
    }
}

void assemblerStoresDoubleX87ValuesThroughMemReferences()
{
    const auto program = compileDspCode(R"(
        memrefin mem;
        streamdoubleout preserved; streamdoubleout loaded; streamdoubleout popped;
        double big=0x4340000000000000;
        mov eax,mem[0];
        dfld big; fld1; fadd;
        dfst [eax+8];
        dfld big; fsub; dfstp preserved;
        dfld [eax+8]; dfstp loaded;
        dfld big; fld1; fadd; dfstp [eax+16];
        dfld [eax+16]; dfstp popped;
    )", DspLanguage::Assembler);
    for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto block = graph.addDspCode(program);
        for (const std::size_t size : {1, 7, 64}) {
            const openstone::green::Memory memory(std::vector<std::uint8_t>(32, 0xa5));
            auto runtime = graph.compileMonoDouble(block.outputs);
            check(runtime.setDspMemRefInput(block.id, 0, memory), "Bind double x87 memory");
            std::array<std::array<double, 64>, 3> output{};
            for (std::size_t frame = 0; frame < 64; frame += size) {
                const auto count = std::min(size, 64 - frame);
                check(runtime.process({}, std::array{MonoDoubleOutputBuffer{std::span{output[0]}.subspan(frame, count)},
                    MonoDoubleOutputBuffer{std::span{output[1]}.subspan(frame, count)}, MonoDoubleOutputBuffer{std::span{output[2]}.subspan(frame, count)}}), "Render DFST and DFSTP through a Mem reference");
            }
            check(std::ranges::all_of(output[0], [](double x) { return x == 1; }) &&
                std::ranges::all_of(output[1], [](double x) { return x == 0x1p53; }) && output[1] == output[2],
                "Double Mem stores round their copy and retain the extended stack value until popped");
            check(memory.wordAt(8) == 0 && memory.wordAt(12) == 0x43400000U && memory.wordAt(16) == 0 && memory.wordAt(20) == 0x43400000U &&
                memory.wordAt(0) == 0xa5a5a5a5U && memory.wordAt(4) == 0xa5a5a5a5U && memory.wordAt(24) == 0xa5a5a5a5U && memory.wordAt(28) == 0xa5a5a5a5U,
                "Double x87 stores write exactly eight bytes and preserve neighboring Mem cells");
        }
    }
}

void assemblerUsesTheLowQwordForPackedShiftCounts()
{
    const auto program = compileDspCode(R"asm(
        streamin count;
        streamout right;
        streamout left;
        int words = {0x80000008, 0x40000000, 0x40000010, 4};
        int countWords;
        cvtps2dq xmm0,count;
        movaps countWords,xmm0;
        movaps xmm1,words;
        movaps xmm2,words;
        psrld xmm1,xmm0;
        pslld xmm2,countWords;
        movaps right,xmm1;
        movaps left,xmm2;
    )asm", DspLanguage::Assembler);
    constexpr std::array counts{Mono4{0, 0, 0, 0}, Mono4{1, 0, 7, -1}, Mono4{31, 0, 0, 0},
                                Mono4{32, 0, 0, 0}, Mono4{1, 1, 0, 0}, Mono4{-1, 0, 0, 0}};
    constexpr std::array amounts{0U, 1U, 31U, 32U, 32U, 32U};
    constexpr std::array words{0x80000008U, 0x40000000U, 0x40000010U, 4U};
    for (auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto block = graph.addDspCode(program);
        graph.connectDspInput(graph.addInput(), block.id, 0);
        auto runtime = graph.compileMono4(block.outputs);
        std::array<Mono4, counts.size()> left{}, right{};
        check(runtime.process(std::array{Mono4InputBuffer{counts}},
                              std::array{Mono4OutputBuffer{right}, Mono4OutputBuffer{left}}),
              "Render packed shifts with register and memory counts");
        for (std::size_t i = 0; i < counts.size(); ++i) {
            const auto count = amounts[i];
            for (std::size_t lane = 0; lane < words.size(); ++lane) {
                const auto r = count < 32 ? words[lane] >> count : 0U;
                const auto l = count < 32 ? words[lane] << count : 0U;
                check(std::bit_cast<std::uint32_t>(right[i][lane]) == r &&
                      std::bit_cast<std::uint32_t>(left[i][lane]) == l,
                      "Packed shifts broadcast the unsigned low qword count and zero oversized shifts");
            }
        }
    }
}

void assemblerReadsAndWritesIndexedArrayOperands()
{
    const auto program = compileDspCode(R"asm(
        streamin index;
        streamout window;
        streamout head;
        streamout tail;
        streamout scalar;
        streamout adjacent;
        float table[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        float history[8];
        float cap[4] = 100;
        float data = {10, 20, 30, 40};
        float guard = {91, 92, 93, 94};
        float scalarResult;
        cvtps2dq xmm0,index;
        movd eax,xmm0;
        mov ebx,4;
        movaps xmm1,table[eax*4+ebx-4];
        minps xmm1,cap[0];
        movaps window,xmm1;
        movaps history[4*eax],data;
        movaps xmm2,history[0];
        movaps head,xmm2;
        movaps xmm3,history[4];
        movaps tail,xmm3;
        fld table[eax*4];
        fst scalarResult[2];
        fld st[0];
        mov ecx,12;
        fstp scalarResult[ecx];
        fstp st[0];
        movaps xmm4,scalarResult;
        movaps scalar,xmm4;
        movaps xmm5,guard;
        movaps adjacent,xmm5;
    )asm", DspLanguage::Assembler);
    constexpr std::array indices{Mono4{1, 0, 0, 0}, Mono4{-1, 0, 0, 0}, Mono4{7, 0, 0, 0}, Mono4{8, 0, 0, 0}};
    constexpr std::array windows{Mono4{2, 3, 4, 5}, Mono4{}, Mono4{8, 0, 0, 0}, Mono4{}};
    constexpr std::array tails{Mono4{40, 0, 0, 0}, Mono4{40, 0, 0, 0}, Mono4{40, 0, 0, 10}, Mono4{40, 0, 0, 10}};
    for (auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto block = graph.addDspCode(program);
        graph.connectDspInput(graph.addInput(), block.id, 0);
        for (std::size_t blockSize : {1, 3, 4}) {
            auto runtime = graph.compileMono4(block.outputs);
            std::array<Mono4, indices.size()> window{}, head{}, tail{}, scalar{}, adjacent{};
            for (std::size_t offset = 0; offset < indices.size(); offset += blockSize) {
                const auto size = std::min(blockSize, indices.size() - offset);
                const auto buffer = [&](auto& output) { return Mono4OutputBuffer{std::span(output).subspan(offset, size)}; };
                check(runtime.process(std::array{Mono4InputBuffer{std::span(indices).subspan(offset, size)}},
                                      std::array{buffer(window), buffer(head), buffer(tail), buffer(scalar), buffer(adjacent)}),
                      "Render indexed Assembler loads and stores");
            }
            check(window == windows && tail == tails, "Scaled and displaced packed accesses stop at the array boundary");
            for (std::size_t i = 0; i < indices.size(); ++i) {
                check(head[i] == Mono4{0, 10, 20, 30} && adjacent[i] == Mono4{91, 92, 93, 94} &&
                      scalar[i] == Mono4{0, 0, windows[i][0], windows[i][0]},
                      "Indexed stores preserve neighbouring storage and x87 accesses one lane");
            }
        }
    }
}

void assemblerAccessesScalarWordsAndDoubleWindows()
{
    const auto program = compileDspCode(R"asm(
        streamdoubleout result;
        streamout converted;
        double a = {1.0000000000009094947017729282379150390625, 2, 4, 8};
        double b = {0, 4, -2, -4};
        int integers = {7, 11, 13, 17};
        int saved;
        movapd xmm0,a[0];
        addpd xmm0,b[0];
        movapd result[0],xmm0;
        movapd xmm0,a[2];
        addpd xmm0,b[2];
        movapd result[2],xmm0;
        mov eax,integers[1];
        mov saved[2],eax;
        mov ebx,8;
        inc saved[ebx];
        fild saved[ebx];
        fistp saved[3];
        cvtdq2ps xmm1,saved;
        movaps converted,xmm1;
    )asm", DspLanguage::Assembler);
    for (auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto block = graph.addDspCode(program);
        auto runtime = graph.compileMonoDouble4(block.outputs);
        std::array<MonoDouble4, 1> result{}, converted{};
        check(runtime.process({}, std::array{MonoDouble4OutputBuffer{result}, MonoDouble4OutputBuffer{converted}}),
              "Render double windows and scalar word lanes");
        check(result[0] == MonoDouble4{1.0 + 0x1p-40, 6, 2, 4} && converted[0] == MonoDouble4{0, 0, 12, 12},
              "Indexed double moves preserve the other half, and MOV, INC, FILD and FISTP address single words");
    }
}

void assemblerReadsExternalArraysAndUnalignedOffsets()
{
    const auto program = compileDspCode(R"asm(
        memin wave[5];
        floatarrayin levels[3];
        streamout window;
        streamout value;
        streamout invalid;
        mov eax,8;
        movaps xmm0,wave[eax];
        movaps window,xmm0;
        fld levels[1];
        fstp value[0];
        mov eax,3;
        movaps xmm0,wave[eax];
        movaps invalid,xmm0;
    )asm", DspLanguage::Assembler);
    for (auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto block = graph.addDspCode(program);
        auto runtime = graph.compileMono4(block.outputs);
        std::array<Mono4, 1> window{}, value{}, invalid{};
        const std::array outputs{Mono4OutputBuffer{window}, Mono4OutputBuffer{value}, Mono4OutputBuffer{invalid}};
        check(runtime.setDspArrayInput(block.id, 0, std::array{1.F, 2.F, 3.F, 4.F, 5.F}) &&
              runtime.setDspArrayInput(block.id, 1, std::array{0.F, 0.25F}) &&
              runtime.process({}, outputs), "Render Assembler Mem and FloatArray inputs");
        check(window[0] == Mono4{3, 4, 5, 0} && value[0] == Mono4{0.25F, 0, 0, 0} && invalid[0] == Mono4{},
              "A packed array read zero-fills at the boundary and rejects non-element-aligned byte offsets");
        check(runtime.setDspArrayInput(block.id, 0, std::array{1.F, 2.F, 8.F}) && runtime.process({}, outputs) &&
              window[0] == Mono4{8, 0, 0, 0}, "Updating external data updates indexed reads on the next render");
    }
}

void assemblerBoundsIndirectMemoryToItsConnectedView()
{
    const auto program = compileDspCode(R"asm(
        memrefin mem;
        streamout sample;
        mov eax,mem[0];
        push eax;
        mov eax,123;
        pop ebx;
        mov ecx,1;
        mov [ebx+ecx],0x3f800000;
        mov [ebx-4],0;
        mov [ebx+5],0;
        fld [ebx+ecx];
        fstp sample[0];
    )asm", DspLanguage::Assembler);
    for (auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto block = graph.addDspCode(program);
        auto runtime = graph.compile(block.outputs);
        const openstone::green::Memory memory(std::vector<std::uint8_t>(12, 0xa5));
        const auto view = memory.view(1, 8, 1);
        std::array<float, 1> output{};
        const std::array outputs{OutputBuffer{output}};
        check(runtime.setDspMemRefInput(block.id, 0, view) && runtime.process({}, outputs) && output[0] == 1.F,
              "An x86 register preserves its Mem reference across push and pop");
        auto expected = std::vector<std::uint8_t>(12, 0xa5);
        expected[2] = expected[3] = 0;
        expected[4] = 0x80;
        expected[5] = 0x3f;
        check(memory.snapshotBytes() == expected, "Indirect writes keep neighbouring bytes untouched at both ends of a Mem view");
        check(runtime.setDspMemRefInput(block.id, 0, {}) && runtime.process({}, outputs) && output[0] == 0.F &&
              memory.snapshotBytes() == expected, "An empty Mem reads zero and leaves the previous buffer untouched");
    }
}

void assemblerCallsLocalSubroutineThroughRegisters()
{
    const auto program = compileDspCode(R"asm(
        streamin in;
        streamout out;
        float half=0.5;
        float one=1;
        movaps xmm0,in;
        mov eax,adjust;
        push eax;
        mov eax,0;
        pop ebx;
        call ebx;
        mov eax,ebx;
        call eax;
        addps xmm0,one;
        movaps out,xmm0;
        jmp done;
        adjust:
        mulps xmm0,half;
        ret;
        done:
    )asm", DspLanguage::Assembler);
    constexpr std::array input{-4.F, -2.F, 0.F, 8.F};
    constexpr std::array expected{0.F, 0.5F, 1.F, 3.F};
    for (auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto block = graph.addDspCode(program);
        graph.connectDspInput(graph.addInput(), block.id, 0);
        auto runtime = graph.compile(block.outputs);
        std::array<float, input.size()> output{};
        check(runtime.process(std::array{InputBuffer{input}}, std::array{OutputBuffer{output}}) && output == expected,
              "A saved or copied call register selects the local body and RET resumes the caller");
    }
}

void assemblerBranchesToLocalReturnByDisplacement()
{
    const auto program = compileDspCode(R"asm(
        streamout out;
        int count;
        mov eax,adjust;
        mov edx,0;
        call eax;
        mov count,edx;
        cvtdq2ps xmm0,count;
        movaps out,xmm0;
        jmp done;
        adjust:
        mov ecx,3;
        body:
        add edx,2;
        sub ecx,1;
        jnz -18;
        jmp 0;
        ret;
        done:
    )asm", DspLanguage::Assembler);
    for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto block = graph.addDspCode(program);
        auto runtime = graph.compile(block.outputs);
        std::array<float, 2> output{};
        check(runtime.process({}, std::array{OutputBuffer{output}}) && output == std::array{6.F, 6.F},
              "Local numeric branches loop and jump to the return instruction");
    }
}

void assemblerLimitsBackwardJumpsPerSample()
{
    const std::string source = R"asm(
        streamout out;
        int counter;
        cvtdq2ps xmm0,counter;
        movaps out,xmm0;
        mov eax,counter;
        jmp enter;
        body:
        enter:
        add eax,1;
        mov counter,eax;
    )asm";
    for (const auto jump : {"jmp -17;", "jmp body;"}) {
        const auto program = compileDspCode(source + jump, DspLanguage::Assembler);
        for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);
            const auto node = graph.addDspCode(program);
            auto runtime = graph.compile(node.outputs);
            std::array<float, 3> output{};
            check(runtime.process({}, std::array{OutputBuffer{output}}) && output == std::array{0.F, 4097.F, 8194.F},
                  "Numeric and label jumps exhaust the backward branch budget separately for each sample");
        }
    }
}

void assemblerCallsEarlierLabelWithinSubroutine()
{
    const auto program = compileDspCode(R"asm(
        streamout out;
        float one=1;
        movaps xmm0,one;
        call outer;
        movaps out,xmm0;
        jmp done;
        outer:
        jmp body;
        adjust:
        addps xmm0,one;
        ret;
        body:
        mov eax,adjust;
        call eax;
        addps xmm0,one;
        ret;
        done:
    )asm", DspLanguage::Assembler);
    for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto block = graph.addDspCode(program);
        auto runtime = graph.compile(block.outputs);
        std::array<float, 2> output{};
        check(runtime.process({}, std::array{OutputBuffer{output}}) && output == std::array{3.F, 3.F},
              "An earlier helper skipped inside a subroutine returns to that subroutine and then to the stage");
    }
}

void assemblerBranchesPastAnEarlyReturnByDisplacement()
{
    const auto program = compileDspCode(R"asm(
        streamin in;
        streamout out;
        float one=1;
        float two=2;
        movaps xmm0,in;
        call choose;
        addps xmm0,one;
        movaps out,xmm0;
        jmp done;
        choose:
        cmp in,0;
        jnz 1;
        ret;
        addps xmm0,two;
        ret;
        done:
    )asm", DspLanguage::Assembler);
    constexpr std::array input{0.F, 1.F, -4.F};
    constexpr std::array expected{1.F, 4.F, -1.F};
    for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto block = graph.addDspCode(program);
        graph.connectDspInput(graph.addInput(), block.id, 0);
        auto runtime = graph.compile(block.outputs);
        std::array<float, input.size()> output{};
        check(runtime.process(std::array{InputBuffer{input}}, std::array{OutputBuffer{output}}) && output == expected,
              "Both local returns resume the caller when a byte displacement skips the early return");
    }
}

void assemblerBoundsRecursiveSubroutineCalls()
{
    const std::string caller = R"asm(
        streamin in;
        streamout out;
        float overflow=-1;
        movaps xmm0,overflow;
        movaps out,xmm0;
        cvttps2dq xmm1,in;
        movd edx,xmm1;
        call descend;
        movd xmm0,ebx;
        cvtdq2ps xmm0,xmm0;
        movaps out,xmm0;
        jmp done;
        descend:
        cmp edx,0;
        jz base;
        sub edx,1;
    )asm";
    constexpr std::string_view unwind = R"asm(
        add ebx,1;
        jmp returned;
        base:
        mov ebx,0;
        returned:
        ret;
    )asm";
    for (const bool mutual : {false, true}) {
        const auto source = caller + (mutual ? "call step;" : "call descend;") + std::string(unwind) +
                            (mutual ? "step: call descend; ret;" : "") + "done:";
        const auto program = compileDspCode(source, DspLanguage::Assembler);
        const float boundary = mutual ? 63.F : 127.F;
        const std::array input{boundary, boundary + 1.F, 2.F, 0.F};
        const std::array expected{boundary, -1.F, 2.F, 0.F};
        for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);
            const auto block = graph.addDspCode(program);
            graph.connectDspInput(graph.addInput(), block.id, 0);
            auto runtime = graph.compile(block.outputs);
            std::array<float, 4> output{};
            check(runtime.process(std::array{InputBuffer{input}}, std::array{OutputBuffer{output}}) && output == expected,
                  "Recursion returns base-case values, aborts only the overflowing sample, and recovers");
        }
    }
}

void assemblerBoundsBranchingRecursionWork()
{
    const auto program = compileDspCode(R"asm(
        streamin in;
        streamout out;
        float overflow=-1;
        float one=1;
        movaps xmm0,overflow;
        movaps out,xmm0;
        cvttps2dq xmm1,in;
        movd edx,xmm1;
        call fork;
        movaps xmm0,one;
        movaps out,xmm0;
        jmp done;
        fork:
        cmp edx,0;
        jz returned;
        push edx;
        sub edx,1;
        call fork;
        pop edx;
        sub edx,1;
        call fork;
        returned:
        ret;
        done:
    )asm", DspLanguage::Assembler);
    // Depth 11 requires 4,094 calls after the inlined root. Depth 12 exceeds the per-sample budget.
    constexpr std::array input{11.F, 12.F, 2.F};
    constexpr std::array expected{1.F, -1.F, 1.F};
    for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto block = graph.addDspCode(program);
        graph.connectDspInput(graph.addInput(), block.id, 0);
        auto runtime = graph.compile(block.outputs);
        std::array<float, input.size()> output{};
        check(runtime.process(std::array{InputBuffer{input}}, std::array{OutputBuffer{output}}) && output == expected,
              "Branching recursion stops when the per-sample call budget runs out and recovers on the next sample");
    }
}

void assemblerTailJumpsReuseTheCallerFrame()
{
    const auto cycle = compileDspCode(R"asm(
        streamout out;
        mov edx,300;
        mov ebx,0;
        mov eax,second;
        call first;
        movd xmm0,ebx;
        cvtdq2ps xmm0,xmm0;
        movaps out,xmm0;
        jmp done;
        first:
        cmp edx,0;
        jnz advance;
        ret;
        advance:
        add ebx,1;
        sub edx,1;
        jmp second;
        second:
        jmp first;
        done:
    )asm", DspLanguage::Assembler);
    const auto recursive = compileDspCode(R"asm(
        streamout out;
        jmp main;
        base:
        mov ebx,0;
        ret;
        walk:
        cmp edx,0;
        jnz descend;
        jmp base;
        descend:
        sub edx,1;
        call walk;
        add ebx,1;
        ret;
        main:
        mov edx,5;
        call walk;
        movd xmm0,ebx;
        cvtdq2ps xmm0,xmm0;
        movaps out,xmm0;
    )asm", DspLanguage::Assembler);
    const auto sharedTarget = compileDspCode(R"asm(
        streamout out;
        float one=1;
        jmp main;
        first:
        jmp common;
        common:
        addps xmm0,one;
        ret;
        second:
        jmp common;
        main:
        movaps xmm0,one;
        call first;
        call second;
        movaps out,xmm0;
    )asm", DspLanguage::Assembler);
    for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        const std::array cases{std::pair{cycle, 300.F}, std::pair{recursive, 5.F}, std::pair{sharedTarget, 3.F}};
        for (const auto& [program, expected] : cases) {
            Graph graph(backend);
            const auto block = graph.addDspCode(program);
            auto runtime = graph.compile(block.outputs);
            std::array<float, 2> output{};
            check(runtime.process({}, std::array{OutputBuffer{output}}) && output == std::array{expected, expected},
                  "Tail-jump cycles, recursive frames, and shared targets all resume the original caller");
        }
    }
}

void assemblerRegisterTailJumpsKeepTheCaller()
{
    const auto cycle = compileDspCode(R"asm(
        streamout out;
        mov edx,300;
        mov ebx,0;
        mov eax,second;
        mov ecx,first;
        call first;
        movd xmm0,ebx;
        cvtdq2ps xmm0,xmm0;
        movaps out,xmm0;
        jmp done;
        first:
        cmp edx,0;
        jnz advance;
        ret;
        advance:
        add ebx,1;
        sub edx,1;
        jmp eax;
        second:
        jmp ecx;
        done:
    )asm", DspLanguage::Assembler);
    const auto savedTarget = compileDspCode(R"asm(
        streamout out;
        float one=1;
        float two=2;
        mov eax,target;
        push eax;
        mov eax,0;
        pop ebx;
        movaps xmm0,one;
        call source;
        addps xmm0,two;
        movaps out,xmm0;
        jmp done;
        source:
        addps xmm0,one;
        jmp ebx;
        target:
        mulps xmm0,two;
        ret;
        done:
    )asm", DspLanguage::Assembler);
    for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        const std::array cases{std::pair{cycle, 300.F}, std::pair{savedTarget, 6.F}};
        for (const auto& [program, expected] : cases) {
            Graph graph(backend);
            const auto block = graph.addDspCode(program);
            auto runtime = graph.compile(block.outputs);
            std::array<float, 2> output{};
            check(runtime.process({}, std::array{OutputBuffer{output}}) && output == std::array{expected, expected},
                  "Register tail jumps reuse a call frame and preserve targets copied through push and pop");
        }
    }
}

void assemblerNumericJumpSkipsARegisterTailJump()
{
    const auto program = compileDspCode(R"asm(
        streamin in;
        streamout out;
        float one=1;
        float two=2;
        movaps xmm0,in;
        mov eax,target;
        call source;
        addps xmm0,one;
        movaps out,xmm0;
        jmp done;
        source:
        cmp in,0;
        jnz 2;
        jmp eax;
        addps xmm0,two;
        ret;
        target:
        addps xmm0,one;
        ret;
        done:
    )asm", DspLanguage::Assembler);
    constexpr std::array input{0.F, 1.F, -4.F};
    constexpr std::array expected{2.F, 4.F, -1.F};
    for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto block = graph.addDspCode(program);
        graph.connectDspInput(graph.addInput(), block.id, 0);
        auto runtime = graph.compile(block.outputs);
        std::array<float, input.size()> output{};
        check(runtime.process(std::array{InputBuffer{input}}, std::array{OutputBuffer{output}}) && output == expected,
              "A numeric displacement uses the two-byte register JMP width and retains both return paths");
    }
}

void assemblerRuntimeTargetsTravelThroughStackAndReturns()
{
    const auto program = compileDspCode(R"asm(
        streamin in;
        streamout out;
        cmp in,0;
        jl chooseA;
        mov eax,assignB;
        jmp invoke;
        chooseA:
        mov eax,assignA;
        invoke:
        push eax;
        mov eax,0;
        pop ecx;
        call ecx;
        call ebx;
        movd xmm0,edx;
        cvtdq2ps xmm0,xmm0;
        movaps out,xmm0;
        jmp done;
        assignA:
        mov ebx,first;
        ret;
        assignB:
        mov ebx,second;
        ret;
        first:
        mov edx,11;
        ret;
        second:
        mov edx,22;
        ret;
        done:
    )asm", DspLanguage::Assembler);
    constexpr std::array input{-1.F, 1.F, 0.F, -2.F};
    constexpr std::array expected{11.F, 22.F, 22.F, 11.F};
    for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto block = graph.addDspCode(program);
        graph.connectDspInput(graph.addInput(), block.id, 0);
        auto runtime = graph.compile(block.outputs);
        std::array<float, input.size()> output{};
        check(runtime.process(std::array{InputBuffer{input}}, std::array{OutputBuffer{output}}) && output == expected,
              "Selected labels survive push/pop and different return paths can produce the next runtime target");
    }
}

void assemblerRuntimeTargetsMergeThreePaths()
{
    const std::string source = R"asm(
        streamin in;
        streamout out;
        movaps xmm0,in;
        call choose;
        movaps out,xmm0;
        jmp done;
        choose:
        cmp in,0;
        jl useA;
        jz useB;
        mov eax,runC;
        push eax;
        jmp invoke;
        useA:
        mov eax,runA;
        push eax;
        jmp invoke;
        useB:
        mov eax,runB;
        push eax;
        invoke:
        pop ebx;
    )asm";
    const std::string routines = R"asm(
        runA:
        mov edx,1;
        movd xmm0,edx;
        cvtdq2ps xmm0,xmm0;
        ret;
        runB:
        mov edx,2;
        movd xmm0,edx;
        cvtdq2ps xmm0,xmm0;
        ret;
        runC:
        mov edx,3;
        movd xmm0,edx;
        cvtdq2ps xmm0,xmm0;
        ret;
        done:
    )asm";
    constexpr std::array input{-2.F, 0.F, 1.F};
    constexpr std::array expected{1.F, 2.F, 3.F};
    for (const auto& transfer : {"call ebx; ret;", "jmp ebx;"}) {
        const auto program = compileDspCode(source + transfer + routines, DspLanguage::Assembler);
        for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);
            const auto block = graph.addDspCode(program);
            graph.connectDspInput(graph.addInput(), block.id, 0);
            auto runtime = graph.compile(block.outputs);
            std::array<float, input.size()> output{};
            check(runtime.process(std::array{InputBuffer{input}}, std::array{OutputBuffer{output}}) && output == expected,
                  "Three different labels merge in a saved stack entry and retain their runtime call or jump behavior");
        }
    }
}

void assemblerReloadsTargetsFromLocalMemoryCells()
{
    const auto program = compileDspCode(R"asm(
        streamout out;
        float cells;
        int table[2];
        mov eax,first;
        mov cells[1],eax;
        mov cells[0],7;
        pxor xmm0,xmm0;
        movss cells,xmm0;
        mov eax,cells[1];
        mov table[0],eax;
        mov eax,second;
        mov table[1],eax;
        mov eax,table[0];
        call eax;
        mov ecx,edx;
        mov eax,table[1];
        call eax;
        add edx,ecx;
        movd xmm0,edx;
        cvtdq2ps xmm0,xmm0;
        movaps out,xmm0;
        jmp done;
        first:
        mov edx,11;
        ret;
        second:
        mov edx,22;
        ret;
        done:
    )asm", DspLanguage::Assembler);
    for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto block = graph.addDspCode(program);
        auto runtime = graph.compile(block.outputs);
        std::array<float, 2> output{};
        check(runtime.process({}, std::array{OutputBuffer{output}}) && output == std::array{33.F, 33.F},
              "Saved target lanes survive writes to neighbouring cells and can be copied into a local target table");
    }
}

void assemblerIndexesSavedTargetTables()
{
    const std::string source = R"asm(
        streamin in;
        streamout out;
        float failed=-1;
        int targets[2];
        movaps xmm0,failed;
        movaps out,xmm0;
        mov eax,first;
        mov targets[0],eax;
        mov eax,second;
        mov targets[1],eax;
        cvttps2dq xmm1,in;
        movd ebx,xmm1;
        shl ebx,2;
        call choose;
        movd xmm0,edx;
        cvtdq2ps xmm0,xmm0;
        movaps out,xmm0;
        jmp done;
        choose:
        mov eax,targets[ebx];
    )asm";
    const std::string routines = R"asm(
        first:
        mov edx,11;
        ret;
        second:
        mov edx,22;
        ret;
        done:
    )asm";
    constexpr std::array input{0.F, 1.F, 2.F, -1.F, 0.F};
    constexpr std::array expected{11.F, 22.F, -1.F, -1.F, 11.F};
    for (const auto& transfer : {"call eax; ret;", "jmp eax;"}) {
        const auto program = compileDspCode(source + transfer + routines, DspLanguage::Assembler);
        for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);
            const auto block = graph.addDspCode(program);
            graph.connectDspInput(graph.addInput(), block.id, 0);
            auto runtime = graph.compile(block.outputs);
            std::array<float, input.size()> output{};
            check(runtime.process(std::array{InputBuffer{input}}, std::array{OutputBuffer{output}}) && output == expected,
                  "Computed target-table loads dispatch calls and tail jumps, stop invalid indices, and recover");
        }
    }
}

void assemblerMemoryTransfersPreserveAddressRegisters()
{
    const auto program = compileDspCode(R"asm(
        memrefin buffer;
        streamout out;
        mov eax,buffer[0];
        mov ebx,first;
        mov [eax],ebx;
        add eax,8;
        mov ebx,second;
        mov [eax],ebx;
        sub eax,8;
        push eax;
        mov eax,0;
        pop ecx;
        jmp 2;
        call [ecx];
        call [ecx];
        add ecx,8;
        call [ecx];
        call outer;
        movd xmm0,edx;
        cvtdq2ps xmm0,xmm0;
        movaps out,xmm0;
        jmp done;
        outer:
        jmp [ecx];
        first:
        mov edx,11;
        ret;
        second:
        add edx,22;
        ret;
        done:
    )asm", DspLanguage::Assembler);
    for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto block = graph.addDspCode(program);
        auto runtime = graph.compile(block.outputs);
        const openstone::green::Memory memory(std::vector<std::uint8_t>(16));
        check(runtime.setDspMemRefInput(block.id, 0, memory), "Bind an indirect target table");
        std::array<float, 2> output{};
        check(runtime.process({}, std::array{OutputBuffer{output}}) && output == std::array{55.F, 55.F},
              "Memory calls and tail jumps preserve copied address registers and handle pointer offsets and byte displacements");
    }
}

void assemblerMemoryTransfersRecoverAfterShortBuffers()
{
    const std::string source = R"asm(
        memrefin buffer;
        streamout out;
        float failed=-1;
        movaps xmm0,failed;
        movaps out,xmm0;
        mov eax,buffer[0];
        add eax,4;
        mov ebx,leaf;
        mov [eax],ebx;
        call outer;
        movd xmm0,edx;
        cvtdq2ps xmm0,xmm0;
        movaps out,xmm0;
        jmp done;
        outer:
    )asm";
    const std::string leaf = "leaf: mov edx,7; ret; done:";
    for (const auto& transfer : {"call [eax]; ret;", "jmp [eax];"}) {
        const auto program = compileDspCode(source + transfer + leaf, DspLanguage::Assembler);
        for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);
            const auto block = graph.addDspCode(program);
            auto runtime = graph.compile(block.outputs);
            for (const std::size_t bytes : {8, 0, 4, 8}) {
                const openstone::green::Memory memory(std::vector<std::uint8_t>(bytes, 0));
                check(runtime.setDspMemRefInput(block.id, 0, memory), "Replace the target Mem input");
                std::array<float, 1> output{};
                check(runtime.process({}, std::array{OutputBuffer{output}}) && output[0] == (bytes == 8 ? 7.F : -1.F),
                      "An unavailable indirect target ends that sample's stage and a valid buffer restores the transfer");
            }
        }
    }
}

void assemblerMemPointerArithmeticLoopsRemainUsable()
{
    const auto program = compileDspCode(R"asm(
        memrefin buffer;
        streamout out;
        mov eax,buffer[0];
        mov edx,3;
        loop_start:
        mov [eax],7;
        add eax,4;
        sub edx,1;
        jnz loop_start;
        sub eax,4;
        fld [eax];
        fstp out[0];
    )asm", DspLanguage::Assembler);
    for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto block = graph.addDspCode(program);
        auto runtime = graph.compile(block.outputs);
        const openstone::green::Memory memory(std::vector<std::uint8_t>(12));
        check(runtime.setDspMemRefInput(block.id, 0, memory), "Bind a Mem for an ordinary pointer loop");
        std::array<float, 1> output{};
        check(runtime.process({}, std::array{OutputBuffer{output}}) && memory.word(0) == 7 &&
              memory.word(1) == 7 && memory.word(2) == 7, "Tracking target offsets preserves numeric pointer-walking loops");
    }
}

void assemblerIndexedTransfersValidateTableCells()
{
    const std::string source = R"asm(
        streamin in;
        memrefin buffer;
        streamout out;
        float failed=-1;
        movaps xmm0,failed;
        movaps out,xmm0;
        mov eax,buffer[0];
        mov ecx,first;
        mov [eax],ecx;
        mov ecx,second;
        mov [eax+8],ecx;
        mov ecx,third;
        mov [eax+16],ecx;
        cvttps2dq xmm1,in;
        movd ebx,xmm1;
        call choose;
        movd xmm0,edx;
        cvtdq2ps xmm0,xmm0;
        movaps out,xmm0;
        mov [eax+24],ebx;
        jmp done;
        choose:
    )asm";
    const std::string routines = R"asm(
        first:
        mov edx,11;
        ret;
        second:
        mov edx,22;
        ret;
        third:
        mov edx,33;
        ret;
        done:
    )asm";
    constexpr std::array input{0.F, 2.F, 4.F, 1.F, 3.F, 8.F, -1.F, 2.F};
    constexpr std::array expected{11.F, 22.F, 33.F, -1.F, -1.F, -1.F, -1.F, 22.F};
    for (const auto& transfer : {"call [eax+ebx*4]; ret;", "jmp [eax+ebx*4];",
                                 "mov ecx,[eax+ebx*4]; call ecx; ret;"}) {
        const auto program = compileDspCode(source + transfer + routines, DspLanguage::Assembler);
        for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);
            const auto block = graph.addDspCode(program);
            graph.connectDspInput(graph.addInput(), block.id, 0);
            auto runtime = graph.compile(block.outputs);
            const openstone::green::Memory memory(std::vector<std::uint8_t>(64));
            check(runtime.setDspMemRefInput(block.id, 1, memory), "Bind an indexed target table");
            std::array<float, 1> primer{};
            check(runtime.process(std::array{InputBuffer{std::span{input}.first(1)}},
                                  std::array{OutputBuffer{primer}}), "Initialize the target table");
            // These holes contain valid target identifiers at runtime, but have never been saved as target cells.
            memory.writeWordAt(4, memory.wordAt(0));
            memory.writeWordAt(12, memory.wordAt(8));
            std::array<float, input.size()> output{};
            check(runtime.process(std::array{InputBuffer{input}}, std::array{OutputBuffer{output}}) && output == expected &&
                  memory.word(6) == 2U,
                  "Indexed transfers validate the selected cell, preserve both registers, and recover after invalid offsets");
            const openstone::green::Memory shortMemory(std::vector<std::uint8_t>(4));
            check(runtime.setDspMemRefInput(block.id, 1, shortMemory), "Replace the table with a short Mem");
            std::array<float, 1> last{};
            check(runtime.process(std::array{InputBuffer{std::span{input}.last(1)}},
                                  std::array{OutputBuffer{last}}) && last[0] == -1.F,
                  "An indexed target outside the current Mem ends only that sample's stage");
            check(runtime.setDspMemRefInput(block.id, 1, memory) &&
                  runtime.process(std::array{InputBuffer{std::span{input}.last(1)}},
                                  std::array{OutputBuffer{last}}) && last[0] == 22.F,
                  "Indexed target transfers recover when a valid Mem is rebound");
        }
    }
}

void assemblerIndexedWritesRetainAndInvalidateTargets()
{
    const std::string setup = R"asm(
        streamin index;
        streamin mode;
        memrefin buffer;
        memrefin alias;
        streamout out;
        float failed=-1;
        float eleven=11;
        float twentyTwo=22;
        movaps xmm0,failed;
        movaps out,xmm0;
        mov eax,buffer[0];
        cvttps2dq xmm1,index;
        movd ebx,xmm1;
        cvttps2dq xmm1,mode;
        movd edx,xmm1;
        cmp edx,0;
        jz invoke;
        cmp edx,2;
        jz numeric;
        cmp edx,3;
        jz aliased;
        cmp edx,0;
        jl chooseB;
        mov ecx,first;
        jmp save;
        chooseB:
        mov ecx,second;
        save:
        mov [eax+ebx*4],ecx;
        jmp invoke;
        numeric:
        mov ecx,123;
        mov [eax+ebx*4],ecx;
        jmp invoke;
        aliased:
        mov edx,alias[0];
        mov ecx,123;
        mov [edx+ebx*4],ecx;
        invoke:
        call choose;
        movaps out,xmm0;
        jmp done;
        choose:
    )asm";
    const std::string routines = R"asm(
        first:
        movaps xmm0,eleven;
        ret;
        second:
        movaps xmm0,twentyTwo;
        ret;
        done:
    )asm";
    constexpr std::array indices{0.F, 2.F, 0.F, 2.F, 2.F, 2.F, 1.F, 32.F, -1.F, 0.F, 0.F, 0.F, 0.F, 0.F};
    constexpr std::array modes{1.F, -1.F, 0.F, 0.F, 1.F, 0.F, 0.F, 1.F, -1.F, 2.F, 1.F, 3.F, -1.F, 0.F};
    constexpr std::array expected{11.F, 22.F, 11.F, 22.F, 11.F, 11.F, -1.F, -1.F, -1.F, -1.F, 11.F, -1.F, 22.F, 22.F};
    for (const auto& transfer : {"call [eax+ebx*4]; ret;", "jmp [eax+ebx*4];",
                                 "mov ecx,[eax+ebx*4]; call ecx; ret;"}) {
        const auto program = compileDspCode(setup + transfer + routines, DspLanguage::Assembler);
        for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);
            const auto block = graph.addDspCode(program);
            graph.connectDspInput(graph.addInput(), block.id, 0);
            graph.connectDspInput(graph.addInput(), block.id, 1);
            for (const std::size_t size : {1, 3, 7, 14}) {
                auto runtime = graph.compile(block.outputs);
                const openstone::green::Memory memory(std::vector<std::uint8_t>(64));
                check(runtime.setDspMemRefInput(block.id, 2, memory) &&
                      runtime.setDspMemRefInput(block.id, 3, memory), "Bind indexed target storage and an aliased input");
                std::array<float, indices.size()> output{};
                for (std::size_t frame = 0; frame < indices.size(); frame += size) {
                    const auto count = std::min(size, indices.size() - frame);
                    check(runtime.setDspMemRefInput(block.id, 2, memory), "Rebinding the same Mem retains saved entries");
                    check(runtime.process(std::array{InputBuffer{std::span{indices}.subspan(frame, count)},
                                                     InputBuffer{std::span{modes}.subspan(frame, count)}},
                                          std::array{OutputBuffer{std::span{output}.subspan(frame, count)}}),
                          "Process indexed writes and transfers");
                }
                for (std::size_t frame = 0; frame < output.size(); ++frame) {
                    if (output[frame] != expected[frame]) throw std::runtime_error(std::string(transfer) +
                        " indexed write mismatch at " + std::to_string(frame) + " buffer " + std::to_string(size) +
                        " actual " + std::to_string(output[frame]) + " expected " + std::to_string(expected[frame]));
                }
                const openstone::green::Memory replacement(memory.snapshotBytes());
                check(runtime.setDspMemRefInput(block.id, 2, replacement), "Bind new storage containing copied identifiers");
                const std::array<float, 1> zero{0.F};
                std::array<float, 1> last{};
                check(runtime.process(std::array{InputBuffer{zero}, InputBuffer{zero}}, std::array{OutputBuffer{last}}) && last[0] == -1.F,
                      "Copied numeric identifiers do not inherit the old Mem's saved target provenance");
            }
        }
    }
}

void assemblerIndexedWritesInitializeTableInLoop()
{
    const auto program = compileDspCode(R"asm(
        streamin index;
        memrefin buffer;
        streamout out;
        float failed=-1;
        float eleven=11;
        float twentyTwo=22;
        movaps xmm0,failed;
        movaps out,xmm0;
        mov eax,buffer[0];
        mov ebx,0;
        fill:
        cmp ebx,1;
        jz chooseB;
        mov ecx,first;
        jmp save;
        chooseB:
        mov ecx,second;
        save:
        mov [eax+ebx*4+1],ecx;
        inc ebx;
        cmp ebx,3;
        jnz fill;
        cvttps2dq xmm1,index;
        movd ebx,xmm1;
        call [eax+ebx*4+1];
        movaps out,xmm0;
        jmp done;
        first:
        movaps xmm0,eleven;
        ret;
        second:
        movaps xmm0,twentyTwo;
        ret;
        done:
    )asm", DspLanguage::Assembler);
    constexpr std::array indices{0.F, 1.F, 2.F, 3.F, -1.F, 1.F};
    constexpr std::array expected{11.F, 22.F, 11.F, -1.F, -1.F, 22.F};
    for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto block = graph.addDspCode(program);
        graph.connectDspInput(graph.addInput(), block.id, 0);
        auto runtime = graph.compile(block.outputs);
        const openstone::green::Memory memory(std::vector<std::uint8_t>(32));
        check(runtime.setDspMemRefInput(block.id, 1, memory), "Bind a dynamically initialized unaligned table");
        std::array<float, indices.size()> output{};
        check(runtime.process(std::array{InputBuffer{indices}}, std::array{OutputBuffer{output}}) && output == expected,
              "A counted loop can initialize different targets in every indexed cell, including unaligned cells");
    }
}

void assemblerCopiesTargetsBetweenIndexedCells()
{
    const std::string setup = R"asm(
        streamin sourceIndex;
        streamin destinationIndex;
        streamin mode;
        streamin readIndex;
        memrefin buffer;
        streamout out;
        float failed=-1;
        float eleven=11;
        float twentyTwo=22;
        float thirtyThree=33;
        movaps xmm0,failed;
        movaps out,xmm0;
        mov eax,buffer[0];
        mov ecx,first;
        mov [eax],ecx;
        mov ecx,second;
        mov [eax+8],ecx;
        mov ecx,third;
        mov [eax+16],ecx;
        cvttps2dq xmm1,sourceIndex;
        movd ebx,xmm1;
        cvttps2dq xmm1,destinationIndex;
        movd edx,xmm1;
        cvttps2dq xmm1,mode;
        movd ecx,xmm1;
        cmp ecx,0;
        jz invoke;
        cmp ecx,3;
        jz numeric;
        mov ecx,[eax+ebx*4];
        mov [eax+edx*4],ecx;
        cvttps2dq xmm1,mode;
        movd ecx,xmm1;
        cmp ecx,2;
        jz invoke;
        mov ecx,0;
        mov [eax+ebx*4],ecx;
        jmp invoke;
        numeric:
        mov ecx,123;
        mov [eax+edx*4],ecx;
        invoke:
        cvttps2dq xmm1,readIndex;
        movd ebx,xmm1;
        call choose;
        movaps out,xmm0;
        jmp done;
        choose:
    )asm";
    const std::string routines = R"asm(
        first:
        movaps xmm0,eleven;
        ret;
        second:
        movaps xmm0,twentyTwo;
        ret;
        third:
        movaps xmm0,thirtyThree;
        ret;
        done:
    )asm";
    // The last nine frames chain 6 -> 8 -> 10 -> 12 -> 14, then check that only the last entry remains.
    constexpr std::array sources{0.F, 2.F, 4.F, 0.F, 0.F, 4.F, 0.F, 1.F, 0.F, 0.F, 0.F, 0.F, 2.F, -1.F, 0.F, 0.F, 2.F, 0.F,
        6.F, 8.F, 10.F, 12.F, 0.F, 0.F, 0.F, 0.F, 0.F};
    constexpr std::array destinations{6.F, 8.F, 10.F, 6.F, 6.F, 6.F, 0.F, 6.F, 6.F, 16.F, 6.F, 6.F, 6.F, 8.F, 8.F, 8.F, -1.F, 6.F,
        8.F, 10.F, 12.F, 14.F, 6.F, 8.F, 10.F, 12.F, 14.F};
    constexpr std::array modes{1.F, 1.F, 1.F, 0.F, 0.F, 1.F, 2.F, 1.F, 0.F, 1.F, 0.F, 3.F, 1.F, 1.F, 0.F, 1.F, 1.F, 0.F,
        1.F, 1.F, 1.F, 1.F, 0.F, 0.F, 0.F, 0.F, 0.F};
    constexpr std::array reads{6.F, 8.F, 10.F, 8.F, 10.F, 6.F, 0.F, 6.F, 6.F, 16.F, 6.F, 6.F, 6.F, 8.F, 8.F, 8.F, -1.F, 6.F,
        8.F, 10.F, 12.F, 14.F, 6.F, 8.F, 10.F, 12.F, 14.F};
    constexpr std::array expected{11.F, 22.F, 33.F, 22.F, 33.F, 33.F, 11.F, -1.F, 33.F, -1.F, 33.F, -1.F, 22.F, -1.F, 22.F, 11.F, -1.F, 22.F,
        22.F, 22.F, 22.F, 22.F, -1.F, -1.F, -1.F, -1.F, 22.F};
    for (const auto& transfer : {"call [eax+ebx*4]; ret;", "jmp [eax+ebx*4];",
                                 "mov ecx,[eax+ebx*4]; call ecx; ret;"}) {
        const auto program = compileDspCode(setup + transfer + routines, DspLanguage::Assembler);
        for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);
            const auto block = graph.addDspCode(program);
            for (std::size_t port = 0; port < 4; ++port) graph.connectDspInput(graph.addInput(), block.id, port);
            for (const std::size_t size : {1, 3, 7, 27}) {
                auto runtime = graph.compile(block.outputs);
                const openstone::green::Memory memory(std::vector<std::uint8_t>(64));
                check(runtime.setDspMemRefInput(block.id, 4, memory), "Bind a Mem for indexed target copies");
                std::array<float, sources.size()> output{};
                for (std::size_t frame = 0; frame < sources.size(); frame += size) {
                    const auto count = std::min(size, sources.size() - frame);
                    check(runtime.setDspMemRefInput(block.id, 4, memory), "Retain copied targets when rebinding the same Mem");
                    check(runtime.process(std::array{InputBuffer{std::span{sources}.subspan(frame, count)},
                                                     InputBuffer{std::span{destinations}.subspan(frame, count)},
                                                     InputBuffer{std::span{modes}.subspan(frame, count)},
                                                     InputBuffer{std::span{reads}.subspan(frame, count)}},
                                          std::array{OutputBuffer{std::span{output}.subspan(frame, count)}}),
                          "Render calls through copied indexed targets");
                }
                for (std::size_t frame = 0; frame < output.size(); ++frame) {
                    if (output[frame] != expected[frame]) throw std::runtime_error(std::string(transfer) +
                        " indexed copy mismatch at " + std::to_string(frame) + " buffer " + std::to_string(size) +
                        " actual " + std::to_string(output[frame]) + " expected " + std::to_string(expected[frame]));
                }
            }
        }
    }
}

void assemblerSwapsIndexedTargetsAcrossBuffers()
{
    const std::string setup = R"asm(
        streamin left;
        streamin right;
        streamin swap;
        streamin read;
        memrefin buffer;
        streamout out;
        float failed=-1;
        float eleven=11;
        float twentyTwo=22;
        float thirtyThree=33;
        int ready;
        int rightIndex;
        movaps xmm0,failed;
        movaps out,xmm0;
        mov eax,buffer[0];
        cmp ready,0;
        jnz initialized;
        mov ebx,0;
        mov ecx,first;
        mov [eax+ebx*4],ecx;
        mov ebx,2;
        mov ecx,second;
        mov [eax+ebx*4],ecx;
        mov ebx,4;
        mov ecx,third;
        mov [eax+ebx*4],ecx;
        mov ready,1;
        initialized:
        cvttps2dq xmm1,left;
        movd ebx,xmm1;
        cvttps2dq xmm1,right;
        movd edx,xmm1;
        mov rightIndex,edx;
        cvttps2dq xmm1,swap;
        movd ecx,xmm1;
        cmp ecx,0;
        jz invoke;
        mov ecx,[eax+ebx*4];
        mov edx,[eax+edx*4];
        mov [eax+ebx*4],edx;
        mov ebx,rightIndex;
        mov [eax+ebx*4],ecx;
        invoke:
        mov ecx,0;
        mov edx,0;
        cvttps2dq xmm1,read;
        movd ebx,xmm1;
        call choose;
        movaps out,xmm0;
        jmp done;
        choose:
    )asm";
    const std::string routines = R"asm(
        first:
        movaps xmm0,eleven;
        ret;
        second:
        movaps xmm0,twentyTwo;
        ret;
        third:
        movaps xmm0,thirtyThree;
        ret;
        done:
    )asm";
    constexpr std::array left{0.F, 0.F, 0.F, 0.F, 2.F, 0.F, 0.F, 0.F, 2.F, 0.F, 4.F, 0.F, 1.F, 0.F, 0.F, 0.F, -1.F, 0.F, 4.F, 2.F, 0.F};
    constexpr std::array right{2.F, 0.F, 2.F, 0.F, 4.F, 0.F, 4.F, 0.F, 2.F, 0.F, 0.F, 0.F, 4.F, 0.F, 16.F, 0.F, 2.F, 0.F, 4.F, 4.F, 0.F};
    constexpr std::array swap{1.F, 0.F, 1.F, 0.F, 1.F, 0.F, 1.F, 0.F, 1.F, 0.F, 1.F, 0.F, 1.F, 0.F, 1.F, 0.F, 1.F, 0.F, 1.F, 1.F, 0.F};
    constexpr std::array read{0.F, 2.F, 0.F, 2.F, 2.F, 4.F, 0.F, 4.F, 2.F, 0.F, 4.F, 0.F, 4.F, 4.F, 0.F, 0.F, 2.F, 2.F, 4.F, 2.F, 4.F};
    constexpr std::array expected{22.F, 11.F, 11.F, 22.F, 33.F, 22.F, 22.F, 11.F, 33.F, 22.F, 22.F, 11.F, -1.F, 22.F, -1.F, 11.F, -1.F, 33.F, 22.F, 22.F, 33.F};
    for (const auto& transfer : {"call [eax+ebx*4]; ret;", "jmp [eax+ebx*4];",
                                 "mov ecx,[eax+ebx*4]; call ecx; ret;"}) {
        const auto program = compileDspCode(setup + transfer + routines, DspLanguage::Assembler);
        for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);
            const auto block = graph.addDspCode(program);
            for (std::size_t port = 0; port < 4; ++port) graph.connectDspInput(graph.addInput(), block.id, port);
            for (const std::size_t size : {1, 3, 7, 21}) {
                auto runtime = graph.compile(block.outputs);
                const openstone::green::Memory memory(std::vector<std::uint8_t>(64));
                check(runtime.setDspMemRefInput(block.id, 4, memory), "Bind a persistent target table for indexed swaps");
                std::array<float, left.size()> output{};
                for (std::size_t frame = 0; frame < left.size(); frame += size) {
                    const auto count = std::min(size, left.size() - frame);
                    check(runtime.setDspMemRefInput(block.id, 4, memory), "Retain swapped entries when rebinding the same Mem");
                    check(runtime.process(std::array{InputBuffer{std::span{left}.subspan(frame, count)},
                                                     InputBuffer{std::span{right}.subspan(frame, count)},
                                                     InputBuffer{std::span{swap}.subspan(frame, count)},
                                                     InputBuffer{std::span{read}.subspan(frame, count)}},
                                          std::array{OutputBuffer{std::span{output}.subspan(frame, count)}}),
                          "Render calls through swapped indexed targets");
                }
                for (std::size_t frame = 0; frame < output.size(); ++frame) {
                    if (output[frame] != expected[frame]) throw std::runtime_error(std::string(transfer) +
                        " indexed swap mismatch at " + std::to_string(frame) + " buffer " + std::to_string(size) +
                        " actual " + std::to_string(output[frame]) + " expected " + std::to_string(expected[frame]));
                }
            }
        }
    }
}

void assemblerMemArrayTablesAndWrites()
{
    using openstone::green::Memory;
    using openstone::green::MemoryArray;
    const Memory mono = Memory::fromSamples(std::array{.25F, .5F, .75F});
    const Memory stereo = Memory::fromSamples(std::array{1.F, -1.F, 2.F, -2.F}, 2);
    const MemoryArray waves({mono, {}, stereo});
    constexpr std::array indices{0.F, 1.F, 2.F, 3.F, -1.F};
    constexpr std::array<std::array<float, 5>, 8> expected{{{1.F, 0.F, 2.F, 0.F, 0.F},
        {12.F, 0.F, 16.F, 0.F, 0.F}, {2.F, 0.F, 1.F, 0.F, 0.F}, {.25F, 0.F, 1.F, 0.F, 0.F}, {3.F, 3.F, 3.F, 3.F, 3.F}, {3.F, 3.F, 3.F, 3.F, 3.F},
        {2.F, 2.F, 2.F, 2.F, 2.F}, {2.F, 2.F, 2.F, 2.F, 2.F}}};
    for (std::size_t test = 0; test < 8; ++test) {
        const auto field = std::array{4, 6, 7, 5, 0, 1, 2, 3}[test];
        std::string source = "memarrayrefin waves; streamin index; streamout out; cvttps2dq xmm0,index; movd ebx,xmm0; mov eax,waves[" +
            std::to_string(field) + "]; ";
        if (field < 4) {
            source = "memarrayrefin waves; streamin index; streamout out; movaps xmm0,waves[" + std::to_string(field) + "]; ";
        } else {
            source += field == 5 ? "mov eax,[eax+ebx*8]; mov ecx,[eax]; " : "mov ecx,[eax+ebx*4]; ";
            source += "movd xmm0,ecx; ";
        }
        if (field < 2 || field == 4 || field == 6) source += "cvtdq2ps xmm0,xmm0; ";
        source += "movaps out,xmm0;";
        const auto program = compileDspCode(source, DspLanguage::Assembler);
        for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);
            const auto block = graph.addDspCode(program);
            graph.connectDspInput(graph.addInput(), block.id, 1);
            auto runtime = graph.compile(block.outputs);
            std::array<float, 5> output{};
            check(runtime.setDspMemArrayInput(block.id, 0, waves) &&
                  runtime.process(std::array{InputBuffer{indices}}, std::array{OutputBuffer{output}}) && output == expected[test],
                  "MemArray tables expose mono, stereo, empty slots, byte sizes, frame limits, and wave pointers");
            check(runtime.setDspMemArrayInput(block.id, 0, {}) &&
                  runtime.process(std::array{InputBuffer{indices}}, std::array{OutputBuffer{output}}) && output == std::array<float, 5>{},
                  "Clearing a MemArray removes its tables and payloads");
            check(runtime.setDspMemArrayInput(block.id, 0, MemoryArray({Memory::fromSamples(std::array{9.F})})),
                  "Bind replacement wave data with a different size and count");
            std::array<float, 1> replacement{};
            const std::array<float, 1> zero{0.F};
            const auto value = std::array{1.F, 4.F, 0.F, 9.F, 1.F, 1.F, 0.F, 0.F}[test];
            check(runtime.process(std::array{InputBuffer{zero}}, std::array{OutputBuffer{replacement}}) && replacement[0] == value,
                  "Rebinding supplies the new tables and keeps their wave storage alive");
        }
    }
    const auto program = compileDspCode(R"asm(
        memarrayrefin waves;
        streamin value;
        streamout out;
        float failed=-99;
        movaps xmm0,failed;
        movaps out,xmm0;
        mov eax,waves[4];
        mov ebx,99;
        mov [eax],ebx;
        mov ebx,[eax];
        cmp ebx,1;
        jnz done;
        mov eax,waves[5];
        mov eax,[eax];
        movaps xmm0,value;
        movss [eax+4],xmm0;
        movss xmm1,[eax+4];
        movaps out,xmm1;
        done:
    )asm", DspLanguage::Assembler);
    for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto block = graph.addDspCode(program);
        graph.connectDspInput(graph.addInput(), block.id, 1);
        auto runtime = graph.compile(block.outputs);
        const auto memory = mono.copy();
        const std::array<float, 3> input{3.F, 4.F, 5.F};
        std::array<float, 3> output{};
        check(runtime.setDspMemArrayInput(block.id, 0, MemoryArray({memory})) &&
              runtime.process(std::array{InputBuffer{input}}, std::array{OutputBuffer{output}}) && output == input &&
              memory.sample(1) == 5.F && memory.channels() == 1,
              "Wave sample writes share Green storage while descriptor tables remain read-only");
    }
}

void assemblerNumericSpansIncludeIndexedTransfers()
{
    const auto program = compileDspCode(R"asm(
        memrefin buffer;
        streamout out;
        mov eax,buffer[0];
        mov ecx,leaf;
        mov [eax],ecx;
        mov ebx,0;
        mov edx,0;
        jmp 3;
        call [eax+ebx*4];
        call [eax+ebx*4];
        call outer;
        movd xmm0,edx;
        cvtdq2ps xmm0,xmm0;
        movaps out,xmm0;
        jmp done;
        outer:
        jmp 3;
        jmp [eax+ebx*4];
        add edx,10;
        ret;
        leaf:
        add edx,1;
        ret;
        done:
    )asm", DspLanguage::Assembler);
    for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto block = graph.addDspCode(program);
        auto runtime = graph.compile(block.outputs);
        const openstone::green::Memory memory(std::vector<std::uint8_t>(4));
        check(runtime.setDspMemRefInput(block.id, 0, memory), "Bind a target for numeric indexed-transfer spans");
        std::array<float, 1> output{};
        check(runtime.process({}, std::array{OutputBuffer{output}}) && output[0] == 11.F,
              "Numeric branch displacements account for the three-byte indexed CALL and JMP forms");
    }
}

void assemblerLimitsNestedCallExpansion()
{
    std::string source = "streamout out; call step0; jmp done; ";
    for (int level = 0; level < 13; ++level) {
        const auto next = "step" + std::to_string(level + 1);
        source += "step" + std::to_string(level) + ": call " + next + "; call " + next + "; ret; ";
    }
    source += "step13: ret; done:";
    bool diagnosed = false;
    try {
        static_cast<void>(compileDspCode(source, DspLanguage::Assembler));
    } catch (const DspCompileError& error) {
        diagnosed = std::string_view(error.what()).contains("too many nested Assembler calls after expansion");
    }
    check(diagnosed, "Nested calls are bounded even when the expanded subroutines contain no instructions");
}

void assemblerRejectsUnsupportedAndInvalidOperands()
{
    struct Error { std::string_view statement, message; };
    constexpr Error cases[]{
        {"cpuid;", "unsupported Assembler instruction 'cpuid'"},
        {"call eax;", "call register must hold a local subroutine label"},
        {"mov eax,5; call eax;", "call register must hold a local subroutine label"},
        {"memrefin mem; mov eax,mem[0]; call eax;", "call register must hold a local subroutine label"},
        {"mov eax,adjust; movaps xmm0,[eax]; jmp done; adjust: ret; done:", "cannot be used as a memory address"},
        {"mov eax,adjust; call eax; jmp done; adjust: push eax; ret; done:", "balance push and pop before returning"},
        {"mov eax,adjust; push eax; call eax; jmp done; adjust: pop eax; ret; done:", "cannot pop the caller's stack"},
        {"cmp in,0; jz useB; mov eax,runA; jmp invoke; useB: mov eax,5; invoke: call eax; "
         "jmp done; runA: ret; runB: ret; done:", "call register must hold a local subroutine label on every path"},
        {"mov eax,adjust; call eax; jmp done; adjust: jmp done; ret; done:",
         "branch target must stay inside this local subroutine"},
        {"mov eax,adjust; call eax; jmp done; adjust: jmp 1; ret; done:",
         "branch target must stay inside this local subroutine"},
        {"mov eax,adjust; call eax; jmp done; adjust: jmp adjust; ret; done:",
         "local subroutine requires a reachable ret"},
        {"cmp in,0; jz main; helper: mov eax,0; main: call helper; ret;", "use call to enter this subroutine"},
        {"stage(1) { helper: mov eax,5; } stage(2) { call helper; }", "call requires a local subroutine in this section"},
        {"call outer; jmp child; outer: call child; ret; child: ret;", "use call to enter this subroutine"},
        {"call outer; jmp done; outer: mov eax,0; push eax; call child; pop eax; ret; "
         "child: pop eax; ret; done:", "cannot pop the caller's stack"},
        {"call outer; jmp done; outer: call child; ret; child: mov eax,0; push eax; ret; done:",
         "balance push and pop before returning"},
        {"mov eax,choose; call eax; jmp done; choose: cmp in,0; jnz alternate; push eax; ret; "
         "alternate: ret; done:", "balance push and pop before returning"},
        {"call choose; mov eax,ebx; jmp done; choose: cmp in,0; jnz alternate; ret; "
         "alternate: mov ebx,1; ret; done:", "before reading it on every path"},
        {"call choose; jmp done; choose: cmp in,0; jnz alternate; fld1; ret; "
         "alternate: ret; done:", "x87 stack depth differs"},
        {"jmp main; leaf: ret; from: mov eax,7; push eax; jmp leaf; main: call from;",
         "balance push and pop before a tail jump"},
        {"call source; jmp done; source: jmp eax; done:", "jump register must hold a local subroutine label"},
        {"call source; jmp done; source: mov eax,5; jmp eax; done:",
         "jump register must hold a local subroutine label"},
        {"memrefin mem; mov eax,mem[0]; call source; jmp done; source: jmp eax; done:",
         "jump register must hold a local subroutine label"},
        {"call source; jmp done; source: cmp in,0; jz useB; mov eax,runA; jmp invoke; "
         "useB: mov eax,5; invoke: jmp eax; runA: ret; runB: ret; done:",
         "jump register must hold a local subroutine label on every path"},
        {"call source; jmp done; source: mov eax,target; push eax; jmp eax; target: ret; done:",
         "balance push and pop before a tail jump"},
        {"mov eax,target; cmp in,0; jnz eax; target: ret;", "only jmp accepts a register target"},
        {"stage(2) { mov eax,local; jmp ready; local: ret; ready: } "
         "stage(2) { call eax; jmp done; local: ret; done: }",
         "register target must be a local label in this section"},
        {"float saved; mov eax,leaf; mov saved,eax; mov saved,0; mov eax,saved; call eax; "
         "jmp done; leaf: ret; done:", "call register must hold a local subroutine label"},
        {"float saved; mov eax,leaf; mov saved,eax; pxor xmm0,xmm0; movaps saved,xmm0; "
         "mov eax,saved; call eax; jmp done; leaf: ret; done:", "call register must hold a local subroutine label"},
        {"float saved; mov eax,leaf; mov saved,eax; fld1; fstp saved; mov eax,saved; call eax; "
         "jmp done; leaf: ret; done:", "call register must hold a local subroutine label"},
        {"float saved; cmp in,0; jz restore; mov eax,leaf; mov saved,eax; restore: mov eax,saved; "
         "call eax; jmp done; leaf: ret; done:", "call register must hold a local subroutine label"},
        {"float saved; mov eax,leaf; mov saved,eax; cmp in,0; jz restore; mov saved,0; restore: "
         "mov eax,saved; call eax; jmp done; leaf: ret; done:", "call register must hold a local subroutine label"},
        {"memrefin mem; mov eax,mem[0]; call [eax];", "memory transfer requires a saved local routine target"},
        {"memrefin mem; mov eax,mem[0]; mov ebx,leaf; mov [eax],ebx; mov [eax+2],0; "
         "call [eax]; jmp done; leaf: ret; done:", "memory transfer requires a saved local routine target"},
        {"memrefin mem; mov eax,mem[0]; cmp in,0; jz invoke; mov ebx,leaf; mov [eax],ebx; "
         "invoke: call [eax]; jmp done; leaf: ret; done:", "memory transfer requires a saved local routine target"},
        {"memrefin mem; mov eax,mem[0]; cmp in,0; jnz [eax];", "only jmp accepts a memory target"},
        {"memrefin mem; mov eax,mem[0]; mov ebx,leaf; mov [eax],ebx; call outer; jmp done; "
         "outer: push ebx; jmp [eax]; leaf: ret; done:", "balance push and pop before a tail jump"},
        {"mov eax,ebx;", "write the integer register before reading"},
        {"pop eax;", "integer stack underflow"},
        {"movaps xmm0,in[eax];", "write the integer register before reading"},
        {"mov eax,0; movaps xmm0,in[eax*3];", "address scale must be 1, 2, 4, or 8"},
        {"memin wave[2]; movaps xmm0,in; movaps wave[0],xmm0;", "cannot assign to a DSP input"},
        {"movaps xmm0,in; psrld xmm0,256;", "packed shift count must fit in an unsigned byte"},
        {"movaps xmm0,[eax];", "write the integer register before reading"},
        {"mov eax,4; movaps xmm0,[eax];", "requires a register loaded from memrefin[0]"},
        {"memrefin mem; mov eax,mem[0]; mov [eax*2],3;", "requires one unscaled Mem reference register"},
        {"memrefin mem; mov eax,mem[0]; mov out,eax;", "store a Mem reference in an integer register"},
        {"movaps out,in;", "requires an XMM register"},
        {"movaps xmm0,in; cmpps xmm0,in,8;", "outside the instruction's range"},
        {"stage(0) { movaps xmm1,in; }", "read stream inputs after stage"},
        {"stage(0) { movaps xmm0,_ONE; movaps in,xmm0; }", "write signal inputs after stage(0)"},
        {"jz done; done:", "requires an earlier cmp"},
        {"cmp in,0; jz missing;", "unknown Assembler label"},
        {"jmp 1; mov eax,edx;", "target falls inside an instruction"},
        {"jmp -4;", "target falls inside an instruction"},
        {"jmp 32; mov eax,5;", "numeric branch displacement leaves this Assembler stage"},
        {"jmp -6;", "numeric branch displacement leaves this Assembler stage"},
        {"jmp 2147483648;", "displacement must fit in signed 32 bits"},
        {"jnz 2; mov eax,edx;", "requires an earlier cmp"},
        {"cmp in,0; jz 0;", "numeric displacements are supported by jmp and jnz"},
        {"mov eax,0; push eax; jmp -6;", "stack state differs at backward branch target"},
        {"loop_start: jnz loop_start;", "requires an earlier cmp"},
        {"mov eax,0; loop_start: push eax; jmp loop_start;", "stack state differs at backward branch target"},
        {"memrefin mem; mov eax,mem[0]; loop_start: mov ecx,[eax]; mov eax,0; jmp loop_start;",
         "integer register state differs at backward branch target"},
        {"call adjust; jmp ready; adjust: ret; ready: jmp adjust;", "use call to enter this subroutine"},
        {"stage(1) { earlier: mov eax,0; } stage(2) { jmp earlier; }", "unknown Assembler label"},
        {"memarrayrefin waves; cmp waves[8],0;", "memarrayrefin fields must be in the range 0 to 7"},
        {"memarrayrefin waves; movaps xmm0,in; movaps waves[2],xmm0;", "cannot assign to a DSP input"},
        {"cmp in,0x100000000;", "Assembler immediate must fit in 32 bits"},
        {"fld in[4];", "lane must be in the range 0 to 3"},
        {"fld st(8);", "x87 stack index must be in the range 0 to 7"},
        {"fld st(0);", "x87 stack underflow"},
        {"fld1; fadd;", "x87 stack underflow"},
        {"fld1; fld1; fld1; fld1; fld1; fld1; fld1; fld1; fld1;", "x87 stack overflow"},
        {"fld1;", "x87 stack must be empty"},
        {"cmp in,0; jz done; fld1; done: fstp out[0];", "x87 stack depth differs"},
        {"memin samples[4]; fld1; fstp samples[2];", "cannot assign to a DSP input"},
        {"double d; movapd xmm0,d; cmppd xmm0,d,8;", "outside the instruction's range"},
    };
    for (const auto [statement, message] : cases) {
        bool diagnosed = false;
        try {
            static_cast<void>(compileDspCode("streamin in; streamout out;\n" + std::string(statement),
                                             DspLanguage::Assembler));
        } catch (const DspCompileError& error) {
            diagnosed = error.line == 2 && error.column > 0 && std::string_view(error.what()).contains(message);
        }
        check(diagnosed, "Invalid assembly reports a diagnostic on the original Assembler source for " +
                         std::string(statement));
    }
}

void assemblerZeroRegisterWorksWithDoubleOperands()
{
    const auto program = compileDspCode(R"asm(
        monoin flag;
        monodoublein signal;
        monodoubleout result;
        pxor xmm0,xmm0;
        cmp flag,0;
        jz add_signal;
        movapd xmm0,signal;
        add_signal:
        addpd xmm0,signal;
        movapd result,xmm0;
    )asm", DspLanguage::Assembler);
    constexpr double value = 1.0 + 0x1p-40;
    for (const auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto block = graph.addDspCode(program);
        graph.connectDspInput(graph.addInput(), block.id, 0);
        graph.connectDspInput(graph.addInput(), block.id, 1);
        auto runtime = graph.compileMonoDouble(block.outputs);
        constexpr std::array flags{0.0, 1.0};
        constexpr std::array inputs{value, value};
        std::array<double, 2> output{};
        check(runtime.process(std::array{MonoDoubleInputBuffer{flags}, MonoDoubleInputBuffer{inputs}},
                              std::array{MonoDoubleOutputBuffer{output}}) && output == std::array{value, 2 * value},
              "A zeroed XMM register works on either branch of a double-precision Assembler calculation");
    }
}

void dspOrcMatchesInterpreterAcrossRenderBlocks()
{
    if (!dspBackendAvailable(DspBackend::LlvmOrc)) {
        return;
    }
    const auto program = compileDspCode(R"(
        streamdoublein input;
        memin memory[5];
        streamdoubleout accumulated;
        streamdoubleout delayed;
        streamdoubleout converted;
        streamdoubleout special;
        double state;
        double table[5] = {1,2,4,8,16};
        float tiny = 0x80000001;
        int ticks;
        int overflow = 0x80000000;
        int offset = {0,1,2,3};
        stage(0) { table[1] += 3; }
        stage(1) { delayed = state; }
        hop(4) {
            loop(3) { ticks += 1; }
        }
        accumulated = state + (memory[offset] | 0x00000000) + table[offset] + double(ticks);
        table[offset] += 0.125;
        converted = double(int(input)) + (double(overflow / -1) + 2147483648.0);
        special = (input == 0) ? double(denorm(tiny)) : sqrt(input);
        stage(3) { state{0} = accumulated * 0.5; }
    )");
    constexpr std::size_t frames = 17;
    std::array<MonoDouble4, frames> values{};
    for (std::size_t i = 0; i < frames; ++i) {
        values[i] = MonoDouble4{static_cast<double>(i) + 0.5, -1.5, 0, 4};
    }
    values[4] = MonoDouble4{2147483647.75, -2147483648.75, 2147483648.0, -2147483649.0};
    values[5] = MonoDouble4{std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN(), -0.0};

    struct Render {
        std::array<std::array<MonoDouble4, frames>, 4> taps{};
    };
    const auto render = [&](DspBackend backend, std::size_t blockSize) {
        DspBlockId id{};
        auto runtime = [&] {
            Graph graph(backend);
            const auto input = graph.addInput();
            const auto block = graph.addDspCode(program);
            graph.connectDspInput(input, block.id, 0);
            id = block.id;
            return graph.compileMonoDouble4(block.outputs);
        }();
        check(runtime.setDspArrayInput(id, 1, std::array{1.F, 3.F, 5.F, 7.F, 9.F}),
              "Could not supply the backend comparison's Mem input");

        Render result;
        for (std::size_t frame = 0; frame < frames; frame += blockSize) {
            const auto size = std::min(blockSize, frames - frame);
            const std::array inputs{MonoDouble4InputBuffer{values}.subspan(frame, size)};
            std::array<MonoDouble4OutputBuffer, 4> outputs;
            for (std::size_t tap = 0; tap < outputs.size(); ++tap) {
                outputs[tap] = MonoDouble4OutputBuffer{result.taps[tap]}.subspan(frame, size);
            }
            check(runtime.process(inputs, outputs), "Could not render a backend comparison block");
        }
        return result;
    };

    const auto expected = render(DspBackend::Interpreter, 1);
    check(std::signbit(expected.taps[3][0][2]) && std::isnan(expected.taps[3][0][1]),
          "The comparison must exercise signed zero and NaN");
    for (const auto blockSize : {1U, 3U, 7U, 17U}) {
        const auto actual = render(DspBackend::LlvmOrc, blockSize);
        for (std::size_t tap = 0; tap < expected.taps.size(); ++tap) {
            for (std::size_t frame = 0; frame < frames; ++frame) {
                for (std::size_t lane = 0; lane < values[frame].size(); ++lane) {
                    const auto interpreted = expected.taps[tap][frame][lane];
                    const auto native = actual.taps[tap][frame][lane];
                    const bool match = std::isnan(interpreted) ? std::isnan(native)
                        : std::bit_cast<std::uint64_t>(interpreted) == std::bit_cast<std::uint64_t>(native);
                    check(match, "LLVM ORC differs from the interpreter at tap " + std::to_string(tap)
                        + ", frame " + std::to_string(frame) + ", lane " + std::to_string(lane));
                }
            }
        }
    }
}

void passiveProbesDoNotScheduleInactiveDsp()
{
    Graph graph;
    const auto counter = graph.addDspCode(compileDspCode(R"(
        streamout count;
        streamout doubled;
        float next;
        count = next;
        doubled = next * 2;
        next += 1;
    )"));
    const auto dormant = graph.addDspCode(compileDspCode("streamout out; out = 99;"));
    auto runtime = graph.compileMonoDouble(std::array{counter.outputs.front()});
    check(runtime.renders(counter.outputs.back()) && !runtime.renders(dormant.outputs.front()),
          "A probe should see every output of a scheduled DSP block and no inactive blocks");

    std::array<double, 4> out{-1, -1, -1, -1};
    std::array<double, 2> tail{-1, -1};
    const std::array outputs{MonoDoubleOutputBuffer{out}};
    using Probe = MonoDoubleRuntime::Probe;
    check(!runtime.process({}, outputs, std::array{Probe{dormant.outputs.front(), tail}}) &&
          out.front() == -1 && tail.front() == -1,
          "An inactive probe must be rejected before advancing or writing samples");

    check(runtime.process({}, outputs, std::array{Probe{counter.outputs.back(), tail}}) &&
          out == std::array{0.0, 1.0, 2.0, 3.0} && tail == std::array{4.0, 6.0},
          "A passive probe should copy the block tail without changing the rendered output");
}

void streamMathKeepsPrecisionAndLanesIndependent()
{
    using namespace openstone::stream;
    using enum SampleOperation;
    using enum SamplePrecision;
    Graph graph;
    const auto left = graph.addInput();
    const auto right = graph.addInput();
    const auto binary = [&](SampleOperation operation, SamplePrecision precision) {
        const auto node = graph.addArithmetic(operation, precision);
        graph.connect(left, node);
        graph.connect(right, node);
        return node;
    };
    const auto sum32 = binary(Add, Float32);
    const auto sum64 = binary(Add, Float64);
    const auto divide = binary(Divide, Float64);
    const auto rounded = graph.addArithmetic(Convert, Float32);
    graph.connect(sum64, rounded);

    auto runtime = graph.compileMonoDouble4(std::array{sum32, sum64, divide, rounded});
    constexpr double base = 16777216.0;
    const std::array<MonoDouble4, 1> a{MonoDouble4{base, -8, 9, 3}};
    const std::array<MonoDouble4, 1> b{MonoDouble4{1, 2, 3, -0.0}};
    std::array<MonoDouble4, 1> narrow{}, wide{}, quotient{}, converted{};
    check(runtime.process(std::array{MonoDouble4InputBuffer{a}, MonoDouble4InputBuffer{b}},
                          std::array{MonoDouble4OutputBuffer{narrow}, MonoDouble4OutputBuffer{wide},
                                     MonoDouble4OutputBuffer{quotient}, MonoDouble4OutputBuffer{converted}}),
          "Process native Stream math on packed samples");
    check(narrow[0] == MonoDouble4{base, -6, 12, 3} && wide[0] == MonoDouble4{base + 1, -6, 12, 3} &&
          narrow == converted, "The first lane retains or rounds extra precision independently of the other lanes");
    check(quotient[0] == MonoDouble4{base, -4, 3, -std::numeric_limits<double>::infinity()},
          "Per-lane division follows IEEE signed-zero semantics");
}

void compileRejectsReachableFeedbackWithoutDelay()
{
    Graph graph;
    const auto sum = graph.addSum();
    const auto gain = graph.addGain(0.5F);
    graph.connect(sum, gain);
    graph.connect(gain, sum);
    bool rejected = false;
    try {
        static_cast<void>(graph.compile(std::array{sum}));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "An instantaneous feedback loop should fail to compile");

    const auto silent = graph.addSum();
    auto runtime = graph.compile(std::array{silent});
    std::array<float, 2> output{1, 1};
    check(runtime.process({}, std::array{OutputBuffer{output}}) && output == std::array{0.0F, 0.0F},
          "An active unconnected sum should produce silence with an unrelated loop in the graph");
}

}

int main()
{
    try {
        feedbackIsIndependentOfBlockSize(1.0F, &Graph::compile);
        feedbackIsIndependentOfBlockSize(1.0 + 0x1p-40, &Graph::compileMonoDouble);
        monoDoubleMultipliersPreservePrecision();
        delaysReadTheirOldStateBeforeWriting();
        variableDelayClampsToTheMaximumAndResetsHistory();
        streamCountersKeepTheirOwnPrecision();
        packedFeedbackMatchesFourScalarStreams<8>(Mono4{1, -2, 4, 8}, &Graph::compileMono4, &Graph::compile,
                                                  std::array<std::size_t, 3>{1, 3, 8});
        constexpr auto impulse = 1.0 + 0x1p-40;
        packedFeedbackMatchesFourScalarStreams<640>(
            MonoDouble4{impulse, -2.0 * impulse, 4.0 * impulse, 8.0 * impulse},
            &Graph::compileMonoDouble4, &Graph::compileMonoDouble,
            std::array<std::size_t, 8>{1, 3, 7, 16, 31, 64, 257, 640});
        mono4MultiplyMatchesFlowStone();
        sumUsesLinkOrderAndSupportsInPlaceBuffers();
        streamMathKeepsPrecisionAndLanesIndependent();
        compileRejectsReachableFeedbackWithoutDelay();
        passiveProbesDoNotScheduleInactiveDsp();
        dspCodeFeedbackUsesGlobalStages();
        dspCodeUsesEachLaneAndPreservesDoublePrecision();
        dspCodeCanShapeIndependentMidiVoices();
        dspCodeReportsSourceErrorsAndRejectsInstantaneousFeedback();
        dspAlphaPreservesIntegerPortsAndMixedPrecision();
        dspAlphaArraysAndLaneSelectionPreserveState();
        dspAlphaHopAndNestedLoopsFollowSampleTime();
        dspAlphaMathAndInitializersRunThroughTheInterpreter();
        dspStageOneReadsReadySourcesInDependencyOrder();
        packedLaneShiftDoesNotAddLatency<Mono4>(&Graph::compileMono4);
        packedLaneShiftDoesNotAddLatency<MonoDouble4>(&Graph::compileMonoDouble4);
        stageThreeOutputsAdvanceTogether();
        assemblerRunsPackedStagesAcrossBufferSizes();
        assemblerPreservesPackedBits();
        assemblerCmpTestsRawWords();
        assemblerRunsIntegerControlFlow();
        assemblerReadsMonotonicTimestamp();
        assemblerConvertsAndMovesSimdLanes();
        assemblerPreservesRawFloatBitsAndIsolatesHostEnvironment();
        assemblerRunsX87MathAndStores();
        assemblerKeepsExtendedX87Precision();
        assemblerStoresDoubleX87ValuesThroughMemReferences();
        assemblerUsesTheLowQwordForPackedShiftCounts();
        assemblerReadsAndWritesIndexedArrayOperands();
        assemblerAccessesScalarWordsAndDoubleWindows();
        assemblerReadsExternalArraysAndUnalignedOffsets();
        assemblerBoundsIndirectMemoryToItsConnectedView();
        assemblerCallsLocalSubroutineThroughRegisters();
        assemblerBranchesToLocalReturnByDisplacement();
        assemblerLimitsBackwardJumpsPerSample();
        assemblerCallsEarlierLabelWithinSubroutine();
        assemblerBranchesPastAnEarlyReturnByDisplacement();
        assemblerBoundsRecursiveSubroutineCalls();
        assemblerBoundsBranchingRecursionWork();
        assemblerTailJumpsReuseTheCallerFrame();
        assemblerRegisterTailJumpsKeepTheCaller();
        assemblerNumericJumpSkipsARegisterTailJump();
        assemblerRuntimeTargetsTravelThroughStackAndReturns();
        assemblerRuntimeTargetsMergeThreePaths();
        assemblerReloadsTargetsFromLocalMemoryCells();
        assemblerIndexesSavedTargetTables();
        assemblerMemoryTransfersPreserveAddressRegisters();
        assemblerMemoryTransfersRecoverAfterShortBuffers();
        assemblerMemPointerArithmeticLoopsRemainUsable();
        assemblerIndexedTransfersValidateTableCells();
        assemblerIndexedWritesRetainAndInvalidateTargets();
        assemblerIndexedWritesInitializeTableInLoop();
        assemblerCopiesTargetsBetweenIndexedCells();
        assemblerSwapsIndexedTargetsAcrossBuffers();
        assemblerMemArrayTablesAndWrites();
        assemblerNumericSpansIncludeIndexedTransfers();
        assemblerLimitsNestedCallExpansion();
        assemblerRejectsUnsupportedAndInvalidOperands();
        assemblerZeroRegisterWorksWithDoubleOperands();
        dspOrcMatchesInterpreterAcrossRenderBlocks();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
