#include "../ReferenceFixtures.hpp"
#include "stream/Runtime.hpp"
#include "stream/PolyRuntime.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {
using namespace openstone::stream;
using Words = std::array<std::uint32_t, 4>;
using Memory = openstone::green::Memory;
void check(bool ok, std::string_view message) { if (!ok) throw std::runtime_error(std::string(message)); }
struct Environment {
    std::fenv_t saved;
    Environment() { std::fegetenv(&saved); }
    ~Environment() { std::fesetenv(&saved); }
};
std::string fixture(bool x64)
{
    std::ifstream file(std::filesystem::path(OPENSTONE_TEST_FIXTURES) / "flowstone" /
        (x64 ? "engine-dezipper-compact-x64.asm" : "engine-dezipper-compact-x86.asm"), std::ios::binary);
    check(file.good(), "Read exact compact-stage kernel");
    std::string body{std::istreambuf_iterator<char>(file), {}};
    check(body.size() == (x64 ? 391U : 337U), "Keep original source bytes and length");
    return body;
}
std::uint32_t word(const Memory& memory, unsigned offset)
{
    std::uint32_t value = 0;
    for (unsigned b = 0; b < 4; ++b) value |= std::uint32_t(memory.byte(offset * 4 + b)) << (8 * b);
    return value;
}
float add(float a, float b) { volatile float x = a + b; return x; }
float sub(float a, float b) { volatile float x = a - b; return x; }
constexpr std::array<unsigned, 8> changes{0, 5, 29, 43, 67, 91, 119, 151};
constexpr std::array<Mono4, 8> targets{{{1.5F,-2,.25F,8}, {4,-4,2,1}, {-.5F,2,-3,4}, {-7,8,-1,0},
    {-0.F,3,5,-6}, {2,20,30,40}, {2,300,400,500}, {0,-.25F,100,-2}}};
constexpr Mono4 positive{.25F,.5F,1,2}, negative{-.5F,-.25F,-2,-1};
constexpr std::size_t frames = 193;
struct SmoothingOracle {
    Mono4 target = targets[0], value = targets[0];
    bool reset = false;
    Mono4 next(bool x64)
    {
        // The x86 source tests and clears p_target, not p_reset. Preserve that original difference.
        const bool direct = x64 ? reset : std::bit_cast<std::uint32_t>(target[0]) != 0;
        if (x64) reset = false;
        const auto requested = target;
        if (!x64) target[0] = 0;
        if (direct) value = requested;
        else if (requested[0] != value[0]) {
            for (unsigned lane = 0; lane < 4; ++lane)
                value[lane] = add(value[lane], std::min(positive[lane], std::max(negative[lane], sub(requested[lane], value[lane]))));
        }
        return value;
    }
};

void originalDezippers(DspBackend backend)
{
    for (bool x64 : {false, true}) {
        // These engine fragments omit the caller's outlet; their entire original bodies follow it unchanged.
        const auto program = compileDspCode("streamout out0;" + fixture(x64), DspLanguage::Assembler);
        check(program.enginePointers().size() == 4, "Keep all original engine resource declarations");
        Graph graph(backend); const auto block = graph.addDspCode(program);
        auto target = Memory::fromSamples(targets[0]), pos = Memory::fromSamples(positive), neg = Memory::fromSamples(negative);
        auto reset = Memory::fromSamples(std::bit_cast<Mono4>(Words{0,0x12345678,0x87654321,0xdeadbeef}));
        check(graph.bindDspEnginePointer(block.id, "p_target", target, !x64) &&
            graph.bindDspEnginePointer(block.id, "p_posstep", pos, false) &&
            graph.bindDspEnginePointer(block.id, "p_negstep", neg, false) &&
            graph.bindDspEnginePointer(block.id, "p_reset", reset), "Bind explicit live caller controls");
        auto runtime = graph.compileMono4(block.outputs);
        std::array<Mono4, frames> expected{};
        Environment environment; std::fesetround(FE_TONEAREST);
        SmoothingOracle oracle;
        for (unsigned n = 0, event = 0; n < frames; ++n) {
            if (event < changes.size() && changes[event] == n) { oracle.target = targets[event]; oracle.reset = event == 3; ++event; }
            expected[n] = oracle.next(x64);
        }
        for (const std::size_t size : {1,3,7,31,64,257}) {
            runtime.reset(); target = Memory::fromSamples(targets[0]);
            check(runtime.setDspEnginePointer(block.id, "p_target", target, !x64), "Rebind target before stage zero");
            std::array<Mono4, frames> output{};
            check(runtime.process({}, std::array{Mono4OutputBuffer{std::span{output}.first(0)}}), "Empty buffer defers initialization");
            SmoothingOracle state;
            for (unsigned offset = 0, event = 0; offset < frames;) {
                if (event < changes.size() && changes[event] == offset) {
                    state.target = targets[event]; state.reset = event == 3;
                    for (unsigned lane = 0; lane < 4; ++lane) target.writeSample(lane, state.target[lane]);
                    reset.writeSample(0, std::bit_cast<float>(std::uint32_t(state.reset)));
                    ++event;
                }
                const auto end = event < changes.size() ? changes[event] : frames;
                const auto count = std::min<std::size_t>({size, frames - offset, end - offset});
                std::fesetround(FE_UPWARD); std::feclearexcept(FE_ALL_EXCEPT); std::feraiseexcept(FE_DIVBYZERO);
                check(runtime.process({}, std::array{Mono4OutputBuffer{std::span{output}.subspan(offset, count)}}), "Render unchanged compact-stage dezipper");
                check(std::fegetround() == FE_UPWARD && std::fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO, "Smoothing preserves caller floating-point state");
                std::fesetround(FE_TONEAREST);
                for (unsigned n = offset; n < offset + count; ++n) {
                    state.next(x64);
                    check(std::bit_cast<Words>(output[n]) == std::bit_cast<Words>(expected[n]), "Every output word matches the independent smoothing recurrence");
                }
                check(word(reset,0) == std::uint32_t(state.reset), "Reset consumption matches the exact architecture's original body");
                for (unsigned lane = 0; lane < 4; ++lane) check(word(target,lane) == std::bit_cast<std::uint32_t>(state.target[lane]), "Original target writes have exact bounded effects");
                check(word(reset,1) == 0x12345678 && word(reset,2) == 0x87654321 && word(reset,3) == 0xdeadbeef, "Reset store preserves adjacent words");
                offset += count;
            }
            for (unsigned lane = 0; lane < 4; ++lane)
                check(word(pos,lane) == std::bit_cast<std::uint32_t>(positive[lane]) && word(neg,lane) == std::bit_cast<std::uint32_t>(negative[lane]), "Step resources stay read-only");
        }
    }
}

template<typename Sample>
void initializedOutputTiming(DspBackend backend)
{
    constexpr bool doubles = std::same_as<Sample, MonoDouble4>;
    const std::string move = doubles ? "movapd" : "movaps", plus = doubles ? "addpd" : "addps";
    const std::string header = doubles ? "streamdoubleout early,current,late,held,zeroLate;double seed={1.0000000000009094947017729282379150390625,-1.999999999998181010596454143524169921875,4.00000000000363797880709171295166015625,16.000000000014551915228366851806640625};" :
        "streamout early,current,late,held,zeroLate;float seed={1.25,-2.5,4,16};";
    const auto copy = [&](std::string_view destination, std::string_view source, bool increment = false) {
        std::string code;
        for (unsigned lane = 0; lane < 4; lane += doubles ? 2 : 4) {
            const auto at = "[" + std::to_string(lane) + "]";
            code += move + " xmm0," + std::string(source) + at + ";";
            if (increment) code += plus + " xmm0," + (doubles ? "_DONE" : "_ONE") + ";";
            code += move + " " + std::string(destination) + at + ",xmm0;";
        }
        return code;
    };
    std::string init = "mov eax,[initCount];inc eax;mov [initCount],eax;";
    for (auto output : {"early","current","late","held"}) init += copy(output, "seed");
    const auto regular = "stage1;" + copy("early", "early", true) + "stage2;" + copy("current", "current", true) + "stage3;" + copy("late", "late", true) + copy("zeroLate", doubles ? "_DONE" : "_ONE");
    const Sample seed = [] { if constexpr (doubles) return Sample{1.0+0x1p-40,-2.0+0x1p-39,4.0+0x1p-38,16.0+0x1p-36}; else return Sample{1.25,-2.5,4,16}; }();
    for (unsigned style = 0; style < 3; ++style) {
        const auto body = header + "__const initCount;" + (style == 0 ? "stage0;" + init : style == 1 ? "stage 0;" + init : "stage(0){" + init + "}") + regular;
        Graph graph(backend); const auto b = graph.addDspCode(compileDspCode(body,DspLanguage::Assembler));
        auto count = Memory::fromSamples(std::array{0.F}); check(graph.bindDspEnginePointer(b.id,"initCount",count), "Bind initialization count");
        auto runtime = [&] { if constexpr (doubles) return graph.compileMonoDouble4(b.outputs); else return graph.compileMono4(b.outputs); }();
        unsigned initializations = 0;
        for (const std::size_t size : {1,3,7,31,64,257}) {
            runtime.reset(); std::array<std::array<Sample,67>,5> out{};
            std::array<std::span<Sample>,5> empty{};
            check(runtime.process({},empty) && word(count,0) == initializations, "Empty buffers do not execute output seeds");
            for (unsigned offset = 0; offset < 67; offset += size) {
                const auto n = std::min<std::size_t>(size,67-offset); std::array<std::span<Sample>,5> outputs;
                for (unsigned port = 0; port < 5; ++port) outputs[port] = std::span{out[port]}.subspan(offset,n);
                check(runtime.process({},outputs), "Render seeded early, ordinary, late and initialization-only outputs");
            }
            check(word(count,0) == ++initializations, "Stage zero executes once per reset");
            for (unsigned n = 0; n < 67; ++n) for (unsigned lane = 0; lane < 4; ++lane) {
                check(out[0][n][lane] == seed[lane] + n + 1 && out[1][n][lane] == seed[lane] + n + 1 &&
                    out[2][n][lane] == seed[lane] + n && out[3][n][lane] == seed[lane] && out[4][n][lane] == (n ? 1 : 0),
                    "Stage-zero publication seeds the first late sample, holds init-only outlets and leaves unseeded late outlets zero");
            }
        }
    }
}

void polyInitialization(DspBackend backend)
{
    Graph graph(backend); const auto b = graph.addDspCode(compileDspCode(
        "polyout out;stage0;movaps xmm0,_ONE;movaps out,xmm0;stage3;movaps xmm0,out;addps xmm0,_ONE;movaps out,xmm0;",DspLanguage::Assembler));
    const std::array<PolyInput,0> bindings{};
    PolyRuntime runtime(graph,bindings,b.outputs,{.maxVoices=5,.holdAfterNoteOff=true});
    const std::array events{PolyEvent{0,PolyEventKind::NoteOn,1,60,.5F},PolyEvent{1,PolyEventKind::NoteOn,2,61,.5F},
        PolyEvent{3,PolyEventKind::NoteOn,3,62,.5F},PolyEvent{5,PolyEventKind::NoteOn,4,63,.5F},PolyEvent{8,PolyEventKind::NoteOn,5,64,.5F},
        PolyEvent{9,PolyEventKind::NoteOff,1},PolyEvent{11,PolyEventKind::EndVoice,1},PolyEvent{14,PolyEventKind::NoteOn,6,65,.5F}};
    for (unsigned size : {1,3,7,31}) {
        runtime.reset(); std::array<float,23> out{};
        for (unsigned offset = 0; offset < out.size(); offset += size) {
            const auto count = std::min<std::size_t>(size,out.size()-offset); std::vector<PolyEvent> part;
            for (auto e : events) if (e.frame >= offset && e.frame < offset+count) { e.frame -= offset; part.push_back(e); }
            check(runtime.process({},std::array{OutputBuffer{std::span{out}.subspan(offset,count)}},part), "Initialize staggered voices across two packets and slot reuse");
        }
        for (unsigned n = 0; n < out.size(); ++n) {
            float expected = 0;
            for (unsigned start : {0,1,3,5,8,14}) if (n >= start && !(start == 0 && n >= 11)) expected += n-start+1;
            if (out[n] != expected) throw std::runtime_error("Poly seed mismatch backend="+std::to_string(int(backend))+" buffer="+std::to_string(size)+" sample="+std::to_string(n)+" got="+std::to_string(out[n])+" expected="+std::to_string(expected));
        }
    }
}

void seededFeedback(DspBackend backend)
{
    const auto program = compileDspCode("streamin in;streamout out;float seed={1,-2,4,-8};stage0;movaps xmm0,seed;movaps out,xmm0;stage3;movaps xmm0,in;movaps out,xmm0;",DspLanguage::Assembler);
    check(program.readsSignalInput(0) && !program.outputDependsOnInput(0,0), "A stage-zero seed does not add a current-sample dependency to a late output");
    Graph graph(backend); const auto b = graph.addDspCode(program);
    const auto input = graph.addInput(), sum = graph.addSum(), half = graph.addGain(.5);
    graph.connect(input,sum); graph.connect(b.outputs[0],half); graph.connect(half,sum); graph.connectDspInput(sum,b.id,0);
    auto runtime = graph.compileMono4(std::array{sum,b.outputs[0]});
    std::array<Mono4,67> in{}, expectedSum{}, expectedDelay{}; Mono4 state{1,-2,4,-8};
    for (unsigned lane = 0; lane < 4; ++lane) in[2+lane][lane] = lane+1;
    for (unsigned n = 0; n < in.size(); ++n) {
        expectedDelay[n] = state;
        for (unsigned lane = 0; lane < 4; ++lane) state[lane] = in[n][lane] + state[lane]*.5F;
        expectedSum[n] = state;
    }
    for (const std::size_t size : {1,3,7,31,64,257}) {
        runtime.reset(); std::array<Mono4,67> sums{}, delays{};
        for (unsigned offset = 0; offset < in.size(); offset += size) {
            const auto count = std::min<std::size_t>(size,in.size()-offset);
            check(runtime.process(std::array{Mono4InputBuffer{std::span{in}.subspan(offset,count)}},
                std::array{Mono4OutputBuffer{std::span{sums}.subspan(offset,count)},Mono4OutputBuffer{std::span{delays}.subspan(offset,count)}}), "Run seeded feedback across buffer boundaries");
        }
        check(sums == expectedSum && delays == expectedDelay, "Feedback starts from its published seed and follows the independent one-sample recurrence");
    }
}

void grammarAndBoundaries(DspBackend backend)
{
    for (const auto body : {
        "float stage0=3;stage1;movaps xmm0,stage0;movaps out,xmm0;",
        "stage1;call stage0;movaps out,xmm0;jmp done;stage0:mov eax,stage0;movaps xmm0,_ONE;addps xmm0,_ONE;addps xmm0,_ONE;ret;done:stage3;",
        "stage1;mov eax,helper;call eax;jmp 6;mov eax,99;movd xmm0,eax;cvtdq2ps xmm0,xmm0;movaps out,xmm0;jmp done;helper:mov eax,3;ret;done:stage3;",
        "stage2;mov eax,0;again:inc eax;cmp eax,3;jnz again;movd xmm0,eax;cvtdq2ps xmm0,xmm0;movaps out,xmm0;stage3;"}) {
        Graph graph(backend); const auto b = graph.addDspCode(compileDspCode(std::string("streamout out;")+body,DspLanguage::Assembler));
        auto runtime = graph.compile(b.outputs); std::array<float,9> out{};
        check(runtime.process({},std::array{OutputBuffer{out}}), "Execute compact directives with local calls, labels, contextual names and numeric branches");
        for (float value : out) check(value == 3, "Compact sections preserve control flow");
    }
    const auto reject = [](std::string_view body, std::string_view message, DspLanguage language=DspLanguage::Assembler) {
        bool rejected = false;
        try { static_cast<void>(compileDspCode(body,language)); }
        catch (const DspCompileError& e) { rejected = e.line && e.column && std::string_view(e.what()).contains(message); }
        check(rejected,std::string("Expected compact-stage diagnostic: ")+std::string(body));
    };
    reject("streamout out;stage4;", "expected stage number");
    reject("streamout out;stage9;", "expected stage number");
    reject("streamout out;stage0 movaps xmm0,_ONE;", "expected ';'");
    reject("streamout out;stage(1){stage0;}", "unsupported Assembler instruction");
    reject("streamin in;streamout out;stage0;movaps xmm0,in;", "read stream inputs after stage(0)");
    reject("streamin in;streamout out;stage0;movaps xmm0,_ONE;movaps in,xmm0;", "write signal inputs after stage(0)");
    reject("streamout out;stage0;movaps xmm0,_ONE;movaps out,xmm0;stage1;movaps xmm0,_ONE;movaps out,xmm0;stage2;movaps xmm0,_ONE;movaps out,xmm0;", "only one stage");
    reject("streamout out;stage1;jmp later;stage3;later:", "unknown Assembler label");
    reject("streamout out;stage1;mov eax,0;jmp 1;stage3;mov eax,1;", "numeric branch displacement leaves this Assembler stage");
    reject("streamout out;stage1;call helper;stage3;helper:ret;", "call requires a local subroutine");
    reject("streamout out;stage0;mov eax,1;push eax;stage2;", "balance push and pop");
    reject("streamout out;stage(0){out=1;}", "assign DSP outputs",DspLanguage::Code);
    reject("streamout out;stage0;", "unknown DSP name",DspLanguage::Code);
    // In DSP Code the same spelling remains an ordinary variable.
    static_cast<void>(compileDspCode("streamout out;float stage0=3;out=stage0;"));
}
}
int main()
{
    try { for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) if (dspBackendAvailable(backend)) {
        openstone::tests::withReferenceSources("originalDezippers", [&] { originalDezippers(backend); }); initializedOutputTiming<Mono4>(backend); initializedOutputTiming<MonoDouble4>(backend);
        polyInitialization(backend); seededFeedback(backend); grammarAndBoundaries(backend);
    } } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
