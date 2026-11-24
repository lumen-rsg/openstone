#include "../ReferenceFixtures.hpp"
#include "stream/Runtime.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using namespace openstone::stream;
constexpr std::size_t frames = 37;

void check(bool valid, std::string_view message)
{
    if (!valid) throw std::runtime_error(std::string(message));
}

template<std::size_t Outputs, typename Sample, std::size_t Inputs, typename Verify>
void acrossBuffers(detail::Runtime<Sample>& runtime,
                   const std::array<std::array<Sample, frames>, Inputs>& input, Verify verify)
{
    for (const std::size_t size : {1, 3, 7, 31, 64}) {
        runtime.reset();
        std::array<std::array<Sample, frames>, Outputs> output{};
        for (std::size_t offset = 0; offset < frames; offset += size) {
            const auto count = std::min(size, frames - offset);
            std::array<std::span<const Sample>, Inputs> in;
            std::array<std::span<Sample>, Outputs> out;
            for (std::size_t port = 0; port < Inputs; ++port) in[port] = std::span{input[port]}.subspan(offset, count);
            for (std::size_t port = 0; port < Outputs; ++port) out[port] = std::span{output[port]}.subspan(offset, count);
            check(runtime.process(in, out), "Render declaration grammar regression");
        }
        verify(output);
    }
}

void listsPreservePortOrderAndStageTiming()
{
    for (const auto language : {DspLanguage::Code, DspLanguage::Assembler}) {
        const auto code = language == DspLanguage::Code;
        const auto program = compileDspCode(code ? R"(
            streamdoublein a,
                // Each name gets its own connector.
                b;
            streamdoubleout sum, difference;
            double stage=0;
            stage(0) { stage=0.5; }
            sum=a+b+stage;
            difference=a-b;
            stage(3) { stage=stage+0.25; }
        )" : R"(
            streamin a,
                // The same declaration grammar applies to Assembler.
                b;
            streamout sum, difference;
            float stage=0, initial=0.5, step=0.25;
            stage(0) { movaps xmm0,initial; movaps stage,xmm0; }
            movaps xmm0,a; addps xmm0,b;
            jmp 7; addps xmm0,stage;
            addps xmm0,stage; movaps sum,xmm0;
            movaps xmm1,a; subps xmm1,b; movaps difference,xmm1;
            stage(3) { movaps xmm2,stage; addps xmm2,step; movaps stage,xmm2; }
        )", language);
        check(program.inputs().size() == 2 && program.inputs()[0].name == "a" && program.inputs()[1].name == "b" &&
              program.outputs().size() == 2 && program.outputs()[0].name == "sum" && program.outputs()[1].name == "difference",
              "Comma declarations retain input/output order independently of instruction order");
        check(program.inputs()[0].type == (code ? DspPortType::StreamDouble : DspPortType::Stream),
              "Every connector in the declaration retains its declared type");
        std::array<std::array<MonoDouble4, frames>, 2> input{};
        for (std::size_t n = 0; n < frames; ++n) for (unsigned lane = 0; lane < 4; ++lane) {
            input[0][n][lane] = (static_cast<double>(n + lane) + 1) * 0.125 + (code ? 0x1p-40 : 0.0);
            input[1][n][lane] = (2 * static_cast<double>(lane) - static_cast<double>(n)) * 0.25;
        }
        for (auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);
            const auto block = graph.addDspCode(program);
            for (std::size_t port = 0; port < 2; ++port) graph.connectDspInput(graph.addInput(), block.id, port);
            auto runtime = graph.compileMonoDouble4(block.outputs);
            acrossBuffers<2>(runtime, input, [&](const auto& output) {
                for (std::size_t n = 0; n < frames; ++n) for (unsigned lane = 0; lane < 4; ++lane) {
                    check(output[0][n][lane] == input[0][n][lane] + input[1][n][lane] + 0.5 + 0.25 * n &&
                          output[1][n][lane] == input[0][n][lane] - input[1][n][lane],
                          "A variable named stage keeps initialization and late-stage state across buffer boundaries");
                }
            });
        }
    }
}

void stageSelectorsAndSubroutinesRemainUnambiguous()
{
    constexpr std::array sources{
        R"(streamout out; float stage={1,2,3,4}; stage(0) { stage{0}=10; } stage(0)=stage+1; out=stage;)",
        R"(streamout out; float stage[2]={3,7}; stage(0) { stage[1]=9; } out=stage[0]+stage[1];)",
        R"(streamout out; float stage={2,3,4,5}; float one=1;
            call increment; movaps xmm0,stage; movaps out,xmm0; jmp done;
            increment: movaps xmm0,stage; addps xmm0,one; movaps stage,xmm0; ret; done:)",
    };
    for (std::size_t test = 0; test < sources.size(); ++test) {
        const auto program = compileDspCode(sources[test], test == 2 ? DspLanguage::Assembler : DspLanguage::Code);
        for (auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);
            const auto block = graph.addDspCode(program);
            auto runtime = graph.compileMono4(block.outputs);
            const std::array<std::array<Mono4, frames>, 0> input{};
            acrossBuffers<1>(runtime, input, [&](const auto& output) {
                for (std::size_t n = 0; n < frames; ++n) for (unsigned lane = 0; lane < 4; ++lane) {
                    const auto expected = test == 0 ? 11 + n : test == 1 ? 12 : 3 + n + lane;
                    check(output[0][n][lane] == static_cast<float>(expected),
                          "Stage blocks, parenthesized lane assignments, arrays and call scans distinguish the stage variable");
                }
            });
        }
    }
    for (const auto language : {DspLanguage::Code, DspLanguage::Assembler}) {
        const auto program = compileDspCode(language == DspLanguage::Code
            ? "streamin stage,other;streamout out;out=stage+other;"
            : "streamin stage,other;streamout out;movaps xmm0,stage;addps xmm0,other;movaps out,xmm0;", language);
        for (auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);
            const auto block = graph.addDspCode(program);
            graph.connectDspInput(graph.addInput(), block.id, 0);
            graph.connectDspInput(graph.addInput(), block.id, 1);
            auto runtime = graph.compile(block.outputs);
            const std::array a{2.0F}, b{3.0F};
            std::array<float, 1> out{};
            check(runtime.process(std::array{InputBuffer{a},InputBuffer{b}}, std::array{OutputBuffer{out}}) && out[0] == 5,
                  "A connector can use the contextual name stage");
        }
    }
}

void arrayDeclaratorsKeepTheirOwnBounds()
{
    for (const auto language : {DspLanguage::Code, DspLanguage::Assembler}) {
        const std::string header = "memin first[2],second[3];floatarrayin gains[1],pads[2];streamout left,right;";
        const auto program = compileDspCode(header + (language == DspLanguage::Code
            ? "int lane={0,1,2,3};left=first[lane]+gains[lane];right=second[lane]+pads[lane];"
            : "movaps xmm0,first[0];addps xmm0,gains[0];movaps left,xmm0;movaps xmm1,second[0];addps xmm1,pads[0];movaps right,xmm1;"), language);
        constexpr std::array names{"first","second","gains","pads"};
        constexpr std::array<std::size_t, 4> sizes{2,3,1,2};
        for (std::size_t port = 0; port < sizes.size(); ++port) {
            check(program.inputs()[port].name == names[port] && program.inputs()[port].arraySize == sizes[port],
                  "Each array connector keeps its name and individual bound");
        }
        for (auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);
            const auto block = graph.addDspCode(program);
            auto runtime = graph.compileMono4(block.outputs);
            check(runtime.setDspArrayInput(block.id,0,std::array{1.F,2.F}) &&
                  runtime.setDspArrayInput(block.id,1,std::array{3.F,4.F,5.F}) &&
                  runtime.setDspArrayInput(block.id,2,std::array{10.F}) &&
                  runtime.setDspArrayInput(block.id,3,std::array{20.F,30.F}), "Bind comma-declared arrays independently");
            std::array<Mono4, 1> left{}, right{};
            check(runtime.process({},std::array{Mono4OutputBuffer{left},Mono4OutputBuffer{right}}) &&
                  left[0] == Mono4{11,2,0,0} && right[0] == Mono4{23,34,5,0},
                  "Array connector slots and bounds remain independent through execution");
        }
    }
    const auto program = compileDspCode(R"(
        memrefin first, second; memarrayrefin wavesA, wavesB;
        streamout bytesA, bytesB, countA, countB;
        mov eax,first[1]; movd xmm0,eax; cvtdq2ps xmm0,xmm0; movaps bytesA,xmm0;
        mov eax,second[1]; movd xmm0,eax; cvtdq2ps xmm0,xmm0; movaps bytesB,xmm0;
        mov eax,wavesA[0]; movd xmm0,eax; cvtdq2ps xmm0,xmm0; movaps countA,xmm0;
        mov eax,wavesB[0]; movd xmm0,eax; cvtdq2ps xmm0,xmm0; movaps countB,xmm0;
    )", DspLanguage::Assembler);
    for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto block = graph.addDspCode(program);
        auto runtime = graph.compile(block.outputs);
        const auto a = openstone::green::Memory::fromSamples(std::array{1.F,2.F});
        const auto b = openstone::green::Memory::fromSamples(std::array{3.F,4.F,5.F});
        check(runtime.setDspMemRefInput(block.id,0,a) && runtime.setDspMemRefInput(block.id,1,b) &&
              runtime.setDspMemArrayInput(block.id,2,openstone::green::MemoryArray({a,b})) &&
              runtime.setDspMemArrayInput(block.id,3,openstone::green::MemoryArray({b})), "Bind grouped reference connectors");
        std::array<std::array<float,1>,4> out{};
        check(runtime.process({},std::array{OutputBuffer{out[0]},OutputBuffer{out[1]},OutputBuffer{out[2]},OutputBuffer{out[3]}}) &&
              out == std::array{std::array{8.F},std::array{12.F},std::array{2.F},std::array{1.F}},
              "Comma-declared Mem and MemArray references keep independent descriptor storage");
    }
}

std::string fixture(std::string_view name)
{
    std::ifstream file(std::filesystem::path(OPENSTONE_TEST_FIXTURES)/"flowstone"/name,std::ios::binary);
    if (!file) throw std::runtime_error("Cannot read declaration grammar fixture");
    return {std::istreambuf_iterator<char>(file),{}};
}

void unchangedBundledDeclarationsRender()
{
    const auto interpolation = compileDspCode(fixture("assembler-interpolation-weights.asm"),DspLanguage::Assembler);
    const auto counter = compileDspCode(fixture("assembler-double-accumulator.asm"),DspLanguage::Assembler);
    for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto block = graph.addDspCode(interpolation);
        graph.connectDspInput(graph.addInput(),block.id,0);
        auto runtime = graph.compileMono4(block.outputs);
        std::array<std::array<Mono4,frames>,1> input{};
        for (std::size_t n=0;n<frames;++n) for (unsigned lane=0;lane<4;++lane) input[0][n][lane]=(static_cast<float>(n)-15.F)*0.375F+lane*0.125F;
        acrossBuffers<4>(runtime,input,[&](const auto& output) {
            for (std::size_t n=0;n<frames;++n) for (unsigned lane=0;lane<4;++lane) {
                const auto whole=std::trunc(input[0][n][lane]);
                const auto fraction=input[0][n][lane]-whole;
                check(output[0][n][lane]==whole-1 && output[1][n][lane]==whole &&
                      output[2][n][lane]==1-fraction && output[3][n][lane]==fraction,
                      "Unchanged four-output interpolation source preserves declared output order");
            }
        });
        Graph doubleGraph(backend);
        const auto ports=doubleGraph.addDspCode(counter);
        doubleGraph.connectDspInput(doubleGraph.addInput(),ports.id,0);
        doubleGraph.connectDspInput(doubleGraph.addInput(),ports.id,1);
        auto doubleRuntime=doubleGraph.compileMonoDouble4(ports.outputs);
        constexpr MonoDouble4 step{0.25+0x1p-40,0.5+0x1p-40,1.0+0x1p-40,0.125+0x1p-40};
        constexpr MonoDouble4 length{1,2,4,0.5};
        std::array<std::array<MonoDouble4,frames>,2> values{};
        values[0].fill(step);values[1].fill(length);
        acrossBuffers<1>(doubleRuntime,values,[&](const auto& output) {
            MonoDouble4 state{};
            for (std::size_t n=0;n<frames;++n) for (unsigned lane=0;lane<4;++lane) {
                state[lane]+=step[lane];
                if (state[lane]>length[lane]) state[lane]-=length[lane];
                check(output[0][n][lane]==static_cast<float>(state[lane]),
                      "Unchanged double accumulator preserves grouped inputs, double state and four-lane output across buffers");
            }
        });
    }
}

void malformedListsKeepPreciseDiagnostics()
{
    struct Case { const char* source; const char* message; std::size_t line; };
    constexpr Case cases[]{
        {"streamin a,\n a;streamout out;","duplicate DSP name",2},
        {"streamin a,b;streamout left,b;","duplicate DSP name",1},
        {"streamin stage;float stage;streamout out;","duplicate DSP name",1},
        {"streamin a,;streamout out;","expected a name",1},
        {"streamout out,;","expected a name",1},
        {"streamin a\n b;streamout out;","expected ';'",2},
        {"memin a[2],b;streamout out;","expected '['",1},
        {"float loop;streamout out;","reserved DSP name",1},
        {"float stage;streamout out;stage(4) {}","expected stage number",1},
    };
    for (auto language : {DspLanguage::Code,DspLanguage::Assembler}) for (const auto& test : cases) {
        bool rejected=false;
        try { static_cast<void>(compileDspCode(test.source,language)); }
        catch (const DspCompileError& e) {
            rejected=e.line==test.line && e.column>0 && std::string_view(e.what()).contains(test.message);
        }
        check(rejected,"Malformed declarations report the original source location and cause");
    }
}
}

int main()
{
    try {
        listsPreservePortOrderAndStageTiming();
        stageSelectorsAndSubroutinesRemainUnambiguous();
        arrayDeclaratorsKeepTheirOwnBounds();
        openstone::tests::withReferenceSources("unchangedBundledDeclarationsRender", [&] { unchangedBundledDeclarationsRender(); });
        malformedListsKeepPreciseDiagnostics();
    } catch (const std::exception& error) {
        std::cerr<<error.what()<<'\n';return 1;
    }
}
