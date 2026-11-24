#include "../ReferenceFixtures.hpp"
#include "stream/Runtime.hpp"
#include "stream/PolyRuntime.hpp"
#include "stream/PolyRenderer.hpp"

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
#include <string_view>
#include <type_traits>
#include <vector>

namespace {
using namespace openstone::stream;
using Words = std::array<std::uint32_t, 4>;
constexpr std::size_t frames = 601;
void check(bool value, std::string_view message)
{
    if (!value) throw std::runtime_error(std::string(message));
}
std::string fixture(std::string_view name)
{
    std::ifstream input(std::filesystem::path(OPENSTONE_TEST_FIXTURES)/"flowstone"/name, std::ios::binary);
    check(input.good(), "Read unchanged native fixture");
    return {std::istreambuf_iterator<char>(input), {}};
}

template<typename Sample>
auto compile(const Graph& graph, std::span<const NodeId> outputs)
{
    if constexpr (std::is_same_v<Sample, float>) return graph.compile(outputs);
    else if constexpr (std::is_same_v<Sample, double>) return graph.compileMonoDouble(outputs);
    else if constexpr (std::is_same_v<Sample, Mono4>) return graph.compileMono4(outputs);
    else return graph.compileMonoDouble4(outputs);
}
template<typename Sample>
Sample splat(double value)
{
    if constexpr (std::is_arithmetic_v<Sample>) return static_cast<Sample>(value);
    else { Sample result{}; for(unsigned lane=0;lane<4;++lane)result[lane]=value; return result; }
}
template<typename Sample>
bool bitsEqual(const Sample& a, const Sample& b)
{
    return std::bit_cast<std::array<std::uint8_t,sizeof(Sample)>>(a) == std::bit_cast<std::array<std::uint8_t,sizeof(Sample)>>(b);
}

void clockBoundaryAndBackendEntry()
{
    // Literal observations of the native INC / signed CMOVLE sequence, not modulo arithmetic.
    constexpr std::array<std::array<std::uint32_t, 3>, 8> cases{{
        {0, 1, 1}, {999999999, 1, 1000000000}, {1000000000, 1, 1},
        {1000000000, 64, 64}, {2000000000, 1, 1000000001},
        {0x7fffffff, 1, 0x80000000}, {0xffffffff, 1, 0}, {1000000001, 0, 1},
    }};
    for (const auto& test : cases) {
        detail::AssemblerSampleClock clock{test[0]};
        for (unsigned n=0; n<test[1]; ++n) clock.advance();
        clock.finishBuffer();
        check(clock.position == test[2], "Clock wraps only once, at buffer end, with the native signed condition");
    }
    detail::DspProgramData program;
    program.slotCount = 2;
    program.stackSize = 3;
    program.stages[2] = {
        {detail::DspOp::ReadSamplePosition, detail::DspType::Int, 0},
        {detail::DspOp::Hop, detail::DspType::Int, 4, 2},
        {.op=detail::DspOp::Constant, .type=detail::DspType::Int, .bits=7},
        {detail::DspOp::Store, detail::DspType::Int, 1},
    };
    program.stages[3] = {
        {detail::DspOp::ReadSamplePosition, detail::DspType::Int, 0},
        {.op=detail::DspOp::Constant, .type=detail::DspType::Int, .bits=2},
        {detail::DspOp::Constant, detail::DspType::Int},
        {detail::DspOp::Constant, detail::DspType::Int},
        {detail::DspOp::IntegerDivide, detail::DspType::Int},
        {detail::DspOp::Store, detail::DspType::Int, 1},
        {detail::DspOp::Store, detail::DspType::Int, 1},
    };
    const auto native = dspBackendAvailable(DspBackend::LlvmOrc) ? detail::compileDspOrc(program) : nullptr;
    for (const auto position : {0U, 1U, 255U, 256U, 65535U, 1000000000U, 1000000001U, 0x7fffffffU, 0x80000000U, 0xffffffffU}) {
        for (bool orc : {false,true}) {
            if (orc && !native) continue;
            for(const std::uint64_t absolute : {0x123456789ULL,0x12345678aULL}) {
                std::array<detail::DspLanes,2> slots{};
                std::array<detail::DspValue,3> stack{};
                detail::DspMemoryReferences references;
                references.samplePosition = position;
                if (orc) native->stages[2](slots.data(), nullptr, &references, absolute);
                else detail::runDspStage(program.stages[2], slots, {}, references, stack, absolute);
                check(slots[0] == detail::DspLanes{position,position,position,position}, "Both backend entries bind all ECX lanes to the exact unsigned word");
                const std::uint64_t hop = absolute%2 ? 0 : 7;
                check(slots[1] == detail::DspLanes{hop,hop,hop,hop},"HOP keeps using its independent absolute frame");
                if (orc) native->stages[3](slots.data(), nullptr, &references, absolute);
                else detail::runDspStage(program.stages[3], slots, {}, references, stack, absolute);
                check(references.fault.kind==DspFaultKind::IntegerDivideByZero && references.fault.frame==absolute,"Divide faults retain their full 64-bit absolute frame");
            }
        }
    }
}

template<typename Sample>
void stageAndBufferClock(DspBackend backend)
{
    const auto program = compileDspCode(R"(
        streamout early,normal,late,initial;float first=0;
        stage 0;movd xmm0,ecx;cvtdq2ps xmm0,xmm0;shufps xmm0,xmm0,0;movaps first,xmm0;
        stage 1;movd xmm0,ecx;cvtdq2ps xmm0,xmm0;shufps xmm0,xmm0,0;movaps early,xmm0;
        stage 2;movaps xmm1,first;movaps initial,xmm1;
        call clock;mov ecx,999;jmp done;
        clock:push ecx;mov ecx,0;pop ecx;movd xmm0,ecx;cvtdq2ps xmm0,xmm0;shufps xmm0,xmm0,0;movaps normal,xmm0;ret;done:
        stage 3;movd xmm0,ecx;cvtdq2ps xmm0,xmm0;shufps xmm0,xmm0,0;movaps late,xmm0;
    )", DspLanguage::Assembler);
    Graph graph(backend);
    const auto block = graph.addDspCode(program);
    const auto second = graph.addDspCode(program);
    const std::array taps{block.outputs[0],block.outputs[1],block.outputs[2],block.outputs[3],second.outputs[1]};
    auto runtime = compile<Sample>(graph, taps);
    for (const std::size_t buffer : {1,3,7,31,64,257,0}) {
        runtime.reset();
        std::array<std::array<Sample,frames>,5> output{};
        std::array<std::span<Sample>,5> empty{};
        check(runtime.process({}, empty), "Empty call before init does not advance the clock");
        for (std::size_t offset=0, iteration=0; offset<frames; ++iteration) {
            const auto size = buffer ? buffer : std::array<std::size_t,5>{7,1,64,3,31}[iteration%5];
            const auto count = std::min(size, frames-offset);
            std::array<std::span<Sample>,5> out;
            for (unsigned p=0;p<out.size();++p) out[p]=std::span{output[p]}.subspan(offset,count);
            check(runtime.process({},out), "Run entry bindings at mixed buffer sizes");
            check(runtime.process({},empty), "Empty calls between buffers preserve entry state");
            offset += count;
        }
        for (std::size_t n=0;n<frames;++n) for (unsigned p=0;p<5;++p) {
            const auto value = p==3 ? 0 : p==2 ? (n ? n-1 : 0) : n;
            check(bitsEqual(output[p][n],splat<Sample>(value)), "Stages and instances receive the engine clock, not previous register contents or buffer offsets");
        }
    }
}

void unchangedClockKernels(DspBackend backend)
{
    const auto ramp = compileDspCode(fixture("assembler-incoming-clock-ramp.asm"),DspLanguage::Assembler);
    const auto smooth = compileDspCode(fixture("assembler-incoming-clock-smooth.asm"),DspLanguage::Assembler);
    Graph graph(backend);
    const auto a=graph.addDspCode(ramp), b=graph.addDspCode(smooth);
    const auto frequency=graph.addInput(), mode=graph.addInput();
    graph.connectDspInput(frequency,a.id,0);graph.connectDspInput(mode,b.id,0);
    auto runtime=graph.compileMono4(std::array{a.outputs[0],b.outputs[0]});
    std::array<Mono4,frames> frequencies{},modes{},wantedRamp{},wantedSmooth{};
    Mono4 accumulator{},lastRamp{},lastSmooth{};
    for (std::size_t n=0;n<frames;++n) {
        frequencies[n]=Mono4{1.F/32,3.F/32,-1.F/32,-3.F/32};
        for (unsigned lane=0;lane<4;++lane) modes[n][lane]=(n%5==lane ? 1.F : 0.F);
        if (n%4==0) {
            lastRamp=accumulator;
            for(unsigned lane=0;lane<4;++lane) {
                const float value=accumulator[lane]+frequencies[n][lane];
                accumulator[lane]=value-static_cast<int>(value);
            }
        }
        if(n%64==0)for(unsigned lane=0;lane<4;++lane)lastSmooth[lane]=modes[n][lane]==1 ? 1.F : 0.F;
        wantedRamp[n]=lastRamp;wantedSmooth[n]=lastSmooth;
    }
    for(const std::size_t size:{1,3,7,31,64,257}) {
        runtime.reset();std::array<Mono4,frames> rampOut{},smoothOut{};
        for(std::size_t offset=0;offset<frames;offset+=size) {
            const auto count=std::min(size,frames-offset);
            check(runtime.process(std::array{Mono4InputBuffer{std::span{frequencies}.subspan(offset,count)},Mono4InputBuffer{std::span{modes}.subspan(offset,count)}},
                std::array{Mono4OutputBuffer{std::span{rampOut}.subspan(offset,count)},Mono4OutputBuffer{std::span{smoothOut}.subspan(offset,count)}}),"Render exact native ECX-dependent kernels");
        }
        for(std::size_t n=0;n<frames;++n) {
            check(bitsEqual(rampOut[n],wantedRamp[n]),"Unchanged ramp updates on every fourth engine sample");
            check(bitsEqual(smoothOut[n],wantedSmooth[n]),"Unchanged Smooth checks exactly every 64th engine sample");
        }
    }
}

void incomingStackAndMemory(DspBackend backend)
{
    const auto writer=compileDspCode(fixture("assembler-mem-byte-writer.asm"),DspLanguage::Assembler);
    const auto reader=compileDspCode(fixture("mem-reader-byte.asm"),DspLanguage::Assembler);
    Graph writeGraph(backend),readGraph(backend);
    const auto w=writeGraph.addDspCode(writer), r=readGraph.addDspCode(reader);
    const auto wi=writeGraph.addInput(),wv=writeGraph.addInput(),ri=readGraph.addInput();
    writeGraph.connectDspInput(wi,w.id,1);writeGraph.connectDspInput(wv,w.id,2);
    readGraph.connectDspInput(ri,r.id,1);
    auto wr=writeGraph.compileMono4(w.outputs),rd=readGraph.compileMono4(r.outputs);
    for(const std::size_t size:{1,3,7,31,64}) {
        wr.reset();rd.reset();
        const openstone::green::Memory writeMemory({1,2,3,4,5,6,7,8}),readMemory({11,22,33,44,55,66,77,88});
        check(wr.setDspMemRefInput(w.id,0,writeMemory) && rd.setDspMemRefInput(r.id,0,readMemory),"Bind independent shared Mem fixtures");
        std::array<Mono4,frames> indices{},rawIndices{},values{},writeOut{},readOut{};
        auto expected=writeMemory.snapshotBytes();
        for(std::size_t n=0;n<frames;++n)for(unsigned lane=0;lane<4;++lane) {
            const auto offset=static_cast<std::uint32_t>((n+lane)%5); // Whole DWORD fits at every address.
            indices[n][lane]=static_cast<float>(offset);
            rawIndices[n][lane]=std::bit_cast<float>(offset);
            values[n][lane]=std::bit_cast<float>(static_cast<std::uint32_t>((n*7+lane)&255));
            expected[offset]=static_cast<std::uint8_t>((n*7+lane)&255);
        }
        for(std::size_t offset=0;offset<frames;offset+=size) {
            const auto count=std::min(size,frames-offset);
            check(wr.process(std::array{Mono4InputBuffer{std::span{rawIndices}.subspan(offset,count)},Mono4InputBuffer{std::span{values}.subspan(offset,count)}},
                std::array{Mono4OutputBuffer{std::span{writeOut}.subspan(offset,count)}}),"Run unchanged writer with opaque incoming saves");
            check(rd.process(std::array{Mono4InputBuffer{std::span{indices}.subspan(offset,count)}},
                std::array{Mono4OutputBuffer{std::span{readOut}.subspan(offset,count)}}),"Run unchanged reader with opaque incoming EDX");
        }
        check(writeMemory.snapshotBytes()==expected,"Only addressed bytes change, with native lane order");
        for(std::size_t n=0;n<frames;++n)for(unsigned lane=0;lane<4;++lane) {
            check(std::bit_cast<Words>(writeOut[n])[lane]==((n*7+lane)&255),"Writer reports each raw byte");
            check(readOut[n][lane]==11.F*(1+(n+lane)%5),"Reader loads the expected numeric byte");
        }
        // Null Mem follows the original no-memory branch without requiring synthetic caller values.
        wr.reset();rd.reset();
        check(wr.setDspMemRefInput(w.id,0,{}) && rd.setDspMemRefInput(r.id,0,{}),"Bind null Mem");
        std::array<Mono4,1> zero{},out{};
        check(wr.process(std::array{Mono4InputBuffer{zero},Mono4InputBuffer{zero}},std::array{Mono4OutputBuffer{out}}) && out==zero,"Writer null branch preserves zero outlet");
        check(rd.process(std::array{Mono4InputBuffer{zero}},std::array{Mono4OutputBuffer{out}}) && out==zero,"Reader null branch preserves zero outlet");
    }
}

void remainingRecoveredHelpers(DspBackend backend)
{
    using openstone::green::Memory;
    using openstone::green::MemoryArray;
    const auto nibble=compileDspCode(fixture("assembler-mem-nibble-reader.asm"),DspLanguage::Assembler);
    const auto wave=compileDspCode(fixture("assembler-memarray-flat-reader.asm"),DspLanguage::Assembler);
    Graph ng(backend),wg(backend);const auto n=ng.addDspCode(nibble),w=wg.addDspCode(wave);
    ng.connectDspInput(ng.addInput(),n.id,1);
    wg.connectDspInput(wg.addInput(),w.id,1);wg.connectDspInput(wg.addInput(),w.id,2);
    auto nr=ng.compileMono4(n.outputs),wr=wg.compileMono4(w.outputs);
    const std::vector<std::uint8_t> bytes{0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef};
    constexpr std::array samples0{-1.F,2.F,3.F,4.F,5.F,6.F,7.F,8.F};
    constexpr std::array samples1{16.F,-8.F,4.F,-2.F};
    const MemoryArray waves({Memory::fromSamples(samples0),Memory::fromSamples(samples1)});
    std::array<Mono4,frames> rawIndex{},waveIndex{},sampleIndex{};
    for(unsigned f=0;f<frames;++f)for(unsigned lane=0;lane<4;++lane) {
        rawIndex[f][lane]=std::bit_cast<float>(static_cast<std::uint32_t>((f+lane)%5));
        waveIndex[f][lane]=(f+lane)%2;
        sampleIndex[f][lane]=static_cast<int>((f+lane)%12)-2;
    }
    for(const unsigned size:{1,3,7,31,64,257}) {
        nr.reset();wr.reset();
        check(nr.setDspMemRefInput(n.id,0,Memory(bytes)) && wr.setDspMemArrayInput(w.id,0,waves),"Bind original nibble and MemArray helper data");
        std::array<Mono4,frames> high{},low{},out{};
        for(unsigned offset=0;offset<frames;offset+=size) {
            const auto count=std::min<std::size_t>(size,frames-offset);
            check(nr.process(std::array{Mono4InputBuffer{std::span{rawIndex}.subspan(offset,count)}},
                std::array{Mono4OutputBuffer{std::span{high}.subspan(offset,count)},Mono4OutputBuffer{std::span{low}.subspan(offset,count)}}),"Run unchanged nibble helper");
            check(wr.process(std::array{Mono4InputBuffer{std::span{waveIndex}.subspan(offset,count)},Mono4InputBuffer{std::span{sampleIndex}.subspan(offset,count)}},
                std::array{Mono4OutputBuffer{std::span{out}.subspan(offset,count)}}),"Run unchanged MemArray helper");
        }
        for(unsigned f=0;f<frames;++f)for(unsigned lane=0;lane<4;++lane) {
            const auto byte=bytes[(f+lane)%5];
            check(std::bit_cast<Words>(high[f])[lane]==(byte>>4) && std::bit_cast<Words>(low[f])[lane]==(byte&15),"Nibble helper preserves its known temporary stack values");
            const auto samples=waveIndex[f][lane]==0 ? std::span<const float>{samples0} : std::span<const float>{samples1};
            const auto i=std::clamp(static_cast<int>(sampleIndex[f][lane]),0,static_cast<int>(samples.size()-1));
            check(out[f][lane]==samples[i],"MemArray helper reads each wave and clamped sample without a synthetic entry prologue");
        }
    }
}

void incomingByteAliases(DspBackend backend)
{
    const auto program=compileDspCode(R"(
        streamout low,high,changed;
        mov eax,0;mov al,cl;movd xmm0,eax;movaps low,xmm0;
        mov eax,0;mov al,ch;movd xmm0,eax;movaps high,xmm0;
        mov cl,0xab;mov ch,0xcd;movd xmm0,ecx;movaps changed,xmm0;
    )",DspLanguage::Assembler);
    Graph graph(backend);const auto block=graph.addDspCode(program);auto runtime=graph.compileMono4(block.outputs);
    for(const std::size_t size:{1,3,7,31,64,257}) {
        runtime.reset();std::array<std::array<Mono4,frames>,3> output{};
        for(std::size_t offset=0;offset<frames;offset+=size) {
            const auto count=std::min(size,frames-offset);
            std::array<Mono4OutputBuffer,3> out;
            for(unsigned p=0;p<3;++p)out[p]=std::span{output[p]}.subspan(offset,count);
            const int round=std::fegetround(),flags=std::fetestexcept(FE_ALL_EXCEPT);
            check(runtime.process({},out),"Run incoming CL/CH and partial writes");
            check(std::fegetround()==round && std::fetestexcept(FE_ALL_EXCEPT)==flags,"Entry bindings preserve the caller's floating-point state");
        }
        for(unsigned n=0;n<frames;++n) {
            check(std::bit_cast<Words>(output[0][n])==Words{n&255,0,0,0},"CL reads the low incoming counter byte");
            check(std::bit_cast<Words>(output[1][n])==Words{(n>>8)&255,0,0,0},"CH reads the high incoming counter byte");
            check(std::bit_cast<Words>(output[2][n])==Words{(n&0xffff0000U)|0xcdab,0,0,0},"Partial writes change the local ECX without modifying the next engine sample");
        }
    }
}

void opaqueCallsAndDiagnostics(DspBackend backend)
{
    const std::array bodies{
        "push eax;mov eax,9;pop eax;",
        "push rdx;mov edx,9;pop rdx;",
        "push esi;push edi;push rbp;mov esi,1;mov edi,2;mov ebp,3;pop rbp;pop edi;pop esi;",
        "push eax;call outer;pop eax;jmp end;outer:push ebx;call inner;pop ebx;ret;inner:push edx;mov edx,9;pop edx;ret;end:",
        "push eax;mov eax,3;call recurse;pop eax;jmp end;recurse:push ebx;cmp eax,0;jz base;dec eax;call recurse;base:pop ebx;ret;end:",
        "cmp ecx,0;jz alternate;push eax;jmp joined;alternate:mov eax,7;push eax;joined:pop ebx;",
    };
    for(const auto body:bodies) {
        const auto program=compileDspCode("streamout out;"+std::string(body)+"movd xmm0,ecx;cvtdq2ps xmm0,xmm0;shufps xmm0,xmm0,0;movaps out,xmm0;",DspLanguage::Assembler);
        Graph graph(backend);const auto block=graph.addDspCode(program);auto runtime=graph.compileMono4(block.outputs);
        std::array<Mono4,13> output{};check(runtime.process({},std::array{Mono4OutputBuffer{output}}),"Run opaque incoming saves across calls/joins");
        for(unsigned n=0;n<output.size();++n)check(output[n]==Mono4{float(n),float(n),float(n),float(n)},"Opaque saves leave the observable engine counter intact");
    }
    for(const auto source:{
        "mov eax,ebx;", "push eax;mov eax,7;pop eax;movd xmm0,eax;", "push eax;pop ecx;movd xmm0,ecx;",
        "push rdi;pop rbp;mov eax,[rbp];", "cmp ecx,0;jz skip;mov eax,1;skip:push eax;pop ebx;movd xmm0,ebx;",
        "cmp ecx,0;jz alternate;mov eax,9;push eax;jmp joined;alternate:push ebx;joined:pop edx;movd xmm0,edx;",
        "jz end;end:", "pop eax;", "push eax;",
    }) {
        bool rejected=false;
        try {static_cast<void>(compileDspCode("streamout out;"+std::string(source),DspLanguage::Assembler));}
        catch(const DspCompileError& error){rejected=error.line>0 && error.column>0;}
        check(rejected,"Unknown GPR entry values cannot escape as arithmetic, addresses, flags or partially defined branch values");
    }
}

void polyRendererClock(DspBackend backend)
{
    const auto program=compileDspCode("polyout out;movd xmm0,ecx;cvtdq2ps xmm0,xmm0;shufps xmm0,xmm0,0;movaps out,xmm0;",DspLanguage::Assembler);
    Graph graph(backend);const auto block=graph.addDspCode(program);
    for(const unsigned size:{1,3,7,31,64}) {
        auto bank=std::make_shared<VoiceBank>();bank->configure({.capacity=5});
        auto snapshot=std::make_shared<VoiceSnapshot>(bank);
        PolyRenderer renderer(graph,block.outputs,{},snapshot,true,PolyMix::Sum,0,std::nullopt);
        for(unsigned pass=0;pass<2;++pass) {
            bank->clear();renderer.reset();
            for(unsigned iteration=0;iteration<10;++iteration) {
                if(iteration==2 || iteration==6) {
                    std::array<openstone::green::NoteEvent,5> notes{};
                    for(unsigned i=0;i<5;++i){notes[i].noteOn=true;notes[i].key=60+i;notes[i].velocity=100;}
                    static_cast<void>(bank->apply(notes));
                }
                if(iteration==4)bank->clear();
                renderer.beginBlock(iteration*size);
                const unsigned active=iteration<2 || (iteration>=4 && iteration<6) ? 0 : 5;
                for(unsigned n=0;n<size;++n)
                    check(renderer.process({})==double(active*(iteration*size+n)),"Editor Poly renderer shares the engine counter across both packets, idle periods, new notes and reset");
            }
        }
    }
}

void polyEngineClock(DspBackend backend)
{
    // Staggered allocations in two SSE packets; a reused slot and long idle intervals
    // must see engine time, not the note's age or the packet's active-frame count.
    const auto program=compileDspCode("polyout out;movd xmm0,ecx;cvtdq2ps xmm0,xmm0;shufps xmm0,xmm0,0;movaps out,xmm0;",DspLanguage::Assembler);
    Graph graph(backend);const auto block=graph.addDspCode(program);
    PolyRuntime runtime(graph,{},std::array{block.outputs[0]}, {.maxVoices=5,.holdAfterNoteOff=true});
    for(const std::size_t size:{1,3,7,31,64,257}) {
        runtime.reset();
        const std::array events{PolyEvent{5,PolyEventKind::NoteOn,1},PolyEvent{8,PolyEventKind::NoteOn,2},
            PolyEvent{9,PolyEventKind::NoteOn,3},PolyEvent{10,PolyEventKind::NoteOn,4},PolyEvent{11,PolyEventKind::NoteOn,5},
            PolyEvent{15,PolyEventKind::AllSoundOff},PolyEvent{80,PolyEventKind::NoteOn,6}};
        std::array<float,frames> output{};
        check(runtime.process({},std::array{OutputBuffer{std::span<float>{}}}),"Empty Poly call does not advance entry time");
        for(std::size_t offset=0;offset<frames;offset+=size) {
            const auto count=std::min(size,frames-offset);std::vector<PolyEvent> local;
            for(auto event:events)if(event.frame>=offset && event.frame<offset+count){event.frame-=offset;local.push_back(event);}
            check(runtime.process({},std::array{OutputBuffer{std::span{output}.subspan(offset,count)}},local),"Run packed Poly entry bindings");
        }
        for(unsigned n=0;n<frames;++n) {
            const unsigned voices=n<5 ? 0 : n<8 ? 1 : n<9 ? 2 : n<10 ? 3 : n<11 ? 4 : n<15 ? 5 : n<80 ? 0 : 1;
            check(output[n]==float(n*voices),"New/reused voices share root engine ECX across packets and idle samples");
        }
    }
}
}
int main()
{
    try {
        clockBoundaryAndBackendEntry();
        for(auto backend:{DspBackend::Interpreter,DspBackend::LlvmOrc}) {
            if(!dspBackendAvailable(backend))continue;
            stageAndBufferClock<float>(backend);stageAndBufferClock<double>(backend);
            stageAndBufferClock<Mono4>(backend);stageAndBufferClock<MonoDouble4>(backend);
            openstone::tests::withReferenceSources("unchangedClockKernels", [&] { unchangedClockKernels(backend); });openstone::tests::withReferenceSources("incomingStackAndMemory", [&] { incomingStackAndMemory(backend); });openstone::tests::withReferenceSources("remainingRecoveredHelpers", [&] { remainingRecoveredHelpers(backend); });incomingByteAliases(backend);opaqueCallsAndDiagnostics(backend);polyEngineClock(backend);polyRendererClock(backend);
        }
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
