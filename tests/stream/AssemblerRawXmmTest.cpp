#include "../ReferenceFixtures.hpp"
#include "stream/Runtime.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {
using namespace openstone::stream;

void check(bool valid, std::string_view message)
{
    if (!valid) throw std::runtime_error(std::string(message));
}

template<std::size_t Outputs, typename Sample, std::size_t Frames, std::size_t Inputs, typename Verify>
void acrossBuffers(detail::Runtime<Sample>& runtime, const std::array<std::array<Sample, Frames>, Inputs>& input, Verify verify)
{
    for (const std::size_t size : {1, 3, 7, 31, 64}) {
        runtime.reset();
        std::array<std::array<Sample, Frames>, Outputs> output{};
        for (std::size_t offset = 0; offset < Frames; offset += size) {
            const auto count = std::min(size, Frames - offset);
            std::array<std::span<const Sample>, Inputs> in;
            std::array<std::span<Sample>, Outputs> out;
            for (std::size_t port = 0; port < Inputs; ++port) in[port] = std::span{input[port]}.subspan(offset, count);
            for (std::size_t port = 0; port < Outputs; ++port) out[port] = std::span{output[port]}.subspan(offset, count);
            check(runtime.process(in, out), "Render raw XMM buffer");
        }
        verify(output);
    }
}

constexpr std::array doubleWords{
    0x0000000000000000ULL, 0x8000000000000000ULL, 0x3ff0000000001000ULL,
    0xbff0000000002000ULL, 0x7ff0000000000000ULL, 0xfff0000000000000ULL,
    0x7ff0000000000042ULL, 0xfff8123456789abcULL, 0x0000000000000001ULL,
    0x8000000000000001ULL, 0x4008000000000000ULL, 0xc008000000000000ULL,
};
constexpr std::size_t frames = 37;

auto doubleInputs()
{
    std::array<std::array<MonoDouble4, frames>, 3> input{};
    for (std::size_t n = 0; n < frames; ++n) {
        for (unsigned lane = 0; lane < 4; ++lane) {
            input[0][n][lane] = std::bit_cast<double>(doubleWords[(n + lane) % doubleWords.size()]);
            input[1][n][lane] = std::bit_cast<double>(doubleWords[(n + lane + (n % 4 == 0 ? 0 : 5)) % doubleWords.size()]);
            input[2][n][lane] = ((n + lane * 2) % 5) < 2 ? 0.0 : 1.0;
        }
    }
    input[0][0] = {1.0 + 0x1p-40, -1.0 - 0x1p-40, 1.0, -1.0};
    input[1][0] = {1.0, -1.0, 1.0 + 0x1p-40, -1.0 - 0x1p-40};
    return input;
}

template<typename Sample>
void halfMovesPreserveRawBits()
{
    constexpr bool doubles = std::is_same_v<Sample, MonoDouble4>;
    using Scalar = typename Sample::value_type;
    using Words = std::array<std::uint32_t, sizeof(Sample) / sizeof(std::uint32_t)>;
    constexpr std::size_t count = 131;
    constexpr std::array floatWords{
        0x00000000U,0x80000000U,0x00000001U,0x80000001U,0x007fffffU,0x807fffffU,
        0x00800000U,0x80800000U,0x7f800000U,0xff800000U,0x7f812345U,0xffc54321U,
        0x7fc12345U,0xff854321U,0xffffffffU,0x01234567U,0x89abcdefU,0x3f800000U,
    };
    std::array<std::array<Sample, count>, 3> input{};
    for (std::size_t n = 0; n < count; ++n) {
        for (unsigned lane = 0; lane < 4; ++lane) {
            if constexpr (doubles) {
                input[0][n][lane] = std::bit_cast<double>(doubleWords[(n + lane) % doubleWords.size()]);
                input[1][n][lane] = std::bit_cast<double>(doubleWords[(n + lane + 5) % doubleWords.size()]);
                input[2][n][lane] = std::bit_cast<double>(std::uint64_t{n % 3});
            } else {
                input[0][n][lane] = std::bit_cast<float>(floatWords[(n + lane) % floatWords.size()]);
                input[1][n][lane] = std::bit_cast<float>(floatWords[(n + lane + 7) % floatWords.size()]);
                input[2][n][lane] = std::bit_cast<float>(static_cast<std::uint32_t>(n % 3));
            }
        }
    }
    constexpr std::array pairs{std::array{0,1},std::array{0,15},std::array{15,0},
                               std::array{14,15},std::array{0,0},std::array{15,15}};
    const std::string move = doubles ? "movapd" : "movaps";
    const std::string declarations = doubles ? "streamdoublein a,b,flag;streamdoubleout " : "streamin a,b,flag;streamout ";
    for (bool lowToHigh : {false, true}) {
        for (const auto pair : pairs) {
            const auto destination = "xmm" + std::to_string(pair[0]);
            const auto source = "xmm" + std::to_string(pair[1]);
            std::string code = declarations + "out,source,c,p,z,l;float zero=0,one=1;mov ecx,flag;cmp ecx,1;";
            for (unsigned window = 0; window < (doubles ? 4U : 1U); window += 2) {
                const auto cell = "[" + std::to_string(window) + "]";
                code += move + " " + destination + ",a" + cell + ";";
                if (pair[0] != pair[1]) code += move + " " + source + ",b" + cell + ";";
                // Exercise case-insensitive names and every low/high register pairing.
                code += (lowToHigh ? "MOVLHPS " : "movhlps ") + destination + "," + source + ";";
                code += move + " out" + cell + "," + destination + ";";
                code += move + " source" + cell + "," + source + ";";
            }
            constexpr std::array branches{"jb","jp","je","jl"};
            constexpr std::array outputs{"c","p","z","l"};
            for (unsigned i = 0; i < branches.size(); ++i) {
                const auto label = std::to_string(i);
                code += "movaps xmm7,zero;" + move + " " + outputs[i] + ",xmm7;" + branches[i] +
                    " yes" + label + ";jmp end" + label + ";yes" + label + ":";
                // Float constants need a numeric conversion for Double flag readouts.
                code += doubles ? "cvtps2pd xmm7,one;" : "movaps xmm7,one;";
                code += move + " " + outputs[i] + ",xmm7;end" + label + ":";
                if constexpr (doubles) {
                    code += move + " " + outputs[i] + "[2],xmm7;";
                }
            }
            const auto program = compileDspCode(code, DspLanguage::Assembler);
            for (auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
                if (!dspBackendAvailable(backend)) continue;
                Graph graph(backend);
                const auto block = graph.addDspCode(program);
                for (unsigned port = 0; port < 3; ++port) graph.connectDspInput(graph.addInput(), block.id, port);
                auto runtime = [&] {
                    if constexpr (doubles) return graph.compileMonoDouble4(block.outputs);
                    else return graph.compileMono4(block.outputs);
                }();
                acrossBuffers<6>(runtime, input, [&](const auto& output) {
                    for (std::size_t n = 0; n < count; ++n) {
                        const auto a = std::bit_cast<Words>(input[0][n]);
                        const auto b = pair[0] == pair[1] ? a : std::bit_cast<Words>(input[1][n]);
                        auto expected = a;
                        for (unsigned word = 0; word < expected.size(); word += 4) {
                            if (lowToHigh) { expected[word+2] = b[word]; expected[word+3] = b[word+1]; }
                            else { expected[word] = b[word+2]; expected[word+1] = b[word+3]; }
                        }
                        check(std::bit_cast<Words>(output[0][n]) == expected,
                              "Half moves preserve the untouched half and exact NaN/zero/subnormal bits, including self-aliasing and Double views");
                        check(std::bit_cast<Words>(output[1][n]) == (pair[0] == pair[1] ? expected : b),
                              "Half moves preserve a distinct source and read the original aliased source before storing");
                        const std::array flags{n%3 == 0,n%3 != 2,n%3 == 1,n%3 == 0};
                        for (unsigned i = 0; i < flags.size(); ++i) {
                            Sample value{};value.fill(flags[i] ? Scalar{1} : Scalar{0});
                            check(output[i+2][n] == value, "Half moves preserve earlier carry/parity/zero/signed flags");
                        }
                    }
                });
            }
        }
    }
}

void halfMoveControlFlowAndDiagnostics()
{
    for (const auto opcode : {"movhlps", "movlhps"}) {
        for (const auto pair : {std::array{0,1},std::array{0,15},std::array{15,0},std::array{14,15}}) {
            const auto a = "xmm" + std::to_string(pair[0]), b = "xmm" + std::to_string(pair[1]);
            const auto width = pair[0] >= 8 || pair[1] >= 8 ? 4 : 3;
            const auto program = compileDspCode("streamin a,b;streamout skip,call;movaps " + a + ",a;movaps " + b +
                ",b;jmp " + std::to_string(width) + ";" + opcode + " " + a + "," + b + ";movaps skip," + a +
                ";call helper;movaps call," + a + ";jmp end;helper:" + opcode + " " + a + "," + b + ";ret;end:", DspLanguage::Assembler);
            std::array<std::array<Mono4,1>,2> input{};
            input[0][0] = {1,2,3,4};input[1][0] = {5,6,7,8};
            for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
                if (!dspBackendAvailable(backend)) continue;
                Graph graph(backend);const auto block = graph.addDspCode(program);
                for (unsigned port = 0; port < 2; ++port) graph.connectDspInput(graph.addInput(),block.id,port);
                auto runtime = graph.compileMono4(block.outputs);
                acrossBuffers<2>(runtime,input,[&](const auto& output) {
                    check(output[0][0] == input[0][0], "Numeric jumps skip the three/four-byte half-move encoding");
                    check(output[1][0] == (std::string_view(opcode) == "movhlps" ? Mono4{7,8,3,4} : Mono4{1,2,5,6}),
                          "Half-move register results survive a local return");
                });
            }
        }
        for (const auto body : {"xmm0,a;", "a,xmm0;", "xmm0,eax;", "eax,xmm0;", "xmm0,[eax];",
                                "xmm0,1;", "xmm0,xmm1,0;"}) {
            bool rejected = false;
            try {
                static_cast<void>(compileDspCode(std::string("streamin a;memrefin data;streamout out;movaps xmm0,a;mov eax,data[0];")+
                    opcode+" "+body,DspLanguage::Assembler));
            } catch (const DspCompileError& e) { rejected = e.line > 0 && e.column > 0; }
            check(rejected, "Half moves require exactly two XMM registers");
        }
        bool rejected = false;
        try {
            static_cast<void>(compileDspCode(std::string("streamin a;streamout out;movaps xmm0,a;")+opcode+
                " xmm0,xmm0;jz end;end:",DspLanguage::Assembler));
        } catch (const DspCompileError&) { rejected = true; }
        check(rejected, "Half moves do not define EFLAGS");
    }
}

void registerAndMemoryViewsPreserveWords()
{
    const auto program = compileDspCode(R"(
        streamin a; streamin b;
        streamout copied; streamout partial; streamout cleared; streamout arrayCopy;
        streamout first; streamout guard;
        double cells[5] = {0x3ff12345deadbeef,2,3,0x400b1234abcdef01,0xc00d567812345678};
        movapd xmm0,a;
        movaps xmm1,xmm0;
        movapd xmm2,xmm1;
        movaps copied,xmm2;
        movaps xmm3,b;
        movss xmm2,xmm3;
        movaps partial,xmm2;
        movss xmm2,b;
        movaps cleared,xmm2;
        mov eax,8;
        movapd cells[eax],xmm0;
        movaps xmm4,cells[eax];
        movaps arrayCopy,xmm4;
        mov eax,4;
        movss cells[eax],xmm3;
        movaps xmm4,cells[0];
        movaps first,xmm4;
        movaps xmm4,cells[3];
        movaps guard,xmm4;
    )", DspLanguage::Assembler);
    constexpr std::array words{0x00000001U,0x80000001U,0x7f812345U,0xffc23456U,0x80000000U,0x00000000U,0x3f800123U};
    std::array<std::array<Mono4, frames>, 2> input{};
    for (std::size_t n = 0; n < frames; ++n) {
        for (unsigned lane = 0; lane < 4; ++lane) {
            input[0][n][lane] = std::bit_cast<float>(words[(n + lane) % words.size()]);
            input[1][n][lane] = std::bit_cast<float>(words[(n + lane + 3) % words.size()]);
        }
    }
    for (auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto block = graph.addDspCode(program);
        for (std::size_t port = 0; port < 2; ++port) graph.connectDspInput(graph.addInput(), block.id, port);
        auto runtime = graph.compileMono4(block.outputs);
        acrossBuffers<6>(runtime, input, [&](const auto& output) {
            for (std::size_t n = 0; n < frames; ++n) {
                const auto a = std::bit_cast<std::array<std::uint32_t, 4>>(input[0][n]);
                const auto b = std::bit_cast<std::array<std::uint32_t, 4>>(input[1][n]);
                const std::array expected{a, std::array{b[0],a[1],a[2],a[3]}, std::array{b[0],0U,0U,0U}, a,
                    std::array{0xdeadbeefU,b[0],a[0],a[1]}, std::array{0xabcdef01U,0x400b1234U,0x12345678U,0xc00d5678U}};
                for (std::size_t port = 0; port < expected.size(); ++port) {
                    check(std::bit_cast<std::array<std::uint32_t, 4>>(output[port][n]) == expected[port],
                          "MOVAPS/MOVAPD and partial MOVSS preserve physical DWORD order, payloads and adjacent Double cells");
                }
            }
        });
    }
}

void doubleRoundTripsAndMixedControlFlow()
{
    const auto input = doubleInputs();
    const auto program = compileDspCode(R"(
        streamdoublein a; streamdoubleout out;
        movapd xmm0,a[0]; movaps xmm1,xmm0; movapd xmm2,xmm1; movapd out[0],xmm2;
        movapd xmm0,a[2]; movaps xmm1,xmm0; movapd xmm2,xmm1; movapd out[2],xmm2;
    )", DspLanguage::Assembler);
    constexpr std::array bodies{
        "cmp flag,0;jz pd;movaps xmm0,a;jmp joined;pd:movapd xmm0,a;joined:movaps out,xmm0;",
        "movaps xmm0,a;mov eax,5;again:movapd xmm1,xmm0;movaps xmm0,xmm1;sub eax,1;jnz again;movaps out,xmm0;",
        "float saved[4];mov eax,helper;mov saved[3],eax;movapd xmm0,a;movss saved[0],xmm0;mov eax,saved[3];call eax;movaps out,xmm0;jmp done;helper:movapd xmm1,a;movaps xmm0,xmm1;ret;done:",
        "movaps xmm0,a;call helper;movaps out,xmm0;jmp done;helper:movapd xmm1,xmm0;movaps xmm0,xmm1;ret;done:",
    };
    for (auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto block = graph.addDspCode(program);
        graph.connectDspInput(graph.addInput(), block.id, 0);
        auto runtime = graph.compileMonoDouble4(block.outputs);
        const std::array<std::array<MonoDouble4, frames>, 1> values{input[0]};
        acrossBuffers<1>(runtime, values, [&](const auto& output) {
            for (std::size_t n = 0; n < frames; ++n) {
                check(std::bit_cast<std::array<std::uint64_t, 4>>(output[0][n]) ==
                      std::bit_cast<std::array<std::uint64_t, 4>>(values[0][n]), "Both 128-bit halves preserve every Double bit through PS/PD register views");
            }
        });
        for (const auto body : bodies) {
            const auto mixed = compileDspCode("streamin a;monoin flag;streamout out;" + std::string(body), DspLanguage::Assembler);
            Graph mixedGraph(backend);
            const auto ports = mixedGraph.addDspCode(mixed);
            mixedGraph.connectDspInput(mixedGraph.addInput(), ports.id, 0);
            mixedGraph.connectDspInput(mixedGraph.addInput(), ports.id, 1);
            auto mixedRuntime = mixedGraph.compileMono4(ports.outputs);
            std::array<std::array<Mono4, frames>, 2> bits{};
            for (std::size_t n = 0; n < frames; ++n) {
                bits[0][n] = {std::bit_cast<float>(0x7f812345U),std::bit_cast<float>(0x80000000U),std::bit_cast<float>(0x00000001U),1.0F};
                bits[1][n].fill(static_cast<float>(n & 1));
            }
            acrossBuffers<1>(mixedRuntime, bits, [&](const auto& output) {
                for (std::size_t n = 0; n < frames; ++n) {
                    check(std::bit_cast<std::array<std::uint32_t, 4>>(output[0][n]) ==
                          std::bit_cast<std::array<std::uint32_t, 4>>(bits[0][n]), "Mixed PS/PD writes stay initialized across joins, backward branches and local calls");
                }
            });
        }
    }
}

void arithmeticUsesTheInstructionView()
{
    const auto program = compileDspCode(R"(
        streamin a;streamout pd;streamout ps;
        float packedDoubles={0,0x3ff00000,0,0x40000000};
        double packedFloats={0xc01000003fc00000,0xc090000040500000,0,0};
        float ones=1;
        movaps xmm0,a;addpd xmm0,packedDoubles;movaps pd,xmm0;
        movapd xmm1,packedFloats;addps xmm1,ones;movaps ps,xmm1;
    )",DspLanguage::Assembler);
    std::array<std::array<Mono4,frames>,1> input{};
    for (std::size_t n=0;n<frames;++n) {
        const std::array values{1.0+static_cast<double>(n)+0x1p-40,-3.5-static_cast<double>(n)};
        for (unsigned lane=0;lane<2;++lane) {
            const auto bits=std::bit_cast<std::uint64_t>(values[lane]);
            input[0][n][lane*2]=std::bit_cast<float>(static_cast<std::uint32_t>(bits));
            input[0][n][lane*2+1]=std::bit_cast<float>(static_cast<std::uint32_t>(bits>>32));
        }
    }
    for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto block=graph.addDspCode(program);
        graph.connectDspInput(graph.addInput(),block.id,0);
        auto runtime=graph.compileMono4(block.outputs);
        acrossBuffers<2>(runtime,input,[&](const auto& output) {
            for (std::size_t n=0;n<frames;++n) {
                const auto actual=std::bit_cast<std::array<std::uint32_t,4>>(output[0][n]);
                const std::array expected{std::bit_cast<std::uint64_t>(2.0+static_cast<double>(n)+0x1p-40),
                                          std::bit_cast<std::uint64_t>(-1.5-static_cast<double>(n))};
                for (unsigned lane=0;lane<2;++lane) {
                    check((std::uint64_t{actual[lane*2]}|(std::uint64_t{actual[lane*2+1]}<<32))==expected[lane],
                          "ADDPD reads two doubles from raw PS registers and Float-declared memory without narrowing");
                }
                check(output[1][n]==Mono4{2.5F,-1.25F,4.25F,-3.5F},
                      "ADDPS reads four floats from a PD register loaded from Double-declared memory");
            }
        });
    }
}

void scalarWritesAndBoundsUsePhysicalBytes()
{
    const auto program = compileDspCode(R"(
        streamin a; streamout low; streamout full; streamout integerWord; streamout zero; streamout intact;
        double cell=0x3ff12345deadbeef; float small[1]=0x3f800001;
        movaps xmm0,a;
        movd cell,xmm0; movd xmm1,cell; movaps low,xmm1;
        movaps xmm1,cell; movaps full,xmm1;
        movd eax,xmm0; movd xmm1,eax; movaps integerWord,xmm1;
        movapd xmm1,small[0]; movaps zero,xmm1;
        movapd small[0],xmm0; movaps xmm1,small[0]; movaps intact,xmm1;
    )", DspLanguage::Assembler);
    std::array<std::array<Mono4, frames>, 1> input{};
    for (std::size_t n = 0; n < frames; ++n) input[0][n] = {std::bit_cast<float>(std::uint32_t{0x7f812340} + static_cast<std::uint32_t>(n)),1,2,3};
    for (auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto block = graph.addDspCode(program);
        graph.connectDspInput(graph.addInput(), block.id, 0);
        auto runtime = graph.compileMono4(block.outputs);
        acrossBuffers<5>(runtime, input, [&](const auto& output) {
            for (std::size_t n = 0; n < frames; ++n) {
                const auto low = std::bit_cast<std::uint32_t>(input[0][n][0]);
                const std::array expected{std::array{low,0U,0U,0U}, std::array{low,0x3ff12345U,0xdeadbeefU,0x3ff12345U},
                    std::array{low,0U,0U,0U},std::array{0U,0U,0U,0U},std::array{0x3f800001U,0U,0U,0U}};
                for (std::size_t port = 0; port < expected.size(); ++port) {
                    check(std::bit_cast<std::array<std::uint32_t,4>>(output[port][n]) == expected[port],
                          "MOVD preserves adjacent DWORDs in Double storage; incomplete double loads zero and stores skip");
                }
            }
        });
    }
    const auto boolean = compileDspCode(R"(
        streamboolin gate; streamboolout mask; streamout numeric; float one=1;
        movaps xmm0,gate; movaps mask,xmm0; movaps xmm1,mask; andps xmm1,one; movaps numeric,xmm1;
    )", DspLanguage::Assembler);
    std::array<std::array<Mono4, frames>, 1> gates{};
    for (std::size_t n = 0; n < frames; ++n) for (unsigned lane=0; lane<4; ++lane) gates[0][n][lane]=static_cast<float>((n+lane)&1);
    for (auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto block = graph.addDspCode(boolean);
        graph.connectDspInput(graph.addInput(),block.id,0);
        auto runtime=graph.compileMono4(block.outputs);
        acrossBuffers<2>(runtime,gates,[&](const auto& output) {
            check(output[1]==gates[0], "Raw Boolean stores clear the full logical false value after a true mask");
        });
    }
    const auto descriptor = compileDspCode("memrefin mem;streamout out;movd xmm0,mem[1];movaps out,xmm0;", DspLanguage::Assembler);
    for (auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto block = graph.addDspCode(descriptor);
        auto runtime = graph.compileMono4(block.outputs);
        const std::array samples{1.0F,2.0F};
        check(runtime.setDspMemRefInput(block.id,0,openstone::green::Memory::fromSamples(samples)), "Bind MOVD descriptor input");
        std::array<Mono4,1> output{};
        check(runtime.process({},std::array{Mono4OutputBuffer{output}}) &&
              std::bit_cast<std::array<std::uint32_t,4>>(output[0])==std::array{8U,0U,0U,0U},
              "MOVD retains scalar descriptor-field reads and clears the upper words");
    }
    for (const auto body : {
        "streamout out;int saved[4];double zero;mov eax,helper;mov saved[3],eax;movapd xmm0,zero;movapd saved[0],xmm0;mov eax,saved[3];call eax;jmp done;helper:ret;done:",
    }) {
        bool rejected=false;
        try { static_cast<void>(compileDspCode(body,DspLanguage::Assembler)); }
        catch (const DspCompileError& error) {
            const std::string_view message=error.what();
            rejected=message.contains("before reading it on every path") || message.contains("call register must hold a local subroutine label");
        }
        check(rejected,"Raw XMM writes invalidate every overwritten saved target cell");
    }
}

template<typename Word>
struct GreaterCase { Word a, b; bool result; };

template<typename Word, std::size_t Cases>
void greaterAliasesUseFlowStoneMasks(const std::array<GreaterCase<Word>, Cases>& cases)
{
    constexpr bool doubles = sizeof(Word) == 8;
    using Scalar = std::conditional_t<doubles, double, float>;
    using Sample = std::array<Scalar, 4>;
    const std::string suffix = doubles ? "pd" : "ps";
    const std::string move = doubles ? "movapd" : "movaps";
    const std::string compare = "cmpgt" + suffix;
    std::string source = doubles ? "streamdoublein a,b;" : "streamin a,b;";
    source += "memrefin data;";
    source += doubles ? "streamdoubleout " : "streamout ";
    source += "named,reg,indexed,indirect,explicitMask,nle,self,right,skipReg,skipMemory;int cursor=0;"
              "mov eax,data[0];mov ebx,cursor;";
    // Both physical 128-bit windows are needed for a four-lane Double signal.
    for (unsigned window = 0; window < (doubles ? 4U : 1U); window += 2) {
        const std::string cell = "[" + std::to_string(window) + "]";
        const auto load = [&](std::string_view reg) { return move + " " + std::string(reg) + ",a" + cell + ";"; };
        const auto store = [&](std::string_view port, std::string_view reg = "xmm0") {
            return move + " " + std::string(port) + cell + "," + std::string(reg) + ";";
        };
        source += move + " xmm15,b" + cell + ";";
        source += load("xmm0") + compare + " xmm0,b" + cell + ";" + store("named");
        source += load("xmm0") + (doubles ? "CMPGTPD" : "CMPGTPS") + " xmm0,xmm15;" + store("reg");
        source += "mov ecx," + std::to_string(window * sizeof(Scalar)) + ";";
        source += load("xmm0") + compare + " xmm0,b[ecx];" + store("indexed");
        source += "mov edx,ebx;add edx," + std::to_string(window * sizeof(Scalar)) + ";";
        source += load("xmm0") + compare + " xmm0,[eax+edx];" + store("indirect");
        source += load("xmm0") + "cmp" + suffix + " xmm0,b" + cell + ",6;" + store("explicitMask");
        source += load("xmm0") + "cmpnle" + suffix + " xmm0,b" + cell + ";" + store("nle");
        source += load("xmm0") + compare + " xmm0,xmm0;" + store("self");
        source += store("right", "xmm15");
        // A skipped compare would turn true masks into NaNs and change a second comparison.
        source += load("xmm8") + "jmp " + (doubles ? "6" : "5") + ";" + compare + " xmm8,xmm15;";
        source += compare + " xmm8,b" + cell + ";" + store("skipReg", "xmm8");
        source += load("xmm8") + "jmp " + (doubles ? "10" : "9") + ";" + compare + " xmm8,b" + cell + ";";
        source += compare + " xmm8,xmm15;" + store("skipMemory", "xmm8");
    }
    source += "add ebx," + std::to_string(sizeof(Sample)) + ";mov cursor,ebx;";
    const auto program = compileDspCode(source, DspLanguage::Assembler);
    std::array<std::array<Sample, frames>, 2> input{};
    std::vector<std::uint8_t> bytes;
    for (std::size_t n = 0; n < frames; ++n) for (unsigned lane = 0; lane < 4; ++lane) {
        const auto& test = cases[(n + 7 * lane) % cases.size()];
        input[0][n][lane] = std::bit_cast<Scalar>(test.a);
        input[1][n][lane] = std::bit_cast<Scalar>(test.b);
        for (unsigned byte = 0; byte < sizeof(Word); ++byte) bytes.push_back(static_cast<std::uint8_t>(test.b >> (8 * byte)));
    }
    const openstone::green::Memory memory(std::move(bytes));
    for (auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto block = graph.addDspCode(program);
        for (unsigned port = 0; port < 2; ++port) graph.connectDspInput(graph.addInput(), block.id, port);
        auto runtime = [&] {
            if constexpr (doubles) return graph.compileMonoDouble4(block.outputs);
            else return graph.compileMono4(block.outputs);
        }();
        check(runtime.setDspMemRefInput(block.id, 2, memory), "Bind greater-than comparison memory");
        acrossBuffers<10>(runtime, input, [&](const auto& output) {
            for (std::size_t n = 0; n < frames; ++n) for (unsigned lane = 0; lane < 4; ++lane) {
                const auto& test = cases[(n + 7 * lane) % cases.size()];
                const Word expected = test.result ? ~Word{0} : Word{0};
                for (unsigned port : {0,1,2,3,4,5,8,9}) {
                    check(std::bit_cast<Word>(output[port][n][lane]) == expected,
                          "CMPGT aliases preserve exact NLE masks for each operand form, alias and numeric branch width");
                }
                const Word magnitude = test.a & (~Word{0} >> 1);
                const Word infinity = doubles ? static_cast<Word>(0x7ff0000000000000ULL) : static_cast<Word>(0x7f800000U);
                check(std::bit_cast<Word>(output[6][n][lane]) == (magnitude > infinity ? ~Word{0} : Word{0}) &&
                      std::bit_cast<Word>(output[7][n][lane]) == test.b,
                      "Self-comparison is true only for NaN and register sources retain their exact bits");
            }
        });
    }
}

void greaterAliasesIncludeNaNsAndSignedZeros()
{
    // Expected flags are explicit: FlowStone binds CMPGT to SSE predicate 6 (NLE/GT-or-unordered).
    // In particular, NaNs are true and all signed-zero pairings are false. Subnormals flush before comparing.
    constexpr std::array<GreaterCase<std::uint32_t>, 29> floatCases{{
        {0x00000000,0x80000000,false},
        {0x80000000,0x00000000,false}, {0x00000000,0x00000000,false}, {0x80000000,0x80000000,false},
        {0x3f800000,0x00000000,true}, {0x00000000,0x3f800000,false}, {0xbf800000,0xc0000000,true},
        {0xc0000000,0xbf800000,false}, {0x3f800000,0x3f800000,false},
        {0x7f800000,0x7f7fffff,true}, {0x7f7fffff,0x7f800000,false}, {0xff800000,0xbf800000,false},
        {0xbf800000,0xff800000,true}, {0x7f800000,0x7f800000,false}, {0xff800000,0xff800000,false},
        {0x7fc12345,0x00000000,true}, {0x00000000,0x7fc12345,true}, {0xffc54321,0x3f800000,true},
        {0x3f800000,0xffc54321,true}, {0x7f812345,0x00000000,true}, {0x00000000,0xff854321,true},
        {0x7f812345,0x7f812345,true}, {0x7fc12345,0xffc54321,true},
        {0x00000001,0x00000000,false}, {0x00000000,0x80000001,false}, {0x00800000,0x00000000,true},
        {0x80800000,0x00000000,false}, {0x3f800001,0x3f800000,true}, {0x3f800000,0x3f800001,false},
    }};
    greaterAliasesUseFlowStoneMasks(floatCases);
    constexpr std::array<GreaterCase<std::uint64_t>, 29> doubleCases{{
        {0x0000000000000000,0x8000000000000000,false},
        {0x8000000000000000,0x0000000000000000,false}, {0x0000000000000000,0x0000000000000000,false},
        {0x8000000000000000,0x8000000000000000,false}, {0x3ff0000000000000,0x0000000000000000,true},
        {0x0000000000000000,0x3ff0000000000000,false}, {0xbff0000000000000,0xc000000000000000,true},
        {0xc000000000000000,0xbff0000000000000,false}, {0x3ff0000000000000,0x3ff0000000000000,false},
        {0x7ff0000000000000,0x7fefffffffffffff,true}, {0x7fefffffffffffff,0x7ff0000000000000,false},
        {0xfff0000000000000,0xbff0000000000000,false}, {0xbff0000000000000,0xfff0000000000000,true},
        {0x7ff0000000000000,0x7ff0000000000000,false}, {0xfff0000000000000,0xfff0000000000000,false},
        {0x7ff8123456789abc,0x0000000000000000,true}, {0x0000000000000000,0x7ff8123456789abc,true},
        {0xfff8543212345678,0x3ff0000000000000,true}, {0x3ff0000000000000,0xfff8543212345678,true},
        {0x7ff0123456789abc,0x0000000000000000,true}, {0x0000000000000000,0xfff0543212345678,true},
        {0x7ff0123456789abc,0x7ff0123456789abc,true}, {0x7ff8123456789abc,0xfff8543212345678,true},
        {0x0000000000000001,0x0000000000000000,false}, {0x0000000000000000,0x8000000000000001,false},
        {0x0010000000000000,0x0000000000000000,true}, {0x8010000000000000,0x0000000000000000,false},
        {0x3ff0000000001000,0x3ff0000000000000,true}, {0x3ff0000000000000,0x3ff0000000001000,false},
    }};
    greaterAliasesUseFlowStoneMasks(doubleCases);
    for (const auto instruction : {"cmpgtps", "cmpgtpd"}) {
        for (const auto body : {"movaps xmm0,in;" + std::string(instruction) + " out,xmm0;",
                "movaps xmm0,in;" + std::string(instruction) + " xmm0,in,6;"}) {
            bool rejected = false;
            try { static_cast<void>(compileDspCode("streamin in;streamout out;" + body, DspLanguage::Assembler)); }
            catch (const DspCompileError&) { rejected = true; }
            check(rejected, "CMPGT aliases require an XMM destination and exactly two operands");
        }
    }
}

std::string fixture(std::string_view name)
{
    std::ifstream input(std::filesystem::path(OPENSTONE_TEST_FIXTURES) / "flowstone" / name, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read unchanged raw-XMM fixture");
    return {std::istreambuf_iterator<char>(input), {}};
}

bool comparison(std::uint64_t a, std::uint64_t b, unsigned predicate)
{
    const auto isNan = [](std::uint64_t word) { return (word & 0x7fffffffffffffffULL) > 0x7ff0000000000000ULL; };
    const bool unordered = isNan(a) || isNan(b);
    if (unordered) return predicate == 5 || predicate == 6;
    const auto flush = [](std::uint64_t word) {
        return (word & 0x7fffffffffffffffULL) < 0x0010000000000000ULL ? word & 0x8000000000000000ULL : word;
    };
    const auto left = std::bit_cast<double>(flush(a)), right = std::bit_cast<double>(flush(b));
    switch (predicate) {
    case 0: return left == right;
    case 1: return left < right;
    case 2: return left <= right;
    case 5: return left >= right;
    case 6: return left > right;
    default: throw std::runtime_error("Unexpected comparison fixture predicate");
    }
}

void unchangedComparisonsAndSwitch()
{
    const auto inputs = doubleInputs();
    constexpr std::array names{"assembler-double-equal.asm", "assembler-double-less.asm", "assembler-double-less-equal.asm",
                               "assembler-double-greater-equal.asm", "assembler-double-greater.asm"};
    constexpr std::array predicates{0U,1U,2U,5U,6U};
    for (std::size_t test = 0; test < names.size(); ++test) {
        const auto program = compileDspCode(fixture(names[test]), DspLanguage::Assembler);
        for (auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);
            const auto block = graph.addDspCode(program);
            for (std::size_t port = 0; port < 2; ++port) graph.connectDspInput(graph.addInput(), block.id, port);
            auto runtime = graph.compileMonoDouble4(block.outputs);
            const std::array<std::array<MonoDouble4, frames>, 2> values{inputs[0],inputs[1]};
            acrossBuffers<1>(runtime, values, [&](const auto& output) {
                for (std::size_t n = 0; n < frames; ++n) {
                    std::array<std::uint64_t, 4> expected{};
                    for (unsigned lane = 0; lane < 4; ++lane) {
                        const auto a = std::bit_cast<std::uint64_t>(inputs[0][n][lane]);
                        const auto b = std::bit_cast<std::uint64_t>(inputs[1][n][lane]);
                        if (comparison(a,b,predicates[test])) expected[lane / 2] |= std::uint64_t{UINT32_MAX} << ((lane % 2) * 32);
                    }
                    check(std::bit_cast<std::array<std::uint64_t, 4>>(output[0][n]) == expected,
                          "Unchanged comparison compacts all four CMPPD masks with SHUFPS and stores precisely 128 bits");
                }
            });
        }
    }
    const auto program = compileDspCode(fixture("assembler-double-switch.asm"), DspLanguage::Assembler);
    for (auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);
        const auto block = graph.addDspCode(program);
        for (std::size_t port = 0; port < 3; ++port) graph.connectDspInput(graph.addInput(), block.id, port);
        auto runtime = graph.compileMonoDouble4(block.outputs);
        acrossBuffers<1>(runtime, inputs, [&](const auto& output) {
            for (std::size_t n = 0; n < frames; ++n) {
                const auto actual = std::bit_cast<std::array<std::uint64_t, 4>>(output[0][n]);
                for (unsigned lane = 0; lane < 4; ++lane) {
                    const auto expected = std::bit_cast<std::uint64_t>(inputs[inputs[2][n][lane] == 0 ? 0 : 1][n][lane]);
                    check(actual[lane] == expected, "Unchanged double switch preserves each selected payload, signed zero and subnormal");
                }
            }
        });
    }
}
}

int main()
{
    try {
        halfMovesPreserveRawBits<Mono4>();
        halfMovesPreserveRawBits<MonoDouble4>();
        halfMoveControlFlowAndDiagnostics();
        registerAndMemoryViewsPreserveWords();
        doubleRoundTripsAndMixedControlFlow();
        arithmeticUsesTheInstructionView();
        scalarWritesAndBoundsUsePhysicalBytes();
        openstone::tests::withReferenceSources("unchangedComparisonsAndSwitch", [&] { unchangedComparisonsAndSwitch(); });
        greaterAliasesIncludeNaNsAndSignedZeros();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
