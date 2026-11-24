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
constexpr std::size_t ports=branches.size()+2;
enum class Op { And,Or,Xor,Test };
const char* name(Op op) {return op==Op::And ? "and" : op==Op::Or ? "or" : op==Op::Xor ? "xor" : "test";}
void check(bool value,std::string_view message) {if (!value) throw std::runtime_error(std::string(message));}
unsigned shift(unsigned alias) {return alias<4 ? 0 : 8;}
std::uint32_t byte(std::uint32_t word,unsigned alias) {return (word>>shift(alias))&255U;}
std::uint32_t replace(std::uint32_t word,unsigned alias,unsigned value) {return (word&~(255U<<shift(alias)))|((value&255U)<<shift(alias));}
struct Sample {unsigned a,b;bool seed;};
struct Result {unsigned value;bool carry,parity,zero,less,overflow;};
Result reference(Op op,unsigned a,unsigned b)
{
    const unsigned result=op==Op::And || op==Op::Test ? a&b : op==Op::Or ? a|b : a^b;
    return {result,false,(std::popcount(result)&1)==0,result==0,bool(result&128U),false};
}
auto branchValues(Result r)
{
    return std::array{r.zero,r.zero,!r.zero,!r.zero,!r.carry&&!r.zero,r.carry,r.carry||r.zero,!r.carry,
        r.parity,!r.parity,!r.less&&!r.zero,r.less,r.less||r.zero,!r.less,!r.less,r.less||r.zero};
}
std::string header()
{
    std::string code="streamin a,b,seed;memrefin data;floatarrayin samples[4];streamout ";
    for (unsigned i=0;i<branches.size();++i) code+="f"+std::to_string(i)+",";
    return code+"result,rhs;float zero=0,one=1,nan=0x7fc12345;int cells[4]={0x01234567,0x89abcdef,0x7f812345,0xffc54321},cursor=0;double wide[2]={0x0123456789abcdef,0xfff8000000000042};";
}
std::string seedFlags()
{
    return "mov edx,seed;cmp edx,0;je withoutCarry;mov edx,0x80000000;add edx,0x80000000;jmp seeded;"
        "withoutCarry:mov edx,0x7fffffff;add edx,1;seeded:";
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
struct Environment {std::fenv_t saved;Environment(){std::fegetenv(&saved);}~Environment(){std::fesetenv(&saved);}};
using Buffers=std::array<std::array<Mono4,64>,ports>;
void checkFlags(const Buffers& outputs,unsigned frame,Result result,const std::string& context)
{
    const auto flags=branchValues(result);
    for (unsigned port=0;port<branches.size();++port) {
        const float value=flags[port] ? 1.F : 0.F;
        if (outputs[port][frame]!=Mono4{value,value,value,value}) throw std::runtime_error(context+" branch "+branches[port]);
    }
}
std::vector<Sample> edgeSamples()
{
    constexpr std::array edges{0U,1U,2U,3U,0x7eU,0x7fU,0x80U,0x81U,0xfeU,0xffU};
    std::vector<Sample> result;
    for (auto a : edges) for (auto b : edges) for (bool carry : {false,true}) result.push_back({a,b,carry});
    for (unsigned n=0;n<256;++n) result.push_back({n,(n*73+19)&255U,bool(n&1)});
    return result;
}
std::uint32_t original(unsigned n) {return 0xa5aa55ccU^(n*0x9e3779b9U);}
void registerCase(Op op,unsigned left,unsigned right,const std::vector<Sample>& samples,int immediate=999,std::string_view flow={},bool exhaustive=false)
{
    const bool same=immediate==999 && left%4==right%4;
    const auto rhsParent=immediate==999 ? right%4 : (left+1)%4;
    const auto operation=std::string(name(op))+" "+aliases[left]+","+(immediate==999 ? aliases[right] : std::to_string(immediate))+";";
    std::string code=header()+seedFlags()+"mov "+parents[left%4]+",a;";
    if (!same) code+="mov "+std::string(parents[rhsParent])+",b;";
    if (flow=="mixed") code+="cmp seed,0;je alternate;"+operation+"jmp joined;alternate:xor "+aliases[left]+","+aliases[right]+";joined:";
    else code+=flow=="call" ? "call calculate;" : flow=="join" ? "cmp seed,0;je alternate;"+operation+"jmp joined;alternate:"+operation+"joined:" : operation;
    code+=readFlags()+capture(parents[left%4],"result")+capture(parents[rhsParent],"rhs");
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
                    auto a=replace(original(n),left,sample.a);
                    if (same && left!=right) a=replace(a,right,sample.b);
                    const auto b=same ? a : replace(original(n)^0x6e198a54U,right,sample.b);
                    input[0][i]=std::bit_cast<Mono4>(Words{a,0x7f812345,0x80000001,0x7fc12345});
                    input[1][i]=std::bit_cast<Mono4>(Words{b,0xff812345,0x80000100,0xffc12345});
                    input[2][i].fill(std::bit_cast<float>(sample.seed ? 1U : 0U));
                }
                std::array<Mono4InputBuffer,3> in;std::array<Mono4OutputBuffer,ports> out;
                for (unsigned port=0;port<3;++port) in[port]=std::span{input[port]}.first(count);
                for (unsigned port=0;port<ports;++port) out[port]=std::span{output[port]}.first(count);
                check(runtime.process(in,out),"Render byte logical operations register/immediate forms");
                for (unsigned i=0;i<count;++i) {
                    const auto n=static_cast<unsigned>(offset+i);const auto a=std::bit_cast<Words>(input[0][i])[0],b=std::bit_cast<Words>(input[1][i])[0];
                    const auto actualOp=flow=="mixed" && !samples[n].seed ? Op::Xor : op;
                    const auto expected=reference(actualOp,byte(a,left),immediate==999 ? byte(same ? a : b,right) : static_cast<unsigned>(immediate)&255U);
                    const auto context=std::string(name(op))+" "+aliases[left]+","+(immediate==999 ? aliases[right] : std::to_string(immediate))+" sample "+std::to_string(n);
                    checkFlags(output,i,expected,context);
                    const auto result=actualOp==Op::Test ? a : replace(a,left,expected.value);
                    check(std::bit_cast<Words>(output[branches.size()][i])==Words{result,0,0,0},"Byte logic preserves all unselected parent bits; TEST preserves the complete parent");
                    check(std::bit_cast<Words>(output[branches.size()+1][i])==Words{same ? result : b,0,0,0},"Source parents remain unchanged except for an explicitly aliased destination byte");
                }
            }
            check(std::fegetround()==FE_UPWARD && std::fetestexcept(FE_ALL_EXCEPT)==FE_DIVBYZERO,"Byte logical operations preserve caller rounding and exception state");
        }
    }
}
void registersAndFlags()
{
    const auto edges=edgeSamples();std::vector<Sample> exhaustive;
    for (unsigned a=0;a<256;++a) for (unsigned b=0;b<256;++b) exhaustive.push_back({a,b,bool((a^b)&1)});
    for (auto op : {Op::And,Op::Or,Op::Xor,Op::Test}) {
        registerCase(op,4,1,exhaustive,999,{},true);
        for (unsigned left=0;left<8;++left) for (unsigned right=0;right<8;++right) registerCase(op,left,right,edges);
        for (unsigned left=0;left<8;++left) for (int value : {-128,-1,0,1,127,128,255}) registerCase(op,left,1,edges,value);
        registerCase(op,4,0,edges,999,"call");registerCase(op,0,4,edges,999,"join");
    }
    registerCase(Op::And,4,0,edges,999,"mixed");
    const auto a=reference(Op::And,0x80,0xff),b=reference(Op::Or,0,0x80),c=reference(Op::Xor,0x80,0x80),d=reference(Op::Test,0xf0,0x0f);
    check(a.value==0x80 && a.less && !a.carry && !a.overflow && b.value==0x80 && b.less && !b.parity &&
        c.value==0 && c.zero && c.parity && !c.less && d.zero && d.parity && !d.carry && !d.overflow,
        "Literal byte logic cases pin sign bit 7, cleared carry/overflow, even parity and zero results");
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
void memoryOperands()
{
    const auto corpus=edgeSamples();
    for (auto op : {Op::And,Op::Or,Op::Xor,Op::Test}) for (unsigned kind=0;kind<3;++kind) for (bool memoryDestination : {false,true}) for (unsigned left : {0U,4U}) {
        if (op==Op::Test && !memoryDestination) continue;
        const std::string target=kind==0 ? "[ecx+edx-1]" : kind==1 ? "cells[edx-1]" : "wide[edx-1]";
        const auto operation=std::string(name(op))+" "+(memoryDestination ? target+",bh;" : std::string(aliases[left])+","+target+";");
        std::string code=header()+seedFlags()+"mov eax,a;mov ebx,b;mov ecx,data[0];mov edx,cursor;mov "+target+","+(memoryDestination ? aliases[left] : "bh")+";"+operation;
        if (memoryDestination) code+="movaps xmm0,"+std::string(kind==0 ? "[ecx]" : kind==1 ? "cells[0]" : "wide[0]")+";movaps result,xmm0;";
        else code+=capture("eax","result");
        code+=capture("ebx","rhs")+readFlags()+"inc edx;cmp edx,19;jb keep;mov edx,0;keep:mov cursor,edx;";
        const auto program=compileDspCode(code,DspLanguage::Assembler);
        Environment environment;
        for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);const auto block=graph.addDspCode(program);
            for (unsigned port=0;port<3;++port) graph.connectDspInput(graph.addInput(),block.id,port);
            auto runtime=graph.compileMono4(block.outputs);
            for (const std::size_t size : {1,3,7,31,64}) {
                runtime.reset();std::vector<std::uint8_t> backing(29);
                for (unsigned i=0;i<backing.size();++i) backing[i]=static_cast<std::uint8_t>(i*73+19);
                const openstone::green::Memory memory(backing);
                check(runtime.setDspMemRefInput(block.id,3,memory.view(5,17,1)),"Bind unaligned byte logical operations Mem view");
                auto expected=kind==0 ? std::vector<std::uint8_t>(backing.begin()+5,backing.begin()+22) : kind==1 ? rawBytes({0x01234567,0x89abcdef,0x7f812345,0xffc54321}) : rawBytes({0x89abcdef,0x01234567,0x42,0xfff80000});
                std::array<std::array<Mono4,64>,3> input{};Buffers output{};
                std::fesetround(FE_UPWARD);std::feclearexcept(FE_ALL_EXCEPT);std::feraiseexcept(FE_DIVBYZERO);
                for (std::size_t offset=0;offset<corpus.size();offset+=size) {
                    const auto count=std::min(size,corpus.size()-offset);
                    for (unsigned i=0;i<count;++i) {
                        const auto n=static_cast<unsigned>(offset+i);const auto sample=corpus[n];
                        input[0][i]=std::bit_cast<Mono4>(Words{replace(original(n),left,sample.a),0x80000001,0x7f812345,0xffc12345});
                        input[1][i]=std::bit_cast<Mono4>(Words{replace(original(n)^0x914acddeU,5,sample.b),0xff812345,0x80000100,0x7fc12345});
                        input[2][i].fill(std::bit_cast<float>(sample.seed ? 1U : 0U));
                    }
                    std::array<Mono4InputBuffer,3> in;std::array<Mono4OutputBuffer,ports> out;
                    for (unsigned port=0;port<3;++port) in[port]=std::span{input[port]}.first(count);
                    for (unsigned port=0;port<ports;++port) out[port]=std::span{output[port]}.first(count);
                    check(runtime.process(in,out),"Render byte memory arithmetic and comparison");
                    for (unsigned i=0;i<count;++i) {
                        const auto n=static_cast<unsigned>(offset+i),a=std::bit_cast<Words>(input[0][i])[0],b=std::bit_cast<Words>(input[1][i])[0];
                        const auto position=static_cast<int>(n%19)-1;const bool valid=position>=0 && static_cast<unsigned>(position)<expected.size();
                        if (valid) expected[position]=static_cast<std::uint8_t>(byte(memoryDestination ? a : b,memoryDestination ? left : 5));
                        const auto result=reference(op,memoryDestination ? valid ? byte(a,left) : 0U : byte(a,left),memoryDestination ? byte(b,5) : valid ? byte(b,5) : 0U);
                        if (memoryDestination && op!=Op::Test && valid) expected[position]=static_cast<std::uint8_t>(result.value);
                        const auto word=op==Op::Test ? a : replace(a,left,result.value);
                        check(std::bit_cast<Words>(output[branches.size()][i])==(memoryDestination ? rawWords(expected) : Words{word,0,0,0}),"Byte logic preserves neighbors and unselected parent bits; TEST leaves all input bytes unchanged");
                        check(std::bit_cast<Words>(output[branches.size()+1][i])==Words{b,0,0,0},"Memory arithmetic preserves the complete high-byte source parent");
                        checkFlags(output,i,result,std::string(name(op))+" memory form "+std::to_string(kind)+" sample "+std::to_string(n));
                    }
                    if (kind==0) {
                        std::copy(expected.begin(),expected.end(),backing.begin()+5);
                        check(memory.snapshotBytes()==backing,"Only selected bytes of the bound Mem view change; external guards and all neighbors survive");
                    }
                }
                check(std::fegetround()==FE_UPWARD && std::fetestexcept(FE_ALL_EXCEPT)==FE_DIVBYZERO,"Byte memory arithmetic preserves caller floating-point state");
            }
        }
    }
}

std::vector<std::uint8_t> literalMemory()
{
    std::vector<std::uint8_t> data(17);data[0]=5;data[1]=6;data[2]=7;data[3]=222;return data;
}
void literal(const std::string& body,std::uint32_t expectedA,std::uint32_t expectedB,Result flags,std::vector<std::uint8_t> memoryExpected=literalMemory())
{
    const auto program=compileDspCode(header()+body+readFlags()+capture("eax","result")+capture("ebx","rhs"),DspLanguage::Assembler);
    for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
        if (!dspBackendAvailable(backend)) continue;
        Graph graph(backend);const auto block=graph.addDspCode(program);auto runtime=graph.compileMono4(block.outputs);
        for (const std::size_t size : {1,3,7,31,64}) {
            runtime.reset();const openstone::green::Memory memory(literalMemory());
            check(runtime.setDspMemRefInput(block.id,3,memory),"Bind literal byte logical operations memory");
            Buffers output{};
            for (std::size_t offset=0;offset<67;offset+=size) {
                const auto count=std::min(size,67-offset);std::array<Mono4OutputBuffer,ports> out;
                for (unsigned port=0;port<ports;++port) out[port]=std::span{output[port]}.first(count);
                check(runtime.process({},out),"Render byte alias/control-flow literal");
                for (unsigned i=0;i<count;++i) {
                    checkFlags(output,i,flags,"Byte literal "+body);
                    check(std::bit_cast<Words>(output[branches.size()][i])==Words{expectedA,0,0,0} && std::bit_cast<Words>(output[branches.size()+1][i])==Words{expectedB,0,0,0},"Byte literals preserve parent bits, original effective addresses and known numeric jump widths");
                }
            }
            check(memory.snapshotBytes()==memoryExpected,"Byte logical operations changes only the intended neighboring memory byte");
        }
    }
}
void aliasesWidthsAndDiagnostics()
{
    for (auto op : {Op::And,Op::Or,Op::Xor,Op::Test}) {
        if (op!=Op::Test) literal("mov ecx,data[0];mov eax,3;mov ebx,0x12345678;"+std::string(name(op))+" ah,[ecx+eax];",
            op==Op::And ? 3U : 0xde03U,0x12345678U,reference(op,0,222));
        for (auto source : {"al","ah"}) {
            const unsigned b=std::string_view(source)=="al" ? 3 : 2;const auto flags=reference(op,127,b);
            auto expected=literalMemory();expected[13]=static_cast<std::uint8_t>(op==Op::Test ? 127 : flags.value);
            literal("mov ecx,data[0];mov eax,0x203;mov ebx,127;mov [ecx+eax*4-2047],bl;"+std::string(name(op))+" [ecx+eax*4-2047],"+source+";mov ebx,0;mov bl,[ecx+13];",
                0x203U,expected[13],flags,expected);
        }
        for (unsigned reg=0;reg<8;++reg) {
            const auto regName=std::string(aliases[reg]);
            const std::string setup="mov eax,0x12345678;mov ebx,0xabcdef01;mov ecx,0;cmp ecx,1;";
            const Result flags{255,true,true,false,true,false}; // CMP(0,1), retained across the skipped instruction.
            literal("mov edx,0x55aa6677;"+setup+"jmp 2;"+name(op)+" "+regName+","+regName+";",0x12345678,0xabcdef01,flags);
            literal("mov edx,0x55aa6677;"+setup+"jmp 6;"+name(op)+" cells[0],"+regName+";",0x12345678,0xabcdef01,flags);
            if (op!=Op::Test) literal("mov edx,0x55aa6677;"+setup+"jmp 6;"+name(op)+" "+regName+",cells[0];",0x12345678,0xabcdef01,flags);
            if (reg!=0) literal("mov edx,0x55aa6677;"+setup+"jmp 3;"+name(op)+" "+regName+",-1;",0x12345678,0xabcdef01,flags);
        }
        if (op==Op::Test) literal("mov eax,0x12345678;mov ebx,0;test a[0],bh;",0x12345678,0,reference(op,0,0));
        const auto immediate=op==Op::And || op==Op::Test ? "-1" : "0";
        for (auto prefix : {"", "mov ecx,0;cmp ecx,1;mov ecx,data[0];add ecx,1;", "movaps xmm8,nan;comiss xmm8,nan;"}) {
            literal(std::string(prefix)+"mov eax,0x1234f080;mov ebx,0x00808080;"+name(op)+" al,"+immediate+";",0x1234f080,0x00808080,reference(op,0x80,static_cast<unsigned>(op==Op::And || op==Op::Test ? 255 : 0)));
        }
        for (auto body : {std::string(name(op))+" ah,1;", "mov eax,0;"+std::string(name(op))+" ah,256;", "mov eax,0;"+std::string(name(op))+" ah,-129;",
            "mov eax,0;"+std::string(name(op))+" ah,eax;", "mov eax,0;"+std::string(name(op))+" eax,ah;", "mov eax,data[0];"+std::string(name(op))+" ah,0;",
            "mov eax,0;mov ebx,data[0];"+std::string(name(op))+" al,bh;", "mov eax,routine;"+std::string(name(op))+" ah,0;",
            "mov eax,0;"+std::string(name(op))+" ah,xmm0;", "mov eax,0;"+std::string(name(op))+" ah,data[1];",
            "mov eax,0;jmp 2;"+std::string(name(op))+" al,1;"}) {
            bool rejected=false;
            try {static_cast<void>(compileDspCode(header()+body+"jmp end;routine:ret;end:",DspLanguage::Assembler));}
            catch (const DspCompileError& e) {rejected=e.line>0&&e.column>0;}
            check(rejected,"Byte logic rejects unwritten/opaque parents, mixed widths, descriptors and ambiguous numeric widths");
        }
        if (op!=Op::Test) for (auto target : {"samples[0]","data[0]"}) {
            bool rejected=false;
            try {static_cast<void>(compileDspCode(header()+"mov ebx,0;"+name(op)+" "+target+",bh;",DspLanguage::Assembler));}
            catch (const DspCompileError& e) {rejected=e.line>0&&e.column>0;}
            check(rejected,"Byte logical stores cannot write read-only inputs or descriptors");
        }
    }
    // ECX has a defined engine entry now; restore an opaque value to test unknown parents.
    for (unsigned alias=0;alias<8;++alias) {
        bool rejected=false;
        try {static_cast<void>(compileDspCode(header()+"push eax;pop ecx;xor "+aliases[alias]+","+aliases[alias]+";",DspLanguage::Assembler));}
        catch (const DspCompileError&) {rejected=true;}
        check(rejected,"Byte self-XOR cannot initialize unknown upper/lower parent bits");
    }
    for (auto body : {"mov eax,0;test ah,cells[0];", "mov eax,data[0];xor al,al;", "mov eax,routine;xor ah,ah;"}) {
        bool rejected=false;
        try {static_cast<void>(compileDspCode(header()+body+"jmp end;routine:ret;end:",DspLanguage::Assembler));}
        catch (const DspCompileError&) {rejected=true;}
        check(rejected,"TEST keeps its register/immediate second-operand rule and partial XOR cannot overwrite opaque parents");
    }
    literal("mov eax,0x1234ab56;mov ebx,0x76543210;xor ah,ah;",0x12340056,0x76543210,reference(Op::Xor,0xab,0xab));
    literal("mov eax,0x1234ab56;mov ebx,0x76543210;xor al,al;",0x1234ab00,0x76543210,reference(Op::Xor,0x56,0x56));
    literal("mov eax,0x12347f80;mov ebx,0;and al,0;inc ebx;",0x12347f00,1,reference(Op::Or,0,1));
    literal("mov eax,0x12340008;mov ebx,0;again:xor al,1;test al,1;jnz again;",0x12340008,0,reference(Op::Test,8,1));
}

void targetProvenance()
{
    for (auto op : {Op::And,Op::Or,Op::Xor,Op::Test}) for (bool alias : {false,true}) {
        const auto code="streamin index;memrefin data,alias;streamout out;float zero=0,one=1;"
            "movaps xmm0,zero;movaps out,xmm0;mov ecx,data[0];mov edx,0;mov eax,routine;mov [ecx+edx*4],eax;mov ebx,"+std::string(op==Op::And ? "0xff00" : "0")+";mov edx,index;"+
            std::string(alias ? "mov ecx,alias[0];" : "")+name(op)+" [ecx+edx],bh;mov ecx,data[0];mov edx,0;call [ecx+edx*4];jmp end;routine:movaps xmm0,one;movaps out,xmm0;ret;end:";
        const auto program=compileDspCode(code,DspLanguage::Assembler);
        for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend)) continue;
            Graph graph(backend);const auto block=graph.addDspCode(program);graph.connectDspInput(graph.addInput(),block.id,0);auto runtime=graph.compile(block.outputs);
            for (const std::size_t size : {1,3,7,31,64}) {
                runtime.reset();const openstone::green::Memory memory(std::vector<std::uint8_t>(20));
                check(runtime.setDspMemRefInput(block.id,1,memory) && runtime.setDspMemRefInput(block.id,2,memory.view(1,19,1)),"Bind byte logical operations target-table views");
                constexpr std::array positions{-1,0,1,2,3,4,7,18,19,20};std::array<float,positions.size()> input{},output{};
                for (unsigned n=0;n<positions.size();++n) input[n]=std::bit_cast<float>(static_cast<std::uint32_t>(positions[n]));
                for (std::size_t offset=0;offset<input.size();offset+=size) {
                    const auto count=std::min(size,input.size()-offset);
                    check(runtime.process(std::array{InputBuffer{std::span{input}.subspan(offset,count)}},std::array{OutputBuffer{std::span{output}.subspan(offset,count)}}),"Render byte logical operations/comparison against saved targets");
                }
                for (unsigned n=0;n<positions.size();++n) {
                    const auto actual=positions[n]+(alias ? 1 : 0);const bool valid=positions[n]>=0 && actual<20;
                    const auto expected=op==Op::Test || !valid || actual>=4 ? 1.F : 0.F;
                    check(output[n]==expected,"TEST preserves target provenance; writing byte logic invalidates overlapping callable bytes even when their bits stay unchanged");
                }
            }
        }
        if (!alias) {
            const auto body=header()+"mov eax,routine;mov cells[0],eax;mov ebx,"+std::string(op==Op::And ? "0xff00" : "0")+";"+name(op)+" cells[0],bh;mov eax,cells[0];call eax;jmp end;routine:movaps xmm0,one;movaps result,xmm0;ret;end:";
            bool rejected=false;
            try {const auto saved=compileDspCode(body,DspLanguage::Assembler);check(op==Op::Test,"Only read-only byte TEST may retain a statically saved local target");}
            catch (const DspCompileError&) {rejected=true;}
            check(rejected==(op!=Op::Test),"Static target facts survive TEST and are cleared by AND/OR/XOR byte stores");
        }
    }
}
}
int main()
{
    try {registersAndFlags();memoryOperands();aliasesWidthsAndDiagnostics();targetProvenance();}
    catch (const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
