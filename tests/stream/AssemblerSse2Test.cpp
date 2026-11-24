#include "../ReferenceFixtures.hpp"
#include "stream/Runtime.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace openstone::stream;

void check(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error(std::string(message));
}

struct ConversionCase { double value; std::int32_t nearest, truncated; };
constexpr auto indefinite = std::numeric_limits<std::int32_t>::min();
constexpr auto maximum = std::numeric_limits<std::int32_t>::max();
constexpr ConversionCase conversions[]{
    {0.0, 0, 0}, {-0.0, 0, 0}, {0.49, 0, 0}, {-0.49, 0, 0},
    {0.5, 0, 0}, {1.5, 2, 1}, {2.5, 2, 2}, {3.5, 4, 3},
    {-0.5, 0, 0}, {-1.5, -2, -1}, {-2.5, -2, -2}, {-3.5, -4, -3},
    {1.5 + 0x1p-40, 2, 1}, {1.5 - 0x1p-40, 1, 1},
    {-1.5 + 0x1p-40, -1, -1}, {-1.5 - 0x1p-40, -2, -1},
    {16777217.0, 16777217, 16777217}, {-16777217.0, -16777217, -16777217},
    {2147483647.0, maximum, maximum}, {2147483647.49, maximum, maximum},
    {2147483647.5, indefinite, maximum}, {2147483648.0, indefinite, indefinite},
    {-2147483648.0, indefinite, indefinite}, {-2147483648.5, indefinite, indefinite},
    {-2147483648.75, indefinite, indefinite}, {-2147483649.0, indefinite, indefinite},
    {std::numeric_limits<double>::infinity(), indefinite, indefinite},
    {-std::numeric_limits<double>::infinity(), indefinite, indefinite},
    {std::bit_cast<double>(0x7ff8123456789abcULL), indefinite, indefinite},
    {std::bit_cast<double>(0xfff0000000000042ULL), indefinite, indefinite},
    {std::bit_cast<double>(0x0000000000000001ULL), 0, 0},
    {std::bit_cast<double>(0x8000000000000001ULL), 0, 0},
};

openstone::green::Memory pairs(std::span<const MonoDouble4> frames)
{
    std::vector<std::uint8_t> bytes;
    for (const auto& frame : frames) {
        for (unsigned lane = 0; lane < 2; ++lane) {
            const auto word = std::bit_cast<std::uint64_t>(frame[lane]);
            for (unsigned byte = 0; byte < 8; ++byte) bytes.push_back(static_cast<std::uint8_t>(word >> (byte * 8)));
        }
    }
    return openstone::green::Memory(std::move(bytes));
}

void conversionsUseTwoDoublesAndClearTheUpperWords()
{
    struct Restore {
        std::fenv_t environment;
        Restore() { std::fegetenv(&environment); }
        ~Restore() { std::fesetenv(&environment); }
    } restore;
    constexpr auto frames = std::size(conversions);
    std::array<MonoDouble4, frames> input{};
    for (std::size_t n = 0; n < frames; ++n) {
        input[n] = {conversions[n].value, conversions[(n + 11) % frames].value, 9999.5, -8888.5};
    }
    const auto memory = pairs(input);
    for (bool nearest : {true, false}) {
        const std::string opcode = nearest ? "cvtpd2dq" : "cvttpd2dq";
        const std::array forms{
            opcode + " xmm0,values;",
            "movapd xmm7,values;" + opcode + " xmm0,xmm7;",
            "movapd xmm0,values;" + opcode + " xmm0,xmm0;",
            "mov eax,0;" + opcode + " xmm0,values[eax];",
            "mov eax,data[0];mov ebx,cursor;" + opcode + " xmm0,[eax+ebx*8];inc ebx;inc ebx;mov cursor,ebx;",
        };
        for (const auto& body : forms) {
            const auto program = compileDspCode(
                "streamdoublein values;memrefin data;streamout raw;streamdoubleout numeric;int cursor=0;" + body +
                "movaps raw,xmm0;cvtdq2pd xmm1,xmm0;movapd numeric,xmm1;", DspLanguage::Assembler);
            for (auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
                if (!dspBackendAvailable(backend)) continue;
                Graph graph(backend);
                const auto block = graph.addDspCode(program);
                graph.connectDspInput(graph.addInput(), block.id, 0);
                auto runtime = graph.compileMonoDouble4(block.outputs);
                for (const std::size_t size : {1, 3, 7, 16, 64}) {
                    runtime.reset();
                    check(runtime.setDspMemRefInput(block.id, 1, memory), "Bind packed double conversion memory");
                    std::array<MonoDouble4, frames> raw{}, numeric{};
                    check(std::fesetround(FE_UPWARD) == 0, "Set a non-default host rounding mode");
                    for (std::size_t offset = 0; offset < frames; offset += size) {
                        const auto count = std::min(size, frames - offset);
                        check(runtime.process(std::array{MonoDouble4InputBuffer{std::span{input}.subspan(offset, count)}},
                            std::array{MonoDouble4OutputBuffer{std::span{raw}.subspan(offset, count)},
                                       MonoDouble4OutputBuffer{std::span{numeric}.subspan(offset, count)}}),
                              "Render double-to-integer conversions across buffers");
                    }
                    check(std::fegetround() == FE_UPWARD, "Conversions restore the caller's rounding mode");
                    std::fesetround(FE_TONEAREST);
                    for (std::size_t n = 0; n < frames; ++n) {
                        for (unsigned lane = 0; lane < 4; ++lane) {
                            const auto test = conversions[(n + (lane == 1 ? 11 : 0)) % frames];
                            const auto expected = lane >= 2 ? 0 : nearest ? test.nearest : test.truncated;
                            check(std::bit_cast<std::uint32_t>(static_cast<float>(raw[n][lane])) == static_cast<std::uint32_t>(expected),
                                  "Double conversions preserve signed integer words, invalid results, and cleared upper lanes");
                            check(numeric[n][lane] == static_cast<double>(expected),
                                  "Double conversions retain precision past binary32 and use the specified rounding");
                        }
                    }
                }
            }
        }
    }
}

void shufflePreservesRawDoubleWords()
{
    constexpr std::array words{
        0x0000000000000000ULL, 0x8000000000000000ULL, 0x7ff0000000000042ULL,
        0xfff8123456789abcULL, 0x0000000000000001ULL, 0x8000000000000001ULL,
        0x7ff0000000000000ULL, 0xfff0000000000000ULL, 0x3ff0000000001000ULL,
    };
    constexpr std::size_t frames = 19;
    std::array<MonoDouble4, frames> a{}, b{};
    for (std::size_t n = 0; n < frames; ++n) {
        for (std::size_t lane = 0; lane < 4; ++lane) {
            a[n][lane] = std::bit_cast<double>(words[(n + lane) % words.size()]);
            b[n][lane] = std::bit_cast<double>(words[(n + lane + 4) % words.size()]);
        }
    }
    const auto memory = pairs(b);
    for (const unsigned control : {0, 1, 2, 3, 252, 253, 254, 255}) {
        for (unsigned form = 0; form < 5; ++form) {
            const std::array<std::string, 5> prepare{
                "", "movapd xmm1,b;", "mov eax,0;",
                "mov eax,data[0];mov ebx,cursor;inc ebx;inc ebx;mov cursor,ebx;sub ebx,2;", "",
            };
            constexpr std::array operands{"b", "xmm1", "b[eax]", "[eax+ebx*8]", "xmm0"};
            const auto program = compileDspCode(
                "streamdoublein a;streamdoublein b;memrefin data;streamdoubleout out;int cursor=0;" +
                prepare[form] + "movapd xmm0,a;shufpd xmm0," + operands[form] + "," + std::to_string(control) +
                ";movapd out,xmm0;", DspLanguage::Assembler);
            for (auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
                if (!dspBackendAvailable(backend)) continue;
                Graph graph(backend);
                const auto block = graph.addDspCode(program);
                graph.connectDspInput(graph.addInput(), block.id, 0);
                graph.connectDspInput(graph.addInput(), block.id, 1);
                auto runtime = graph.compileMonoDouble4(block.outputs);
                for (const std::size_t size : {1, 3, 7, 32}) {
                    runtime.reset();
                    check(runtime.setDspMemRefInput(block.id, 2, memory), "Bind SHUFPD memory");
                    std::array<MonoDouble4, frames> output{};
                    for (std::size_t offset = 0; offset < frames; offset += size) {
                        const auto count = std::min(size, frames - offset);
                        check(runtime.process(std::array{MonoDouble4InputBuffer{std::span{a}.subspan(offset, count)},
                                                         MonoDouble4InputBuffer{std::span{b}.subspan(offset, count)}},
                            std::array{MonoDouble4OutputBuffer{std::span{output}.subspan(offset, count)}}), "Render SHUFPD operands");
                    }
                    for (std::size_t n = 0; n < frames; ++n) {
                        const auto& right = form == 4 ? a[n] : b[n];
                        const MonoDouble4 expected{a[n][control & 1U], right[(control >> 1) & 1U], 0, 0};
                        check(std::bit_cast<std::array<std::uint64_t, 4>>(output[n]) ==
                              std::bit_cast<std::array<std::uint64_t, 4>>(expected),
                              "SHUFPD selects raw qwords, ignores high control bits, and reads aliases before writing");
                    }
                }
            }
        }
    }
}

std::string fixture(std::string_view name)
{
    std::ifstream input(std::filesystem::path(OPENSTONE_TEST_FIXTURES) / "flowstone" / name, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read bundled SSE2 source fixture");
    return {std::istreambuf_iterator<char>(input), {}};
}

void unchangedBundledKernelsRenderAcrossBuffers()
{
    constexpr std::size_t frames = std::size(conversions);
    for (unsigned test = 0; test < 4; ++test) {
        constexpr std::array names{"assembler-double-round.asm", "assembler-double-truncate.asm",
                                   "assembler-double-pack.asm", "assembler-double-unpack.asm"};
        const auto program = compileDspCode(fixture(names[test]), DspLanguage::Assembler);
        for (auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);
            const auto block = graph.addDspCode(program);
            for (std::size_t port = 0; port < program.inputs().size(); ++port) graph.connectDspInput(graph.addInput(), block.id, port);
            auto runtime = graph.compileMonoDouble4(block.outputs);
            for (const std::size_t size : {1, 7, 31, 128}) {
                runtime.reset();
                std::array<std::array<MonoDouble4, frames>, 4> inputs{}, output{};
                for (std::size_t port = 0; port < inputs.size(); ++port) {
                    for (std::size_t n = 0; n < frames; ++n) {
                        for (unsigned lane = 0; lane < 4; ++lane) {
                            // Keep fixture oracles numeric; raw NaN payloads are covered above.
                            inputs[port][n][lane] = conversions[(n + lane + 3 * port) % 26].value;
                        }
                    }
                }
                for (std::size_t offset = 0; offset < frames; offset += size) {
                    const auto count = std::min(size, frames - offset);
                    std::vector<MonoDouble4InputBuffer> in;
                    std::vector<MonoDouble4OutputBuffer> out;
                    for (std::size_t port = 0; port < program.inputs().size(); ++port) in.emplace_back(std::span{inputs[port]}.subspan(offset, count));
                    for (std::size_t port = 0; port < program.outputs().size(); ++port) out.emplace_back(std::span{output[port]}.subspan(offset, count));
                    check(runtime.process(in, out), "Render unchanged bundled SSE2 kernels");
                }
                for (std::size_t n = 0; n < frames; ++n) {
                    for (unsigned lane = 0; lane < 4; ++lane) {
                        if (test < 2) {
                            const auto expected = conversions[(n + lane) % 26];
                            check(output[0][n][lane] == (test == 0 ? expected.nearest : expected.truncated),
                                  "Unchanged bundled rounding and truncation process all four double lanes");
                        } else if (test == 2) {
                            check(output[0][n][lane] == inputs[lane][n][0], "Unchanged bundled double Pack preserves lane order");
                        } else {
                            check(output[lane][n][0] == inputs[0][n][lane], "Unchanged bundled double Unpack produces each scalar output");
                        }
                    }
                }
            }
        }
    }
}

void numericBranchesUseSse2EncodingWidths()
{
    // Each relative jump skips exactly one instruction, including its SSE prefix,
    // immediate and optional high-register REX prefix or named-memory displacement.
    constexpr std::array skipped{
        "cvtpd2dq xmm0,xmm1;", "cvttpd2dq xmm0,values;", "shufpd xmm0,xmm1,0;", "shufpd xmm8,values,255;",
    };
    constexpr std::array sizes{4, 8, 5, 10};
    for (std::size_t test = 0; test < skipped.size(); ++test) {
        const auto program = compileDspCode(
            "streamdoublein values;streamdoubleout out;movapd xmm0,values;movapd xmm1,values;movapd xmm8,values;jmp " +
            std::to_string(sizes[test]) + ";" + skipped[test] + "movapd out,xmm0;", DspLanguage::Assembler);
        for (auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);
            const auto block = graph.addDspCode(program);
            graph.connectDspInput(graph.addInput(), block.id, 0);
            auto runtime = graph.compileMonoDouble4(block.outputs);
            const std::array<MonoDouble4, 1> input{{{1.25, -2.75, 3.5, -4.5}}};
            std::array<MonoDouble4, 1> output{};
            check(runtime.process(std::array{MonoDouble4InputBuffer{input}}, std::array{MonoDouble4OutputBuffer{output}}) && output[0] == MonoDouble4{input[0][0], input[0][1], 0, 0},
                  "Numeric branch displacements skip the new SSE2 instruction encodings");
        }
    }
    for (const auto invalid : {"double a; cvtpd2dq a,a;", "double a; cvttpd2dq a,a;",
                               "double a; movapd xmm0,a; shufpd xmm0,a,256;", "double a; movapd xmm0,a; shufpd xmm0,a,-1;"}) {
        bool rejected = false;
        try { static_cast<void>(compileDspCode(invalid, DspLanguage::Assembler)); }
        catch (const DspCompileError&) { rejected = true; }
        check(rejected, "SSE2 conversions and shuffle reject invalid destinations and immediates");
    }
}
}

int main()
{
    try {
        conversionsUseTwoDoublesAndClearTheUpperWords();
        shufflePreservesRawDoubleWords();
        openstone::tests::withReferenceSources("unchangedBundledKernelsRenderAcrossBuffers", [&] { unchangedBundledKernelsRenderAcrossBuffers(); });
        numericBranchesUseSse2EncodingWidths();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
