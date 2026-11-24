#include "stream/Runtime.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace openstone::stream;
void check(bool value, std::string_view message) { if (!value) throw std::runtime_error(std::string(message)); }
enum class Op { Cmp, Add, Sub, Inc, Dec, Test, Xor, And, Or };
struct Input { std::uint32_t a,b; bool carry; };
struct Result { std::uint32_t value; bool carry,parity,zero,less,overflow; };
Result reference(Op op, Input in)
{
    if (op == Op::Test || op == Op::Xor || op == Op::And || op == Op::Or) {
        const auto result = op == Op::Test || op == Op::And ? in.a & in.b : op == Op::Or ? in.a | in.b : in.a ^ in.b;
        return {op == Op::Test ? in.a : result,false,(std::popcount(result & 0xffU)&1)==0,result==0,
                (result & 0x80000000U)!=0,false};
    }
    const bool unitStep = op == Op::Inc || op == Op::Dec;
    const auto b = unitStep ? 1U : in.b;
    const bool add = op == Op::Add || op == Op::Inc;
    const std::uint64_t wide = std::uint64_t{in.a} + b;
    const std::uint32_t result = add ? static_cast<std::uint32_t>(wide) : in.a - b;
    const bool overflow = ((add ? ~(in.a ^ b) : (in.a ^ b)) & (in.a ^ result) & 0x80000000U) != 0;
    const bool carry = unitStep ? in.carry : add ? wide > UINT32_MAX : in.a < b;
    return {op == Op::Cmp ? in.a : result,carry,(std::popcount(result & 0xffU)&1)==0,result==0,
            bool(result&0x80000000U)!=overflow,overflow};
}
constexpr std::array branches{"je","jz","jne","jnz","ja","jb","jbe","jae","jp","jnp","jg","jl","jng","jnl","jge","jle"};
constexpr std::size_t outputCount = branches.size() + 2;
bool taken(unsigned branch, Result r)
{
    const std::array result{r.zero,r.zero,!r.zero,!r.zero,!r.carry&&!r.zero,r.carry,r.carry||r.zero,!r.carry,
        r.parity,!r.parity,!r.less&&!r.zero,r.less,r.less||r.zero,!r.less,!r.less,r.less||r.zero};
    return result[branch];
}
std::string branchRead(std::string_view branch, unsigned id, std::string_view out)
{
    const auto name=std::to_string(id);
    return "movaps xmm7,zero;movaps "+std::string(out)+",xmm7;"+std::string(branch)+" yes"+name+
        ";jmp done"+name+";yes"+name+":movaps xmm7,one;movaps "+std::string(out)+",xmm7;done"+name+":";
}
std::vector<Input> inputs()
{
    constexpr std::array<std::array<std::uint32_t,2>,20> edges{{
        {0,0},{0,1},{1,0},{UINT32_MAX,1},{UINT32_MAX,UINT32_MAX},{0x7fffffff,1},{0x80000000,1},
        {0x80000000,UINT32_MAX},{0x80000000,0x80000000},{0x7fffffff,UINT32_MAX},{0x7fffffff,0x80000000},
        {0xff,1},{0x100,0},{0xffff,1},{0xfffffffe,2},{0x80000001,1},{0x12345603,0},{0x800000ff,1},{3,0},{0x10101,0},
    }};
    std::vector<Input> result;
    const auto append=[&](std::uint32_t a,std::uint32_t b) { result.push_back({a,b,false});result.push_back({a,b,true}); };
    for (auto pair : edges) append(pair[0],pair[1]);
    // All 256 result bytes appear with different high bits and both incoming carry states.
    for (std::uint32_t byte=0;byte<256;++byte) append((byte&1 ? 0x87654300U : 0x12345600U)|byte,0);
    std::uint32_t state=0x12345678;
    for (unsigned n=0;n<96;++n) {
        state=state*1664525U+1013904223U;const auto a=state;
        state=state*1664525U+1013904223U;append(a,state);
    }
    return result;
}

void arithmeticFlagsAcrossOperandsAndBuffers()
{
    const auto corpus=inputs();
    for (const auto op : {Op::Cmp,Op::Add,Op::Sub,Op::Inc,Op::Dec,Op::Test,Op::Xor,Op::And,Op::Or}) for (unsigned form=0;form<((op==Op::Cmp || op==Op::Inc || op==Op::Dec) ? 4U : 8U);++form) {
        const bool logical = op==Op::Test || op==Op::Xor || op==Op::And || op==Op::Or;
        const bool unitStep = op==Op::Inc || op==Op::Dec;
        if (unitStep && form==3) continue;
        const auto opcode=op==Op::Cmp ? "cmp" : op==Op::Add ? "add" : op==Op::Sub ? "sub" : op==Op::Inc ? "inc" : op==Op::Dec ? "dec" : op==Op::Test ? "test" : op==Op::And ? "and" : op==Op::Or ? "or" : "xor";
        std::string source="streamin a,b,seed;memrefin data;streamout ";
        for (unsigned n=0;n<branches.size();++n) source+="f"+std::to_string(n)+",";
        source+="result,rhs;float one=1,zero=0;int cell=0,cursor=0;mov edx,seed;cmp edx,1;mov eax,a;mov ebx,b;";
        if (logical) {
            // Both incoming carry states have overflow set, so logical branches must replace OF as well as CF.
            source+="jb withCarry;mov edx,0x7fffffff;add edx,1;jmp seeded;withCarry:mov edx,0x80000000;add edx,0x80000000;seeded:";
        }
        if (form>=4) {
            const bool memory = form>=6;
            const auto cell = memory ? "[ecx+edx]" : "cell";
            if (memory) source+="mov ecx,data[0];mov edx,cursor;";
            source+=std::string("mov ")+cell+",eax;"+opcode+" "+cell+","+(form%2 ? "0x80000001" : "ebx")+";mov eax,"+cell+";";
        } else if (op==Op::Test && form==1) source+="test b,eax;";
        else if (unitStep) {
            if (form==0) source+=std::string(opcode)+" eax;";
            else if (form==1) source+="mov cell,eax;"+std::string(opcode)+" cell;mov eax,cell;";
            else source+="mov ecx,data[0];mov edx,cursor;mov [ecx+edx],eax;"+std::string(opcode)+" [ecx+edx];mov eax,[ecx+edx];";
        } else if (form==2 && op==Op::Cmp) source+="mov cell,eax;cmp cell,0x80000001;mov eax,cell;";
        else source+=std::string(opcode)+" eax,"+(form==0 ? "ebx" : form==1 ? "b" : form==2 ? "0x80000001" : "eax")+";";
        for (unsigned n=0;n<branches.size();++n) source+=branchRead(branches[n],n,"f"+std::to_string(n));
        source+="movd xmm0,eax;movaps result,xmm0;movd xmm0,ebx;movaps rhs,xmm0;";
        if ((unitStep && form==2) || form>=6) source+="add edx,4;mov cursor,edx;";
        const auto program=compileDspCode(source,DspLanguage::Assembler);
        std::array<std::vector<Mono4>,3> in;
        for (auto& port : in) port.resize(corpus.size());
        for (std::size_t n=0;n<corpus.size();++n) {
            in[0][n]={std::bit_cast<float>(corpus[n].a),9,-8,7};
            in[1][n]={std::bit_cast<float>(corpus[n].b),-9,8,-7};
            in[2][n].fill(std::bit_cast<float>(corpus[n].carry ? 0U : 2U));
        }
        for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);const auto block=graph.addDspCode(program);
            for (unsigned port=0;port<3;++port) graph.connectDspInput(graph.addInput(),block.id,port);
            auto runtime=graph.compileMono4(block.outputs);
            const openstone::green::Memory memory(std::vector<std::uint8_t>(corpus.size()*4));
            check(runtime.setDspMemRefInput(block.id,3,memory),"Bind integer arithmetic/logical memory");
            for (const std::size_t size : {1,3,7,31,64}) {
                runtime.reset();
                std::array<std::vector<Mono4>,outputCount> output;
                for (auto& port : output) port.resize(corpus.size());
                for (std::size_t offset=0;offset<corpus.size();offset+=size) {
                    const auto count=std::min(size,corpus.size()-offset);
                    std::array<Mono4InputBuffer,3> input;
                    std::array<Mono4OutputBuffer,outputCount> out;
                    for (unsigned port=0;port<input.size();++port) input[port]=std::span{in[port]}.subspan(offset,count);
                    for (unsigned port=0;port<out.size();++port) out[port]=std::span{output[port]}.subspan(offset,count);
                    check(runtime.process(input,out),"Render integer arithmetic flags");
                }
                for (std::size_t n=0;n<corpus.size();++n) {
                    auto sample=corpus[n];
                    if (!unitStep) sample.b=(form==2 || (form>=4 && form%2)) ? 0x80000001U : form==3 ? sample.a : sample.b;
                    const auto expected=reference(op,sample);
                    for (unsigned branch=0;branch<branches.size();++branch) {
                        const float value=taken(branch,expected) ? 1.F : 0.F;
                        if (output[branch][n]!=Mono4{value,value,value,value}) throw std::runtime_error(
                            std::string(opcode)+" form "+std::to_string(form)+" sample "+std::to_string(n)+" branch "+branches[branch]);
                    }
                    check(std::bit_cast<std::array<std::uint32_t,4>>(output[branches.size()][n])==std::array{expected.value,0U,0U,0U},
                          "Arithmetic wraps to 32 bits; CMP and TEST leave their operands unchanged");
                    check(std::bit_cast<std::array<std::uint32_t,4>>(output[branches.size()+1][n])==std::array{corpus[n].b,0U,0U,0U},
                          "Register and memory sources retain their original raw words");
                }
            }
        }
    }
}

void callsJoinsAndMixedProducers()
{
    constexpr std::array bodies{
        "call calculate;", "cmp choose,0;jz alternate;add eax,ebx;jmp join;alternate:sub eax,ebx;join:",
        "movaps xmm8,nan;comiss xmm8,nan;add eax,ebx;",
        "mov ecx,0;again:add eax,ebx;inc ecx;cmp ecx,3;jnz again;inc eax;",
    };
    const auto corpus=inputs();
    for (unsigned test=0;test<bodies.size();++test) {
        const auto program=compileDspCode(std::string("streamin a,b;monoin choose;streamout c,p,result;float one=1,zero=0,nan=0x7fc12345;")+
            "mov eax,a;mov ebx,b;"+bodies[test]+branchRead("jb",0,"c")+branchRead("jp",1,"p")+
            "movd xmm0,eax;movaps result,xmm0;"+(test==0 ? "jmp end;calculate:add eax,ebx;ret;end:" : ""),DspLanguage::Assembler);
        for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);const auto block=graph.addDspCode(program);
            for (unsigned port=0;port<3;++port) graph.connectDspInput(graph.addInput(),block.id,port);
            auto runtime=graph.compile(block.outputs);
            for (std::size_t n=0;n<corpus.size();n+=7) for (float choose : {0.F,1.F}) {
                const auto sample=corpus[n];
                auto expected=reference(test==1 && choose==0 ? Op::Sub : Op::Add,sample);
                if (test==3) {
                    auto value=sample.a;
                    for (unsigned loop=0;loop<3;++loop) value+=sample.b;
                    // The final CMP(3,3) clears carry; INC changes parity but preserves that clear carry.
                    expected=reference(Op::Inc,{value,0,false});
                }
                const std::array a{std::bit_cast<float>(sample.a)},b{std::bit_cast<float>(sample.b)},selector{choose};
                std::array<float,1> c{},p{},result{};
                check(runtime.process(std::array{InputBuffer{a},InputBuffer{b},InputBuffer{selector}},
                    std::array{OutputBuffer{c},OutputBuffer{p},OutputBuffer{result}}) && c[0]==expected.carry && p[0]==expected.parity &&
                    std::bit_cast<std::uint32_t>(result[0])==expected.value,
                    "Integer carry/parity cross calls, joins, COMISS overwrites and counted loops");
            }
        }
    }
    for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        for (const auto op : {Op::Inc,Op::Dec}) {
            // Unit steps produce parity without inventing an incoming carry.
            Graph graph(backend);const auto block=graph.addDspCode(compileDspCode(
                std::string("streamin a;streamout out;float one=1,zero=0;mov eax,a;")+(op==Op::Inc ? "inc" : "dec")+" eax;"+branchRead("jp",0,"out"),DspLanguage::Assembler));
            graph.connectDspInput(graph.addInput(),block.id,0);auto runtime=graph.compile(block.outputs);
            for (const auto sample : corpus) {
                const std::array a{std::bit_cast<float>(sample.a)};std::array<float,1> out{};
                check(runtime.process(std::array{InputBuffer{a}},std::array{OutputBuffer{out}}) && out[0]==reference(op,sample).parity,
                      "INC/DEC define parity even when carry has no producer");
            }
        }
    }
}

void decrementControlFlowAndWidths()
{
    constexpr std::array bodies{
        "call lower;",
        "cmp choose,0;jz memory;mov edx,seed;cmp edx,1;DEC eax;jmp joined;memory:mov edx,seed;cmp edx,1;mov cell,eax;dec cell;mov eax,cell;joined:",
        "mov ecx,3;again:dec eax;dec ecx;jnz again;dec eax;",
        "movaps xmm0,nan;comiss xmm0,zero;dec eax;",
        "jmp 2;dec eax;dec eax;",
        "mov cell,eax;jmp 6;dec cell;dec cell;mov eax,cell;",
        "movaps xmm0,one;comiss xmm0,zero;dec eax;",
    };
    const auto corpus=inputs();
    for (unsigned test=0;test<bodies.size();++test) {
        const auto program=compileDspCode(std::string("streamin a,seed;monoin choose;streamout c,p,z,l,g,result;")+
            "float one=1,zero=0,nan=0x7fc12345;int cell=0;mov edx,seed;cmp edx,1;mov eax,a;"+bodies[test]+
            branchRead("jb",0,"c")+branchRead("jp",1,"p")+branchRead("je",2,"z")+branchRead("jl",3,"l")+branchRead("jg",4,"g")+
            "movd xmm0,eax;movaps result,xmm0;"+(test==0 ? "jmp end;lower:dec eax;ret;end:" : ""),DspLanguage::Assembler);
        for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);const auto block=graph.addDspCode(program);
            for (unsigned port=0;port<3;++port) graph.connectDspInput(graph.addInput(),block.id,port);
            auto runtime=graph.compile(block.outputs);
            // Both carry states remain paired while sampling boundaries and the byte sweep.
            for (std::size_t n=0;n<corpus.size();n+=2) for (unsigned carry=0;carry<2;++carry) for (float choose : {0.F,1.F}) {
                auto sample=corpus[n+carry];
                if (test==2) sample.a-=3;
                if (test==3 || test==6) sample.carry=test==3;
                const auto expected=reference(Op::Dec,sample);
                const std::array a{std::bit_cast<float>(corpus[n+carry].a)},seed{std::bit_cast<float>(corpus[n+carry].carry ? 0U : 2U)},selector{choose};
                std::array<std::array<float,1>,6> out{};
                check(runtime.process(std::array{InputBuffer{a},InputBuffer{seed},InputBuffer{selector}},
                    std::array{OutputBuffer{out[0]},OutputBuffer{out[1]},OutputBuffer{out[2]},OutputBuffer{out[3]},OutputBuffer{out[4]},OutputBuffer{out[5]}}) &&
                    out[0][0]==expected.carry && out[1][0]==expected.parity && out[2][0]==expected.zero && out[3][0]==expected.less &&
                    out[4][0]==(!expected.less&&!expected.zero) && std::bit_cast<std::uint32_t>(out[5][0])==expected.value,
                    "DEC flags survive calls, joins, counted loops, COMISS and numeric skips without changing carry");
            }
        }
    }
    // These literal cases distinguish DEC's overflow, wraparound and preserved carry from SUB 1.
    for (bool carry : {false,true}) {
        const auto wrap=reference(Op::Dec,{0,0,carry});
        const auto overflow=reference(Op::Dec,{0x80000000U,0,carry});
        const auto zero=reference(Op::Dec,{1,0,carry});
        check(wrap.value==UINT32_MAX && wrap.less && !wrap.overflow && wrap.parity && wrap.carry==carry &&
              overflow.value==0x7fffffffU && overflow.overflow && overflow.less && overflow.parity && overflow.carry==carry &&
              zero.value==0 && zero.zero && zero.parity && zero.carry==carry,
              "DEC reference covers zero wrap, signed overflow, byte parity and both preserved carry states");
    }
    constexpr std::array invalid{
        "dec eax;", "movaps xmm0,zero;dec xmm0;", "mov eax,1;dec eax,1;", "dec 1;",
    };
    for (const auto body : invalid) {
        bool rejected=false;
        try { static_cast<void>(compileDspCode(std::string("streamout out;float zero=0;")+body,DspLanguage::Assembler)); }
        catch (const DspCompileError& e) { rejected=e.line>0&&e.column>0; }
        check(rejected,"DEC requires one initialized 32-bit integer register or writable memory operand");
    }
}

void signedAliasBoundariesAndControlFlow()
{
    struct Boundary { Op op; std::uint32_t a,b; bool ge,le; };
    // Literal EFLAGS expectations, including cases where the wrapped result has the opposite sign.
    constexpr Boundary tests[]{
        {Op::Cmp,0,0,true,true}, {Op::Cmp,1,0,true,false}, {Op::Cmp,0,1,false,true},
        {Op::Cmp,0x80000000,0x7fffffff,false,true}, {Op::Cmp,0x7fffffff,0x80000000,true,false},
        {Op::Add,0x7fffffff,1,true,false}, {Op::Add,0x80000000,UINT32_MAX,false,true},
        {Op::Add,0x80000000,0x80000000,false,true},
        {Op::Sub,0x80000000,1,false,true}, {Op::Sub,0x7fffffff,UINT32_MAX,true,false},
        {Op::Inc,0x7fffffff,0,true,false}, {Op::Inc,UINT32_MAX,0,true,true},
        {Op::Inc,0x80000000,0,false,true}, {Op::Dec,0x80000000,0,false,true},
        {Op::Dec,0,0,false,true}, {Op::Dec,1,0,true,true},
    };
    for (const auto op : {Op::Cmp,Op::Add,Op::Sub,Op::Inc,Op::Dec}) {
        const auto opcode=op==Op::Cmp ? "cmp" : op==Op::Add ? "add" : op==Op::Sub ? "sub" : op==Op::Inc ? "inc" : "dec";
        const auto source=std::string("streamin a,b,seed;streamout ge,le,c;float one=1,zero=0;")+
            "mov edx,seed;cmp edx,1;mov eax,a;mov ebx,b;"+opcode+" eax"+
            (op==Op::Inc || op==Op::Dec ? ";" : ",ebx;")+branchRead("JGE",0,"ge")+branchRead("JLE",1,"le")+branchRead("jb",2,"c");
        const auto program=compileDspCode(source,DspLanguage::Assembler);
        for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);const auto block=graph.addDspCode(program);
            for (unsigned port=0;port<3;++port) graph.connectDspInput(graph.addInput(),block.id,port);
            auto runtime=graph.compile(block.outputs);
            for (const auto& test : tests) if (test.op==op) for (bool carry : {false,true}) {
                const std::array a{std::bit_cast<float>(test.a)},b{std::bit_cast<float>(test.b)},seed{std::bit_cast<float>(carry ? 0U : 2U)};
                std::array<float,1> ge{},le{},c{};
                check(runtime.process(std::array{InputBuffer{a},InputBuffer{b},InputBuffer{seed}},
                    std::array{OutputBuffer{ge},OutputBuffer{le},OutputBuffer{c}}) && ge[0]==test.ge && le[0]==test.le &&
                    c[0]==reference(op,{test.a,test.b,carry}).carry,
                    "JGE/JLE use signed overflow and zero correctly after each integer producer without changing carry");
            }
        }
    }
    struct Flow { const char* body; std::uint32_t expected; };
    constexpr Flow flows[]{
        {"mov eax,2;again:dec eax;jge again;",UINT32_MAX},
        {"mov eax,0xfffffffe;again:inc eax;jle again;",1},
        {"mov eax,0;cmp eax,0;jnz unwanted;jmp 6;jge unwanted;mov eax,1;jmp done;unwanted:mov eax,2;done:",1},
        {"mov eax,0;cmp eax,0;jnz unwanted;jmp 6;jle unwanted;mov eax,1;jmp done;unwanted:mov eax,2;done:",1},
        {"call calculate;jge returned;mov eax,2;jmp done;returned:mov eax,1;jmp done;calculate:mov eax,0x7fffffff;inc eax;ret;done:",1},
    };
    for (const auto& test : flows) for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);const auto block=graph.addDspCode(compileDspCode(std::string("streamout out;")+test.body+
            "movd xmm0,eax;movaps out,xmm0;",DspLanguage::Assembler));
        auto runtime=graph.compile(block.outputs);std::array<float,3> out{};
        check(runtime.process({},std::array{OutputBuffer{out}}),"Render signed alias control flow");
        for (auto value : out) check(std::bit_cast<std::uint32_t>(value)==test.expected,
            "JGE/JLE support backward labels, local returns and six-byte native branch spans");
    }
    for (auto opcode : {"jge","jle"}) {
        bool rejected=false;
        try { static_cast<void>(compileDspCode(std::string("streamout out;")+opcode+" done;done:",DspLanguage::Assembler)); }
        catch (const DspCompileError& e) { rejected=std::string_view(e.what()).contains("branch requires an earlier"); }
        check(rejected,"Signed aliases still require a flag producer on every path");
    }
}

void logicalAndShiftFlagWriters()
{
    struct Case { const char* instruction; std::uint32_t input,result; bool carry; };
    constexpr Case tests[]{
        {"and eax,0xff",0x80000003,3,false}, {"or eax,1",0x80000000,0x80000001,false},
        {"shl eax,1",0x80000001,2,true}, {"shr eax,1",0x80000001,0x40000000,true},
        {"shl eax,31",2,0,true}, {"shr eax,31",0x40000000,0,true},
        {"shl eax,1",1,2,false}, {"shr eax,1",2,1,false},
    };
    for (const auto& test : tests) for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        const auto source=std::string("streamout c,p;float one=1,zero=0;mov eax,")+std::to_string(test.input)+";"+test.instruction+";"+
            branchRead("jb",0,"c")+branchRead("jp",1,"p");
        Graph graph(backend);const auto block=graph.addDspCode(compileDspCode(source,DspLanguage::Assembler));auto runtime=graph.compile(block.outputs);
        std::array<float,1> c{},p{};
        check(runtime.process({},std::array{OutputBuffer{c},OutputBuffer{p}}) && c[0]==test.carry &&
              p[0]==((std::popcount(test.result&0xffU)&1)==0),"Logical/shift flag writers replace carry and low-byte parity consistently");
    }
}

void logicalControlFlowAndWidths()
{
    constexpr std::array bodies{
        "call calculate;", "call calculate;",
        "cmp choose,0;jz alternate;xor eax,ebx;jmp joined;alternate:test eax,ebx;joined:",
        "mov ecx,3;again:xor eax,ebx;dec ecx;test ecx,ecx;jnz again;test eax,eax;",
        "jmp 2;test eax,ebx;", "jmp 6;test ecx,0x80000001;",
        "jmp 6;test cell,ebx;", "jmp 10;test cell,0x80000001;",
        "jmp 2;xor eax,ebx;", "jmp 6;xor ecx,0x80000001;",
        "jmp 6;xor cell,ebx;", "jmp 10;xor cell,0x80000001;",
    };
    const auto corpus=inputs();
    for (unsigned test=0;test<bodies.size();++test) {
        const auto code=std::string("streamin a,b,seed;monoin choose;streamout c,p,z,l,g,result,rhs;")+
            "float one=1,zero=0;int cell;mov eax,a;mov ebx,b;mov ecx,a;mov cell,eax;mov edx,seed;cmp edx,1;"+bodies[test]+
            branchRead("jb",0,"c")+branchRead("jp",1,"p")+branchRead("je",2,"z")+branchRead("jl",3,"l")+branchRead("jg",4,"g")+
            "movd xmm0,eax;movaps result,xmm0;movd xmm0,ebx;movaps rhs,xmm0;"+
            (test<2 ? std::string("jmp end;calculate:")+(test==0 ? "TEST eax,ebx;" : "XOR eax,ebx;")+"ret;end:" : "");
        const auto program=compileDspCode(code,DspLanguage::Assembler);
        for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);const auto block=graph.addDspCode(program);
            for (unsigned port=0;port<4;++port) graph.connectDspInput(graph.addInput(),block.id,port);
            auto runtime=graph.compile(block.outputs);
            for (std::size_t n=0;n<corpus.size();n+=7) for (float choose : {0.F,1.F}) {
                const auto sample=corpus[n];
                auto expected=reference(test==0 || (test==2 && choose==0) ? Op::Test : Op::Xor,sample);
                if (test>=4) expected={sample.a,sample.carry,true,!sample.carry,sample.carry,false};
                const std::array a{std::bit_cast<float>(sample.a)},b{std::bit_cast<float>(sample.b)},
                    seed{std::bit_cast<float>(sample.carry ? 0U : 1U)},selector{choose};
                std::array<std::array<float,1>,7> out{};
                check(runtime.process(std::array{InputBuffer{a},InputBuffer{b},InputBuffer{seed},InputBuffer{selector}},
                    std::array{OutputBuffer{out[0]},OutputBuffer{out[1]},OutputBuffer{out[2]},OutputBuffer{out[3]},OutputBuffer{out[4]},OutputBuffer{out[5]},OutputBuffer{out[6]}}) &&
                    out[0][0]==expected.carry && out[1][0]==expected.parity && out[2][0]==expected.zero && out[3][0]==expected.less &&
                    out[4][0]==(!expected.less&&!expected.zero) && std::bit_cast<std::uint32_t>(out[5][0])==expected.value &&
                    std::bit_cast<std::uint32_t>(out[6][0])==sample.b,
                    "TEST/XOR flags and untouched operands survive calls, mixed-producer joins, counted loops and numeric skips");
            }
        }
    }
}

void logicalZeroIdiomsAndDiagnostics()
{
    // These cases have no incoming numeric register or flag producer.
    constexpr std::array setups{
        "xor eax,eax;XOR ebx,ebx;xor ecx,ecx;xor edx,edx;",
        "mov eax,data[0];xor eax,eax;",
        "mov eax,helper;xor eax,eax;",
        "cmp choose,0;jz joined;mov eax,0x80000000;joined:xor eax,eax;",
        "mov eax,helper;mov cell,eax;test cell,0;mov eax,cell;call eax;",
    };
    for (unsigned test=0;test<setups.size();++test) {
        std::string code=std::string("memrefin data;monoin choose;streamout c,p,z,l,result;int cell;float zero=0,one=1;")+setups[test]+
            branchRead("jb",0,"c")+branchRead("jp",1,"p")+branchRead("je",2,"z")+branchRead("jl",3,"l")+"movd xmm0,eax;movaps result,xmm0;";
        if (test==0) code+="or eax,ebx;or eax,ecx;or eax,edx;movd xmm0,eax;movaps result,xmm0;";
        if (test==2 || test==4) code+="jmp end;helper:mov eax,0;ret;end:";
        const auto program=compileDspCode(code,DspLanguage::Assembler);
        for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);const auto block=graph.addDspCode(program);
            graph.connectDspInput(graph.addInput(),block.id,1);auto runtime=graph.compile(block.outputs);
            for (float choose : {0.F,1.F}) {
                const std::array input{choose};std::array<std::array<float,1>,5> out{};
                check(runtime.process(std::array{InputBuffer{input}},std::array{OutputBuffer{out[0]},OutputBuffer{out[1]},OutputBuffer{out[2]},OutputBuffer{out[3]},OutputBuffer{out[4]}}) &&
                    out[0][0]==0 && out[1][0]==1 && out[2][0]==1 && out[3][0]==0 && std::bit_cast<std::uint32_t>(out[4][0])==0,
                    "Self-XOR defines zero and flags without old data and clears pointer/target tags; TEST preserves a saved target cell");
            }
        }
    }
    constexpr std::array invalid{
        "test eax,eax;", "mov eax,1;test eax,ebx;", "test xmm0,eax;", "mov eax,1;test eax,xmm0;",
        "mov eax,1;test eax,a;", "test a,b;", "mov eax,1;test eax;", "mov eax,1;test eax,eax,1;",
        "xor eax,ebx;", "xor cell,a;", "xor xmm0,xmm0;", "xor rax,rax;", "xor eax,rax;", "xor eax,eax,1;",
        "mov eax,data[0];test eax,eax;", "mov eax,data[0];xor eax,1;",
        "mov eax,data[0];xor eax,eax;movaps xmm0,[eax];",
        "mov eax,helper;xor eax,eax;call eax;jmp end;helper:ret;end:",
        "mov eax,helper;mov cell,eax;xor cell,0;mov eax,cell;call eax;jmp end;helper:ret;end:",
        "xor eax,eax;stage(1){jz end;end:}", "mov eax,1;test eax,eax;stage(1){jp end;end:}",
    };
    for (const auto body : invalid) {
        bool rejected=false;
        try { static_cast<void>(compileDspCode(std::string("streamin a,b;memrefin data;streamout out;int cell;movaps xmm0,a;")+body,DspLanguage::Assembler)); }
        catch (const DspCompileError& e) { rejected=e.line>0&&e.column>0; }
        check(rejected,"TEST/XOR reject invalid/unwritten operands, opaque address arithmetic, stale tags and cross-stage flags");
    }
}

void invalidCarryPaths()
{
    constexpr std::array tests{
        "mov eax,0;inc eax;jb done;done:",
        "mov eax,0;dec eax;jb done;done:",
        "mov eax,0;dec eax;stage(1){jb done;done:}",
        "mov eax,0;inc eax;stage(1){jb done;done:}",
        "mov eax,data[0];add eax,4;mov ecx,0;inc ecx;jb done;done:",
    };
    for (auto body : tests) {
        bool rejected=false;
        try { static_cast<void>(compileDspCode(std::string("memrefin data;streamout out;")+body,DspLanguage::Assembler)); }
        catch (const DspCompileError& e) { rejected=std::string_view(e.what()).contains("branch requires"); }
        check(rejected,"INC/DEC preserve unavailable carry and opaque address arithmetic cannot supply integer flags");
    }
}

void memoryShiftFlags()
{
    auto corpus=inputs();corpus.resize(40);
    for(bool left:{false,true})for(unsigned count:{0U,1U,2U,7U,31U,32U,33U,63U,255U})for(unsigned form=0;form<4;++form) {
        const auto opcode=left ? "shl" : "shr";
        const std::string memory=form==0 ? "cell" : form==1 ? "cells[edx]" : "[ecx+edx]";
        std::string code="streamin a,b,seed;"+std::string(form==2 ? "memrefin data;" : form==3 ? "__const data;" : "")+"streamout ";
        for(unsigned n=0;n<branches.size();++n)code+="f"+std::to_string(n)+",";
        code+="result,address;float one=1,zero=0;int cell,cells[3],cursor;mov eax,a;mov edx,seed;cmp edx,1;";
        if(form==1)code+="mov edx,4;";
        if(form>=2)code+=std::string("mov ecx,")+(form==2 ? "data[0]" : "data")+";mov edx,cursor;";
        code+="mov "+memory+",eax;"+opcode+" "+memory+","+std::to_string(count)+";mov eax,"+memory+";";
        for(unsigned n=0;n<branches.size();++n)code+=branchRead(branches[n],n,"f"+std::to_string(n));
        code+="movd xmm0,eax;movaps result,xmm0;movd xmm0,edx;movaps address,xmm0;";
        if(form>=2)code+="add edx,4;mov cursor,edx;";
        const auto program=compileDspCode(code,DspLanguage::Assembler);
        for(auto backend:{DspBackend::Interpreter,DspBackend::LlvmOrc})if(dspBackendAvailable(backend)) {
            Graph graph(backend);const auto block=graph.addDspCode(program);
            for(unsigned p=0;p<3;++p)graph.connectDspInput(graph.addInput(),block.id,p);
            openstone::green::Memory owner(std::vector<std::uint8_t>(corpus.size()*4+9,0xa5));const auto memoryView=owner.view(3,corpus.size()*4,1);
            if(form==3)check(graph.bindDspEnginePointer(block.id,"data",memoryView,true),"Bind writable engine memory shift target");
            auto runtime=graph.compileMono4(block.outputs);
            if(form==2)check(runtime.setDspMemRefInput(block.id,3,memoryView),"Bind bounded memory shift target");
            std::array<std::vector<Mono4>,3> in;for(auto& p:in)p.resize(corpus.size());
            for(unsigned n=0;n<corpus.size();++n){in[0][n].fill(std::bit_cast<float>(corpus[n].a));in[2][n].fill(std::bit_cast<float>(n%3));}
            for(const std::size_t size:{1,3,7,31,64,257}) {
                runtime.reset();std::array<std::vector<Mono4>,outputCount> output;for(auto& p:output)p.resize(corpus.size());
                for(unsigned offset=0;offset<corpus.size();offset+=size) {
                    const auto frames=std::min(size,corpus.size()-offset);std::array<Mono4InputBuffer,3> input;std::array<Mono4OutputBuffer,outputCount> out;
                    for(unsigned p=0;p<3;++p)input[p]=std::span{in[p]}.subspan(offset,frames);
                    for(unsigned p=0;p<out.size();++p)out[p]=std::span{output[p]}.subspan(offset,frames);
                    check(runtime.process(input,out),"Render memory shifts and their defined integer flags");
                }
                for(unsigned n=0;n<corpus.size();++n) {
                    const auto amount=count&31U,a=corpus[n].a;
                    auto expected=reference(Op::Cmp,{n%3,1,false});expected.value=a;
                    if(amount) {
                        const auto result=left ? a<<amount : a>>amount;
                        const bool carry=((a>>(left ? 32-amount : amount-1))&1U)!=0;
                        const bool overflow=amount==1 && (left ? bool(result&0x80000000U)!=carry : bool(a&0x80000000U));
                        expected={result,carry,(std::popcount(result&255U)&1)==0,result==0,bool(result&0x80000000U)!=overflow,overflow};
                    }
                    for(unsigned branch=0;branch<branches.size();++branch) {
                        if(amount>1 && branch>=10)continue; // OF, and hence signed comparisons, is undefined for larger shifts.
                        const float bit=taken(branch,expected) ? 1.F : 0.F;
                        check(output[branch][n]==Mono4{bit,bit,bit,bit},"Memory shifts preserve zero-count flags and match CF/PF/ZF and one-bit overflow");
                    }
                    check(std::bit_cast<std::array<std::uint32_t,4>>(output[branches.size()][n])==std::array{expected.value,0U,0U,0U},"Memory shift results wrap at 32 bits and mask the count to five bits");
                    const auto address=form==0 ? n%3 : form==1 ? 4U : n*4;
                    check(std::bit_cast<std::array<std::uint32_t,4>>(output[branches.size()+1][n])==std::array{address,0U,0U,0U},"A memory shift leaves its integer address register unchanged");
                    if(form>=2)check(memoryView.word(n)==expected.value,"Mem and engine targets receive exactly the expected shifted DWORD");
                }
                for(unsigned n=0;n<3;++n)check(owner.byte(n)==0xa5,"A memory shift preserves owner prefix guards");
                for(unsigned n=3+corpus.size()*4;n<owner.size();++n)check(owner.byte(n)==0xa5,"A memory shift preserves owner suffix guards");
            }
        }
    }
}

void aliasedMemoryAndInputCopies()
{
    constexpr std::array positions{-4,0,1,4,9,12,13,16,1000000};
    constexpr std::size_t frames=37;
    constexpr std::array ops{"add","sub","and","or","xor"};
    for(unsigned which=0;which<ops.size();++which)for(auto backend:{DspBackend::Interpreter,DspBackend::LlvmOrc})if(dspBackendAvailable(backend)) {
        const auto operation=which==0 ? Op::Add : which==1 ? Op::Sub : which==2 ? Op::And : which==3 ? Op::Or : Op::Xor;
        Graph graph(backend);const auto b=graph.addDspCode(compileDspCode("streamin index;memrefin mem;streamout value,address,window;mov ecx,mem[0];mov edx,index;"+std::string(ops[which])+" [ecx+edx],edx;mov eax,[ecx+edx];movd xmm0,eax;movaps value,xmm0;movd xmm0,edx;movaps address,xmm0;movaps xmm0,[ecx];movaps window,xmm0;",DspLanguage::Assembler));
        graph.connectDspInput(graph.addInput(),b.id,0);auto runtime=graph.compileMono4(b.outputs);
        std::array<Mono4,frames> indices{};for(unsigned n=0;n<frames;++n)indices[n].fill(std::bit_cast<float>(static_cast<std::uint32_t>(positions[n%positions.size()])));
        for(const std::size_t size:{1,3,7,31,64,257}) {
            runtime.reset();openstone::green::Memory owner(std::vector<std::uint8_t>(29,0x5a));const auto memory=owner.view(3,17,1);
            check(runtime.setDspMemRefInput(b.id,1,memory),"Bind an unaligned bounded Mem view for aliased read-modify-write");
            auto expected=owner.snapshotBytes();
            const auto read=[&](int offset){std::uint32_t word=0;if(offset>=0 && offset<=13)for(unsigned i=0;i<4;++i)word|=std::uint32_t{expected[3+offset+i]}<<(8*i);return word;};
            for(unsigned offset=0;offset<frames;offset+=size) {
                const auto count=std::min(size,frames-offset);std::array<std::array<Mono4,frames>,3> out{};
                check(runtime.process(std::array{Mono4InputBuffer{std::span{indices}.subspan(offset,count)}},std::array{Mono4OutputBuffer{std::span{out[0]}.first(count)},Mono4OutputBuffer{std::span{out[1]}.first(count)},Mono4OutputBuffer{std::span{out[2]}.first(count)}}),"Modify memory with a source register that also indexes the destination");
                for(unsigned n=0;n<count;++n) {
                    const auto position=positions[(offset+n)%positions.size()];const auto result=reference(operation,{read(position),static_cast<std::uint32_t>(position),false}).value;
                    if(position>=0 && position<=13)for(unsigned byte=0;byte<4;++byte)expected[3+position+byte]=static_cast<std::uint8_t>(result>>(8*byte));
                    check(std::bit_cast<std::array<std::uint32_t,4>>(out[0][n])==std::array{read(position),0U,0U,0U},"Each bounded memory result matches an independent byte-update model");
                    check(std::bit_cast<std::array<std::uint32_t,4>>(out[1][n])==std::array{static_cast<std::uint32_t>(position),0U,0U,0U},"The aliased source/address register retains its original DWORD");
                    check(std::bit_cast<std::array<std::uint32_t,4>>(out[2][n])==std::array{read(0),read(4),read(8),read(12)},"Subsequent packed reads see updated bytes and untouched neighbors");
                }
                check(owner.snapshotBytes()==expected,"Read-modify-write cannot escape the supplied Mem view");
            }
        }
        Graph copies(backend);const auto root=copies.addInput();const auto c=copies.addDspCode(compileDspCode("streamin value,index;streamout out;mov edx,index;"+std::string(ops[which])+" value[edx],0x80000001;movaps xmm0,value;movaps out,xmm0;",DspLanguage::Assembler));
        copies.connectDspInput(root,c.id,0);copies.connectDspInput(copies.addInput(),c.id,1);auto run=copies.compileMono4(std::array{c.outputs[0],root});
        std::array<Mono4,frames> input{},index{},result{},peer{};
        for(unsigned n=0;n<frames;++n){input[n]=std::bit_cast<Mono4>(std::array{n,0x7fffffffU,0x80000000U,0xffffffffU});index[n].fill(std::bit_cast<float>((n%4)*4));}
        const auto original=input;
        for(const std::size_t size:{1,3,7,31,64,257}) {
            run.reset();
            for(unsigned offset=0;offset<frames;offset+=size){const auto count=std::min(size,frames-offset);check(run.process(std::array{Mono4InputBuffer{std::span{input}.subspan(offset,count)},Mono4InputBuffer{std::span{index}.subspan(offset,count)}},std::array{Mono4OutputBuffer{std::span{result}.subspan(offset,count)},Mono4OutputBuffer{std::span{peer}.subspan(offset,count)}}),"Apply memory-destination operations to private stream input slots");}
            for(unsigned n=0;n<frames;++n){auto words=std::bit_cast<std::array<std::uint32_t,4>>(input[n]);words[n%4]=reference(operation,{words[n%4],0x80000001U,false}).value;check(std::bit_cast<std::array<std::uint32_t,4>>(result[n])==words && std::bit_cast<std::array<std::uint32_t,4>>(peer[n])==std::bit_cast<std::array<std::uint32_t,4>>(original[n]),"Private input modifications preserve other lanes and upstream fan-out");}
            check(std::bit_cast<std::array<std::array<std::uint32_t,4>,frames>>(input)==std::bit_cast<std::array<std::array<std::uint32_t,4>,frames>>(original),"Memory destinations never mutate caller buffers");
        }
    }
}

void memoryDestinationContracts()
{
    for(auto backend:{DspBackend::Interpreter,DspBackend::LlvmOrc})if(dspBackendAvailable(backend)) {
        for(const auto shift:{"shl","shr"})for(unsigned count:{0U,32U}) {
            Graph graph(backend);const auto b=graph.addDspCode(compileDspCode("streamout out;int table[2];mov eax,leaf;mov table[1],eax;mov ebx,0x7fffffff;add ebx,1;"+std::string(shift)+" table[1],"+std::to_string(count)+";mov eax,table[1];call eax;jmp done;leaf:jge yes;movaps xmm0,_ZERO;movaps out,xmm0;ret;yes:movaps xmm0,_ONE;movaps out,xmm0;ret;done:",DspLanguage::Assembler));
            auto run=graph.compileMono4(b.outputs);std::array<Mono4,3> out{};check(run.process({},std::array{Mono4OutputBuffer{out}}),"Zero-count memory shifts preserve callable targets and overflow-dependent flags");for(auto packet:out)check(packet==Mono4{1,1,1,1},"Count-zero shifts retain target provenance and earlier SF/OF");
        }
        Graph ro(backend);const auto b=ro.addDspCode(compileDspCode("__const data;streamout out;mov ecx,data;mov ebx,0xffffffff;add [ecx],ebx;and [ecx+4],0;shl [ecx+8],7;movaps xmm0,[ecx];movaps out,xmm0;",DspLanguage::Assembler));
        const auto memory=openstone::green::Memory::fromSamples(std::array{1.F,2.F,3.F,4.F});const auto original=memory.snapshotBytes();check(ro.bindDspEnginePointer(b.id,"data",memory,false),"Bind read-only engine storage");auto run=ro.compileMono4(b.outputs);std::array<Mono4,3> out{};check(run.process({},std::array{Mono4OutputBuffer{out}}),"Read-only engine views tolerate bounded writes without changing backing data");for(auto packet:out)check(packet==Mono4{1,2,3,4},"Arithmetic, logic and shifts cannot write read-only engine regions");check(memory.snapshotBytes()==original,"Read-only storage retains every byte");
    }
    for(const auto body:{"add cell,a;","sub cell,a;","and cell,a;","or cell,a;","shl cell,a;","add data[1],1;","and _ONE,1;","shl _ONE,0;","floatarrayin external[2];or external[0],1;","floatarrayin external[2];shr external[0],32;","double wide;add wide,1;","mov eax,leaf;mov cell,eax;add cell,0;mov eax,cell;call eax;jmp done;leaf:ret;done:","mov cell,0;shl cell,0;jz done;done:"}) {
        bool rejected=false;try{static_cast<void>(compileDspCode("streamin a;memrefin data;streamout out;int cell;"+std::string(body),DspLanguage::Assembler));}catch(const DspCompileError& e){rejected=e.line&&e.column;}check(rejected,"Memory destinations retain operand-width, memory-to-memory, read-only, target-provenance and flag-availability checks");
    }
}

void memoryStagesAndFrames()
{
    const auto program=compileDspCode("streamout early,current,late,frameValue;int cell;"
        "stage0;mov cell,1;add cell,1;mov [rbp+64],10;and [rbp+64],15;"
        "stage1;add cell,1;add [rbp+64],1;mov eax,cell;movd xmm0,eax;cvtdq2ps xmm0,xmm0;movaps early,xmm0;"
        "stage2;sub cell,1;sub [rbp+64],1;mov eax,cell;movd xmm0,eax;cvtdq2ps xmm0,xmm0;movaps current,xmm0;mov eax,[rbp+64];movd xmm0,eax;cvtdq2ps xmm0,xmm0;movaps frameValue,xmm0;"
        "stage3;or cell,4;xor [rbp+64],1;mov eax,cell;movd xmm0,eax;cvtdq2ps xmm0,xmm0;movaps late,xmm0;",DspLanguage::Assembler);
    for(auto backend:{DspBackend::Interpreter,DspBackend::LlvmOrc})if(dspBackendAvailable(backend)) {
        Graph graph(backend);const auto block=graph.addDspCode(program);auto run=graph.compileMono4(block.outputs);
        for(const std::size_t size:{1,3,7,31,64,257}) {
            run.reset();std::array<std::array<Mono4,17>,4> out{};
            for(unsigned offset=0;offset<17;offset+=size){const auto count=std::min<std::size_t>(size,17-offset);std::array<Mono4OutputBuffer,4> output;for(unsigned p=0;p<4;++p)output[p]=std::span{out[p]}.subspan(offset,count);check(run.process({},output),"Run memory destinations in initialization, early, processing and late stages");}
            for(unsigned n=0;n<17;++n){check(out[0][n]==Mono4{n ? 7.F : 3.F,0,0,0} && out[1][n]==Mono4{n ? 6.F : 2.F,0,0,0} && out[2][n]==Mono4{n ? 6.F : 0.F,0,0,0},"Local memory mutations keep existing stage order and one-sample late-output visibility");check(out[3][n]==Mono4{10.F+float(n%2),0,0,0},"Native frame memory mutations persist across stages and host buffers, then reset cleanly");}
        }
    }
}
}
int main()
{
    try { arithmeticFlagsAcrossOperandsAndBuffers();callsJoinsAndMixedProducers();decrementControlFlowAndWidths();signedAliasBoundariesAndControlFlow();logicalAndShiftFlagWriters();logicalControlFlowAndWidths();logicalZeroIdiomsAndDiagnostics();invalidCarryPaths();memoryShiftFlags();aliasedMemoryAndInputCopies();memoryDestinationContracts();memoryStagesAndFrames(); }
    catch (const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
