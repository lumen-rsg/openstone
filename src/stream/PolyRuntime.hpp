#pragma once

#include "stream/Runtime.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace openstone::stream {

// One manager owns one Poly section. A note instance has its own DSP state,
// even when another note has the same pitch. IDs increase until reset().
using NoteId = std::uint64_t;

enum class VoiceStage { Off = 0, On = 1, Execute = 3, Release = 4 };
enum class PolyEventKind { NoteOn, NoteOff, EndVoice, AllSoundOff };

struct PolyEvent {
    std::size_t frame{}; // Offset within this process call; applied before that sample.
    PolyEventKind kind{};
    NoteId id{};
    int pitch = 60;        // MIDI note number, 0..127; used by NoteOn only.
    float velocity = 1;   // Normalized 0..1; used by NoteOn only (zero is still a note).
};

// One supported MIDI 1.0 message and its block-relative time.
// The low nibble of status encodes channels 1..16 as values 0..15.
struct MidiNoteEvent {
    std::size_t frame{};
    std::uint8_t status{};
    std::uint8_t pitch{};    // First data byte: note number, controller number, or bend LSB.
    std::uint8_t velocity{}; // Second data byte: note velocity, controller value, or bend MSB.
};

enum class PolyInputKind {
    Mono, Gate, NoteOn, Pitch, Velocity, Frequency, Stage,
    Modulation, ChannelVolume, Pan, Expression, MidiCC, MidiCC14,
};

// A disconnected Index selects every active voice. An explicit Index selects
// one zero-based voice slot, including while that voice is in release.
[[nodiscard]] constexpr bool matchesVoiceIndex(std::optional<std::int32_t> index, std::size_t slot) noexcept
{
    return !index || (*index >= 0 && static_cast<std::size_t>(*index) == slot);
}

// One binding per Graph::addInput(), in declaration order. Mono channel
// indices must be dense starting at zero.
// Modulation, ChannelVolume, and Expression are normalized to 0..1. Pan ranges
// from -1 (left) through 0 (center) to +1 (right). Voices started through process()
// use 0 for Modulation and Pan and 1 for ChannelVolume and Expression.
// MidiCC reads a 7-bit value. MidiCC14 reads a paired 14-bit value by its MSB
// controller number. Both read from the voice's MIDI channel or return 0 for
// direct Poly events.
struct PolyInput {
    PolyInputKind kind{};
    std::size_t channel{}; // Only used for Mono.
    std::size_t controller{}; // Only used for MidiCC or MidiCC14.
    std::optional<std::int32_t> voiceIndex{}; // Only used for Mono.
};

struct PolyOptions {
    std::size_t maxVoices = 16;
    double sampleRate = 48000;
    bool holdAfterNoteOff = false;
};

struct VoiceInfo {
    NoteId id{}; // Zero for an unused slot.
    VoiceStage stage = VoiceStage::Off;
    int pitch{};
    float velocity{};
};

// Four neighboring voice slots share DSP lanes; scalar graph state stays per voice.
// Construction/compilation happens off the
// audio thread; process/reset allocate nothing and belong to one thread.
class PolyRuntime {
public:
    // Outputs are summed in voice-slot order without normalization or clipping.
    // Optional releaseEnd is a per-voice graph tap: nonzero ends a released
    // voice AFTER mixing that sample. It cannot terminate a held note.
    PolyRuntime(const Graph& graph, std::span<const PolyInput> bindings,
                std::span<const NodeId> outputs, PolyOptions options = {},
                std::optional<NodeId> releaseEnd = std::nullopt);

    PolyRuntime(const PolyRuntime&) = delete;
    PolyRuntime& operator=(const PolyRuntime&) = delete;
    PolyRuntime(PolyRuntime&&) noexcept = default;
    PolyRuntime& operator=(PolyRuntime&&) noexcept = default;

    // All buffers have equal lengths. Exact input/output aliasing is supported;
    // partial overlaps and overlapping output channels are unsupported.
    // Events are sorted by frame, preserving caller order at equal frames.
    // NoteOn IDs are nonzero and strictly increasing across successful calls.
    // Stale NoteOff/EndVoice IDs are ignored. EndVoice only ends released voices.
    // Invalid blocks are rejected before changing state or writing any output.
    // Zero-frame calls accept no events and leave state unchanged.
    [[nodiscard]] bool process(std::span<const InputBuffer> monoInputs,
                               std::span<const OutputBuffer> mixedOutputs,
                               std::span<const PolyEvent> events = {}) noexcept;

    // Same buffer and timing contract as process(). Each Note On gets its own
    // voice ID. Note Off (including zero-velocity Note On) matches the oldest
    // unmatched Note On on that channel and pitch, even if its voice was stolen.
    // Velocity is scaled by 1/127. Pitch Bend adjusts Frequency on its channel.
    // The range defaults to ±2 semitones. Select RPN 0 with CC101 and CC100,
    // then set its semitones with CC6 and cents with CC38.
    // Channel Fine Tuning (RPN 1) uses both Data Entry bytes, while Channel
    // Coarse Tuning (RPN 2) uses CC6. Both add to the bend in semitones.
    // CC96 and CC97 adjust the selected RPN by one step.
    // Pitch remains the MIDI key number.
    // CC1, CC7, CC10, and CC11 supply Modulation, ChannelVolume, Pan, and
    // Expression graph inputs. MIDI channels start at 0, 100, 64, and 127.
    // CC64 holds released keys while down. CC66 holds keys that were down
    // when the sostenuto pedal was pressed. CC123–127 release held notes on
    // their channel through the same pedal logic. CC120 immediately stops its
    // channel's voices and preserves pedal state and outstanding Note Ons.
    // CC121 lifts both pedals, centers bend and clears parameter selection.
    // All data bytes must be 7-bit and match Channel Mode ranges.
    [[nodiscard]] bool processMidi(std::span<const InputBuffer> monoInputs,
                                  std::span<const OutputBuffer> mixedOutputs,
                                  std::span<const MidiNoteEvent> events) noexcept;
    void reset() noexcept;

    // Update converter Index controls between process calls. Use nullopt to
    // restore broadcast or sum. Negative and unoccupied slots carry silence.
    // monoInput identifies a graph input binding, not the shared Mono channel.
    [[nodiscard]] bool setMonoInputVoiceIndex(std::size_t monoInput, std::optional<std::int32_t> index) noexcept;
    [[nodiscard]] bool setOutputVoiceIndex(std::size_t output, std::optional<std::int32_t> index) noexcept;

    // Supplies the same read-only DSP array to every voice between render calls.
    [[nodiscard]] bool setDspArrayInput(DspBlockId block, std::size_t input, std::span<const float> values) noexcept;
    [[nodiscard]] bool setDspMemArrayInput(DspBlockId block, std::size_t input, const green::MemoryArray& values) noexcept;
    [[nodiscard]] bool setDspMemRefInput(DspBlockId block, std::size_t input, const green::Memory& value) noexcept;
    [[nodiscard]] bool setDspEnginePointer(DspBlockId block, std::string_view name, const green::Memory& memory, bool writable = true);
    [[nodiscard]] bool setDspEnginePointerTable(DspBlockId block, std::string_view name, const DspEnginePointerTable& table);

    [[nodiscard]] std::size_t inputCount() const noexcept { return monoInputCount_; }
    [[nodiscard]] std::size_t outputCount() const noexcept { return mixedSamples_.size(); }
    [[nodiscard]] std::size_t voiceCapacity() const noexcept { return voices_.size(); }
    [[nodiscard]] std::size_t activeVoiceCount() const noexcept;
    [[nodiscard]] VoiceInfo voiceInfo(std::size_t slot) const;

private:
    struct Voice {
        explicit Voice(Runtime compiled);
        Runtime runtime;
        VoiceInfo info;
        std::optional<std::uint8_t> midiChannel;
        NoteId midiKeyOrder{};
        bool midiKeyReleased{};
        bool sostenutoLatched{};
        std::vector<float> inputs;
        std::vector<float> outputs;
    };

    struct MidiKeyState {
        NoteId noteOns{};
        NoteId noteOffs{};
    };

    struct MidiChannelControls {
        MidiChannelControls() noexcept;
        void updateFrequencyRatio() noexcept;

        double frequencyRatio = 1.0;
        std::array<std::uint8_t, 128> ccValues{};
        int pitchBendOffset{};
        std::uint8_t pitchBendSemitones = 2;
        std::uint8_t pitchBendCents{};
        std::uint8_t fineTuningMsb = 64;
        std::uint8_t fineTuningLsb{};
        std::uint8_t coarseTuningMsb = 64;
        bool rpnSelected{};
    };

    Voice& startVoice(NoteId id, int pitch, float velocity, std::optional<std::uint8_t> midiChannel) noexcept;
    void releaseVoice(Voice& voice) noexcept;
    void noteOffMidiVoice(Voice& voice, bool sustainDown) noexcept;
    void releaseDeferredMidiVoices(std::uint8_t channel, bool clearSostenuto) noexcept;
    void applyMidiControl(std::uint8_t channel, std::uint8_t controller, std::uint8_t value) noexcept;
    void apply(const PolyEvent& event) noexcept;
    void apply(const MidiNoteEvent& event) noexcept;
    [[nodiscard]] bool validEvents(std::span<const PolyEvent> events, std::size_t frames) const noexcept;
    [[nodiscard]] bool validEvents(std::span<const MidiNoteEvent> events, std::size_t frames) const noexcept;

    template <typename Event>
    [[nodiscard]] bool processEvents(std::span<const InputBuffer> monoInputs,
                                    std::span<const OutputBuffer> mixedOutputs,
                                    std::span<const Event> events) noexcept;

    PolyOptions options_;
    std::vector<PolyInput> bindings_;
    std::vector<Voice> voices_;
    std::vector<Runtime::VoicePacket> packets_;
    std::vector<float> mixedSamples_;
    std::vector<std::optional<std::int32_t>> outputVoiceIndices_;
    std::array<double, 128> frequencies_{}; // Indexed by pitch; 1 is Nyquist.
    std::array<MidiChannelControls, 16> midiControls_{};
    std::array<std::array<MidiKeyState, 128>, 16> midiKeys_{};
    std::size_t monoInputCount_{};
    NoteId lastNoteId_{};
    std::uint64_t renderFrame_{};
    detail::AssemblerSampleClock sampleClock_;
    bool hasReleaseEnd_{};
};

}
