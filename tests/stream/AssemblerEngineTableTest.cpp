#include "../ReferenceFixtures.hpp"
#include "stream/Runtime.hpp"
#include "stream/PolyRuntime.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace allocations { bool enabled=false;std::size_t count=0; }
void* operator new(std::size_t n){if(allocations::enabled)++allocations::count;if(auto* p=std::malloc(n ? n : 1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}
void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}
void operator delete[](void* p,std::size_t)noexcept{std::free(p);}

namespace {
using namespace openstone::stream;
using openstone::green::Memory;
using Words=std::array<std::uint32_t,4>;
constexpr std::size_t frames=137;
void check(bool value,std::string_view message){if(!value)throw std::runtime_error(std::string(message));}
std::string fixture(std::string_view name){std::ifstream f(std::filesystem::path(OPENSTONE_TEST_FIXTURES)/"flowstone"/name,std::ios::binary);check(f.good(),"Read unchanged engine-table fixture");return {std::istreambuf_iterator<char>(f),{}};}
Memory bytes(std::size_t n){return Memory(std::vector<std::uint8_t>(n));}
Memory integer(std::uint32_t n){auto m=bytes(4);m.writeWordAt(0,n);return m;}
DspEnginePointerTable cell(const Memory& data,unsigned width=8,std::size_t offset=0,bool writable=true)
{
    return {{{bytes(width),true},{data,writable}},{{0,0,1,offset,width}}};
}
bool noAlloc(auto&& process)
{
    const auto old=allocations::count;allocations::enabled=true;const bool result=process();allocations::enabled=false;
    check(allocations::count==old,"Engine-table rendering must not allocate");return result;
}

void originalSampleIndex(DspBackend backend)
{
    for(bool x64:{false,true})for(bool doubles:{false,true}) {
        const auto source=fixture(std::string("engine-sample-index-")+(doubles ? "double-" : "float-")+(x64 ? "x64.asm" : "x86.asm"));
        Graph graph(backend);const auto b=graph.addDspCode(compileDspCode((doubles ? "" : "streamout out0;")+source,DspLanguage::Assembler));
        const auto data=Memory::fromSamples(std::array{0.3125F,-1.25F,3.5F});
        auto binding=cell(data,x64 ? 8 : 4);
        check(graph.bindDspEnginePointerTable(b.id,"p_sampleIndexAddress",binding),"Bind native pointer cell to caller sample-index data");
        for(const std::size_t size:{1,3,7,31,64,257}) {
            if(doubles) {
                auto runtime=graph.compileMonoDouble4(b.outputs);std::array<MonoDouble4,frames> out{};
                for(unsigned offset=0;offset<frames;offset+=size){const auto count=std::min<std::size_t>(size,frames-offset);const auto round=std::fegetround(),flags=std::fetestexcept(FE_ALL_EXCEPT);
                    check(noAlloc([&]{return runtime.process({},std::array{MonoDouble4OutputBuffer{std::span{out}.subspan(offset,count)}});}),"Render native sample-index Double kernel");
                    check(std::fegetround()==round && std::fetestexcept(FE_ALL_EXCEPT)==flags,"Pointer lookup preserves caller floating-point state");}
                for(const auto& packet:out)for(const auto v:packet)check(v==0.3125,"The original FLD reads Float data, then DFST widens it to Double");
                binding.cells[0].targetOffset=4;check(runtime.setDspEnginePointerTable(b.id,"p_sampleIndexAddress",binding),"Retarget native sample-index cell between buffers");
                std::array<MonoDouble4,1> next{};check(runtime.process({},std::array{MonoDouble4OutputBuffer{next}}),"Read changed pointer target");for(auto v:next[0])check(v==-1.25,"No recompilation is required for a different referenced offset");binding.cells[0].targetOffset=0;
            } else {
                auto runtime=graph.compileMono4(b.outputs);std::array<Mono4,frames> out{};
                for(unsigned offset=0;offset<frames;offset+=size){const auto count=std::min<std::size_t>(size,frames-offset);check(noAlloc([&]{return runtime.process({},std::array{Mono4OutputBuffer{std::span{out}.subspan(offset,count)}});}),"Render native sample-index Float kernel");}
                for(const auto& packet:out)check(packet==Mono4{0.3125F,0.3125F,0.3125F,0.3125F},"Both native absolute and register-based cell forms read every lane");
                auto null=binding;null.cells[0].targetRegion.reset();check(runtime.setDspEnginePointerTable(b.id,"p_sampleIndexAddress",null),"Supply explicit null cell");
                std::array<Mono4,1> next{};check(runtime.process({},std::array{Mono4OutputBuffer{next}}) && next[0]==Mono4{},"Null pointer dereferences follow bounded zero-read policy");
            }
        }
    }
}

void originalBufferTable(DspBackend backend)
{
    for(bool x64:{false,true}) {
        auto source=fixture(x64 ? "engine-buffer-table-x64.asm" : "engine-buffer-table-x86.asm");
        std::string declarations,readout;
        for(unsigned lane=0;lane<4;++lane){declarations+="streamout tap"+std::to_string(lane)+";";readout+="movaps xmm0,[rbp+"+std::to_string(256+lane*32)+"];movaps tap"+std::to_string(lane)+",xmm0;";}
        const auto p=compileDspCode(declarations+source+readout,DspLanguage::Assembler);
        Graph graph(backend);const auto b=graph.addDspCode(p);const unsigned width=x64 ? 8 : 4;
        DspEnginePointerTable table{{{bytes(width),false},{bytes(width*4),false}},{{0,0,1,0,width}}};
        std::array<std::array<float,frames>,4> expected{};
        for(unsigned lane=0;lane<4;++lane){for(unsigned n=0;n<frames;++n)expected[lane][n]=(lane+1)*0.125F+(lane%2 ? -1.F : 1.F)*n/1024.F;
            table.regions.push_back({Memory::fromSamples(expected[lane]),false});table.cells.push_back({1,lane*width,lane+2,0,width});}
        const auto current=integer(0),last=integer(frames-1),channels=integer(4);auto offsets=bytes(16);for(unsigned lane=0;lane<4;++lane)offsets.writeWordAt(lane*4,256+lane*32);
        check(graph.bindDspEnginePointerTable(b.id,"p_buffers",table) && graph.bindDspEnginePointer(b.id,"p_curIndex",current) && graph.bindDspEnginePointer(b.id,"p_lastIndex",last,false) && graph.bindDspEnginePointer(b.id,"p_channels",channels,false) && graph.bindDspEnginePointer(b.id,"p_outOffsetArray",offsets,false),"Bind native table root, channel pointers, cursors and output-frame offsets");
        auto runtime=graph.compileMono4(b.outputs);
        for(const std::size_t size:{1,3,7,31,64,257}) {
            runtime.reset();current.writeWordAt(0,0);std::array<std::array<Mono4,frames>,4> out{};
            for(unsigned offset=0;offset<frames;offset+=size){const auto count=std::min<std::size_t>(size,frames-offset);std::array<Mono4OutputBuffer,4> outputs;for(unsigned lane=0;lane<4;++lane)outputs[lane]=std::span{out[lane]}.subspan(offset,count);
                check(noAlloc([&]{return runtime.process({},outputs);}),"Render unchanged multi-channel buffer reader");}
            for(unsigned n=0;n<frames;++n)for(unsigned channel=0;channel<4;++channel)for(unsigned lane=0;lane<4;++lane)check(out[channel][n][lane]==expected[channel][n],"Four/eight-byte channel table entries route every original sample to all frame lanes");
            check(current.wordAt(0)==frames,"Original buffer reader increments shared caller cursor once per sample");
            std::array<std::array<Mono4,1>,4> held{};std::array<Mono4OutputBuffer,4> outputs;for(unsigned lane=0;lane<4;++lane)outputs[lane]=held[lane];
            check(runtime.process({},outputs) && current.wordAt(0)==frames,"Past-last cursor leaves frame values and cursor held");
            for(unsigned ch=0;ch<4;++ch)check(held[ch][0][0]==expected[ch].back(),"Buffer-complete branch preserves the last frame packet");
        }
    }
}

void indexedNestedAndBounds(DspBackend backend)
{
    for(unsigned width:{4,8}) {
        const auto source="__const table;streamin pick;streamout out,live;mov rax,table;mov rax,[rax];mov ecx,pick;mov rdx,[rax+rcx*"+std::to_string(width)+"];cmp rdx,0;jz absent;mov rdi,rdx;push rdi;pop rsi;movaps xmm0,[rsi];movaps out,xmm0;movaps xmm0,_ONE;movaps live,xmm0;jmp done;absent:movaps xmm0,_ZERO;movaps out,xmm0;movaps live,xmm0;done:";
        Graph graph(backend);const auto b=graph.addDspCode(compileDspCode(source,DspLanguage::Assembler));graph.connectDspInput(graph.addInput(),b.id,0);
        const auto data=Memory::fromSamples(std::array{1.F,2.F,3.F,4.F,5.F});
        DspEnginePointerTable table{{{bytes(width),true},{bytes(width*3),true},{data,true}},{{0,0,1,0,width},{1,0,2,0,width},{1,width,std::nullopt,0,width},{1,width*2,2,4,width}}};
        check(graph.bindDspEnginePointerTable(b.id,"table",table),"Bind nested table with aliases, explicit null and target addends");auto runtime=graph.compileMono4(b.outputs);
        constexpr std::array<std::int32_t,8> picks{0,1,2,3,-1,-4,0x7fffffff,0};
        for(const std::size_t size:{1,3,7,31,64,257}) {
            runtime.reset();std::array<Mono4,frames> input{},out{},live{};for(unsigned n=0;n<frames;++n)input[n]=std::bit_cast<Mono4>(Words{static_cast<std::uint32_t>(picks[n%picks.size()]),0,0,0});
            for(unsigned offset=0;offset<frames;offset+=size){const auto count=std::min<std::size_t>(size,frames-offset);check(noAlloc([&]{return runtime.process(std::array{Mono4InputBuffer{std::span{input}.subspan(offset,count)}},std::array{Mono4OutputBuffer{std::span{out}.subspan(offset,count)},Mono4OutputBuffer{std::span{live}.subspan(offset,count)}});}),"Render indexed pointer chain");}
            for(unsigned n=0;n<frames;++n){const auto choice=picks[n%picks.size()];const bool valid=choice==0 || choice==2;check(out[n]==(choice==0 ? Mono4{1,2,3,4} : choice==2 ? Mono4{2,3,4,5} : Mono4{}) && live[n]==(valid ? Mono4{1,1,1,1} : Mono4{}),"Only declared in-bounds cells resolve; negative indices cannot escape into the preceding root cell");}
        }
        auto replacement=Memory::fromSamples(std::array{-4.F,-3.F,-2.F,-1.F,0.F});table.regions[2].memory=replacement;check(runtime.setDspEnginePointerTable(b.id,"table",table),"Replace child region without replacing the code");std::array<Mono4,1> in{},out{},live{};
        check(runtime.process(std::array{Mono4InputBuffer{in}},std::array{Mono4OutputBuffer{out},Mono4OutputBuffer{live}}) && out[0]==Mono4{-4,-3,-2,-1},"Next buffer uses the rebound child graph");
    }
}

void childWrites(DspBackend backend)
{
    for(bool writable:{false,true}) {
        const auto source="__const table;streamin index,value;streamout out;mov rax,table;mov rax,[rax];mov ecx,index;mov edx,value;mov [rax+ecx*4],edx;mov eax,[rax+ecx*4];movd xmm0,eax;movaps out,xmm0;";
        Graph graph(backend);const auto b=graph.addDspCode(compileDspCode(source,DspLanguage::Assembler));graph.connectDspInput(graph.addInput(),b.id,0);graph.connectDspInput(graph.addInput(),b.id,1);
        const Memory storage(std::vector<std::uint8_t>(26,0xa5));auto binding=cell(storage.view(5,16,1),8,0,writable);check(graph.bindDspEnginePointerTable(b.id,"table",binding),"Bind writable/read-only child byte view");auto runtime=graph.compileMono4(b.outputs);
        constexpr std::array<std::int32_t,7> positions{-17,-1,0,1,3,4,1024};
        for(unsigned size:{1,3,7,31,64,257}) {
            runtime.reset();std::array<Mono4,frames> index{},value{},out{};
            for(unsigned n=0;n<frames;++n){index[n]=std::bit_cast<Mono4>(Words{static_cast<std::uint32_t>(positions[n%positions.size()]),0,0,0});value[n]=std::bit_cast<Mono4>(Words{0x12340000U+n,0,0,0});}
            for(unsigned offset=0;offset<frames;offset+=size){const auto count=std::min<std::size_t>(size,frames-offset);check(noAlloc([&]{return runtime.process(std::array{Mono4InputBuffer{std::span{index}.subspan(offset,count)},Mono4InputBuffer{std::span{value}.subspan(offset,count)}},std::array{Mono4OutputBuffer{std::span{out}.subspan(offset,count)}});}),"Write data through a loaded child reference");}
            for(unsigned n=0;n<frames;++n){const auto index=positions[n%positions.size()];const auto expected=index>=0 && index<4 ? writable ? 0x12340000U+n : 0xa5a5a5a5U : 0U;check(std::bit_cast<Words>(out[n])==Words{expected,0,0,0},"Loaded references enforce child bounds and write permission, including invalid indices into other bound regions");}
            for(unsigned n=0;n<26;++n)if(n<5 || n>=21)check(storage.byte(n)==0xa5,"Child stores preserve owner bytes outside the view");
        }
    }
}

void aliasesInvalidationAndOpacity(DspBackend backend)
{
    for(bool external:{false,true})for(int position:{-1,0,3,7,8}) {
        const auto source=std::string("__const table;memrefin alias;streamout before,after,raw,held;mov rax,table;mov rdi,[rax];movaps xmm0,[rdi];movaps before,xmm0;mov eax,[rax];movd xmm0,eax;movaps raw,xmm0;")+
            (external ? "mov rbx,alias[0];" : "mov rbx,table;")+"mov ecx,"+std::to_string(position)+";mov edx,0;mov [rbx+ecx],dl;mov rax,table;mov rsi,[rax];movaps xmm0,[rsi];movaps after,xmm0;movaps xmm0,[rdi];movaps held,xmm0;";
        Graph graph(backend);const auto b=graph.addDspCode(compileDspCode(source,DspLanguage::Assembler));const auto data=Memory::fromSamples(std::array{1.F,2.F,3.F,4.F});auto table=cell(data);
        check(graph.bindDspEnginePointerTable(b.id,"table",table),"Bind typed pointer cell for alias invalidation");auto runtime=graph.compileMono4(b.outputs);check(runtime.setDspMemRefInput(b.id,0,table.regions[0].memory),"Bind alias of pointer bytes");
        std::array<Mono4,1> before{},after{},raw{},held{};check(runtime.process({},std::array{Mono4OutputBuffer{before},Mono4OutputBuffer{after},Mono4OutputBuffer{raw},Mono4OutputBuffer{held}}),"Render pointer-byte overwrite through engine/Mem alias");
        check(before[0]==Mono4{1,2,3,4} && held[0]==before[0] && raw[0]==Mono4{} && after[0]==(position>=0 && position<8 ? Mono4{} : Mono4{1,2,3,4}),"Pointer bytes cannot become samples; overlapping writes invalidate cell provenance even when bytes stay zero");
    }
    const auto source="__const table;streamout out;mov rax,table;mov rax,[rax];movaps xmm0,[rax];movaps out,xmm0;";
    Graph graph(backend);const auto b=graph.addDspCode(compileDspCode(source,DspLanguage::Assembler));auto table=cell(Memory::fromSamples(std::array{1.F,2.F,3.F,4.F}));
    check(graph.bindDspEnginePointerTable(b.id,"table",table),"Bind host-edited cell");auto runtime=graph.compileMono4(b.outputs);table.regions[0].memory.writeByte(0,1);std::array<Mono4,1> out{};
    check(runtime.process({},std::array{Mono4OutputBuffer{out}}) && out[0]==Mono4{},"Unannounced host byte changes do not manufacture pointer targets");
    check(runtime.setDspEnginePointerTable(b.id,"table",table) && runtime.process({},std::array{Mono4OutputBuffer{out}}) && out[0]==Mono4{1,2,3,4},"Explicit rebinding validates the caller's updated relationship");
}

void precisionLifetimeAndContracts(DspBackend backend)
{
    const auto p=compileDspCode("__const table;streamdoubleout out;mov rax,table;mov rax,[rax];movapd xmm0,[rax];movapd out[0],xmm0;movapd xmm0,[rax+16];movapd out[2],xmm0;",DspLanguage::Assembler);
    auto memory=bytes(32);constexpr std::array values{1.0+0x1p-40,-2.0-0x1p-39,4.0+0x1p-38,8.0+0x1p-37};for(unsigned i=0;i<4;++i){const auto bits=std::bit_cast<std::uint64_t>(values[i]);memory.writeWordAt(i*8,bits);memory.writeWordAt(i*8+4,bits>>32);}
    const auto make=[&]{Graph local(backend);const auto b=local.addDspCode(p);check(local.bindDspEnginePointerTable(b.id,"table",cell(memory)),"Retain full table graph after caller lifetime");return local.compileMonoDouble4(b.outputs);};
    auto runtime=make();std::array<MonoDouble4,frames> out{};for(const unsigned size:{1,3,7,31,64,257}){runtime.reset();for(unsigned offset=0;offset<frames;offset+=size){const auto count=std::min<std::size_t>(size,frames-offset);check(runtime.process({},std::array{MonoDouble4OutputBuffer{std::span{out}.subspan(offset,count)}}),"Read Double data through pointer cell");}for(const auto& packet:out)check(packet==values,"Pointer tables retain all four independent Double precision lanes");}
    Graph graph(backend);const auto b=graph.addDspCode(p);auto valid=cell(memory);check(!graph.bindDspEnginePointerTable(b.id,"missing",valid),"Unknown engine names are rejected");
    for(unsigned invalid=0;invalid<6;++invalid){auto bad=valid;if(invalid==0)bad.cells[0].bytes=3;if(invalid==1)bad.cells[0].offset=7;if(invalid==2)bad.cells[0].targetRegion=2;if(invalid==3)bad.cells[0].targetOffset=33;if(invalid==4)bad.cells.push_back(bad.cells[0]);if(invalid==5){bad.cells[0].targetRegion.reset();bad.cells[0].targetOffset=1;}check(!graph.bindDspEnginePointerTable(b.id,"table",bad),"Reject malformed, overlapping or out-of-bounds table relationships");check(!runtime.setDspEnginePointerTable(b.id,"table",bad),"Invalid rebind does not disturb the installed graph");}
    check(runtime.process({},std::array{MonoDouble4OutputBuffer{out}}) && out[0]==values,"Invalid update retains the previous valid table");
    Graph cyclic(backend);const auto cb=cyclic.addDspCode(compileDspCode("__const table;streamout out;mov rax,table;mov rax,[rax];mov rax,[rax];mov rax,[rax];cmp rax,0;jz empty;movaps xmm0,_ONE;movaps out,xmm0;jmp done;empty:movaps xmm0,_ZERO;movaps out,xmm0;done:",DspLanguage::Assembler));
    const auto root=bytes(8);DspEnginePointerTable cycle{{{root,false},{root.view(0,8,1),false}},{{0,0,1,0,8}}};check(cyclic.bindDspEnginePointerTable(cb.id,"table",cycle),"Bind alias of a typed cell without duplicate pointer metadata");auto cr=cyclic.compile(cb.outputs);std::array<float,1> one{};check(cr.process({},std::array{OutputBuffer{one}}) && one[0]==1.F,"Finite traversals through aliased cyclic cells keep their explicit relationship");
    Graph walking(backend);const auto wb=walking.addDspCode(compileDspCode("__const table;streamout out;mov rax,table;mov ebx,0;walk:mov rax,[rax];inc ebx;cmp rax,0;jnz walk;movd xmm0,ebx;cvtdq2ps xmm0,xmm0;movaps out,xmm0;",DspLanguage::Assembler));
    DspEnginePointerTable chain{{{bytes(8),false},{bytes(8),false},{bytes(8),false}},{{0,0,1,0,8},{1,0,2,0,8},{2,0,std::nullopt,0,8}}};check(walking.bindDspEnginePointerTable(wb.id,"table",chain),"Bind null-terminated pointer chain");auto wr=walking.compile(wb.outputs);check(wr.process({},std::array{OutputBuffer{one}}) && one[0]==3.F,"Root and loaded pointers share provenance through a backward pointer-walking loop");
    bool liveRejected=false;try{static_cast<void>(compileDspCode("streamout out;mov eax,0;next:cmp eax,0;jz read;mov edx,0;jmp next;read:movaps xmm0,[rdx];movaps out,xmm0;",DspLanguage::Assembler));}catch(const DspCompileError& e){liveRejected=std::string_view(e.what()).find("backward branch")!=std::string_view::npos;}
    check(liveRejected,"Deferred loop typing still rejects a live register whose reference kind changes");
    for(const auto body:{"movd xmm0,rax;", "mov [table],rax;", "call rax;"}){bool rejected=false;try{static_cast<void>(compileDspCode(std::string("__const table;streamout out;mov rax,table;mov rax,[rax];")+body,DspLanguage::Assembler));}catch(const DspCompileError&){rejected=true;}check(rejected,"Opaque loaded pointers cannot be emitted, stored as integers or called as native code");}
}

void polyRebinding(DspBackend backend)
{
    Graph graph(backend);const auto b=graph.addDspCode(compileDspCode("__const table;streamout out;mov rax,table;mov rax,[rax];movaps xmm0,[rax];movaps out,xmm0;",DspLanguage::Assembler));
    check(graph.bindDspEnginePointerTable(b.id,"table",cell(Memory::fromSamples(std::array{0.25F,0.25F,0.25F,0.25F}))),"Bind Poly table");PolyRuntime runtime(graph,{},b.outputs,{.maxVoices=5,.holdAfterNoteOff=true});
    const std::array events{PolyEvent{0,PolyEventKind::NoteOn,1},PolyEvent{0,PolyEventKind::NoteOn,2}};std::array<float,2> out{};check(runtime.process({},std::array{OutputBuffer{out}},events) && out==std::array{0.5F,0.5F},"Packed Poly voices read bound table targets");
    check(runtime.setDspEnginePointerTable(b.id,"table",cell(Memory::fromSamples(std::array{0.75F,0.75F,0.75F,0.75F}))) && runtime.process({},std::array{OutputBuffer{out}}) && out==std::array{1.5F,1.5F},"Poly forwarding updates held voices on the next buffer");
}
}
int main(){try{for(auto backend:{DspBackend::Interpreter,DspBackend::LlvmOrc})if(dspBackendAvailable(backend)){openstone::tests::withReferenceSources("originalSampleIndex", [&] { originalSampleIndex(backend); });openstone::tests::withReferenceSources("originalBufferTable", [&] { originalBufferTable(backend); });indexedNestedAndBounds(backend);childWrites(backend);aliasesInvalidationAndOpacity(backend);precisionLifetimeAndContracts(backend);polyRebinding(backend);}}catch(const std::exception& e){allocations::enabled=false;std::cerr<<e.what()<<'\n';return 1;}}
