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
void check(bool value,std::string_view message) {if (!value) throw std::runtime_error(std::string(message));}
unsigned shift(unsigned alias) {return alias<4 ? 0 : 8;}
std::uint32_t replace(std::uint32_t word,unsigned alias,unsigned byte) {return (word&~(255U<<shift(alias)))|((byte&255U)<<shift(alias));}
unsigned byte(std::uint32_t word,unsigned alias) {return (word>>shift(alias))&255U;}
struct Case {int ax,divisor;};
struct Result {DspFaultKind fault;std::uint32_t eax{},q{},r{};};
Result reference(std::uint32_t eax,std::uint32_t rawDivisor)
{
    // Independent signed 32-bit arithmetic; neither -32768/-1 nor any other byte case can overflow it.
    const int ax=std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(eax));
    const int divisor=std::bit_cast<std::int8_t>(static_cast<std::uint8_t>(rawDivisor));
    if (!divisor) return {DspFaultKind::IntegerDivideByZero,eax};
    const int q=ax/divisor,r=ax%divisor;
    if (q< -128 || q>127) return {DspFaultKind::IntegerDivideOverflow,eax};
    check(q*divisor+r==ax && (r==0 || (r<0)==(ax<0)) && (r<0 ? -r : r)<(divisor<0 ? -divisor : divisor),"Signed byte division identity, remainder sign and magnitude");
    return {DspFaultKind::None,(eax&0xffff0000U)|(static_cast<unsigned>(q)&255U)|((static_cast<unsigned>(r)&255U)<<8),static_cast<std::uint32_t>(q),static_cast<std::uint32_t>(r)};
}
std::vector<Case> corpus()
{
    std::vector<Case> cases{{7,3},{-7,3},{7,-3},{-7,-3},{0,1},{1,0},{0,0},{-1,0},
        {127,1},{128,1},{-128,1},{-129,1},{128,-1},{129,-1},{-127,-1},{-128,-1},
        {-32768,-1},{-32768,-128},{-32768,127},{32767,-128},{16384,-128},{-16384,-128}};
    for (int d=-128;d<=127;++d) {
        for (int q : {-129,-128,-127,-1,0,1,126,127,128}) {
            const int tail=d<0 ? -d-1 : d>0 ? d-1 : 1;
            for (int r : {-tail,0,tail}) cases.push_back({q*d+r,d});
        }
    }
    for (int ax : {-32768,-32767,-16385,-16384,-129,-128,-127,-1,0,1,126,127,128,129,16383,16384,16385,32766,32767})
        for (int d : {-128,-127,-2,-1,0,1,2,126,127}) cases.push_back({ax,d});
    cases.insert(cases.end(),7,Case{-17,5});return cases;
}
std::string capture(std::string_view reg,std::string_view out) {return "movd xmm0,"+std::string(reg)+";movaps "+std::string(out)+",xmm0;";}
struct Environment {std::fenv_t saved;Environment(){std::fegetenv(&saved);}~Environment(){std::fesetenv(&saved);}};
Words packet(std::uint32_t word) {return {word,0,0,0};}
void operandsAndBuffers()
{
    const auto cases=corpus();Environment environment;
    for (unsigned form=0;form<18;++form) {
        const bool reg=form<8 || form==15 || form==16;
        const auto alias=form<8 ? form : form==16 ? 5U : 1U;
        const auto operand=reg ? std::string(aliases[alias]) : form==8 || form==17 ? "byte cell" : form==9 ? "BYTE PTR cells[edx]" :
            form==10 ? "byte wide[edx]" : form==11 ? "byte ptr [ecx+edx*4]" : form==12 ? "byte samples[edx]" :
            form==13 ? "byte value[edx]" : "byte ptr [ecx+eax*4-1221591040]";
        std::string setup="mov edx,0x89abcdef;mov eax,input;";
        if (reg && alias%4!=0) setup+="mov "+std::string(parents[alias%4])+",value;";
        if (!reg) {
            setup+="mov ebx,value;";
            if (form==8 || form==17) setup+="mov cell,ebx;";
            if (form==9) setup+="mov edx,15;mov cells[edx],bl;";
            if (form==10) setup+="mov edx,15;mov wide[edx],bl;";
            if (form==11) setup+="mov ecx,data[0];mov edx,4;mov [ecx+edx*4],bl;";
            if (form==12 || form==13) setup+="mov edx,15;";
            if (form==14) setup+="mov ecx,data[0];mov [ecx+eax*4-1221591040],bl;";
        }
        const auto sourceParent=reg ? parents[alias%4] : "ebx";
        auto code="streamin input,value;memrefin data;floatarrayin samples[4];streamout result,q,r,marker,before,source,edxBefore,edxAfter;"
            "float sentinel=0x4f123456,zero=0,one=1,cell;int cells[4];double wide[2];"
            "movaps xmm0,sentinel;movaps result,xmm0;movaps q,xmm0;movaps r,xmm0;movaps xmm0,zero;movaps marker,xmm0;movaps edxAfter,xmm0;"+
            setup+capture("eax","before")+capture(sourceParent,"source")+capture("edx","edxBefore");
        code+=form==15 ? "call divide;" : form==16 ? "jmp 2;idiv bh;" : form==17 ? "jmp 6;idiv byte cell;" : "IDIV "+operand+";";
        code+=capture("eax","result")+capture(sourceParent,"source")+capture("edx","edxAfter")+
            "mov ebx,0;mov bl,al;"+capture("ebx","q")+"mov bl,ah;"+capture("ebx","r")+"movaps xmm0,one;movaps marker,xmm0;";
        if (form==15) code+="jmp end;divide:idiv bl;ret;end:";
        const auto program=compileDspCode(code,DspLanguage::Assembler);
        std::array<std::vector<Mono4>,2> input;for (auto& p:input)p.resize(cases.size());
        std::vector<std::uint32_t> original(cases.size()),divisors(cases.size()),source(cases.size()),edx(cases.size());
        for (unsigned n=0;n<cases.size();++n) {
            original[n]=((n*0x9e3779b9U)&0xffff0000U)|(static_cast<unsigned>(cases[n].ax)&65535U);
            if (form==14) original[n]=0x12340004U;
            auto value=replace(0xa5aa55ccU^(n*0x914acddeU),reg ? alias : 1,static_cast<unsigned>(cases[n].divisor));
            if (form==12 || form==13) value=(value&0x00ffffffU)|((static_cast<unsigned>(cases[n].divisor)&255U)<<24);
            input[0][n]=std::bit_cast<Mono4>(Words{original[n],0x7f812345,0x80000001,0xffc12345});
            input[1][n]=std::bit_cast<Mono4>(Words{value,0x7fc12345,0x80000000,value});
            divisors[n]=reg && alias%4==0 ? byte(original[n],alias) : static_cast<unsigned>(cases[n].divisor)&255U;
            source[n]=reg && alias%4==0 ? original[n] : value;
            edx[n]=reg && alias%4==3 ? value : form==9 || form==10 || form==12 || form==13 ? 15U : form==11 ? 4U : 0x89abcdefU;
        }
        for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);const auto block=graph.addDspCode(program);
            for (unsigned p=0;p<2;++p)graph.connectDspInput(graph.addInput(),block.id,p);
            auto runtime=graph.compileMono4(block.outputs);
            if (backend==DspBackend::LlvmOrc && form==1)check(runtime.compiledDspCode().contains("sdiv i"),"Byte IDIV has an independent guarded LLVM signed division path");
            for (const std::size_t size : {1,3,7,31,64}) {
                runtime.reset();check(runtime.dspFault(block.id).kind==DspFaultKind::None,"Reset clears prior byte division faults");
                const openstone::green::Memory memory(std::vector<std::uint8_t>(17,0x5a));
                check(runtime.setDspMemRefInput(block.id,2,memory),"Bind final-byte IDIV memory");
                std::array<std::uint64_t,4> sampleWords{};sampleWords[3]=static_cast<std::uint64_t>(static_cast<unsigned>(cases.front().divisor)&255U)<<24;
                DspFault first;
                for (std::size_t offset=0;offset<cases.size();offset+=size) {
                    const auto count=std::min(size,cases.size()-offset);
                    // FloatArray is buffer-scoped, so form 12 uses a fixed final input byte for this batch.
                    if (form==12) {
                        const std::array values{0.F,0.F,0.F,std::bit_cast<float>(static_cast<std::uint32_t>(sampleWords[3]))};
                        check(runtime.setDspArrayInput(block.id,3,values),"Bind a read-only final array byte");
                    }
                    std::array<std::array<Mono4,64>,8> output{};std::array<Mono4InputBuffer,2> in;std::array<Mono4OutputBuffer,8> out;
                    for (unsigned p=0;p<2;++p)in[p]=std::span{input[p]}.subspan(offset,count);
                    for (unsigned p=0;p<8;++p)out[p]=std::span{output[p]}.first(count);
                    for (std::size_t n=offset;n<offset+count;++n) {
                        const auto d=form==12 ? (sampleWords[3]>>24)&255U : divisors[n];
                        const auto expected=reference(original[n],static_cast<std::uint32_t>(d));
                        if (form!=16 && form!=17 && first.kind==DspFaultKind::None && expected.fault!=DspFaultKind::None) first={expected.fault,n};
                    }
                    std::fesetround(FE_UPWARD);std::feclearexcept(FE_ALL_EXCEPT);std::feraiseexcept(FE_DIVBYZERO);
                    check(runtime.process(in,out),"Render signed byte IDIV/fault recovery");
                    check(std::fegetround()==FE_UPWARD && std::fetestexcept(FE_ALL_EXCEPT)==FE_DIVBYZERO,"Byte division preserves caller rounding and exceptions");
                    check(runtime.dspFault(block.id)==first,"Byte faults report the first exact kind/frame until reset");
                    for (unsigned i=0;i<count;++i) {
                        const auto n=static_cast<unsigned>(offset+i);const auto d=form==12 ? (sampleWords[3]>>24)&255U : divisors[n];
                        auto expected=reference(original[n],static_cast<std::uint32_t>(d));
                        if (form==16 || form==17)expected={DspFaultKind::None,original[n],original[n]&255U,(original[n]>>8)&255U};
                        const bool failed=expected.fault!=DspFaultKind::None;const Words sentinel{0x4f123456,0x4f123456,0x4f123456,0x4f123456};
                        check(std::bit_cast<Words>(output[0][i])==(failed ? sentinel : packet(expected.eax)) &&
                              std::bit_cast<Words>(output[1][i])==(failed ? sentinel : packet(expected.q&255U)) &&
                              std::bit_cast<Words>(output[2][i])==(failed ? sentinel : packet(expected.r&255U)),"Signed AX division yields AL quotient/AH remainder with upper EAX preserved, or commits no faulting result");
                        const float marker=failed ? 0.F : 1.F;check(output[3][i]==Mono4{marker,marker,marker,marker},"Faults stop the current stage and nested caller; following samples continue");
                        check(std::bit_cast<Words>(output[4][i])==packet(original[n]) && std::bit_cast<Words>(output[6][i])==packet(edx[n]),"Division captures original AX/EAX and EDX");
                        check(std::bit_cast<Words>(output[5][i])==packet(reg && alias%4==0 && !failed ? expected.eax : source[n]),"Divisor parent stays unchanged except when it is the implicit EAX destination");
                        check(std::bit_cast<Words>(output[7][i])==(failed ? Words{} : packet(edx[n])),"Byte division does not change EDX");
                    }
                    if (form==11 || form==14) {
                        auto bytes=std::vector<std::uint8_t>(17,0x5a);bytes[16]=static_cast<std::uint8_t>(divisors[offset+count-1]);
                        check(memory.snapshotBytes()==bytes,"Final-byte divisors are read-only and neighboring memory remains unchanged");
                    }
                }
            }
        }
    }
}
void directFaultAtomicity()
{
    using namespace openstone::stream::detail;
    DspProgramData program;program.slotCount=4;program.stackSize=3;
    program.stages[2]={{DspOp::Load,DspType::Int,0},{DspOp::Load,DspType::Int,1},{DspOp::Load,DspType::Int,2},
        {.op=DspOp::IntegerDivide,.type=DspType::Int,.bits=8},{DspOp::Store,DspType::Int,1},{DspOp::Store,DspType::Int,0},
        {.op=DspOp::Constant,.type=DspType::Int,.bits=99},{DspOp::Store,DspType::Int,3}};
    Environment environment;
    for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend))continue;
        const auto native=backend==DspBackend::LlvmOrc ? compileDspOrc(program) : nullptr;
        std::fesetround(FE_UPWARD);std::feclearexcept(FE_ALL_EXCEPT);std::feraiseexcept(FE_DIVBYZERO);
        for (unsigned ax=0;ax<65536;++ax) for (int divisor : {-128,-127,-1,0,1,2,127}) {
            const auto eax=((ax*0x9e3779b9U)&0xffff0000U)|ax;
            const auto rawDivisor=0xa5aa5500U|(static_cast<unsigned>(divisor)&255U);
            const auto expected=reference(eax,rawDivisor);
            std::array<DspLanes,4> slots{};slots[0].fill(eax);slots[1].fill(0xdeadbeef);slots[2].fill(rawDivisor);slots[3].fill(123);
            const auto before=slots;DspMemoryReferences references;std::array<DspValue,3> stack{};
            if (native)native->stages[2](slots.data(),nullptr,&references,42);
            else runDspStage(program.stages[2],slots,{},references,stack,42);
            if (expected.fault!=DspFaultKind::None) {
                check(slots==before && references.fault==DspFault{expected.fault,42},"Every signed AX pattern: zero/overflow faults commit no output or following instruction");
            } else {
                DspLanes q{},r{};q.fill(expected.q);r.fill(expected.r);
                check(slots[0]==q && slots[1]==r && slots[2]==before[2] && slots[3][0]==99 && references.fault.kind==DspFaultKind::None,
                    "Every signed AX pattern: independent backends ignore upper EAX, the high input, and upper divisor bits; quotient/remainder signs are exact");
            }
        }
        check(std::fegetround()==FE_UPWARD && std::fetestexcept(FE_ALL_EXCEPT)==FE_DIVBYZERO,"Direct byte divide stages preserve caller floating-point state");
    }
    const auto a=reference(0xdeadfff9U,3),b=reference(0xbeef0007U,0xfd),c=reference(0x1234fff9U,0xfd);
    check(a.eax==0xdeadfffeU && b.eax==0xbeef01feU && c.eax==0x1234ff02U,"Literal negative quotient and dividend-signed remainder words preserve the upper half of EAX");
}
void controlFlowBoundsAndDiagnostics()
{
    for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend))continue;
        for (auto code : {
            "mov eax,0x12340007;mov ebx,3;idiv bl;", // No EDX initialization is required.
            "mov eax,0x12340007;mov ebx,0x12340007;mov ecx,3;mov edx,0;again:mov eax,ebx;idiv cl;dec edx;cmp edx,-3;jnz again;",
            "mov eax,0x12340007;mov ebx,3;jmp first;other:idiv bl;ret;first:call other;",
            "mov eax,0x12340007;mov ebx,3;cmp eax,0;jz alternate;idiv bl;jmp joined;alternate:idiv bl;joined:",
        }) {
            Graph graph(backend);const auto block=graph.addDspCode(compileDspCode(std::string("streamout out;")+code+capture("eax","out"),DspLanguage::Assembler));auto runtime=graph.compileMono4(block.outputs);
            for (const std::size_t size : {1,3,7,31,64}) {
                runtime.reset();std::array<Mono4,64> out{};
                check(runtime.process({},std::array{Mono4OutputBuffer{std::span{out}.first(size)}}),"Render byte division control flow without implicit EDX reads");
                for (unsigned n=0;n<size;++n)check(std::bit_cast<Words>(out[n])==packet(0x12340102),"Byte IDIV works in calls, counted loops and joins");
            }
        }
        // A one-byte allocation proves the memory operand is not silently widened to a DWORD.
        for (auto operand : {"byte [ecx]","BYTE PTR [rcx]","byte cells[0]"}) {
            const auto code="memrefin data;streamout out;int cells[1]={0x123456fd};mov eax,0x89ab0007;mov rcx,data[0];idiv "+std::string(operand)+";"+capture("eax","out");
            Graph graph(backend);const auto block=graph.addDspCode(compileDspCode(code,DspLanguage::Assembler));auto runtime=graph.compileMono4(block.outputs);
            check(runtime.setDspMemRefInput(block.id,0,openstone::green::Memory({0xfd})),"Bind a single signed divisor byte");
            std::array<Mono4,1> out{};check(runtime.process({},std::array{Mono4OutputBuffer{out}}) && std::bit_cast<Words>(out[0])==packet(0x89ab01fe),"Explicit byte memory reads signed -3 and preserves upper EAX");
        }
        for (auto memory : {openstone::green::Memory{},openstone::green::Memory({3})}) {
            Graph graph(backend);const auto block=graph.addDspCode(compileDspCode("memrefin data;streamout out;float zero=0,one=1;movaps xmm0,zero;movaps out,xmm0;mov eax,7;mov ecx,data[0];idiv byte [ecx+1];movaps xmm0,one;movaps out,xmm0;",DspLanguage::Assembler));auto runtime=graph.compile(block.outputs);
            check(runtime.setDspMemRefInput(block.id,0,memory),"Bind bounded byte divide memory");std::array<float,1> out{};
            check(runtime.process({},std::array{OutputBuffer{out}}) && out[0]==0.F && runtime.dspFault(block.id)==DspFault{DspFaultKind::IntegerDivideByZero,0},"An invalid/empty byte read returns zero and takes the normal divide-by-zero path");
        }
        // Qualifier support does not reserve an existing variable named byte or change unsized IDIV.
        Graph graph(backend);const auto block=graph.addDspCode(compileDspCode("streamout out;int byte=3;mov eax,7;mov edx,0;idiv byte;"+capture("eax","out"),DspLanguage::Assembler));auto runtime=graph.compileMono4(block.outputs);std::array<Mono4,1> out{};
        check(runtime.process({},std::array{Mono4OutputBuffer{out}}) && std::bit_cast<Words>(out[0])==packet(2),"A named byte variable retains the existing dword divisor grammar");
    }
    constexpr std::array branches{"je","jz","jne","jnz","ja","jb","jbe","jae","jp","jnp","jg","jl","jng","jnl","jge","jle"};
    for (auto branch:branches)for(auto operation:{"idiv bl;","idiv byte cell;","call divide;"}) {
        bool rejected=false;
        try {static_cast<void>(compileDspCode("streamout out;int cell=1;mov eax,7;mov ebx,3;cmp eax,1;"+std::string(operation)+branch+" end;end:jmp finished;divide:idiv bl;ret;finished:",DspLanguage::Assembler));}
        catch(const DspCompileError& e){rejected=e.line>0&&e.column>0;}
        check(rejected,"All byte IDIV arithmetic flags require a new producer, including after calls");
    }
    for (auto body:{"idiv bl;","mov ebx,1;idiv bl;","mov eax,1;idiv bl;","mov eax,data[0];mov ebx,1;idiv bl;",
        "mov eax,1;mov ebx,data[0];idiv bh;","mov eax,routine;mov ebx,1;idiv bl;",
        "mov eax,1;idiv byte eax;","mov eax,1;mov ebx,1;idiv byte bl;","mov eax,1;idiv byte data[0];",
        "mov eax,1;idiv byte waves[0];","mov eax,1;idiv byte ptr;","mov eax,1;idiv byte 2;",
        "mov eax,1;mov ebx,1;idiv bl,1;","mov eax,1;jmp 1;idiv al;",
        "mov eax,1;mov ecx,data[0];jmp 2;idiv byte [ecx];"}) {
        bool rejected=false;
        try {static_cast<void>(compileDspCode("memrefin data;memarrayrefin waves;streamout out;"+std::string(body)+"jmp end;routine:ret;end:",DspLanguage::Assembler));}
        catch(const DspCompileError& e){rejected=e.line>0&&e.column>0;}
        check(rejected,"Byte IDIV rejects missing/opaque parents, invalid qualifiers/descriptors, malformed operands and unknown numeric spans");
    }
}
}
int main()
{
    try {operandsAndBuffers();directFaultAtomicity();controlFlowBoundsAndDiagnostics();}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
