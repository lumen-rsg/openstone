#include "../ReferenceFixtures.hpp"
#include "stream/Runtime.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace openstone::stream;
using Words=std::array<std::uint32_t,4>;
struct Constant {const char* name;std::uint64_t bits;};
constexpr Constant constants[]{{"_DONE",0x3ff0000000000000ULL},{"_DMONE",0xbff0000000000000ULL},
    {"_DABS",0x7fffffffffffffffULL},{"_DSIGN",0x8000000000000000ULL}};
void check(bool value,std::string_view message) {if (!value)throw std::runtime_error(std::string(message));}
struct Environment {std::fenv_t saved;Environment(){std::fegetenv(&saved);}~Environment(){std::fesetenv(&saved);}};
std::string fixture(const char* name,std::size_t length)
{
    const auto path=std::filesystem::path(OPENSTONE_TEST_FIXTURES)/"flowstone"/name;
    std::ifstream input(path,std::ios::binary);check(input.good(),"Read unchanged recovered engine kernel");
    const std::string body{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
    check(body.size()==length,"Recovered engine kernel retains its exact stored length");return body;
}
std::vector<std::uint64_t> values()
{
    std::vector<std::uint64_t> result{0,0x8000000000000000ULL,1,0x8000000000000001ULL,
        0x000fffffffffffffULL,0x800fffffffffffffULL,0x0010000000000000ULL,0x8010000000000000ULL,
        0x7ff0000000000000ULL,0xfff0000000000000ULL,0x7ff8000000000042ULL,0xfff0123456789abcULL,
        0x7fefffffffffffffULL,0xffefffffffffffffULL};
    for (double value : {-2147483649.0,-2147483648.75,-2147483648.0,-16777217.5,-3.75,-2.5,-1.0,-0.5,
        -0x1p-40,0x1p-40,0.5,1.0,1.0+0x1p-40,1.5,2.5,3.0,3.75,16777217.5,2147483647.0,
        2147483647.75,2147483648.0,0x1p1022,0x1p1023})result.push_back(std::bit_cast<std::uint64_t>(value));
    return result;
}
std::uint64_t flush(std::uint64_t bits)
{
    return (bits&0x7ff0000000000000ULL)==0 ? bits&0x8000000000000000ULL : bits;
}
std::uint64_t expected(unsigned kernel,std::uint64_t bits)
{
    constexpr auto sign=0x8000000000000000ULL;
    if (kernel==3)return (bits&sign)|0x3ff0000000000000ULL;
    if (kernel==4)return bits&~sign;
    bits=flush(bits);
    const auto magnitude=bits&~sign;
    const bool nan=magnitude>0x7ff0000000000000ULL;
    const double value=std::bit_cast<double>(bits);
    if (kernel==2) {
        if (magnitude==0)return (bits&sign)|0x7ff0000000000000ULL;
        if (magnitude==0x7ff0000000000000ULL)return bits&sign;
        check(!nan,"Reciprocal corpus uses finite values/infinities/zeros, without assuming an uncaptured NaN policy");
        volatile double one=1.0,denominator=value;
        return flush(std::bit_cast<std::uint64_t>(one/denominator));
    }
    // Preserve the original kernels' CVTTPD2DQ algorithm, including their signed-DWORD limits.
    const bool valid=!nan && value>=-2147483648.0 && value<2147483648.0;
    const auto truncated=valid ? static_cast<std::int32_t>(value) : INT32_MIN;
    const double converted=static_cast<double>(truncated);
    const bool adjust=kernel==0 ? nan || converted>value : !nan && converted<value;
    return std::bit_cast<std::uint64_t>(converted+(adjust ? kernel==0 ? -1.0 : 1.0 : 0.0));
}
void unchangedKernels()
{
    constexpr std::array names{"engine-double-floor.asm","engine-double-ceiling.asm","engine-double-reciprocal.asm","engine-double-sign.asm","engine-double-absolute.asm"};
    constexpr std::array lengths{304U,302U,151U,184U,151U};
    Environment environment;
    for (unsigned kernel=0;kernel<names.size();++kernel) {
        const auto program=compileDspCode(fixture(names[kernel],lengths[kernel]),DspLanguage::Assembler);
        auto corpus=values();
        if (kernel==2)std::erase_if(corpus,[](auto b){return (b&0x7fffffffffffffffULL)>0x7ff0000000000000ULL;});
        constexpr std::size_t frames=131;
        std::array<MonoDouble4,frames> input{},wanted{};
        std::fesetround(FE_TONEAREST);
        for (unsigned n=0;n<frames;++n)for(unsigned lane=0;lane<4;++lane) {
            const auto bits=corpus[(n+lane*11)%corpus.size()];input[n][lane]=std::bit_cast<double>(bits);
            wanted[n][lane]=std::bit_cast<double>(expected(kernel,bits));
        }
        for (auto backend : {DspBackend::Interpreter,DspBackend::LlvmOrc}) {
            if (!dspBackendAvailable(backend))continue;
            Graph graph(backend);const auto block=graph.addDspCode(program);graph.connectDspInput(graph.addInput(),block.id,0);
            auto runtime=graph.compileMonoDouble4(block.outputs);
            for(const std::size_t size:{1,3,7,31,64}) {
                runtime.reset();std::array<MonoDouble4,frames> output{};
                std::fesetround(FE_UPWARD);std::feclearexcept(FE_ALL_EXCEPT);std::feraiseexcept(FE_DIVBYZERO);
                for(std::size_t offset=0;offset<frames;offset+=size) {
                    const auto count=std::min(size,frames-offset);
                    check(runtime.process(std::array{MonoDouble4InputBuffer{std::span{input}.subspan(offset,count)}},
                        std::array{MonoDouble4OutputBuffer{std::span{output}.subspan(offset,count)}}),"Render unchanged engine kernels with implicit constant bindings");
                }
                for(unsigned n=0;n<frames;++n)for(unsigned lane=0;lane<4;++lane)
                    if(std::bit_cast<std::uint64_t>(output[n][lane])!=std::bit_cast<std::uint64_t>(wanted[n][lane]))
                        throw std::runtime_error(std::string(names[kernel])+" sample "+std::to_string(n)+" lane "+std::to_string(lane));
                check(std::fegetround()==FE_UPWARD && std::fetestexcept(FE_ALL_EXCEPT)==FE_DIVBYZERO,"Engine kernels preserve caller rounding/exception state");
            }
        }
    }
    check(expected(0,std::bit_cast<std::uint64_t>(-0.5))==std::bit_cast<std::uint64_t>(-1.0) &&
          expected(1,std::bit_cast<std::uint64_t>(0.5))==std::bit_cast<std::uint64_t>(1.0) &&
          expected(3,0)==0x3ff0000000000000ULL && expected(3,0x8000000000000000ULL)==0xbff0000000000000ULL &&
          expected(4,0xfff0123456789abcULL)==0x7ff0123456789abcULL,"Literal floor/ceiling/sign/mask cases distinguish exact constant values and raw NaN payloads");
}
void verifyRaw(const std::string& body,Words wanted,unsigned latency=0)
{
    constexpr unsigned repeats=67;
    const auto program=compileDspCode("streamout out;"+body,DspLanguage::Assembler);
    for(auto backend:{DspBackend::Interpreter,DspBackend::LlvmOrc}) {
        if(!dspBackendAvailable(backend))continue;
        Graph graph(backend);const auto block=graph.addDspCode(program);auto runtime=graph.compileMono4(block.outputs);
        for(const std::size_t size:{1,3,7,31,64}) {
            runtime.reset();std::array<Mono4,64> output{};
            for(std::size_t offset=0;offset<repeats;offset+=size) {
                const auto count=std::min<std::size_t>(size,repeats-offset);
                check(runtime.process({},std::array{Mono4OutputBuffer{std::span{output}.first(count)}}),"Render raw engine constant views");
                for(unsigned n=0;n<count;++n)if(std::bit_cast<Words>(output[n])!=(offset+n<latency ? Words{} : wanted)) {const auto actual=std::bit_cast<Words>(output[n]);throw std::runtime_error("Raw engine constant body: "+body+" actual "+std::to_string(actual[0])+","+std::to_string(actual[1])+","+std::to_string(actual[2])+","+std::to_string(actual[3]));}
            }
        }
    }
}
void rawViewsAndControlFlow()
{
    for(auto constant:constants) {
        const auto name=std::string(constant.name);
        const Words raw{static_cast<std::uint32_t>(constant.bits),static_cast<std::uint32_t>(constant.bits>>32),static_cast<std::uint32_t>(constant.bits),static_cast<std::uint32_t>(constant.bits>>32)};
        verifyRaw("movaps xmm0,"+name+";movaps out,xmm0;",raw);
        verifyRaw("movapd xmm0,"+name+"[2];movaps out,xmm0;",raw);
        verifyRaw("mov eax,0;movaps xmm0,"+name+"[eax];movaps out,xmm0;",raw);
        verifyRaw("call readConstant;movaps out,xmm0;jmp end;readConstant:movapd xmm0,"+name+";ret;end:",raw);
        verifyRaw("mov eax,"+name+";movd xmm0,eax;movaps out,xmm0;",{raw[0],0,0,0});
        verifyRaw("mov eax,0x12345678;mov ebx,7;mov al,"+name+"[ebx];movd xmm0,eax;movaps out,xmm0;",{(0x12345678U&~255U)|static_cast<std::uint32_t>(constant.bits>>56),0,0,0});
        // The scanner sees this constant before any lazy slot is bound.
        verifyRaw("double value=2;movapd xmm0,value;jmp 8;movapd xmm0,"+name+";movaps out,xmm0;",{0,0x40000000,0,0x40000000});
        verifyRaw("double value=2;movaps xmm0,value;jmp 7;movaps xmm0,"+name+";movaps out,xmm0;",{0,0x40000000,0,0x40000000});
        verifyRaw("double value=2;movapd xmm0,value;jmp 8;movapd xmm0,"+name+"[2];movaps out,xmm0;",{0,0x40000000,0,0x40000000});
        // Each stage may read the same lazily bound constant without changing its bits.
        verifyRaw("stage(0){movapd xmm0,"+name+";}stage(1){movapd xmm0,"+name+";}stage(3){movapd xmm0,"+name+";movaps out,xmm0;}",raw,1);
    }
    verifyRaw("movapd xmm0,_DONE;movapd xmm1,_DMONE;movapd xmm2,_DABS;movapd xmm3,_DSIGN;andpd xmm2,xmm3;orpd xmm0,xmm1;movaps out,xmm0;",{0,0xbff00000,0,0xbff00000});
    verifyRaw("double _DONE=2;movapd xmm0,_DONE;movaps out,xmm0;",{0,0x40000000,0,0x40000000});
    verifyRaw("double _DONE=2,other=3;movapd xmm0,other;movapd _DONE,xmm0;movapd xmm0,_DONE;movaps out,xmm0;",{0,0x40080000,0,0x40080000});
    verifyRaw("movapd xmm0,_DONE;double _DONE=2;movapd xmm1,_DONE;addpd xmm0,xmm1;movaps out,xmm0;",{0,0x40080000,0,0x40080000});
    verifyRaw("double local;dfld _DONE;dfstp local;movapd xmm0,local;movaps out,xmm0;",{0,0x3ff00000,0,0});
    verifyRaw("mov eax,0;cmp eax,1;movapd xmm0,_DONE;movapd xmm1,_DABS;andpd xmm0,xmm1;jae wrong;jnp wrong;jge wrong;movaps out,xmm0;jmp end;wrong:xorps xmm0,xmm0;movaps out,xmm0;end:",{0,0x3ff00000,0,0x3ff00000});
}
struct FloatConstant {const char* name;std::uint32_t bits;};
constexpr FloatConstant floatConstants[]{
    {"_PI",0x40490fdbU},{"_E",0x402df854U},{"_SQRT2",0x3fb504f3U},
    {"_ABS",0x7fffffffU},{"_SIGN",0x80000000U},{"_DENORM",0x219392efU},
    {"_ZERO",0x00000000U},{"_ONE",0x3f800000U},{"_MONE",0xbf800000U}};

std::uint32_t expectedFloat(unsigned kernel,std::uint32_t bits)
{
    if(kernel==2)return (bits&0x80000000U)|0x3f800000U;
    if(kernel==3)return bits&0x7fffffffU;
    // DAZ before CVTT/compare; retain the recovered kernels' signed-DWORD algorithm.
    if((bits&0x7f800000U)==0)bits&=0x80000000U;
    const bool nan=(bits&0x7fffffffU)>0x7f800000U;
    const float value=std::bit_cast<float>(bits);
    const bool valid=!nan && value>=-2147483648.0F && value<2147483648.0F;
    const auto truncated=valid ? static_cast<std::int32_t>(value) : INT32_MIN;
    volatile float converted=static_cast<float>(truncated);
    // CMPGTPS is the NLE alias (true for unordered), whereas CMPLTPS is ordered.
    const bool adjust=kernel==0 ? nan || converted>value : !nan && converted<value;
    volatile float delta=adjust ? kernel==0 ? -1.0F : 1.0F : 0.0F;
    return std::bit_cast<std::uint32_t>(converted+delta);
}
void unchangedFloatKernels()
{
    constexpr std::array names{"engine-float-floor.asm","engine-float-ceiling.asm","engine-float-sign.asm","engine-float-absolute.asm"};
    constexpr std::array lengths{122U,121U,65U,49U};
    // These engine fragments omit their caller's ports. Keep the fixture bytes unchanged;
    // supply only the known typed I/O context separately, never a declaration of a constant.
    constexpr auto caller="streamin in0;streamout out0;";
    std::vector<std::uint32_t> corpus{0,0x80000000U,1,0x80000001U,0x007fffffU,0x807fffffU,
        0x00800000U,0x80800000U,0x7f800000U,0xff800000U,0x7fc00042U,0xff812345U,0x7f7fffffU,0xff7fffffU};
    for(float v:{-2147483904.0F,-2147483648.0F,-2147483520.0F,-16777218.0F,-3.75F,-2.5F,-1.0F,-0.5F,
        -0x1p-40F,0x1p-40F,0.5F,1.0F,1.5F,2.5F,3.0F,3.75F,16777218.0F,2147483520.0F,2147483648.0F})
        corpus.push_back(std::bit_cast<std::uint32_t>(v));
    Environment environment;
    for(unsigned kernel=0;kernel<names.size();++kernel) {
        const auto body=fixture(names[kernel],lengths[kernel]);
        bool needsCaller=false;
        try{static_cast<void>(compileDspCode(body,DspLanguage::Assembler));}
        catch(const DspCompileError& e){needsCaller=std::string_view(e.what()).find("unknown DSP name 'in0'")!=std::string_view::npos;}
        check(needsCaller,"Strict unchanged-source audit must still report missing caller ports");
        const auto program=compileDspCode(caller+body,DspLanguage::Assembler);
        constexpr unsigned frames=137;
        std::array<Mono4,frames> input{};std::array<Words,frames> wanted{};
        std::fesetround(FE_TONEAREST);
        for(unsigned n=0;n<frames;++n)for(unsigned lane=0;lane<4;++lane) {
            const auto bits=corpus[(n+lane*7)%corpus.size()];
            input[n][lane]=std::bit_cast<float>(bits);wanted[n][lane]=expectedFloat(kernel,bits);
        }
        for(auto backend:{DspBackend::Interpreter,DspBackend::LlvmOrc}) {
            if(!dspBackendAvailable(backend))continue;
            Graph graph(backend);const auto block=graph.addDspCode(program);graph.connectDspInput(graph.addInput(),block.id,0);
            auto runtime=graph.compileMono4(block.outputs);
            for(const std::size_t size:{1,3,7,31,64}) {
                runtime.reset();std::array<Mono4,frames> output{};
                std::fesetround(FE_UPWARD);std::feclearexcept(FE_ALL_EXCEPT);std::feraiseexcept(FE_DIVBYZERO);
                for(std::size_t offset=0;offset<frames;offset+=size) {
                    const auto count=std::min<std::size_t>(size,frames-offset);
                    check(runtime.process(std::array{Mono4InputBuffer{std::span{input}.subspan(offset,count)}},
                        std::array{Mono4OutputBuffer{std::span{output}.subspan(offset,count)}}),"Render unchanged Float kernel in caller context");
                }
                for(unsigned n=0;n<frames;++n)if(std::bit_cast<Words>(output[n])!=wanted[n])
                    throw std::runtime_error(std::string(names[kernel])+" sample "+std::to_string(n)+" backend "+std::to_string(static_cast<int>(backend)));
                check(std::fegetround()==FE_UPWARD && std::fetestexcept(FE_ALL_EXCEPT)==FE_DIVBYZERO,"Float engine kernels preserve caller rounding/exception state");
            }
        }
    }
    std::fesetround(FE_TONEAREST);
    check(expectedFloat(0,0xbf000000U)==0xbf800000U && expectedFloat(1,0x3f000000U)==0x3f800000U &&
        expectedFloat(0,0x7fc00042U)==0xcf000000U && expectedFloat(1,0x7fc00042U)==0xcf000000U &&
        expectedFloat(2,0x80000000U)==0xbf800000U && expectedFloat(3,0xff812345U)==0x7f812345U,
        "Literal Float floor/ceiling, conversion-invalid, negative-zero and NaN-payload results");
}
void floatRawViewsAndNames()
{
    for(auto constant:floatConstants) {
        const auto name=std::string(constant.name);const auto bits=constant.bits;
        const Words raw{bits,bits,bits,bits};
        verifyRaw("movaps xmm0,"+name+";movaps out,xmm0;",raw);
        verifyRaw("movapd xmm0,"+name+";movaps out,xmm0;",raw);
        verifyRaw("mov eax,0;movaps xmm0,"+name+"[eax];movaps out,xmm0;",raw);
        verifyRaw("call readConstant;movaps out,xmm0;jmp end;readConstant:movaps xmm0,"+name+";ret;end:",raw);
        verifyRaw("mov eax,"+name+";movd xmm0,eax;movaps out,xmm0;",{bits,0,0,0});
        for(unsigned byte=0;byte<4;++byte)
            verifyRaw("mov eax,0x12345678;mov ebx,"+std::to_string(byte)+";mov al,"+name+"[ebx];movd xmm0,eax;movaps out,xmm0;",
                {(0x12345678U&~255U)|((bits>>(byte*8))&255U),0,0,0});
        verifyRaw("float value=2;movaps xmm0,value;jmp 7;movaps xmm0,"+name+";movaps out,xmm0;",{0x40000000,0x40000000,0x40000000,0x40000000});
        verifyRaw("float value=2;movaps xmm0,value;jmp 8;movapd xmm0,"+name+";movaps out,xmm0;",{0x40000000,0x40000000,0x40000000,0x40000000});
        verifyRaw("stage(0){movaps xmm0,"+name+";}stage(1){movaps xmm0,"+name+";}stage(3){movaps xmm0,"+name+";movaps out,xmm0;}",raw,1);
        // Raw values must survive a local write and a later read of the immutable binding.
        verifyRaw("float local;movaps xmm0,"+name+";movaps local,xmm0;xorps xmm0,xmm0;movaps local,xmm0;movaps xmm0,"+name+";movaps out,xmm0;",raw);
        for(auto operation:{"movaps TARGET,xmm0;","movapd TARGET[0],xmm0;","movd TARGET,xmm0;","mov TARGET,al;",
            "add TARGET,al;","xor TARGET,al;","inc TARGET;","xchg al,TARGET;","xchg TARGET,eax;","fstp TARGET;"}) {
            std::string code=operation;code.replace(code.find("TARGET"),6,name);bool rejected=false;
            try{static_cast<void>(compileDspCode(std::string("streamout out;float value=2;mov eax,0;movaps xmm0,value;")+
                (std::string_view(operation).starts_with("fstp") ? "fld value;" : "")+code,DspLanguage::Assembler));}
            catch(const DspCompileError& e){rejected=e.line>0&&e.column>0&&std::string_view(e.what()).find("engine constant")!=std::string_view::npos;}
            check(rejected,"Float constants reject packed/scalar/byte/x87 stores and exchanges");
        }
        bool codeRejected=false;
        try{static_cast<void>(compileDspCode("streamout out;out="+name+";"));}catch(const DspCompileError&){codeRejected=true;}
        check(codeRejected,"Float engine bindings remain Assembler-only");
        const auto words=dspLanguageWords(DspLanguage::Assembler);
        check(std::ranges::any_of(words,[&](const auto& word){return word.text==name&&word.kind==DspWordKind::Literal;}),"Float constants appear in highlighting and completion");
    }
    verifyRaw("movaps xmm0,_ONE;movaps xmm1,_MONE;andps xmm1,_ABS;subps xmm0,xmm1;addps xmm0,_ZERO;movaps out,xmm0;",{});
    verifyRaw("mov eax,0;cmp eax,1;movaps xmm0,_ONE;andps xmm0,_ABS;jae wrong;jnp wrong;jge wrong;movaps out,xmm0;jmp end;wrong:xorps xmm0,xmm0;movaps out,xmm0;end:",{0x3f800000,0x3f800000,0x3f800000,0x3f800000});
    verifyRaw("float local;fld _ONE;fstp local;movaps xmm0,local;movaps out,xmm0;",{0x3f800000,0,0,0});
    verifyRaw("float _ONE=2,other=3;movaps xmm0,other;movaps _ONE,xmm0;movaps xmm0,_ONE;movaps out,xmm0;",{0x40400000,0x40400000,0x40400000,0x40400000});
    verifyRaw("movaps xmm0,_ONE;float _ONE=2;movaps xmm1,_ONE;addps xmm0,xmm1;movaps out,xmm0;",{0x40400000,0x40400000,0x40400000,0x40400000});
    verifyRaw("movaps xmm0,_ONE;movapd xmm1,_DONE;movaps out,xmm0;",{0x3f800000,0x3f800000,0x3f800000,0x3f800000});
    verifyRaw("movaps xmm0,_ONE;movapd xmm1,_DONE;movaps out,xmm1;",{0,0x3ff00000,0,0x3ff00000});
    for(auto name:{"_one","_PI_typo","_ONE_typo","_SQRT"}) {
        bool rejected=false;
        try{static_cast<void>(compileDspCode("streamout out;movaps xmm0,"+std::string(name)+";movaps out,xmm0;",DspLanguage::Assembler));}
        catch(const DspCompileError&){rejected=true;}
        check(rejected,"Unverified, misspelled and wrong-case engine names remain errors");
    }
}

void readonlyAndNames()
{
    for(auto constant:constants)for(auto operation:{"movapd TARGET,xmm0;","movaps TARGET[0],xmm0;","movd TARGET,xmm0;","mov TARGET,al;",
        "add TARGET,al;","xor TARGET,al;","inc TARGET;","xchg al,TARGET;","xchg TARGET,eax;","dfstp TARGET;"}) {
        std::string code=operation;code.replace(code.find("TARGET"),6,constant.name);
        bool rejected=false;
        try{static_cast<void>(compileDspCode(std::string("streamout out;double value=2;mov eax,0;movapd xmm0,value;")+(std::string_view(operation).starts_with("dfstp") ? "dfld value;" : "")+code,DspLanguage::Assembler));}
        catch(const DspCompileError& e){rejected=e.line>0&&e.column>0&&std::string_view(e.what()).find("engine constant")!=std::string_view::npos;}
        check(rejected,"Implicit engine constants reject packed/scalar/byte/x87 stores and exchanges");
    }
    for(auto name:{"_UNKNOWN","_done","_DONE_typo"}) {
        bool rejected=false;
        try{static_cast<void>(compileDspCode("streamdoubleout out;movapd xmm0,"+std::string(name)+";movapd out,xmm0;",DspLanguage::Assembler));}
        catch(const DspCompileError&){rejected=true;}
        check(rejected,"Unknown engine symbols are not fabricated or treated as zero");
    }
    bool codeRejected=false;
    try{static_cast<void>(compileDspCode("streamdoubleout out;out=_DONE;"));}catch(const DspCompileError&){codeRejected=true;}
    check(codeRejected,"Assembler-only engine bindings do not change DSP Code's namespace");
    const auto words=dspLanguageWords(DspLanguage::Assembler);
    for(auto constant:constants)check(std::ranges::any_of(words,[&](const auto& word){return word.text==constant.name&&word.kind==DspWordKind::Literal;}),"Verified engine constants appear in Assembler highlighting/completion");
}
}
int main()
{
    try{openstone::tests::withReferenceSources("unchangedKernels", [&] { unchangedKernels(); });rawViewsAndControlFlow();readonlyAndNames();openstone::tests::withReferenceSources("unchangedFloatKernels", [&] { unchangedFloatKernels(); });floatRawViewsAndNames();}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
