#include "stream/Runtime.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace openstone::stream;
void check(bool value, std::string_view message) { if (!value) throw std::runtime_error(std::string(message)); }
constexpr auto indefinite=std::numeric_limits<std::int32_t>::min();
struct Case { std::uint32_t bits; std::int32_t nearest; };
constexpr Case cases[]{
    {0x00000000,0},{0x80000000,0},{0x3efae148,0},{0xbefae148,0},
    {0x3f000000,0},{0xbf000000,0},{0x3fc00000,2},{0xbfc00000,-2},
    {0x40200000,2},{0xc0200000,-2},{0x40600000,4},{0xc0600000,-4},
    {0x3effffff,0},{0x3f000001,1},{0xbeffffff,0},{0xbf000001,-1},
    {0x3fbfffff,1},{0x3fc00001,2},{0xbfbfffff,-1},{0xbfc00001,-2},
    {0x4b800000,16777216},{0x4b800001,16777218},
    {0x4effffff,2147483520},{0xceffffff,-2147483520},
    {0x4f000000,indefinite},{0xcf000000,indefinite},{0x4f000001,indefinite},{0xcf000001,indefinite},
    {0x7f7fffff,indefinite},{0xff7fffff,indefinite},{0x7f800000,indefinite},{0xff800000,indefinite},
    {0x7fc12345,indefinite},{0xffc54321,indefinite},{0x7f812345,indefinite},{0xff854321,indefinite},
    {0x00000001,0},{0x80000001,0},{0x007fffff,0},{0x807fffff,0},{0x00800000,0},{0x80800000,0},
};
constexpr std::size_t frames=std::size(cases)*3+5;
struct RestoreEnvironment {
    std::fenv_t value;
    RestoreEnvironment() { std::fegetenv(&value); }
    ~RestoreEnvironment() { std::fesetenv(&value); }
};
std::string branchRead(std::string_view branch, unsigned id, std::string_view output)
{
    const auto name=std::to_string(id);
    return "movaps xmm7,zero;movaps "+std::string(output)+",xmm7;"+std::string(branch)+" yes"+name+
        ";jmp end"+name+";yes"+name+":movaps xmm7,one;movaps "+std::string(output)+",xmm7;end"+name+":";
}

void conversionFormsRoundingAndFlags()
{
    RestoreEnvironment restore;
    constexpr std::array bodies{
        "cvtss2si eax,xmm2;", "CVTSS2SI ebx,xmm15;", "cvtss2si ecx,value;",
        "mov ecx,8;cvtss2si ecx,value[ecx];", "mov eax,data[0];mov ebx,cursor;cvtss2si eax,[eax+ebx];",
        "movss cell[1],xmm15;cvtss2si edx,cell[1];",
    };
    constexpr std::array targets{"eax","ebx","ecx","ecx","eax","edx"};
    for (unsigned form=0;form<bodies.size();++form) {
        const auto source=std::string("streamin value,flag;memrefin data;streamout raw,source,c,p,z,l,ge,le;")+
            "float zero=0,one=1;float cell[3]={123,456,789};int cursor=0;mov edx,flag;cmp edx,1;movaps xmm15,value;movaps xmm2,xmm15;"+
            bodies[form]+"movd xmm0,"+targets[form]+";movaps raw,xmm0;movaps source,xmm15;"+
            branchRead("jb",0,"c")+branchRead("jp",1,"p")+branchRead("je",2,"z")+branchRead("jl",3,"l")+
            branchRead("jge",4,"ge")+branchRead("jle",5,"le")+(form==4 ? "add ebx,4;mov cursor,ebx;" : "");
        const auto program=compileDspCode(source,DspLanguage::Assembler);
        std::array<std::array<Mono4,frames>,2> input{};
        std::vector<std::uint8_t> bytes;
        for (std::size_t n=0;n<frames;++n) {
            const auto test=cases[n%std::size(cases)];
            input[0][n]={std::bit_cast<float>(test.bits),std::bit_cast<float>(0x7f812345U),99999,-99999};
            if (form==3) { input[0][n][0]=17.5F;input[0][n][2]=std::bit_cast<float>(test.bits); }
            input[1][n].fill(std::bit_cast<float>(static_cast<std::uint32_t>((n+n/std::size(cases))%3)));
            for (unsigned byte=0;byte<4;++byte) bytes.push_back(static_cast<std::uint8_t>(test.bits>>(8*byte)));
        }
        const openstone::green::Memory memory(std::move(bytes));
        for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);const auto block=graph.addDspCode(program);
            for (unsigned port=0;port<2;++port) graph.connectDspInput(graph.addInput(),block.id,port);
            auto runtime=graph.compileMono4(block.outputs);
            check(runtime.setDspMemRefInput(block.id,2,memory),"Bind scalar conversion memory");
            for (const std::size_t size : {1,3,7,31,64}) {
                runtime.reset();std::array<std::array<Mono4,frames>,8> output{};
                check(std::fesetround(FE_UPWARD)==0,"Set a non-default caller rounding mode");
                std::feclearexcept(FE_ALL_EXCEPT);std::feraiseexcept(FE_DIVBYZERO);
                for (std::size_t offset=0;offset<frames;offset+=size) {
                    const auto count=std::min(size,frames-offset);
                    std::array<Mono4OutputBuffer,8> out;
                    for (unsigned port=0;port<out.size();++port) out[port]=std::span{output[port]}.subspan(offset,count);
                    check(runtime.process(std::array{Mono4InputBuffer{std::span{input[0]}.subspan(offset,count)},
                                                     Mono4InputBuffer{std::span{input[1]}.subspan(offset,count)}},out),"Render CVTSS2SI operands");
                    check(std::fegetround()==FE_UPWARD && std::fetestexcept(FE_ALL_EXCEPT)==FE_DIVBYZERO,
                          "Scalar conversions preserve the caller's rounding mode and exception flags");
                }
                std::fesetround(FE_TONEAREST);
                for (std::size_t n=0;n<frames;++n) {
                    check(std::bit_cast<std::array<std::uint32_t,4>>(output[0][n])==
                          std::array{static_cast<std::uint32_t>(cases[n%std::size(cases)].nearest),0U,0U,0U},
                          "CVTSS2SI rounds only the selected float word, with ties to even and exact invalid-result words");
                    check(std::bit_cast<std::array<std::uint32_t,4>>(output[1][n])==std::bit_cast<std::array<std::uint32_t,4>>(input[0][n]),
                          "CVTSS2SI preserves source NaN payloads, signed zeros, subnormals and upper lanes");
                    const auto seed=(n+n/std::size(cases))%3;
                    const bool less=seed==0,zero=seed==1;
                    const std::array flags{less,seed!=2,zero,less,!less,less||zero};
                    for (unsigned port=0;port<flags.size();++port) {
                        const float expected=flags[port] ? 1.F : 0.F;
                        check(output[port+2][n]==Mono4{expected,expected,expected,expected},
                              "CVTSS2SI preserves earlier carry/parity/zero/signed flags even when it overwrites their source register");
                    }
                }
            }
        }
    }
}

void doubleViewsCallsAndWidths()
{
    const auto rawView=compileDspCode("streamdoublein value;streamout out;movapd xmm15,value;cvtss2si eax,xmm15;movd xmm0,eax;movaps out,xmm0;",DspLanguage::Assembler);
    constexpr std::array bodies{
        "call convert;", "cvtss2si eax,value;",
        "mov eax,17;jmp 4;cvtss2si eax,xmm0;", "mov eax,17;jmp 5;cvtss2si eax,xmm15;",
        "mov eax,17;jmp 8;cvtss2si eax,value;",
    };
    for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);const auto block=graph.addDspCode(rawView);
        graph.connectDspInput(graph.addInput(),block.id,0);auto runtime=graph.compileMonoDouble4(block.outputs);
        const std::array<MonoDouble4,1> input{{{std::bit_cast<double>(0x405900003fc00000ULL),-999,999,0}}};
        std::array<MonoDouble4,1> out{};
        check(runtime.process(std::array{MonoDouble4InputBuffer{input}},std::array{MonoDouble4OutputBuffer{out}}) &&
              std::bit_cast<std::uint32_t>(static_cast<float>(out[0][0]))==2U,
              "CVTSS2SI reads a raw Float DWORD even in a Double-loaded register");
        for (unsigned test=0;test<bodies.size();++test) {
            Graph flow(backend);const auto ports=flow.addDspCode(compileDspCode(
                std::string("streamin value;streamout out;movaps xmm0,value;movaps xmm15,xmm0;")+bodies[test]+"movd xmm1,eax;movaps out,xmm1;"+
                (test==0 ? "jmp end;convert:cvtss2si eax,xmm15;ret;end:" : ""),DspLanguage::Assembler));
            flow.connectDspInput(flow.addInput(),ports.id,0);auto run=flow.compile(ports.outputs);
            const std::array input{1.5F,2.5F,-1.5F,-2.5F};std::array<float,4> output{};
            check(run.process(std::array{InputBuffer{input}},std::array{OutputBuffer{output}}),"Render scalar conversion call/width probes");
            constexpr std::array<std::int32_t,4> expected{2,2,-2,-2};
            for (unsigned n=0;n<output.size();++n) check(std::bit_cast<std::uint32_t>(output[n])==
                static_cast<std::uint32_t>(test<2 ? expected[n] : 17),"CVTSS2SI supports local returns and known native register/memory widths");
        }
    }
    constexpr std::array invalid{
        "cvtss2si xmm0,value;", "float cell;cvtss2si cell,value;", "cvtss2si rax,value;",
        "cvtss2si eax,ebx;", "cvtss2si eax,1;", "cvtss2si eax,value,0;",
        "cvtss2si eax,value;jge end;end:",
        "mov eax,data[0];cvtss2si eax,value;movaps xmm0,[eax];",
        "mov eax,convert;cvtss2si eax,value;call eax;jmp end;convert:ret;end:",
    };
    for (auto body : invalid) {
        bool rejected=false;
        try { static_cast<void>(compileDspCode(std::string("streamin value;memrefin data;streamout out;")+body,DspLanguage::Assembler)); }
        catch (const DspCompileError& e) { rejected=e.line>0&&e.column>0; }
        check(rejected,"Scalar conversion rejects invalid forms, missing flags and stale pointer/target provenance");
    }
}
}
int main()
{
    try { conversionFormsRoundingAndFlags();doubleViewsCallsAndWidths(); }
    catch (const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
