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
using Words = std::array<std::uint32_t,4>;
void check(bool value, std::string_view message) { if (!value) throw std::runtime_error(std::string(message)); }
constexpr std::array branches{"je","jz","jne","jnz","ja","jb","jbe","jae","jp","jnp","jg","jl","jng","jnl","jge","jle"};
constexpr std::size_t frames=131, outputs=branches.size()+3;
constexpr std::array words{
    0U,1U,UINT32_MAX,0x80000000U,0x7fffffffU,0x00000001U,0x80000001U,0x007fffffU,
    0x807fffffU,0x00800000U,0x80800000U,0x7f800000U,0xff800000U,0x7f812345U,0xffc54321U,
    0x7fc12345U,0xff854321U,0x01234567U,0x89abcdefU,
};
Words raw(std::uint32_t value) { return {value,0,0,0}; }
Words broadcast(std::uint32_t value) { return {value,value,value,value}; }
std::uint32_t a(std::size_t n) { return words[n%words.size()]; }
std::uint32_t b(std::size_t n) { return words[(n+7)%words.size()]; }
std::string store(std::string_view output, std::string_view reg)
{
    return "movd xmm0,"+std::string(reg)+";movaps "+std::string(output)+",xmm0;";
}
std::string readFlags()
{
    std::string result;
    for (unsigned i=0;i<branches.size();++i) {
        const auto id=std::to_string(i);
        result+="movaps xmm7,zero;movaps f"+id+",xmm7;"+branches[i]+" yes"+id+";jmp next"+id+
            ";yes"+id+":movaps xmm7,one;movaps f"+id+",xmm7;next"+id+":";
    }
    return result;
}
std::string seedFlags(std::string_view suffix)
{
    const std::string end="seeded"+std::string(suffix), cmp="cmpSeed"+std::string(suffix),
        overflow="overflow"+std::string(suffix), carry="carryOverflow"+std::string(suffix);
    return "mov edx,seed;cmp edx,3;jb "+cmp+";je "+overflow+";cmp edx,4;je "+carry+
        ";movaps xmm6,nan;comiss xmm6,nan;jmp "+end+";"+cmp+":cmp edx,1;jmp "+end+";"+
        overflow+":mov edx,0x7fffffff;add edx,1;jmp "+end+";"+carry+":mov edx,0x80000000;add edx,0x80000000;"+end+":";
}
std::array<bool,branches.size()> expectedFlags(std::size_t seed)
{
    // Literal incoming states: CMP less/equal/greater, two ADD overflows, COMISS unordered.
    constexpr std::array carry{true,false,false,false,true,true}, parity{true,true,false,true,true,true},
        zero{false,true,false,false,true,true}, less{true,false,false,false,true,false};
    const auto c=carry[seed],p=parity[seed],z=zero[seed],l=less[seed];
    return {z,z,!z,!z,!c&&!z,c,c||z,!c,p,!p,!l&&!z,l,l||z,!l,!l,l||z};
}
openstone::green::Memory memory(std::initializer_list<std::uint32_t> words)
{
    std::vector<std::uint8_t> bytes;
    for (auto word : words) for (unsigned byte=0;byte<4;++byte) bytes.push_back(static_cast<std::uint8_t>(word>>(8*byte)));
    return openstone::green::Memory(std::move(bytes));
}

template<typename Expected>
void verify(std::string_view preparation, std::string_view body, Expected expected, std::string_view routines={})
{
    std::string source="streamin a,b,seed;memrefin data,other;streamout ";
    for (unsigned i=0;i<branches.size();++i) source+="f"+std::to_string(i)+",";
    source+="left,right,guard;int cell=0,cells[4]={0x01234567,0x76543210,0,0x89abcdef};float zero=0,one=1,nan=0x7fc12345;"
            "float alphaValue=0x01234567,betaValue=0x89abcdef;movaps xmm0,zero;movaps guard,xmm0;";
    source+=preparation;
    source+=seedFlags("incoming");
    source+=body;source+=readFlags();
    if (!routines.empty()) source+="jmp finished;"+std::string(routines)+"finished:";
    const auto program=[&] {
        try { return compileDspCode(source,DspLanguage::Assembler); }
        catch (const DspCompileError& error) {
            throw std::runtime_error(std::string(error.what())+"\nXCHG setup: "+std::string(preparation)+"\nXCHG body: "+std::string(body));
        }
    }();
    std::array<std::array<Mono4,frames>,3> input{};
    for (std::size_t n=0;n<frames;++n) {
        input[0][n]={std::bit_cast<float>(a(n)),999,-999,std::bit_cast<float>(0x7f854321U)};
        input[1][n]={std::bit_cast<float>(b(n)),-999,999,std::bit_cast<float>(0xff812345U)};
        input[2][n].fill(std::bit_cast<float>(static_cast<std::uint32_t>(n%6)));
    }
    for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);const auto block=graph.addDspCode(program);
        for (unsigned port=0;port<3;++port) graph.connectDspInput(graph.addInput(),block.id,port);
        auto runtime=graph.compileMono4(block.outputs);
        check(runtime.setDspMemRefInput(block.id,3,memory({0x11111111,0x22222222,0x33333333,0x44444444,0,0,0,0})) &&
              runtime.setDspMemRefInput(block.id,4,memory({0x55555555,0x66666666,0x77777777,0x88888888})),"Bind XCHG memories");
        for (const std::size_t size : {1,3,7,31,64}) {
            runtime.reset();std::array<std::array<Mono4,frames>,outputs> output{};
            for (std::size_t offset=0;offset<frames;offset+=size) {
                const auto count=std::min(size,frames-offset);
                std::array<Mono4InputBuffer,3> in;
                std::array<Mono4OutputBuffer,outputs> out;
                for (unsigned port=0;port<in.size();++port) in[port]=std::span{input[port]}.subspan(offset,count);
                for (unsigned port=0;port<out.size();++port) out[port]=std::span{output[port]}.subspan(offset,count);
                check(runtime.process(in,out),"Render XCHG raw words");
            }
            for (std::size_t n=0;n<frames;++n) {
                const auto flags=expectedFlags(n%6);
                for (unsigned port=0;port<branches.size();++port) {
                    const auto value=flags[port] ? 1.F : 0.F;
                    check(output[port][n]==Mono4{value,value,value,value},"XCHG preserves every incoming flag and conditional branch");
                }
                const auto packets=expected(n);
                for (unsigned port=0;port<3;++port) check(std::bit_cast<Words>(output[branches.size()+port][n])==packets[port],
                    "XCHG swaps exact original words and metadata, preserves guard cells and uses the old effective address");
            }
        }
    }
}

void registerAndMemorySwaps()
{
    constexpr std::array regs{"eax","ebx","ecx","edx"};
    for (unsigned left=0;left<regs.size();++left) for (unsigned right=0;right<regs.size();++right) {
        const auto body=std::string("mov ")+regs[left]+",a;"+(left==right ? "" : std::string("mov ")+regs[right]+",b;")+
            "XCHG "+regs[left]+","+regs[right]+";"+store("left",regs[left])+store("right",regs[right]);
        verify("",body,[=](std::size_t n) { return std::array{raw(left==right ? a(n) : b(n)),raw(a(n)),Words{}}; });
    }
    for (bool reversed : {false,true}) {
        for (unsigned form=0;form<6;++form) {
            const bool indirect=form>=3, alias=form==2 || form>=4, scaled=form==5;
            const auto reg=alias ? "ebx" : "eax";
            const auto address=indirect ? (alias ? (scaled ? "[ecx+ebx*4]" : "[ecx+ebx]") : "[ecx+8]")
                                       : form==0 ? "cell" : form==1 ? "cells[2]" : "cells[ebx]";
            const std::uint32_t old=alias ? (scaled ? 2 : 8) : 0;
            std::string body="mov eax,a;mov edx,b;";
            if (alias) body+="mov ebx,"+std::to_string(old)+";";
            if (indirect) body+="mov ecx,data[0];mov [ecx],0x01234567;mov [ecx+4],0x76543210;mov [ecx+12],0x89abcdef;";
            body+="mov "+std::string(address)+",edx;";
            body+=std::string("xchg ")+(reversed ? std::string(address)+","+reg : std::string(reg)+","+address)+";";
            body+=store("left",reg);
            body+=std::string("mov edx,")+(indirect ? "[ecx+8]" : form==0 ? "cell" : "cells[2]")+";"+store("right","edx");
            if (form!=0) body+=indirect ? "movaps xmm0,[ecx];movaps guard,xmm0;" : "movaps xmm0,cells[0];movaps guard,xmm0;";
            verify("",body,[=](std::size_t n) {
                const auto value=alias ? old : a(n);
                return std::array{raw(b(n)),raw(value),form==0 ? Words{} : Words{0x01234567,0x76543210,value,0x89abcdef}};
            });
        }
    }
}

void referencesAndTargets()
{
    for (const auto suffix : {"32","64"}) {
        const bool wide=std::string_view(suffix)=="64";
        const auto a=wide ? "rax" : "eax", b=wide ? "rbx" : "ebx";
        const auto prepare=std::string("mov ")+a+",data[0];add "+a+",4;mov "+b+",other[0];";
        const auto body=std::string("xchg ")+a+","+b+";xchg "+a+","+a+";mov edx,["+a+"];"+store("left","edx")+
            "mov edx,["+b+"];"+store("right","edx");
        verify(prepare,body,[](std::size_t) { return std::array{raw(0x55555555),raw(0x22222222),Words{}}; });
    }
    verify("mov eax,data[0];mov ebx,0x89abcdef;","xchg eax,ebx;"+store("left","eax")+"mov edx,[ebx];"+store("right","edx"),
        [](std::size_t) { return std::array{raw(0x89abcdef),raw(0x11111111),Words{}}; });
    constexpr auto alpha="alpha:movaps xmm0,alphaValue;ret;";
    const auto routines=std::string(alpha)+"beta:movaps xmm0,betaValue;ret;";
    const auto targets=[](std::size_t) { return std::array{broadcast(0x89abcdef),broadcast(0x01234567),Words{}}; };
    verify("mov eax,alpha;mov ebx,beta;","xchg eax,ebx;call eax;movaps left,xmm0;call ebx;movaps right,xmm0;",targets,routines);
    verify("mov eax,alpha;","xchg eax,eax;call eax;movaps left,xmm0;movaps right,xmm0;",
        [](std::size_t) { return std::array{broadcast(0x01234567),broadcast(0x01234567),Words{}}; },alpha);
    verify("mov eax,alpha;mov ebx,0x89abcdef;","xchg eax,ebx;"+store("left","eax")+"call ebx;movaps right,xmm0;",
        [](std::size_t) { return std::array{raw(0x89abcdef),broadcast(0x01234567),Words{}}; },alpha);
    verify("mov eax,alpha;mov cell,eax;mov ebx,beta;","xchg cell,ebx;mov eax,cell;call eax;movaps left,xmm0;call ebx;movaps right,xmm0;",targets,routines);
    verify("mov eax,alpha;mov cell,0x89abcdef;","xchg eax,cell;"+store("left","eax")+"mov ebx,cell;call ebx;movaps right,xmm0;",
        [](std::size_t) { return std::array{raw(0x89abcdef),broadcast(0x01234567),Words{}}; },alpha);
    for (bool reversed : {false,true}) {
        verify("","mov ecx,data[0];mov edx,seed;mov ebx,alpha;mov [ecx+edx*4],ebx;mov eax,beta;"+
            std::string(reversed ? "xchg [ecx+edx*4],eax;" : "xchg eax,[ecx+edx*4];")+
            "call eax;movaps right,xmm0;mov eax,[ecx+edx*4];call eax;movaps left,xmm0;",targets,routines);
        const auto exchange=std::string(reversed ? "xchg [ecx+edx*4],eax;" : "xchg eax,[ecx+edx*4];");
        const auto mixed=[](std::size_t) { return std::array{raw(0x89abcdef),broadcast(0x01234567),Words{}}; };
        verify("","mov ecx,data[0];mov edx,seed;mov ebx,alpha;mov [ecx+edx*4],ebx;mov eax,0x89abcdef;"+exchange+
            "call eax;movaps right,xmm0;movd xmm0,[ecx+edx*4];movaps left,xmm0;",mixed,alpha);
        verify("","mov ecx,data[0];mov edx,seed;mov [ecx+edx*4],0x89abcdef;mov eax,alpha;"+exchange+
            store("left","eax")+"mov ebx,[ecx+edx*4];call ebx;movaps right,xmm0;",mixed,alpha);
    }
}

void controlFlowWidthsAndDiagnostics()
{
    for (auto body : {
        "call swap;", "mov edx,seed;test edx,1;jz alternate;xchg ebx,ecx;jmp joined;alternate:xchg cell,ebx;mov ecx,cell;joined:",
        "mov edx,3;again:xchg ebx,ecx;dec edx;jnz again;",
    }) {
        // Reseed flags after a counted loop: the swap values survive, while loop arithmetic legitimately replaces flags.
        const bool call=std::string_view(body)=="call swap;";
        const auto code=std::string(body)+(call ? "" : seedFlags("restored"))+store("left","ebx")+store("right","ecx");
        verify("mov ebx,a;mov ecx,b;mov cell,ecx;",code,
            [](std::size_t n) { return std::array{raw(b(n)),raw(a(n)),Words{}}; },
            std::string_view(body)=="call swap;" ? "swap:xchg ebx,ecx;ret;" : "");
    }
    for (const auto body : {"jmp 2;xchg ebx,ecx;", "jmp 6;xchg cell,ebx;", "jmp 6;xchg ebx,cell;"}) {
        verify("mov ebx,a;mov ecx,b;mov cell,ecx;",std::string(body)+store("left","ebx")+store("right","ecx"),
            [](std::size_t n) { return std::array{raw(a(n)),raw(b(n)),Words{}}; });
    }
    constexpr std::array invalid{
        "xchg eax,eax;", "mov eax,1;xchg eax,ebx;", "xchg cell,cells[0];", "xchg xmm0,xmm0;",
        "mov eax,1;xchg eax,1;", "mov eax,1;xchg eax;", "mov eax,1;xchg eax,eax,0;",
        "mov eax,1;mov ebx,2;xchg eax,rbx;", "mov eax,1;mov ebx,2;xchg rax,rbx;",
        "stage 0;mov eax,1;xchg eax,a;", "mov eax,1;xchg eax,data[0];",
        "mov eax,data[0];xchg eax,cell;", "mov eax,data[0];xchg eax,[eax];",
        "mov eax,data[0];mov ebx,1;xchg eax,ebx;movaps xmm0,[eax];",
        "mov eax,alpha;mov ebx,1;xchg eax,ebx;call eax;jmp end;alpha:ret;end:",
        "mov eax,alpha;mov cell,eax;mov ebx,1;xchg cell,ebx;mov eax,cell;call eax;jmp end;alpha:ret;end:",
        "mov eax,1;mov ebx,2;xchg eax,ebx;jz end;end:",
        "mov eax,1;mov ebx,2;xchg eax,ebx;jb end;end:",
        "mov eax,1;mov ebx,2;xchg eax,ebx;jp end;end:",
        "mov eax,1;mov ebx,2;jmp 1;xchg eax,ebx;",
    };
    for (auto body : invalid) {
        bool rejected=false;
        try { static_cast<void>(compileDspCode(std::string("streamin a;memrefin data;streamout out;int cell,cells[4];")+body,DspLanguage::Assembler)); }
        catch (const DspCompileError& e) { rejected=e.line>0&&e.column>0; }
        check(rejected,"XCHG rejects invalid/unwritten operands, stale tags, missing flags and unknown accumulator widths");
    }
}
}
int main()
{
    try { registerAndMemorySwaps();referencesAndTargets();controlFlowWidthsAndDiagnostics(); }
    catch (const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
