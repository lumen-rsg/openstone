#include "stream/Runtime.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace openstone::stream;
using Words=std::array<std::uint32_t,4>;
void check(bool value,std::string_view message) { if (!value) throw std::runtime_error(std::string(message)); }
struct Case { std::int64_t dividend;std::int32_t divisor; };
struct Result { DspFaultKind fault;std::uint32_t q{},r{}; };
Result reference(Case value)
{
    if (!value.divisor) return {DspFaultKind::IntegerDivideByZero};
    if (value.dividend==INT64_MIN && value.divisor==-1) return {DspFaultKind::IntegerDivideOverflow};
    // Independent signed arithmetic, unlike the interpreter's unsigned-magnitude algorithm.
    const auto q=value.dividend/value.divisor,r=value.dividend%value.divisor;
    if (q<INT32_MIN || q>INT32_MAX) return {DspFaultKind::IntegerDivideOverflow};
    check(q*value.divisor+r==value.dividend && (r==0 || (r<0)==(value.dividend<0)) &&
          (r<0 ? -r : r)<(value.divisor<0 ? -std::int64_t{value.divisor} : value.divisor),"Reference division identity, sign and remainder bound");
    return {DspFaultKind::None,static_cast<std::uint32_t>(q),static_cast<std::uint32_t>(r)};
}
std::vector<Case> corpus()
{
    std::vector<Case> result{
        {7,3},{-7,3},{7,-3},{-7,-3},{0,3},{0,-3},{1,2},{-1,2},{1,-2},{-1,-2},
        {INT32_MAX,1},{INT32_MIN,1},{INT32_MIN,-1},{INT32_MAX,-1},
        {4294967301LL,3},{-4294967301LL,3},{4294967302LL,3},{-4294967302LL,-3},
        {4294967295LL,INT32_MIN},{-4294967296LL,INT32_MIN},
        {INT64_MIN,-1},{INT64_MIN,1},{INT64_MIN,INT32_MIN},{INT64_MAX,1},{INT64_MAX,-1},
        {std::int64_t{INT32_MAX}*INT32_MAX+INT32_MAX-1,INT32_MAX},
        {std::int64_t{INT32_MIN}*INT32_MAX-(INT32_MAX-1),INT32_MAX},
        {std::int64_t{INT32_MAX}+1,1},{std::int64_t{INT32_MIN}-1,1},
        {0,0},{1,0},{-1,0},{INT64_MIN,0},{INT64_MAX,0},
    };
    // Random full-width dividends include both valid and oversized quotients.
    std::uint64_t state=0x879edcaf12345678ULL;
    for (unsigned n=0;n<128;++n) {
        state=state*6364136223846793005ULL+1442695040888963407ULL;
        const auto dividend=std::bit_cast<std::int64_t>(state);
        state=state*6364136223846793005ULL+1442695040888963407ULL;
        result.push_back({dividend,std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(state>>17))});
    }
    for (const auto literal : {Case{7,3},Case{-7,3},Case{7,-3},Case{-7,-3}}) {
        const auto actual=reference(literal);
        check(std::bit_cast<std::int32_t>(actual.q)==((literal.dividend<0)!=(literal.divisor<0) ? -2 : 2) &&
              std::bit_cast<std::int32_t>(actual.r)==(literal.dividend<0 ? -1 : 1),"Literal quotient/remainder signs");
    }
    return result;
}
std::string read(std::string_view out,std::string_view operand)
{ return "movd xmm5,"+std::string(operand)+";movaps "+std::string(out)+",xmm5;"; }
struct RestoreEnvironment {
    std::fenv_t saved;
    RestoreEnvironment() { std::fegetenv(&saved); }
    ~RestoreEnvironment() { std::fesetenv(&saved); }
};

void operandsBuffersAndFaultRecovery()
{
    RestoreEnvironment restore;
    auto cases=corpus();cases.insert(cases.end(),7,Case{-17,5});
    for (unsigned form=0;form<15;++form) {
        const std::string operand=form==0 || form==10 || form==11 ? "ebx" : form==1 ? "ecx" : form==2 || form==12 ? "value" :
            form==3 ? "cells[2]" : form==4 ? "cells[ebx]" : form==5 ? "[ecx+4]" : form==6 ? "[ecx+ebx*4]" :
            form==7 ? "eax" : form==8 ? "edx" : form==9 ? "data[3]" : form==13 ? "cells[eax]" : "cells[edx]";
        std::string setup="mov ebx,value;";
        if (form==1) setup+="mov ecx,ebx;";
        if (form==3 || form==13) setup+="mov cells[2],ebx;";
        if (form==14) setup+="mov cells[1],ebx;";
        if (form==4) setup+="mov cells[2],ebx;mov ebx,8;";
        if (form==5 || form==6) setup+="mov ecx,data[0];mov [ecx+4],ebx;";
        if (form==6) setup+="mov ebx,1;";
        const auto operation=form==10 ? std::string("call divide;") : form==11 ? "jmp 2;idiv ebx;" : form==12 ? "jmp 6;idiv value;" : "IDIV "+operand+";";
        const auto program=compileDspCode(std::string("streamin low,high,value;memrefin data;streamout q,r,marker,seenLow,seenHigh,source;")+
            "float sentinel=0x4f123456,zero=0,one=1;int cells[4];movaps xmm5,sentinel;movaps q,xmm5;movaps r,xmm5;"
            "movaps xmm5,zero;movaps marker,xmm5;mov eax,low;mov edx,high;"+read("seenLow","eax")+read("seenHigh","edx")+
            setup+read("source",operand)+operation+read("q","eax")+read("r","edx")+
            (form==13 || form==14 ? "" : read("source",operand))+"movaps xmm5,one;movaps marker,xmm5;"+
            (form==10 ? "jmp done;divide:idiv ebx;ret;done:" : ""),DspLanguage::Assembler);
        std::array<std::vector<Mono4>,3> input;
        for (auto& port : input) port.resize(cases.size());
        std::vector<Case> actual=cases;
        for (std::size_t n=0;n<cases.size();++n) {
            auto bits=std::bit_cast<std::uint64_t>(cases[n].dividend);
            if (form==13) bits=(bits&0xffffffff00000000ULL)|8U;
            if (form==14) bits=(bits&0xffffffffULL)|(std::uint64_t{4}<<32);
            actual[n].dividend=std::bit_cast<std::int64_t>(bits);
            if (form==7) actual[n].divisor=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(bits));
            if (form==8) actual[n].divisor=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(bits>>32));
            if (form==9) actual[n].divisor=2;
            input[0][n]={std::bit_cast<float>(static_cast<std::uint32_t>(bits)),111,-222,std::bit_cast<float>(0x7f812345U)};
            input[1][n]={std::bit_cast<float>(static_cast<std::uint32_t>(bits>>32)),-111,222,std::bit_cast<float>(0xff854321U)};
            input[2][n]={std::bit_cast<float>(static_cast<std::uint32_t>(cases[n].divisor)),999,-999,std::bit_cast<float>(0x7fc12345U)};
        }
        for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);const auto block=graph.addDspCode(program);
            for (unsigned port=0;port<3;++port) graph.connectDspInput(graph.addInput(),block.id,port);
            auto runtime=graph.compileMono4(block.outputs);
            check(runtime.setDspMemRefInput(block.id,3,openstone::green::Memory(std::vector<std::uint8_t>(16),2)),"Bind IDIV descriptor and wave memory");
            if (backend==DspBackend::LlvmOrc && form==0) check(runtime.compiledDspCode().contains("sdiv i64"),"ORC emits its own guarded signed division");
            for (const std::size_t size : {1,3,7,31,64}) {
                runtime.reset();check(runtime.dspFault(block.id).kind==DspFaultKind::None,"Reset clears arithmetic faults");
                std::array<std::vector<Mono4>,6> output;
                for (auto& port : output) port.resize(cases.size());
                DspFault first;
                for (std::size_t offset=0;offset<cases.size();offset+=size) {
                    const auto count=std::min(size,cases.size()-offset);
                    std::array<Mono4InputBuffer,3> in;std::array<Mono4OutputBuffer,6> out;
                    for (unsigned port=0;port<in.size();++port) in[port]=std::span{input[port]}.subspan(offset,count);
                    for (unsigned port=0;port<out.size();++port) out[port]=std::span{output[port]}.subspan(offset,count);
                    for (std::size_t n=offset;n<offset+count;++n) {
                        const auto expected=reference(actual[n]);
                        if (form!=11 && form!=12 && first.kind==DspFaultKind::None && expected.fault!=DspFaultKind::None) first={expected.fault,n};
                    }
                    std::fesetround(FE_UPWARD);std::feclearexcept(FE_ALL_EXCEPT);std::feraiseexcept(FE_DIVBYZERO);
                    check(runtime.process(in,out),"IDIV faults stop the stage without rejecting valid render buffers");
                    check(std::fegetround()==FE_UPWARD && std::fetestexcept(FE_ALL_EXCEPT)==FE_DIVBYZERO,"IDIV restores caller floating-point state without host exceptions");
                    check(runtime.dspFault(block.id)==first,"The first precise divide fault and frame remain available until reset");
                }
                for (std::size_t n=0;n<cases.size();++n) {
                    auto expected=reference(actual[n]);const auto bits=std::bit_cast<std::uint64_t>(actual[n].dividend);
                    if (form==11 || form==12) expected={DspFaultKind::None,static_cast<std::uint32_t>(bits),static_cast<std::uint32_t>(bits>>32)};
                    const bool failed=expected.fault!=DspFaultKind::None;
                    const auto packet=[](std::uint32_t word) { return Words{word,0,0,0}; };
                    const Words sentinel{0x4f123456,0x4f123456,0x4f123456,0x4f123456};
                    check(std::bit_cast<Words>(output[0][n])==(failed ? sentinel : packet(expected.q)) &&
                          std::bit_cast<Words>(output[1][n])==(failed ? sentinel : packet(expected.r)),"Exact signed quotient and dividend-signed remainder, or no faulting-result commit");
                    const auto marker=failed ? 0.F : 1.F;
                    check(output[2][n]==Mono4{marker,marker,marker,marker},"A divide fault aborts the whole stage, including nested callers; later samples recover");
                    check(std::bit_cast<Words>(output[3][n])==packet(static_cast<std::uint32_t>(bits)) &&
                          std::bit_cast<Words>(output[4][n])==packet(static_cast<std::uint32_t>(bits>>32)),"Both original EDX:EAX words are used");
                    const auto source=!failed && form==7 ? expected.q : !failed && form==8 ? expected.r : static_cast<std::uint32_t>(actual[n].divisor);
                    check(std::bit_cast<Words>(output[5][n])==packet(source),"Divisor sources retain their bits; aliased implicit registers receive their defined results");
                }
            }
        }
    }
}

void faultAtomicityAndControlFlow()
{
    using namespace openstone::stream::detail;
    const auto cases=corpus();
    DspProgramData program;program.slotCount=4;program.stackSize=3;
    program.stages[2]={{DspOp::Load,DspType::Int,0},{DspOp::Load,DspType::Int,1},{DspOp::Load,DspType::Int,2},
        {DspOp::IntegerDivide,DspType::Int},{DspOp::Store,DspType::Int,1},{DspOp::Store,DspType::Int,0},
        {.op=DspOp::Constant,.type=DspType::Int,.bits=99},{DspOp::Store,DspType::Int,3}};
    for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        const auto native=backend==DspBackend::LlvmOrc ? compileDspOrc(program) : nullptr;
        for (auto sample : cases) {
            const auto expected=reference(sample);const auto bits=std::bit_cast<std::uint64_t>(sample.dividend);
            std::array<DspLanes,4> slots{};slots[0].fill(static_cast<std::uint32_t>(bits));slots[1].fill(bits>>32);
            slots[2].fill(static_cast<std::uint32_t>(sample.divisor));slots[3].fill(123);
            const auto original=slots;DspMemoryReferences references;std::array<DspValue,3> stack{};
            if (native) native->stages[2](slots.data(),nullptr,&references,42);
            else runDspStage(program.stages[2],slots,{},references,stack,42);
            if (expected.fault!=DspFaultKind::None) {
                check(slots==original && references.fault==DspFault{expected.fault,42},"Zero/overflow faults commit neither register and skip following instructions");
            } else {
                DspLanes q{},r{};q.fill(expected.q);r.fill(expected.r);
                check(slots[0]==q && slots[1]==r && slots[2]==original[2] && slots[3][0]==99 && references.fault.kind==DspFaultKind::None,"A valid division atomically writes both outputs and leaves its divisor untouched");
            }
        }
    }
    for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);const auto block=graph.addDspCode(compileDspCode("streamin value;streamout out;mov eax,value;mov ebx,2;mov ecx,3;again:mov edx,0;idiv ebx;dec ecx;jnz again;movd xmm0,eax;movaps out,xmm0;",DspLanguage::Assembler));
        graph.connectDspInput(graph.addInput(),block.id,0);auto runtime=graph.compile(block.outputs);
        const std::array input{std::bit_cast<float>(0xffffffffU),std::bit_cast<float>(12345U)};std::array<float,2> out{};
        check(runtime.process(std::array{InputBuffer{input}},std::array{OutputBuffer{out}}) && std::bit_cast<std::uint32_t>(out[0])==0xffffffffU/8 && std::bit_cast<std::uint32_t>(out[1])==12345U/8,"IDIV works in counted loops with a defined subsequent flag producer");
    }
    for (const auto body : {"idiv ebx;", "mov eax,1;idiv ebx;", "mov edx,0;idiv ebx;", "mov eax,1;mov edx,0;idiv xmm0;",
        "mov eax,1;mov edx,0;idiv 2;", "mov eax,1;mov edx,0;idiv ebx,1;", "mov eax,1;mov edx,0;idiv rbx;",
        "mov eax,data[0];mov edx,0;idiv value;", "mov eax,1;mov edx,0;mov ebx,data[0];idiv ebx;",
        "mov eax,1;mov edx,0;cmp eax,1;idiv value;jz done;done:", "mov eax,1;mov edx,0;cmp eax,1;idiv value;jb done;done:",
        "mov eax,1;mov edx,0;cmp eax,1;idiv value;jp done;done:", "mov eax,1;mov edx,0;cmp eax,1;idiv value;jge done;done:",
        "mov eax,1;mov edx,0;cmp value,0;jz skip;idiv value;skip:jb done;done:"}) {
        bool rejected=false;
        try { static_cast<void>(compileDspCode(std::string("streamin value;memrefin data;streamout out;")+body,DspLanguage::Assembler)); }
        catch (const DspCompileError& e) { rejected=e.line>0 && e.column>0; }
        check(rejected,"IDIV requires initialized numeric EDX:EAX, one numeric 32-bit divisor and a new flag producer afterward");
    }
}
}
int main()
{
    try { operandsBuffersAndFaultRecovery();faultAtomicityAndControlFlow(); }
    catch (const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
