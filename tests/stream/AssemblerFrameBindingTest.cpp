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
using Words=std::array<std::uint32_t,4>;
constexpr std::size_t frames=137;
void check(bool value,std::string_view message){if(!value)throw std::runtime_error(std::string(message));}
std::string fixture(bool doubles)
{
    std::ifstream file(std::filesystem::path(OPENSTONE_TEST_FIXTURES)/"flowstone"/(doubles ? "engine-frame-load-double.asm" : "engine-frame-load-float.asm"),std::ios::binary);
    check(file.good(),"Read unchanged native frame-load fixture");return {std::istreambuf_iterator<char>(file),{}};
}
std::uint64_t read(std::span<const std::uint8_t> data,std::size_t offset,unsigned bytes)
{
    if(offset>data.size() || data.size()-offset<bytes)return 0;
    std::uint64_t result=0;for(unsigned i=0;i<bytes;++i)result|=std::uint64_t{data[offset+i]}<<(8*i);return result;
}
void put(std::vector<std::uint8_t>& bytes,std::size_t offset,std::uint64_t value,unsigned size)
{
    for(unsigned i=0;i<size;++i)bytes[offset+i]=static_cast<std::uint8_t>(value>>(8*i));
}

void originalFrameLoads(DspBackend backend)
{
    for(bool doubles:{false,true}) {
        const auto body=fixture(doubles);
        // RBP/RDX are supplied by the runtime. RAX remains an explicit caller byte cursor.
        const auto program=compileDspCode(std::string("int cursor=0;mov eax,cursor;")+
            (doubles ? "streamdoubleout first,second,third;" : "streamout first,second,third;")+body+
            (doubles ? "movapd first[0],xmm0;movapd first[2],xmm1;movapd second[0],xmm2;movapd second[2],xmm3;movapd third[0],xmm4;movapd third[2],xmm5;" : "movaps first,xmm0;movaps second,xmm2;movaps third,xmm4;")+
            "add eax,"+(doubles ? "8" : "4")+";cmp eax,64;jl keep;mov eax,0;keep:mov cursor,eax;",DspLanguage::Assembler);
        Graph graph(backend);const auto block=graph.addDspCode(program);
        std::vector<std::uint8_t> image(detail::nativeFramePrefixBytes);
        constexpr std::array<std::uint32_t,8> f{0x3f812345,0xbfabcdef,0,0x80000000,0x7fc12345,0xffc54321,1,0x7f800000};
        constexpr std::array<std::uint64_t,8> d{0x3ff0000000001000ULL,0xbffabcde12345678ULL,0,0x8000000000000000ULL,0x7ff8123456789abcULL,0xfff8abcde1234567ULL,1,0x7ff0000000000000ULL};
        for(unsigned i=0;i<24;++i)put(image,i*(doubles?8:4),doubles ? d[i%8] : f[i%8],doubles?8:4);
        const auto test=[&]<typename Sample>(detail::Runtime<Sample> runtime) {
            check(runtime.setDspNativeFrame(block.id,image),"Copy native caller frame without a fake pointer prologue");
            auto expected=image;image[0]^=255; // Binding owns a copy.
            for(const std::size_t size:{1,3,7,31,64,257}) {
                runtime.reset();std::array<std::array<Sample,frames>,3> output{};
                for(std::size_t offset=0;offset<frames;offset+=size) {
                    const auto count=std::min(size,frames-offset);std::array<std::span<Sample>,3> out;
                    for(unsigned p=0;p<3;++p)out[p]=std::span{output[p]}.subspan(offset,count);
                    const int rounding=std::fegetround(),flags=std::fetestexcept(FE_ALL_EXCEPT);
                    check(runtime.process({},out),"Render unchanged Float/Double frame loads");
                    check(std::fegetround()==rounding && std::fetestexcept(FE_ALL_EXCEPT)==flags,"Frame access preserves the caller floating-point state");
                }
                for(unsigned n=0;n<frames;++n)for(unsigned lane=0;lane<4;++lane)for(unsigned p=0;p<3;++p) {
                    const auto at=(n*(doubles?8:4))%64+lane*(doubles?8:4);
                    if constexpr(std::is_same_v<Sample,Mono4>)check(std::bit_cast<std::uint32_t>(output[p][n][lane])==read(expected,at,4),"Native Float frame copies preserve all raw DWORDs");
                    else check(std::bit_cast<std::uint64_t>(output[p][n][lane])==read(expected,at,8),"Native Double frame copies preserve both raw physical windows");
                }
            }
        };
        if(doubles)test(graph.compileMonoDouble4(block.outputs));else test(graph.compileMono4(block.outputs));
    }
}

void memoryFormsAndStages(DspBackend backend)
{
    const auto program=compileDspCode(R"(
        streamout words,bytes,stageValue,nonnull,tail;
        int index=0;
        stage 0;mov [rbp+64],1;
        stage 1;inc [rdx+64];
        stage 2;
        push rdx;push rbp;mov rbx,rdx;mov ecx,4;add rbx,ecx;
        mov [rbx],0x12345678;mov eax,0xab;mov [rbp+5],al;mov ebx,0;mov bl,[rdx+5];
        movd xmm0,ebx;movaps bytes,xmm0;
        mov eax,[rbp+4];movd xmm0,eax;movaps words,xmm0;
        mov eax,[rdx+64];movd xmm0,eax;cvtdq2ps xmm0,xmm0;movaps stageValue,xmm0;
        cmp rbp,0;jz failed;movaps xmm0,_ONE;movaps nonnull,xmm0;jmp end;
        failed:movaps xmm0,_ZERO;movaps nonnull,xmm0;end:
        mov eax,[rbp+0x110e];movd xmm0,eax;movaps tail,xmm0;
        pop rbp;pop rdx;
        stage 3;inc [rbp+64];
    )",DspLanguage::Assembler);
    Graph graph(backend);const auto block=graph.addDspCode(program),second=graph.addDspCode(program);
    const std::array taps{block.outputs[0],block.outputs[1],block.outputs[2],block.outputs[3],block.outputs[4],second.outputs[2]};
    auto runtime=graph.compileMono4(taps);
    for(const std::size_t size:{1,3,7,31,64}) {
        runtime.reset();std::array<std::array<Mono4,frames>,6> output{};
        for(std::size_t offset=0;offset<frames;offset+=size) {
            const auto count=std::min(size,frames-offset);std::array<Mono4OutputBuffer,6> out;
            for(unsigned p=0;p<6;++p)out[p]=std::span{output[p]}.subspan(offset,count);
            check(runtime.process({},out),"Run native frame aliases, scalar/byte stores, pointer saves and stages");
        }
        for(unsigned n=0;n<frames;++n) {
            check(std::bit_cast<Words>(output[0][n])==Words{0x1234ab78,0,0,0},"Frame aliases see byte stores without changing neighbors");
            check(std::bit_cast<Words>(output[1][n])==Words{0xab,0,0,0},"Frame byte loads zero-extend");
            check(output[2][n]==Mono4{float(2+2*n),0,0,0} && output[5][n]==output[2][n],"Owned frame state persists across stages/buffers, resets, and stays independent per block");
            check(output[3][n]==Mono4{1,1,1,1},"Incoming frame references are non-null opaque references");
            check(output[4][n]==Mono4{},"Frame accesses crossing the bound return zero");
        }
    }
    for(const auto body:{
        "mov eax,target;mov [rbp+16],eax;mov eax,1;call [rdx+16];jmp done;target:mov eax,7;ret;done:",
        "mov eax,target;mov [rbp+16],eax;mov eax,1;call run;jmp done;run:jmp [rdx+16];target:mov eax,7;ret;done:",
    }) {
        const auto p=compileDspCode("streamout out;"+std::string(body)+"movd xmm0,eax;cvtdq2ps xmm0,xmm0;movaps out,xmm0;",DspLanguage::Assembler);
        Graph g(backend);const auto b=g.addDspCode(p);auto rt=g.compile(b.outputs);std::array<float,3> out{};
        check(rt.process({},std::array{OutputBuffer{out}}) && out==std::array{7.F,7.F,7.F},"Frame call targets preserve provenance through calls and tail jumps");
    }
}

void offsetsAndDefaults(DspBackend backend)
{
    constexpr std::array<std::string_view,8> names{"_CHANNEL_GATE_OFFSET","_CHANNEL_TRIGGER_OFFSET","_SAMPLE_POS_OFFSET","_FRAME_POS_OFFSET","_CHANNEL_MASK_OFFSET","_IS_POLY_PROCESS","_CHANNEL_CHANNEL_ID_OFFSET","_CHANNEL_NOTE_ID_OFFSET"};
    constexpr std::array<std::uint32_t,8> offsets{0x1010,0x1020,0x1030,0x1040,0x1050,0x1080,0x10c0,0x10e0};
    std::string code="streamout out;";
    for(unsigned i=0;i<names.size();++i)code+="mov eax,"+std::string(names[i])+";mov [rdx+"+std::to_string(i*4)+"],eax;";
    code+="movaps xmm0,[rbp];movaps out,xmm0;";
    const auto p=compileDspCode(code,DspLanguage::Assembler);Graph g(backend);const auto b=g.addDspCode(p);auto rt=g.compileMono4(b.outputs);
    std::vector<std::uint8_t> aliasImage(detail::nativeFramePrefixBytes);
    for(unsigned i=0;i<offsets.size();++i)put(aliasImage,offsets[i],0x12340000U+i,4);
    check(rt.setDspNativeFrame(b.id,aliasImage),"Bind raw engine alias packets");
    std::array<Mono4,1> raw{};check(rt.process({},std::array{Mono4OutputBuffer{raw}}) && std::bit_cast<Words>(raw[0])==Words{0,0x12340001,0x12340002,0},"OFFSET names read native frame memory rather than literal byte offsets");
    const auto p2=compileDspCode(R"(
        streamboolout active,gate,poly;streamout position,zero;
        movaps xmm0,_CHANNEL_MASK_OFFSET;movaps active,xmm0;
        movaps xmm0,_CHANNEL_GATE_OFFSET;movaps gate,xmm0;
        movaps xmm0,_IS_POLY_PROCESS;movaps poly,xmm0;
        movaps xmm0,_FRAME_POS_OFFSET;movaps position,xmm0;
        movaps xmm0,_SAMPLE_POS_OFFSET;movaps zero,xmm0;
    )",DspLanguage::Assembler);
    Graph g2(backend);const auto b2=g2.addDspCode(p2);auto mono=g2.compile(b2.outputs);auto packed=g2.compileMono4(b2.outputs);
    std::array<std::array<float,3>,5> scalar{};std::array<OutputBuffer,5> sout;for(unsigned i=0;i<5;++i)sout[i]=scalar[i];
    check(mono.process({},sout),"Read native Mono header");
    std::array<std::array<Mono4,3>,5> quad{};std::array<Mono4OutputBuffer,5> qout;for(unsigned i=0;i<5;++i)qout[i]=quad[i];
    check(packed.process({},qout),"Read packed Mono header");
    for(unsigned n=0;n<3;++n) {
        check(scalar[0][n]==1 && scalar[1][n]==0 && scalar[2][n]==0 && scalar[3][n]==n && scalar[4][n]==0,"Mono header has active mask, no Poly gate, low Float frame position and initialized zero sample origin");
        check(quad[0][n]==Mono4{1,1,1,1} && quad[1][n]==Mono4{} && quad[2][n]==Mono4{} && quad[3][n]==Mono4{float(n),0,0,0},"Packed Mono header reflects all valid lanes and the native low-word frame-position layout");
    }
    std::vector<std::uint8_t> small(16),large(detail::nativeFramePrefixBytes+64);
    check(!rt.setDspNativeFrame(b.id,small) && !rt.setDspNativeFrame(static_cast<DspBlockId>(999),large),"Frame import rejects incomplete prefixes and unknown nodes");
    for(const auto source:{"mov eax,rbp;movd xmm0,eax;","movd xmm0,edx;","mov eax,0;mov rbp,eax;mov eax,[rbp];","mov eax,[rbp+rax];"}) {
        bool rejected=false;try{static_cast<void>(compileDspCode("streamout out;"+std::string(source),DspLanguage::Assembler));}catch(const DspCompileError&){rejected=true;}
        check(rejected,"Native frame binding preserves pointer opacity, stale-pointer diagnostics and missing caller-register errors");
    }
}

void doubleFrameStores(DspBackend backend)
{
    const auto p=compileDspCode(R"(
        streamdoublein value;streamdoubleout out;
        movapd xmm0,value[0];movapd [rbp],xmm0;
        movapd xmm1,value[2];movapd [rdx+16],xmm1;
        movapd xmm2,[rdx];movapd out[0],xmm2;
        movapd xmm3,[rbp+16];movapd out[2],xmm3;
    )",DspLanguage::Assembler);
    Graph graph(backend);const auto block=graph.addDspCode(p);graph.connectDspInput(graph.addInput(),block.id,0);
    auto runtime=graph.compileMonoDouble4(block.outputs);
    constexpr std::array<std::uint64_t,8> bits{0x3ff0000000001000ULL,0xbff0000000001000ULL,0x8000000000000000ULL,0,0x7ff8123456789abcULL,0xfff8123456789abcULL,1,0x7ff0000000000000ULL};
    std::array<MonoDouble4,frames> input{},output{};
    for(unsigned n=0;n<frames;++n)for(unsigned lane=0;lane<4;++lane)input[n][lane]=std::bit_cast<double>(bits[(n+lane)%bits.size()]);
    for(const unsigned size:{1,3,7,31,64,257}) {
        runtime.reset();
        for(unsigned offset=0;offset<frames;offset+=size) {
            const auto count=std::min<std::size_t>(size,frames-offset);
            check(runtime.process(std::array{MonoDouble4InputBuffer{std::span{input}.subspan(offset,count)}},std::array{MonoDouble4OutputBuffer{std::span{output}.subspan(offset,count)}}),"Render both physical Double frame-store windows");
        }
        for(unsigned n=0;n<frames;++n)check(std::bit_cast<std::array<std::uint64_t,4>>(input[n])==std::bit_cast<std::array<std::uint64_t,4>>(output[n]),"Double frame stores preserve precision, signed zero, subnormals and NaN payloads bit for bit");
    }
}

void recoveredMaskReads(DspBackend backend)
{
    for(bool x64:{false,true}) {
        const auto name=x64 ? "engine-frame-mask-read-x64.asm" : "engine-frame-mask-read-x86.asm";
        std::ifstream file(std::filesystem::path(OPENSTONE_TEST_FIXTURES)/"flowstone"/name,std::ios::binary);
        check(file.good(),"Read exact recovered Poly-reducer frame fragment");
        const std::string body{std::istreambuf_iterator<char>(file),{}};
        const auto p=compileDspCode("memrefin other;streamin inPoly;streamout out;mov "+std::string(x64 ? "rdx" : "edx")+",other[0];"+body+"movaps out,"+(x64 ? "xmm1" : "xmm0")+";",DspLanguage::Assembler);
        Graph graph(backend);const auto b=graph.addDspCode(p);graph.connectDspInput(graph.addInput(),b.id,1);auto runtime=graph.compileMono4(b.outputs);
        constexpr Words masks{0xffffffff,0,0x7fffffff,0x80000000};
        std::vector<std::uint8_t> image(detail::nativeFramePrefixBytes);for(unsigned i=0;i<4;++i)put(image,0x1050+4*i,masks[i],4);
        std::array<Mono4,frames> input{},output{};
        constexpr Words values{0xbf812345,0x7fc12345,0xffabcdef,0x80000000};
        for(unsigned n=0;n<frames;++n)for(unsigned lane=0;lane<4;++lane)input[n][lane]=std::bit_cast<float>(values[(n+lane)%4]);
        for(const unsigned size:{1,3,7,31,64,257}) {
            runtime.reset();check(runtime.setDspMemRefInput(b.id,0,openstone::green::Memory(image)),"Bind caller frame for original reducer fragment");
            for(unsigned offset=0;offset<frames;offset+=size) {
                const auto count=std::min<std::size_t>(size,frames-offset);
                check(runtime.process(std::array{Mono4InputBuffer{std::span{input}.subspan(offset,count)}},std::array{Mono4OutputBuffer{std::span{output}.subspan(offset,count)}}),"Render original x86/x64 frame alias and masking instructions");
            }
            for(unsigned n=0;n<frames;++n)for(unsigned lane=0;lane<4;++lane)check(std::bit_cast<Words>(output[n])[lane]==(std::bit_cast<Words>(input[n])[lane]&masks[lane]),"Recovered reducer frame aliases select exact raw mask bits");
        }
    }
}

void aliasRebindingAndTargets(DspBackend backend)
{
    std::vector<std::uint8_t> image(detail::nativeFramePrefixBytes+64);
    constexpr Words words{0x12345678,0xabcdef01,0x87654321,0x11223344};
    for(unsigned i=0;i<4;++i)put(image,0x1050+i*4,words[i],4);
    const auto p=compileDspCode(R"(
        memrefin other;streamout whole,lane;
        mov rdx,other[0];xchg rbp,rdx;
        movaps xmm0,_CHANNEL_MASK_OFFSET;movaps whole,xmm0;
        mov eax,_CHANNEL_MASK_OFFSET[1];movd xmm0,eax;movaps lane,xmm0;
        xchg rbp,rdx;
    )",DspLanguage::Assembler);
    Graph graph(backend);const auto block=graph.addDspCode(p);auto runtime=graph.compileMono4(block.outputs);
    check(runtime.setDspMemRefInput(block.id,0,openstone::green::Memory(image)),"Bind explicit alternate native frame");
    std::array<Mono4,1> whole{},lane{};check(runtime.process({},std::array{Mono4OutputBuffer{whole},Mono4OutputBuffer{lane}}),"Retarget engine aliases by exchanging RBP/RDX");
    check(std::bit_cast<Words>(whole[0])==words && std::bit_cast<Words>(lane[0])==Words{words[1],0,0,0},"Engine aliases follow the current frame base and physical lane addressing");
    const auto imported=compileDspCode("streamout out;mov eax,[rbp+0x1120];movd xmm0,eax;movaps out,xmm0;",DspLanguage::Assembler);
    Graph ig(backend);const auto ib=ig.addDspCode(imported);auto ir=ig.compileMono4(ib.outputs);
    put(image,0x1120,0x31415926,4);check(ir.setDspNativeFrame(ib.id,image),"Import an extended native frame allocation");
    check(ir.process({},std::array{Mono4OutputBuffer{whole}}) && std::bit_cast<Words>(whole[0])==Words{0x31415926,0,0,0},"Extended imported frame ranges remain bounded and readable");
    const auto shadow=compileDspCode("streamout out;int _CHANNEL_MASK_OFFSET=7;mov eax,_CHANNEL_MASK_OFFSET;movd xmm0,eax;cvtdq2ps xmm0,xmm0;movaps out,xmm0;",DspLanguage::Assembler);
    Graph sg(backend);const auto sb=sg.addDspCode(shadow);auto sr=sg.compile(sb.outputs);std::array<float,1> out{};
    check(sr.process({},std::array{OutputBuffer{out}}) && out[0]==7,"An explicit source declaration retains precedence over a frame fallback alias");
    const auto skip=compileDspCode("streamout out;movaps xmm0,_ONE;jmp 7;movaps xmm0,_CHANNEL_MASK_OFFSET;movaps out,xmm0;",DspLanguage::Assembler);
    Graph kg(backend);const auto kb=kg.addDspCode(skip);auto kr=kg.compile(kb.outputs);
    check(kr.process({},std::array{OutputBuffer{out}}) && out[0]==1,"Numeric jump widths recognize engine-frame named memory operands");
    const auto invalidated=compileDspCode(R"(
        streamout out;movaps xmm0,_MONE;movaps out,xmm0;
        mov eax,leaf;mov ebx,8;mov [rbp+ebx*4],eax;
        mov ecx,0;mov [rdx+33],cl;call [rbp+ebx*4];
        movaps xmm0,_ONE;movaps out,xmm0;jmp done;leaf:ret;done:
    )",DspLanguage::Assembler);
    Graph tg(backend);const auto tb=tg.addDspCode(invalidated);auto tr=tg.compile(tb.outputs);
    check(tr.process({},std::array{OutputBuffer{out}}) && out[0]==-1,"Overlapping byte writes invalidate frame jump-table provenance");
}

void polyFrameSurvivesSlotReuse(DspBackend backend)
{
    const auto p=compileDspCode("polyout out;mov eax,[rbp];inc eax;mov [rdx],eax;movd xmm0,eax;cvtdq2ps xmm0,xmm0;shufps xmm0,xmm0,0;movaps out,xmm0;",DspLanguage::Assembler);
    Graph graph(backend);const auto block=graph.addDspCode(p);
    PolyRuntime runtime(graph,{},block.outputs,{.maxVoices=2,.holdAfterNoteOff=true});
    const std::array events{PolyEvent{0,PolyEventKind::NoteOn,1},PolyEvent{0,PolyEventKind::NoteOn,2},PolyEvent{3,PolyEventKind::NoteOff,1},PolyEvent{4,PolyEventKind::EndVoice,1},PolyEvent{5,PolyEventKind::NoteOn,3}};
    for(const unsigned size:{1,3,7,31,64}) {
        runtime.reset();std::array<float,frames> out{};
        for(unsigned offset=0;offset<frames;offset+=size) {
            const auto count=std::min<std::size_t>(size,frames-offset);std::vector<PolyEvent> local;
            for(auto event:events)if(event.frame>=offset && event.frame<offset+count){event.frame-=offset;local.push_back(event);}
            check(runtime.process({},std::array{OutputBuffer{std::span{out}.subspan(offset,count)}},local),"Run packet frame across owner-slot reuse");
        }
        for(unsigned n=0;n<frames;++n)check(out[n]==float((n+1)*(n==4 ? 1 : 2)),"Reinitializing a voice must not erase the frame shared by the still-running packet");
    }
}

void polyHeaders(DspBackend backend)
{
    const auto program=compileDspCode(R"(
        polyout active,gate,poly,slot,position;
        movaps xmm0,_CHANNEL_MASK_OFFSET;andps xmm0,_ONE;movaps active,xmm0;
        movaps xmm0,_CHANNEL_GATE_OFFSET;andps xmm0,_ONE;movaps gate,xmm0;
        movaps xmm0,_IS_POLY_PROCESS;andps xmm0,_ONE;movaps poly,xmm0;
        movaps xmm0,_CHANNEL_CHANNEL_ID_OFFSET;movaps slot,xmm0;
        movaps xmm0,_FRAME_POS_OFFSET;movaps position,xmm0;
    )",DspLanguage::Assembler);
    Graph graph(backend);const auto block=graph.addDspCode(program);
    PolyRuntime runtime(graph,{},block.outputs,{.maxVoices=5,.holdAfterNoteOff=true});
    const std::array events{PolyEvent{5,PolyEventKind::NoteOn,1},PolyEvent{8,PolyEventKind::NoteOn,2},PolyEvent{9,PolyEventKind::NoteOn,3},PolyEvent{10,PolyEventKind::NoteOn,4},PolyEvent{11,PolyEventKind::NoteOn,5},PolyEvent{12,PolyEventKind::NoteOff,1},PolyEvent{15,PolyEventKind::AllSoundOff},PolyEvent{80,PolyEventKind::NoteOn,6}};
    for(const unsigned size:{1,3,7,31,64,257}) {
        runtime.reset();std::array<std::array<float,frames>,5> output{};
        for(unsigned offset=0;offset<frames;offset+=size) {
            const auto count=std::min<std::size_t>(size,frames-offset);std::vector<PolyEvent> local;
            for(auto event:events)if(event.frame>=offset && event.frame<offset+count){event.frame-=offset;local.push_back(event);}
            std::array<OutputBuffer,5> out;for(unsigned p=0;p<5;++p)out[p]=std::span{output[p]}.subspan(offset,count);
            check(runtime.process({},out,local),"Run two native frame packets across Poly allocations, release, idle and reuse");
        }
        for(unsigned n=0;n<frames;++n) {
            const unsigned count=n<5 ? 0 : n<8 ? 1 : n<9 ? 2 : n<10 ? 3 : n<11 ? 4 : n<15 ? 5 : n<80 ? 0 : 1;
            check(output[0][n]==count && output[2][n]==count && output[1][n]==count-(n>=12&&n<15 ? 1U : 0U),"Poly active and held-gate masks distinguish release voices in both packets");
            check(output[3][n]==count*(count-1)/2,"Native channel-ID packet contains Float voice-slot indices");
            check(output[4][n]==(count ? n*(count>4 ? 2 : 1) : 0),"Frame position uses root render time across idle periods and independent packets");
        }
    }
}
}
int main()
{
    try {for(auto backend:{DspBackend::Interpreter,DspBackend::LlvmOrc})if(dspBackendAvailable(backend)){
        openstone::tests::withReferenceSources("originalFrameLoads", [&] { originalFrameLoads(backend); });memoryFormsAndStages(backend);offsetsAndDefaults(backend);polyHeaders(backend);polyFrameSurvivesSlotReuse(backend);aliasRebindingAndTargets(backend);doubleFrameStores(backend);openstone::tests::withReferenceSources("recoveredMaskReads", [&] { recoveredMaskReads(backend); });
    }}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
