#include "stream/AudioProcessor.hpp"

#include <algorithm>
#include <concepts>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace openstone::stream {
namespace {

static_assert(std::atomic<double>::is_always_lock_free);
static_assert(std::atomic<std::size_t>::is_always_lock_free);

template<class Sample>
Sample scalarSample(double value)
{
    if constexpr (std::same_as<Sample, double>) {
        return value;
    } else {
        return {value, value, value, value};
    }
}

std::optional<std::size_t> sourceChannel(const AudioProcessor::InputBinding& binding)
{
    if (const auto* stereo = std::get_if<AudioChannel>(&binding)) return static_cast<std::size_t>(*stereo);
    if (const auto* host = std::get_if<HostAudioChannel>(&binding)) return host->index;
    return std::nullopt;
}

double firstLane(double value) { return value; }
double firstLane(const MonoDouble4& value) { return value.front(); }

}

double TransportInput::read(const HostTransport& transport, std::size_t offset) const noexcept
{
    const double elapsed = transport.playing ? static_cast<double>(offset) : 0;
    double value = 0;
    switch (field) {
    case TransportField::Sample: value = transport.samplePosition + elapsed; break;
    case TransportField::Quarter:
        value = transport.quarterPosition;
        if (transport.sampleRate > 0) value += elapsed * transport.tempo / (60 * transport.sampleRate);
        break;
    case TransportField::BarStart: value = transport.barStart; break;
    case TransportField::Tempo: value = transport.tempo; break;
    case TransportField::Numerator: value = transport.numerator; break;
    case TransportField::Denominator: value = transport.denominator; break;
    case TransportField::Playing: value = transport.playing; break;
    }
    return precision == SamplePrecision::Float32 ? static_cast<double>(static_cast<float>(value)) : value;
}

struct AudioProcessor::History {
    struct Channel {
        std::size_t frames;
        std::vector<std::atomic<double>> samples;
    };

    History(std::span<const Probe> probes, std::size_t blockSize)
    {
        channels.reserve(probes.size());
        for (const auto& probe : probes) {
            // Keep enough space to write the next block while the reader takes a snapshot.
            channels.push_back({probe.frames, std::vector<std::atomic<double>>(probe.frames + 2 * blockSize)});
        }
    }

    void publish(const auto& buffers, std::size_t count) noexcept
    {
        const auto begin = published.load(std::memory_order_relaxed);
        const auto end = begin + count;
        writingThrough.store(end, std::memory_order_release);
        for (std::size_t i = 0; i < channels.size(); ++i) {
            auto& ring = channels[i].samples;
            auto index = begin % ring.size();
            for (const auto& sample : std::span{buffers[i]}.first(count)) {
                ring[index].store(firstLane(sample), std::memory_order_release);
                if (++index == ring.size()) {
                    index = 0;
                }
            }
        }
        published.store(end, std::memory_order_release);
    }

    std::optional<std::vector<std::vector<double>>> read(std::span<const ProbeIndex> probes, std::size_t frames) const
    {
        std::vector<std::vector<double>> result(probes.size());
        for (int attempt = 0; attempt < 3; ++attempt) {
            const auto end = published.load(std::memory_order_acquire);
            const auto count = std::min(frames, end);
            const auto begin = end - count;
            std::size_t retainedUntil = std::numeric_limits<std::size_t>::max();
            for (std::size_t i = 0; i < probes.size(); ++i) {
                result[i].resize(count);
                if (!probes[i]) {
                    continue;
                }
                const auto& ring = channels[*probes[i]].samples;
                retainedUntil = std::min(retainedUntil, begin + ring.size());
                auto index = begin % ring.size();
                for (auto& sample : result[i]) {
                    sample = ring[index].load(std::memory_order_acquire);
                    if (++index == ring.size()) {
                        index = 0;
                    }
                }
            }
            // Reading any overwritten sample synchronizes with its writer, including the
            // reservation published before that write. Retry if it crossed this window.
            if (writingThrough.load(std::memory_order_acquire) <= retainedUntil) {
                return result;
            }
        }
        return std::nullopt;
    }

    std::optional<Frame> readFrame(std::size_t probe, std::size_t frames, std::size_t after) const
    {
        const auto& ring = channels[probe].samples;
        for (int attempt = 0; attempt < 3; ++attempt) {
            const auto sequence = published.load(std::memory_order_acquire) / frames;
            if (sequence <= after) return std::nullopt;
            const auto begin = (sequence - 1) * frames;
            Frame frame{sequence, std::vector<float>(frames)};
            for (std::size_t i = 0; i < frames; ++i) {
                frame.samples[i] = static_cast<float>(ring[(begin + i) % ring.size()].load(std::memory_order_acquire));
            }
            if (writingThrough.load(std::memory_order_acquire) <= begin + ring.size()) return frame;
        }
        return std::nullopt;
    }

    std::vector<Channel> channels;
    std::atomic<std::size_t> writingThrough = 0;
    std::atomic<std::size_t> published = 0;
};

template<class Runtime>
struct AudioProcessor::Workspace {
    using Sample = typename Runtime::InputBuffer::value_type;

    Workspace(Runtime runtime, std::size_t inputCount, std::size_t blockSize, std::span<const Probe> specifications)
        : runtime(std::move(runtime)), inputBuffers(inputCount, std::vector<Sample>(blockSize)),
          inputs(inputCount), outputs(this->runtime.outputCount(), std::vector<Sample>(blockSize)), taps(outputs.size())
    {
        if (this->runtime.inputCount() != inputCount || outputs.empty()) {
            throw std::invalid_argument("Audio processing needs matching input bindings and at least one output channel");
        }
        probeBuffers.reserve(specifications.size());
        probes.reserve(specifications.size());
        for (const auto& spec : specifications) {
            if (!this->runtime.renders(spec.signal) || spec.frames == 0) {
                throw std::invalid_argument("Audio history requires an active signal and a positive frame count");
            }
            probeBuffers.emplace_back(blockSize);
            probes.push_back({spec.signal, {}});
        }
    }

    bool render(const AudioProcessor& owner, HostInput source, HostOutput destination, std::size_t offset,
                std::size_t count, const HostTransport& transport) noexcept
    {
        for (std::size_t i = 0; i < inputBuffers.size(); ++i) {
            auto buffer = std::span{inputBuffers[i]}.first(count);
            if (const auto channel = sourceChannel(owner.bindings_[i])) {
                const auto samples = *channel < source.size() ? source[*channel] : std::span<const float>{};
                if (!samples.empty()) {
                    std::ranges::transform(samples.subspan(offset, count), buffer.begin(), scalarSample<Sample>);
                } else {
                    std::ranges::fill(buffer, Sample{});
                }
            } else if (const auto* input = std::get_if<TransportInput>(&owner.bindings_[i])) {
                for (std::size_t frame = 0; frame < count; ++frame) {
                    buffer[frame] = scalarSample<Sample>(input->read(transport, offset + frame));
                }
            } else {
                const auto value = owner.parameters_[i].load(std::memory_order_relaxed);
                std::ranges::fill(buffer, scalarSample<Sample>(value));
            }
            inputs[i] = buffer;
        }
        for (std::size_t i = 0; i < outputs.size(); ++i) taps[i] = std::span{outputs[i]}.first(count);
        for (std::size_t i = 0; i < probes.size(); ++i) {
            probes[i].tail = std::span{probeBuffers[i]}.first(count);
        }
        if (!runtime.process(inputs, taps, probes)) {
            return false;
        }
        if (owner.history_) {
            owner.history_->publish(probeBuffers, count);
        }
        for (std::size_t channel = 0; channel < destination.size(); ++channel) {
            if (destination[channel].empty()) continue;
            std::ranges::transform(taps[channel], destination[channel].subspan(offset, count).begin(),
                                   [](const auto& sample) { return static_cast<float>(firstLane(sample)); });
        }
        return true;
    }

    Runtime runtime;
    std::vector<std::vector<Sample>> inputBuffers;
    std::vector<typename Runtime::InputBuffer> inputs;
    std::vector<std::vector<Sample>> outputs;
    std::vector<typename Runtime::OutputBuffer> taps;
    std::vector<std::vector<Sample>> probeBuffers;
    std::vector<typename Runtime::Probe> probes;
};

AudioProcessor::AudioProcessor(Engine engine, std::vector<InputBinding> inputs, std::size_t blockSize,
                               std::span<const Probe> probes,
                               std::shared_ptr<const std::atomic<std::uint64_t>> resetRequests,
                               std::size_t monitorOutputs, std::vector<std::shared_ptr<RouteMixTable>> routeTables,
                               std::shared_ptr<SequenceTransport> sequencer)
    : bindings_(std::move(inputs)), routeTables_(std::move(routeTables)),
      sequencer_(std::move(sequencer)), parameters_(std::make_unique<std::atomic<double>[]>(bindings_.size())),
      outputCount_(std::visit([monitorOutputs](const auto& runtime) {
          return runtime.outputCount() - std::min(runtime.outputCount(), monitorOutputs);
      }, engine)),
      needsInput_(std::ranges::any_of(bindings_, [](const auto& input) { return sourceChannel(input).has_value(); })),
      blockSize_(blockSize), resetRequests_(std::move(resetRequests)),
      resetSeen_(resetRequests_ ? resetRequests_->load(std::memory_order_acquire) : 0)
{
    if (!outputCount_) throw std::invalid_argument("No host audio outputs");
    if (!blockSize_) {
        throw std::invalid_argument("Audio block size must be positive");
    }
    for (std::size_t i = 0; i < bindings_.size(); ++i) {
        if (const auto value = std::get_if<double>(&bindings_[i])) {
            setParameter(i, *value);
        }
    }
    renderer_ = std::visit([&](auto& runtime) -> Renderer {
        using Runtime = std::remove_cvref_t<decltype(runtime)>;
        return std::make_unique<Workspace<Runtime>>(std::move(runtime), bindings_.size(), blockSize_, probes);
    }, engine);
    if (!probes.empty()) {
        history_ = std::make_unique<History>(probes, blockSize_);
    }
}

AudioProcessor::~AudioProcessor() = default;

void AudioProcessor::setParameter(std::size_t input, double value) noexcept
{
    if (input < bindings_.size() && std::holds_alternative<double>(bindings_[input])) {
        parameters_[input].store(value, std::memory_order_relaxed);
    }
}

bool AudioProcessor::process(const Input& source, const Output& destination) noexcept
{
    const auto frames = destination.front().size();
    return destination.back().size() == frames && processHost(source, destination, frames);
}

bool AudioProcessor::processHost(HostInput source, HostOutput destination, std::size_t frames,
                                 const HostTransport& transport) noexcept
{
    const auto invalidFrames = [frames](auto channel) { return !channel.empty() && channel.size() != frames; };
    if (destination.size() != outputCount_ || std::ranges::any_of(source, invalidFrames) ||
        std::ranges::any_of(destination, invalidFrames)) {
        return false;
    }
    if (frames) {
        for (const auto& routes : routeTables_) routes->latch();
        if (sequencer_) sequencer_->beginBuffer();
    }
    return std::visit([&](auto& renderer) {
        if (const auto requested = resetRequests_ ? resetRequests_->load(std::memory_order_acquire) : resetSeen_;
            frames && requested != resetSeen_) {
            renderer->runtime.reset();
            resetSeen_ = requested;
        }
        for (std::size_t offset = 0; offset < frames;) {
            const auto count = std::min(blockSize_, frames - offset);
            if (!renderer->render(*this, source, destination, offset, count, transport)) {
                return false;
            }
            offset += count;
            if (sequencer_) sequencer_->advance(count);
        }
        return true;
    }, renderer_);
}

std::optional<std::vector<std::vector<double>>> AudioProcessor::readHistory(
    std::span<const ProbeIndex> probes, std::size_t frames) const
{
    if (!history_ || std::ranges::any_of(probes, [&](const auto& probe) {
            return probe && (*probe >= history_->channels.size() || frames > history_->channels[*probe].frames);
        })) {
        return std::nullopt;
    }
    return history_->read(probes, frames);
}

std::optional<AudioProcessor::Frame> AudioProcessor::readFrame(std::size_t probe, std::size_t frames,
                                                               std::size_t after) const
{
    if (!history_ || probe >= history_->channels.size() || frames == 0 ||
        frames > history_->channels[probe].frames / 2) return std::nullopt;
    return history_->readFrame(probe, frames, after);
}

}
