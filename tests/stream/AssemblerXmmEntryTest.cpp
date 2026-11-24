#include "stream/Runtime.hpp"
#include "stream/PolyRuntime.hpp"
#include <algorithm>
#include <bit>
#include <iostream>
#include <stdexcept>

namespace {
using namespace openstone::stream;
void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
constexpr std::size_t frames = 131;
constexpr std::array sizes{1U, 3U, 7U, 31U, 64U};
std::array<DspXmmRegister, frames> trace() {
    std::array<DspXmmRegister, frames> result{};
    constexpr std::array<std::uint32_t, 8> words{0,0x80000000,1,0x80000001,0x7fc12345,0x7f812345,0xffffffff,0x12345678};
    for (std::size_t n=0;n<frames;++n) for (unsigned lane=0;lane<4;++lane) result[n][lane]=words[(n+lane*3)%words.size()];
    return result;
}
void rawRegisters(DspBackend backend) {
    const auto values=trace();
    for (unsigned reg=0;reg<16;++reg) {
        const auto name="xmm"+std::to_string(reg);
        const auto p=compileDspCode("streamout before,after;movaps before,"+name+";pcmpeqd "+name+","+name+";movaps after,"+name+";",DspLanguage::Assembler);
        Graph graph(backend);const auto block=graph.addDspCode(p);
        auto runtime=graph.compileMono4(block.outputs);
        auto owned=values; check(runtime.setDspIncomingXmm(block.id,reg,owned),"Bind every incoming XMM register");
        owned.fill({}); // Caller memory is copied, never borrowed by audio.
        for (auto size:sizes) {
            runtime.reset();
            std::array<std::array<Mono4,frames>,2> output{};
            for (std::size_t n=0;n<frames;n+=size) {
                auto count=std::min<std::size_t>(size,frames-n);
                check(runtime.process({},std::array{Mono4OutputBuffer{output[0]}.subspan(n,count),Mono4OutputBuffer{output[1]}.subspan(n,count)}),"Render incoming raw register trace");
            }
            for(std::size_t n=0;n<frames;++n) {
                check(std::bit_cast<DspXmmRegister>(output[0][n])==values[n],"Incoming XMM preserves NaNs, zeros, subnormals and all payload bits");
                check(std::bit_cast<DspXmmRegister>(output[1][n])==DspXmmRegister{UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX},"A register write does not change the next invocation's entry value");
            }
        }
        check(runtime.setDspIncomingXmm(block.id,reg,{}),"Clear incoming binding");
        std::array<std::array<Mono4,2>,2> output{};
        check(runtime.process({},std::array{Mono4OutputBuffer{output[0]},Mono4OutputBuffer{output[1]}}),"Render default bank");
        for (auto sample:output[0]) check(std::bit_cast<DspXmmRegister>(sample)==DspXmmRegister{},"Unbound register entry is zero on every sample");
        check(!runtime.setDspIncomingXmm(DspBlockId{999},reg,values)&&!runtime.setDspIncomingXmm(block.id,16,values),"Invalid bindings fail without changing the bank");
    }
}
void doubleViewsAndRebinding(DspBackend backend) {
    Graph graph(backend);const auto block=graph.addDspCode(compileDspCode("streamdoubleout out;movapd out,xmm15;",DspLanguage::Assembler));
    auto runtime=graph.compileMonoDouble4(block.outputs);
    const auto values=trace();check(runtime.setDspIncomingXmm(block.id,15,values),"Bind physical XMM for double view");
    for(auto size:sizes) {
        runtime.reset();std::array<MonoDouble4,frames> output{};
        for(std::size_t n=0;n<frames;n+=size) check(runtime.process({},std::array{MonoDouble4OutputBuffer{output}.subspan(n,std::min<std::size_t>(size,frames-n))}),"Render double view");
        for(std::size_t n=0;n<frames;++n) check(std::bit_cast<std::array<std::uint64_t,4>>(output[n])==std::array<std::uint64_t,4>{
            values[n][0]|(std::uint64_t{values[n][1]}<<32),values[n][2]|(std::uint64_t{values[n][3]}<<32),0,0},"Double moves reinterpret the exact physical 128 bits");
    }
    const std::array replacement{DspXmmRegister{1,2,3,4},DspXmmRegister{5,6,7,8}};
    check(runtime.setDspIncomingXmm(block.id,15,replacement),"Rebind starts at current frame");
    std::array<MonoDouble4,3> output{};
    check(runtime.process({},std::array{MonoDouble4OutputBuffer{output}}),"Render rebound trace and its exhaustion");
    check(std::bit_cast<std::uint64_t>(output[0][0])==(1ULL|(2ULL<<32))&&std::bit_cast<std::uint64_t>(output[1][0])==(5ULL|(6ULL<<32))&&output[2]==MonoDouble4{},"Trace exhaustion reads zero");
    runtime.reset();
    check(runtime.process({},std::array{MonoDouble4OutputBuffer{output}})&&std::bit_cast<std::uint64_t>(output[0][0])==(1ULL|(2ULL<<32)),"Reset retains and rewinds caller data");
}
void branchesCallsAndStages(DspBackend backend) {
    Graph graph(backend);auto input=graph.addInput();
    const auto block=graph.addDspCode(compileDspCode("streamin choose;streamout out;cmp choose,0;jz join;movaps xmm1,choose;join:call echo;jmp done;echo:movaps out,xmm1;ret;done:",DspLanguage::Assembler));
    graph.connectDspInput(input,block.id,0);auto runtime=graph.compile(block.outputs);
    constexpr std::array values{1.F,0.F,1.F,0.F};std::array<float,4> output{};
    check(runtime.process(std::array{InputBuffer{values}},std::array{OutputBuffer{output}})&&output==values,"An unwritten branch uses this invocation's entry bank, including local calls");
    Graph stages(backend);const auto staged=stages.addDspCode(compileDspCode("streamout early,now,late;stage(0){pcmpeqd xmm1,xmm1;}stage(1){movaps early,xmm1;pcmpeqd xmm1,xmm1;}stage(2){movaps now,xmm1;pxor xmm1,xmm1;}stage(3){movaps late,xmm1;}",DspLanguage::Assembler));
    auto stagedRuntime=stages.compile(staged.outputs);
    const std::array<DspXmmRegister,1> entry{DspXmmRegister{0x40000000,0x40400000,0x40800000,0x40a00000}};
    check(stagedRuntime.setDspIncomingXmm(staged.id,1,entry),"Bind constant entry across stages");
    std::array<std::array<float,4>,3> outputs{};
    check(stagedRuntime.process({},std::array{OutputBuffer{outputs[0]},OutputBuffer{outputs[1]},OutputBuffer{outputs[2]}}),"Render stage entry bank");
    for(unsigned n=0;n<4;++n) check(outputs[0][n]==2&&outputs[1][n]==2&&outputs[2][n]==(n?2:0),"Stages get independent fresh entry values and retain late output timing");
    for (const auto code : {"streamout out;stage 1;movaps xmm0,_ONE;stage 3;movaps out,xmm0;",
                            "streamout out;stage0;movaps xmm0,_ONE;stage2;movaps out,xmm0;",
                            "streamout out;cvtss2si eax,xmm15;movd xmm0,eax;movaps out,xmm0;"}) {
        Graph entryGraph(backend);const auto b=entryGraph.addDspCode(compileDspCode(code,DspLanguage::Assembler));
        auto run=entryGraph.compile(b.outputs);std::array<float,4> observed{};
        check(run.process({},std::array{OutputBuffer{observed}}),"Use defined incoming values in directive stages and scalar conversion");
        for(float value:observed) check(std::bit_cast<std::uint32_t>(value)==0,"Earlier stage writes do not leak into an incoming bank or conversion");
    }
    Graph poly(backend);const auto voice=poly.addDspCode(compileDspCode("streamout out;movaps xmm0,xmm1;addps xmm0,_ONE;movaps out,xmm0;pcmpeqd xmm1,xmm1;",DspLanguage::Assembler));
    PolyRuntime renderer(poly,{},voice.outputs,{.maxVoices=5,.holdAfterNoteOff=true});
    const std::array events{PolyEvent{0,PolyEventKind::NoteOn,1},PolyEvent{0,PolyEventKind::NoteOn,2},PolyEvent{0,PolyEventKind::NoteOn,3},PolyEvent{0,PolyEventKind::NoteOn,4},PolyEvent{0,PolyEventKind::NoteOn,5}};
    std::array<float,64> mixed{};
    check(renderer.process({},std::array{OutputBuffer{mixed}},events),"Render five Poly voices across two SSE packets");
    for(float v:mixed) check(v==5,"Default incoming bank is fresh in every Poly packet and does not leak between voices");
}
}
int main(){try{for(auto backend:{DspBackend::Interpreter,DspBackend::LlvmOrc})if(dspBackendAvailable(backend)){rawRegisters(backend);doubleViewsAndRebinding(backend);branchesCallsAndStages(backend);}}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
