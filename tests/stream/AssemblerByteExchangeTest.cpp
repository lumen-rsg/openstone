#include "stream/Runtime.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace openstone::stream;
using Words=std::array<std::uint32_t,4>;
constexpr std::array parents{"eax","ebx","ecx","edx"};
constexpr std::array aliases{"al","bl","cl","dl","ah","bh","ch","dh"};
constexpr std::array branches{"je","jz","jne","jnz","ja","jb","jbe","jae","jp","jnp","jg","jl","jng","jnl","jge","jle"};
constexpr std::size_t ports=branches.size()+3;
constexpr Words pattern{0x01234567,0x89abcdef,0x7f812345,0xffc54321};
using Buffers=std::array<std::array<Mono4,64>,ports>;
void check(bool value,std::string_view message) { if (!value) throw std::runtime_error(std::string(message)); }
unsigned shift(unsigned alias) {return alias<4 ? 0 : 8;}
unsigned byte(std::uint32_t word,unsigned alias) {return (word>>shift(alias))&255U;}
std::uint32_t replace(std::uint32_t word,unsigned alias,unsigned value) {return (word&~(255U<<shift(alias)))|((value&255U)<<shift(alias));}
std::uint32_t original(unsigned n) {return 0xa5aa55ccU^(n*0x9e3779b9U);}
struct Environment {std::fenv_t saved;Environment(){std::fegetenv(&saved);}~Environment(){std::fesetenv(&saved);}};
std::string header()
{
    std::string code="streamin a,b,seed;memrefin data,alias;memarrayrefin waves;floatarrayin samples[4];streamout ";
    for (unsigned n=0;n<branches.size();++n) code+="f"+std::to_string(n)+",";
    return code+"left,right,guard;float zero=0,one=1,nan=0x7fc12345,cell;"
        "float pattern[4]={0x01234567,0x89abcdef,0x7f812345,0xffc54321};"
        "int cells[4]={0x01234567,0x89abcdef,0x7f812345,0xffc54321};"
        "double wide[2]={0x0123456789abcdef,0xfff8000000000042};";
}
std::string seedFlags(std::string_view suffix={})
{
    const auto end="seeded"+std::string(suffix),cmp="cmpSeed"+std::string(suffix),
        overflow="overflow"+std::string(suffix),carry="carryOverflow"+std::string(suffix);
    return "mov edx,seed;cmp edx,3;jb "+cmp+";je "+overflow+";cmp edx,4;je "+carry+
        ";movaps xmm6,nan;comiss xmm6,nan;jmp "+end+";"+cmp+":cmp edx,1;jmp "+end+";"+
        overflow+":mov edx,0x7fffffff;add edx,1;jmp "+end+";"+carry+":mov edx,0x80000000;add edx,0x80000000;"+end+":";
}
std::array<bool,branches.size()> flagValues(unsigned seed)
{
    constexpr std::array carry{true,false,false,false,true,true};
    constexpr std::array parity{true,true,false,true,true,true};
    constexpr std::array zero{false,true,false,false,true,true};
    constexpr std::array less{true,false,false,false,true,false};
    const auto c=carry[seed],p=parity[seed],z=zero[seed],l=less[seed];
    return {z,z,!z,!z,!c&&!z,c,c||z,!c,p,!p,!l&&!z,l,l||z,!l,!l,l||z};
}
std::string readFlags()
{
    std::string code;
    for (unsigned n=0;n<branches.size();++n) {
        const auto id=std::to_string(n);
        code+="movaps xmm7,zero;movaps f"+id+",xmm7;"+branches[n]+" yes"+id+";jmp next"+id+
            ";yes"+id+":movaps xmm7,one;movaps f"+id+",xmm7;next"+id+":";
    }
    return code;
}
void checkFlags(const Buffers& output,unsigned frame,unsigned seed,const std::string& context)
{
    const auto flags=flagValues(seed);
    for (unsigned port=0;port<branches.size();++port) {
        const float value=flags[port] ? 1.F : 0.F;
        if (output[port][frame]!=Mono4{value,value,value,value}) throw std::runtime_error(context+" changed "+branches[port]);
    }
}
std::string capture(std::string_view reg,std::string_view output)
{
    return "movd xmm0,"+std::string(reg)+";movaps "+std::string(output)+",xmm0;";
}
struct Sample {unsigned a,b;};
void registers(unsigned left,unsigned right,bool exhaustive=false,std::string_view flow={})
{
    const bool shared=left%4==right%4;
    const auto operation="XCHG "+std::string(aliases[left])+","+aliases[right]+";";
    std::string code=header()+seedFlags()+"mov "+parents[left%4]+",a;";
    if (!shared) code+="mov "+std::string(parents[right%4])+",b;";
    if (flow=="call") code+="call swap;";
    else if (flow=="join") code+="jz alternate;"+operation+"jmp joined;alternate:"+operation+"joined:";
    else code+=operation+(flow=="twice" ? operation : "");
    code+=capture(parents[left%4],"left")+capture(parents[right%4],"right")+readFlags();
    if (flow=="call") code+="jmp end;swap:"+operation+"ret;end:";
    const auto program=compileDspCode(code,DspLanguage::Assembler);
    std::vector<Sample> samples;
    if (exhaustive) for (unsigned a=0;a<256;++a) for (unsigned b=0;b<256;++b) samples.push_back({a,b});
    else for (unsigned n=0;n<263;++n) samples.push_back({n&255U,(n*73+19)&255U});
    Environment environment;
    for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);const auto block=graph.addDspCode(program);
        for (unsigned port=0;port<3;++port) graph.connectDspInput(graph.addInput(),block.id,port);
        auto runtime=graph.compileMono4(block.outputs);
        for (const std::size_t size : {1,3,7,31,64}) {
            if (exhaustive && size!=64) continue;
            runtime.reset();std::array<std::array<Mono4,64>,3> input{};Buffers output{};
            std::fesetround(FE_UPWARD);std::feclearexcept(FE_ALL_EXCEPT);std::feraiseexcept(FE_DIVBYZERO);
            for (std::size_t offset=0;offset<samples.size();offset+=size) {
                const auto count=std::min(size,samples.size()-offset);
                for (unsigned i=0;i<count;++i) {
                    const auto n=static_cast<unsigned>(offset+i);auto a=replace(original(n),left,samples[n].a);
                    if (shared && left!=right) a=replace(a,right,samples[n].b);
                    const auto b=shared ? a : replace(original(n)^0x6e198a54U,right,samples[n].b);
                    input[0][i]=std::bit_cast<Mono4>(Words{a,0x7f812345,0x80000001,0xffc12345});
                    input[1][i]=std::bit_cast<Mono4>(Words{b,0xff812345,0x80000100,0x7fc12345});
                    input[2][i].fill(std::bit_cast<float>(n%6));
                }
                std::array<Mono4InputBuffer,3> in;std::array<Mono4OutputBuffer,ports> out;
                for (unsigned p=0;p<3;++p) in[p]=std::span{input[p]}.first(count);
                for (unsigned p=0;p<ports;++p) out[p]=std::span{output[p]}.first(count);
                check(runtime.process(in,out),"Render byte register exchanges");
                for (unsigned i=0;i<count;++i) {
                    const auto n=static_cast<unsigned>(offset+i),a=std::bit_cast<Words>(input[0][i])[0],b=std::bit_cast<Words>(input[1][i])[0];
                    const auto oldLeft=byte(a,left),oldRight=byte(b,right);
                    auto expectedA=replace(a,left,oldRight),expectedB=replace(b,right,oldLeft);
                    if (shared) expectedA=expectedB=replace(expectedA,right,oldLeft);
                    if (flow=="twice") {expectedA=a;expectedB=b;}
                    check(std::bit_cast<Words>(output[branches.size()][i])==Words{expectedA,0,0,0} &&
                        std::bit_cast<Words>(output[branches.size()+1][i])==Words{expectedB,0,0,0},"Byte XCHG uses both old bytes and preserves every unselected parent bit, including self/shared-parent aliases");
                    checkFlags(output,i,n%6,operation);
                }
            }
            check(std::fegetround()==FE_UPWARD && std::fetestexcept(FE_ALL_EXCEPT)==FE_DIVBYZERO,"Register exchanges preserve caller floating-point state");
        }
    }
}
std::vector<std::uint8_t> rawBytes(Words words)
{
    std::vector<std::uint8_t> result(16);
    for (unsigned n=0;n<16;++n) result[n]=static_cast<std::uint8_t>(words[n/4]>>((n%4)*8));
    return result;
}
Words rawWords(const std::vector<std::uint8_t>& bytes)
{
    Words words{};
    for (unsigned n=0;n<4;++n) if (n*4+4<=bytes.size()) for (unsigned b=0;b<4;++b) words[n]|=std::uint32_t{bytes[n*4+b]}<<(b*8);
    return words;
}
void memorySwaps()
{
    constexpr std::size_t frames=263;
    constexpr std::array scales{1U,2U,4U,8U};
    constexpr std::array<std::int32_t,4> displacements{0,1,3,-1};
    Environment environment;
    for (unsigned reg=0;reg<8;++reg) for (unsigned storage=0;storage<8;++storage) for (bool reverse : {false,true}) {
        const bool external=storage<4;
        const auto base=(reg+1)%4,index=(reg+2)%4,form=storage==3 ? 0U : (reg+storage)%4;
        const std::string expression=std::string(parents[index])+(form ? "*"+std::to_string(scales[form]) : "")+
            (form==1 ? "+1" : form==2 ? "+3" : form==3 ? "-1" : "");
        const std::string target=external ? "["+std::string(parents[base])+"+"+expression+"]" :
            (storage==4 ? "cells[" : storage==5 ? "wide[" : storage==6 ? "cell[" : "guard[")+expression+"]";
        std::string code=header()+seedFlags()+"mov "+parents[reg%4]+",a;mov "+parents[base]+",data[0];mov "+parents[index]+",b;";
        if (storage>=6) code+=std::string("movaps xmm1,pattern[0];movaps ")+(storage==6 ? "cell" : "guard")+",xmm1;";
        code+="xchg "+(reverse ? target+","+aliases[reg] : std::string(aliases[reg])+","+target)+";"+capture(parents[reg%4],"left");
        if (storage!=7) code+="movaps xmm0,"+std::string(external ? "["+std::string(parents[base])+"]" : storage==4 ? "cells[0]" : storage==5 ? "wide[0]" : "cell")+";movaps guard,xmm0;";
        code+="mov "+std::string(parents[reg%4])+",0;mov "+aliases[reg]+","+target+";"+capture(parents[reg%4],"right")+readFlags();
        const auto program=compileDspCode(code,DspLanguage::Assembler);
        std::array<std::array<Mono4,frames>,3> input{};
        for (unsigned n=0;n<frames;++n) {
            const auto position=n<256 ? n%17 : n==256 ? UINT32_MAX : n==257 ? 17U : n==258 ? 16U : n==259 ? 0x80000000U : n==260 ? 0x40000000U : n==261 ? 0x7fffffffU : 0;
            input[0][n]=std::bit_cast<Mono4>(Words{replace(original(n),reg,n&255U),0x7f812345,0xffc12345,0x80000001});
            input[1][n].fill(std::bit_cast<float>(position));input[2][n].fill(std::bit_cast<float>(n%6));
        }
        for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);const auto block=graph.addDspCode(program);
            for (unsigned port=0;port<3;++port) graph.connectDspInput(graph.addInput(),block.id,port);
            auto runtime=graph.compileMono4(block.outputs);
            for (const std::size_t size : {1,3,7,31,64}) {
                runtime.reset();const auto length=storage==2 ? 0U : storage==3 ? 1U : external ? 17U : 16U;
                const auto start=storage==1 ? 5U : 0U;
                std::vector<std::uint8_t> backing(length+(storage==1 ? 12U : 0U));
                for (unsigned n=0;n<backing.size();++n) backing[n]=static_cast<std::uint8_t>(n*73+19);
                const openstone::green::Memory memory(backing);
                check(runtime.setDspMemRefInput(block.id,3,storage==1 ? memory.view(start,length,1) : memory),"Bind byte exchange memory");
                auto expected=external ? std::vector<std::uint8_t>(backing.begin()+start,backing.begin()+start+length) :
                    storage==5 ? rawBytes({0x89abcdef,0x01234567,0x42,0xfff80000}) : rawBytes(pattern);
                for (std::size_t offset=0;offset<frames;offset+=size) {
                    const auto count=std::min(size,frames-offset);Buffers output{};
                    std::array<Mono4InputBuffer,3> in;std::array<Mono4OutputBuffer,ports> out;
                    for (unsigned p=0;p<3;++p) in[p]=std::span{input[p]}.subspan(offset,count);
                    for (unsigned p=0;p<ports;++p) out[p]=std::span{output[p]}.first(count);
                    std::fesetround(FE_UPWARD);std::feclearexcept(FE_ALL_EXCEPT);std::feraiseexcept(FE_DIVBYZERO);
                    check(runtime.process(in,out),"Render byte register/memory swaps");
                    for (unsigned i=0;i<count;++i) {
                        const auto n=static_cast<unsigned>(offset+i);if (storage>=6) expected=rawBytes(pattern);
                        const auto address=std::bit_cast<std::int32_t>(std::bit_cast<std::uint32_t>(input[1][n][0])*scales[form]+static_cast<std::uint32_t>(displacements[form]));
                        const bool valid=address>=0 && static_cast<unsigned>(address)<expected.size();
                        const auto oldByte=valid ? expected[address] : 0U;
                        const auto parent=std::bit_cast<Words>(input[0][n])[0];
                        if (valid) expected[address]=static_cast<std::uint8_t>(byte(parent,reg));
                        check(std::bit_cast<Words>(output[branches.size()][i])==Words{replace(parent,reg,oldByte),0,0,0},"Memory exchange replaces only the chosen register byte with the old bounded memory byte");
                        check(std::bit_cast<Words>(output[branches.size()+1][i])==Words{valid ? std::uint32_t{expected[address]}<<shift(reg) : 0U,0,0,0},"Memory readback observes the old register byte at the original address");
                        check(std::bit_cast<Words>(output[branches.size()+2][i])==rawWords(expected),"Every neighboring byte and raw word survives a byte exchange");
                        checkFlags(output,i,n%6,"Memory XCHG");
                    }
                    if (external) {
                        std::copy(expected.begin(),expected.end(),backing.begin()+start);
                        check(memory.snapshotBytes()==backing,"All Mem bytes and guards outside an unaligned binding survive except the selected exchanged byte");
                    }
                    check(std::fegetround()==FE_UPWARD && std::fetestexcept(FE_ALL_EXCEPT)==FE_DIVBYZERO,"Memory exchanges preserve caller floating-point state");
                }
            }
        }
    }
}
void literal(const std::string& body,std::uint32_t left,std::uint32_t right,Words guard={},std::string_view routines={})
{
    auto code=header()+"mov edx,0;cmp edx,1;"+body+readFlags();
    if (!routines.empty()) code+="jmp finished;"+std::string(routines)+"finished:";
    const auto program=compileDspCode(code,DspLanguage::Assembler);
    for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);const auto block=graph.addDspCode(program);auto runtime=graph.compileMono4(block.outputs);
        for (const std::size_t size : {1,3,7,31,64}) {
            runtime.reset();std::vector<std::uint8_t> bytes(17);
            for (unsigned n=0;n<bytes.size();++n) bytes[n]=static_cast<std::uint8_t>(n*13+19);
            const openstone::green::Memory memory(bytes);check(runtime.setDspMemRefInput(block.id,3,memory),"Bind literal swap memory");
            Buffers output{};
            for (std::size_t offset=0;offset<67;offset+=size) {
                const auto count=std::min(size,67-offset);std::array<Mono4OutputBuffer,ports> out;
                for (unsigned p=0;p<ports;++p) out[p]=std::span{output[p]}.first(count);
                check(runtime.process({},out),"Render byte exchange address/control-flow literals");
                for (unsigned n=0;n<count;++n) {
                    checkFlags(output,n,0,"Literal XCHG");
                    check(std::bit_cast<Words>(output[branches.size()][n])==Words{left,0,0,0} &&
                          std::bit_cast<Words>(output[branches.size()+1][n])==Words{right,0,0,0} &&
                          std::bit_cast<Words>(output[branches.size()+2][n])==guard,"Byte swaps use the old effective address and preserve exact literal data/neighbor words");
                }
            }
        }
    }
}
void addressesAndControlFlow()
{
    for (unsigned alias=0;alias<8;++alias) for (unsigned scale : {1U,2U,4U,8U}) for (bool reverse : {false,true}) {
        const auto reg=std::string(parents[alias%4]),base=std::string(parents[(alias+1)%4]);
        const auto target="["+base+"+"+reg+"*"+std::to_string(scale)+"-"+std::to_string(0x203U*scale-13U)+"]";
        // Reset byte 13 each sample so an incorrect post-swap index cannot accidentally pass later.
        const auto body="mov "+reg+",0x203;mov "+base+",data[0];movaps xmm0,pattern[0];movaps ["+base+"],xmm0;mov ["+base+"+13],"+aliases[alias]+";"
            "mov "+reg+",0xde;mov ["+base+"+13],"+aliases[alias%4]+";mov "+reg+",0x203;"+
            "xchg "+(reverse ? target+","+aliases[alias] : std::string(aliases[alias])+","+target)+";"+
            capture(reg,"left")+"mov "+reg+",0;mov "+aliases[alias]+",["+base+"+13];"+capture(reg,"right")+
            "movaps xmm0,["+base+"];movaps guard,xmm0;";
        auto bytes=rawBytes(pattern);bytes[13]=static_cast<std::uint8_t>(byte(0x203U,alias));
        literal(body,replace(0x203U,alias,0xde),std::uint32_t{byte(0x203U,alias)}<<shift(alias),rawWords(bytes));
    }
    for (unsigned alias=0;alias<8;++alias) {
        const auto reg=std::string(parents[alias%4]);
        literal("mov "+reg+",0x12345678;jmp 2;xchg "+aliases[alias]+","+aliases[alias]+";"+capture(reg,"left"),0x12345678,0);
        for (bool reverse : {false,true}) {
            const auto op=reverse ? "cells[0],"+std::string(aliases[alias]) : std::string(aliases[alias])+",cells[0]";
            literal("mov "+reg+",0x12345678;jmp 6;xchg "+op+";"+capture(reg,"left")+"movaps xmm0,cells[0];movaps guard,xmm0;",0x12345678,0,pattern);
        }
    }
    literal("mov eax,0x12345678;mov ebx,0xabcdef01;call swap;"+capture("eax","left")+capture("ebx","right"),0x12340178,0xabcdef56,{},"swap:xchg ah,bl;ret;");
    literal("mov eax,0x12345678;mov ebx,0xabcdef01;mov ecx,3;again:xchg ah,bl;dec ecx;jnz again;mov ecx,0;cmp ecx,1;"+capture("eax","left")+capture("ebx","right"),0x12340178,0xabcdef56);
}
void provenanceAndMemoryKinds()
{
    for (bool reverse : {false,true}) for (bool alias : {false,true}) {
        const auto address="[ecx+edx]";
        const auto code="streamin index;memrefin data,alias;streamout out;float zero=0,one=1;"
            "movaps xmm0,zero;movaps out,xmm0;mov ecx,data[0];mov edx,0;mov eax,routine;mov [ecx+edx*4],eax;mov ebx,0;mov edx,index;"+
            std::string(alias ? "mov ecx,alias[0];" : "")+"mov bh,"+address+";xchg "+
            (reverse ? std::string(address)+",bh;" : "bh,"+std::string(address)+";")+
            "mov ecx,data[0];mov edx,0;call [ecx+edx*4];jmp end;routine:movaps xmm0,one;movaps out,xmm0;ret;end:";
        const auto program=compileDspCode(code,DspLanguage::Assembler);
        for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);const auto block=graph.addDspCode(program);graph.connectDspInput(graph.addInput(),block.id,0);auto runtime=graph.compile(block.outputs);
            for (const std::size_t size : {1,3,7,31,64}) {
                runtime.reset();const openstone::green::Memory memory(std::vector<std::uint8_t>(20));
                check(runtime.setDspMemRefInput(block.id,1,memory) && runtime.setDspMemRefInput(block.id,2,memory.view(1,19,1)),"Bind aliased XCHG target table views");
                constexpr std::array positions{-1,0,1,2,3,4,7,18,19,20};std::array<float,positions.size()> input{},output{};
                for (unsigned n=0;n<positions.size();++n) input[n]=std::bit_cast<float>(static_cast<std::uint32_t>(positions[n]));
                for (std::size_t offset=0;offset<input.size();offset+=size) {
                    const auto count=std::min(size,input.size()-offset);
                    check(runtime.process(std::array{InputBuffer{std::span{input}.subspan(offset,count)}},std::array{OutputBuffer{std::span{output}.subspan(offset,count)}}),"Render byte exchanges over saved routine targets");
                }
                for (unsigned n=0;n<positions.size();++n) {
                    const auto actual=positions[n]+(alias ? 1 : 0);const bool valid=positions[n]>=0 && actual<20;
                    check(output[n]==(!valid || actual>=4 ? 1.F : 0.F),"Identity byte swaps invalidate overlapping callable provenance, retain neighboring targets, and ignore invalid writes");
                }
            }
        }
    }
    for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        for (unsigned field=4;field<8;++field) {
            const auto program=compileDspCode("memarrayrefin waves;streamout before,after,old;mov eax,waves["+std::to_string(field)+
                "];movd xmm0,[eax];movaps before,xmm0;mov ebx,0x12345678;xchg bh,[eax];movd xmm0,[eax];movaps after,xmm0;"+capture("ebx","old"),DspLanguage::Assembler);
            Graph graph(backend);const auto block=graph.addDspCode(program);auto runtime=graph.compileMono4(block.outputs);
            const auto bytes=rawBytes(pattern);const openstone::green::Memory memory(bytes);
            check(runtime.setDspMemArrayInput(block.id,0,openstone::green::MemoryArray({memory})),"Bind protected MemArray tables for XCHG");
            std::array<Mono4,1> before{},after{},old{};
            check(runtime.process({},std::array{Mono4OutputBuffer{before},Mono4OutputBuffer{after},Mono4OutputBuffer{old}}),"Render protected table byte exchanges");
            check(std::bit_cast<Words>(before[0])==std::bit_cast<Words>(after[0]) && memory.snapshotBytes()==bytes &&
                std::bit_cast<Words>(old[0])==Words{replace(0x12345678,5,std::bit_cast<Words>(before[0])[0]&255U),0,0,0},"XCHG can read table bytes but cannot modify protected descriptors or wave bytes through table pointers");
        }
        for (bool reverse : {false,true}) {
            const auto code="memarrayrefin waves;streamin index,value;streamout old;mov eax,waves[5];mov eax,[eax];mov ecx,index;mov ebx,value;"+
                std::string(reverse ? "xchg [eax+ecx],bh;" : "xchg bh,[eax+ecx];")+capture("ebx","old");
            Graph graph(backend);const auto block=graph.addDspCode(compileDspCode(code,DspLanguage::Assembler));
            graph.connectDspInput(graph.addInput(),block.id,1);graph.connectDspInput(graph.addInput(),block.id,2);auto runtime=graph.compile(block.outputs);
            constexpr std::size_t frames=67;std::array<float,frames> input{},values{},output{};
            for (unsigned n=0;n<frames;++n) {input[n]=std::bit_cast<float>(static_cast<std::uint32_t>(n%19-1));values[n]=std::bit_cast<float>(original(n));}
            for (const std::size_t size : {1,3,7,31,64}) {
                runtime.reset();auto expected=rawBytes(pattern);expected.push_back(222);const openstone::green::Memory memory(expected);
                check(runtime.setDspMemArrayInput(block.id,0,openstone::green::MemoryArray({memory})),"Bind loaded wave data for byte exchange");
                for (std::size_t offset=0;offset<frames;offset+=size) {
                    const auto count=std::min(size,frames-offset);
                    check(runtime.process(std::array{InputBuffer{std::span{input}.subspan(offset,count)},InputBuffer{std::span{values}.subspan(offset,count)}},std::array{OutputBuffer{std::span{output}.subspan(offset,count)}}),"Render wave byte exchanges");
                    for (unsigned n=offset;n<offset+count;++n) {
                        const auto position=std::bit_cast<std::int32_t>(input[n]);const bool valid=position>=0 && static_cast<unsigned>(position)<expected.size();
                        const auto old=valid ? expected[position] : 0U;if (valid) expected[position]=static_cast<std::uint8_t>(byte(original(n),5));
                        check(std::bit_cast<std::uint32_t>(output[n])==replace(original(n),5,old),"Loaded-wave byte swaps preserve other parent bits and return the old addressed byte");
                    }
                    check(memory.snapshotBytes()==expected,"Wave byte exchanges change only the selected bytes across buffers");
                }
            }
        }
    }
    for (auto body : {"mov eax,routine;mov cells[0],eax;mov ebx,0;xchg bh,cells[0];mov eax,cells[0];call eax;",
                     "mov eax,routine;mov ecx,data[0];mov [ecx],eax;mov ebx,0;xchg [ecx+3],bh;mov eax,[ecx];call eax;"}) {
        bool rejected=false;
        try {static_cast<void>(compileDspCode(header()+body+"jmp end;routine:ret;end:",DspLanguage::Assembler));}
        catch (const DspCompileError&) {rejected=true;}
        check(rejected,"Partial byte exchanges erase static callable target facts");
    }
    literal("mov eax,routine;mov cells[1],eax;mov ebx,0;xchg bh,cells[0];mov eax,cells[1];call eax;",0x12345678,0,{},"routine:mov eax,0x12345678;"+capture("eax","left")+"ret;");
}
void diagnostics()
{
    constexpr std::array invalid{
        "xchg al,al;", "mov eax,0;xchg al,bl;", "mov eax,0;xchg al,eax;", "mov eax,0;xchg eax,ah;",
        "mov eax,0;xchg al,1;", "mov eax,0;xchg 1,al;", "xchg xmm0,xmm1;", "xchg cell,cells[0];",
        "stage 0;mov eax,0;xchg al,a;", "mov eax,0;xchg samples[0],al;", "mov eax,0;xchg al,data[0];", "mov eax,0;xchg waves[0],al;",
        "mov eax,data[0];xchg ah,ah;", "mov eax,routine;mov ebx,0;xchg al,bl;", "mov eax,0;mov ebx,data[0];xchg al,bh;",
        "mov eax,0;xchg al;", "mov eax,0;xchg al,al,0;", "mov eax,0;mov ebx,0;xchg al,[bl];",
        "mov eax,0;xchg al,al;jz done;done:", "mov eax,0;xchg al,al;jb done;done:", "mov eax,0;xchg al,al;jp done;done:",
        "mov eax,0;jmp 1;xchg al,ah;", "mov eax,data[0];mov ebx,0;jmp 2;xchg [eax],bl;",
    };
    for (auto body : invalid) {
        bool rejected=false;
        try {static_cast<void>(compileDspCode(header()+body+"jmp end;routine:ret;end:",DspLanguage::Assembler));}
        catch (const DspCompileError& e) {rejected=e.line>0 && e.column>0;}
        check(rejected,"Byte XCHG rejects unwritten/opaque parents, mixed widths, two memories, immediates, stage-zero inputs, external arrays/descriptors, missing flags and unknown spans");
    }
    // A swap requires initialized numeric data, but no incoming flags unless a later branch consumes them.
    for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);const auto block=graph.addDspCode(compileDspCode("streamout out;mov eax,0x12345678;xchg al,ah;"+capture("eax","out"),DspLanguage::Assembler));auto runtime=graph.compileMono4(block.outputs);
        std::array<Mono4,1> output{};check(runtime.process({},std::array{Mono4OutputBuffer{output}}) && std::bit_cast<Words>(output[0])==Words{0x12347856,0,0,0},"Data-only swaps require no flags and preserve the upper parent word");
    }
}
}
int main()
{
    try {
        for (unsigned a=0;a<8;++a) for (unsigned b=0;b<8;++b) registers(a,b);
        registers(4,1,true);registers(4,0,true);
        for (auto flow : {"call","join","twice"}) {registers(4,0,false,flow);registers(4,1,false,flow);}
        memorySwaps();addressesAndControlFlow();provenanceAndMemoryKinds();diagnostics();
    } catch (const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
