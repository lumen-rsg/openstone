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
using Words = std::array<std::uint32_t,4>;
constexpr std::array parents{"eax","ebx","ecx","edx"};
constexpr std::array aliases{"al","bl","cl","dl","ah","bh","ch","dh"};
constexpr std::array branches{"je","jz","jne","jnz","ja","jb","jbe","jae","jp","jnp","jg","jl","jng","jnl","jge","jle"};
constexpr std::size_t ports = branches.size()+2;
constexpr std::array counts{0U,1U,2U,7U,8U,9U,15U,31U,32U,33U,39U,40U,63U,64U,65U,127U,128U,129U,255U};
void check(bool value,std::string_view message) { if (!value) throw std::runtime_error(std::string(message)); }
unsigned shift(unsigned alias) { return alias<4 ? 0 : 8; }
unsigned byte(std::uint32_t word,unsigned alias) { return (word>>shift(alias))&255U; }
std::uint32_t replace(std::uint32_t word,unsigned alias,unsigned value) { return (word&~(255U<<shift(alias)))|((value&255U)<<shift(alias)); }
struct Flags { bool carry,parity,zero,less; };
Flags seed(unsigned mode)
{
    constexpr std::array<Flags,6> flags{{{false,true,false,false},{true,true,true,true},
        {false,false,false,false},{true,true,true,false},{false,false,false,true},{true,true,false,true}}};
    return flags[mode];
}
struct Result { unsigned value; Flags flags; };
Result reference(bool left,unsigned value,unsigned rawCount,Flags incoming)
{
    unsigned count=rawCount&31U, result=value;
    if (count==0) return {value,incoming};
    bool carry=false;
    // Independent iterative eight-bit model, including the deterministic extension for undefined CF.
    for (unsigned n=0;n<count;++n) {
        carry=left ? (result&128U)!=0 : (result&1U)!=0;
        result=left ? (result*2)&255U : result/2;
    }
    const bool sign=(result&128U)!=0;
    // x86 defines OF only for count 1. Larger counts use OpenStone's scalar convention OF=0.
    const bool overflow=count==1 && (left ? sign!=carry : (value&128U)!=0);
    return {result,{carry,(std::popcount(result)&1)==0,result==0,sign!=overflow}};
}
auto branchValues(Flags r)
{
    return std::array{r.zero,r.zero,!r.zero,!r.zero,!r.carry&&!r.zero,r.carry,r.carry||r.zero,!r.carry,
        r.parity,!r.parity,!r.less&&!r.zero,r.less,r.less||r.zero,!r.less,!r.less,r.less||r.zero};
}
std::string header()
{
    std::string code="streamin value,count,seed;memrefin data;streamout ";
    for (unsigned n=0;n<branches.size();++n) code+="f"+std::to_string(n)+",";
    return code+"result,countParent;float zero=0,one=1,nan=0x7fc12345;int cell=0;";
}
std::string seedFlags()
{
    return "mov edx,seed;cmp edx,0;je seed0;cmp edx,1;je seed1;cmp edx,2;je seed2;cmp edx,3;je seed3;cmp edx,4;je seed4;"
        "mov edx,0;cmp edx,1;jmp seeded;"
        "seed0:mov edx,0x7fffffff;add edx,1;jmp seeded;"
        "seed1:mov edx,0x80000000;add edx,0x80000000;jmp seeded;"
        "seed2:movaps xmm8,one;comiss xmm8,zero;jmp seeded;"
        "seed3:movaps xmm8,nan;comiss xmm8,nan;jmp seeded;"
        "seed4:mov edx,0x80000000;or edx,1;seeded:";
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
std::string capture(std::string_view reg,std::string_view output)
{
    return "movd xmm0,"+std::string(reg)+";movaps "+std::string(output)+",xmm0;";
}
using Buffers=std::array<std::array<Mono4,64>,ports>;
void checkFlags(const Buffers& output,unsigned frame,Flags flags,const std::string& context)
{
    const auto expected=branchValues(flags);
    for (unsigned port=0;port<branches.size();++port) {
        const float value=expected[port] ? 1.F : 0.F;
        if (output[port][frame]!=Mono4{value,value,value,value}) throw std::runtime_error(context+" branch "+branches[port]);
    }
}
struct Environment { std::fenv_t saved; Environment(){std::fegetenv(&saved);} ~Environment(){std::fesetenv(&saved);} };
struct Sample { unsigned value,count,seed; };
std::vector<Sample> corpus(bool exhaustive)
{
    std::vector<Sample> result;
    for (unsigned value=0;value<256;++value) for (unsigned count=0;count<(exhaustive ? 256U : counts.size());++count)
        result.push_back({value,exhaustive ? count : counts[count],(value+count)%6});
    return result;
}
void sweep(bool left,unsigned alias,int immediate,const std::vector<Sample>& samples,bool exhaustive=false,std::string_view flow={})
{
    const bool variable=immediate<0;
    const auto operation=std::string(left ? "SHL " : "SHR ")+aliases[alias]+","+(variable ? "CL" : std::to_string(immediate))+";";
    const auto reverse=std::string(left ? "shr " : "shl ")+aliases[alias]+","+(variable ? "cl" : std::to_string(immediate))+";";
    std::string code=header()+seedFlags()+"mov "+parents[alias%4]+",value;";
    if (alias%4!=2) code+="mov ecx,count;";
    if (flow=="call") code+="call calculate;";
    else if (flow=="join" || flow=="mixed") code+="jz alternate;"+operation+"jmp joined;alternate:"+(flow=="mixed" ? reverse : operation)+"joined:";
    else code+=operation;
    code+=readFlags()+capture(parents[alias%4],"result")+capture("ecx","countParent");
    if (flow=="call") code+="jmp end;calculate:"+operation+"ret;end:";
    const auto program=compileDspCode(code,DspLanguage::Assembler);
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
                    const auto n=static_cast<unsigned>(offset+i);const auto sample=samples[n];
                    auto parent=replace(0xa5aa55ccU^(n*0x9e3779b9U),alias,sample.value);
                    if (variable && alias%4==2) parent=replace(parent,2,sample.count);
                    input[0][i]=std::bit_cast<Mono4>(Words{parent,0x7f812345,0x80000001,0xffc12345});
                    input[1][i]=std::bit_cast<Mono4>(Words{0x59ad8c00U|sample.count,0xff812345,0x80000100,0x7fc12345});
                    input[2][i].fill(std::bit_cast<float>(sample.seed));
                }
                std::array<Mono4InputBuffer,3> in;std::array<Mono4OutputBuffer,ports> out;
                for (unsigned port=0;port<3;++port) in[port]=std::span{input[port]}.first(count);
                for (unsigned port=0;port<ports;++port) out[port]=std::span{output[port]}.first(count);
                check(runtime.process(in,out),"Render byte shift matrix");
                for (unsigned i=0;i<count;++i) {
                    const auto n=static_cast<unsigned>(offset+i);const auto sample=samples[n];
                    const auto parent=std::bit_cast<Words>(input[0][i])[0];
                    const auto countParent=alias%4==2 ? parent : std::bit_cast<Words>(input[1][i])[0];
                    const auto rawCount=variable ? countParent&255U : static_cast<unsigned>(immediate);
                    const auto flags=seed(sample.seed);
                    const bool direction=flow=="mixed" && flags.zero ? !left : left;
                    const auto expected=reference(direction,byte(parent,alias),rawCount,flags);
                    const auto context=operation+" sample "+std::to_string(n)+" count "+std::to_string(rawCount);
                    checkFlags(output,i,expected.flags,context);
                    const auto word=replace(parent,alias,expected.value);
                    check(std::bit_cast<Words>(output[branches.size()][i])==Words{word,0,0,0},"SHL/SHR preserve all 24 parent bits outside the selected byte");
                    check(std::bit_cast<Words>(output[branches.size()+1][i])==Words{alias%4==2 ? word : countParent,0,0,0},"CL count uses the old value and leaves its parent unchanged except for an aliased destination");
                }
            }
            check(std::fegetround()==FE_UPWARD && std::fetestexcept(FE_ALL_EXCEPT)==FE_DIVBYZERO,"Byte shifts preserve caller rounding and exception state");
        }
    }
}
void registersCountsAndFlags()
{
    const auto edges=corpus(false),exhaustive=corpus(true);
    std::vector<Sample> values;
    for (unsigned n=0;n<512;++n) values.push_back({n&255U,(n*73)&255U,n%6});
    for (bool left : {false,true}) {
        sweep(left,4,-1,exhaustive,true);
        for (unsigned alias=0;alias<8;++alias) {
            sweep(left,alias,-1,edges);
            for (auto count : counts) sweep(left,alias,static_cast<int>(count),values);
            for (auto flow : {"call","join","mixed"}) sweep(left,alias,-1,values,false,flow);
        }
    }
    const auto shl=reference(true,0x40,1,seed(0)),shr=reference(false,0x80,1,seed(0));
    check(shl.value==0x80 && !shl.flags.less && !shl.flags.carry && shr.value==0x40 && shr.flags.less && !shr.flags.carry,
        "One-bit overflow makes signed branches differ from the raw sign bit for both directions");
    check(reference(true,1,8,seed(0)).value==0 && reference(false,255,9,seed(0)).value==0 &&
          reference(true,0x81,32,seed(1)).flags.less==seed(1).less && reference(true,0x81,33,seed(1)).value==2,
        "Wide byte shifts zero-fill; counts wrap modulo 32 and masked zero preserves flags");
}
void literal(const std::string& body,std::uint32_t parent,std::uint32_t countParent,Flags flags)
{
    const auto program=compileDspCode(header()+body+readFlags()+capture("eax","result")+capture("ecx","countParent"),DspLanguage::Assembler);
    for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);const auto block=graph.addDspCode(program);auto runtime=graph.compileMono4(block.outputs);
        for (const std::size_t size : {1,3,7,31,64}) {
            runtime.reset();Buffers output{};
            for (std::size_t offset=0;offset<67;offset+=size) {
                const auto count=std::min(size,67-offset);std::array<Mono4OutputBuffer,ports> out;
                for (unsigned port=0;port<ports;++port) out[port]=std::span{output[port]}.first(count);
                check(runtime.process({},out),"Render byte shift flag/width literal");
                for (unsigned i=0;i<count;++i) {
                    checkFlags(output,i,flags,body);
                    check(std::bit_cast<Words>(output[branches.size()][i])==Words{parent,0,0,0} &&
                          std::bit_cast<Words>(output[branches.size()+1][i])==Words{countParent,0,0,0},"Byte shifts retain literal parent bits and numeric instruction spans");
                }
            }
        }
    }
}
void controlFlowAndDiagnostics()
{
    for (auto op : {"shl","shr"}) {
        for (unsigned count : {0U,32U,64U,128U,224U}) {
            literal("mov eax,0x1234a556;mov ecx,0;cmp ecx,1;"+std::string(op)+" ah,"+std::to_string(count)+";",0x1234a556,0,seed(5));
            literal("mov eax,0x1234a556;mov ecx,"+std::to_string(count)+";movaps xmm8,nan;comiss xmm8,nan;"+op+" ah,cl;",0x1234a556,count,seed(3));
        }
        for (unsigned alias=0;alias<8;++alias) {
            const auto setup="mov eax,0x1234a556;mov ebx,0;mov ecx,0x7f;mov edx,0;cmp ebx,1;";
            literal(setup+std::string("jmp 2;")+op+" "+aliases[alias]+",cl;",0x1234a556,0x7f,seed(5));
            for (auto count : {0U,2U,8U,31U,32U,33U,255U})
                literal(setup+std::string("jmp 3;")+op+" "+aliases[alias]+","+std::to_string(count)+";",0x1234a556,0x7f,seed(5));
        }
        // A positive immediate defines flags without a previous producer; zero and CL cannot do so.
        literal("mov eax,0x12348001;mov ecx,0;"+std::string(op)+" ah,1;",std::string_view(op)=="shl" ? 0x12340001U : 0x12344001U,0,reference(std::string_view(op)=="shl",128,1,{}).flags);
        literal("mov eax,0x12348001;mov ecx,0;cmp ecx,1;mov ebx,data[0];add ebx,1;"+std::string(op)+" ah,33;",std::string_view(op)=="shl" ? 0x12340001U : 0x12344001U,0,reference(std::string_view(op)=="shl",128,1,{}).flags);
        for (auto operand : {"-1","256","eax","al","ch","xmm0","cell","data[1]"}) {
            bool rejected=false;
            try {static_cast<void>(compileDspCode(header()+"mov eax,0;mov ecx,1;"+op+" ah,"+operand+";",DspLanguage::Assembler));}
            catch (const DspCompileError& e) {rejected=e.line>0 && e.column>0;}
            check(rejected,"Byte shifts reject non-CL registers, memory counts and out-of-range immediates");
        }
        for (auto body : {std::string(op)+" ah,1;", "mov eax,data[0];"+std::string(op)+" ah,0;", "mov eax,routine;"+std::string(op)+" al,1;",
                          "mov eax,0;"+std::string(op)+" ah,cl;", "mov eax,0;mov ecx,data[0];"+std::string(op)+" al,cl;",
                          "mov ecx,1;"+std::string(op)+" cell,cl;", "mov eax,0;jmp 2;"+std::string(op)+" ah,1;"}) {
            bool rejected=false;
            try {static_cast<void>(compileDspCode(header()+body+"jmp end;routine:ret;end:",DspLanguage::Assembler));}
            catch (const DspCompileError& e) {rejected=e.line>0 && e.column>0;}
            check(rejected,"Byte shifts reject missing/opaque parents, unsized memory destinations and ambiguous count-1 encodings");
        }
        for (auto branch : branches) for (auto count : {"0","32","cl"}) for (bool stale : {false,true}) {
            const std::string invalidate=stale ? "mov edx,0;cmp edx,1;mov edx,data[0];add edx,1;" : "";
            bool rejected=false;
            try {static_cast<void>(compileDspCode(header()+invalidate+"mov eax,0;mov ecx,1;"+op+" ah,"+count+";"+branch+" done;done:",DspLanguage::Assembler));}
            catch (const DspCompileError&) {rejected=true;}
            check(rejected,"A potentially zero shift cannot fabricate unavailable flag state, including stale slots");
        }
    }
    literal("mov eax,0x12340001;mov ecx,0;cmp ecx,0;again:shl al,1;jnz again;",0x12340000,0,reference(true,128,1,{}).flags);
    literal("mov eax,0x12340080;mov ecx,1;cmp ecx,0;again:shr al,cl;jnz again;",0x12340000,1,reference(false,1,1,{}).flags);
    literal("mov eax,0x12348001;mov ecx,0;shl ah,1;inc ecx;",0x12340001,1,{true,false,false,false});
    literal("mov eax,0x12348001;mov ecx,0;shr ah,1;inc ecx;",0x12344001,1,{false,false,false,false});
}
void partialFlagAvailability()
{
    for (bool left : {false,true}) for (unsigned count : {0U,1U,8U,32U}) {
        const auto operation=std::string(left ? "shl ah,cl;" : "shr ah,cl;");
        const auto body="mov ebx,0;inc ebx;mov eax,0x12348056;mov ecx,"+std::to_string(count)+";"+operation;
        // INC supplies ZF/PF without supplying CF. A CL shift must keep those independent validity states.
        const auto code="streamout result,z,p;float zero=0,one=1;"+body+
            "movaps xmm0,zero;movaps z,xmm0;movaps p,xmm0;jz isZero;jmp next;isZero:movaps xmm0,one;movaps z,xmm0;"
            "next:jp isEven;jmp end;isEven:movaps xmm0,one;movaps p,xmm0;end:"+capture("eax","result");
        const auto program=compileDspCode(code,DspLanguage::Assembler);
        const auto expected=reference(left,128,count,{false,false,false,false});
        for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);const auto block=graph.addDspCode(program);auto runtime=graph.compileMono4(block.outputs);
            for (const std::size_t size : {1,3,7,31,64}) {
                runtime.reset();std::array<std::array<Mono4,64>,3> output{};std::array<Mono4OutputBuffer,3> out;
                for (unsigned port=0;port<3;++port) out[port]=std::span{output[port]}.first(size);
                check(runtime.process({},out),"Render CL shift with defined zero/parity and undefined incoming carry");
                for (unsigned n=0;n<size;++n) {
                    const float z=expected.flags.zero ? 1.F : 0.F,p=expected.flags.parity ? 1.F : 0.F;
                    check(std::bit_cast<Words>(output[0][n])==Words{replace(0x12348056,4,expected.value),0,0,0} &&
                        output[1][n]==Mono4{z,z,z,z} && output[2][n]==Mono4{p,p,p,p},"CL shifts preserve/update independently available ZF/PF without inventing CF");
                }
            }
        }
        for (auto prefix : {"mov ebx,0;inc ebx;", "mov ebx,data[0];cmp ebx,0;"}) for (auto condition : {"jb","jp"}) {
            if (std::string_view(condition)=="jp" && std::string_view(prefix).find("inc")!=std::string_view::npos) continue;
            bool rejected=false;
            try {static_cast<void>(compileDspCode(header()+prefix+"mov eax,0x12348056;mov ecx,1;"+operation+condition+" done;done:",DspLanguage::Assembler));}
            catch (const DspCompileError&) {rejected=true;}
            check(rejected,"CL cannot define unavailable carry/parity even when zero/signed flags are available");
        }
    }
    // Neither count-zero nor CL requires prior flags when the program observes only the resulting data.
    for (auto count : {"0","32","cl"}) {
        const auto program=compileDspCode("streamout out;mov eax,0x12348056;mov ecx,32;shr ah,"+std::string(count)+";"+capture("eax","out"),DspLanguage::Assembler);
        for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);const auto block=graph.addDspCode(program);auto runtime=graph.compileMono4(block.outputs);std::array<Mono4,1> out{};
            check(runtime.process({},std::array{Mono4OutputBuffer{out}}) && std::bit_cast<Words>(out[0])==Words{0x12348056,0,0,0},"Data-only zero shifts need no prior flag producer");
        }
    }
}
}
int main()
{
    try {registersCountsAndFlags();controlFlowAndDiagnostics();partialFlagAvailability();}
    catch (const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
