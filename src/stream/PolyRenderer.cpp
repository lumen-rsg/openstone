#include "stream/PolyRenderer.hpp"
#include "math/SignalConversions.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace openstone::stream {
namespace {

const auto midiFrequencies = [] {
    std::array<double, 128> result{};
    for (std::size_t key = 0; key < result.size(); ++key) {
        result[key] = math::convertPitch(math::PitchConversion::PitchToHz, static_cast<double>(key), 1.0);
    }
    return result;
}();

}

VoiceSnapshot::VoiceSnapshot(std::shared_ptr<const VoiceBank> bank)
    : bank_(std::move(bank)), voices_(bank_->capacity()), scratch_(voices_.size())
{
}

std::span<const VoiceSlot> VoiceSnapshot::read(std::uint64_t frame) noexcept
{
    if (frame != frame_) {
        bank_->read(voices_, scratch_);
        frame_ = frame;
    }
    return voices_;
}

PolyRenderer::PolyRenderer(const Graph& graph, std::span<const NodeId> outputs,
    std::vector<PolyBinding> bindings, std::shared_ptr<VoiceSnapshot> snapshot, bool managed,
    PolyMix mix, std::size_t sampleRateInput, std::optional<std::size_t> control)
    : snapshot_(std::move(snapshot)), bindings_(std::move(bindings)),
      mix_(mix), sampleRateInput_(sampleRateInput), control_(control), hasReleaseEnd_(outputs.size() == 2)
{
    const auto count = snapshot_ ? snapshot_->capacity() : managed ? 0 : 1;
    // A Poly patch without a Voices source can still be used as a one-voice DSP bench.
    if (!snapshot_ && !managed) voices_ = {&preview_, 1};
    instances_.reserve(count);
    for (std::size_t slot = 0; slot < count; ++slot) instances_.emplace_back(graph.compileMonoDouble(outputs));
    packets_.reserve((count + 3) / 4);
    for (std::size_t first = 0; first < count; first += 4) {
        std::array<MonoDoubleRuntime*, 4> lanes{};
        for (std::size_t lane = 0; lane < 4 && first + lane < count; ++lane) lanes[lane] = &instances_[first + lane].runtime;
        packets_.push_back(MonoDoubleRuntime::makeVoicePacket(lanes));
    }
}

void PolyRenderer::beginBlock(std::uint64_t frame) noexcept
{
    sampleClock_.finishBuffer();
    if (snapshot_) voices_ = snapshot_->read(frame);
}

void PolyRenderer::reset() noexcept
{
    if (snapshot_) snapshot_->reset();
    for (auto& instance : instances_) {
        instance.runtime.reset();
        instance.generation = 0;
        instance.ended = false;
        instance.active = false;
    }
    for (auto& packet : packets_) { packet.frame = 0; packet.samplePosition = 0; }
    sampleClock_ = {};
    frame_ = 0;
    held_ = 0;
}

double PolyRenderer::inputSample(PolyBinding binding, std::span<const double> external,
                                 const VoiceSlot& voice, std::size_t slot) const noexcept
{
    using enum PolyValue;
    switch (binding.value) {
    case External: return external[binding.input];
    case Mono:
        return !binding.index || external[*binding.index] == static_cast<double>(slot) ? external[binding.input] : 0;
    case Frequency:
        return static_cast<float>(2.0 * midiFrequencies[voice.note.key] / external[sampleRateInput_]);
    case Pitch: return voice.note.key;
    case Velocity: return voice.note.velocity / 127.0F;
    case Gate: return voice.note.noteOn;
    case Trigger: return static_cast<float>(voice.triggerCount);
    case Channel: return voice.note.channel;
    case ChannelController: return external[binding.input + voice.note.channel];
    case Slot: return static_cast<double>(slot);
    case Note: return static_cast<float>(voice.id);
    case Tag: {
        const double index = external[binding.input];
        return index >= 0 && index < voice.note.tags.size() ? voice.note.tags[static_cast<std::size_t>(index)] : 0;
    }
    }
    return 0;
}

double PolyRenderer::process(std::span<const double> external) noexcept
{
    const double control = control_ ? external[*control_] : 0;
    const auto hop = mix_ == PolyMix::Hop
        ? std::bit_floor(static_cast<unsigned>(std::clamp(control, 1.0, 4096.0)))
        : 1U;
    const auto collect = frame_++ % hop == 0;
    for (std::size_t slot = 0; slot < voices_.size(); ++slot) {
        const auto& voice = voices_[slot];
        auto& instance = instances_[slot];
        instance.active = false;
        if (!voice.id || (!voice.note.noteOn && !hasReleaseEnd_)) continue;
        if (instance.generation != voice.generation) {
            instance.runtime.resetVoice();
            instance.generation = voice.generation;
            instance.ended = false;
        }
        if (instance.ended && !voice.note.noteOn) continue;
        instance.active = true;
        for (std::size_t input = 0; input < bindings_.size(); ++input) {
            instance.inputs[input] = inputSample(bindings_[input], external, voice, slot);
        }
    }
    for (std::size_t pack = 0; pack < packets_.size(); ++pack) {
        unsigned active = 0;
        std::array<std::span<const double>, 4> inputs;
        std::array<std::span<double>, 4> outputs;
        for (std::size_t lane = 0; lane < 4 && pack * 4 + lane < instances_.size(); ++lane) {
            auto& instance = instances_[pack * 4 + lane];
            inputs[lane] = instance.inputs;
            outputs[lane] = std::span{instance.outputs}.first(hasReleaseEnd_ ? 2 : 1);
            if (instance.active) active |= 1U << lane;
        }
        packets_[pack].samplePosition = sampleClock_.position;
        packets_[pack].nativeFramePosition = frame_ - 1;
        packets_[pack].gates = 0;
        for (std::size_t lane = 0; lane < 4 && pack * 4 + lane < voices_.size(); ++lane) {
            const auto slot = pack * 4 + lane;
            packets_[pack].voiceSlots[lane] = slot;
            if (instances_[slot].active && voices_[slot].note.noteOn) packets_[pack].gates |= 1U << lane;
        }
        if (active) MonoDoubleRuntime::processVoicePacket(packets_[pack], active, inputs, outputs);
    }
    sampleClock_.advance();
    double result = 0;
    bool first = true;
    for (std::size_t slot = 0; slot < voices_.size(); ++slot) {
        auto& instance = instances_[slot];
        if (!instance.active) continue;
        const auto& voice = voices_[slot];
        const auto value = instance.outputs[0];
        if (!voice.note.noteOn && instance.outputs[1] != 0) instance.ended = true;
        if (!collect) continue;
        if (mix_ == PolyMix::SelectGroup && static_cast<double>(slot / 4) != std::nearbyint(control)) continue;
        if (mix_ == PolyMix::Sum && control_ && static_cast<double>(slot) != control) continue;
        if (mix_ == PolyMix::Minimum) result = first ? value : std::fmin(result, value);
        else if (mix_ == PolyMix::Maximum) result = first ? value : std::fmax(result, value);
        else result += value;
        first = false;
    }
    if (collect) held_ = result;
    return held_;
}

std::string PolyRenderer::compiledDspCode() const
{
    return instances_.empty() ? std::string{} : instances_.front().runtime.compiledDspCode();
}

bool PolyRenderer::setDspMemArrayInput(DspBlockId block, std::size_t input, const green::MemoryArray& values) noexcept
{
    return std::ranges::all_of(instances_, [&](auto& voice) {
        return voice.runtime.setDspMemArrayInput(block, input, values);
    });
}

bool PolyRenderer::setDspArrayInput(DspBlockId block, std::size_t input, std::span<const float> values) noexcept
{
    return std::ranges::all_of(instances_, [&](auto& instance) {
        return instance.runtime.setDspArrayInput(block, input, values);
    });
}

bool PolyRenderer::setDspMemRefInput(DspBlockId block, std::size_t input, const green::Memory& value) noexcept
{
    return std::ranges::all_of(instances_, [&](auto& instance) {
        return instance.runtime.setDspMemRefInput(block, input, value);
    });
}

bool PolyRenderer::setDspEnginePointerTable(DspBlockId block, std::string_view name, const DspEnginePointerTable& table)
{
    return std::ranges::all_of(instances_, [&](auto& instance) {
        return instance.runtime.setDspEnginePointerTable(block, name, table);
    });
}

bool PolyRenderer::setDspEnginePointer(DspBlockId block, std::string_view name, const green::Memory& memory, bool writable)
{
    return std::ranges::all_of(instances_, [&](auto& instance) {
        return instance.runtime.setDspEnginePointer(block, name, memory, writable);
    });
}

}
