#include "stream/Runtime.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace openstone::stream;
void check(bool value, std::string_view message) { if (!value) throw std::runtime_error(std::string(message)); }

// Literal Intel COMISS flag results: CF=bit 0, PF=bit 2, ZF=bit 6; SF/OF/AF clear.
struct Case { std::uint32_t a, b; unsigned flags; };
constexpr std::array<Case, 24> cases{{
    {0x40000000,0x3f800000,0}, {0x3f800000,0x40000000,1}, {0xbf800000,0xc0000000,0},
    {0xc0000000,0xbf800000,1}, {0x3f800000,0x3f800000,0x40},
    {0x00000000,0x80000000,0x40}, {0x80000000,0x00000000,0x40},
    {0x00000000,0x00000000,0x40}, {0x80000000,0x80000000,0x40},
    {0x7fc12345,0x3f800000,0x45}, {0x3f800000,0x7fc12345,0x45},
    {0xffc54321,0x7f800000,0x45}, {0xff800000,0xffc54321,0x45},
    {0x7f812345,0x00000000,0x45}, {0x00000000,0xff854321,0x45},
    {0x7f812345,0x7fc12345,0x45}, {0x7f800000,0x7f800000,0x40},
    {0x7f800000,0x7f7fffff,0}, {0xff800000,0xff7fffff,1},
    {0x00000001,0x00000000,0x40}, {0x00000000,0x80000001,0x40},
    {0x00800000,0x00000000,0}, {0x3f800001,0x3f800000,0}, {0x3f800000,0x3f800001,1},
}};
constexpr std::size_t frames = 53;
constexpr std::array branches{"je","jz","jne","jnz","ja","jb","jbe","jae","jp","jnp","jg","jl","jng","jnl","jge","jle"};

constexpr std::size_t outputCount = branches.size() + 2;

bool taken(unsigned branch, unsigned flags)
{
    const bool z = flags & 0x40, c = flags & 1, p = flags & 4;
    const std::array result{z,z,!z,!z,!c&&!z,c,c||z,!c,p,!p,!z,false,z,true,true,z};
    return result[branch];
}

std::string readBranch(std::string_view branch, unsigned id, std::string_view output)
{
    const auto label = std::to_string(id);
    return "movaps xmm7,zero;movaps " + std::string(output) + ",xmm7;" + std::string(branch) + " yes" + label +
           ";jmp done" + label + ";yes" + label + ":movaps xmm7,one;movaps " + std::string(output) + ",xmm7;done" + label + ":";
}

void flagsAndOperandForms()
{
    for (unsigned form = 0; form < 6; ++form) {
        std::string source = "streamin a,b;memrefin data;streamout ";
        for (unsigned branch = 0; branch < branches.size(); ++branch) source += "f" + std::to_string(branch) + ",";
        source += "left,right;float zero=0,one=1;int cursor=0;movaps xmm8,a;movaps xmm15,b;mov eax,data[0];mov ebx,cursor;mov edx,0;";
        constexpr std::array compare{
            "COMISS xmm8,xmm15;", "comiss xmm8,b;", "comiss xmm8,b[edx];",
            "comiss xmm8,b[2];", "comiss xmm8,[eax+ebx];", "comiss xmm8,xmm8;",
        };
        source += compare[form];
        // None of these writes, moves, conversions, stack operations or packed comparisons changes EFLAGS.
        source += "mov ecx,17;push ecx;pop ecx;movaps xmm0,xmm8;cmpps xmm0,xmm15,3;";
        for (unsigned branch = 0; branch < branches.size(); ++branch) source += readBranch(branches[branch],branch,"f"+std::to_string(branch));
        source += "movaps left,xmm8;movaps right,xmm15;add ebx,16;mov cursor,ebx;";
        const auto program = compileDspCode(source,DspLanguage::Assembler);
        std::array<std::array<Mono4,frames>,2> input{};
        std::vector<std::uint8_t> bytes;
        for (std::size_t n = 0; n < frames; ++n) {
            const auto test = cases[n % cases.size()];
            // Contradictory upper lanes, including signaling NaNs, must not affect the flags.
            input[0][n] = {std::bit_cast<float>(test.a),std::bit_cast<float>(0x7f812345U),-999,999};
            input[1][n] = {std::bit_cast<float>(test.b),-999,std::bit_cast<float>(0xff854321U),-999};
            if (form == 3) {
                input[1][n][0] = std::bit_cast<float>(0x7fc99999U);
                input[1][n][2] = std::bit_cast<float>(test.b);
            }
            for (auto value : input[1][n]) {
                const auto word = std::bit_cast<std::uint32_t>(value);
                for (unsigned byte = 0; byte < 4; ++byte) bytes.push_back(static_cast<std::uint8_t>(word >> (8*byte)));
            }
        }
        const openstone::green::Memory memory(std::move(bytes));
        for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);
            const auto block = graph.addDspCode(program);
            for (unsigned port = 0; port < 2; ++port) graph.connectDspInput(graph.addInput(),block.id,port);
            auto runtime = graph.compileMono4(block.outputs);
            check(runtime.setDspMemRefInput(block.id,2,memory),"Bind COMISS indirect source");
            for (const std::size_t size : {1,3,7,31,64}) {
                runtime.reset();
                std::array<std::array<Mono4,frames>,outputCount> output{};
                for (std::size_t offset = 0; offset < frames; offset += size) {
                    const auto count = std::min(size,frames-offset);
                    std::vector<Mono4OutputBuffer> out;
                    for (auto& port : output) out.emplace_back(std::span{port}.subspan(offset,count));
                    check(runtime.process(std::array{Mono4InputBuffer{std::span{input[0]}.subspan(offset,count)},
                                                     Mono4InputBuffer{std::span{input[1]}.subspan(offset,count)}},out),"Render COMISS flag branches");
                }
                for (std::size_t n = 0; n < frames; ++n) {
                    const auto test = cases[n % cases.size()];
                    const auto flags = form == 5 ? ((test.a & 0x7fffffffU)>0x7f800000U ? 0x45U : 0x40U) : test.flags;
                    for (unsigned branch = 0; branch < branches.size(); ++branch) {
                        const float expected = taken(branch,flags) ? 1.F : 0.F;
                        check(output[branch][n] == Mono4{expected,expected,expected,expected},
                              "COMISS low-lane flags drive zero, carry, parity and signed branches exactly");
                    }
                    for (unsigned port = 0; port < 2; ++port) check(
                        std::bit_cast<std::array<std::uint32_t,4>>(output[branches.size()+port][n]) ==
                        std::bit_cast<std::array<std::uint32_t,4>>(input[port][n]),
                        "COMISS preserves all source bits, including signed zeros, subnormals and NaN payloads");
                }
            }
        }
    }
}

void controlFlowAndWidths()
{
    const std::array bodies{
        std::string{"call classify;"},
        std::string{"cmp choose,0;jz other;comiss xmm8,b;jmp join;other:comiss xmm8,b;join:"},
        std::string{"jmp 4;comiss xmm8,xmm15;comiss xmm8,b;"},
        std::string{"jmp 8;comiss xmm8,b;comiss xmm8,xmm15;"},
        std::string{"comiss xmm8,b;mov eax,1;shl eax,0;shr eax,0;"},
        std::string{"mov eax,2;comiss xmm8,b;again:comiss xmm8,b;jp seen;seen:sub eax,1;jnz again;comiss xmm8,b;"},
    };
    for (const auto& body : bodies) {
        const std::string routine = body.starts_with("call") ? "jmp end;classify:comiss xmm8,b;ret;end:" : "";
        const auto program = compileDspCode("streamin a,b;monoin choose;streamout c,p,z;float zero=0,one=1;"
            "movaps xmm8,a;movaps xmm15,b;"+body+readBranch("jb",0,"c")+readBranch("jp",1,"p")+readBranch("je",2,"z")+
            routine,DspLanguage::Assembler);
        for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);
            const auto block = graph.addDspCode(program);
            for (unsigned port = 0; port < 3; ++port) graph.connectDspInput(graph.addInput(),block.id,port);
            auto runtime = graph.compile(block.outputs);
            for (const auto& test : cases) for (float choose : {0.F,1.F}) {
                const std::array a{std::bit_cast<float>(test.a)},b{std::bit_cast<float>(test.b)},selector{choose};
                std::array<float,1> c{},p{},z{};
                check(runtime.process(std::array{InputBuffer{a},InputBuffer{b},InputBuffer{selector}},
                    std::array{OutputBuffer{c},OutputBuffer{p},OutputBuffer{z}}) && c[0]==bool(test.flags&1) &&
                    p[0]==bool(test.flags&4) && z[0]==bool(test.flags&0x40),
                    "COMISS flags survive local returns, joins, numeric skips and zero-count shifts");
            }
        }
    }
    const auto increment = compileDspCode("streamin a,b;streamout out;float zero=0,one=1;"
        "movaps xmm8,a;comiss xmm8,b;mov eax,0;inc eax;"+readBranch("jb",0,"out"),DspLanguage::Assembler);
    for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);const auto block=graph.addDspCode(increment);
        for (unsigned port=0;port<2;++port) graph.connectDspInput(graph.addInput(),block.id,port);
        auto runtime=graph.compile(block.outputs);
        for (const auto& test : cases) {
            const std::array a{std::bit_cast<float>(test.a)},b{std::bit_cast<float>(test.b)};
            std::array<float,1> out{};
            check(runtime.process(std::array{InputBuffer{a},InputBuffer{b}},std::array{OutputBuffer{out}}) && out[0]==bool(test.flags&1),
                  "INC preserves COMISS carry while replacing its zero/signed flags");
        }
    }
    // A Double declaration does not turn COMISS into a double comparison.
    const auto program = compileDspCode("streamdoublein a,b;streamout out;float one=1,zero=0;"
        "movapd xmm8,a;movapd xmm15,b;comiss xmm8,xmm15;"+readBranch("ja",0,"out"),DspLanguage::Assembler);
    for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);const auto block=graph.addDspCode(program);
        for (unsigned port=0;port<2;++port) graph.connectDspInput(graph.addInput(),block.id,port);
        auto runtime=graph.compileMonoDouble4(block.outputs);
        const std::array<MonoDouble4,1> a{{{std::bit_cast<double>(0x3ff0000040000000ULL),0,0,0}}};
        const std::array<MonoDouble4,1> b{{{std::bit_cast<double>(0x400000003f800000ULL),0,0,0}}};
        std::array<MonoDouble4,1> out{};
        check(runtime.process(std::array{MonoDouble4InputBuffer{a},MonoDouble4InputBuffer{b}},std::array{MonoDouble4OutputBuffer{out}}) &&
              out[0]==MonoDouble4{1,1,1,1},"COMISS compares raw low float words through Double register views");
    }
}

void invalidFlagPathsAreRejected()
{
    struct Bad { const char* body; const char* diagnostic; };
    constexpr Bad tests[]{
        {"movaps xmm8,a;comiss a,b;", "XMM first operand"},
        {"movaps xmm8,a;comiss xmm8,1;", "expected a name"},
        {"movaps xmm8,a;comiss xmm8,b,0;", "expected ';'"},
        {"movaps xmm8,a;jp done;done:", "branch requires an earlier"},
        {"movaps xmm8,a;comiss xmm8,b;je join;mov eax,data[0];add eax,4;mov ecx,0;inc ecx;join:jb done;done:", "carry branch requires"},
        {"movaps xmm8,a;comiss xmm8,b;mov eax,data[0];add eax,4;jp done;done:", "branch requires an earlier"},
        {"movaps xmm8,a;comiss xmm8,b;stage(1){jp done;done:}", "branch requires an earlier"},
    };
    for (const auto& test : tests) {
        bool rejected=false;
        try { static_cast<void>(compileDspCode(std::string("streamin a,b;monoin choose;memrefin data;streamout out;")+test.body,DspLanguage::Assembler)); }
        catch (const DspCompileError& e) { rejected=e.line>0&&e.column>0&&std::string_view(e.what()).contains(test.diagnostic); }
        check(rejected,"COMISS rejects invalid operands and unavailable flag paths with specific diagnostics");
    }
}
}

int main()
{
    try { flagsAndOperandForms();controlFlowAndWidths();invalidFlagPathsAreRejected(); }
    catch (const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
