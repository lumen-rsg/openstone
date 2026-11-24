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
#include <type_traits>
#include <vector>

namespace {
using namespace openstone::stream;
using Memory = openstone::green::Memory;
using Words = std::array<std::uint32_t, 4>;
constexpr std::array<std::size_t, 6> bufferSizes{1, 3, 7, 31, 64, 257};
void check(bool value, std::string_view message) { if (!value) throw std::runtime_error(std::string(message)); }
struct Environment {
    std::fenv_t saved;
    Environment() { std::fegetenv(&saved); std::fesetround(FE_TONEAREST); }
    ~Environment() { std::fesetenv(&saved); }
};
std::string writer(bool stereo)
{
    std::ifstream file(std::filesystem::path(OPENSTONE_TEST_FIXTURES) / "flowstone" /
        (stereo ? "engine-mem-writer-stereo.asm" : "engine-mem-writer-mono.asm"), std::ios::binary);
    check(file.good(), "Read unchanged native Wave Write source");
    return (stereo ? "streamin in1,in2,in3;streamout out0,out1;" : "streamin in1,in2;streamout out0;") +
        std::string(std::istreambuf_iterator<char>(file), {});
}

// Construct literal native descriptor bytes independently of the compiler/cache representation.
Words descriptorWindow(const Memory& memory, unsigned field)
{
    std::array<std::uint32_t, 12> words{};
    words[2] = static_cast<std::uint32_t>(memory.size());
    words[4] = static_cast<std::uint32_t>(memory.size() / 4);
    words[6] = static_cast<std::uint32_t>(memory.channels());
    const auto count = memory.size() / (memory.channels() == 2 ? 8U : 4U);
    const auto limit = memory.size() ? std::bit_cast<std::uint32_t>(float(int(count) - 1)) : 0;
    std::fill(words.begin() + 8, words.end(), limit);
    Words result{};
    std::copy_n(words.begin() + field * 2, 4, result.begin());
    return result;
}
void rawWindows(DspBackend backend)
{
    Memory owner(std::vector<std::uint8_t>(300, 0xa5));
    const auto original = owner.snapshotBytes();
    for (bool doubles : {false, true}) {
        Graph graph(backend);
        std::string code = "memrefin first,second;floatarrayin sentinel[1];";
        code += doubles ? "streamdoubleout a,b,c,d,peer;" : "streamout a,b,c,d,peer;";
        code += "streamout sentinelValue;";
        const std::array<std::string, 5> names{"a", "b", "c", "d", "peer"};
        for (unsigned i = 0; i < names.size(); ++i) {
            const auto source = (i == 4 ? "second[2]" : "first[" + std::to_string(i + 1) + "]");
            if (doubles) code += "movapd xmm0," + source + ";movapd " + names[i] + "[0],xmm0;movapd " + names[i] + "[2],xmm0;";
            else code += "movups xmm0," + source + ";movaps " + names[i] + ",xmm0;";
        }
        code += "movaps xmm0,sentinel[0];movaps sentinelValue,xmm0;";
        const auto b = graph.addDspCode(compileDspCode(code, DspLanguage::Assembler));
        const auto run = [&]<typename Sample>(detail::Runtime<Sample> runtime) {
            const auto peer = owner.view(3, 19, 2);
            check(runtime.setDspMemRefInput(b.id, 1, peer), "Bind independent adjacent descriptor");
            check(runtime.setDspArrayInput(b.id, 2, std::array{7.F}), "Bind neighboring array slot");
            for (const auto size : bufferSizes) {
                runtime.reset();
                for (int channels : {1, 2}) for (unsigned bytes : {0U, 1U, 3U, 4U, 7U, 8U, 15U, 16U, 19U, 31U, 257U}) {
                    const auto memory = owner.view(1, bytes, channels);
                    check(runtime.setDspMemRefInput(b.id, 0, memory), "Refresh packed metadata without recompilation");
                    std::array<std::array<Sample, 17>, 6> values{};
                    for (unsigned offset = 0; offset < 17; offset += size) {
                        const auto count = std::min<std::size_t>(size, 17 - offset);
                        std::array<std::span<Sample>, 6> out;
                        for (unsigned p = 0; p < 6; ++p) out[p] = std::span{values[p]}.subspan(offset, count);
                        check(runtime.process({}, out), "Read numeric-to-limit descriptor windows as raw PS/PD packets");
                    }
                    for (unsigned p = 0; p < 5; ++p) for (const auto& packet : values[p]) {
                        const auto words = descriptorWindow(p == 4 ? peer : memory, p == 4 ? 2 : p + 1);
                        if constexpr (std::is_same_v<Sample, Mono4>) {
                            check(std::bit_cast<Words>(packet) == words, "Packed reads span adjacent eight-byte fields, including the Float-limit overlap");
                        } else {
                            const auto pair = std::bit_cast<std::array<std::uint64_t, 2>>(words);
                            check(std::bit_cast<std::array<std::uint64_t, 4>>(packet) ==
                                std::array{pair[0], pair[1], pair[0], pair[1]}, "PD reads preserve descriptor bits without numeric conversion or subnormal flushing");
                        }
                    }
                    for (const auto& packet : values[5]) check(packet == Sample{7, 0, 0, 0}, "Descriptor rebindings cannot overwrite a following array input");
                }
            }
        };
        if (doubles) run(graph.compileMonoDouble4(b.outputs)); else run(graph.compileMono4(b.outputs));
    }
    check(owner.snapshotBytes() == original, "Descriptor reads leave unaligned source and owner bytes untouched");
}

void stagesFlagsAndRestrictions(DspBackend backend)
{
    Graph graph(backend);
    const auto b = graph.addDspCode(compileDspCode(
        "memrefin mem;streamout initial,early,late,flags;"
        "stage0;movaps xmm0,mem[2];movaps initial,xmm0;"
        "stage1;movaps xmm0,mem[3];movaps early,xmm0;"
        "stage2;mov eax,7;cmp eax,7;movaps xmm0,mem[1];movapd xmm1,mem[3];"
        "movaps xmm0,_ZERO;jne done;movaps xmm0,_ONE;done:movaps flags,xmm0;"
        "stage3;movaps xmm0,mem[2];movaps late,xmm0;", DspLanguage::Assembler));
    auto runtime = graph.compileMono4(b.outputs);
    const std::array memories{Memory(std::vector<std::uint8_t>(20), 1), Memory(std::vector<std::uint8_t>(23), 2), Memory{}, Memory(std::vector<std::uint8_t>(3), 1)};
    for (const auto size : bufferSizes) {
        runtime.reset();
        Words previous{};
        for (const auto& memory : memories) {
            check(runtime.setDspMemRefInput(b.id, 0, memory), "Rebind raw descriptor reads in all stages");
            std::array<std::array<Mono4, 5>, 4> values{};
            check(runtime.process({}, std::array<Mono4OutputBuffer, 4>{}), "Empty calls do not initialize descriptor outputs");
            for (unsigned offset = 0; offset < 5; offset += size) {
                const auto count = std::min<std::size_t>(size, 5 - offset);
                std::array<Mono4OutputBuffer, 4> out;
                for (unsigned p = 0; p < 4; ++p) out[p] = std::span{values[p]}.subspan(offset, count);
                check(runtime.process({}, out), "Render raw descriptor packets across stage boundaries");
            }
            for (unsigned n = 0; n < 5; ++n) {
                check(std::bit_cast<Words>(values[0][n]) == descriptorWindow(memories[0], 2), "Stage-zero packet stays seeded until reset");
                check(std::bit_cast<Words>(values[1][n]) == descriptorWindow(memory, 3), "Early packets observe current metadata and limits");
                check(std::bit_cast<Words>(values[2][n]) == (n ? descriptorWindow(memory, 2) : previous), "Late packet visibility stays one sample across rebindings and clears");
                check(values[3][n] == Mono4{1, 1, 1, 1}, "Packed descriptor reads preserve integer flags");
            }
            previous = descriptorWindow(memory, 2);
        }
    }
    for (unsigned field = 0; field <= 4; ++field) for (const auto move : {"movaps", "movapd"}) {
        bool rejected = false;
        try { static_cast<void>(compileDspCode("memrefin mem;streamout out;movaps xmm0,_ONE;" + std::string(move) + " mem[" + std::to_string(field) + "],xmm0;", DspLanguage::Assembler)); }
        catch (const DspCompileError& e) { rejected = std::string(e.what()).find("cannot write a Mem descriptor") != std::string::npos; }
        check(rejected, "Every packed Mem descriptor field remains read-only");
    }
    for (const auto body : {"movaps xmm0,mem[0];", "movapd xmm0,mem[5];", "mov eax,mem[2];fld [eax];fstp out[0];"}) {
        bool rejected = false;
        try { static_cast<void>(compileDspCode("memrefin mem;streamout out;" + std::string(body), DspLanguage::Assembler)); }
        catch (const DspCompileError&) { rejected = true; }
        check(rejected, "Opaque pointer bits, invalid selectors and numeric-to-pointer casts remain unavailable");
    }
}

void put(std::vector<std::uint8_t>& bytes, std::size_t offset, float value)
{
    check(offset + 4 <= bytes.size(), "Reference writer stays within its bounded owner");
    const auto word = std::bit_cast<std::uint32_t>(value);
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(word >> (8 * i));
}
// Model the source instructions and branch order, including the root lane-zero early exit.
void writeReference(std::vector<std::uint8_t>& bytes, std::size_t start, std::size_t length, int channels,
    bool stereo, bool poly, unsigned active, const Mono4& positions, const Mono4& left, const Mono4& right)
{
    if (!length) return;
    const auto count = stereo ? length / 8 : length / (channels == 2 ? 8 : 4);
    const auto limit = float(int(count) - 1);
    bool first = false;
    for (unsigned lane = 0; lane < 4; ++lane) {
        const float position = positions[lane] > 0 ? positions[lane] : 0; // MAXPS chooses its zero second operand for NaN/ties.
        if (!(active & (1U << lane)) || !(position <= limit)) continue;
        if (lane == 0) first = true;
        const auto frame = static_cast<std::size_t>(std::nearbyint(position));
        const auto offset = frame * (stereo ? 8 : 4);
        check(offset + (stereo ? 8 : 4) <= length, "Independent RNE write fits the selected descriptor");
        put(bytes, start + offset, left[lane]);
        if (stereo) put(bytes, start + offset + 4, right[lane]);
        if (first && !poly) break;
    }
}

void unchangedWriters(DspBackend backend)
{
    constexpr std::size_t frames = 149;
    constexpr std::array changes{0U, 11U, 37U, 58U, 83U, 111U};
    struct Binding { unsigned start, length; int channels; };
    constexpr std::array bindings{Binding{3, 0, 1}, Binding{3, 71, 1}, Binding{3, 79, 2}, Binding{3, 3, 1}, Binding{3, 0, 2}, Binding{3, 8, 2}};
    const std::array<float, 16> positions{-100, -.5F, -0.F, 0, .49F, .5F, 1.5F, 2.5F, 3.5F, 8, 17, 1000,
        std::bit_cast<float>(0x7fc12345U), std::bit_cast<float>(0x7f800000U), std::bit_cast<float>(0xff800000U), 4};
    std::array<std::array<Mono4, frames>, 3> input{};
    for (unsigned n = 0; n < frames; ++n) for (unsigned lane = 0; lane < 4; ++lane) {
        input[0][n][lane] = positions[(n + lane * 3) % positions.size()];
        input[1][n][lane] = (n + lane) % 7 ? float(n * 4 + lane) * .125F : -0.F;
        input[2][n][lane] = -float(n * 4 + lane + 1) * .25F;
    }
    const auto originalInput = std::bit_cast<std::array<std::array<Words, frames>, 3>>(input);
    for (bool stereo : {false, true}) {
        Graph graph(backend);
        const auto b = graph.addDspCode(compileDspCode(writer(stereo), DspLanguage::Assembler));
        for (unsigned p = 0; p < (stereo ? 3U : 2U); ++p) graph.connectDspInput(graph.addInput(), b.id, p);
        auto runtime = graph.compileMono4(b.outputs);
        for (const auto size : bufferSizes) {
            runtime.reset();
            Memory owner(std::vector<std::uint8_t>(101, 0xa5));
            auto expected = owner.snapshotBytes();
            for (unsigned offset = 0, event = 0; offset < frames;) {
                if (event + 1 < changes.size() && offset == changes[event + 1]) ++event;
                const auto binding = bindings[event];
                const auto memory = owner.view(binding.start, binding.length, binding.channels);
                check(runtime.setDspMemRefInput(b.id, stereo ? 3 : 2, memory), "Change/clear the writer destination between buffers");
                const auto end = event + 1 < changes.size() ? changes[event + 1] : frames;
                const auto count = std::min<std::size_t>({size, frames - offset, end - offset});
                std::array<Mono4InputBuffer, 3> in;
                std::array<std::array<Mono4, frames>, 2> values{};
                std::array<Mono4OutputBuffer, 2> out;
                for (unsigned p = 0; p < in.size(); ++p) in[p] = std::span{input[p]}.subspan(offset, count);
                for (unsigned p = 0; p < out.size(); ++p) out[p] = std::span{values[p]}.first(count);
                const auto rounding = std::fegetround(), flags = std::fetestexcept(FE_ALL_EXCEPT);
                check(runtime.process(std::span{in}.first(stereo ? 3 : 2), std::span{out}.first(stereo ? 2 : 1)), "Run the complete unchanged native writer body");
                check(std::fegetround() == rounding && std::fetestexcept(FE_ALL_EXCEPT) == flags, "CVTPS2DQ and x87 stores preserve caller rounding/exception state");
                for (unsigned n = 0; n < count; ++n) {
                    writeReference(expected, binding.start, binding.length, binding.channels, stereo, false, 15,
                        input[0][offset + n], input[1][offset + n], input[2][offset + n]);
                    for (unsigned p = 0; p < (stereo ? 2U : 1U); ++p)
                        check(std::bit_cast<Words>(values[p][n]) == std::bit_cast<Words>(input[p + 1][offset + n]), "Every passthrough sample/lane retains its exact source bits");
                }
                check(owner.snapshotBytes() == expected, "Every buffer matches independent write bounds, RNE indices, branch order and owner guards");
                offset += count;
            }
        }
    }
    check(std::bit_cast<std::array<std::array<Words, frames>, 3>>(input) == originalInput, "Writers never mutate caller inputs");
}

void polyWriters(DspBackend backend)
{
    constexpr unsigned frames = 29;
    const std::array events{PolyEvent{0, PolyEventKind::NoteOn, 1, 1, .125F}, PolyEvent{0, PolyEventKind::NoteOn, 2, 2, .25F},
        PolyEvent{0, PolyEventKind::NoteOn, 3, 3, .5F}, PolyEvent{2, PolyEventKind::NoteOn, 4, 4, .75F},
        PolyEvent{5, PolyEventKind::NoteOn, 5, 1, 1.F}, PolyEvent{7, PolyEventKind::NoteOff, 3},
        PolyEvent{9, PolyEventKind::EndVoice, 3}, PolyEvent{10, PolyEventKind::NoteOn, 6, 3, .375F},
        PolyEvent{18, PolyEventKind::NoteOff, 1}, PolyEvent{19, PolyEventKind::EndVoice, 1}, PolyEvent{22, PolyEventKind::AllSoundOff}};
    struct Voice { NoteId id{}; int pitch{}; float velocity{}; bool released{}; };
    for (bool stereo : {false, true}) {
        Graph graph(backend);
        const auto b = graph.addDspCode(compileDspCode(writer(stereo), DspLanguage::Assembler));
        for (unsigned p = 0; p < (stereo ? 3U : 2U); ++p) graph.connectDspInput(graph.addInput(), b.id, p);
        std::vector<PolyInput> inputs{{PolyInputKind::Pitch}, {PolyInputKind::Velocity}};
        if (stereo) inputs.push_back({PolyInputKind::Mono, 0});
        PolyRuntime runtime(graph, inputs, b.outputs, {.maxVoices = 5, .holdAfterNoteOff = true});
        for (const auto size : bufferSizes) {
            runtime.reset();
            Memory owner(std::vector<std::uint8_t>(107, 0xa5));
            auto expected = owner.snapshotBytes();
            std::array<Voice, 5> voices{};
            std::array<float, frames> right{};
            for (unsigned n = 0; n < frames; ++n) right[n] = n * .125F;
            for (unsigned offset = 0; offset < frames;) {
                // Bindings change at fixed sample boundaries regardless of the requested host buffer size.
                const auto end = offset < 8 ? 8U : offset < 13 ? 13U : offset < 17 ? 17U : frames;
                const auto count = std::min<std::size_t>({size, frames - offset, end - offset});
                const unsigned length = offset >= 13 && offset < 17 ? 0 : offset >= 8 ? 47 : 96;
                const int channels = offset >= 8 ? 2 : 1;
                check(runtime.setDspMemRefInput(b.id, stereo ? 3 : 2, owner.view(3, length, channels)), "Rebind writer Mem across both Poly packets");
                std::vector<PolyEvent> part;
                for (auto event : events) if (event.frame >= offset && event.frame < offset + count) { event.frame -= offset; part.push_back(event); }
                std::array<std::array<float, frames>, 2> values{};
                std::array<OutputBuffer, 2> out{std::span{values[0]}.first(count), std::span{values[1]}.first(count)};
                const std::array in{InputBuffer{std::span{right}.subspan(offset, count)}};
                check(runtime.process(stereo ? std::span<const InputBuffer>{in} : std::span<const InputBuffer>{}, std::span{out}.first(stereo ? 2 : 1), part), "Run unchanged writers across five voices, release and slot reuse");
                for (unsigned n = 0; n < count; ++n) {
                    const auto frame = offset + n;
                    for (const auto& event : events) if (event.frame == frame) {
                        if (event.kind == PolyEventKind::AllSoundOff) voices = {};
                        else if (event.kind == PolyEventKind::NoteOn) {
                            const auto slot = std::find_if(voices.begin(), voices.end(), [](const auto& voice) { return !voice.id; });
                            check(slot != voices.end(), "Independent oracle has an unused voice slot");
                            *slot = {event.id, event.pitch, event.velocity, false};
                        } else for (auto& voice : voices) if (voice.id == event.id) {
                            if (event.kind == PolyEventKind::NoteOff) voice.released = true;
                            else if (voice.released) voice = {};
                        }
                    }
                    float sum = 0, sumRight = 0;
                    for (unsigned first = 0; first < voices.size(); first += 4) {
                        unsigned active = 0;
                        Mono4 position{}, left{}, r{};
                        for (unsigned lane = 0; lane < 4 && first + lane < voices.size(); ++lane) {
                            const auto& voice = voices[first + lane];
                            if (!voice.id) continue;
                            active |= 1U << lane;
                            position[lane] = float(voice.pitch); left[lane] = voice.velocity; r[lane] = right[frame];
                            sum += voice.velocity; sumRight += right[frame];
                        }
                        writeReference(expected, 3, length, channels, stereo, true, active, position, left, r);
                    }
                    check(values[0][n] == sum && (!stereo || values[1][n] == sumRight), "Poly passthrough mixing includes held/released voices and excludes inactive lanes");
                }
                check(owner.snapshotBytes() == expected, "Packed writers honor channel masks, packet/lane order, overlapping destinations and reused slots");
                offset += count;
            }
        }
    }
}
}
int main()
{
    try {
        Environment environment;
        for (auto backend : {DspBackend::Interpreter, DspBackend::LlvmOrc}) if (dspBackendAvailable(backend)) {
            rawWindows(backend); stagesFlagsAndRestrictions(backend); openstone::tests::withReferenceSources("unchangedWriters", [&] { unchangedWriters(backend); }); openstone::tests::withReferenceSources("polyWriters", [&] { polyWriters(backend); });
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
