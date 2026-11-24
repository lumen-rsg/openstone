#include "../ReferenceFixtures.hpp"
#include "stream/Runtime.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <cmath>
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
void check(bool ok,std::string_view message) { if(!ok)throw std::runtime_error(std::string(message)); }
struct Environment {
    std::fenv_t saved;
    Environment() { std::fegetenv(&saved); }
    ~Environment() { std::fesetenv(&saved); }
};
std::string fixture(std::string_view name)
{
    std::ifstream f(std::filesystem::path(OPENSTONE_TEST_FIXTURES)/"flowstone"/name,std::ios::binary);
    check(f.good(),"Read complete original Mem reader");return {std::istreambuf_iterator<char>(f),{}};
}
std::uint32_t smallLimit(const Memory& m)
{
    if(!m.size())return 0;
    const auto count=m.size()/(m.channels()==2 ? 8U : 4U);
    // These small test counts are exact Float integers. Large-count rounding is pinned by literals below.
    return std::bit_cast<std::uint32_t>(float(int(count)-1));
}
void limitPackets(DspBackend backend)
{
    Graph graph(backend);const auto b=graph.addDspCode(compileDspCode(
        "memrefin first,second;floatarrayin sentinel[1];streamout a,b,c,raw,x87,rounded,eq,bytes;"
        "movaps xmm0,first[4];movaps a,xmm0;movaps xmm0,second[4];movaps b,xmm0;movaps xmm0,sentinel[0];movaps c,xmm0;"
        "mov eax,first[4];movd xmm0,eax;movaps raw,xmm0;fld first[4];fstp x87[0];"
        "cvtss2si eax,first[4];movd xmm0,eax;movaps rounded,xmm0;"
        "movaps xmm0,first[4];comiss xmm0,first[4];movaps xmm0,_ZERO;movaps eq,xmm0;jne done;movaps xmm0,_ONE;movaps eq,xmm0;done:"
        "mov eax,first[1];movd xmm0,eax;movaps bytes,xmm0;",DspLanguage::Assembler));
    auto runtime=graph.compileMono4(b.outputs);check(runtime.setDspArrayInput(b.id,2,std::array{7.F}),"Bind neighboring FloatArray sentinel");
    Memory owner(std::vector<std::uint8_t>(128,0xa5));const auto original=owner.snapshotBytes();
    const auto other=owner.view(3,19,2);check(runtime.setDspMemRefInput(b.id,1,other),"Bind independent second descriptor");
    Environment environment;
    for(const std::size_t size:{1,3,7,31,64,257}) {
        runtime.reset();
        for(unsigned channels:{1U,2U})for(unsigned bytes:{0U,1U,3U,4U,7U,8U,15U,16U,19U,31U,64U}) {
            const auto memory=owner.view(1,bytes,channels);
            std::fesetround(FE_DOWNWARD);std::feclearexcept(FE_ALL_EXCEPT);std::feraiseexcept(FE_DIVBYZERO);
            check(runtime.setDspMemRefInput(b.id,0,memory),"Update packed Float limit without recompilation");
            check(std::fegetround()==FE_DOWNWARD && std::fetestexcept(FE_ALL_EXCEPT)==FE_DIVBYZERO,"Binding preserves caller rounding and exception flags");
            const auto expected=smallLimit(memory),second=smallLimit(other);
            std::array<std::array<Mono4,17>,8> out{};
            for(unsigned offset=0;offset<17;offset+=size) {
                const auto n=std::min<std::size_t>(size,17-offset);std::array<Mono4OutputBuffer,8> outputs;
                for(unsigned port=0;port<8;++port)outputs[port]=std::span{out[port]}.subspan(offset,n);
                check(runtime.process({},outputs),"Render packed, scalar, x87 and comparison limit reads");
            }
            check(std::fegetround()==FE_DOWNWARD && std::fetestexcept(FE_ALL_EXCEPT)==FE_DIVBYZERO,"Limit instructions preserve the caller environment");
            for(unsigned n=0;n<17;++n) {
                check(std::bit_cast<Words>(out[0][n])==Words{expected,expected,expected,expected},"Field four is a four-word Float broadcast, with zero for absent data");
                check(std::bit_cast<Words>(out[1][n])==Words{second,second,second,second} && out[2][n]==Mono4{7,0,0,0},"The extra descriptor packet cannot overwrite following Mem or FloatArray storage");
                check(std::bit_cast<Words>(out[3][n])==Words{expected,0,0,0} && std::bit_cast<Words>(out[4][n])==Words{expected,0,0,0},"MOV and FLD read the same first word without changing the packet");
                const auto integer=static_cast<std::uint32_t>(static_cast<std::int32_t>(std::bit_cast<float>(expected)));
                check(std::bit_cast<Words>(out[5][n])==Words{integer,0,0,0} && out[6][n]==Mono4{1,1,1,1},"Scalar conversion and COMISS accept the Float limit packet");
                check(std::bit_cast<Words>(out[7][n])==Words{bytes,0,0,0},"Existing numeric descriptor fields retain their layout");
            }
        }
    }
    check(owner.snapshotBytes()==original,"Descriptor access never changes the source bytes");
}

void roundingOrder(DspBackend backend)
{
    struct Case {unsigned frames;std::uint32_t limit;};
    constexpr std::array cases{Case{16777213,0x4b7ffffc},Case{16777214,0x4b7ffffd},Case{16777215,0x4b7ffffe},
        Case{16777216,0x4b7fffff},Case{16777217,0x4b7fffff},Case{16777218,0x4b800000},Case{16777219,0x4b800002},Case{16777220,0x4b800002}};
    // One owner supplies bounded views near 2^24 frames; no metadata values are injected into the runtime.
    const auto owner=Memory::fromSamples(std::vector<float>(cases.back().frames));
    Graph graph(backend);const auto b=graph.addDspCode(compileDspCode("memrefin mem;streamout out;movaps xmm0,mem[4];movaps out,xmm0;",DspLanguage::Assembler));auto runtime=graph.compileMono4(b.outputs);
    Environment environment;
    for(const auto c:cases)for(int mode:{FE_UPWARD,FE_DOWNWARD,FE_TOWARDZERO}) {
        std::fesetround(mode);std::feclearexcept(FE_ALL_EXCEPT);std::feraiseexcept(FE_INVALID);
        check(runtime.setDspMemRefInput(b.id,0,owner.view(0,std::size_t(c.frames)*4,1)),"Bind a real large Mem view");
        check(std::fegetround()==mode && std::fetestexcept(FE_ALL_EXCEPT)==FE_INVALID,"Inexact count conversion cannot leak flags or depend on host rounding");
        std::array<Mono4,3> out{};check(runtime.process({},std::array{Mono4OutputBuffer{out}}),"Read large-frame limits");
        for(auto packet:out)check(std::bit_cast<Words>(packet)==Words{c.limit,c.limit,c.limit,c.limit},"Native CVTDQ2PS then SUBSS rounding matches literal bit oracles");
    }
}

struct Wave { Memory memory;std::vector<float> samples; };
Wave wave(unsigned frames,unsigned channels,unsigned tail,unsigned tag)
{
    Memory owner(std::vector<std::uint8_t>(frames*channels*4+tail+9,0xa5));std::vector<float> data;
    for(unsigned frame=0;frame<frames;++frame)for(unsigned channel=0;channel<channels;++channel) {
        const float value=channel ? -float(tag)-float(frame+1)*.5F : float(tag)+float(frame+1)*.25F;
        data.push_back(value);owner.writeWordAt(3+(data.size()-1)*4,std::bit_cast<std::uint32_t>(value));
    }
    return {owner.view(3,frames*channels*4+tail,channels),std::move(data)};
}
void unchangedReaders(DspBackend backend)
{
    constexpr std::size_t frames=157;constexpr std::array<unsigned,6> changes{0,9,37,68,91,117};
    const std::array waves{Wave{},wave(17,1,0,1),wave(9,2,7,3),wave(5,1,3,5),Wave{},wave(1,2,0,7)};
    const std::array<float,15> positions{-100,-.5F,-0.F,0,.49F,.5F,1.5F,2.5F,3.5F,8,17,1000,
        std::bit_cast<float>(0x7fc12345U),std::bit_cast<float>(0x7f800000U),std::bit_cast<float>(0xff800000U)};
    std::array<Mono4,frames> input{};
    for(unsigned n=0;n<frames;++n)for(unsigned lane=0;lane<4;++lane)input[n][lane]=positions[(n+lane*3)%positions.size()];
    const auto originalInput=std::bit_cast<std::array<Words,frames>>(input);
    std::array<std::vector<std::uint8_t>,6> originalWaves;
    for(unsigned i=0;i<waves.size();++i)originalWaves[i]=waves[i].memory.snapshotBytes();
    Environment environment;std::fesetround(FE_TONEAREST);
    for(const auto name:{"engine-mem-reader-channels.asm","engine-mem-reader-hop-x86.asm","engine-mem-reader-hop-x64.asm"}) {
        // Only the omitted caller ports are added; the complete original body follows byte-for-byte.
        const auto program=compileDspCode("streamin in1;streamout out0,out1;"+fixture(name),DspLanguage::Assembler);
        Graph graph(backend);const auto b=graph.addDspCode(program);graph.connectDspInput(graph.addInput(),b.id,0);
        const bool hop=!program.enginePointers().empty();auto mask=Memory::fromSamples(std::array{0.F});
        if(hop)check(graph.bindDspEnginePointer(b.id,"p_hop",mask,false),"Bind only the original caller hop control");
        auto runtime=graph.compileMono4(b.outputs);
        for(unsigned hopMask:{0U,1U,3U,15U}) {
            if(!hop && hopMask)continue;mask.writeSample(0,std::bit_cast<float>(hopMask));
            std::array<std::array<Mono4,frames>,2> expected{};std::array<Mono4,2> previous{};
            for(unsigned n=0,event=0;n<frames;++n) {
                if(event+1<changes.size() && n==changes[event+1])++event;
                const auto& w=waves[event];const auto channels=w.memory.channels();
                if(channels && (!hop || !(n&hopMask))) {
                    const auto count=w.samples.size()/channels;
                    for(unsigned lane=0;lane<4;++lane) {
                        const auto pos=std::min(float(count-1),std::max(0.F,input[n][lane]));
                        const auto index=static_cast<unsigned>(std::nearbyint(pos));
                        previous[0][lane]=w.samples[index*channels];previous[1][lane]=w.samples[index*channels+(channels==2 ? 1 : 0)];
                    }
                }
                for(unsigned port=0;port<2;++port)expected[port][n]=previous[port];
            }
            for(const std::size_t size:{1,3,7,31,64,257}) {
                runtime.reset();std::array<std::array<Mono4,frames>,2> out{};
                for(unsigned offset=0,event=0;offset<frames;) {
                    if(event+1<changes.size() && offset==changes[event+1])++event;
                    check(runtime.setDspMemRefInput(b.id,1,waves[event].memory),"Switch waves or clear the binding between buffers");
                    const auto end=event+1<changes.size() ? changes[event+1] : frames;
                    const auto count=std::min<std::size_t>({size,frames-offset,end-offset});
                    check(runtime.process(std::array{Mono4InputBuffer{std::span{input}.subspan(offset,count)}},
                        std::array{Mono4OutputBuffer{std::span{out[0]}.subspan(offset,count)},Mono4OutputBuffer{std::span{out[1]}.subspan(offset,count)}}),"Run the complete unchanged native sample reader");
                    offset+=count;
                }
                for(unsigned port=0;port<2;++port)for(unsigned n=0;n<frames;++n)
                    check(std::bit_cast<Words>(out[port][n])==std::bit_cast<Words>(expected[port][n]),"Every complete reader sample matches independent clamped/RNE wave lookup and hop/empty holds");
            }
        }
    }
    check(std::bit_cast<std::array<Words,frames>>(input)==originalInput,"Complete readers leave every input word, including NaN payloads, untouched");
    for(unsigned i=0;i<waves.size();++i)check(waves[i].memory.snapshotBytes()==originalWaves[i],"Sample reads preserve wave data and incomplete tails");
}

void stagedLimits(DspBackend backend)
{
    Graph graph(backend);const auto b=graph.addDspCode(compileDspCode(
        "memrefin mem;streamout init,early,late;stage0;movaps xmm0,mem[4];movaps init,xmm0;"
        "stage1;movaps xmm0,mem[4];movaps early,xmm0;stage3;movaps xmm0,mem[4];movaps late,xmm0;",DspLanguage::Assembler));
    auto runtime=graph.compileMono4(b.outputs);
    const std::array memories{Memory(std::vector<std::uint8_t>(20),1),Memory(std::vector<std::uint8_t>(23),2),Memory{},Memory(std::vector<std::uint8_t>(3),1)};
    for(unsigned size:{1,3,7,31}) {
        runtime.reset();std::uint32_t previous=0;
        for(const auto& memory:memories) {
            check(runtime.setDspMemRefInput(b.id,0,memory),"Refresh the limit cache for all stages");
            std::array<std::array<Mono4,5>,3> values{};
            std::array<Mono4OutputBuffer,3> empty{};
            check(runtime.process({},empty),"An empty call does not execute stage-zero limit reads");
            for(unsigned offset=0;offset<5;offset+=size) {
                const auto count=std::min(size,5-offset);std::array<Mono4OutputBuffer,3> outputs;
                for(unsigned port=0;port<3;++port)outputs[port]=std::span{values[port]}.subspan(offset,count);
                check(runtime.process({},outputs),"Read cached limit packets in initialization, early and late stages");
            }
            const auto limit=smallLimit(memory);
            for(unsigned n=0;n<5;++n) {
                const auto late=n ? limit : previous;
                check(values[0][n]==Mono4{4,4,4,4},"The initialized limit remains the first binding's value until reset");
                check(std::bit_cast<Words>(values[1][n])==Words{limit,limit,limit,limit},"Early reads observe the current binding immediately");
                check(std::bit_cast<Words>(values[2][n])==Words{late,late,late,late},"Late reads keep their one-sample visibility across rebinds and clears");
            }
            previous=limit;
        }
    }
}

void rawViewsAndRestrictions(DspBackend backend)
{
    Graph graph(backend);const auto b=graph.addDspCode(compileDspCode("memrefin mem;streamdoubleout out;movapd xmm0,mem[4];movapd out[0],xmm0;movapd out[2],xmm0;",DspLanguage::Assembler));auto runtime=graph.compileMonoDouble4(b.outputs);
    check(runtime.setDspMemRefInput(b.id,0,Memory(std::vector<std::uint8_t>(20),1)),"Bind a Float limit of four");
    std::array<MonoDouble4,3> out{};check(runtime.process({},std::array{MonoDouble4OutputBuffer{out}}),"Read the descriptor packet through a PD raw view");
    for(auto packet:out)for(double lane:packet)check(std::bit_cast<std::uint64_t>(lane)==0x4080000040800000ULL,"MOVAPD preserves Float packet bytes rather than numerically converting them");
    for(const auto body:{"movaps xmm0,_ONE;movaps mem[4],xmm0;","mov eax,1;mov mem[4],eax;","fld1;fstp mem[4];",
        "mov rax,mem[4];","mov eax,mem[5];","mov eax,0;xchg eax,mem[4];"}) {
        bool rejected=false;try{static_cast<void>(compileDspCode(std::string("memrefin mem;streamout out;")+body,DspLanguage::Assembler));}
        catch(const DspCompileError& e){rejected=e.line && e.column;}
        check(rejected,"Descriptors stay read-only, field selectors bounded, and wide numeric operations restricted");
    }
}
}
int main()
{
    try { for(auto backend:{DspBackend::Interpreter,DspBackend::LlvmOrc})if(dspBackendAvailable(backend)) {
        limitPackets(backend);roundingOrder(backend);openstone::tests::withReferenceSources("unchangedReaders", [&] { unchangedReaders(backend); });stagedLimits(backend);rawViewsAndRestrictions(backend);
    }}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
