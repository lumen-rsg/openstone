#pragma once

#include "stream/HostTransport.hpp"
#include "stream/Runtime.hpp"
#include "stream/Sequencer.hpp"

#include <atomic>
#include <memory>
#include <optional>
#include <variant>

namespace openstone::stream {

enum class AudioChannel : std::size_t { Left, Right };
struct HostAudioChannel { std::size_t index; };

enum class TransportField { Sample, Quarter, BarStart, Tempo, Numerator, Denominator, Playing };

struct TransportInput {
    TransportField field;
    SamplePrecision precision = SamplePrecision::Float32;
    [[nodiscard]] double read(const HostTransport& transport, std::size_t offset) const noexcept;
};

// The host owns the audio buffers and the processing thread. Prepare the graph
// before calling process. A disconnected input channel is an empty span.
class AudioProcessor {
public:
    using Input = std::array<std::span<const float>, 2>;
    using Output = std::array<std::span<float>, 2>;
    using HostInput = std::span<const std::span<const float>>;
    using HostOutput = std::span<const std::span<float>>;
    using InputBinding = std::variant<AudioChannel, HostAudioChannel, TransportInput, double>;
    using Engine = std::variant<MonoDoubleRuntime, MonoDouble4Runtime>;
    struct Probe {
        NodeId signal;
        std::size_t frames;
    };
    using ProbeIndex = std::optional<std::size_t>;

    AudioProcessor(Engine engine, std::vector<InputBinding> inputs, std::size_t blockSize,
                   std::span<const Probe> probes = {},
                   std::shared_ptr<const std::atomic<std::uint64_t>> resetRequests = {},
                   std::size_t monitorOutputs = 0, std::vector<std::shared_ptr<RouteMixTable>> routeTables = {},
                   std::shared_ptr<SequenceTransport> sequencer = {});
    ~AudioProcessor();

    [[nodiscard]] bool needsInput() const noexcept { return needsInput_; }
    [[nodiscard]] std::size_t outputCount() const noexcept { return outputCount_; }
    // Safe to call from the control thread while process is running.
    void setParameter(std::size_t input, double value) noexcept;
    // process allocates nothing. Call it from one processing thread.
    [[nodiscard]] bool process(const Input& source, const Output& destination) noexcept;
    // Empty channels are inactive. The caller supplies total frames even when all outputs are inactive.
    // Transport describes the first frame of the host callback, including after a seek or loop.
    [[nodiscard]] bool processHost(HostInput source, HostOutput destination, std::size_t frames,
                                   const HostTransport& transport = {}) noexcept;
    // Call on the control thread. Channels come from one time window, oldest sample first.
    // An omitted probe returns silence. Early reads may return fewer than frames samples.
    [[nodiscard]] std::optional<std::vector<std::vector<double>>> readHistory(
        std::span<const ProbeIndex> probes, std::size_t frames) const;

    struct Frame {
        std::size_t sequence;
        std::vector<float> samples;
    };
    [[nodiscard]] std::optional<Frame> readFrame(std::size_t probe, std::size_t frames, std::size_t after) const;

private:
    template<class Runtime>
    struct Workspace;
    using Renderer = std::variant<std::unique_ptr<Workspace<MonoDoubleRuntime>>,
                                  std::unique_ptr<Workspace<MonoDouble4Runtime>>>;

    std::vector<InputBinding> bindings_;
    std::vector<std::shared_ptr<RouteMixTable>> routeTables_;
    std::shared_ptr<SequenceTransport> sequencer_;
    std::unique_ptr<std::atomic<double>[]> parameters_;
    const std::size_t outputCount_;
    const bool needsInput_;
    const std::size_t blockSize_;
    std::shared_ptr<const std::atomic<std::uint64_t>> resetRequests_;
    std::uint64_t resetSeen_{};
    struct History;
    std::unique_ptr<History> history_;
    Renderer renderer_;
};

}
