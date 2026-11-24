#include "../ReferenceFixtures.hpp"
#include "stream/Runtime.hpp"
#include "stream/PolyRuntime.hpp"

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
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace openstone::stream;
using Words=std::array<std::uint32_t,4>;
constexpr std::size_t frames=1537;
void check(bool ok,std::string_view message){if(!ok)throw std::runtime_error(std::string(message));}
std::string fixture(std::string_view name){std::ifstream f(std::filesystem::path(OPENSTONE_TEST_FIXTURES)/"flowstone"/name,std::ios::binary);check(f.good(),"Read original serialized envelope");return {std::istreambuf_iterator<char>(f),{}};}
// Each operation rounds separately, matching the source instruction sequence. No implementation helpers are reused.
float add(float a,float b){volatile float x=a+b;return x;}
float sub(float a,float b){volatile float x=a-b;return x;}
float mul(float a,float b){volatile float x=a*b;return x;}
float div(float a,float b){volatile float x=a/b;return x;}
float quantize(float a){return mul(16.F,std::nearbyint(sub(mul(.0625F,std::max(16.F,a)),.4999F)));}
struct Inputs {float env,a,d,s,r;bool fast,smooth;};
Inputs parameters(unsigned n,unsigned lane)
{
    constexpr std::array<float,4> attacks{1,39,71,135},decays{35,71,135,263},sustains{.25F,.5F,.75F,0},releases{35,71,135,263};
    constexpr std::array<unsigned,4> starts{32,35,48,64},ends{256,333,512,601};
    float env=-1;
    if(n>=starts[lane] && n<800)env=n==starts[lane] ? 1.F : n>=ends[lane] ? 4.F : 3.F;
    if((lane==2 && n==113) || (lane==3 && n==145))env=1;
    const auto second=896+lane*3,secondEnd=1024+lane*5;
    if(n>=second)env=n==second ? 1.F : n>=secondEnd ? 4.F : 3.F;
    return {env,attacks[lane]+(n>=688 ? 16.F : 0.F),decays[lane]+(n>=1100 ? 16.F : 0.F),
        n>=400 && lane==1 ? .375F : sustains[lane],releases[lane],lane%2==1,lane==2};
}
struct Result{float value{},stage{};bool done{};};
struct Envelope {
    float cnt{},val{},inc{},adLen{},aStep{},dStep{},rStep{},stage{},off{},out{};
    Result next(Inputs p,unsigned n,bool legacy)
    {
        if(n%16==0){p.a=quantize(p.a);p.d=quantize(p.d);p.r=quantize(p.r);aStep=div(16,p.a);dStep=mul(div(16,p.d),sub(1,p.s));adLen=add(p.a,p.d);}
        const bool keep=p.env==0 || p.env>1;
        cnt=keep ? cnt : 0;off=keep ? off : 0;
        const auto old=val;val=add(keep ? old : 0,p.env==1 && p.smooth ? old : 0);
        val=cnt>0 || p.smooth ? val : 0;
        if(n%16==0){
            out=val;stage=add(cnt>=p.a ? 1.F : 0.F,cnt>=adLen ? 1.F : 0.F);
            const float release=legacy ? 4.F : 3.F;
            stage=legacy ? add(p.env!=4 ? stage : 0,p.env>=4 ? 4.F : 0.F) : add(stage,p.env>=4 ? sub(3,stage) : 0);
            const auto oldVal=val;
            val=add(val,stage==0 ? aStep : 0);val=sub(val,stage==1 ? dStep : 0);val=sub(val,stage==release ? rStep : 0);
            val=add(stage==2 ? p.s : 0,stage!=2 ? val : 0);val=std::min(1.F,val);
            rStep=add(stage<release ? mul(div(16,p.r),val) : 0,stage>=release ? rStep : 0);
            rStep=add(rStep,p.fast ? mul(.001F,val) : 0);
            inc=p.env>=0 ? mul(.0625F,sub(val,oldVal)) : 0;
        }
        out=std::max(0.F,add(out,inc));val=out;cnt=add(cnt,stage<2 ? 1.F : 0.F);
        const bool done=out<=0 && p.env>3 && off==0;off=add(off,done ? 1.F : 0.F);
        return {out,p.env>0 ? add(stage,1) : 0,done};
    }
};

void unchangedEnvelopes(DspBackend backend)
{
    for(bool legacy:{false,true}) {
        Graph graph(backend);const auto b=graph.addDspCode(compileDspCode(fixture(legacy ? "assembler-envelope-legacy.asm" : "assembler-envelope-current.asm"),DspLanguage::Assembler));
        check(b.outputs.size()==(legacy ? 2U : 3U),"Original envelope outlets retain their declaration order");
        for(unsigned port=0;port<7;++port)graph.connectDspInput(graph.addInput(),b.id,port);
        auto runtime=graph.compileMono4(b.outputs);
        std::array<std::array<Mono4,frames>,7> input{};
        std::array<std::array<Result,4>,frames> expected{};std::array<Envelope,4> reference{};
        const auto round=std::fegetround();check(std::fesetround(FE_TONEAREST)==0,"Set oracle rounding");
        for(unsigned n=0;n<frames;++n)for(unsigned lane=0;lane<4;++lane){const auto p=parameters(n,lane);input[0][n][lane]=p.env;input[1][n][lane]=p.fast;input[2][n][lane]=p.a;input[3][n][lane]=p.d;input[4][n][lane]=p.s;input[5][n][lane]=p.r;input[6][n][lane]=p.smooth;expected[n][lane]=reference[lane].next(p,n,legacy);}
        check(std::fesetround(round)==0,"Restore caller rounding");const auto original=input;
        for(const std::size_t size:{1,3,7,31,64,257}) {
            runtime.reset();std::array<std::array<Mono4,frames>,3> output{};
            for(unsigned offset=0;offset<frames;offset+=size){const auto count=std::min<std::size_t>(size,frames-offset);std::array<Mono4InputBuffer,7> in;std::array<Mono4OutputBuffer,3> out;
                for(unsigned port=0;port<7;++port)in[port]=std::span{input[port]}.subspan(offset,count);
                for(unsigned port=0;port<b.outputs.size();++port)out[port]=std::span{output[port]}.subspan(offset,count);
                const auto rounding=std::fegetround(),flags=std::fetestexcept(FE_ALL_EXCEPT);
                check(runtime.process(in,std::span{out}.first(b.outputs.size())),"Render unchanged serialized envelope");
                check(std::fegetround()==rounding && std::fetestexcept(FE_ALL_EXCEPT)==flags,"Envelope preserves caller floating-point state");
            }
            std::array<unsigned,4> completions{};
            for(unsigned n=0;n<frames;++n)for(unsigned lane=0;lane<4;++lane){const auto e=expected[n][lane];
                if(std::bit_cast<std::uint32_t>(output[1][n][lane])!=std::bit_cast<std::uint32_t>(e.value))throw std::runtime_error("Envelope value mismatch: legacy="+std::to_string(legacy)+" sample="+std::to_string(n)+" lane="+std::to_string(lane)+" got="+std::to_string(output[1][n][lane])+" expected="+std::to_string(e.value));
                check(output[0][n][lane]==float(e.done),"Envelope Done matches the independent per-sample state machine");completions[lane]+=e.done;
                if(!legacy)check(output[2][n][lane]==e.stage,"Original current envelope reports its expected stage");
            }
            for(auto count:completions)check(count>=1,"Each lane exercised release completion");
            check(input==original,"Envelope parameter stores cannot mutate caller buffers");
        }
    }
}

void copiesAreFreshAndIsolated(DspBackend backend)
{
    Graph graph(backend);const auto root=graph.addInput();
    const auto source=graph.addDspCode(compileDspCode("streamin x;streamout out;movaps xmm0,x;movaps out,xmm0;",DspLanguage::Assembler));graph.connectDspInput(root,source.id,0);
    const auto mutate=graph.addDspCode(compileDspCode("streamin v;streamout out;float one=1;movaps xmm0,v;addps xmm0,one;movaps v,xmm0;movaps xmm0,v;movaps out,xmm0;",DspLanguage::Assembler));graph.connectDspInput(source.outputs[0],mutate.id,0);
    const auto scalar=graph.addDspCode(compileDspCode("streamin v;streamout out;float one=1;movaps xmm0,v;addps xmm0,one;movaps v,xmm0;movaps out,xmm0;",DspLanguage::Assembler));graph.connectDspInput(source.outputs[0],scalar.id,0,DspInputWidth::Scalar);
    const auto unwired=graph.addDspCode(compileDspCode("streamin v;streamout out;float one=1;movaps xmm0,v;addps xmm0,one;movaps v,xmm0;movaps out,xmm0;",DspLanguage::Assembler));
    const std::array taps{mutate.outputs[0],scalar.outputs[0],unwired.outputs[0],source.outputs[0],root};auto runtime=graph.compileMono4(taps);
    std::array<Mono4,37> in{};for(unsigned n=0;n<in.size();++n)in[n]={n/8.F,-float(n)/8.F,2.F,3.F};
    for(unsigned size:{1,3,7,31,64}){runtime.reset();std::array<std::array<Mono4,37>,5> out{};
        for(unsigned offset=0;offset<in.size();offset+=size){const auto count=std::min<std::size_t>(size,in.size()-offset);std::array<Mono4OutputBuffer,5> outputs;for(unsigned port=0;port<5;++port)outputs[port]=std::span{out[port]}.subspan(offset,count);check(runtime.process(std::array{Mono4InputBuffer{std::span{in}.subspan(offset,count)}},outputs),"Render private working copies");}
        for(unsigned n=0;n<in.size();++n){for(unsigned lane=0;lane<4;++lane)check(out[0][n][lane]==in[n][lane]+1,"A fresh packed signal copy is loaded each sample");check(out[1][n]==Mono4{in[n][0]+1,1,1,1} && out[2][n]==Mono4{1,1,1,1},"Unwired and non-scalar lanes are reset before the stage rather than retaining stores");check(out[3][n]==in[n] && out[4][n]==in[n],"Working-copy writes leave upstream DSP slots and fan-out taps intact");}
    }
}

void rawFormsAndPrecision(DspBackend backend)
{
    constexpr Words original{0x7fc12300U,0x123456ffU,0x80000000U,0x3f800000U};
    for(bool x87:{false,true}) {
        Graph graph(backend);const auto b=graph.addDspCode(compileDspCode(std::string("streamin v;streamout out;")+(x87 ? "fld1;fstp v[2];" : "mov ebx,0x00001201;mov v[0],bl;add v[1],bh;xor v[2],bl;inc v[3];")+"movaps xmm0,v;movaps out,xmm0;",DspLanguage::Assembler));graph.connectDspInput(graph.addInput(),b.id,0);auto runtime=graph.compileMono4(b.outputs);
        std::array<Mono4,17> input{},out{};input.fill(std::bit_cast<Mono4>(original));check(runtime.process(std::array{Mono4InputBuffer{input}},std::array{Mono4OutputBuffer{out}}),"Run packed, byte, arithmetic and x87 signal stores");
        const Words expected=x87 ? Words{original[0],original[1],0x3f800000U,original[3]} : Words{0x7fc12301U,0x12345611U,0x80000001U,0x3f800001U};for(auto packet:out)check(std::bit_cast<Words>(packet)==expected,"Input stores preserve every untouched byte/lane and start from current source bits");for(auto packet:input)check(std::bit_cast<Words>(packet)==original,"Byte/x87 stores are local copies");
    }
    for(bool byte:{false,true}) {
        Graph graph(backend);const auto b=graph.addDspCode(compileDspCode(std::string("streamin v;streamout out,prior;mov eax,0x12345678;")+(byte ? "xchg ah,v[1];" : "xchg eax,v[1];")+"movd xmm0,eax;movaps prior,xmm0;movaps xmm0,v;movaps out,xmm0;",DspLanguage::Assembler));graph.connectDspInput(graph.addInput(),b.id,0);auto runtime=graph.compileMono4(b.outputs);
        std::array<Mono4,17> input{},out{},prior{};input.fill(std::bit_cast<Mono4>(original));check(runtime.process(std::array{Mono4InputBuffer{input}},std::array{Mono4OutputBuffer{out},Mono4OutputBuffer{prior}}),"Swap registers with signal working copies");
        auto expected=original;expected[1]=byte ? 0x12345656U : 0x12345678U;
        for(unsigned n=0;n<input.size();++n)check(std::bit_cast<Words>(out[n])==expected && std::bit_cast<Words>(prior[n])==Words{byte ? 0x1234ff78U : original[1],0,0,0} && std::bit_cast<Words>(input[n])==original,"Byte/DWORD exchanges snapshot the original register and refreshed input, preserving caller bits");
    }
    Graph doubles(backend);const auto b=doubles.addDspCode(compileDspCode("streamdoublein v;streamdoubleout out;double one=1.0000000000009094947017729282379150390625;dfld one;dfstp v[2];movapd xmm0,v[0];movapd out[0],xmm0;movapd xmm0,v[2];movapd out[2],xmm0;",DspLanguage::Assembler));doubles.connectDspInput(doubles.addInput(),b.id,0);auto runtime=doubles.compileMonoDouble4(b.outputs);
    std::array<MonoDouble4,17> input{},out{};input.fill({-1.0+0x1p-40,2.0+0x1p-39,3,4});check(runtime.process(std::array{MonoDouble4InputBuffer{input}},std::array{MonoDouble4OutputBuffer{out}}),"Store a Double through a signal input");for(auto packet:out)check(packet==MonoDouble4{-1.0+0x1p-40,2.0+0x1p-39,1.0+0x1p-40,4},"Double input copies preserve all untouched low bits and scalar-store neighbors");for(auto packet:input)check(packet==MonoDouble4{-1.0+0x1p-40,2.0+0x1p-39,3,4},"Double stores cannot modify caller buffers");
    for(bool integer:{false,true}) {
        Graph g(backend);const auto node=g.addDspCode(compileDspCode(std::string(integer ? "polyintin v;" : "streamboolin v;")+"streamout bits;mov eax,0;mov v[1],eax;movaps xmm0,v;movaps bits,xmm0;",DspLanguage::Assembler));g.connectDspInput(g.addInput(),node.id,0);auto r=g.compileMono4(node.outputs);std::array<Mono4,1> in{Mono4{1,1,0,1}},out{};check(r.process(std::array{Mono4InputBuffer{in}},std::array{Mono4OutputBuffer{out}}),"Write Boolean/integer signal operands");const auto one=integer ? 1U : 0xffffffffU;check(std::bit_cast<Words>(out[0])==Words{one,0,0,one},"Boolean masks and integer bits keep their declared input representations");
    }
}

void stagesAndReadonlyContracts(DspBackend backend)
{
    Graph graph(backend);const auto b=graph.addDspCode(compileDspCode("streamin v;streamout early,current,late;float one=1;stage 1;movaps xmm0,v;addps xmm0,one;movaps v,xmm0;movaps early,xmm0;stage 2;movaps xmm0,v;movaps current,xmm0;stage 3;movaps xmm0,v;addps xmm0,one;movaps v,xmm0;movaps late,xmm0;",DspLanguage::Assembler));graph.connectDspInput(graph.addInput(),b.id,0);auto runtime=graph.compile(b.outputs);const std::array input{1.F,2.F,3.F,4.F};std::array<float,4> early{},current{},late{};check(runtime.process(std::array{InputBuffer{input}},std::array{OutputBuffer{early},OutputBuffer{current},OutputBuffer{late}}),"Run mutable inputs in all processing stages");check(early==std::array{2.F,3.F,4.F,5.F} && current==input && late==std::array{0.F,2.F,3.F,4.F},"Each stage refreshes its signal copy and stage-three output keeps its existing one-sample visibility");
    for(const auto source:{"streamin v;streamout out;stage 0;mov v,1;", "floatarrayin v[4];streamout out;movaps xmm0,_ONE;movaps v[0],xmm0;", "memin v[4];streamout out;fld1;fstp v[0];", "memarrayrefin v;streamout out;movaps xmm0,_ONE;movaps v[0],xmm0;", "streamin v;streamout out;mov eax,leaf;mov v,eax;jmp done;leaf:ret;done:"}){bool rejected=false;try{static_cast<void>(compileDspCode(source,DspLanguage::Assembler));}catch(const DspCompileError&){rejected=true;}check(rejected,"Initialization, external arrays, descriptors and opaque routine targets retain their write restrictions");}
    bool rejected=false;try{static_cast<void>(compileDspCode("streamin v;streamout out;v=1;out=v;"));}catch(const DspCompileError&){rejected=true;}check(rejected,"DSP Code's input-assignment contract is unchanged");
}

void polyCopies(DspBackend backend)
{
    Graph graph(backend);const auto velocity=graph.addInput();const auto b=graph.addDspCode(compileDspCode("polyin v;polyout out;movaps xmm0,v;addps xmm0,v;movaps v,xmm0;movaps out,xmm0;",DspLanguage::Assembler));graph.connectDspInput(velocity,b.id,0);
    const auto peer=graph.addDspCode(compileDspCode("polyin v;polyout out;movaps xmm0,v;movaps out,xmm0;",DspLanguage::Assembler));graph.connectDspInput(velocity,peer.id,0);
    const std::array taps{b.outputs[0],peer.outputs[0],velocity};const std::array bindings{PolyInput{PolyInputKind::Velocity}};PolyRuntime runtime(graph,bindings,taps,{.maxVoices=5,.holdAfterNoteOff=true});
    const std::array events{PolyEvent{0,PolyEventKind::NoteOn,1,60,.125F},PolyEvent{0,PolyEventKind::NoteOn,2,61,.25F},PolyEvent{0,PolyEventKind::NoteOn,3,62,.5F},PolyEvent{0,PolyEventKind::NoteOn,4,63,.75F},PolyEvent{0,PolyEventKind::NoteOn,5,64,1.F},PolyEvent{4,PolyEventKind::NoteOff,3},PolyEvent{7,PolyEventKind::EndVoice,3},PolyEvent{9,PolyEventKind::NoteOn,6,65,.375F}};
    for(unsigned size:{1,3,7,31}){runtime.reset();std::array<std::array<float,17>,3> out{};
        for(unsigned offset=0;offset<17;offset+=size){const auto count=std::min(size,17-offset);std::vector<PolyEvent> part;for(auto e:events)if(e.frame>=offset && e.frame<offset+count){e.frame-=offset;part.push_back(e);}check(runtime.process({},std::array{OutputBuffer{std::span{out[0]}.subspan(offset,count)},OutputBuffer{std::span{out[1]}.subspan(offset,count)},OutputBuffer{std::span{out[2]}.subspan(offset,count)}},part),"Render mutable Poly inputs across two packets and slot reuse");}
        for(unsigned n=0;n<17;++n){const float sum=n<7 ? 2.625F : n<9 ? 2.125F : 2.5F;check(out[0][n]==sum*2 && out[1][n]==sum && out[2][n]==sum,"Poly input writes are refreshed and isolated from other nodes, voices and reused slots");}
    }
}
}
int main(){try{for(auto backend:{DspBackend::Interpreter,DspBackend::LlvmOrc})if(dspBackendAvailable(backend)){openstone::tests::withReferenceSources("unchangedEnvelopes", [&] { unchangedEnvelopes(backend); });copiesAreFreshAndIsolated(backend);rawFormsAndPrecision(backend);stagesAndReadonlyContracts(backend);polyCopies(backend);}}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
