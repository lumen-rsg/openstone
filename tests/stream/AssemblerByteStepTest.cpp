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

namespace {
using namespace openstone::stream;
using Words=std::array<std::uint32_t,4>;
constexpr std::array parents{"eax","ebx","ecx","edx"};
constexpr std::array aliases{"al","bl","cl","dl","ah","bh","ch","dh"};
constexpr std::array branches{"je","jz","jne","jnz","ja","jb","jbe","jae","jp","jnp","jg","jl","jng","jnl","jge","jle"};
constexpr std::size_t ports=branches.size()+2;
void check(bool value,std::string_view message) {if (!value) throw std::runtime_error(std::string(message));}
struct Result {unsigned value;bool carry,parity,zero,less,overflow;};
Result reference(bool increment,unsigned value,bool carry)
{
    const auto result=static_cast<unsigned>(static_cast<int>(value)+(increment ? 1 : -1))&255U;
    const int signedValue=value<128 ? static_cast<int>(value) : static_cast<int>(value)-256;
    const auto mathematical=signedValue+(increment ? 1 : -1);
    const bool overflow=mathematical< -128 || mathematical>127;
    return {result,carry,(std::popcount(result)&1)==0,result==0,bool(result&128U)!=overflow,overflow};
}
auto branchValues(Result r)
{
    return std::array{r.zero,r.zero,!r.zero,!r.zero,!r.carry&&!r.zero,r.carry,r.carry||r.zero,!r.carry,
        r.parity,!r.parity,!r.less&&!r.zero,r.less,r.less||r.zero,!r.less,!r.less,r.less||r.zero};
}
std::string header()
{
    std::string code="streamin value,carry;memrefin data;streamout ";
    for (unsigned i=0;i<branches.size();++i) code+="f"+std::to_string(i)+",";
    return code+"result,other;float zero=0,one=1,nan=0x7fc12345;";
}
std::string seedFlags(unsigned mode)
{
    std::string code="mov edx,carry;cmp edx,0;je withoutCarry;";
    if (mode==0) code+="mov edx,0x80000000;add edx,0x80000000;jmp seeded;withoutCarry:mov edx,0x7fffffff;add edx,1;";
    if (mode==1) code+="mov edx,255;add dl,1;jmp seeded;withoutCarry:mov edx,127;add dl,1;";
    if (mode==2) code+="mov edx,0;sub dl,1;jmp seeded;withoutCarry:mov edx,128;sub dl,1;";
    if (mode==3) code+="movaps xmm8,nan;comiss xmm8,nan;jmp seeded;withoutCarry:movaps xmm8,one;comiss xmm8,zero;";
    return code+"seeded:";
}
std::string readFlags()
{
    std::string code;
    for (unsigned i=0;i<branches.size();++i) {
        const auto id=std::to_string(i);
        code+="movaps xmm7,zero;movaps f"+id+",xmm7;"+branches[i]+" yes"+id+";jmp next"+id+
            ";yes"+id+":movaps xmm7,one;movaps f"+id+",xmm7;next"+id+":";
    }
    return code;
}
std::string capture(std::string_view reg,std::string_view output)
{
    return "movd xmm0,"+std::string(reg)+";movaps "+std::string(output)+",xmm0;";
}
unsigned shift(unsigned alias) {return alias<4 ? 0 : 8;}
std::uint32_t replace(std::uint32_t word,unsigned alias,unsigned value) {return (word&~(255U<<shift(alias)))|(value<<shift(alias));}
struct Environment {std::fenv_t saved;Environment(){std::fegetenv(&saved);}~Environment(){std::fesetenv(&saved);}};
using Buffers=std::array<std::array<Mono4,64>,ports>;
void checkFlags(const Buffers& output,unsigned i,Result result,const std::string& context)
{
    const auto flags=branchValues(result);
    for (unsigned port=0;port<branches.size();++port) {
        const float value=flags[port] ? 1.F : 0.F;
        if (output[port][i]!=Mono4{value,value,value,value}) throw std::runtime_error(context+" branch "+branches[port]);
    }
}
void sweep(bool increment,unsigned alias,unsigned mode,std::string_view flow={})
{
    const auto operation=std::string(increment ? "INC " : "DEC ")+aliases[alias]+";";
    const auto reverse=std::string(increment ? "dec " : "inc ")+aliases[alias]+";";
    const auto other=(alias+1)%4;
    std::string code=header()+seedFlags(mode)+"mov "+parents[alias%4]+",value;mov "+parents[other]+",0x59ad8c20;";
    if (flow=="call") code+="call calculate;";
    else if (flow=="join" || flow=="mixed") code+="jz alternate;"+operation+"jmp joined;alternate:"+(flow=="mixed" ? reverse : operation)+"joined:";
    else code+=operation+(flow=="pair" ? reverse : "");
    code+=readFlags()+capture(parents[alias%4],"result")+capture(parents[other],"other");
    if (flow=="call") code+="jmp end;calculate:"+operation+"ret;end:";
    const auto program=compileDspCode(code,DspLanguage::Assembler);
    Environment environment;
    for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);const auto block=graph.addDspCode(program);
        graph.connectDspInput(graph.addInput(),block.id,0);graph.connectDspInput(graph.addInput(),block.id,1);
        auto runtime=graph.compileMono4(block.outputs);
        for (const std::size_t size : {1,3,7,31,64}) {
            runtime.reset();std::array<std::array<Mono4,64>,2> input{};Buffers output{};
            std::fesetround(FE_UPWARD);std::feclearexcept(FE_ALL_EXCEPT);std::feraiseexcept(FE_DIVBYZERO);
            for (std::size_t offset=0;offset<512;offset+=size) {
                const auto count=std::min(size,512-offset);
                for (unsigned i=0;i<count;++i) {
                    const auto n=static_cast<unsigned>(offset+i),value=n/2;
                    const auto parent=replace(0xa5aa55ccU^(value*0x9e3779b9U),alias,value);
                    input[0][i]=std::bit_cast<Mono4>(Words{parent,0x7f812345,0x80000001,0xffc12345});
                    input[1][i].fill(std::bit_cast<float>(n&1U));
                }
                std::array<Mono4InputBuffer,2> in;std::array<Mono4OutputBuffer,ports> out;
                for (unsigned port=0;port<2;++port) in[port]=std::span{input[port]}.first(count);
                for (unsigned port=0;port<ports;++port) out[port]=std::span{output[port]}.first(count);
                check(runtime.process(in,out),"Render byte INC/DEC");
                for (unsigned i=0;i<count;++i) {
                    const auto n=static_cast<unsigned>(offset+i);const bool carry=n&1U;
                    auto expected=reference(flow=="mixed" && carry ? !increment : increment,n/2,carry);
                    if (flow=="pair") expected=reference(!increment,expected.value,carry);
                    checkFlags(output,i,expected,operation+" mode "+std::to_string(mode)+" "+std::string(flow)+" sample "+std::to_string(n));
                    const auto before=std::bit_cast<Words>(input[0][i])[0];
                    check(std::bit_cast<Words>(output[branches.size()][i])==Words{replace(before,alias,expected.value),0,0,0},"Byte steps preserve every parent bit outside the selected byte");
                    check(std::bit_cast<Words>(output[branches.size()+1][i])==Words{0x59ad8c20,0,0,0},"Byte steps do not change another parent register");
                }
            }
            check(std::fegetround()==FE_UPWARD && std::fetestexcept(FE_ALL_EXCEPT)==FE_DIVBYZERO,"Byte steps preserve caller floating-point state");
        }
    }
}
void exhaustiveByteSteps()
{
    for (bool increment : {false,true}) for (unsigned alias=0;alias<8;++alias) {
        for (unsigned mode=0;mode<4;++mode) sweep(increment,alias,mode);
        for (auto flow : {"call","join","pair"}) sweep(increment,alias,0,flow);
    }
    for (unsigned alias=0;alias<8;++alias) sweep(true,alias,0,"mixed");
    for (bool carry : {false,true}) {
        const auto incOverflow=reference(true,127,carry),incWrap=reference(true,255,carry),decOverflow=reference(false,128,carry),decWrap=reference(false,0,carry);
        check(incOverflow.value==128 && incOverflow.overflow && !incOverflow.less && incOverflow.carry==carry &&
            incWrap.value==0 && !incWrap.overflow && incWrap.zero && incWrap.parity && incWrap.carry==carry &&
            decOverflow.value==127 && decOverflow.overflow && decOverflow.less && decOverflow.carry==carry &&
            decWrap.value==255 && !decWrap.overflow && decWrap.less && decWrap.parity && decWrap.carry==carry,
            "Literal byte boundaries distinguish signed overflow, wraparound and preserved carry");
    }
}
void literalsAndWidths()
{
    for (bool increment : {false,true}) for (unsigned alias=0;alias<8;++alias) for (bool carry : {false,true}) for (bool loop : {false,true}) {
        const auto byteValue=loop ? increment ? 253U : 3U : increment ? 127U : 128U;
        const auto original=replace(0xa5c35a69,alias,byteValue);
        std::string code=header()+"mov edx,0;cmp edx,"+(carry ? std::string("1;") : std::string("0;"))+"mov "+parents[alias%4]+","+std::to_string(original)+";";
        const auto operation=std::string(increment ? "inc " : "dec ")+aliases[alias]+";";
        if (loop) code+="again:"+operation+"jnz again;";
        else code+="jmp 2;"+operation;
        code+=readFlags()+capture(parents[alias%4],"result");
        const auto program=compileDspCode(code,DspLanguage::Assembler);
        for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);const auto block=graph.addDspCode(program);auto runtime=graph.compileMono4(block.outputs);
            for (const std::size_t size : {1,3,7,31,64}) {
                runtime.reset();Buffers output{};
                for (std::size_t offset=0;offset<67;offset+=size) {
                    const auto count=std::min(size,67-offset);std::array<Mono4OutputBuffer,ports> out;
                    for (unsigned port=0;port<ports;++port) out[port]=std::span{output[port]}.first(count);
                    check(runtime.process({},out),"Render byte step loops and numeric skips");
                    for (unsigned i=0;i<count;++i) {
                        const Result expected=loop ? reference(increment,increment ? 255 : 1,carry) : Result{0,carry,true,!carry,carry,false};
                        checkFlags(output,i,expected,"Byte step loop or skip");
                        check(std::bit_cast<Words>(output[branches.size()][i])==Words{loop ? replace(original,alias,0) : original,0,0,0},"Byte loop uses ZF without touching carry, and a numeric jump skips its two-byte instruction");
                    }
                }
            }
        }
    }
}
void carryAvailabilityAndDiagnostics()
{
    for (bool increment : {false,true}) for (unsigned alias=0;alias<8;++alias) {
        const auto operation=std::string(increment ? "inc " : "dec ")+aliases[alias]+";";
        const auto initialize="mov "+std::string(parents[alias%4])+",0;";
        for (auto branch : {"jb","ja","jae","jbe"}) for (bool oldSlot : {false,true}) {
            const auto old=oldSlot ? std::string("mov ebx,0;cmp ebx,1;mov ebx,data[0];add ebx,1;") : "";
            bool rejected=false;
            try {static_cast<void>(compileDspCode(header()+old+initialize+operation+branch+" end;end:",DspLanguage::Assembler));}
            catch (const DspCompileError& e) {rejected=e.line>0 && e.column>0;}
            check(rejected,"INC/DEC cannot define carry, including after a former flag slot has become invalid");
        }
        for (auto body : {operation,"mov "+std::string(parents[alias%4])+",data[0];"+operation,
            "mov "+std::string(parents[alias%4])+",routine;"+operation,initialize+std::string(increment ? "inc " : "dec ")+aliases[alias]+",1;"}) {
            bool rejected=false;
            try {static_cast<void>(compileDspCode(header()+body+"jmp end;routine:ret;end:",DspLanguage::Assembler));}
            catch (const DspCompileError& e) {rejected=e.line>0 && e.column>0;}
            check(rejected,"Byte steps require one initialized numeric byte operand and reject opaque parents");
        }
        const auto expected=reference(increment,0,false);
        std::string code="streamout z,p,l,result;float zero=0,one=1;"+initialize+operation;
        constexpr std::array names{"z","p","l"},branchesWithoutCarry{"jz","jp","jl"};
        for (unsigned flag=0;flag<3;++flag) {
            const auto id=std::to_string(flag);
            code+="movaps xmm0,zero;movaps "+std::string(names[flag])+",xmm0;"+branchesWithoutCarry[flag]+" yes"+id+";jmp next"+id+";yes"+id+":movaps xmm0,one;movaps "+names[flag]+",xmm0;next"+id+":";
        }
        code+=capture(parents[alias%4],"result");
        const auto program=compileDspCode(code,DspLanguage::Assembler);
        for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);const auto block=graph.addDspCode(program);auto runtime=graph.compile(block.outputs);
            std::array<std::array<float,1>,4> output{};
            check(runtime.process({},std::array{OutputBuffer{output[0]},OutputBuffer{output[1]},OutputBuffer{output[2]},OutputBuffer{output[3]}}),"Render zero/parity/signed flags without incoming carry");
            check(output[0][0]==expected.zero && output[1][0]==expected.parity && output[2][0]==expected.less &&
                std::bit_cast<std::uint32_t>(output[3][0])==(expected.value<<shift(alias)),"INC/DEC update non-carry flags even when CF is unavailable");
        }
    }
}
}
int main()
{
    try {exhaustiveByteSteps();literalsAndWidths();carryAvailabilityAndDiagnostics();}
    catch (const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
