#include "../ReferenceFixtures.hpp"
#include "stream/Runtime.hpp"
#include "stream/PolyRuntime.hpp"

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
#include <vector>

namespace {
using namespace openstone::stream;
using Words = std::array<std::uint32_t, 4>;
constexpr std::array<std::size_t, 6> bufferSizes{1, 3, 7, 31, 64, 257};
void check(bool ok, std::string_view message) { if (!ok) throw std::runtime_error(std::string(message)); }
struct Environment {
    std::fenv_t saved;
    Environment() { std::fegetenv(&saved); std::fesetround(FE_TONEAREST); }
    ~Environment() { std::fesetenv(&saved); }
};
std::string fixture(std::string_view name)
{
    std::ifstream file(std::filesystem::path(OPENSTONE_TEST_FIXTURES) / "flowstone" / name, std::ios::binary);
    check(file.good(), "Read unchanged native conversion fragment");
    return {std::istreambuf_iterator<char>(file), {}};
}
// Independent IEEE binary32 -> binary64 bit expansion, including x87 NaN quieting.
std::uint64_t widen(std::uint32_t bits)
{
    const auto sign = std::uint64_t{bits & 0x80000000U} << 32;
    const unsigned exponent = (bits >> 23) & 255;
    auto fraction = bits & 0x7fffffU;
    if (exponent == 255) return sign | 0x7ff0000000000000ULL |
        (fraction ? (std::uint64_t{fraction} << 29) | 0x0008000000000000ULL : 0);
    if (exponent) return sign | (std::uint64_t{exponent + 896} << 52) | (std::uint64_t{fraction} << 29);
    if (!fraction) return sign;
    unsigned shifts = 0;
    while (!(fraction & 0x800000U)) { fraction <<= 1; ++shifts; }
    return sign | (std::uint64_t{897 - shifts} << 52) | (std::uint64_t{fraction & 0x7fffffU} << 29);
}
constexpr std::array floatCases{0U, 0x80000000U, 1U, 0x80000001U, 0x007fffffU, 0x00800000U,
    0x3f800000U, 0x3f800001U, 0xbf800001U, 0x3eaaaaabU, 0x7f7fffffU, 0xff7fffffU,
    0x7f800000U, 0xff800000U, 0x7fc12345U, 0xff812345U, 0x7f800001U, 0xffc54321U};
struct Narrow { std::uint64_t from; std::uint32_t to; };
// Literal nearest-even expectations: ties, extra precision, subnormals, overflow, signed zero and NaNs.
constexpr std::array doubleCases{
    Narrow{0, 0}, Narrow{0x8000000000000000ULL, 0x80000000U},
    Narrow{0x3ff0000000001000ULL, 0x3f800000U}, Narrow{0xbff0000000001000ULL, 0xbf800000U},
    Narrow{0x3ff0000010000000ULL, 0x3f800000U}, Narrow{0x3ff0000030000000ULL, 0x3f800002U},
    Narrow{0x3ff0000020000000ULL, 0x3f800001U}, Narrow{0x36a0000000000000ULL, 1},
    Narrow{0x3690000000000000ULL, 0}, Narrow{0xb690000000000000ULL, 0x80000000U},
    Narrow{0x36a8000000000000ULL, 2}, Narrow{0x36b4000000000000ULL, 2},
    Narrow{1, 0}, Narrow{0x8000000000000001ULL, 0x80000000U},
    Narrow{0x3810000000000000ULL, 0x00800000U}, Narrow{0x47efffffe0000000ULL, 0x7f7fffffU},
    Narrow{0x47effffff0000000ULL, 0x7f800000U}, Narrow{0xc7effffff0000000ULL, 0xff800000U},
    Narrow{0x7ff0000000000000ULL, 0x7f800000U}, Narrow{0xfff0000000000000ULL, 0xff800000U},
    Narrow{0x7ff8000020000000ULL, 0x7fc00001U}, Narrow{0xfff0000040000000ULL, 0xffc00002U}};

void unchangedConversions(DspBackend backend)
{
    constexpr unsigned frames = 137;
    for (bool narrow : {false, true}) for (bool add : {false, true}) {
        Graph graph(backend);
        std::string code;
        if (narrow) {
            code = "streamin first,second;streamout out;float io[8],tmp;"
                "movaps xmm0,first;movaps io[0],xmm0;movaps xmm0,second;movaps io[4],xmm0;";
            if (add) code += "xorps xmm0,xmm0;";
        } else {
            code = "streamin io;streamout first,second;float tmp[8];";
            if (add) code += "xorpd xmm0,xmm0;xorpd xmm1,xmm1;";
        }
        code += fixture(std::string("engine-") + (narrow ? "double-to-float-" : "float-to-double-") + (add ? "add.asm" : "move.asm"));
        // Capture raw result words, avoiding an additional numerical conversion in the readout.
        code += narrow ? "movaps out,xmm0;" : "movaps first,xmm0;movaps second,xmm1;";
        const auto block = graph.addDspCode(compileDspCode(code, DspLanguage::Assembler));
        for (unsigned p = 0; p < (narrow ? 2U : 1U); ++p) graph.connectDspInput(graph.addInput(), block.id, p);
        auto runtime = graph.compileMono4(block.outputs);
        std::array<std::array<Mono4, frames>, 2> input{};
        std::array<std::array<Words, frames>, 2> expected{};
        for (unsigned n = 0; n < frames; ++n) for (unsigned lane = 0; lane < 4; ++lane) {
            if (narrow) {
                auto c = doubleCases[(n + lane * 3) % doubleCases.size()];
                auto words = std::bit_cast<Words>(input[lane / 2][n]);
                words[(lane % 2) * 2] = static_cast<std::uint32_t>(c.from);
                words[(lane % 2) * 2 + 1] = static_cast<std::uint32_t>(c.from >> 32);
                input[lane / 2][n] = std::bit_cast<Mono4>(words);
                // The add fragment then uses ADDPS, whose existing engine policy flushes
                // subnormal operands. The move fragment must retain the x87 result bits.
                expected[0][n][lane] = add && (c.to & 0x7fffffffU) < 0x00800000U ? 0 : c.to;
            } else {
                const auto bits = floatCases[(n + lane * 3) % floatCases.size()];
                input[0][n][lane] = std::bit_cast<float>(bits);
                const auto result = add && !(bits & 0x7fffffffU) ? 0 : widen(bits);
                expected[lane / 2][n][(lane % 2) * 2] = static_cast<std::uint32_t>(result);
                expected[lane / 2][n][(lane % 2) * 2 + 1] = static_cast<std::uint32_t>(result >> 32);
            }
        }
        const auto original = std::bit_cast<std::array<std::array<Words, frames>, 2>>(input);
        for (int mode : {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD}) for (const auto size : bufferSizes) {
            runtime.reset();
            std::fesetround(mode); std::feclearexcept(FE_ALL_EXCEPT); std::feraiseexcept(FE_DIVBYZERO);
            std::array<std::array<Mono4, frames>, 2> output{};
            for (unsigned offset = 0; offset < frames; offset += size) {
                const auto count = std::min<std::size_t>(size, frames - offset);
                std::array<Mono4InputBuffer, 2> in;
                std::array<Mono4OutputBuffer, 2> out;
                for (unsigned p = 0; p < 2; ++p) { in[p] = std::span{input[p]}.subspan(offset, count); out[p] = std::span{output[p]}.subspan(offset, count); }
                check(runtime.process(std::span{in}.first(narrow ? 2 : 1), std::span{out}.first(narrow ? 1 : 2)), "Render the unchanged native conversion body");
                check(std::fegetround() == mode && std::fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO, "x87 conversion restores caller rounding and sticky exception flags");
            }
            for (unsigned p = 0; p < (narrow ? 1U : 2U); ++p) for (unsigned n = 0; n < frames; ++n) for (unsigned lane = 0; lane < 4; ++lane) {
                const auto actual = std::bit_cast<Words>(output[p][n])[lane];
                if (actual != expected[p][n][lane]) throw std::runtime_error("Conversion mismatch: narrow=" + std::to_string(narrow) +
                    " add=" + std::to_string(add) + " port=" + std::to_string(p) + " frame=" + std::to_string(n) +
                    " word=" + std::to_string(lane) + " actual=" + std::to_string(actual) + " expected=" + std::to_string(expected[p][n][lane]));
            }
        }
        check(std::bit_cast<std::array<std::array<Words, frames>, 2>>(input) == original, "Native conversion fragments preserve caller input words and NaN payloads");
    }
}

std::uint64_t read(const std::vector<std::uint8_t>& bytes, int offset, unsigned width)
{
    if (offset < 0 || offset % 4 || std::size_t(offset) + width > bytes.size()) return 0;
    std::uint64_t result = 0;
    for (unsigned i = 0; i < width; ++i) result |= std::uint64_t{bytes[offset + i]} << (8 * i);
    return result;
}
void write(std::vector<std::uint8_t>& bytes, int offset, unsigned width, std::uint64_t value)
{
    if (offset < 0 || offset % 4 || std::size_t(offset) + width > bytes.size()) return;
    for (unsigned i = 0; i < width; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
}
void mixedMemory(DspBackend backend)
{
    struct Storage { const char* declaration; unsigned bytes; };
    constexpr std::array stores{Storage{"float target;",16}, Storage{"double target;",32}, Storage{"int target;",16},
        Storage{"float target[6];",24}, Storage{"double target[3];",24}, Storage{"int target[6];",24}};
    constexpr std::array seed{0x11111111U,0x3ff00000U,0x22222222U,0x40000000U,0x33333333U,0x40080000U,0x44444444U,0x40100000U};
    constexpr std::array positions{-8,-4,0,1,3,4,8,12,16,20,24,28,32,1000000};
    constexpr std::array values{-2.5F,-1.5F,-.5F,.5F,1.5F,2.5F,17.25F,0.F};
    constexpr std::array integers{-2,-2,0,0,2,2,17,0};
    constexpr unsigned frames = 43;
    for (const auto storage : stores) for (unsigned kind = 0; kind < 3; ++kind) {
        const unsigned width = kind == 1 ? 8 : 4;
        const std::string store = kind == 1 ? "dfst" : kind == 2 ? "fist" : "fst";
        const std::string load = kind == 1 ? "dfld" : kind == 2 ? "fild" : "fld";
        std::string code = "streamin index,value;streamout first,tail,loaded,guard;" + std::string(storage.declaration) +
            "int seed0={0x11111111,0x3ff00000,0x22222222,0x40000000};"
            "int seed1={0x33333333,0x40080000,0x44444444,0x40100000};int sentinel={7,11,13,17};double back;"
            "movaps xmm0,seed0;movaps target[0],xmm0;";
        if (storage.bytes > 16) code += "mov ebx,16;movaps xmm0,seed1;movaps target[ebx],xmm0;";
        code += "cvtss2si eax,index;fld value[0];" + store + " target[eax];fstp st(0);" + load + " target[eax];dfstp back;"
            "movaps xmm0,target[0];movaps first,xmm0;mov ebx,16;movaps xmm0,target[ebx];movaps tail,xmm0;"
            "movaps xmm0,back;movaps loaded,xmm0;movaps xmm0,sentinel;movaps guard,xmm0;";
        Graph graph(backend); const auto b = graph.addDspCode(compileDspCode(code, DspLanguage::Assembler));
        graph.connectDspInput(graph.addInput(), b.id, 0); graph.connectDspInput(graph.addInput(), b.id, 1);
        auto runtime = graph.compileMono4(b.outputs);
        std::array<std::array<Mono4, frames>, 2> input{};
        std::array<std::array<Words, frames>, 3> expected{};
        for (unsigned n = 0; n < frames; ++n) {
            const int offset = positions[n % positions.size()]; const auto vi = n % values.size();
            input[0][n].fill(float(offset)); input[1][n].fill(values[vi]);
            std::vector<std::uint8_t> bytes(storage.bytes);
            for (unsigned i = 0; i < storage.bytes / 4; ++i) write(bytes, i * 4, 4, seed[i]);
            const auto bits = std::bit_cast<std::uint32_t>(values[vi]);
            write(bytes, offset, width, kind == 1 ? widen(bits) : kind == 2 ? static_cast<std::uint32_t>(integers[vi]) : bits);
            for (unsigned word = 0; word < 8; ++word) expected[word / 4][n][word % 4] = static_cast<std::uint32_t>(read(bytes, word * 4, 4));
            const auto raw = read(bytes, offset, width);
            const auto back = kind == 1 ? raw : kind == 2 ? std::bit_cast<std::uint64_t>(double(std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(raw)))) : widen(static_cast<std::uint32_t>(raw));
            expected[2][n] = {static_cast<std::uint32_t>(back), static_cast<std::uint32_t>(back >> 32), 0, 0};
        }
        for (const auto size : bufferSizes) {
            runtime.reset(); std::array<std::array<Mono4, frames>, 4> output{};
            for (unsigned offset = 0; offset < frames; offset += size) {
                const auto count = std::min<std::size_t>(size, frames - offset);
                std::array<Mono4OutputBuffer, 4> out;
                for (unsigned p = 0; p < 4; ++p) out[p] = std::span{output[p]}.subspan(offset, count);
                check(runtime.process(std::array{Mono4InputBuffer{std::span{input[0]}.subspan(offset,count)},Mono4InputBuffer{std::span{input[1]}.subspan(offset,count)}},out), "Render cross-width x87 loads/stores through packet and array memory");
            }
            for (unsigned n = 0; n < frames; ++n) {
                for (unsigned p = 0; p < 3; ++p) check(std::bit_cast<Words>(output[p][n]) == expected[p][n], "The instruction width controls raw bytes, half-Double preservation, four-byte alignment and complete-access bounds");
                check(std::bit_cast<Words>(output[3][n]) == Words{7,11,13,17}, "Mixed-width writes cannot overrun neighboring storage");
            }
        }
    }
}

void inputIsolationAndProvenance(DspBackend backend)
{
    Graph graph(backend); const auto root = graph.addInput();
    const auto b = graph.addDspCode(compileDspCode("streamdoublein value;streamdoubleout out;fld1;fstp value;movapd xmm0,value[0];movapd out[0],xmm0;movapd xmm0,value[2];movapd out[2],xmm0;",DspLanguage::Assembler));
    graph.connectDspInput(root,b.id,0); auto runtime = graph.compileMonoDouble4(std::array{b.outputs[0],root});
    std::array<MonoDouble4,17> input{},out{},peer{}; input.fill({1.0+0x1p-40,2,3,4}); const auto original = input;
    for (const auto size : bufferSizes) {
        runtime.reset();
        for (unsigned offset=0;offset<input.size();offset+=size) {
            const auto count=std::min<std::size_t>(size,input.size()-offset);
            check(runtime.process(std::array{MonoDouble4InputBuffer{std::span{input}.subspan(offset,count)}},std::array{MonoDouble4OutputBuffer{std::span{out}.subspan(offset,count)},MonoDouble4OutputBuffer{std::span{peer}.subspan(offset,count)}}),"Render partial x87 writes to private signal input copies");
        }
        for (unsigned n=0;n<input.size();++n) {
            check(std::bit_cast<std::uint64_t>(out[n][0])==0x3ff000003f800000ULL && out[n][1]==2 && out[n][2]==3 && out[n][3]==4,"Bare FSTP writes one raw Float word and preserves the rest of a Double signal");
            check(peer[n]==original[n] && input[n]==original[n],"Every sample refreshes the private input copy without mutating fan-out or caller buffers");
        }
    }
    for (const auto source : {
        "int table[4];mov eax,leaf;mov table[1],eax;fld1;dfstp table[0];mov eax,table[1];call eax;jmp done;leaf:ret;done:",
        "floatarrayin external[4];fld1;dfstp external[0];",
        "memrefin mem;fld1;fstp mem[4];",
        "memarrayrefin mem;fld1;dfstp mem[0];",
        "streamin in;stage0;fld1;dfstp in;",
        "streamin in;stage0;dfld in;fstp st(0);"
    }) {
        bool rejected=false;
        try { static_cast<void>(compileDspCode("streamout out;"+std::string(source),DspLanguage::Assembler)); }
        catch(const DspCompileError& e) {
            rejected=e.line && e.column;
            if(std::string_view(source).starts_with("int table[4]"))
                rejected=rejected && std::string_view(e.what()).find("call register must hold a local subroutine label")!=std::string_view::npos;
        }
        check(rejected,"Cross-width stores clear every overlapping routine-target tag and retain descriptor, external-array and stage-zero input restrictions");
    }
    Graph targets(backend);const auto t=targets.addDspCode(compileDspCode("streamout out;int table[4];mov eax,leaf;mov table[2],eax;fld1;dfstp table[0];mov eax,table[2];call eax;jmp done;leaf:movaps xmm0,_ONE;movaps out,xmm0;ret;done:",DspLanguage::Assembler));
    auto run=targets.compileMono4(t.outputs);std::array<Mono4,3> result{};
    check(run.process({},std::array{Mono4OutputBuffer{result}}),"A disjoint routine-target cell remains callable");
    for(auto packet:result)check(packet==Mono4{1,1,1,1},"Only overlapping target provenance is invalidated");
}

void polyScratch(DspBackend backend)
{
    Graph graph(backend);const auto b=graph.addDspCode(compileDspCode("polyin io;polyout out;float tmp[8];"+fixture("engine-float-to-double-move.asm")+
        "movapd tmp[0],xmm0;movapd tmp[4],xmm1;dfld tmp[0];fstp out[0];dfld tmp[2];fstp out[1];dfld tmp[4];fstp out[2];dfld tmp[6];fstp out[3];",DspLanguage::Assembler));
    graph.connectDspInput(graph.addInput(),b.id,0);const std::array bindings{PolyInput{PolyInputKind::Velocity}};
    PolyRuntime runtime(graph,bindings,b.outputs,{.maxVoices=5,.holdAfterNoteOff=true});
    const std::array events{PolyEvent{0,PolyEventKind::NoteOn,1,60,.125F},PolyEvent{0,PolyEventKind::NoteOn,2,61,.25F},PolyEvent{0,PolyEventKind::NoteOn,3,62,.5F},PolyEvent{0,PolyEventKind::NoteOn,4,63,.75F},PolyEvent{0,PolyEventKind::NoteOn,5,64,1.F},PolyEvent{4,PolyEventKind::NoteOff,3},PolyEvent{7,PolyEventKind::EndVoice,3},PolyEvent{9,PolyEventKind::NoteOn,6,65,.375F}};
    for(const auto size:bufferSizes) {
        runtime.reset();std::array<float,17> out{};
        for(unsigned offset=0;offset<out.size();offset+=size) {
            const auto count=std::min<std::size_t>(size,out.size()-offset);std::vector<PolyEvent> part;
            for(auto event:events)if(event.frame>=offset && event.frame<offset+count){event.frame-=offset;part.push_back(event);}
            check(runtime.process({},std::array{OutputBuffer{std::span{out}.subspan(offset,count)}},part),"Run mixed-width conversion scratch memory across two Poly packets and reused slots");
        }
        for(unsigned n=0;n<out.size();++n)check(out[n]==(n<7 ? 2.625F : n<9 ? 2.125F : 2.5F),"Each active/released/reused voice retains its own converted sample");
    }
}
}
int main()
{
    try {
        Environment environment;
        for(auto backend:{DspBackend::Interpreter,DspBackend::LlvmOrc})if(dspBackendAvailable(backend)) {
            openstone::tests::withReferenceSources("unchangedConversions", [&] { unchangedConversions(backend); }); std::fesetround(FE_TONEAREST);
            mixedMemory(backend); inputIsolationAndProvenance(backend); openstone::tests::withReferenceSources("polyScratch", [&] { polyScratch(backend); });
        }
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
