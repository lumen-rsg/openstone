#include "../ReferenceFixtures.hpp"
#include "stream/Runtime.hpp"
#include "stream/PolyRuntime.hpp"

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
#include <vector>

namespace {
using namespace openstone::stream;
using openstone::green::Memory;
using Words=std::array<std::uint32_t,4>;
using DoubleWords=std::array<std::uint64_t,4>;
constexpr std::size_t frames=137;
void check(bool ok,std::string_view message){if(!ok)throw std::runtime_error(std::string(message));}
std::string fixture(std::string_view name)
{
    std::ifstream file(std::filesystem::path(OPENSTONE_TEST_FIXTURES)/"flowstone"/name,std::ios::binary);
    check(file.good(),"Read exact native __const kernel");return {std::istreambuf_iterator<char>(file),{}};
}
Memory doubleMemory(const std::array<double,4>& values)
{
    std::vector<std::uint8_t> bytes(32);
    for(unsigned lane=0;lane<4;++lane){const auto word=std::bit_cast<std::uint64_t>(values[lane]);for(unsigned i=0;i<8;++i)bytes[lane*8+i]=static_cast<std::uint8_t>(word>>(8*i));}
    return Memory(std::move(bytes));
}
std::uint64_t word64(const Memory& m,unsigned lane)
{
    return m.wordAt(lane*8) | (std::uint64_t{m.wordAt(lane*8+4)}<<32);
}

void unchangedDoubleCounter(DspBackend backend)
{
    for(const auto name : {"engine-bound-double-counter.asm","engine-bound-double-counter-x86.asm"}) {
    const auto source=fixture(name);
    const auto program=compileDspCode(source,DspLanguage::Assembler);
    check(program.inputs().empty() && program.enginePointers().size()==1 && program.enginePointers()[0]=="p_counter","Engine names are a separate caller contract, not added signal inputs");
    Graph graph(backend);const auto block=graph.addDspCode(program);
    constexpr std::array<double,4> initial{1.0+0x1p-40,-2.0+0x1p-39,10.0+0x1p-35,0.125+0x1p-40};
    auto memory=doubleMemory(initial);
    check(graph.bindDspEnginePointer(block.id,"p_counter",memory),"Bind caller-owned packed Double counter");
    auto runtime=graph.compileMonoDouble4(block.outputs);
    for(const std::size_t size:{1,3,7,31,64,257}) {
        runtime.reset();memory=doubleMemory(initial);check(runtime.setDspEnginePointer(block.id,"p_counter",memory),"Rebind counter between render calls");
        std::array<MonoDouble4,frames> output{};
        for(std::size_t offset=0;offset<frames;offset+=size) {
            const auto count=std::min(size,frames-offset);
            const auto round=std::fegetround(),flags=std::fetestexcept(FE_ALL_EXCEPT);
            check(runtime.process({},std::array{MonoDouble4OutputBuffer{std::span{output}.subspan(offset,count)}}),"Run unchanged native Double counter");
            check(std::fegetround()==round && std::fetestexcept(FE_ALL_EXCEPT)==flags,"Engine pointer access preserves caller floating-point state");
        }
        for(unsigned n=0;n<frames;++n)for(unsigned lane=0;lane<4;++lane)
            check(std::bit_cast<std::uint64_t>(output[n][lane])==std::bit_cast<std::uint64_t>(initial[lane]+n),"Both Double windows retain extra precision sample for sample across buffers");
        for(unsigned lane=0;lane<4;++lane)check(word64(memory,lane)==std::bit_cast<std::uint64_t>(initial[lane]+frames),"__const fixes the address while pointed counter data remains writable");
        runtime.reset();std::array<MonoDouble4,1> next{};
        check(runtime.process({},std::array{MonoDouble4OutputBuffer{next}}),"Reset does not reset caller-owned engine data");
        for(unsigned lane=0;lane<4;++lane)check(next[0][lane]==initial[lane]+frames,"External counters survive DSP-state reset");
    }
    }
}

void unchangedValueReads(DspBackend backend)
{
    for(unsigned mode=0;mode<3;++mode) {
        const bool doubles=mode!=0;
        const auto source=fixture(mode==2 ? "engine-bound-double-read-x86.asm" : doubles ? "engine-bound-double-read.asm" : "engine-bound-float-read.asm");
        // The original x86 fragment relies on its caller's outlet declaration.
        const auto program=compileDspCode((doubles ? "" : "streamout out0;")+source,DspLanguage::Assembler);
        Graph graph(backend);const auto block=graph.addDspCode(program);
        const auto initial=doubleMemory({1.0+0x1p-40,0,0,0});
        const auto f=Memory::fromSamples(std::array{-0.125F});
        check(graph.bindDspEnginePointer(block.id,"p_address",doubles ? initial : f,false),"Bind read-only live Green value storage");
        for(const std::size_t size:{1,3,7,31,64}) {
            if(doubles) {
                auto runtime=graph.compileMonoDouble4(block.outputs);std::array<MonoDouble4,frames> output{};
                for(unsigned offset=0;offset<frames;offset+=size) {
                    const auto count=std::min<std::size_t>(size,frames-offset);
                    check(runtime.process({},std::array{MonoDouble4OutputBuffer{std::span{output}.subspan(offset,count)}}),"Read unchanged native dfld/dfst value kernel");
                }
                for(const auto& packet:output)for(const auto value:packet)check(value==1.0+0x1p-40,"Double engine-address reads preserve the low precision bits");
                const auto replacement=doubleMemory({-3.0+0x1p-39,0,0,0});
                check(runtime.setDspEnginePointer(block.id,"p_address",replacement,false),"Change the caller binding without recompiling");
                std::array<MonoDouble4,1> next{};check(runtime.process({},std::array{MonoDouble4OutputBuffer{next}}),"Render rebound native pointer");
                for(auto value:next[0])check(value==-3.0+0x1p-39,"Every lane sees the new engine referent on the next buffer");
            } else {
                auto runtime=graph.compileMono4(block.outputs);std::array<Mono4,frames> output{};
                for(unsigned offset=0;offset<frames;offset+=size) {
                    const auto count=std::min<std::size_t>(size,frames-offset);
                    check(runtime.process({},std::array{Mono4OutputBuffer{std::span{output}.subspan(offset,count)}}),"Read unchanged native direct absolute Float address");
                }
                for(const auto& packet:output)check(packet==Mono4{-0.125F,-0.125F,-0.125F,-0.125F},"The native absolute-pointer form reads the same Float into all lanes");
                f.writeSample(0,0.75F);std::array<Mono4,1> next{};check(runtime.process({},std::array{Mono4OutputBuffer{next}}) && next[0]==Mono4{0.75F,0.75F,0.75F,0.75F},"Live caller writes are read without copying values into the program");
                f.writeSample(0,-0.125F);
            }
        }
    }
}

void unchangedDoublePair(DspBackend backend)
{
    for(const auto name:{"engine-bound-double-pair-x86.asm","engine-bound-double-pair-x64.asm"}) {
        const auto program=compileDspCode(fixture(name),DspLanguage::Assembler);
        check(program.enginePointers().size()==2 && program.enginePointers()[0]=="p_addressNum" && program.enginePointers()[1]=="p_addressDen","Multiple relocations retain their named owner contracts");
        Graph graph(backend);const auto b=graph.addDspCode(program);
        check(graph.bindDspEnginePointer(b.id,"p_addressNum",doubleMemory({1.0+0x1p-40,0,0,0}),false) &&
              graph.bindDspEnginePointer(b.id,"p_addressDen",doubleMemory({3.0+0x1p-39,0,0,0}),false),"Bind independent native numerator and denominator values");
        auto runtime=graph.compileMonoDouble4(b.outputs);
        for(const std::size_t size:{1,3,7,31,64,257}) {
            runtime.reset();std::array<MonoDouble4,frames> num{},den{};
            for(unsigned offset=0;offset<frames;offset+=size) {
                const auto count=std::min<std::size_t>(size,frames-offset);
                check(runtime.process({},std::array{MonoDouble4OutputBuffer{std::span{num}.subspan(offset,count)},MonoDouble4OutputBuffer{std::span{den}.subspan(offset,count)}}),"Run unchanged two-pointer native reader");
            }
            for(unsigned n=0;n<frames;++n)for(unsigned lane=0;lane<4;++lane)
                check(num[n][lane]==1.0+0x1p-40 && den[n][lane]==3.0+0x1p-39,"Both named engine resources preserve exact Double values sample for sample");
        }
        check(runtime.setDspEnginePointer(b.id,"p_addressNum",doubleMemory({-7.0+0x1p-38,0,0,0}),false),"Rebind just one resource");
        std::array<MonoDouble4,1> num{},den{};
        check(runtime.process({},std::array{MonoDouble4OutputBuffer{num},MonoDouble4OutputBuffer{den}}),"Read independently rebound resources");
        for(unsigned lane=0;lane<4;++lane)check(num[0][lane]==-7.0+0x1p-38 && den[0][lane]==3.0+0x1p-39,"Rebinding one name leaves the other referent unchanged");
    }
}

void boundedViewsAndIsolation(DspBackend backend)
{
    const auto p=compileDspCode("__const data;streamin index;streamout out;mov eax,index;mov ebx,0x11223344;mov [data+eax],ebx;mov ecx,[data+eax];movd xmm0,ecx;movaps out,xmm0;",DspLanguage::Assembler);
    Graph graph(backend);const auto b=graph.addDspCode(p);graph.connectDspInput(graph.addInput(),b.id,0);
    Memory memory(std::vector<std::uint8_t>(24,0xa5));
    check(graph.bindDspEnginePointer(b.id,"data",memory.view(5,8,1)),"Bind an unaligned bounded view");
    auto runtime=graph.compileMono4(b.outputs);
    constexpr std::array<std::int32_t,8> offsets{-1,0,1,4,5,7,8,0x7fffffff};
    std::array<Mono4,offsets.size()> input{},out{};
    for(unsigned n=0;n<offsets.size();++n)input[n]=std::bit_cast<Mono4>(Words{static_cast<std::uint32_t>(offsets[n]),0,0,0});
    check(runtime.process(std::array{Mono4InputBuffer{input}},std::array{Mono4OutputBuffer{out}}),"Read and write view boundaries");
    for(unsigned n=0;n<offsets.size();++n)check(std::bit_cast<Words>(out[n])==Words{offsets[n]>=0 && offsets[n]<=4 ? 0x11223344U : 0U,0,0,0},"Invalid engine accesses are zero reads and ignored whole writes");
    for(unsigned n=0;n<24;++n)if(n<5 || n>=13)check(memory.byte(n)==0xa5,"Engine view cannot overwrite neighboring owner bytes");

    const auto read=compileDspCode("__const sameName;streamout out;fld [sameName];fstp out[0];",DspLanguage::Assembler);
    const auto make=[&](float value) {
        Graph local(backend);const auto block=local.addDspCode(read);
        check(local.bindDspEnginePointer(block.id,"sameName",Memory::fromSamples(std::array{value}),false),"Retain caller data ownership");
        return local.compile(block.outputs);
    };
    auto first=make(0.25F),second=make(-0.5F);std::array<float,3> a{},c{};
    check(first.process({},std::array{OutputBuffer{a}}) && second.process({},std::array{OutputBuffer{c}}) &&
          a==std::array{0.25F,0.25F,0.25F} && c==std::array{-0.5F,-0.5F,-0.5F},"Same compiled name has instance-local binding and retained lifetime after its Graph/caller is gone");
}

void targetAliases(DspBackend backend)
{
    for(bool engineTable:{false,true}) {
        const std::string source="__const bound;streamin touch;memrefin data;streamout out;float zero=0,one=1;"
            "movaps xmm0,zero;movaps out,xmm0;mov eax,"+std::string(engineTable ? "bound;" : "data[0];")+
            "mov edx,0;mov ebx,routine;mov [eax+edx*4],ebx;mov ebx,"+(engineTable ? "data[0];" : "bound;")+
            "mov ecx,touch;mov edx,0;mov [ebx+ecx],dl;mov edx,0;call [eax+edx*4];jmp end;routine:movaps xmm0,one;movaps out,xmm0;ret;end:";
        Graph graph(backend);const auto b=graph.addDspCode(compileDspCode(source,DspLanguage::Assembler));graph.connectDspInput(graph.addInput(),b.id,0);
        const Memory memory(std::vector<std::uint8_t>(20));
        check(graph.bindDspEnginePointer(b.id,"bound",engineTable ? memory : memory.view(1,19,1)),"Bind shared engine and Mem storage");
        auto runtime=graph.compile(b.outputs);
        check(runtime.setDspMemRefInput(b.id,1,engineTable ? memory.view(1,19,1) : memory),"Bind an overlapping Mem alias");
        constexpr std::array<std::int32_t,9> offsets{-1,0,1,2,3,4,18,19,20};std::array<float,offsets.size()> input{},out{};
        for(unsigned n=0;n<offsets.size();++n)input[n]=std::bit_cast<float>(static_cast<std::uint32_t>(offsets[n]));
        check(runtime.process(std::array{InputBuffer{input}},std::array{OutputBuffer{out}}),"Render callable engine/Mem table aliases");
        for(unsigned n=0;n<offsets.size();++n)check(out[n]==(offsets[n]>=0 && offsets[n]<3 ? 0.F : 1.F),"Overlapping byte writes invalidate local target provenance across engine and Mem views in either direction");
    }
}

void polyBindings(DspBackend backend)
{
    Graph graph(backend);const auto b=graph.addDspCode(compileDspCode("__const value;streamout out;fld [value];fst out[0];fst out[1];fst out[2];fstp out[3];",DspLanguage::Assembler));
    check(graph.bindDspEnginePointer(b.id,"value",Memory::fromSamples(std::array{0.25F}),false),"Bind resource for Poly instances");
    PolyRuntime runtime(graph,{},b.outputs,{.maxVoices=5,.holdAfterNoteOff=true});
    std::array<float,2> out{};const std::array notes{PolyEvent{0,PolyEventKind::NoteOn,1},PolyEvent{0,PolyEventKind::NoteOn,2}};
    check(runtime.process({},std::array{OutputBuffer{out}},notes) && out==std::array{0.5F,0.5F},"Packed Poly voices read caller bindings");
    check(runtime.setDspEnginePointer(b.id,"value",Memory::fromSamples(std::array{0.75F}),false),"Forward engine rebind to all Poly instances");
    check(runtime.process({},std::array{OutputBuffer{out}}) && out==std::array{1.5F,1.5F},"Held voices see new caller binding on the next buffer");
}

void memoryFormsAndNull(DspBackend backend)
{
    const auto p=compileDspCode(R"(
        __const data,flag;streamin index,value;streamout out,byte,live;
        mov eax,index;mov ebx,value;
        mov [data+eax*4],ebx;mov ecx,[4*eax+data];movd xmm0,ecx;movaps out,xmm0;
        mov edx,0;mov dl,[data+eax*4];movd xmm0,edx;movaps byte,xmm0;
        mov esi,flag;push esi;mov esi,0;pop esi;cmp esi,0;jz null;
        movaps xmm0,_ONE;movaps live,xmm0;jmp done;
        null:movaps xmm0,_ZERO;movaps live,xmm0;done:
    )",DspLanguage::Assembler);
    Graph graph(backend);const auto b=graph.addDspCode(p);const auto ix=graph.addInput(),v=graph.addInput();graph.connectDspInput(ix,b.id,0);graph.connectDspInput(v,b.id,1);
    auto memory=Memory(std::vector<std::uint8_t>(32));
    check(graph.bindDspEnginePointer(b.id,"data",memory) && graph.bindDspEnginePointer(b.id,"flag",{}),"Bind data and explicitly absent engine pointers");
    auto runtime=graph.compileMono4(b.outputs);
    for(const unsigned size:{1,3,7,31,64,257}) {
        runtime.reset();check(runtime.setDspEnginePointer(b.id,"flag",{}),"Reset the caller null binding explicitly");memory=Memory(std::vector<std::uint8_t>(32));check(runtime.setDspEnginePointer(b.id,"data",memory),"Reset caller data explicitly");
        std::array<Mono4,frames> indices{},values{},out{},bytes{},live{};
        for(unsigned n=0;n<frames;++n){indices[n]=std::bit_cast<Mono4>(Words{n%8,0,0,0});values[n]=std::bit_cast<Mono4>(Words{0x12340000U+n,0,0,0});}
        for(unsigned offset=0;offset<frames;offset+=size) {
            const auto count=std::min<std::size_t>(size,frames-offset);
            check(runtime.process(std::array{Mono4InputBuffer{std::span{indices}.subspan(offset,count)},Mono4InputBuffer{std::span{values}.subspan(offset,count)}},std::array{Mono4OutputBuffer{std::span{out}.subspan(offset,count)},Mono4OutputBuffer{std::span{bytes}.subspan(offset,count)},Mono4OutputBuffer{std::span{live}.subspan(offset,count)}}),"Use direct, scaled and saved engine-pointer forms");
        }
        for(unsigned n=0;n<frames;++n) {
            check(std::bit_cast<Words>(out[n])==Words{0x12340000U+n,0,0,0} && std::bit_cast<Words>(bytes[n])==Words{n&255,0,0,0},"Raw DWORD and byte accesses use byte-scaled offsets without conversion");
            check(live[n]==Mono4{},"Explicit null pointer takes its native zero-comparison branch");
        }
        const auto flag=Memory({1,0,0,0});check(runtime.setDspEnginePointer(b.id,"flag",flag,false),"Rebind null to a present pointer");
        std::array<Mono4,1> zero{},a{},c{},d{};
        check(runtime.process(std::array{Mono4InputBuffer{zero},Mono4InputBuffer{zero}},std::array{Mono4OutputBuffer{a},Mono4OutputBuffer{c},Mono4OutputBuffer{d}}) && d[0]==Mono4{1,1,1,1},"Both backends test bound presence rather than the virtual byte cursor");
    }
    const auto readOnly=compileDspCode("__const data;streamout out;mov eax,0x12345678;mov [data],eax;mov eax,[data];movd xmm0,eax;movaps out,xmm0;",DspLanguage::Assembler);
    Graph rg(backend);const auto rb=rg.addDspCode(readOnly);const Memory original({1,2,3,4});check(rg.bindDspEnginePointer(rb.id,"data",original,false),"Bind a non-writable capability");auto rr=rg.compileMono4(rb.outputs);std::array<Mono4,1> out{};
    check(rr.process({},std::array{Mono4OutputBuffer{out}}) && std::bit_cast<Words>(out[0])==Words{0x04030201,0,0,0} && original.snapshotBytes()==std::vector<std::uint8_t>{1,2,3,4},"Read-only is an explicit host policy, not the meaning of __const");
}

void contractsAndStages(DspBackend backend)
{
    const auto p=compileDspCode("__const init;streamout out;float first=0;stage 0;mov eax,[init];mov first,eax;stage 2;movaps xmm0,first;movaps out,xmm0;",DspLanguage::Assembler);
    Graph graph(backend);const auto b=graph.addDspCode(p);
    bool missing=false;try{static_cast<void>(graph.compile(b.outputs));}catch(const std::invalid_argument& e){missing=std::string(e.what()).find("init")!=std::string::npos;}
    const auto unused=compileDspCode("__const unused;streamout out;movaps xmm0,_ONE;movaps out,xmm0;",DspLanguage::Assembler);
    check(!unused.requiresEnginePointer(0) && !unused.requiresEnginePointer(99),"Only relocated names require engine resources");
    Graph ug(backend);const auto ub=ug.addDspCode(unused);auto ur=ug.compile(ub.outputs);std::array<float,1> unusedOut{};
    check(ur.process({},std::array{OutputBuffer{unusedOut}}) && unusedOut[0]==1.F,"Unused declarations require no invented binding");
    for(bool wide:{false,true}) {
        const auto skip=compileDspCode(std::string("__const p;streamout out;movaps xmm0,_ONE;jmp ")+(wide ? "10;mov rax,p;" : "5;mov eax,p;")+"movaps out,xmm0;",DspLanguage::Assembler);
        Graph sg(backend);const auto sb=sg.addDspCode(skip);check(sg.bindDspEnginePointer(sb.id,"p",{}),"Bind skipped relocation explicitly");auto sr=sg.compile(sb.outputs);
        check(sr.process({},std::array{OutputBuffer{unusedOut}}) && unusedOut[0]==1.F,"Numeric skips use native 32/64-bit engine-immediate MOV widths");
    }
    check(missing,"An active block cannot compile with an invented zero engine address");
    Graph idle(backend);static_cast<void>(idle.addDspCode(p));const auto signal=idle.addInput();static_cast<void>(idle.compile(std::array{signal}));
    check(!graph.bindDspEnginePointer(b.id,"wrong",{}) && !graph.bindDspEnginePointer(static_cast<DspBlockId>(999),"init",{}),"Names and block IDs are checked");
    check(graph.bindDspEnginePointer(b.id,"init",Memory::fromSamples(std::array{0.75F})),"Supply initialization context before stage zero");auto runtime=graph.compile(b.outputs);std::array<float,2> out{};
    check(runtime.process({},std::array{OutputBuffer{out}}) && out==std::array{0.75F,0.75F},"Stage zero reads the caller value once");
    check(runtime.setDspEnginePointer(b.id,"init",Memory::fromSamples(std::array{0.25F})) && !runtime.setDspEnginePointer(b.id,"wrong",{}),"Rebinding validates names");
    check(runtime.process({},std::array{OutputBuffer{out}}) && out==std::array{0.75F,0.75F},"A pointer rebind does not rerun initialization");runtime.reset();check(runtime.process({},std::array{OutputBuffer{out}}) && out==std::array{0.25F,0.25F},"Reset reinitializes from the current binding");
    for(const auto source:{"__const p;__const p;streamout out;", "__const eax;streamout out;", "__const p=0;streamout out;", "__const p;streamout out;mov p,1;", "__const p;streamout out;movaps xmm0,p;", "__const p;streamout out;mov eax,p;movd xmm0,eax;", "__const p;streamout out;mov eax,[p+p];", "__const p;streamout out;mov eax,[p*4];", "__const p;streamout out;mov eax,p[1];"}) {
        bool rejected=false;try{static_cast<void>(compileDspCode(source,DspLanguage::Assembler));}catch(const DspCompileError&){rejected=true;}
        check(rejected,"Engine declarations reject assignments, duplicate/invalid names and forged address arithmetic");
    }
    check(std::ranges::any_of(dspLanguageWords(DspLanguage::Assembler),[](const auto& word){return word.text=="__const" && word.kind==DspWordKind::Type;}),"__const appears in Assembler highlighting and completion");
}
}
int main()
{
    try{for(auto backend:{DspBackend::Interpreter,DspBackend::LlvmOrc})if(dspBackendAvailable(backend)){
        openstone::tests::withReferenceSources("unchangedDoubleCounter", [&] { unchangedDoubleCounter(backend); });openstone::tests::withReferenceSources("unchangedValueReads", [&] { unchangedValueReads(backend); });openstone::tests::withReferenceSources("unchangedDoublePair", [&] { unchangedDoublePair(backend); });boundedViewsAndIsolation(backend);targetAliases(backend);polyBindings(backend);memoryFormsAndNull(backend);contractsAndStages(backend);
    }}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
