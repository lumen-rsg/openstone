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
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {
using namespace openstone::stream;
using Words=std::array<std::uint32_t,4>;
constexpr std::size_t frames=137;
constexpr std::array names{"eax","ebx","ecx","edx","esi","edi","ebp"};
constexpr std::array aliases{"rax","rbx","rcx","rdx","rsi","rdi","rbp"};
constexpr std::array branches{"je","jz","jne","jnz","ja","jb","jbe","jae","jp","jnp","jg","jl","jng","jnl","jge","jle"};
constexpr std::array<std::uint32_t,7> sentinels{0x12345678U,0x23456789U,0x3456789aU,0x456789abU,0x56789abcU,0x6789abcdU,0x789abcdeU};
void check(bool valid,std::string_view message){if(!valid)throw std::runtime_error(std::string(message));}
struct Environment {std::fenv_t saved;Environment(){std::fegetenv(&saved);}~Environment(){std::fesetenv(&saved);}};
std::string seedRegisters()
{
    std::string code;
    for(unsigned i=0;i<names.size();++i)code+="mov "+std::string(names[i])+","+std::to_string(sentinels[i])+";";
    return code;
}
std::string snapshotRegisters()
{
    std::string code;
    for(unsigned i=0;i<names.size();++i)code+="movd xmm0,"+std::string(names[i])+";movaps r"+std::to_string(i)+",xmm0;";
    return code;
}
std::string outputs(bool flags)
{
    std::string code="streamout ";
    if(flags)for(unsigned i=0;i<branches.size();++i)code+="f"+std::to_string(i)+",";
    for(unsigned i=0;i<names.size();++i)code+="r"+std::to_string(i)+(i+1==names.size() ? ";" : ",");
    return code;
}
std::string snapshotFlags()
{
    std::string code;
    for(unsigned i=0;i<branches.size();++i) {
        const auto suffix=std::to_string(i);
        code+="movaps xmm7,zero;movaps f"+suffix+",xmm7;"+branches[i]+" yes"+suffix+";jmp next"+suffix+
            ";yes"+suffix+":movaps xmm7,one;movaps f"+suffix+",xmm7;next"+suffix+":";
    }
    return code;
}
struct Flags {bool carry,parity,zero,less;};
auto branchValues(Flags f)
{
    return std::array{f.zero,f.zero,!f.zero,!f.zero,!f.carry&&!f.zero,f.carry,f.carry||f.zero,!f.carry,
        f.parity,!f.parity,!f.less&&!f.zero,f.less,f.less||f.zero,!f.less,!f.less,f.less||f.zero};
}
struct Expected {std::uint32_t stored;Flags flags;};
Expected arithmetic(std::string_view op,std::uint32_t a,std::uint32_t b,bool carry)
{
    std::uint32_t result=a;bool overflow=false;
    if(op=="inc")b=1;
    if(op=="dec")b=1;
    if(op=="add" || op=="inc") {
        result=a+b;if(op=="add")carry=std::uint64_t{a}+b>0xffffffffULL;
        overflow=((~(a^b)&(a^result))>>31)!=0;
    } else if(op=="sub" || op=="cmp" || op=="dec") {
        result=a-b;if(op!="dec")carry=a<b;
        overflow=(((a^b)&(a^result))>>31)!=0;
    } else {
        carry=false;
        if(op=="and" || op=="test")result=a&b;
        else if(op=="or")result=a|b;
        else result=a^b;
    }
    return {op=="cmp" || op=="test" ? a : result,
        {carry,(std::popcount(result&255U)%2)==0,result==0,((result>>31)!=0)!=overflow}};
}

template<std::size_t Outputs,std::size_t Inputs,typename Verify>
void across(detail::Runtime<Mono4>& runtime,const std::array<std::array<Mono4,frames>,Inputs>& input,Verify verify)
{
    for(const std::size_t size:{1,3,7,31,64}) {
        runtime.reset();
        std::array<std::array<Mono4,frames>,Outputs> output{};
        for(std::size_t offset=0;offset<frames;offset+=size) {
            const auto count=std::min(size,frames-offset);
            std::array<std::span<const Mono4>,Inputs> in{};std::array<std::span<Mono4>,Outputs> out{};
            for(unsigned p=0;p<Inputs;++p)in[p]=std::span{input[p]}.subspan(offset,count);
            for(unsigned p=0;p<Outputs;++p)out[p]=std::span{output[p]}.subspan(offset,count);
            check(runtime.process(in,out),"Render additional GPR forms across buffers");
        }
        verify(output);
    }
}
void numericRegistersAndFlags()
{
    constexpr std::array<std::uint32_t,16> corpus{0,1,2,3,127,128,255,256,0x7ffffffe,0x7fffffff,0x80000000,0x80000001,0xfffffffe,0xffffffff,0x12345678,0x87654321};
    std::array<std::array<Mono4,frames>,3> input{};
    for(unsigned n=0;n<frames;++n) {
        input[0][n]=std::bit_cast<Mono4>(Words{corpus[n%corpus.size()],0xdeadbeef,0x7f812345,0xabcdef01});
        input[1][n]=std::bit_cast<Mono4>(Words{corpus[(n/3+5)%corpus.size()],0xabcdef01,0,0});
        input[2][n]=std::bit_cast<Mono4>(Words{n&1U,0,0,0});
    }
    Environment environment;
    for(unsigned reg=4;reg<names.size();++reg)for(auto op:{"add","sub","cmp","inc","dec","and","or","xor","test"}) {
        const bool unary=std::string_view(op)=="inc" || std::string_view(op)=="dec";
        const auto code="streamin a,b,seed;"+outputs(true)+"float zero=0,one=1;"+seedRegisters()+
            "mov "+names[reg]+",a;cmp seed,1;"+(std::string_view(op)=="test" ? "test b,"+std::string(names[reg])+";" : std::string(op)+" "+names[reg]+(unary ? ";" : ",b;"))+snapshotFlags()+snapshotRegisters();
        const auto program=compileDspCode(code,DspLanguage::Assembler);
        for(auto backend:{DspBackend::Interpreter,DspBackend::LlvmOrc}) {
            if(!dspBackendAvailable(backend))continue;
            Graph graph(backend);const auto block=graph.addDspCode(program);
            for(unsigned p=0;p<input.size();++p)graph.connectDspInput(graph.addInput(),block.id,p);
            auto runtime=graph.compileMono4(block.outputs);
            std::fesetround(FE_UPWARD);std::feclearexcept(FE_ALL_EXCEPT);std::feraiseexcept(FE_DIVBYZERO);
            across<23>(runtime,input,[&](const auto& output) {
                for(unsigned n=0;n<frames;++n) {
                    const auto expected=arithmetic(op,std::bit_cast<Words>(input[0][n])[0],std::bit_cast<Words>(input[1][n])[0],!(n&1));
                    const auto flags=branchValues(expected.flags);
                    for(unsigned f=0;f<branches.size();++f)check(output[f][n]==(flags[f] ? Mono4{1,1,1,1} : Mono4{}),"Additional GPR arithmetic produces correct conditional branches");
                    for(unsigned other=0;other<names.size();++other)
                        check(std::bit_cast<Words>(output[branches.size()+other][n])==Words{other==reg ? expected.stored : sentinels[other],0,0,0},"GPR storage is independent, and raw numeric results preserve all 32 bits");
                }
            });
            check(std::fegetround()==FE_UPWARD && std::fetestexcept(FE_ALL_EXCEPT)==FE_DIVBYZERO,"Additional GPR operations preserve caller state");
        }
    }
}
std::string fixture(const char* name,std::size_t bytes)
{
    std::ifstream file(std::filesystem::path(OPENSTONE_TEST_FIXTURES)/"flowstone"/name,std::ios::binary);check(file.good(),"Read unchanged recovered GPR fixture");
    std::string body{std::istreambuf_iterator<char>(file),{}};check(body.size()==bytes,"Preserve recovered fixture length");return body;
}
std::vector<std::uint8_t> bytes()
{
    std::vector<std::uint8_t> data(64);
    constexpr std::array<std::uint32_t,16> words{0x80000000,0x7f812345,0x01234567,0xffffffff,1,0x3f800000,0x7f800000,0xdeadbeef,
        0xfff01234,0x7ff80000,0x00100000,0,0x12345678,0x87654321,0x3ff00000,0x1000};
    for(unsigned i=0;i<words.size();++i)for(unsigned b=0;b<4;++b)data[i*4+b]=static_cast<std::uint8_t>(words[i]>>(8*b));
    return data;
}
std::uint64_t read(const std::vector<std::uint8_t>& data,std::size_t offset,unsigned size)
{
    if(offset>data.size() || size>data.size()-offset)return 0;
    std::uint64_t word=0;for(unsigned b=0;b<size;++b)word|=std::uint64_t{data[offset+b]}<<(8*b);return word;
}
void recoveredFrameLoads()
{
    const auto data=bytes();const openstone::green::Memory memory(data);
    for(bool doubles:{false,true}) {
        const auto body=fixture(doubles ? "engine-frame-load-double.asm" : "engine-frame-load-float.asm",doubles ? 115 : 56);
        // The original body is untouched. Bind its explicit caller frame to a bounded Mem,
        // and provide only its ports, moving byte cursor, and output taps around it.
        const auto program=compileDspCode(std::string("memrefin data;int cursor=0;mov rbp,data[0];mov eax,cursor;")+
            (doubles ? "streamdoubleout first,second,third;" : "streamout first,second,third;")+body+
            (doubles ? "movapd first[0],xmm0;movapd first[2],xmm1;movapd second[0],xmm2;movapd second[2],xmm3;movapd third[0],xmm4;movapd third[2],xmm5;" : "movaps first,xmm0;movaps second,xmm2;movaps third,xmm4;")+
            "add eax,"+(doubles ? "8" : "4")+";cmp eax,64;jl keep;mov eax,0;keep:mov cursor,eax;",DspLanguage::Assembler);
        for(auto backend:{DspBackend::Interpreter,DspBackend::LlvmOrc}) {
            if(!dspBackendAvailable(backend))continue;
            Graph graph(backend);const auto block=graph.addDspCode(program);
            const auto test=[&]<typename Sample>(detail::Runtime<Sample> runtime) {
                for(const std::size_t size:{1,3,7,31,64}) {
                    runtime.reset();check(runtime.setDspMemRefInput(block.id,0,memory),"Bind explicit frame-load caller memory");
                    std::array<std::array<Sample,frames>,3> output{};
                    for(std::size_t offset=0;offset<frames;offset+=size) {
                        const auto count=std::min(size,frames-offset);std::array<std::span<Sample>,3> out{};
                        for(unsigned p=0;p<3;++p)out[p]=std::span{output[p]}.subspan(offset,count);
                        check(runtime.process({},out),"Render unchanged frame-register kernels");
                    }
                    for(unsigned n=0;n<frames;++n)for(unsigned lane=0;lane<4;++lane) {
                        const auto offset=(n*(doubles ? 8U : 4U))%64+lane*(doubles ? 8U : 4U);
                        for(unsigned p=0;p<3;++p) {
                            if constexpr(std::is_same_v<Sample,Mono4>)check(std::bit_cast<std::uint32_t>(output[p][n][lane])==read(data,offset,4),"Original Float frame loads retain raw words, copies and bounded tail reads");
                            else check(std::bit_cast<std::uint64_t>(output[p][n][lane])==read(data,offset,8),"Original Double frame loads retain both physical windows and raw QWORDs");
                        }
                    }
                }
            };
            if(doubles)test(graph.compileMonoDouble4(block.outputs));else test(graph.compileMono4(block.outputs));
        }
    }
}

void recoveredByteWriter()
{
    const auto body=fixture("assembler-mem-byte-writer.asm",1530);
    const auto program=compileDspCode(seedRegisters()+body+outputs(false)+snapshotRegisters(),DspLanguage::Assembler);
    std::array<std::array<Mono4,frames>,2> input{};
    for(unsigned n=0;n<frames;++n)for(unsigned lane=0;lane<4;++lane) {
        const auto index=(n+lane*7)%24;
        input[0][n][lane]=std::bit_cast<float>(index);
        input[1][n][lane]=std::bit_cast<float>(0xabcdef00U|((n*17+lane*53)&255U));
    }
    for(auto backend:{DspBackend::Interpreter,DspBackend::LlvmOrc}) {
        if(!dspBackendAvailable(backend))continue;
        Graph graph(backend);const auto block=graph.addDspCode(program);
        graph.connectDspInput(graph.addInput(),block.id,1);graph.connectDspInput(graph.addInput(),block.id,2);
        auto runtime=graph.compileMono4(block.outputs);
        for(const std::size_t size:{1,3,7,31,64}) {
            runtime.reset();auto wanted=bytes();const openstone::green::Memory memory(wanted);
            check(runtime.setDspMemRefInput(block.id,0,memory),"Bind recovered Mem writer");
            for(std::size_t offset=0;offset<frames;offset+=size) {
                const auto count=std::min(size,frames-offset);std::array<std::array<Mono4,64>,8> output{};
                std::array<std::span<Mono4>,8> out{};for(unsigned p=0;p<8;++p)out[p]=std::span{output[p]}.first(count);
                check(runtime.process(std::array{Mono4InputBuffer{input[0]}.subspan(offset,count),Mono4InputBuffer{input[1]}.subspan(offset,count)},out),"Run unchanged EDI byte writer with explicit caller save-register values");
                for(unsigned n=0;n<count;++n) {
                    Words expected{};
                    for(unsigned lane=0;lane<4;++lane) {
                        const auto index=std::bit_cast<Words>(input[0][offset+n])[lane];const auto value=std::bit_cast<Words>(input[1][offset+n])[lane]&255U;
                        wanted[index]=static_cast<std::uint8_t>(value);expected[lane]=value;
                    }
                    check(std::bit_cast<Words>(output[0][n])==expected,"Original writer reports each written low byte");
                    for(unsigned reg=0;reg<names.size();++reg)check(std::bit_cast<Words>(output[reg+1][n])==Words{sentinels[reg],0,0,0},"Original push/pop restores all seven independent caller registers");
                }
                check(std::ranges::equal(memory.bytes(),wanted),"Original EDI writer changes only selected bytes and preserves neighbors");
            }
        }
    }
}

void referencesCallsAndByteParents()
{
    for(unsigned reg=4;reg<names.size();++reg) {
        const auto name=std::string(names[reg]),alias=std::string(aliases[reg]);
        const std::array sources{
            "mov "+alias+",data[0];push "+alias+";mov "+name+",0;pop "+alias+";mov eax,["+alias+"+4];movd xmm0,eax;movaps out,xmm0;",
            "mov "+name+",helper;push "+name+";mov "+name+",0;pop "+alias+";call "+alias+";movd xmm0,eax;movaps out,xmm0;jmp done;helper:mov eax,0x7f812345;ret;done:",
            "mov "+alias+",helper;call start;movd xmm0,eax;movaps out,xmm0;jmp done;start:jmp "+alias+";helper:mov eax,0x7f812345;ret;done:",
            "mov "+name+",0;loop:inc "+name+";cmp "+name+",3;jnz loop;jmp 6;mov "+name+",99;mov eax,"+name+";movd xmm0,eax;movaps out,xmm0;",
            "mov "+name+",0x7f812345;mov eax,0x12345678;xchg eax,"+name+";movd xmm0,eax;movaps out,xmm0;",
        };
        for(unsigned form=0;form<sources.size();++form) {
            const auto& source=sources[form];
            const auto program=compileDspCode("memrefin data;streamout out;"+source,DspLanguage::Assembler);
            for(auto backend:{DspBackend::Interpreter,DspBackend::LlvmOrc}) {
                if(!dspBackendAvailable(backend))continue;
                Graph graph(backend);const auto block=graph.addDspCode(program);auto runtime=graph.compileMono4(block.outputs);
                for(const std::size_t size:{1,3,7,31,64}) {
                    runtime.reset();check(runtime.setDspMemRefInput(block.id,0,openstone::green::Memory(bytes())),"Bind pointer-alias caller memory");
                    std::array<Mono4,frames> output{};
                    for(unsigned offset=0;offset<frames;offset+=size) {
                        const auto count=std::min<std::size_t>(size,frames-offset);
                        check(runtime.process({},std::array{Mono4OutputBuffer{std::span{output}.subspan(offset,count)}}),"Run additional pointer/target aliases and numeric spans");
                    }
                    for(const auto& sample:output)check(std::bit_cast<Words>(sample)==Words{form==3 ? 3U : 0x7f812345U,0,0,0},"Saved pointer/local-target provenance survives new register aliases and stack operations");
                }
            }
        }
    }
    // Extending the GPR bank must not move AH/BH/CH/DH onto the new registers.
    const auto program=compileDspCode(outputs(false)+seedRegisters()+
        "mov al,0xa1;mov bl,0xb2;mov cl,0xc3;mov dl,0xd4;mov ah,0x15;mov bh,0x26;mov ch,0x37;mov dh,0x48;"+snapshotRegisters(),DspLanguage::Assembler);
    for(auto backend:{DspBackend::Interpreter,DspBackend::LlvmOrc}) {
        if(!dspBackendAvailable(backend))continue;
        Graph graph(backend);const auto block=graph.addDspCode(program);auto runtime=graph.compileMono4(block.outputs);
        across<7>(runtime,std::array<std::array<Mono4,frames>,0>{},[&](const auto& output) {
            constexpr std::array<std::uint32_t,4> low{0x15a1,0x26b2,0x37c3,0x48d4};
            for(unsigned n=0;n<frames;++n)for(unsigned reg=0;reg<names.size();++reg)
                check(std::bit_cast<Words>(output[reg][n])==Words{reg<4 ? (sentinels[reg]&0xffff0000U)|low[reg] : sentinels[reg],0,0,0},"Legacy low/high byte parents retain their original mapping and preserve the new GPRs");
        });
    }
}
void diagnosticsAndCompletion()
{
    const auto words=dspLanguageWords(DspLanguage::Assembler);
    for(unsigned reg=4;reg<names.size();++reg) {
        for(auto name:{names[reg],aliases[reg]})check(std::ranges::any_of(words,[&](const auto& word){return word.text==name && word.kind==DspWordKind::Register;}),"New register names appear in Assembler highlighting/completion");
        const std::array invalid{
            "streamout out;movd xmm0,"+std::string(names[reg])+";movaps out,xmm0;",
            "streamout out;mov "+std::string(names[reg])+",1;stage 1;movd xmm0,"+names[reg]+";movaps out,xmm0;",
            "memrefin data;streamout out;mov "+std::string(aliases[reg])+",data[0];mov "+names[reg]+",0;mov eax,["+aliases[reg]+"];",
            "streamout out;mov "+std::string(aliases[reg])+",1;",
            "streamout out;int "+std::string(names[reg])+";",
            "streamout out;int "+std::string(aliases[reg])+";",
        };
        for(const auto& source:invalid) {
            bool rejected=false;try{static_cast<void>(compileDspCode(source,DspLanguage::Assembler));}
            catch(const DspCompileError& error){rejected=error.line>0 && error.column>0;}
            check(rejected,"New registers retain initialization, stage, pointer, width and reserved-name diagnostics");
        }
    }
}
}
int main()
{
    try{numericRegistersAndFlags();openstone::tests::withReferenceSources("recoveredFrameLoads", [&] { recoveredFrameLoads(); });openstone::tests::withReferenceSources("recoveredByteWriter", [&] { recoveredByteWriter(); });referencesCallsAndByteParents();diagnosticsAndCompletion();}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
