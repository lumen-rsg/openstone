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
#include <string_view>
#include <vector>

namespace {
using namespace openstone::stream;
using openstone::green::Memory;
using Words = std::array<std::uint32_t,4>;
constexpr std::size_t frames=137;
void check(bool ok,std::string_view message){if(!ok)throw std::runtime_error(std::string(message));}
Memory bytes(std::size_t n){return Memory(std::vector<std::uint8_t>(n));}
Memory integer(std::uint32_t n){auto m=bytes(4);m.writeWordAt(0,n);return m;}
std::string fixture(std::string_view name)
{
    std::ifstream file(std::filesystem::path(OPENSTONE_TEST_FIXTURES)/"flowstone"/name,std::ios::binary);
    check(file.good(),"Read unchanged native transfer fragment");return {std::istreambuf_iterator<char>(file),{}};
}
std::uint32_t sampleWord(unsigned channel,unsigned frame,unsigned lane)
{
    constexpr std::array words{0x00000000U,0x80000000U,0x00000001U,0x80000001U,0x3f800000U,0xbf000000U,0x7f800000U,0xff800000U,0x7fc12345U,0x7f812345U,0xffc54321U,0x40400000U};
    return words[(channel*5+frame*4+lane)%words.size()];
}

void nativeTransfer(DspBackend backend)
{
    for(bool x64:{false,true})for(const auto channels:std::array{std::array{0U,0U},std::array{1U,1U},std::array{3U,2U},std::array{2U,3U}}) {
        std::string source="streamin a,b,c;streamout x,y,z,count;";
        for(unsigned ch=0;ch<3;++ch)source+="movaps xmm0,"+std::string(1,"abc"[ch])+";movaps [rbp+"+std::to_string(256+ch*32)+"],xmm0;";
        source+=fixture(x64 ? "engine-buffer-transfer-x64.asm" : "engine-buffer-transfer-x86.asm");
        // The recovered body is a caller-owned fragment: finish its counter writeback,
        // restore the original four saves, and expose the frame packets it filled.
        source+="mov rax,p_counter;mov [rax],ebx;movd xmm0,ebx;cvtdq2ps xmm0,xmm0;movaps count,xmm0;pop rdi;pop rdx;pop rcx;pop rbx;";
        for(unsigned ch=0;ch<3;++ch)source+="movaps xmm0,[rbp+"+std::to_string(512+ch*32)+"];movaps "+std::string(1,"xyz"[ch])+",xmm0;";
        Graph graph(backend);const auto b=graph.addDspCode(compileDspCode(source,DspLanguage::Assembler));
        for(unsigned ch=0;ch<3;++ch)graph.connectDspInput(graph.addInput(),b.id,ch);
        const auto counter=integer(0);check(graph.bindDspEnginePointer(b.id,"p_counter",counter),"Bind packet cursor");
        const unsigned width=x64 ? 8 : 4;
        std::array<std::vector<Memory>,2> buffers,owners;
        for(unsigned direction=0;direction<2;++direction) {
            const auto count=channels[direction];auto offsets=bytes((count+1)*4);
            DspEnginePointerTable table{{{bytes(count*width),false}}, {}};
            for(unsigned ch=0;ch<count;++ch) {
                offsets.writeWordAt(ch*4,(direction ? 512 : 256)+ch*32);
                Memory owner(std::vector<std::uint8_t>(frames*16+8,0xa5));const auto view=owner.view(4,frames*16,1);
                if(direction)for(unsigned n=0;n<frames;++n)for(unsigned lane=0;lane<4;++lane)view.writeWordAt(n*16+lane*4,sampleWord(ch+3,n,lane));
                owners[direction].push_back(owner);buffers[direction].push_back(view);
                table.regions.push_back({view,direction==0});table.cells.push_back({0,ch*width,ch+1,0,width});
            }
            check(graph.bindDspEnginePointer(b.id,direction ? "p_outOffsets" : "p_inOffsets",offsets,false) &&
                  graph.bindDspEnginePointerTable(b.id,direction ? "p_outBuffers" : "p_inBuffers",table),"Bind zero-terminated offset list and native-width channel pointer table");
        }
        auto runtime=graph.compileMono4(b.outputs);
        std::array<std::array<Mono4,frames>,3> input{};
        for(unsigned ch=0;ch<3;++ch)for(unsigned n=0;n<frames;++n){Words words{};for(unsigned lane=0;lane<4;++lane)words[lane]=sampleWord(ch,n,lane);input[ch][n]=std::bit_cast<Mono4>(words);}
        const auto original=input;
        for(const std::size_t size:{1,3,7,31,64,257}) {
            runtime.reset();counter.writeWordAt(0,0);std::array<std::array<Mono4,frames>,4> output{};
            for(unsigned offset=0;offset<frames;offset+=size) {
                const auto count=std::min<std::size_t>(size,frames-offset);std::array<Mono4InputBuffer,3> in;std::array<Mono4OutputBuffer,4> out;
                for(unsigned ch=0;ch<3;++ch)in[ch]=std::span{input[ch]}.subspan(offset,count);
                for(unsigned ch=0;ch<4;++ch)out[ch]=std::span{output[ch]}.subspan(offset,count);
                const auto rounding=std::fegetround(),flags=std::fetestexcept(FE_ALL_EXCEPT);
                check(runtime.process(in,out),"Render original native packet transfers");
                check(std::fegetround()==rounding && std::fetestexcept(FE_ALL_EXCEPT)==flags,"Transfer loops preserve the caller floating-point environment");
            }
            for(unsigned n=0;n<frames;++n)for(unsigned lane=0;lane<4;++lane) {
                for(unsigned ch=0;ch<3;++ch)check(std::bit_cast<std::uint32_t>(output[ch][n][lane])==(ch<channels[1] ? sampleWord(ch+3,n,lane) : 0U),"Both original loops route exact packet words, including NaNs, signed zeros and subnormals");
                check(output[3][n][lane]==(lane==0 ? float((n+1)*4) : 0.F),"Recovered fragment advances the packet cursor by four");
            }
            for(unsigned ch=0;ch<channels[0];++ch)for(unsigned n=0;n<frames;++n)for(unsigned lane=0;lane<4;++lane)
                check(buffers[0][ch].wordAt(n*16+lane*4)==sampleWord(ch,n,lane),"Input frame packets were copied into the intended caller buffer");
            for(const auto& list:owners)for(const auto& owner:list)for(unsigned i=0;i<4;++i)
                check(owner.byte(i)==0xa5 && owner.byte(owner.size()-1-i)==0xa5,"Packet stores preserve view neighbors");
            for(unsigned ch=0;ch<3;++ch)for(unsigned n=0;n<frames;++n)check(std::bit_cast<Words>(input[ch][n])==std::bit_cast<Words>(original[ch][n]),"Caller input buffers remain unchanged");
            check(counter.wordAt(0)==frames*4,"Every processed packet is counted once across all buffer partitions");
        }
    }
}

bool condition(std::string_view op,int k)
{
    if(op=="je")return k==3;if(op=="jne")return k!=3;
    if(op=="jl" || op=="jb")return k<3;if(op=="jge" || op=="jae")return k>=3;
    if(op=="jg" || op=="ja")return k>3;if(op=="jle" || op=="jbe")return k<=3;
    const auto parity=std::popcount(static_cast<std::uint32_t>(k-3)&255U)%2==0;
    return op=="jp" ? parity : !parity;
}
void regeneratedFlags(DspBackend backend)
{
    for(const auto branch:{"je","jne","jl","jge","jg","jle","jb","jae","jbe","ja","jp","jnp"})for(bool forward:{false,true}) {
        const auto source=std::string("memrefin data;streamin n;streamout out;mov edx,data[0];mov ecx,n;mov ebx,0;cmp ebx,1;again:")+
            (forward ? "jmp producer;producer:" : "")+"cmp ecx,0;jle done;cmp ecx,3;"+branch+" taken;jmp next;taken:inc ebx;next:dec ecx;add edx,0;jmp again;done:movd xmm0,ebx;cvtdq2ps xmm0,xmm0;movaps out,xmm0;";
        Graph graph(backend);const auto b=graph.addDspCode(compileDspCode(source,DspLanguage::Assembler));graph.connectDspInput(graph.addInput(),b.id,1);auto runtime=graph.compile(b.outputs);check(runtime.setDspMemRefInput(b.id,0,bytes(16)),"Bind bounded cursor for flag-clobbering address arithmetic");
        constexpr std::array counts{-1,0,1,2,3,4,5,7,8,16,31};std::array<float,counts.size()> input{},out{};
        for(unsigned n=0;n<counts.size();++n)input[n]=std::bit_cast<float>(static_cast<std::uint32_t>(counts[n]));
        for(const std::size_t size:{1,3,7,31,64}) {
            runtime.reset();for(unsigned offset=0;offset<input.size();offset+=size){const auto count=std::min<std::size_t>(size,input.size()-offset);check(runtime.process(std::array{InputBuffer{std::span{input}.subspan(offset,count)}},std::array{OutputBuffer{std::span{out}.subspan(offset,count)}}),"Render loops regenerating consumed flags");}
            for(unsigned n=0;n<counts.size();++n){int expected=0;for(int k=counts[n];k>0;--k)expected+=condition(branch,k);check(out[n]==float(expected),"Zero, signed, carry and parity branches consume each iteration's new flags");}
        }
    }
    // CMP(6), JZ(6), DEC(2), ADD(6), JMP(5): native near displacement -25.
    const auto numeric=compileDspCode("memrefin data;streamin n;streamout out;mov edx,data[0];mov ecx,n;cmp ecx,1;again:cmp ecx,0;jz done;dec ecx;add edx,0;jmp -25;done:movd xmm0,ecx;movaps out,xmm0;",DspLanguage::Assembler);
    Graph graph(backend);const auto b=graph.addDspCode(numeric);graph.connectDspInput(graph.addInput(),b.id,1);auto runtime=graph.compile(b.outputs);check(runtime.setDspMemRefInput(b.id,0,bytes(4)),"Bind numeric-loop cursor");
    const std::array input{std::bit_cast<float>(3U),std::bit_cast<float>(0U),std::bit_cast<float>(7U)};std::array<float,3> out{};check(runtime.process(std::array{InputBuffer{input}},std::array{OutputBuffer{out}}) && out==std::array<float,3>{},"Numeric backward branches use the same final flag-liveness checks");
}

void partialFlagsAndConditionalBackedge(DspBackend backend)
{
    const auto p=compileDspCode("memrefin data;streamin n;streamout out;mov edx,data[0];mov ecx,n;mov eax,0;mov ebx,0;cmp ecx,1;again:inc eax;jp even;jmp measured;even:inc ebx;measured:cmp eax,ecx;jge done;cmp edx,0;jnz again;done:movd xmm0,ebx;cvtdq2ps xmm0,xmm0;movaps out,xmm0;",DspLanguage::Assembler);
    Graph graph(backend);const auto b=graph.addDspCode(p);graph.connectDspInput(graph.addInput(),b.id,1);auto runtime=graph.compile(b.outputs);check(runtime.setDspMemRefInput(b.id,0,bytes(4)),"Bind non-null reference for conditional backedge");
    std::array<float,34> input{},out{};for(unsigned n=0;n<input.size();++n)input[n]=std::bit_cast<float>(n);
    for(unsigned size:{1,3,7,31,64}) {
        runtime.reset();for(unsigned offset=0;offset<input.size();offset+=size){const auto count=std::min<std::size_t>(size,input.size()-offset);check(runtime.process(std::array{InputBuffer{std::span{input}.subspan(offset,count)}},std::array{OutputBuffer{std::span{out}.subspan(offset,count)}}),"Render partial flag producer across conditional backward branch");}
        for(unsigned n=0;n<input.size();++n){unsigned expected=0;for(unsigned i=1;i<=std::max(n,1U);++i)expected+=std::popcount(i&255U)%2==0;check(out[n]==float(expected),"INC defines the consumed parity while preserved carry is dead before the next CMP");}
    }
}

void rejectLiveFlags()
{
    for(const auto branch:{"jz","jl","jb","jp"})for(bool forward:{false,true}) {
        const auto source=std::string("memrefin data;streamout out;mov edx,data[0];mov eax,1;cmp eax,0;again:")+
            (forward ? "jmp consumer;consumer:" : "")+branch+" done;cmp eax,0;add edx,0;jmp again;done:movaps xmm0,_ONE;movaps out,xmm0;";
        bool rejected=false;try{static_cast<void>(compileDspCode(source,DspLanguage::Assembler));}catch(const DspCompileError& e){rejected=std::string_view(e.what()).find("flags are unavailable at backward branch target")!=std::string_view::npos;}
        check(rejected,"Never accept an incoming flag read before replacement on a resolved loop path");
    }
    // INC replaces ZF/SF/PF but preserves CF. A live incoming CF must still fail.
    const auto source="memrefin data;streamout out;mov edx,data[0];mov eax,0;cmp eax,1;again:inc eax;jb done;cmp eax,0;add edx,0;jmp again;done:movaps xmm0,_ONE;movaps out,xmm0;";
    bool rejected=false;try{static_cast<void>(compileDspCode(source,DspLanguage::Assembler));}catch(const DspCompileError& e){rejected=std::string_view(e.what()).find("flags are unavailable at backward branch target")!=std::string_view::npos;}
    check(rejected,"Replacing other flags does not define the carry preserved by INC");
    for(const auto source:{"streamout out;jz done;done:","memrefin data;streamout out;mov edx,data[0];mov eax,0;cmp eax,1;add edx,0;jp done;done:"}) {
        bool rejected=false;try{static_cast<void>(compileDspCode(source,DspLanguage::Assembler));}catch(const DspCompileError&){rejected=true;}check(rejected,"Undefined and clobbered flags remain errors outside loops");
    }
}
}
int main()
{
    try{rejectLiveFlags();for(auto backend:{DspBackend::Interpreter,DspBackend::LlvmOrc})if(dspBackendAvailable(backend)){openstone::tests::withReferenceSources("nativeTransfer", [&] { nativeTransfer(backend); });regeneratedFlags(backend);partialFlagsAndConditionalBackedge(backend);}}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
