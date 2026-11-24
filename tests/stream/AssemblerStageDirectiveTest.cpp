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
#include <type_traits>
#include <vector>

namespace {
using namespace openstone::stream;
constexpr std::size_t frames=137;

void check(bool valid,std::string_view message)
{
    if(!valid)throw std::runtime_error(std::string(message));
}
struct Environment {
    std::fenv_t saved;
    Environment(){std::fegetenv(&saved);}
    ~Environment(){std::fesetenv(&saved);}
};
std::string fixture(bool doubles)
{
    const auto name=doubles ? "assembler-stage-delay1-double.asm" : "assembler-stage-delay1-float.asm";
    std::ifstream file(std::filesystem::path(OPENSTONE_TEST_FIXTURES)/"flowstone"/name,std::ios::binary);
    check(file.good(),"Read exact stage-directive kernel fixture");
    std::string body{std::istreambuf_iterator<char>(file),{}};
    check(body.size()==(doubles ? 228U : 96U),"Preserve the recovered kernel's stored length");
    return body;
}
DspProgram delayProgram(bool doubles)
{
    const auto body=fixture(doubles);
    // The Float engine fragment omits its caller ports; the Double kernel is self-contained.
    return compileDspCode((doubles ? "" : "streamin in0;streamout out0;")+body,DspLanguage::Assembler);
}
template<typename Sample>
auto compile(const Graph& graph,std::span<const NodeId> outputs)
{
    if constexpr(std::is_same_v<Sample,float>)return graph.compile(outputs);
    else if constexpr(std::is_same_v<Sample,double>)return graph.compileMonoDouble(outputs);
    else if constexpr(std::is_same_v<Sample,Mono4>)return graph.compileMono4(outputs);
    else return graph.compileMonoDouble4(outputs);
}
template<typename Sample>
bool sameBits(const Sample& a,const Sample& b)
{
    using Bytes=std::array<std::uint8_t,sizeof(Sample)>;
    return std::bit_cast<Bytes>(a)==std::bit_cast<Bytes>(b);
}
template<typename Sample,std::size_t Outputs>
using Capture=std::array<std::array<Sample,frames>,Outputs>;

template<typename Sample,std::size_t Outputs>
Capture<Sample,Outputs> render(detail::Runtime<Sample>& runtime,const std::array<Sample,frames>& input,
                               std::span<const std::size_t> sizes)
{
    Capture<Sample,Outputs> output{};
    std::array<std::span<Sample>,Outputs> empty{};
    check(runtime.process(std::array<std::span<const Sample>,1>{},empty),"Zero-frame call at reset");
    for(std::size_t offset=0,block=0;offset<frames;++block) {
        const auto count=std::min(sizes[block%sizes.size()],frames-offset);
        std::array<std::span<Sample>,Outputs> out{};
        for(std::size_t port=0;port<Outputs;++port)out[port]=std::span{output[port]}.subspan(offset,count);
        check(runtime.process(std::array{std::span{input}.subspan(offset,count)},out),"Render stage directives across buffer boundaries");
        offset+=count;
    }
    return output;
}

template<typename Scalar>
std::vector<Scalar> rawValues()
{
    std::vector<Scalar> values;
    if constexpr(std::is_same_v<Scalar,float>) {
        for(std::uint32_t bits:{0U,0x80000000U,1U,0x80000001U,0x007fffffU,0x00800000U,
            0x7f800000U,0xff800000U,0x7fc00042U,0xff812345U,0x3f800001U,0x7f7fffffU})
            values.push_back(std::bit_cast<float>(bits));
    } else {
        for(std::uint64_t bits:{0ULL,0x8000000000000000ULL,1ULL,0x8000000000000001ULL,
            0x000fffffffffffffULL,0x0010000000000000ULL,0x7ff0000000000000ULL,0xfff0000000000000ULL,
            0x7ff8000000000042ULL,0xfff0123456789abcULL,0x3ff0000000001000ULL,0x7fefffffffffffffULL})
            values.push_back(std::bit_cast<double>(bits));
    }
    return values;
}

template<typename Sample>
void unchangedDelayKernel()
{
    constexpr bool packed=std::is_same_v<Sample,Mono4> || std::is_same_v<Sample,MonoDouble4>;
    using Scalar=std::conditional_t<std::is_same_v<Sample,MonoDouble4> || std::is_same_v<Sample,double>,double,float>;
    const auto program=delayProgram(std::is_same_v<Scalar,double>);
    check(program.readsSignalInput(0) && !program.outputDependsOnInput(0,0),
          "Stage-3 write and stage-1 read advertise a real delay to the graph scheduler");
    const auto values=rawValues<Scalar>();
    std::array<Sample,frames> input{},wanted{};
    for(std::size_t n=0;n<frames;++n) {
        if constexpr(packed)for(unsigned lane=0;lane<4;++lane)input[n][lane]=values[(n+lane*5)%values.size()];
        else input[n]=values[n%values.size()];
        if(n)wanted[n]=input[n-1];
    }
    Environment environment;
    for(auto backend:{DspBackend::Interpreter,DspBackend::LlvmOrc}) {
        if(!dspBackendAvailable(backend))continue;
        Graph graph(backend);const auto block=graph.addDspCode(program);
        graph.connectDspInput(graph.addInput(),block.id,0);
        auto runtime=compile<Sample>(graph,block.outputs);
        for(const std::size_t size:{1,3,7,31,64,257}) {
            runtime.reset();
            std::fesetround(FE_UPWARD);std::feclearexcept(FE_ALL_EXCEPT);std::feraiseexcept(FE_DIVBYZERO);
            const auto output=render<Sample,1>(runtime,input,std::array{size});
            for(std::size_t n=0;n<frames;++n)
                check(sameBits(output[0][n],wanted[n]),"Unchanged delay kernel must preserve each previous sample's exact bits");
            check(std::fegetround()==FE_UPWARD && std::fetestexcept(FE_ALL_EXCEPT)==FE_DIVBYZERO,
                  "Stage directives preserve the caller's floating-point environment");
        }
        runtime.reset();const auto output=render<Sample,1>(runtime,input,std::array<std::size_t,7>{0,3,1,0,17,64,7});
        for(std::size_t n=0;n<frames;++n)check(sameBits(output[0][n],wanted[n]),"Variable/empty buffer calls retain delay timing");
    }
}

template<typename Sample>
void controlledFeedback()
{
    constexpr bool doubles=std::is_same_v<Sample,MonoDouble4>;
    using Scalar=typename Sample::value_type;
    const auto program=delayProgram(doubles);
    std::array<Sample,frames> input{};
    // Keep the decay normal in Float, while exercising a distinct impulse and sign in every lane.
    for(unsigned lane=0;lane<4;++lane)input[frames-90+lane][lane]=static_cast<Scalar>((lane%2 ? -1 : 1)*static_cast<int>(lane+1))+
        (doubles ? static_cast<Scalar>(0x1p-40) : static_cast<Scalar>(0x1p-20));
    std::array<Sample,frames> wantedSum{},wantedDelay{};Sample previous{};
    for(std::size_t n=0;n<frames;++n) {
        wantedDelay[n]=previous;
        for(unsigned lane=0;lane<4;++lane)wantedSum[n][lane]=input[n][lane]+previous[lane]*static_cast<Scalar>(0.5);
        previous=wantedSum[n];
    }
    for(auto backend:{DspBackend::Interpreter,DspBackend::LlvmOrc}) {
        if(!dspBackendAvailable(backend))continue;
        Graph graph(backend);const auto block=graph.addDspCode(program);
        const auto inputNode=graph.addInput(),sum=graph.addSum(),half=graph.addGain(0.5);
        graph.connect(inputNode,sum);graph.connect(block.outputs[0],half);graph.connect(half,sum);
        graph.connectDspInput(sum,block.id,0);
        // Tapping either side of the feedback loop must not change its schedule.
        for(bool both:{false,true}) {
            const auto compare=[&]<std::size_t Outputs>(const std::array<NodeId,Outputs>& taps) {
                auto runtime=compile<Sample>(graph,taps);
                for(const std::size_t size:{1,3,7,31,64,257}) {
                    runtime.reset();const auto output=render<Sample,Outputs>(runtime,input,std::array{size});
                    for(std::size_t n=0;n<frames;++n) {
                        check(sameBits(output[0][n],taps[0]==sum ? wantedSum[n] : wantedDelay[n]),"Feedback response follows the independent recurrence");
                        if constexpr(Outputs==2)check(sameBits(output[1][n],taps[1]==sum ? wantedSum[n] : wantedDelay[n]),"Capturing both taps adds no latency");
                    }
                }
            };
            if(both) {compare(std::array{sum,block.outputs[0]});compare(std::array{block.outputs[0],sum});}
            else {compare(std::array{sum});compare(std::array{block.outputs[0]});}
        }
    }
}

void stagesKeepTheirTimingAndNames()
{
    // Source order is deliberately 2, 0, 3, 1; stage execution is still 0 then 1/2/3.
    const auto program=compileDspCode(R"(
        streamout normal,early,late;
        float state=99,stage=0;
        movaps xmm0,state;movaps normal,xmm0;
        stage // init
            0;
        movaps xmm0,_ONE;movaps state,xmm0;
        stage 3;
        call step;movaps late,xmm0;jmp done;
        step:movaps xmm0,state;addps xmm0,_ONE;movaps state,xmm0;ret;done:
        stage 1;
        call stage;movaps early,xmm0;jmp done;
        stage:movaps xmm0,state;ret;done:
    )",DspLanguage::Assembler);
    const auto mixed=compileDspCode(R"(
        streamout out;float stage=0;
        stage 0;movaps xmm0,_ONE;movaps stage,xmm0;
        stage 1;movaps xmm0,stage;
        stage(3){movaps xmm0,stage;addps xmm0,_ONE;movaps stage,xmm0;}
        movaps xmm0,stage;movaps out,xmm0;
        stage 0x2; // Re-enter stage 2 without forgetting its register state.
        movaps out,xmm0;
    )",DspLanguage::Assembler);
    for(auto backend:{DspBackend::Interpreter,DspBackend::LlvmOrc}) {
        if(!dspBackendAvailable(backend))continue;
        for(const std::size_t size:{1,3,7,31,64}) {
            Graph graph(backend);const auto block=graph.addDspCode(program);auto runtime=graph.compileMono4(block.outputs);
            // Programs have no inputs; use direct empty-input processing here.
            std::array<std::array<Mono4,frames>,3> output{};
            for(std::size_t offset=0;offset<frames;offset+=size) {
                const auto count=std::min(size,frames-offset);
                check(runtime.process({},std::array{Mono4OutputBuffer{std::span{output[0]}.subspan(offset,count)},
                    Mono4OutputBuffer{std::span{output[1]}.subspan(offset,count)},Mono4OutputBuffer{std::span{output[2]}.subspan(offset,count)}}),"Run directive stages and local calls");
            }
            for(std::size_t n=0;n<frames;++n)for(unsigned lane=0;lane<4;++lane)
                check(output[0][n][lane]==n+1 && output[1][n][lane]==n+1 && output[2][n][lane]==(n ? n+1 : 0),
                      "Initialization runs once, early/default stages precede late writes, and late outputs are held one sample");
            Graph mixedGraph(backend);const auto ports=mixedGraph.addDspCode(mixed);auto mixedRuntime=mixedGraph.compileMono4(ports.outputs);
            std::array<Mono4,2> samples{};
            check(mixedRuntime.process({},std::array{Mono4OutputBuffer{samples}}) && samples[0]==Mono4{1,1,1,1} && samples[1]==Mono4{2,2,2,2},
                  "Stage blocks still return to default stage 2, including after a directive");
        }
    }
}

void controlFlowWithinDirectives()
{
    constexpr std::array bodies{
        // A numeric skip and an indirect local call must stop their scans at the directive.
        "stage 1;mov eax,helper;call eax;jmp 6;mov eax,99;movd xmm0,eax;cvtdq2ps xmm0,xmm0;movaps out,xmm0;jmp done;helper:mov eax,3;ret;done:stage 3;",
        "stage 2;mov eax,0;loop:inc eax;cmp eax,3;jnz loop;jmp 6;mov eax,99;movd xmm0,eax;cvtdq2ps xmm0,xmm0;movaps out,xmm0;stage 3;",
        // RET ends only this stage, and identical helper names belong to their own sections.
        "float state=0;stage 0;call helper;movd xmm0,eax;cvtdq2ps xmm0,xmm0;movaps state,xmm0;ret;helper:mov eax,3;ret;stage 1;call helper;movaps out,xmm0;jmp done;helper:movaps xmm0,state;ret;done:stage 3;",
    };
    for(auto body:bodies) {
        const auto program=compileDspCode(std::string("streamout out;")+body,DspLanguage::Assembler);
        for(auto backend:{DspBackend::Interpreter,DspBackend::LlvmOrc}) {
            if(!dspBackendAvailable(backend))continue;
            Graph graph(backend);const auto block=graph.addDspCode(program);auto runtime=graph.compileMono4(block.outputs);
            for(const std::size_t size:{1,3,7,31,64}) {
                runtime.reset();std::array<Mono4,frames> output{};
                for(std::size_t offset=0;offset<frames;offset+=size) {
                    const auto count=std::min(size,frames-offset);
                    check(runtime.process({},std::array{Mono4OutputBuffer{std::span{output}.subspan(offset,count)}}),"Run calls, loops and numeric skips in directive sections");
                }
                for(const auto& sample:output)check(sample==Mono4{3,0,0,0},"Stage section boundaries retain local target and branch behavior");
            }
        }
    }
}

void reject(std::string_view body,std::string_view message,std::size_t line=0,DspLanguage language=DspLanguage::Assembler)
{
    bool rejected=false;
    try{static_cast<void>(compileDspCode(body,language));}
    catch(const DspCompileError& error){rejected=error.line>0 && error.column>0 && (!line || error.line==line) && std::string_view(error.what()).contains(message);}
    check(rejected,std::string("Expected stage diagnostic: ")+std::string(message)+" in "+std::string(body));
}
void directiveBoundariesAndDiagnostics()
{
    reject("streamout out;\nstage 4;","expected stage number",2);
    reject("streamout out;\nstage -1;","expected a nonnegative integer",2);
    reject("streamout out;\nstage 1.5;","expected a nonnegative integer",2);
    reject("streamout out;\nstage 1\nmovaps xmm0,_ONE;movaps out,xmm0;","expected ';'",3);
    reject("streamout out;stage 1 {movaps xmm0,_ONE;movaps out,xmm0;}","expected ';'");
    reject("streamout out;stage(1){stage 3;}","unsupported Assembler instruction 'stage'");
    reject("streamin in;streamout out;stage 0;movaps xmm0,in;","read stream inputs after stage(0)");
    reject("streamout out;stage(0){out=1;}","assign DSP outputs",0,DspLanguage::Code);
    reject("streamout out;stage 1;movaps xmm0,_ONE;movaps out,xmm0;stage 2;movaps xmm0,_ONE;movaps out,xmm0;","only one stage");
    reject("streamout out;stage 1;mov eax,1;stage 3;movd xmm0,eax;movaps out,xmm0;","write the integer register");
    reject("streamout out;stage 1;mov eax,0;cmp eax,1;stage 3;jnz done;done:movaps xmm0,_ONE;movaps out,xmm0;","branch requires an earlier");
    reject("streamout out;stage 1;jmp later;stage 3;later:movaps xmm0,_ONE;movaps out,xmm0;","unknown Assembler label");
    reject("streamout out;stage 1;call helper;stage 3;helper:movaps xmm0,_ONE;movaps out,xmm0;ret;","call requires a local subroutine in this section");
    reject("streamout out;stage 1;mov eax,0;jmp 1;stage 3;movaps xmm0,_ONE;movaps out,xmm0;","numeric branch displacement leaves this Assembler stage");
    reject("streamout out;stage 1;call helper;jmp done;helper:mov eax,1;stage 3;ret;done:movaps xmm0,_ONE;movaps out,xmm0;","call requires a local subroutine ending in ret");
    reject("streamout out;stage 1;mov eax,0;push eax;stage 2;movaps xmm0,_ONE;movaps out,xmm0;","balance push and pop");
    reject("streamout out;float one=1;stage 1;fld one;stage 2;movaps xmm0,_ONE;movaps out,xmm0;","x87 stack must be empty");
    reject("streamout out;stage 1;out=1;","unknown DSP name",0,DspLanguage::Code);
}
}
int main()
{
    try {
        openstone::tests::withReferenceSources("unchangedDelayKernel", [&] { unchangedDelayKernel<float>(); });openstone::tests::withReferenceSources("unchangedDelayKernel", [&] { unchangedDelayKernel<double>(); });
        openstone::tests::withReferenceSources("unchangedDelayKernel", [&] { unchangedDelayKernel<Mono4>(); });openstone::tests::withReferenceSources("unchangedDelayKernel", [&] { unchangedDelayKernel<MonoDouble4>(); });
        openstone::tests::withReferenceSources("controlledFeedback", [&] { controlledFeedback<Mono4>(); });openstone::tests::withReferenceSources("controlledFeedback", [&] { controlledFeedback<MonoDouble4>(); });
        stagesKeepTheirTimingAndNames();controlFlowWithinDirectives();directiveBoundariesAndDiagnostics();
    } catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
