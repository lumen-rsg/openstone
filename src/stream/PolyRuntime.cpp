#include "stream/PolyRuntime.hpp"
#include "stream/DspFloatingEnvironment.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace openstone::stream {
namespace {

constexpr std::uint8_t modulationController = 1;
constexpr std::uint8_t dataEntryMsb = 6;
constexpr std::uint8_t channelVolumeController = 7;
constexpr std::uint8_t panController = 10;
constexpr std::uint8_t expressionController = 11;
constexpr std::uint8_t lsbControllerOffset = 32;
constexpr std::uint8_t dataEntryLsb = dataEntryMsb + lsbControllerOffset;
constexpr std::uint8_t sustainController = 64;
constexpr std::uint8_t portamentoController = 65;
constexpr std::uint8_t sostenutoController = 66;
constexpr std::uint8_t softPedalController = 67;
constexpr std::uint8_t dataIncrement = 96;
constexpr std::uint8_t dataDecrement = 97;
constexpr std::uint8_t nrpnSelectLsb = 98;
constexpr std::uint8_t nrpnSelectMsb = 99;
constexpr std::uint8_t rpnSelectLsb = 100;
constexpr std::uint8_t rpnSelectMsb = 101;
constexpr std::uint8_t allSoundOffController = 120;
constexpr std::uint8_t resetAllControllers = 121;
constexpr std::uint8_t localControl = 122;
constexpr std::uint8_t allNotesOffController = 123;
constexpr std::uint8_t omniModeOff = 124;
constexpr std::uint8_t omniModeOn = 125;
constexpr std::uint8_t monoModeOn = 126;
constexpr std::uint8_t polyModeOn = 127;
constexpr std::uint8_t pitchBendStatus = 0xe0;
constexpr std::uint8_t pitchBendSensitivityRpn = 0;
constexpr std::uint8_t channelFineTuningRpn = 1;
constexpr std::uint8_t channelCoarseTuningRpn = 2;
constexpr int midi7BitCenter = 64;
constexpr int midi14BitCenter = 8192;
constexpr int midi14BitMax = 16383;
constexpr std::uint8_t midiDataMax = 127;
constexpr std::uint8_t initialVolume = 100;

bool isMidiNoteOn(const MidiNoteEvent& event) noexcept
{
    return (event.status & 0xf0) == 0x90 && event.velocity != 0;
}

bool validMidiControl(const MidiNoteEvent& event) noexcept
{
    if ((event.status & 0xf0) != 0xb0) {
        return false;
    }
    if (event.pitch < allSoundOffController) {
        return true;
    }
    if (event.pitch == localControl) {
        return event.velocity == 0 || event.velocity == midiDataMax;
    }
    if (event.pitch == monoModeOn) {
        return event.velocity <= 16;
    }
    return event.velocity == 0;
}

int midi14Bit(std::uint8_t msb, std::uint8_t lsb) noexcept
{
    return (static_cast<int>(msb) << 7) | lsb;
}

float normalizedCC(std::uint8_t value) noexcept
{
    return value / static_cast<float>(midiDataMax);
}

float panPosition(std::uint8_t value) noexcept
{
    const int offset = value - midi7BitCenter;
    const int steps = offset < 0 ? midi7BitCenter : midiDataMax - midi7BitCenter;
    return static_cast<float>(offset) / steps;
}

}

PolyRuntime::Voice::Voice(Runtime compiled)
    : runtime(std::move(compiled)), inputs(runtime.inputCount()), outputs(runtime.outputCount())
{
}

PolyRuntime::MidiChannelControls::MidiChannelControls() noexcept
{
    ccValues[channelVolumeController] = initialVolume;
    ccValues[panController] = midi7BitCenter;
    ccValues[expressionController] = midiDataMax;
    ccValues[nrpnSelectLsb] = ccValues[nrpnSelectMsb] = midiDataMax;
    ccValues[rpnSelectLsb] = ccValues[rpnSelectMsb] = midiDataMax;
}

void PolyRuntime::MidiChannelControls::updateFrequencyRatio() noexcept
{
    // Map both wheel endpoints to the configured bend range.
    const int bendSteps = pitchBendOffset < 0 ? midi14BitCenter : midi14BitCenter - 1;
    const double bendFraction = static_cast<double>(pitchBendOffset) / bendSteps;
    const double bendRange = pitchBendSemitones + pitchBendCents / 100.0;
    const int fineTuning = midi14Bit(fineTuningMsb, fineTuningLsb);
    const double fineSemitones = (fineTuning - midi14BitCenter) / static_cast<double>(midi14BitCenter);
    const int coarseSemitones = coarseTuningMsb - midi7BitCenter;
    frequencyRatio = std::exp2((coarseSemitones + fineSemitones + bendFraction * bendRange) / 12.0);
}

PolyRuntime::PolyRuntime(const Graph& graph, std::span<const PolyInput> bindings,
                         std::span<const NodeId> outputs, PolyOptions options,
                         std::optional<NodeId> releaseEnd)
    : options_(options), bindings_(bindings.begin(), bindings.end()),
      mixedSamples_(outputs.size()), outputVoiceIndices_(outputs.size()),
      hasReleaseEnd_(releaseEnd.has_value())
{
    if (options.maxVoices == 0 || !std::isfinite(options.sampleRate) || options.sampleRate <= 0
        || outputs.empty()) {
        throw std::invalid_argument("Poly requires voices, a positive finite sample rate, and output taps");
    }
    const double maxBend = midiDataMax + midiDataMax / 100.0;
    const double maxFineTuning = (midi14BitCenter - 1.0) / midi14BitCenter;
    const int maxCoarseTuning = midiDataMax - midi7BitCenter;
    const double maxFrequencyRatio = std::exp2((maxCoarseTuning + maxFineTuning + maxBend) / 12.0);
    for (std::size_t pitch = 0; pitch < frequencies_.size(); ++pitch) {
        const double frequency = (880.0 * std::exp2((static_cast<double>(pitch) - 69) / 12.0)) / options.sampleRate;
        if (frequency * maxFrequencyRatio > std::numeric_limits<float>::max()) {
            throw std::invalid_argument("Poly sample rate is too small to represent normalized frequencies");
        }
        frequencies_[pitch] = frequency;
    }
    for (const auto binding : bindings_) {
        switch (binding.kind) {
        case PolyInputKind::Mono:
            // Dense channels cannot exceed the number of bindings. Check before +1.
            if (binding.channel >= bindings_.size()) {
                throw std::invalid_argument("Poly Mono input channels must be dense from zero");
            }
            monoInputCount_ = std::max(monoInputCount_, binding.channel + 1);
            break;
        case PolyInputKind::Gate:
        case PolyInputKind::NoteOn:
        case PolyInputKind::Pitch:
        case PolyInputKind::Velocity:
        case PolyInputKind::Frequency:
        case PolyInputKind::Stage:
        case PolyInputKind::Modulation:
        case PolyInputKind::ChannelVolume:
        case PolyInputKind::Pan:
        case PolyInputKind::Expression:
            break;
        case PolyInputKind::MidiCC:
            if (binding.controller > midiDataMax) {
                throw std::invalid_argument("Poly MIDI CC requires a controller number from 0 to 127");
            }
            break;
        case PolyInputKind::MidiCC14:
            if (binding.controller >= lsbControllerOffset || binding.controller == dataEntryMsb) {
                throw std::invalid_argument("Poly 14-bit MIDI CC requires an MSB controller number from 0 to 31, except Data Entry");
            }
            break;
        default:
            throw std::invalid_argument("Unknown Poly input binding");
        }
    }
    for (std::size_t channel = 0; channel < monoInputCount_; ++channel) {
        if (std::ranges::none_of(bindings_, [channel](const auto binding) {
                return binding.kind == PolyInputKind::Mono && binding.channel == channel;
            })) {
            throw std::invalid_argument("Poly Mono input channels must be dense from zero");
        }
    }
    std::vector<NodeId> taps(outputs.begin(), outputs.end());
    if (releaseEnd) {
        taps.push_back(*releaseEnd);
    }
    auto first = graph.compile(taps);
    if (first.inputCount() != bindings.size()) {
        throw std::invalid_argument("Poly requires one binding per graph input");
    }
    voices_.reserve(options.maxVoices);
    voices_.emplace_back(std::move(first));
    for (std::size_t slot = 1; slot < options.maxVoices; ++slot) {
        voices_.emplace_back(graph.compile(taps));
    }
    packets_.reserve((voices_.size() + 3) / 4);
    for (std::size_t first = 0; first < voices_.size(); first += 4) {
        std::array<Runtime*, 4> lanes{};
        for (std::size_t lane = 0; lane < 4 && first + lane < voices_.size(); ++lane) lanes[lane] = &voices_[first + lane].runtime;
        packets_.push_back(Runtime::makeVoicePacket(lanes));
    }
}

bool PolyRuntime::validEvents(std::span<const PolyEvent> events, std::size_t frames) const noexcept
{
    auto lastId = lastNoteId_;
    std::size_t lastFrame = 0;
    for (const auto& event : events) {
        if (event.frame >= frames || event.frame < lastFrame) {
            return false;
        }
        lastFrame = event.frame;
        switch (event.kind) {
        case PolyEventKind::NoteOn:
            if (event.id <= lastId || event.pitch < 0 || event.pitch > 127
                || !std::isfinite(event.velocity) || event.velocity < 0 || event.velocity > 1) {
                return false;
            }
            lastId = event.id;
            break;
        case PolyEventKind::NoteOff:
        case PolyEventKind::EndVoice:
            if (event.id == 0) {
                return false;
            }
            break;
        case PolyEventKind::AllSoundOff:
            break;
        default:
            return false;
        }
    }
    return true;
}

bool PolyRuntime::validEvents(std::span<const MidiNoteEvent> events, std::size_t frames) const noexcept
{
    auto lastId = lastNoteId_;
    std::size_t lastFrame = 0;
    for (const auto& event : events) {
        const bool isNoteMessage = event.status >= 0x80 && event.status < 0xa0;
        const bool isPitchBend = (event.status & 0xf0) == pitchBendStatus;
        if (event.frame >= frames || event.frame < lastFrame
            || (!isNoteMessage && !isPitchBend && !validMidiControl(event))
            || event.pitch > midiDataMax || event.velocity > midiDataMax) {
            return false;
        }
        lastFrame = event.frame;
        if (isMidiNoteOn(event)) {
            if (lastId == std::numeric_limits<NoteId>::max()) {
                return false;
            }
            ++lastId;
        }
    }
    return true;
}

PolyRuntime::Voice& PolyRuntime::startVoice(NoteId id, int pitch, float velocity,
                                            std::optional<std::uint8_t> midiChannel) noexcept
{
    auto slot = std::ranges::find_if(voices_, [](const auto& voice) {
        return voice.info.stage == VoiceStage::Off;
    });
    if (slot == voices_.end()) {
        // IDs increase in note-on order: oldest active note is stolen first.
        slot = std::ranges::min_element(voices_, {}, [](const auto& voice) { return voice.info.id; });
    }
    slot->runtime.resetVoice();
    slot->info = {id, VoiceStage::On, pitch, velocity};
    slot->midiChannel = midiChannel;
    slot->midiKeyOrder = 0;
    slot->midiKeyReleased = false;
    slot->sostenutoLatched = false;
    lastNoteId_ = id;
    return *slot;
}

void PolyRuntime::releaseVoice(Voice& voice) noexcept
{
    voice.midiKeyReleased = false;
    voice.sostenutoLatched = false;
    if (options_.holdAfterNoteOff) {
        voice.info.stage = VoiceStage::Release;
    } else {
        voice.info = {};
    }
}

void PolyRuntime::apply(const PolyEvent& event) noexcept
{
    if (event.kind == PolyEventKind::AllSoundOff) {
        for (auto& voice : voices_) {
            voice.info = {};
            voice.midiKeyReleased = false;
            voice.sostenutoLatched = false;
        }
        return;
    }
    if (event.kind == PolyEventKind::NoteOn) {
        startVoice(event.id, event.pitch, event.velocity, std::nullopt);
        return;
    }
    const auto slot = std::ranges::find_if(voices_, [&](const auto& voice) { return voice.info.id == event.id; });
    if (slot == voices_.end()) {
        return; // A late note-off must not kill the new owner of a stolen slot.
    }
    if (event.kind == PolyEventKind::NoteOff) {
        releaseVoice(*slot);
    } else if (event.kind == PolyEventKind::EndVoice && slot->info.stage == VoiceStage::Release) {
        slot->info = {};
    }
}

void PolyRuntime::noteOffMidiVoice(Voice& voice, bool sustainDown) noexcept
{
    if (sustainDown || voice.sostenutoLatched) {
        voice.midiKeyReleased = true;
    } else {
        releaseVoice(voice);
    }
}

void PolyRuntime::releaseDeferredMidiVoices(std::uint8_t channel, bool clearSostenuto) noexcept
{
    const bool sustainDown = midiControls_[channel].ccValues[sustainController] >= midi7BitCenter;
    for (auto& voice : voices_) {
        if (voice.midiChannel != channel) {
            continue;
        }
        if (clearSostenuto) {
            voice.sostenutoLatched = false;
        }
        if (voice.midiKeyReleased && !voice.sostenutoLatched && !sustainDown) {
            releaseVoice(voice);
        }
    }
}

void PolyRuntime::applyMidiControl(std::uint8_t channel, std::uint8_t controller, std::uint8_t value) noexcept
{
    auto& controls = midiControls_[channel];
    auto& cc = controls.ccValues;
    const auto previous = std::exchange(cc[controller], value);
    if (controller < lsbControllerOffset && controller != dataEntryMsb) {
        cc[controller + lsbControllerOffset] = 0;
    }
    switch (controller) {
    case rpnSelectMsb:
    case rpnSelectLsb:
        controls.rpnSelected = true;
        return;
    case nrpnSelectMsb:
    case nrpnSelectLsb:
        controls.rpnSelected = false;
        return;
    case dataEntryMsb:
    case dataEntryLsb: {
        if (!controls.rpnSelected || cc[rpnSelectMsb] != 0) {
            return;
        }
        const bool isMsb = controller == dataEntryMsb;
        switch (cc[rpnSelectLsb]) {
        case pitchBendSensitivityRpn:
            (isMsb ? controls.pitchBendSemitones : controls.pitchBendCents) = value;
            break;
        case channelFineTuningRpn:
            (isMsb ? controls.fineTuningMsb : controls.fineTuningLsb) = value;
            break;
        case channelCoarseTuningRpn:
            if (!isMsb) {
                return;
            }
            controls.coarseTuningMsb = value;
            break;
        default:
            return;
        }
        controls.updateFrequencyRatio();
        return;
    }
    case dataIncrement:
    case dataDecrement: {
        if (!controls.rpnSelected || cc[rpnSelectMsb] != 0) {
            return;
        }
        const int step = controller == dataIncrement ? 1 : -1;
        switch (cc[rpnSelectLsb]) {
        case pitchBendSensitivityRpn: {
            const int cents = std::clamp(controls.pitchBendSemitones * 100 + controls.pitchBendCents + step,
                                         0, midiDataMax * 101);
            controls.pitchBendSemitones = static_cast<std::uint8_t>(std::min(cents / 100, int{midiDataMax}));
            controls.pitchBendCents = static_cast<std::uint8_t>(cents - controls.pitchBendSemitones * 100);
            break;
        }
        case channelFineTuningRpn: {
            const int tuned = std::clamp(midi14Bit(controls.fineTuningMsb, controls.fineTuningLsb) + step,
                                         0, midi14BitMax);
            controls.fineTuningMsb = static_cast<std::uint8_t>(tuned >> 7);
            controls.fineTuningLsb = static_cast<std::uint8_t>(tuned & midiDataMax);
            break;
        }
        case channelCoarseTuningRpn:
            controls.coarseTuningMsb = static_cast<std::uint8_t>(
                std::clamp(controls.coarseTuningMsb + step, 0, int{midiDataMax}));
            break;
        default:
            return;
        }
        controls.updateFrequencyRatio();
        return;
    }
    case resetAllControllers:
        cc[modulationController] = cc[modulationController + lsbControllerOffset] = 0;
        cc[expressionController] = midiDataMax;
        cc[expressionController + lsbControllerOffset] = 0;
        cc[sustainController] = cc[portamentoController] = cc[sostenutoController] = cc[softPedalController] = 0;
        cc[nrpnSelectLsb] = cc[nrpnSelectMsb] = cc[rpnSelectLsb] = cc[rpnSelectMsb] = midiDataMax;
        controls.rpnSelected = false;
        controls.pitchBendOffset = 0;
        controls.updateFrequencyRatio();
        releaseDeferredMidiVoices(channel, true);
        return;
    case sustainController:
        if (previous >= midi7BitCenter && value < midi7BitCenter) {
            releaseDeferredMidiVoices(channel, false);
        }
        return;
    case sostenutoController:
        if (previous >= midi7BitCenter && value < midi7BitCenter) {
            releaseDeferredMidiVoices(channel, true);
        } else if (previous < midi7BitCenter && value >= midi7BitCenter) {
            for (auto& voice : voices_) {
                const auto stage = voice.info.stage;
                if (voice.midiChannel == channel && !voice.midiKeyReleased
                    && (stage == VoiceStage::On || stage == VoiceStage::Execute)) {
                    voice.sostenutoLatched = true;
                }
            }
        }
        return;
    case allSoundOffController:
        for (auto& voice : voices_) {
            if (voice.midiChannel == channel) {
                voice.info = {};
                voice.midiKeyReleased = false;
                voice.sostenutoLatched = false;
            }
        }
        // Subsequent Note Offs still match these Note Ons in order.
        return;
    case allNotesOffController:
    case omniModeOff:
    case omniModeOn:
    case monoModeOn:
    case polyModeOn:
        for (auto& voice : voices_) {
            const auto stage = voice.info.stage;
            if ((stage == VoiceStage::On || stage == VoiceStage::Execute)
                && voice.midiChannel == channel && !voice.midiKeyReleased) {
                noteOffMidiVoice(voice, cc[sustainController] >= midi7BitCenter);
            }
        }
        // Consume every outstanding Note On, including stolen voices.
        for (auto& key : midiKeys_[channel]) {
            key.noteOffs = key.noteOns;
        }
        return;
    }
}

void PolyRuntime::apply(const MidiNoteEvent& event) noexcept
{
    const auto channel = static_cast<std::uint8_t>(event.status & 0x0f);
    const auto status = event.status & 0xf0;
    if (status == pitchBendStatus) {
        const int value = midi14Bit(event.velocity, event.pitch);
        auto& controls = midiControls_[channel];
        controls.pitchBendOffset = value - midi14BitCenter;
        controls.updateFrequencyRatio();
        return;
    }
    if (status == 0xb0) {
        applyMidiControl(channel, event.pitch, event.velocity);
        return;
    }
    auto& key = midiKeys_[channel][event.pitch];
    if (isMidiNoteOn(event)) {
        startVoice(lastNoteId_ + 1, event.pitch, event.velocity / 127.0f, channel).midiKeyOrder = ++key.noteOns;
        return;
    }
    if (key.noteOffs == key.noteOns) {
        return;
    }
    // Advance even if the matched voice no longer exists, so a stolen
    // note's eventual Note Off cannot cut a newer instance of the same key.
    const auto keyOrder = ++key.noteOffs;
    for (auto& voice : voices_) {
        const auto stage = voice.info.stage;
        if ((stage == VoiceStage::On || stage == VoiceStage::Execute)
            && voice.midiChannel == channel && voice.info.pitch == event.pitch && voice.midiKeyOrder == keyOrder) {
            noteOffMidiVoice(voice, midiControls_[channel].ccValues[sustainController] >= midi7BitCenter);
            return;
        }
    }
}

template <typename Event>
bool PolyRuntime::processEvents(std::span<const InputBuffer> monoInputs,
                                std::span<const OutputBuffer> mixedOutputs,
                                std::span<const Event> events) noexcept
{
    detail::DspFloatingEnvironment floatingEnvironment;
    if (monoInputs.size() != inputCount() || mixedOutputs.size() != outputCount() || mixedOutputs.empty()) {
        return false;
    }
    const auto frames = mixedOutputs.front().size();
    const auto wrongLength = [frames](const auto buffer) { return buffer.size() != frames; };
    if (std::ranges::any_of(monoInputs, wrongLength) || std::ranges::any_of(mixedOutputs, wrongLength)
        || !validEvents(events, frames)) {
        return false;
    }

    std::size_t eventIndex = 0;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        while (eventIndex < events.size() && events[eventIndex].frame == frame) {
            apply(events[eventIndex++]);
        }
        std::ranges::fill(mixedSamples_, 0.0f);
        for (std::size_t slot = 0; slot < voices_.size(); ++slot) {
            auto& voice = voices_[slot];
            if (voice.info.stage == VoiceStage::Off) {
                continue;
            }
            const auto* midi = voice.midiChannel ? &midiControls_[*voice.midiChannel] : nullptr;
            for (std::size_t input = 0; input < bindings_.size(); ++input) {
                const auto& binding = bindings_[input];
                auto& sample = voice.inputs[input];
                switch (binding.kind) {
                case PolyInputKind::Mono:
                    sample = matchesVoiceIndex(binding.voiceIndex, slot) ? monoInputs[binding.channel][frame] : 0.0f;
                    break;
                case PolyInputKind::Gate: sample = voice.info.stage != VoiceStage::Release ? 1.0f : 0.0f; break;
                case PolyInputKind::NoteOn: sample = voice.info.stage == VoiceStage::On ? 1.0f : 0.0f; break;
                case PolyInputKind::Pitch: sample = static_cast<float>(voice.info.pitch); break;
                case PolyInputKind::Velocity: sample = voice.info.velocity; break;
                case PolyInputKind::Frequency: {
                    const double frequency = frequencies_[static_cast<std::size_t>(voice.info.pitch)];
                    const double ratio = midi ? midi->frequencyRatio : 1.0;
                    sample = static_cast<float>(frequency * ratio);
                    break;
                }
                case PolyInputKind::Stage: sample = static_cast<float>(voice.info.stage); break;
                case PolyInputKind::Modulation:
                    sample = midi ? normalizedCC(midi->ccValues[modulationController]) : 0.0f;
                    break;
                case PolyInputKind::ChannelVolume:
                    sample = midi ? normalizedCC(midi->ccValues[channelVolumeController]) : 1.0f;
                    break;
                case PolyInputKind::Pan:
                    sample = midi ? panPosition(midi->ccValues[panController]) : 0.0f;
                    break;
                case PolyInputKind::Expression:
                    sample = midi ? normalizedCC(midi->ccValues[expressionController]) : 1.0f;
                    break;
                case PolyInputKind::MidiCC:
                    sample = midi ? static_cast<float>(midi->ccValues[binding.controller]) : 0.0f;
                    break;
                case PolyInputKind::MidiCC14:
                    sample = midi ? static_cast<float>(midi14Bit(midi->ccValues[binding.controller],
                                        midi->ccValues[binding.controller + lsbControllerOffset])) : 0.0f;
                    break;
                }
            }
        }
        for (std::size_t pack = 0; pack < packets_.size(); ++pack) {
            unsigned active = 0;
            std::array<std::span<const float>, 4> inputs;
            std::array<std::span<float>, 4> outputs;
            for (std::size_t lane = 0; lane < 4 && pack * 4 + lane < voices_.size(); ++lane) {
                auto& voice = voices_[pack * 4 + lane];
                inputs[lane] = voice.inputs;
                outputs[lane] = voice.outputs;
                if (voice.info.stage != VoiceStage::Off) active |= 1U << lane;
            }
            packets_[pack].samplePosition = sampleClock_.position;
            packets_[pack].nativeFramePosition = renderFrame_;
            packets_[pack].gates = 0;
            for (std::size_t lane = 0; lane < 4 && pack * 4 + lane < voices_.size(); ++lane) {
                const auto slot = pack * 4 + lane;
                packets_[pack].voiceSlots[lane] = slot;
                if (voices_[slot].info.stage != VoiceStage::Off && voices_[slot].info.stage != VoiceStage::Release)
                    packets_[pack].gates |= 1U << lane;
            }
            if (active) Runtime::processVoicePacket(packets_[pack], active, inputs, outputs);
        }
        for (std::size_t slot = 0; slot < voices_.size(); ++slot) {
            auto& voice = voices_[slot];
            if (voice.info.stage == VoiceStage::Off) continue;
            for (std::size_t channel = 0; channel < mixedSamples_.size(); ++channel) {
                const auto index = outputVoiceIndices_[channel];
                if (!index) {
                    mixedSamples_[channel] += voice.outputs[channel];
                } else if (matchesVoiceIndex(index, slot)) {
                    mixedSamples_[channel] = voice.outputs[channel];
                }
            }
            if (voice.info.stage == VoiceStage::Release && hasReleaseEnd_ && voice.outputs.back() != 0) {
                voice.info = {};
            } else if (voice.info.stage == VoiceStage::On) {
                voice.info.stage = VoiceStage::Execute;
            }
        }
        // Write only after all voices consume this frame's shared Mono inputs.
        for (std::size_t channel = 0; channel < mixedOutputs.size(); ++channel) {
            mixedOutputs[channel][frame] = mixedSamples_[channel];
        }
        ++renderFrame_;
        sampleClock_.advance();
    }
    if (frames != 0) sampleClock_.finishBuffer();
    return true;
}

bool PolyRuntime::process(std::span<const InputBuffer> monoInputs,
                          std::span<const OutputBuffer> mixedOutputs,
                          std::span<const PolyEvent> events) noexcept
{
    return processEvents(monoInputs, mixedOutputs, events);
}

bool PolyRuntime::processMidi(std::span<const InputBuffer> monoInputs,
                             std::span<const OutputBuffer> mixedOutputs,
                             std::span<const MidiNoteEvent> events) noexcept
{
    return processEvents(monoInputs, mixedOutputs, events);
}

bool PolyRuntime::setMonoInputVoiceIndex(std::size_t monoInput, std::optional<std::int32_t> index) noexcept
{
    if (monoInput >= bindings_.size() || bindings_[monoInput].kind != PolyInputKind::Mono) {
        return false;
    }
    bindings_[monoInput].voiceIndex = index;
    return true;
}

bool PolyRuntime::setOutputVoiceIndex(std::size_t output, std::optional<std::int32_t> index) noexcept
{
    if (output >= outputVoiceIndices_.size()) {
        return false;
    }
    outputVoiceIndices_[output] = index;
    return true;
}

bool PolyRuntime::setDspMemArrayInput(DspBlockId block, std::size_t input, const green::MemoryArray& values) noexcept
{
    return std::ranges::all_of(voices_, [&](auto& voice) {
        return voice.runtime.setDspMemArrayInput(block, input, values);
    });
}

bool PolyRuntime::setDspArrayInput(DspBlockId block, std::size_t input, std::span<const float> values) noexcept
{
    return std::ranges::all_of(voices_, [&](auto& voice) {
        return voice.runtime.setDspArrayInput(block, input, values);
    });
}

bool PolyRuntime::setDspMemRefInput(DspBlockId block, std::size_t input, const green::Memory& value) noexcept
{
    return std::ranges::all_of(voices_, [&](auto& voice) {
        return voice.runtime.setDspMemRefInput(block, input, value);
    });
}

bool PolyRuntime::setDspEnginePointerTable(DspBlockId block, std::string_view name, const DspEnginePointerTable& table)
{
    return std::ranges::all_of(voices_, [&](auto& voice) {
        return voice.runtime.setDspEnginePointerTable(block, name, table);
    });
}

bool PolyRuntime::setDspEnginePointer(DspBlockId block, std::string_view name, const green::Memory& memory, bool writable)
{
    return std::ranges::all_of(voices_, [&](auto& voice) {
        return voice.runtime.setDspEnginePointer(block, name, memory, writable);
    });
}

void PolyRuntime::reset() noexcept
{
    midiControls_.fill({});
    for (auto& channel : midiKeys_) {
        channel.fill({});
    }
    for (auto& voice : voices_) {
        voice.runtime.reset();
        voice.info = {};
        voice.midiKeyReleased = false;
        voice.sostenutoLatched = false;
    }
    for (auto& packet : packets_) { packet.frame = 0; packet.samplePosition = 0; }
    sampleClock_ = {};
    renderFrame_ = 0;
    lastNoteId_ = 0;
}

std::size_t PolyRuntime::activeVoiceCount() const noexcept
{
    return static_cast<std::size_t>(std::ranges::count_if(voices_, [](const auto& voice) {
        return voice.info.stage != VoiceStage::Off;
    }));
}

VoiceInfo PolyRuntime::voiceInfo(std::size_t slot) const
{
    return voices_.at(slot).info;
}

}
