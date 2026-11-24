#pragma once

#include "green/Memory.hpp"
#include "stream/DspCode.hpp"
#include "stream/AssemblerSampleClock.hpp"
#include "stream/DspProgramData.hpp"
#include "stream/RealtimeValue.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace openstone::stream {

class PolyRuntime;
class PolyRenderer;

enum class NodeId : std::size_t {};
enum class DspBlockId : std::size_t {};
enum class DspInputWidth { Scalar, Packed };
// One physical SIMD register, in low-to-high DWORD order; no numerical conversion.
using DspXmmRegister = std::array<std::uint32_t, 4>;
enum class FeedbackPolicy { ExplicitDelay, ImplicitDelay };
enum class SamplePrecision { Float32, Float64 };
enum class SampleOperation {
    Add, Subtract, Multiply, Divide, Absolute, Inverse, Reciprocal, SquareRoot, Min, Max, Sign, Convert,
    Round, Floor, Ceiling, Truncate, Power, Logarithm, Ln, Log2, Log10,
    FrequencyToHz, FrequencyToPitch, HzToFrequency, HzToPitch, PitchToFrequency, PitchToHz,
};

struct DspEnginePointerBinding {
    green::Memory memory;
    bool writable = true;
};

// Each region retains its backing bytes. Region zero is the __const referent.
struct DspEnginePointerCell {
    std::size_t region{};
    std::size_t offset{};
    std::optional<std::size_t> targetRegion; // nullopt is an explicit null pointer.
    std::size_t targetOffset{};
    unsigned bytes = 8; // Native table stride/field width, independent of the host ABI.
};

struct DspEnginePointerTable {
    std::vector<DspEnginePointerBinding> regions;
    std::vector<DspEnginePointerCell> cells;
};

struct DspCodeBlock {
    DspBlockId id;
    std::vector<NodeId> outputs; // In the DSP program's output declaration order.
};

using MonoDouble = double;

// Four independent channels at the same sample position, in pack order.
using Mono4 = std::array<float, 4>;
using MonoDouble4 = std::array<MonoDouble, 4>;

class FloatArrayTable {
public:
    static constexpr std::size_t size = 2048;
    // One control thread publishes and one render thread reads.
    void publish(std::span<const float> values) noexcept;
    // The returned view is used by the render thread until the next read.
    [[nodiscard]] std::span<const float, size> read() noexcept;

private:
    RealtimeValue<std::array<float, size>> slots_;
};

// An audio patch latches these routes once per host buffer. All outputs and
// Poly voices then read the same table until the next buffer.
class RouteMixTable {
public:
    using Routes = std::vector<std::vector<std::size_t>>;
    void publish(Routes routes) noexcept;
    void latch() noexcept;
    [[nodiscard]] std::span<const std::size_t> sources(std::size_t output) const noexcept;

private:
    RealtimeValue<Routes> slots_;
    const Routes* active_{};
};

namespace detail {
template<typename Sample>
struct SampleScalar {
    using Type = Sample;
};

template<typename Scalar>
struct SampleScalar<std::array<Scalar, 4>> {
    using Type = Scalar;
};

template<typename Sample>
class Runtime;
}

using Runtime = detail::Runtime<float>;
using MonoDoubleRuntime = detail::Runtime<MonoDouble>;
using Mono4Runtime = detail::Runtime<Mono4>;
using MonoDouble4Runtime = detail::Runtime<MonoDouble4>;

// A compiled scalar processor embedded in a stream schedule.
class SampleProcessor {
public:
    virtual ~SampleProcessor() = default;
    virtual void beginBlock(std::uint64_t) noexcept {}
    virtual void reset() noexcept {}
    [[nodiscard]] virtual std::string compiledDspCode() const { return {}; }
    [[nodiscard]] virtual double process(std::span<const double> inputs) noexcept = 0;
};

class Graph {
public:
    explicit Graph(DspBackend backend = defaultDspBackend(),
                   FeedbackPolicy feedback = FeedbackPolicy::ExplicitDelay);

    [[nodiscard]] NodeId addInput();
    [[nodiscard]] NodeId addGain(double multiplier);
    [[nodiscard]] NodeId addMultiply();
    [[nodiscard]] NodeId addSum();
    [[nodiscard]] NodeId addInputMix(SampleOperation operation, SamplePrecision precision);
    [[nodiscard]] NodeId addRouteMix(std::shared_ptr<const RouteMixTable> routes, std::size_t output,
                                     SampleOperation operation, SamplePrecision precision);
    [[nodiscard]] NodeId addDelay(SamplePrecision precision = SamplePrecision::Float64);
    // length must be an input buffer. The Mono history is read before the current frame is written.
    [[nodiscard]] NodeId addVariableDelay(NodeId length, SamplePrecision precision);
    // In a packed runtime, scalar signals are read from their first lane.
    [[nodiscard]] NodeId addPack(SamplePrecision precision);
    [[nodiscard]] NodeId addUnpack(std::size_t lane);
    [[nodiscard]] NodeId addArithmetic(SampleOperation operation, SamplePrecision precision);
    [[nodiscard]] NodeId addFloatArrayRead(std::shared_ptr<FloatArrayTable> table);
    using PrepareProcessor = std::function<std::unique_ptr<SampleProcessor>()>;
    [[nodiscard]] NodeId addProcessor(std::size_t inputCount, PrepareProcessor prepare);
    [[nodiscard]] DspCodeBlock addDspCode(const DspProgram& program);
    // Bind referenced __const names before compiling an active block. Empty memory explicitly binds null.
    [[nodiscard]] bool bindDspEnginePointer(DspBlockId block, std::string_view name,
                                          const green::Memory& memory, bool writable = true);

    [[nodiscard]] bool bindDspEnginePointerTable(DspBlockId block, std::string_view name,
                                               const DspEnginePointerTable& table);

    // Connect one source per declared input. Unconnected DSP inputs read zero.
    void connectDspInput(NodeId source, DspBlockId block, std::size_t input,
                         DspInputWidth width = DspInputWidth::Packed);

    // Connect arithmetic operands in order. Unconnected trailing operands read zero.
    // Pack accepts four ordered sources. Gain, both delays and Unpack accept one.
    // A route mix can select any of its connected sources by index.
    void connect(NodeId source, NodeId target);

    // Output taps define the active graph. Only reachable DSP blocks compile to native code.
    // ImplicitDelay schedules previous-sample reads on feedback paths that have no explicit delay.
    [[nodiscard]] Runtime compile(std::span<const NodeId> outputs) const;
    [[nodiscard]] MonoDoubleRuntime compileMonoDouble(std::span<const NodeId> outputs) const;
    [[nodiscard]] Mono4Runtime compileMono4(std::span<const NodeId> outputs) const;
    [[nodiscard]] MonoDouble4Runtime compileMonoDouble4(std::span<const NodeId> outputs) const;

private:
    template<typename Sample>
    friend class detail::Runtime;

    enum class Kind { Input, Gain, Multiply, Sum, Delay, Pack, Unpack, Arithmetic, Processor, Dsp,
                      DspEarlyOutput, DspOutput, DspLateOutput, FloatArrayRead, InputMix, RouteMix };

    static constexpr bool isDspOutput(Kind kind) noexcept
    {
        return kind == Kind::DspEarlyOutput || kind == Kind::DspOutput || kind == Kind::DspLateOutput;
    }

    struct LaneStep {
        NodeId node;
        unsigned lanes;
    };
    static constexpr auto allLanes = (1U << std::tuple_size_v<Mono4>) - 1;

    [[nodiscard]] std::vector<LaneStep> feedbackSchedule(const std::vector<bool>& active, bool packed, bool early,
                                                         std::unique_ptr<Graph>& repaired) const;
    void delayInput(NodeId target, std::size_t port, NodeId source);

    struct DspInput {
        NodeId source;
        DspInputWidth width;
    };

    struct DspBlock {
        std::shared_ptr<const detail::DspProgramData> program;
        NodeId owner;
        std::vector<std::optional<DspInput>> inputs;
        std::vector<NodeId> outputs;
        std::vector<std::optional<DspEnginePointerTable>> enginePointers;
    };

    template<typename Scalar>
    struct Node {
        Kind kind;
        Scalar gain = Scalar{1};
        std::vector<NodeId> sources{};
        std::size_t dspBlock{};
        std::size_t dspPort{};
        SampleOperation operation{};
        SamplePrecision precision = SamplePrecision::Float64;
        std::size_t lane = 0;
        std::optional<NodeId> delayLength;
        std::shared_ptr<FloatArrayTable> table;
        std::shared_ptr<const RouteMixTable> routes;
        std::size_t routeOutput{};
    };

    struct ProcessorSpec {
        std::size_t inputs;
        PrepareProcessor prepare;
    };
    std::vector<ProcessorSpec> processors_;

    template<typename Sample>
    [[nodiscard]] detail::Runtime<Sample> compileFor(std::span<const NodeId> outputs) const;
    [[nodiscard]] NodeId addNode(Node<double> node);
    DspBackend backend_;
    FeedbackPolicy feedback_;
    std::vector<Node<double>> nodes_;
    std::vector<DspBlock> dspBlocks_;
};

namespace detail {

// A compiled graph and its per-render state, owned by one processing thread.
template<typename Sample>
class Runtime {
public:
    using InputBuffer = std::span<const Sample>;
    using OutputBuffer = std::span<Sample>;

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) noexcept = default;
    Runtime& operator=(Runtime&&) noexcept = default;

    // A passive tap copies the tail of the block from a signal already scheduled for rendering.
    // Probe buffers must not overlap the render buffers or other probes.
    struct Probe {
        NodeId signal;
        OutputBuffer tail;
    };
    [[nodiscard]] bool renders(NodeId signal) const noexcept;
    // Diagnostic listing of the active LLVM kernels, in graph block order.
    [[nodiscard]] std::string compiledDspCode() const;
    // First arithmetic fault in this block since reset. Buffer validation and rendering remain independent.
    [[nodiscard]] DspFault dspFault(DspBlockId block) const noexcept;

    // Inputs follow addInput order. Outputs follow the order passed to compile.
    // Render buffers share a frame count. An input and output may share a buffer in place.
    // Invalid buffers or probes return false before changing state or output.
    [[nodiscard]] bool process(std::span<const InputBuffer> inputs, std::span<const OutputBuffer> outputs,
                               std::span<const Probe> probes = {}) noexcept;
    void reset() noexcept;

    // Copies source samples into a Mem or FloatArray port between render calls.
    // Excess samples are discarded and unused capacity is zeroed. Reset keeps the data.
    [[nodiscard]] bool setDspArrayInput(DspBlockId block, std::size_t input, std::span<const float> values) noexcept;
    // Snapshots the wave count and highest index of a MemArray input between render calls.
    [[nodiscard]] bool setDspMemArrayInput(DspBlockId block, std::size_t input, const green::MemoryArray& values) noexcept;
    [[nodiscard]] bool setDspMemRefInput(DspBlockId block, std::size_t input, const green::Memory& value) noexcept;
    // Copies a caller/import frame image between render calls. Reset restores it.
    // Fixed engine fields are refreshed by the runtime; raw pointers are never accepted.
    [[nodiscard]] bool setDspNativeFrame(DspBlockId block, std::span<const std::uint8_t> bytes);
    // Copies an incoming XMM trace between render calls. One packet is a constant;
    // multiple packets start at the current frame and read zero after their end.
    // Empty clears the binding. Reset retains the trace and rewinds it to frame 0.
    // Unbound registers read zero, independently for each block and stage call.
    [[nodiscard]] bool setDspIncomingXmm(DspBlockId block, unsigned reg,
                                       std::span<const DspXmmRegister> frames);
    // Rebind caller-owned engine data between buffers; reset leaves external data intact.
    [[nodiscard]] bool setDspEnginePointer(DspBlockId block, std::string_view name,
                                         const green::Memory& memory, bool writable = true);
    [[nodiscard]] bool setDspEnginePointerTable(DspBlockId block, std::string_view name,
                                              const DspEnginePointerTable& table);
    [[nodiscard]] std::size_t inputCount() const noexcept { return inputs_.size(); }
    [[nodiscard]] std::size_t outputCount() const noexcept { return outputs_.size(); }

private:
    friend class openstone::stream::Graph;
    friend class openstone::stream::PolyRuntime;
    friend class openstone::stream::PolyRenderer;
    Runtime() = default;

    // Scalar graph state remains private to each voice. Only DSP stages share an SSE packet.
    struct VoicePacket {
        std::array<Runtime*, 4> voices{};
        std::vector<DspLanes> dspSlots;
        std::vector<DspValue> stack;
        std::uint64_t frame{};
        std::uint32_t samplePosition{};
        std::uint64_t nativeFramePosition{};
        unsigned gates{};
        std::array<std::size_t, 4> voiceSlots{};
    };
    [[nodiscard]] static VoicePacket makeVoicePacket(std::array<Runtime*, 4> voices) requires (std::same_as<Sample, float> || std::same_as<Sample, MonoDouble>);
    static void processVoicePacket(VoicePacket& packet, unsigned active,
        const std::array<std::span<const Sample>, 4>& inputs,
        const std::array<std::span<Sample>, 4>& outputs) noexcept requires (std::same_as<Sample, float> || std::same_as<Sample, MonoDouble>);
    static void processVoiceDsp(VoicePacket& packet, std::size_t block, unsigned stage,
        unsigned active, unsigned writeMask, bool loadInputs = true) noexcept requires (std::same_as<Sample, float> || std::same_as<Sample, MonoDouble>);

    void resetVoice() noexcept { resetState(true); }
    void resetState(bool preserveNativeFrame) noexcept;

    using Scalar = typename SampleScalar<Sample>::Type;
    using Node = Graph::Node<Scalar>;

    struct DelayState {
        NodeId node;
        Sample previous{};
    };

    struct VariableDelayState {
        NodeId node;
        NodeId length;
        std::vector<Scalar> history;
        std::size_t next = 0;
    };

    struct IncomingXmm {
        std::vector<DspXmmRegister> frames;
        std::uint64_t startFrame{};
    };

    struct DspState {
        Graph::DspBlock block;
        std::shared_ptr<const DspNativeProgram> native;
        std::vector<DspLanes> dspSlots;
        std::vector<DspLanes> arrays;
        detail::DspMemoryReferences references;
        std::vector<DspValue> stack;
        std::unique_ptr<std::array<IncomingXmm, 16>> incomingXmm;
    };

    static void loadIncomingXmm(DspState& state, unsigned stage, std::uint64_t frame,
                                std::span<DspLanes> slots) noexcept;
    void loadDspInputs(DspState& state, unsigned stage) noexcept;
    void executeDspStage(DspState& state, unsigned stage) noexcept;
    void processDspStage(DspState& state, unsigned stage) noexcept;
    [[nodiscard]] Sample sample(NodeId node) const noexcept;
    [[nodiscard]] Sample inputSample(const Node& node, std::size_t input = 0) const noexcept;
    [[nodiscard]] Sample sumSources(const Node& node) const noexcept;
    [[nodiscard]] Sample mixInput(const Node& node) const noexcept;
    [[nodiscard]] Sample calculateNode(NodeId id) noexcept;

    std::vector<Node> nodes_;
    std::vector<NodeId> inputs_;
    std::vector<NodeId> outputs_;
    std::vector<NodeId> schedule_;
    std::vector<Graph::LaneStep> laneSchedule_;
    std::vector<DelayState> delays_;
    std::vector<VariableDelayState> variableDelays_;
    std::vector<Sample> samples_;
    std::vector<bool> rendered_;
    struct ProcessorState {
        std::unique_ptr<SampleProcessor> processor;
        std::vector<double> inputs;
    };
    std::vector<ProcessorState> processors_;
    std::vector<DspState> dspBlocks_;
    std::vector<std::size_t> activeDspBlocks_;
    std::vector<NodeId> earlySchedule_;
    std::vector<std::size_t> lateDspBlocks_;
    std::uint64_t frame_{};
    AssemblerSampleClock sampleClock_;
    bool initialized_{};
};

}

using InputBuffer = Runtime::InputBuffer;
using OutputBuffer = Runtime::OutputBuffer;
using MonoDoubleInputBuffer = MonoDoubleRuntime::InputBuffer;
using MonoDoubleOutputBuffer = MonoDoubleRuntime::OutputBuffer;
using Mono4InputBuffer = Mono4Runtime::InputBuffer;
using Mono4OutputBuffer = Mono4Runtime::OutputBuffer;
using MonoDouble4InputBuffer = MonoDouble4Runtime::InputBuffer;
using MonoDouble4OutputBuffer = MonoDouble4Runtime::OutputBuffer;

}
