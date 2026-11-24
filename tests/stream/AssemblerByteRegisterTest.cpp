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
using Words=std::array<std::uint32_t,4>;
void check(bool value,std::string_view message) { if (!value) throw std::runtime_error(std::string(message)); }
constexpr std::array parents{"eax","ebx","ecx","edx"};
constexpr std::array bytes{"al","bl","cl","dl","ah","bh","ch","dh"};
constexpr std::array branches{"je","jz","jne","jnz","ja","jb","jbe","jae","jp","jnp","jg","jl","jng","jnl","jge","jle"};
constexpr std::size_t frames=259, outputs=branches.size()+2;
constexpr Words packet{0x01234567,0x89abcdef,0x7f812345,0xffc54321};
constexpr std::array<std::uint64_t,2> doubles{0x0123456789abcdefULL,0xfff8000000000042ULL};
constexpr Words arrayWords{0x3f123456,0xc1234567,0x456789ab,0x76543210};
std::uint32_t parent(std::size_t n) { return 0xa5aa55ccU^(static_cast<std::uint32_t>(n)*0x9e3779b9U); }
constexpr unsigned byteShift(unsigned reg) { return reg<4 ? 0 : 8; }
std::uint32_t byteParent(std::size_t n) { return (parent(n)&0xffff0000U)|(((n^0x5aU)&255U)<<8)|(n&255U); }
std::uint32_t selectedByte(std::uint32_t word,unsigned reg) { return (word>>byteShift(reg))&255U; }
std::vector<std::uint8_t> memoryBytes()
{
    std::vector<std::uint8_t> result(257);
    for (unsigned i=0;i<result.size();++i) result[i]=static_cast<std::uint8_t>(i*73+19);
    return result;
}
std::string seedFlags()
{
    return "mov edx,flag;cmp edx,3;jb cmpSeed;je overflow;cmp edx,4;je carryOverflow;"
        "movaps xmm6,nan;comiss xmm6,nan;jmp seeded;cmpSeed:cmp edx,1;jmp seeded;"
        "overflow:mov edx,0x7fffffff;add edx,1;jmp seeded;carryOverflow:mov edx,0x80000000;add edx,0x80000000;seeded:";
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
auto flagValues(std::size_t seed)
{
    constexpr std::array c{true,false,false,false,true,true},p{true,true,false,true,true,true},
        z{false,true,false,false,true,true},l{true,false,false,false,true,false};
    return std::array{z[seed],z[seed],!z[seed],!z[seed],!c[seed]&&!z[seed],c[seed],c[seed]||z[seed],!c[seed],
        p[seed],!p[seed],!l[seed]&&!z[seed],l[seed],l[seed]||z[seed],!l[seed],!l[seed],l[seed]||z[seed]};
}
struct RestoreEnvironment {
    std::fenv_t saved;
    RestoreEnvironment() { std::fegetenv(&saved); }
    ~RestoreEnvironment() { std::fesetenv(&saved); }
};

void byteLoadsCopiesAndFlags()
{
    RestoreEnvironment restore;
    const auto memoryData=memoryBytes();
    for (unsigned destination=0;destination<bytes.size();++destination) for (unsigned form=0;form<9+bytes.size();++form) {
        const auto destinationParent=destination%4,base=(destination+1)%4,index=(destination+2)%4;
        const bool copy=form>=9;
        const unsigned source=copy ? form-9 : 0,sourceParent=source%4;
        const auto extent=form<=3 ? 257U : 16U;
        std::string body=std::string("mov ")+parents[destinationParent]+",old;";
        std::string operand;
        if (form<=3) {
            body+="mov "+std::string(parents[base])+",data[0];mov "+parents[index]+",index;";
            operand=form==0 || form==3 ? "["+std::string(parents[base])+"+"+parents[index]+"]" :
                "["+std::string(parents[base])+(form==1 ? "+256]" : "+257]");
        } else if (form<=7) {
            body+="mov "+std::string(parents[index])+",index;";
            operand=(form==4 ? "value[" : form==5 ? "cells[" : form==6 ? "wide[" : "samples[")+std::string(parents[index])+"]";
        } else if (form==8) operand="0"; // Representative byte immediates are checked separately below.
        else {
            if (sourceParent!=destinationParent) body+="mov "+std::string(parents[sourceParent])+",value;";
            operand=bytes[source];
        }
        if (form==8) continue;
        body+="MOV "+std::string(bytes[destination])+","+operand+";movd xmm0,"+parents[destinationParent]+";movaps result,xmm0;";
        body+=copy ? "movd xmm0,"+std::string(parents[sourceParent])+";movaps unchanged,xmm0;" : "movaps xmm0,value;movaps unchanged,xmm0;";
        std::string code="streamin old,value,index,flag;memrefin data;floatarrayin samples[4];streamout ";
        for (unsigned i=0;i<branches.size();++i) code+="f"+std::to_string(i)+",";
        code+="result,unchanged;float zero=0,one=1,nan=0x7fc12345;int cells[4]={0x01234567,0x89abcdef,0x7f812345,0xffc54321};"
              "double wide[2]={0x0123456789abcdef,0xfff8000000000042};";
        code+=seedFlags()+body+readFlags();
        const auto program=compileDspCode(code,DspLanguage::Assembler);
        std::array<std::array<Mono4,frames>,4> input{};
        for (std::size_t n=0;n<frames;++n) {
            input[0][n].fill(std::bit_cast<float>(parent(n)));
            input[1][n]=std::bit_cast<Mono4>(copy ? Words{byteParent(n),111,222,333} : packet);
            input[2][n].fill(std::bit_cast<float>(n==257 ? UINT32_MAX : n==258 ? extent : static_cast<std::uint32_t>(n%extent)));
            input[3][n].fill(std::bit_cast<float>(static_cast<std::uint32_t>(n%6)));
        }
        for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);const auto block=graph.addDspCode(program);
            for (unsigned port=0;port<4;++port) graph.connectDspInput(graph.addInput(),block.id,port);
            auto runtime=graph.compileMono4(block.outputs);
            const openstone::green::Memory memory(form==3 ? std::vector<std::uint8_t>{} : memoryData);
            const auto array=std::bit_cast<std::array<float,4>>(arrayWords);
            check(runtime.setDspMemRefInput(block.id,4,memory) && runtime.setDspArrayInput(block.id,5,array),"Bind byte-load data");
            for (const std::size_t size : {1,3,7,31,64}) {
                runtime.reset();std::array<std::array<Mono4,frames>,outputs> output{};
                for (std::size_t offset=0;offset<frames;offset+=size) {
                    const auto count=std::min(size,frames-offset);
                    std::array<Mono4InputBuffer,4> in;std::array<Mono4OutputBuffer,outputs> out;
                    for (unsigned port=0;port<in.size();++port) in[port]=std::span{input[port]}.subspan(offset,count);
                    for (unsigned port=0;port<out.size();++port) out[port]=std::span{output[port]}.subspan(offset,count);
                    std::fesetround(FE_UPWARD);std::feclearexcept(FE_ALL_EXCEPT);std::feraiseexcept(FE_DIVBYZERO);
                    check(runtime.process(in,out),"Render byte MOV operands");
                    check(std::fegetround()==FE_UPWARD && std::fetestexcept(FE_ALL_EXCEPT)==FE_DIVBYZERO,"Byte moves preserve caller floating-point state");
                }
                for (std::size_t n=0;n<frames;++n) {
                    const auto position=std::bit_cast<std::uint32_t>(input[2][n][0]);
                    std::uint32_t byte=0;
                    if (copy) byte=selectedByte(sourceParent==destinationParent ? parent(n) : byteParent(n),source);
                    else if (form==1) byte=memoryData[256];
                    else if (form==0 && position<memoryData.size()) byte=memoryData[position];
                    else if (form>=4 && form<=7 && position<16) {
                        if (form==6) byte=static_cast<std::uint32_t>((doubles[position/8]>>((position%8)*8))&255);
                        else byte=((form==7 ? arrayWords : packet)[position/4]>>((position%4)*8))&255;
                    }
                    const auto expected=(parent(n)&~(255U<<byteShift(destination)))|(byte<<byteShift(destination));
                    check(std::bit_cast<Words>(output[branches.size()][n])==Words{expected,0,0,0},"Byte MOV replaces only the selected low/high byte and preserves every other parent bit");
                    const auto unchanged=copy ? Words{sourceParent==destinationParent ? expected : byteParent(n),0,0,0} : packet;
                    check(std::bit_cast<Words>(output[branches.size()+1][n])==unchanged,"All 64 byte-register copies read the original source and preserve other bits, including high/low aliases of one parent");
                    const auto flags=flagValues(n%6);
                    for (unsigned i=0;i<branches.size();++i) {
                        const float expected=flags[i] ? 1.F : 0.F;
                        check(output[i][n]==Mono4{expected,expected,expected,expected},"Byte MOV preserves arithmetic flags and all conditional branches");
                    }
                }
                check(memory.snapshotBytes()==(form==3 ? std::vector<std::uint8_t>{} : memoryData),"Byte loads never change source or neighboring bytes");
            }
        }
    }
}

void immediatesAliasesAndDiagnostics()
{
    for (unsigned reg=0;reg<bytes.size();++reg) {
        std::string body="streamin old;streamout a,b,c,d,e;mov "+std::string(parents[reg%4])+",old;";
        constexpr std::array values{"-128","-1","128","255","0"}, names{"a","b","c","d","e"};
        for (unsigned i=0;i<values.size();++i) body+="mov "+std::string(bytes[reg])+","+values[i]+";movd xmm0,"+parents[reg%4]+";movaps "+names[i]+",xmm0;";
        for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);const auto block=graph.addDspCode(compileDspCode(body,DspLanguage::Assembler));
            graph.connectDspInput(graph.addInput(),block.id,0);auto runtime=graph.compile(block.outputs);
            const std::array input{std::bit_cast<float>(0xabcdef12U)};std::array<std::array<float,1>,5> output{};
            check(runtime.process(std::array{InputBuffer{input}},std::array{OutputBuffer{output[0]},OutputBuffer{output[1]},OutputBuffer{output[2]},OutputBuffer{output[3]},OutputBuffer{output[4]}}),"Render byte immediates");
            const auto expected=reg<4 ? std::array<std::uint32_t,5>{0xabcdef80,0xabcdefff,0xabcdef80,0xabcdefff,0xabcdef00}
                : std::array<std::uint32_t,5>{0xabcd8012,0xabcdff12,0xabcd8012,0xabcdff12,0xabcd0012};
            for (unsigned i=0;i<expected.size();++i) check(std::bit_cast<std::uint32_t>(output[i][0])==expected[i],"Signed/unsigned byte immediates preserve every parent bit outside the selected byte");
        }
    }
    for (const auto operation : {"mov eax,data[0];mov ecx,3;mov cl,[eax+ecx];", "mov ecx,0x12345678;jmp 2;mov cl,255;", "mov ecx,0x12345678;mov edx,255;jmp 2;mov cl,dl;", "mov ecx,0x12345678;jmp 6;mov cl,value;"}) {
        const auto program=compileDspCode(std::string("streamin value;memrefin data;streamout result;")+operation+"movd xmm0,ecx;movaps result,xmm0;",DspLanguage::Assembler);
        for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);const auto block=graph.addDspCode(program);graph.connectDspInput(graph.addInput(),block.id,0);auto runtime=graph.compile(block.outputs);
            check(runtime.setDspMemRefInput(block.id,1,openstone::green::Memory({5,6,7,222})),"Bind aliased-index byte memory");
            const std::array input{0.F};std::array<float,1> output{};
            check(runtime.process(std::array{InputBuffer{input}},std::array{OutputBuffer{output}}) && std::bit_cast<std::uint32_t>(output[0])==
                (std::string_view(operation).starts_with("mov eax") ? 222U : 0x12345678U),"Byte load uses the original ECX index; numeric jumps skip known byte MOV widths");
        }
    }
    // EAX remains unknown; ECX/CL/CH now have a verified incoming engine value.
    for (auto body : {"mov al,1;", "mov al,al;", "xor ecx,ecx;mov cl,al;", "xor ecx,ecx;mov cl,256;", "xor ecx,ecx;mov cl,-129;",
        "xor ecx,ecx;mov cl,ecx;", "xor ecx,ecx;mov ecx,cl;", "xor al,al;",
        "xor ecx,ecx;movd xmm0,cl;", "xor ecx,ecx;idiv cl;", "xor ecx,ecx;xchg cl,ecx;", "xor ecx,ecx;mov cl,[cl];",
        "mov ecx,data[0];mov cl,1;", "mov eax,data[0];xor ecx,ecx;mov cl,al;", "xor ecx,ecx;mov cl,data[0];",
        "mov ecx,1;mov cl,2;jz done;done:", "mov ecx,1;mov cl,2;jb done;done:", "mov ecx,1;mov cl,2;jp done;done:"}) {
        bool rejected=false;
        try { static_cast<void>(compileDspCode(std::string("memrefin data;streamout out;")+body,DspLanguage::Assembler)); }
        catch (const DspCompileError& e) { rejected=e.line>0&&e.column>0; }
        check(rejected,"Byte MOV enforces initialized numeric parents, operand widths and explicit unsupported operations");
    }
}

std::vector<std::uint8_t> rawBytes(Words words)
{
    std::vector<std::uint8_t> result(16);
    for (unsigned i=0;i<16;++i) result[i]=static_cast<std::uint8_t>(words[i/4]>>((i%4)*8));
    return result;
}
Words rawWords(const std::vector<std::uint8_t>& data)
{
    Words result{};
    for (unsigned word=0;word<4;++word) if (word*4+4<=data.size())
        for (unsigned byte=0;byte<4;++byte) result[word]|=std::uint32_t{data[word*4+byte]}<<(byte*8);
    return result;
}

void byteStoresAndNeighbors()
{
    RestoreEnvironment restore;
    constexpr std::size_t count=263,ports=branches.size()+3;
    constexpr std::array scales{1U,2U,4U,8U};
    constexpr std::array<std::int32_t,4> displacements{0,1,3,-1};
    for (unsigned reg=0;reg<bytes.size();++reg) for (unsigned storage=0;storage<8;++storage) for (unsigned form=0;form<4;++form) {
        const bool external=storage<4;
        const auto base=(reg+1)%4,index=(reg+2)%4;
        const std::string expression=std::string(parents[index])+(form ? "*"+std::to_string(scales[form]) : "")+
            (form==1 ? "+1" : form==2 ? "+3" : form==3 ? "-1" : "");
        const std::string target=external ? "["+std::string(parents[base])+"+"+expression+"]" :
            (storage==4 ? "cells[" : storage==5 ? "wide[" : storage==6 ? "cell[" : "result[")+expression+"]";
        std::string code="streamin old,index,flag;memrefin data;streamout ";
        for (unsigned i=0;i<branches.size();++i) code+="f"+std::to_string(i)+",";
        code+="result,unchanged,readback;float zero=0,one=1,nan=0x7fc12345,cell;"
              "float pattern[4]={0x01234567,0x89abcdef,0x7f812345,0xffc54321};"
              "int cells[4]={0x01234567,0x89abcdef,0x7f812345,0xffc54321};"
              "double wide[2]={0x0123456789abcdef,0xfff8000000000042};";
        code+=seedFlags()+"mov "+parents[reg%4]+",old;mov "+parents[base]+",data[0];mov "+parents[index]+",index;";
        if (storage>=6) code+=std::string("movaps xmm1,pattern[0];movaps ")+(storage==6 ? "cell" : "result")+",xmm1;";
        code+="MOV "+target+","+bytes[reg]+";movd xmm0,"+parents[reg%4]+";movaps unchanged,xmm0;";
        if (storage!=7) code+="movaps xmm0,"+std::string(external ? "["+std::string(parents[base])+"]" :
            storage==4 ? "cells[0]" : storage==5 ? "wide[0]" : "cell")+";movaps result,xmm0;";
        code+="mov "+std::string(parents[reg%4])+",0;mov "+bytes[reg]+","+target+";movd xmm0,"+parents[reg%4]+";movaps readback,xmm0;"+readFlags();
        const auto program=compileDspCode(code,DspLanguage::Assembler);
        std::array<std::array<Mono4,count>,3> input{};
        for (unsigned n=0;n<count;++n) {
            const std::uint32_t position=n<256 ? n%17 : n==256 ? UINT32_MAX : n==257 ? 17U : n==258 ? 16U :
                n==259 ? 0x80000000U : n==260 ? 0x40000000U : n==261 ? 0x7fffffffU : 0;
            input[0][n]=std::bit_cast<Mono4>(Words{byteParent(n),0x7f812345,0xffc12345,0x80000001});
            input[1][n].fill(std::bit_cast<float>(position));input[2][n].fill(std::bit_cast<float>(n%6));
        }
        for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);const auto block=graph.addDspCode(program);
            for (unsigned port=0;port<3;++port) graph.connectDspInput(graph.addInput(),block.id,port);
            auto runtime=graph.compileMono4(block.outputs);
            for (const std::size_t size : {1,3,7,31,64}) {
                const auto length=storage==2 ? 0U : storage==3 ? 1U : external ? 17U : 16U;
                const auto start=storage==1 ? 5U : 0U;
                std::vector<std::uint8_t> backing(length+(storage==1 ? 12U : 0U));
                for (unsigned i=0;i<backing.size();++i) backing[i]=static_cast<std::uint8_t>(i*73+19);
                const openstone::green::Memory memory(backing);
                const auto view=storage==1 ? memory.view(start,length,1) : memory;
                check(runtime.setDspMemRefInput(block.id,3,view),"Bind byte store memory");runtime.reset();
                auto expected=external ? std::vector<std::uint8_t>(backing.begin()+start,backing.begin()+start+length) :
                    storage==5 ? rawBytes(Words{0x89abcdef,0x01234567,0x42,0xfff80000}) : rawBytes(packet);
                for (std::size_t offset=0;offset<count;offset+=size) {
                    const auto take=std::min(size,count-offset);
                    std::array<std::vector<Mono4>,ports> output;
                    std::array<Mono4InputBuffer,3> in;std::array<Mono4OutputBuffer,ports> out;
                    for (unsigned port=0;port<3;++port) in[port]=std::span{input[port]}.subspan(offset,take);
                    for (unsigned port=0;port<ports;++port) {output[port].resize(take);out[port]=output[port];}
                    std::fesetround(FE_UPWARD);std::feclearexcept(FE_ALL_EXCEPT);std::feraiseexcept(FE_DIVBYZERO);
                    check(runtime.process(in,out),"Render indexed byte stores");
                    check(std::fegetround()==FE_UPWARD && std::fetestexcept(FE_ALL_EXCEPT)==FE_DIVBYZERO,"Byte stores preserve caller floating-point state");
                    for (unsigned i=0;i<take;++i) {
                        const auto n=offset+i;
                        if (storage>=6) expected=rawBytes(packet);
                        const auto indexBits=std::bit_cast<std::uint32_t>(input[1][n][0]);
                        const auto address=std::bit_cast<std::int32_t>(indexBits*scales[form]+static_cast<std::uint32_t>(displacements[form]));
                        const bool valid=address>=0 && static_cast<std::uint32_t>(address)<expected.size();
                        if (valid) expected[address]=static_cast<std::uint8_t>(selectedByte(byteParent(n),reg));
                        check(std::bit_cast<Words>(output[branches.size()][i])==rawWords(expected),"Store replaces just the addressed byte and preserves every adjacent raw word");
                        check(std::bit_cast<Words>(output[branches.size()+1][i])==Words{byteParent(n),0,0,0},"Byte store preserves the complete source parent register");
                        check(std::bit_cast<Words>(output[branches.size()+2][i])==Words{valid ? std::uint32_t{expected[address]}<<byteShift(reg) : 0U,0,0,0},"Immediate byte readback sees the stored byte at the same effective address");
                        const auto flags=flagValues(n%6);
                        for (unsigned flag=0;flag<branches.size();++flag) {
                            const float value=flags[flag] ? 1.F : 0.F;
                            check(output[flag][i]==Mono4{value,value,value,value},"Byte stores preserve arithmetic flags and every conditional branch");
                        }
                    }
                    if (external) {
                        std::copy(expected.begin(),expected.end(),backing.begin()+start);
                        check(memory.snapshotBytes()==backing,"All neighboring bytes, including guards outside an unaligned Mem view, survive each rendered buffer");
                    }
                }
            }
        }
    }
}

void byteStoreAliasesAndTargets()
{
    // The source parent can also provide the address index; a store must leave it unchanged.
    for (unsigned reg=0;reg<bytes.size();++reg) {
        const auto base=(reg+1)%4;
        const auto code="memrefin data;streamout out,unchanged;mov "+std::string(parents[base])+",data[0];mov "+parents[reg%4]+",0x203;mov ["+
            parents[base]+"+"+parents[reg%4]+"*4-2047],"+bytes[reg]+";movaps xmm0,["+parents[base]+"];movaps out,xmm0;movd xmm0,"+parents[reg%4]+";movaps unchanged,xmm0;";
        for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);const auto block=graph.addDspCode(compileDspCode(code,DspLanguage::Assembler));auto runtime=graph.compileMono4(block.outputs);
            const auto initial=rawBytes(packet);const openstone::green::Memory memory(initial);check(runtime.setDspMemRefInput(block.id,0,memory),"Bind source/index alias Mem");
            std::array<Mono4,1> out{},unchanged{};check(runtime.process({},std::array{Mono4OutputBuffer{out},Mono4OutputBuffer{unchanged}}),"Render source/index alias");
            auto expected=initial;expected[13]=reg<4 ? 3 : 2;
            check(memory.snapshotBytes()==expected && std::bit_cast<Words>(out[0])==rawWords(expected) && std::bit_cast<Words>(unchanged[0])==Words{0x203,0,0,0},"Aliased source/index retains its original full value and address");
        }
    }
    for (bool aliasPort : {false,true}) for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        const std::string code="streamin touch;memrefin data,alias;streamout out;float zero=0,one=1;"
            "movaps xmm0,zero;movaps out,xmm0;mov eax,data[0];mov edx,0;mov ebx,routine;mov [eax+edx*4],ebx;mov ecx,0;"+
            std::string(aliasPort ? "mov ebx,alias[0];" : "mov ebx,data[0];")+
            "mov ecx,touch;mov edx,0;mov [ebx+ecx],dl;mov edx,0;call [eax+edx*4];jmp end;routine:movaps xmm0,one;movaps out,xmm0;ret;end:";
        Graph graph(backend);const auto block=graph.addDspCode(compileDspCode(code,DspLanguage::Assembler));graph.connectDspInput(graph.addInput(),block.id,0);auto runtime=graph.compile(block.outputs);
        for (const std::size_t size : {1,3,7,31,64}) {
            runtime.reset();const openstone::green::Memory memory(std::vector<std::uint8_t>(20));
            check(runtime.setDspMemRefInput(block.id,1,memory) && runtime.setDspMemRefInput(block.id,2,memory.view(1,19,1)),"Bind overlapping target table views");
            constexpr std::array<std::int32_t,10> positions{-1,0,1,2,3,4,7,18,19,20};
            std::array<float,positions.size()> input{},output{};
            for (unsigned n=0;n<positions.size();++n) input[n]=std::bit_cast<float>(static_cast<std::uint32_t>(positions[n]));
            for (std::size_t offset=0;offset<input.size();offset+=size) {
                const auto take=std::min(size,input.size()-offset);
                check(runtime.process(std::array{InputBuffer{std::span{input}.subspan(offset,take)}},std::array{OutputBuffer{std::span{output}.subspan(offset,take)}}),"Render byte writes to target tables");
            }
            for (unsigned n=0;n<positions.size();++n) {
                const auto position=positions[n]+(aliasPort ? 1 : 0);
                const bool valid=positions[n]>=0 && position<20;
                check(output[n]==(valid && position<4 ? 0.F : 1.F),"Touching any target byte invalidates its callable provenance, even if the bits stay unchanged, including writes through another view");
            }
        }
    }
    for (auto body : {"mov cells[0],ebx;mov cells[0],cl;mov eax,cells[0];call eax;", "mov cells[1],ebx;mov edx,7;mov cells[edx],cl;mov eax,cells[1];call eax;",
        "mov [eax],ebx;mov [eax+3],cl;mov ebx,[eax];call ebx;", "mov data[0],cl;", "mov waves[0],cl;", "mov samples[0],cl;",
        "mov [eax],dl;", "mov ecx,data[0];mov [eax],cl;", "mov ecx,routine;mov [eax],cl;",
        "mov eax,1;mov eax,cl;", "mov xmm0,cl;"}) {
        bool rejected=false;
        try {static_cast<void>(compileDspCode(std::string("streamin input;memrefin data;memarrayrefin waves;floatarrayin samples[4];streamout out;int cells[4];mov eax,data[0];mov ebx,routine;mov ecx,0;")+body+"jmp end;routine:ret;end:",DspLanguage::Assembler));}
        catch (const DspCompileError& e) {rejected=e.line>0&&e.column>0;}
        check(rejected,"Byte stores reject stale static targets, external-array/descriptor writes, uninitialized/opaque byte sources and invalid widths");
    }
}

void byteStoreMemoryKindsAndControlFlow()
{
    for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        for (unsigned field=4;field<8;++field) {
            const auto code="memarrayrefin waves;streamout before,after;mov eax,waves["+std::to_string(field)+
                "];movd xmm0,[eax];movaps before,xmm0;mov ecx,0;mov ebx,255;mov [eax+ecx],bl;movd xmm0,[eax];movaps after,xmm0;";
            Graph graph(backend);const auto block=graph.addDspCode(compileDspCode(code,DspLanguage::Assembler));auto runtime=graph.compileMono4(block.outputs);
            const auto original=memoryBytes();const openstone::green::Memory memory(original);
            check(runtime.setDspMemArrayInput(block.id,0,openstone::green::MemoryArray({memory})),"Bind read-only MemArray tables");
            std::array<Mono4,1> before{},after{};
            check(runtime.process({},std::array{Mono4OutputBuffer{before},Mono4OutputBuffer{after}}) && std::bit_cast<Words>(before[0])==std::bit_cast<Words>(after[0]) &&
                memory.snapshotBytes()==original,"Byte writes cannot modify MemArray descriptor tables or their wave storage");
        }
        const auto program=compileDspCode("memarrayrefin waves;streamin index,value;streamout out;mov eax,waves[5];mov eax,[eax];mov ecx,index;mov ebx,value;mov [eax+ecx],bl;mov ebx,0;mov bl,[eax+ecx];movd xmm0,ebx;movaps out,xmm0;",DspLanguage::Assembler);
        Graph graph(backend);const auto block=graph.addDspCode(program);
        graph.connectDspInput(graph.addInput(),block.id,1);graph.connectDspInput(graph.addInput(),block.id,2);auto runtime=graph.compile(block.outputs);
        constexpr std::size_t count=67;std::array<float,count> positions{},values{},output{};
        for (unsigned n=0;n<count;++n) {positions[n]=std::bit_cast<float>(static_cast<std::uint32_t>(n%19-1));values[n]=std::bit_cast<float>(parent(n));}
        for (const std::size_t size : {1,3,7,31,64}) {
            runtime.reset();auto expected=memoryBytes();expected.resize(17);const openstone::green::Memory memory(expected);
            check(runtime.setDspMemArrayInput(block.id,0,openstone::green::MemoryArray({memory})),"Bind a loaded wave for byte stores");
            for (std::size_t offset=0;offset<count;offset+=size) {
                const auto take=std::min(size,count-offset);
                check(runtime.process(std::array{InputBuffer{std::span{positions}.subspan(offset,take)},InputBuffer{std::span{values}.subspan(offset,take)}},
                    std::array{OutputBuffer{std::span{output}.subspan(offset,take)}}),"Render MemArray wave byte writes");
                for (unsigned n=offset;n<offset+take;++n) {
                    const auto index=std::bit_cast<std::int32_t>(positions[n]);const bool valid=index>=0 && static_cast<unsigned>(index)<expected.size();
                    if (valid) expected[index]=static_cast<std::uint8_t>(parent(n));
                    check(std::bit_cast<std::uint32_t>(output[n])==(valid ? expected[index] : 0U),"Wave byte readback uses the selected sample address and bounds");
                }
                check(memory.snapshotBytes()==expected,"Wave byte stores preserve all neighboring loaded-wave bytes");
            }
        }
        for (const auto operation : {"mov cells[1],eax;mov cells[0],cl;mov eax,cells[1];call eax;",
            "mov [ebx+4],eax;mov [ebx+3],cl;mov eax,[ebx+4];call eax;",
            "jmp 6;mov cells[0],cl;mov eax,cells[0];cmp eax,0x01234567;jne end;call routine;"}) {
            const std::string code="memrefin data;streamout out;int cells[2]={0x01234567,0};float zero=0,one=1;movaps xmm0,zero;movaps out,xmm0;mov eax,routine;mov ebx,data[0];mov ecx,255;"+
                std::string(operation)+"jmp end;routine:movaps xmm0,one;movaps out,xmm0;ret;end:";
            Graph calls(backend);const auto routine=calls.addDspCode(compileDspCode(code,DspLanguage::Assembler));auto runner=calls.compile(routine.outputs);
            check(runner.setDspMemRefInput(routine.id,0,openstone::green::Memory(std::vector<std::uint8_t>(8))),"Bind neighbor-target test table");
            std::array<float,1> result{};check(runner.process({},std::array{OutputBuffer{result}}) && result[0]==1.F,"Byte writes preserve adjacent callable targets and known numeric skips use the byte-store width");
        }
    }
}

void highByteAliasesWidthsAndDiagnostics()
{
    const auto verify=[](const std::string& body,std::uint32_t expected,const std::vector<std::uint8_t>& expectedMemory) {
        const auto program=compileDspCode("memrefin data;streamout out;int cells[2]={0x01234567,0};"+body,DspLanguage::Assembler);
        for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);const auto block=graph.addDspCode(program);auto runtime=graph.compileMono4(block.outputs);
            for (const std::size_t size : {1,3,7,31,64}) {
                runtime.reset();const openstone::green::Memory memory({5,6,7,222});
                check(runtime.setDspMemRefInput(block.id,0,memory),"Bind byte-alias and branch-width memory");
                std::array<Mono4,67> output{};
                for (std::size_t offset=0;offset<output.size();offset+=size) {
                    const auto count=std::min(size,output.size()-offset);
                    check(runtime.process({},std::array{Mono4OutputBuffer{std::span{output}.subspan(offset,count)}}),"Render high-byte aliases and numeric skips");
                }
                for (const auto& value : output) check(std::bit_cast<Words>(value)==Words{expected,0,0,0},"High-byte loads use the original address; numeric skips preserve byte register/known memory instruction widths");
                check(memory.snapshotBytes()==expectedMemory,"High-byte stores preserve neighboring bytes with 64-bit address-register spellings");
            }
        }
    };
    for (unsigned reg=0;reg<bytes.size();++reg) {
        const auto base=(reg+1)%4;const auto parentName=std::string(parents[reg%4]);
        verify("mov "+std::string(parents[base])+",data[0];mov "+parentName+",3;mov "+bytes[reg]+",["+parents[base]+"+"+parentName+"];movd xmm0,"+parentName+";movaps out,xmm0;",
            reg<4 ? 222U : 0xde03U,{5,6,7,222});
        if (reg<4) continue;
        for (const auto operation : {"jmp 2;mov "+std::string(bytes[reg])+",255;", "jmp 2;mov "+std::string(bytes[reg])+","+bytes[reg-4]+";",
            "jmp 6;mov "+std::string(bytes[reg])+",cells[0];"}) {
            verify("mov "+parentName+",0x12345678;"+operation+"movd xmm0,"+parentName+";movaps out,xmm0;",0x12345678U,{5,6,7,222});
        }
        verify("mov "+parentName+",0x12345678;jmp 6;mov cells[0],"+bytes[reg]+";mov "+parentName+",cells[0];movd xmm0,"+parentName+";movaps out,xmm0;",0x01234567U,{5,6,7,222});
        for (const auto body : {"push eax;pop "+parentName+";mov "+std::string(bytes[reg])+",1;", "mov "+parentName+",data[0];mov "+bytes[reg]+",0;",
            "mov "+parentName+",routine;mov "+bytes[reg]+",0;", "mov "+parentName+",0;mov "+bytes[reg]+","+parentName+";",
            "mov "+parentName+",0;mov "+parentName+","+bytes[reg]+";",
            "mov "+parentName+",0;xchg "+bytes[reg]+","+parentName+";", "mov "+parentName+",0;idiv "+bytes[reg]+",1;",
            "mov "+parentName+",0;movd xmm0,"+bytes[reg]+";", "mov "+parentName+",0;mov "+bytes[reg]+",["+bytes[reg]+"];",
            "mov "+parentName+",0;mov "+bytes[reg]+",0;jp end;"}) {
            bool rejected=false;
            try {static_cast<void>(compileDspCode("memrefin data;streamout out;"+body+"jmp end;routine:ret;end:",DspLanguage::Assembler));}
            catch (const DspCompileError& e) {rejected=e.line>0&&e.column>0;}
            check(rejected,"High-byte operands reject unwritten/opaque parents, mixed widths, unsupported instructions, byte address indices and missing flag producers");
        }
    }
    verify("mov rbx,data[0];mov eax,0x12345678;MOV AH,[rbx+3];mov [rbx+2],AH;movd xmm0,eax;movaps out,xmm0;",0x1234de78U,{5,6,222,222});
    const auto words=dspLanguageWords(DspLanguage::Assembler);
    for (const auto name : bytes) check(std::ranges::any_of(words,[=](const auto& word) {return word.text==name && word.kind==DspWordKind::Register;}),"All low/high byte aliases appear in syntax highlighting and completion");
}

void recoveredMemReader()
{
    const auto path=std::filesystem::path(OPENSTONE_TEST_FIXTURES)/"flowstone/mem-reader-byte.asm";
    std::ifstream file(path,std::ios::binary);check(file.good(),"Read unchanged recovered Mem reader");
    const std::string source{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
    // This is an explicit caller context. The recovered source itself remains byte-for-byte unchanged.
    const auto program=compileDspCode("mov edx,0;mov ecx,0;\n"+source,DspLanguage::Assembler);
    constexpr std::size_t count=131;
    constexpr std::array values{-11.F,0.F,0.5F,1.F,1.5F,2.5F,128.F,128.5F,255.F,256.F,999.F};
    std::array<Mono4,count> input{};
    for (unsigned n=0;n<count;++n) for (unsigned lane=0;lane<4;++lane) input[n][lane]=values[(n+lane*3)%values.size()];
    const auto data=memoryBytes();
    for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) for (unsigned mode=0;mode<3;++mode) {
        if (!dspBackendAvailable(backend)) continue;
        const auto memory=mode==0 ? openstone::green::Memory{} : openstone::green::Memory(data,static_cast<int>(mode));
        Graph graph(backend);const auto block=graph.addDspCode(program);graph.connectDspInput(graph.addInput(),block.id,1);auto runtime=graph.compileMono4(block.outputs);
        check(runtime.setDspMemRefInput(block.id,0,memory),"Bind recovered Mem reader");
        for (const std::size_t size : {1,3,7,31,64}) {
            runtime.reset();std::array<Mono4,count> output{};
            for (std::size_t offset=0;offset<count;offset+=size) {
                const auto take=std::min(size,count-offset);
                check(runtime.process(std::array{Mono4InputBuffer{std::span{input}.subspan(offset,take)}},std::array{Mono4OutputBuffer{std::span{output}.subspan(offset,take)}}),"Render recovered byte Mem reader with explicit incoming registers");
                check(runtime.dspFault(block.id).kind==DspFaultKind::None,"The original empty-Mem branch prevents division by zero");
            }
            for (unsigned n=0;n<count;++n) for (unsigned lane=0;lane<4;++lane) {
                float expected=0;
                if (mode) {
                    const auto value=std::clamp(input[n][lane],0.F,static_cast<float>(data.size()/mode-1));
                    auto index=static_cast<unsigned>(value);const auto fraction=value-static_cast<float>(index);
                    if (fraction>0.5F || (fraction==0.5F && (index&1))) ++index;
                    expected=data[index];
                }
                check(output[n][lane]==expected,"The recovered reader preserves byte indices, rounding/clamping and mono/stereo channel behavior");
            }
            check(memory.snapshotBytes()==(mode ? data : std::vector<std::uint8_t>{}),"Recovered reader never changes loaded data");
        }
    }
}
}
int main()
{
    try { byteLoadsCopiesAndFlags();immediatesAliasesAndDiagnostics();byteStoresAndNeighbors();byteStoreAliasesAndTargets();byteStoreMemoryKindsAndControlFlow();highByteAliasesWidthsAndDiagnostics();openstone::tests::withReferenceSources("recoveredMemReader", [&] { recoveredMemReader(); }); }
    catch (const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
