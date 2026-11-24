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
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace openstone::stream;
using Memory = openstone::green::Memory;
using Words = std::array<std::uint32_t,4>;
constexpr std::array<const char*,7> wide{"rax","rbx","rcx","rdx","rsi","rdi","rbp"};
constexpr std::array<const char*,7> narrow{"eax","ebx","ecx","edx","esi","edi","ebp"};
void check(bool ok,std::string_view message) { if (!ok) throw std::runtime_error(std::string(message)); }
struct Environment {
    std::fenv_t saved;
    Environment() { std::fegetenv(&saved); }
    ~Environment() { std::fesetenv(&saved); }
};
std::string fixture(std::string_view name)
{
    std::ifstream f(std::filesystem::path(OPENSTONE_TEST_FIXTURES)/"flowstone"/name,std::ios::binary);
    check(f.good(),"Read unchanged native sample-reader fixture");
    return {std::istreambuf_iterator<char>(f),{}};
}
std::string capture(std::string_view reg,std::string_view output)
{
    return "movd xmm0,"+std::string(reg)+";movaps "+std::string(output)+",xmm0;";
}
std::string flags()
{
    std::string code;
    constexpr std::array names{"cf","pf","zf","lt"},branches{"jb","jp","jz","jl"};
    for (unsigned i=0;i<4;++i) {
        const auto id=std::to_string(i);
        code+="movaps xmm0,_ZERO;movaps "+std::string(names[i])+",xmm0;"+branches[i]+" yes"+id+";jmp next"+id+";yes"+id+":movaps xmm0,_ONE;movaps "+names[i]+",xmm0;next"+id+":";
    }
    return code;
}

void allAliasesAndFields(DspBackend backend)
{
    struct Seed {const char* code;std::array<bool,4> flags;};
    constexpr std::array seeds{
        Seed{"cmp zero,0;",{false,true,true,false}},
        Seed{"cmp zero,1;",{true,true,false,true}},
        Seed{"cmp two,0;",{false,false,false,false}},
        Seed{"mov eax,0x7fffffff;add eax,1;",{false,true,false,false}},
        Seed{"mov eax,0x80000000;sub eax,1;",{false,true,false,true}},
    };
    Memory owner(std::vector<std::uint8_t>(70000,0xa5)); const auto original=owner.snapshotBytes();
    const std::array memories{Memory{},owner.view(3,1,1),owner.view(5,3,2),owner.view(7,4,1),owner.view(9,7,2),owner.view(1,8,2),owner.view(11,15,1),owner.view(13,257,2),owner.view(3,65537,1)};
    Environment environment;
    for (unsigned alias=0;alias<wide.size();++alias) for (const auto& seed:seeds) {
        const std::string r=wide[alias],e=narrow[alias];
        const auto source="memrefin mem;streamout bytes,items,channels,cf,pf,zf,lt,number;int zero=0,two=2;mov "+r+",mem[0];"+seed.code+
            "mov "+r+",mem[1];"+capture(e,"bytes")+"mov "+r+",mem[2];"+capture(e,"items")+"mov "+r+",mem[3];"+capture(e,"channels")+flags()+
            "movd xmm0,"+e+";cvtdq2ps xmm0,xmm0;movaps number,xmm0;";
        const auto program=compileDspCode(source,DspLanguage::Assembler);
        Graph graph(backend); const auto b=graph.addDspCode(program); auto runtime=graph.compileMono4(b.outputs);
        check(!program.readsSignalInput(0),"Descriptor metadata does not create a signal-input dependency");
        for (const std::size_t size:{1,3,7,31,64}) {
            runtime.reset();
            for (const auto& memory:memories) {
                check(runtime.setDspMemRefInput(b.id,0,memory),"Rebind descriptor metadata between buffers");
                std::array<std::array<Mono4,17>,8> out{};
                for (unsigned offset=0;offset<17;offset+=size) {
                    const auto count=std::min<std::size_t>(size,17-offset); std::array<Mono4OutputBuffer,8> outputs;
                    for (unsigned port=0;port<8;++port) outputs[port]=std::span{out[port]}.subspan(offset,count);
                    std::fesetround(FE_UPWARD);std::feclearexcept(FE_ALL_EXCEPT);std::feraiseexcept(FE_DIVBYZERO);
                    check(runtime.process({},outputs),"Render every descriptor field through each wide register spelling");
                    check(std::fegetround()==FE_UPWARD && std::fetestexcept(FE_ALL_EXCEPT)==FE_DIVBYZERO,"Descriptor MOV preserves caller floating-point state");
                }
                const std::array expected{static_cast<std::uint32_t>(memory.size()),static_cast<std::uint32_t>(memory.size()/4),static_cast<std::uint32_t>(memory.channels())};
                for (unsigned n=0;n<17;++n) {
                    for (unsigned port=0;port<3;++port) check(std::bit_cast<Words>(out[port][n])==Words{expected[port],0,0,0},"MOV defines exact numeric parent bits for byte length, Float count and channels");
                    for (unsigned flag=0;flag<4;++flag) check(out[3+flag][n]==Mono4{float(seed.flags[flag]),float(seed.flags[flag]),float(seed.flags[flag]),float(seed.flags[flag])},"All modeled comparison flags survive descriptor MOV");
                    check(out[7][n]==Mono4{float(memory.channels()),0,0,0},"The corresponding DWORD alias can consume the numeric result");
                }
            }
        }
    }
    check(owner.snapshotBytes()==original,"All metadata reads leave bounded owner bytes untouched");
}

void originalDispatchPrefixes(DspBackend backend)
{
    for (const auto name:{"engine-mem-reader-channels.asm","engine-mem-reader-hop-x86.asm","engine-mem-reader-hop-x64.asm"}) {
        const auto original=fixture(name); const auto end=original.find("mono:"); check(end!=std::string::npos,"Find the original channel-dispatch boundary");
        // Keep the complete original dispatch prefix, including its native hop-clock and branch operands.
        // The complete sample-loading bodies are tested separately by assembler_mem_limit.
        const auto prefix=original.substr(0,end);
        const auto source="streamout result;movaps xmm0,_ZERO;movaps result,xmm0;"+prefix+
            "mono:movaps xmm0,_ONE;movaps result,xmm0;jmp end;stereo:movaps xmm0,_ONE;addps xmm0,_ONE;movaps result,xmm0;jmp end;end:";
        const auto program=compileDspCode(source,DspLanguage::Assembler); const bool hop=!program.enginePointers().empty();
        Graph graph(backend);const auto b=graph.addDspCode(program);
        auto hopMemory=Memory::fromSamples(std::array{0.F}); if(hop)check(graph.bindDspEnginePointer(b.id,"p_hop",hopMemory,false),"Bind the original caller's hop mask");
        auto runtime=graph.compileMono4(b.outputs);
        for (unsigned mask: {0U,1U,3U,15U}) for (const auto& memory:std::array{Memory{},Memory(std::vector<std::uint8_t>(16),1),Memory(std::vector<std::uint8_t>(16),2)}) {
            if(!hop && mask)continue;
            hopMemory.writeSample(0,std::bit_cast<float>(mask));
            check(runtime.setDspMemRefInput(b.id,0,memory),"Bind mono, stereo or absent data to the unchanged prefix");
            for (const std::size_t size:{1,3,7,31,64,257}) {
                runtime.reset();std::array<Mono4,137> out{};
                for(unsigned offset=0;offset<out.size();offset+=size) {
                    const auto count=std::min<std::size_t>(size,out.size()-offset);
                    check(runtime.process({},std::array{Mono4OutputBuffer{std::span{out}.subspan(offset,count)}}),"Run original channel-dispatch prefix across buffer sizes");
                }
                for(unsigned n=0;n<out.size();++n) {
                    const float value=(!hop || !(n&mask)) ? float(memory.channels()) : 0;
                    check(out[n]==Mono4{value,value,value,value},"Original branches select absent, mono and stereo channels on the expected global clock samples");
                }
            }
        }
    }
}

void stateAndControlFlow(DspBackend backend)
{
    for (unsigned alias=0;alias<wide.size();++alias) {
        const std::string r=wide[alias],e=narrow[alias];
        const std::array bodies{
            "mov "+r+",mem[3];push "+r+";mov "+e+",99;pop "+r+";"+capture(e,"out"),
            "call load;"+capture(e,"out")+"jmp end;load:mov "+r+",mem[3];ret;end:",
            "mov "+r+",leaf;mov "+r+",mem[3];"+capture(e,"out")+"jmp end;leaf:ret;end:",
        };
        for (const auto& body:bodies) {
            Graph graph(backend);const auto b=graph.addDspCode(compileDspCode("memrefin mem;streamout out;"+body,DspLanguage::Assembler));auto runtime=graph.compileMono4(b.outputs);
            check(runtime.setDspMemRefInput(b.id,0,Memory(std::vector<std::uint8_t>(8),2)),"Bind descriptor for calls, stacks and target replacement");
            std::array<Mono4,19> out{};check(runtime.process({},std::array{Mono4OutputBuffer{out}}),"Run descriptor state transitions");
            for(auto packet:out)check(std::bit_cast<Words>(packet)==Words{2,0,0,0},"Calls and stacks retain the numeric descriptor value; target replacement yields data");
        }
    }
    const auto p=compileDspCode("memrefin mem;streamout out;mov rax,mem[0];mov ebx,0;again:mov rax,mem[3];inc ebx;cmp ebx,3;jnz again;"+capture("eax","out"),DspLanguage::Assembler);
    Graph graph(backend);const auto b=graph.addDspCode(p);auto runtime=graph.compile(b.outputs);check(runtime.setDspMemRefInput(b.id,0,Memory(std::vector<std::uint8_t>(4),1)),"Bind loop descriptor");
    std::array<float,31> out{};check(runtime.process({},std::array{OutputBuffer{out}}),"Overwrite dead pointer state at a loop header");for(float v:out)check(std::bit_cast<std::uint32_t>(v)==1,"The descriptor store defines numeric loop state before each use");

    Graph skips(backend);const auto skip=skips.addDspCode(compileDspCode("memrefin mem;streamout out;mov eax,7;jmp 6;mov eax,mem[3];"+capture("eax","out"),DspLanguage::Assembler));auto skipped=skips.compile(skip.outputs);
    std::array<float,7> unchanged{};check(skipped.process({},std::array{OutputBuffer{unchanged}}),"Keep the established DWORD numeric skip width");
    for(float value:unchanged)check(std::bit_cast<std::uint32_t>(value)==7,"A known-width DWORD descriptor load is skipped without changing the numeric parent");

    Graph stages(backend);const auto block=stages.addDspCode(compileDspCode("memrefin mem;streamout init,early,late;stage0;mov rax,mem[1];"+capture("eax","init")+"stage1;mov rax,mem[3];"+capture("eax","early")+"stage3;mov rax,mem[1];"+capture("eax","late"),DspLanguage::Assembler));auto staged=stages.compileMono4(block.outputs);
    for(unsigned size:{1,3,7,31}) {
        staged.reset();unsigned previous=0;
        for(unsigned bytes:{4,12,8,0,31}) {
            check(staged.setDspMemRefInput(block.id,0,Memory(std::vector<std::uint8_t>(bytes),2)),"Update metadata without restarting a staged renderer");
            std::array<std::array<Mono4,3>,3> values{};std::array<Mono4OutputBuffer,3> outputs;
            for(unsigned offset=0;offset<3;offset+=size) {
                const auto count=std::min(size,3-offset);
                for(unsigned port=0;port<3;++port)outputs[port]=std::span{values[port]}.subspan(offset,count);
                check(staged.process({},outputs),"Render descriptor loads in each stage");
            }
            for(unsigned n=0;n<3;++n) {
                check(std::bit_cast<Words>(values[0][n])==Words{4,0,0,0},"Stage-zero metadata snapshot remains initialized once");
                check(std::bit_cast<Words>(values[1][n])==Words{bytes ? 2U : 0U,0,0,0},"Early metadata reflects the current binding");
                check(std::bit_cast<Words>(values[2][n])==Words{n ? bytes : previous,0,0,0},"Late metadata retains its established one-sample visibility");
            }
            previous=bytes;
        }
    }
}

void diagnostics()
{
    const auto reject=[](std::string_view source,std::string_view message) {
        bool rejected=false;try{static_cast<void>(compileDspCode(source,DspLanguage::Assembler));}
        catch(const DspCompileError& e){rejected=e.line && e.column && std::string_view(e.what()).contains(message);}
        check(rejected,std::string("Expected descriptor diagnostic: ")+std::string(source));
    };
    reject("memrefin mem;streamout out;mov rax,mem[0];mov rax,mem[3];fld [rax];fstp out[0];","requires a register loaded from memrefin[0]");
    reject("memrefin mem;streamout out;mov rax,leaf;mov rax,mem[3];call rax;jmp end;leaf:ret;end:","call register must hold a local subroutine label");
    reject("memrefin mem;streamout out;mov rax,mem[3];mov ebx,rax;","use a 32-bit register");
    reject("memrefin mem;streamout out;mov rax,1;","use a 32-bit register");
    reject("memarrayrefin waves;streamout out;mov rax,waves[0];","use a 32-bit register");
    reject("memrefin mem;streamout out;mov eax,1;mov mem[3],eax;","cannot write a Mem descriptor");
    reject("memrefin mem;streamout out;movaps xmm0,mem[0];","load the Mem reference into an integer register with mov");
    reject("memrefin mem;streamout out;mov rax,mem[4];","use a 32-bit register");
    reject("memrefin mem;streamout out;mov rax,mem[3];jz end;end:","branch requires an earlier");
    reject("memrefin mem;streamout out;stage1;mov rax,mem[3];stage2;movd xmm0,eax;movaps out,xmm0;","write the integer register");
    reject("memrefin mem;streamout out;mov eax,7;jmp 6;mov rax,mem[3];movd xmm0,eax;movaps out,xmm0;","unsupported instruction width in numeric branch span");
}
}
int main()
{
    try {for(auto backend:{DspBackend::Interpreter,DspBackend::LlvmOrc})if(dspBackendAvailable(backend)) {
        allAliasesAndFields(backend);openstone::tests::withReferenceSources("originalDispatchPrefixes", [&] { originalDispatchPrefixes(backend); });stateAndControlFlow(backend);
    } diagnostics();} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
