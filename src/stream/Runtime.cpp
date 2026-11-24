#include "stream/Runtime.hpp"
#include "stream/DspFloatingEnvironment.hpp"

#include "stream/DspProgramData.hpp"
#include "math/Logarithm.hpp"
#include "math/SignalConversions.hpp"

#include <algorithm>
#include <cmath>
#include <concepts>
#include <stdexcept>
#include <utility>

namespace openstone::stream {
namespace {

// Prepare table topology off the render thread. Native address bits are never decoded.
detail::DspEngineMemory prepareEngineTable(const DspEnginePointerTable& table, bool targets)
{
    if (table.regions.size() > 65536 || table.cells.size() > 262144) throw std::invalid_argument("Engine pointer table is too large");
    detail::DspEngineMemory result;
    std::size_t cursor = detail::engineRootAddress;
    for (const auto& supplied : table.regions) {
        if (cursor > INT32_MAX - 16 || supplied.memory.size() > static_cast<std::size_t>(INT32_MAX) - cursor - 16)
            throw std::invalid_argument("Engine pointer table exceeds its bounded address space");
        detail::DspEngineRegion region{static_cast<std::uint32_t>(cursor), supplied.memory, supplied.writable, {}};
        if (targets) region.targets.resize(supplied.memory.size());
        result.regions.push_back(std::move(region));
        cursor = (cursor + supplied.memory.size() + 31) & ~std::size_t(15);
    }
    for (const auto& cell : table.cells) {
        if ((cell.bytes != 4 && cell.bytes != 8) || cell.region >= result.regions.size())
            throw std::invalid_argument("Engine pointer cell needs a region and a four/eight-byte width");
        const auto& source = result.regions[cell.region];
        if (cell.offset > source.memory.size() || cell.bytes > source.memory.size() - cell.offset)
            throw std::invalid_argument("Engine pointer cell crosses its source region");
        std::uint32_t target = 0;
        if (cell.targetRegion) {
            if (*cell.targetRegion >= result.regions.size() || cell.targetOffset > result.regions[*cell.targetRegion].memory.size())
                throw std::invalid_argument("Engine pointer target is outside its region");
            const auto& region = result.regions[*cell.targetRegion];
            if (region.memory.size()) target = region.start + static_cast<std::uint32_t>(cell.targetOffset);
        } else if (cell.targetOffset) throw std::invalid_argument("A null engine pointer has no offset");
        const auto low = source.memory.wordAt(cell.offset);
        const auto bits = cell.bytes == 4 ? std::uint64_t{low} : low | (std::uint64_t{source.memory.wordAt(cell.offset + 4)} << 32);
        result.cells.push_back({source.start + static_cast<std::uint32_t>(cell.offset), target,
            reinterpret_cast<std::uintptr_t>(source.memory.address()) + cell.offset, bits, cell.region, cell.offset, cell.bytes});
    }
    std::ranges::sort(result.cells, {}, &detail::DspEngineCell::start);
    result.physicalCells.resize(result.cells.size());
    for (std::size_t i = 0; i < result.cells.size(); ++i) result.physicalCells[i] = i;
    std::ranges::sort(result.physicalCells, [&](auto a, auto b) { return result.cells[a].address < result.cells[b].address; });
    for (std::size_t i = 1; i < result.physicalCells.size(); ++i) {
        const auto& previous = result.cells[result.physicalCells[i-1]];
        if (previous.address + previous.bytes > result.cells[result.physicalCells[i]].address)
            throw std::invalid_argument("Engine pointer cells overlap backing bytes");
    }
    return result;
}

constexpr std::size_t maxDelaySamples = 262144;

std::size_t floatArrayIndex(float value) noexcept
{
    const float rounded = std::nearbyint(value);
    if (!(rounded >= -0x1p31F && rounded < 0x1p31F)) return 0;
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(rounded)) & (FloatArrayTable::size - 1);
}

template<std::floating_point Scalar>
std::size_t delayLengthInSamples(Scalar length) noexcept
{
    if (!(length > Scalar{1})) return 1; // Also handles NaN from a headless input buffer.
    return static_cast<std::size_t>(std::min(length, static_cast<Scalar>(maxDelaySamples)));
}

bool hasTwoOperands(SampleOperation operation) noexcept
{
    using enum SampleOperation;
    return operation == Add || operation == Subtract || operation == Multiply || operation == Divide ||
           operation == Min || operation == Max || operation == Power || operation == Logarithm ||
           (operation >= FrequencyToHz && operation <= PitchToHz);
}

template<std::floating_point Scalar>
Scalar calculate(SampleOperation operation, Scalar left, Scalar right) noexcept
{
    switch (operation) {
    case SampleOperation::Add: return left + right;
    case SampleOperation::Subtract: return left - right;
    case SampleOperation::Multiply: return left * right;
    case SampleOperation::Divide: return left / right;
    case SampleOperation::Absolute: return std::abs(left);
    case SampleOperation::Round: return std::round(left);
    case SampleOperation::Floor: return std::floor(left);
    case SampleOperation::Ceiling: return std::ceil(left);
    case SampleOperation::Truncate: return std::trunc(left);
    case SampleOperation::Inverse: return -left;
    case SampleOperation::Reciprocal: return Scalar{1} / left;
    case SampleOperation::SquareRoot: return std::sqrt(left);
    case SampleOperation::Min: return std::fmin(left, right);
    case SampleOperation::Max: return std::fmax(left, right);
    case SampleOperation::Power: return std::pow(left, right);
    case SampleOperation::Logarithm: return math::logarithm(left, right);
    case SampleOperation::Ln: return std::log(left);
    case SampleOperation::Log2: return std::log2(left);
    case SampleOperation::Log10: return std::log10(left);
    case SampleOperation::Sign: return left < Scalar{} ? Scalar{-1} : Scalar{1};
    case SampleOperation::Convert: return left;
    case SampleOperation::FrequencyToHz: return math::convertPitch(math::PitchConversion::FrequencyToHz, left, right);
    case SampleOperation::FrequencyToPitch: return math::convertPitch(math::PitchConversion::FrequencyToPitch, left, right);
    case SampleOperation::HzToFrequency: return math::convertPitch(math::PitchConversion::HzToFrequency, left, right);
    case SampleOperation::HzToPitch: return math::convertPitch(math::PitchConversion::HzToPitch, left, right);
    case SampleOperation::PitchToFrequency: return math::convertPitch(math::PitchConversion::PitchToFrequency, left, right);
    case SampleOperation::PitchToHz: return math::convertPitch(math::PitchConversion::PitchToHz, left, right);
    }
    return {};
}

template<typename Sample>
Sample calculateSamples(SampleOperation operation, SamplePrecision precision, Sample left, const Sample& right) noexcept
{
    const auto lane = [=]<std::floating_point Scalar>(Scalar a, Scalar b) -> Scalar {
        if constexpr (std::same_as<Scalar, double>) {
            if (precision == SamplePrecision::Float32) {
                return calculate(operation, static_cast<float>(a), static_cast<float>(b));
            }
        }
        return calculate(operation, a, b);
    };
    if constexpr (std::floating_point<Sample>) {
        return lane(left, right);
    } else {
        for (std::size_t i = 0; i < left.size(); ++i) {
            left[i] = lane(left[i], right[i]);
        }
        return left;
    }
}

std::size_t index(NodeId node) noexcept
{
    return static_cast<std::size_t>(node);
}

template<std::floating_point Scalar>
Scalar addSamples(Scalar left, Scalar right) noexcept
{
    return left + right;
}

template<std::floating_point Scalar>
std::array<Scalar, 4> addSamples(std::array<Scalar, 4> left, const std::array<Scalar, 4>& right) noexcept
{
    for (std::size_t lane = 0; lane < left.size(); ++lane) {
        left[lane] += right[lane];
    }
    return left;
}

template<std::floating_point Scalar>
Scalar multiplySamples(Scalar left, Scalar right) noexcept
{
    return left * right;
}

template<std::floating_point Scalar>
std::array<Scalar, 4> multiplySamples(std::array<Scalar, 4> left, const std::array<Scalar, 4>& right) noexcept
{
    for (std::size_t lane = 0; lane < left.size(); ++lane) {
        left[lane] *= right[lane];
    }
    return left;
}

template<std::floating_point Scalar>
std::array<Scalar, 4> multiplySamples(std::array<Scalar, 4> sample, Scalar gain) noexcept
{
    return multiplySamples(sample, std::array{gain, gain, gain, gain});
}

}

DspBackend defaultDspBackend() noexcept
{
    return OPENSTONE_HAS_LLVM_ORC ? DspBackend::LlvmOrc : DspBackend::Interpreter;
}

bool dspBackendAvailable(DspBackend backend) noexcept
{
    return backend == DspBackend::Interpreter || (backend == DspBackend::LlvmOrc && OPENSTONE_HAS_LLVM_ORC);
}

#if !OPENSTONE_HAS_LLVM_ORC
std::shared_ptr<const detail::DspNativeProgram> detail::compileDspOrc(const DspProgramData&)
{
    throw std::logic_error("This build does not include LLVM ORC");
}
#endif

std::shared_ptr<const detail::DspNativeProgram> detail::DspProgramData::nativeProgram() const
{
    std::call_once(compileNative_, [this] { native_ = compileDspOrc(*this); });
    return native_;
}

Graph::Graph(DspBackend backend, FeedbackPolicy feedback) : backend_(backend), feedback_(feedback)
{
    if (!dspBackendAvailable(backend)) {
        throw std::invalid_argument("The selected DSP backend is unavailable in this build");
    }
}

void FloatArrayTable::publish(std::span<const float> values) noexcept
{
    auto& destination = slots_.writable();
    const auto count = std::min(values.size(), destination.size());
    std::ranges::copy(values.first(count), destination.begin());
    std::ranges::fill(std::span{destination}.subspan(count), 0.0F);
    slots_.publish();
}

std::span<const float, FloatArrayTable::size> FloatArrayTable::read() noexcept
{
    return slots_.read();
}

NodeId Graph::addInput()
{
    return addNode({Kind::Input});
}

NodeId Graph::addGain(double multiplier)
{
    return addNode({Kind::Gain, multiplier});
}

NodeId Graph::addMultiply()
{
    return addNode({Kind::Multiply});
}

NodeId Graph::addSum()
{
    return addNode({Kind::Sum});
}

NodeId Graph::addInputMix(SampleOperation operation, SamplePrecision precision)
{
    return addNode({.kind = Kind::InputMix, .operation = operation, .precision = precision});
}

void RouteMixTable::publish(Routes routes) noexcept
{
    slots_.writable() = std::move(routes);
    slots_.publish();
}

void RouteMixTable::latch() noexcept
{
    active_ = &slots_.read();
}

std::span<const std::size_t> RouteMixTable::sources(std::size_t output) const noexcept
{
    if (!active_ || output >= active_->size()) return {};
    return (*active_)[output];
}

NodeId Graph::addRouteMix(std::shared_ptr<const RouteMixTable> routes, std::size_t output,
                         SampleOperation operation, SamplePrecision precision)
{
    if (!routes) throw std::invalid_argument("Choose a route mix table");
    return addNode({.kind = Kind::RouteMix, .operation = operation, .precision = precision,
                    .routes = std::move(routes), .routeOutput = output});
}

NodeId Graph::addDelay(SamplePrecision precision)
{
    return addNode({.kind = Kind::Delay, .precision = precision});
}

NodeId Graph::addVariableDelay(NodeId length, SamplePrecision precision)
{
    if (nodes_.at(index(length)).kind != Kind::Input) {
        throw std::invalid_argument("Variable delay length needs an input buffer");
    }
    return addNode({.kind = Kind::Delay, .precision = precision, .delayLength = length});
}

NodeId Graph::addPack(SamplePrecision precision)
{
    return addNode({.kind = Kind::Pack, .precision = precision});
}

NodeId Graph::addUnpack(std::size_t lane)
{
    if (lane >= Mono4{}.size()) {
        throw std::invalid_argument("Choose a packed stream lane from 0 to 3");
    }
    return addNode({.kind = Kind::Unpack, .lane = lane});
}

NodeId Graph::addArithmetic(SampleOperation operation, SamplePrecision precision)
{
    return addNode({.kind = Kind::Arithmetic, .operation = operation, .precision = precision});
}

NodeId Graph::addFloatArrayRead(std::shared_ptr<FloatArrayTable> table)
{
    if (!table) throw std::invalid_argument("Choose a Float Array Read table");
    return addNode({.kind = Kind::FloatArrayRead, .table = std::move(table)});
}

NodeId Graph::addProcessor(std::size_t inputCount, PrepareProcessor prepare)
{
    if (!prepare) throw std::invalid_argument("Choose a sample processor factory");
    const auto index = processors_.size();
    processors_.push_back({inputCount, std::move(prepare)});
    return addNode({.kind = Kind::Processor, .dspBlock = index});
}

bool Graph::bindDspEnginePointer(DspBlockId block, std::string_view name, const green::Memory& memory, bool writable)
{
    return bindDspEnginePointerTable(block, name, {{DspEnginePointerBinding{memory, writable}}, {}});
}

bool Graph::bindDspEnginePointerTable(DspBlockId block, std::string_view name, const DspEnginePointerTable& table)
{
    const auto id = static_cast<std::size_t>(block);
    if (id >= dspBlocks_.size()) return false;
    auto& state = dspBlocks_[id];
    const auto& names = state.program->enginePointers;
    const auto found = std::ranges::find(names, name);
    if (found == names.end()) return false;
    try { static_cast<void>(prepareEngineTable(table, false)); }
    catch (const std::invalid_argument&) { return false; }
    state.enginePointers[found - names.begin()] = table;
    return true;
}

DspCodeBlock Graph::addDspCode(const DspProgram& program)
{
    if (!program.program_) {
        throw std::invalid_argument("Choose a compiled DSP program");
    }
    const auto blockId = dspBlocks_.size();
    const auto owner = NodeId{nodes_.size()};
    DspBlock block{program.program_, owner,
        std::vector<std::optional<DspInput>>(program.inputs().size()), {},
        std::vector<std::optional<DspEnginePointerTable>>(program.enginePointers().size())};
    block.outputs.reserve(program.outputs().size());
    for (std::size_t i = 0; i < program.outputs().size(); ++i) {
        block.outputs.push_back(NodeId{nodes_.size() + 1 + i});
    }
    DspCodeBlock ports{DspBlockId{blockId}, block.outputs};

    nodes_.reserve(nodes_.size() + 1 + ports.outputs.size());
    dspBlocks_.push_back(std::move(block));
    static_cast<void>(addNode({.kind = Kind::Dsp, .dspBlock = blockId}));
    for (std::size_t port = 0; port < ports.outputs.size(); ++port) {
        constexpr std::array stages{Kind::DspEarlyOutput, Kind::DspOutput, Kind::DspLateOutput};
        const auto kind = stages[program.program_->outputStages[port] - 1];
        static_cast<void>(addNode({.kind = kind, .dspBlock = blockId, .dspPort = port}));
    }
    return ports;
}

void Graph::connectDspInput(NodeId source, DspBlockId block, std::size_t input, DspInputWidth width)
{
    const auto& sourceNode = nodes_.at(index(source));
    auto& destination = dspBlocks_.at(static_cast<std::size_t>(block));
    const auto& inlet = destination.program->inputs.at(input);
    if (inlet.type == DspPortType::MemArrayRef) {
        throw std::invalid_argument("Use setDspMemArrayInput to supply a MemArray");
    }
    if (inlet.type == DspPortType::MemRef) {
        throw std::invalid_argument("Use setDspMemRefInput to supply a Mem reference");
    }
    if (inlet.arraySize != 0) {
        throw std::invalid_argument("Use setDspArrayInput to supply array data");
    }
    auto& port = destination.inputs.at(input);
    if (sourceNode.kind == Kind::Dsp || port) {
        throw std::invalid_argument("Connect an output to an unused DSP input");
    }
    port = DspInput{source, width};
}

NodeId Graph::addNode(Node<double> node)
{
    const auto id = NodeId{nodes_.size()};
    nodes_.push_back(std::move(node));
    return id;
}

void Graph::connect(NodeId source, NodeId target)
{
    if (nodes_.at(index(source)).kind == Kind::Dsp) {
        throw std::invalid_argument("Choose a declared DSP output");
    }
    auto& node = nodes_.at(index(target));
    if (node.kind == Kind::Input) {
        throw std::invalid_argument("A stream input node reads from the render buffer");
    }
    if (node.kind == Kind::Dsp || isDspOutput(node.kind)) {
        throw std::invalid_argument("Use connectDspInput to connect a declared DSP input");
    }
    const bool binary = node.kind == Kind::Multiply || (node.kind == Kind::Arithmetic && hasTwoOperands(node.operation));
    const std::size_t inputCount = node.kind == Kind::Processor ? processors_[node.dspBlock].inputs
                                 : node.kind == Kind::Pack ? Mono4{}.size() : binary ? 2 : 1;
    if (node.kind != Kind::Sum && node.kind != Kind::InputMix && node.kind != Kind::RouteMix && node.sources.size() >= inputCount) {
        throw std::invalid_argument("This stream node has no free inputs");
    }
    node.sources.push_back(source);
}

Runtime Graph::compile(std::span<const NodeId> outputs) const
{
    return compileFor<float>(outputs);
}

MonoDoubleRuntime Graph::compileMonoDouble(std::span<const NodeId> outputs) const
{
    return compileFor<MonoDouble>(outputs);
}

Mono4Runtime Graph::compileMono4(std::span<const NodeId> outputs) const
{
    return compileFor<Mono4>(outputs);
}

MonoDouble4Runtime Graph::compileMonoDouble4(std::span<const NodeId> outputs) const
{
    return compileFor<MonoDouble4>(outputs);
}

void Graph::delayInput(NodeId target, std::size_t port, NodeId source)
{
    const auto delay = addDelay();
    connect(source, delay);
    auto& node = nodes_[index(target)];
    if (node.kind == Kind::Dsp) dspBlocks_[node.dspBlock].inputs[port]->source = delay;
    else node.sources[port] = delay;
}

std::vector<Graph::LaneStep> Graph::feedbackSchedule(const std::vector<bool>& active, bool packed, bool early,
                                                     std::unique_ptr<Graph>& repaired) const
{
    constexpr std::size_t lanes = std::tuple_size_v<Mono4>;
    // Only split stateless nodes. DSP stages and processors still execute once per sample.
    const auto split = [&](Kind kind) {
        if (!packed) return false;
        switch (kind) {
        case Kind::Gain:
        case Kind::Multiply:
        case Kind::Sum:
        case Kind::InputMix:
        case Kind::RouteMix:
        case Kind::Pack:
        case Kind::Arithmetic:
            return true;
        default:
            return false;
        }
    };
    const auto scheduled = [&](std::size_t id) {
        if (!active[id]) return false;
        const auto kind = nodes_[id].kind;
        return kind != Kind::Input && kind != Kind::Delay && !isDspOutput(kind);
    };
    struct Dependency {
        std::size_t producer;
        NodeId source;
        std::size_t port;
    };

    const auto stride = packed ? lanes : 1;
    std::vector<std::vector<Dependency>> dependencies(nodes_.size() * stride);
    for (std::size_t id = 0; id < nodes_.size(); ++id) {
        if (!scheduled(id)) continue;
        const auto& node = nodes_[id];
        for (std::size_t lane = 0; lane < (split(node.kind) ? lanes : 1); ++lane) {
            auto& sources = dependencies[id * stride + lane];
            const auto dependOn = [&](NodeId source, std::size_t port, std::optional<std::size_t> sourceLane) {
                const auto& upstream = nodes_[index(source)];
                const auto producer = upstream.kind == (early ? Kind::DspEarlyOutput : Kind::DspOutput)
                    ? index(dspBlocks_[upstream.dspBlock].owner) : index(source);
                if (!scheduled(producer)) return;
                const auto first = split(nodes_[producer].kind) ? sourceLane.value_or(0) : 0;
                const auto count = !sourceLane && split(nodes_[producer].kind) ? lanes : 1;
                for (std::size_t i = 0; i < count; ++i) {
                    sources.push_back({producer * stride + first + i, source, port});
                }
            };
            if (node.kind == Kind::Dsp) {
                const auto& block = dspBlocks_[node.dspBlock];
                for (auto port : block.program->stageInputs[early ? 1 : 2]) {
                    if (const auto& input = block.inputs[port]) {
                        dependOn(input->source, port,
                                 input->width == DspInputWidth::Scalar ? std::optional<std::size_t>{0} : std::nullopt);
                    }
                }
            } else if (node.kind == Kind::Pack && packed) {
                if (lane < node.sources.size()) dependOn(node.sources[lane], lane, 0);
            } else {
                const auto sourceLane = split(node.kind) ? std::optional{lane}
                    : node.kind == Kind::Unpack ? std::optional{node.lane}
                    : node.kind == Kind::Processor ? std::optional<std::size_t>{0} : std::nullopt;
                for (std::size_t port = 0; port < node.sources.size(); ++port) {
                    dependOn(node.sources[port], port, sourceLane);
                }
            }
        }
    }

    enum class Visit { Pending, Reading, Ready };
    std::vector<Visit> states(dependencies.size(), Visit::Pending);
    struct Frame { std::size_t step; std::size_t nextInput = 0; };
    std::vector<Frame> pending;
    std::vector<LaneStep> result;
    for (std::size_t first = 0; first < dependencies.size(); ++first) {
        const auto id = first / stride;
        if (!scheduled(id) || (!split(nodes_[id].kind) && first % stride != 0) || states[first] != Visit::Pending) {
            continue;
        }
        pending.push_back({first});
        states[first] = Visit::Reading;
        while (!pending.empty()) {
            auto& frame = pending.back();
            const auto& sources = dependencies[frame.step];
            const auto target = frame.step / stride;
            if (frame.nextInput == sources.size()) {
                const auto mask = split(nodes_[target].kind) ? 1U << (frame.step % stride) : allLanes;
                const auto node = NodeId{target};
                const bool dependsOnOwnLane = std::ranges::any_of(sources, [&](const auto& edge) {
                    return edge.producer / stride == target;
                });
                if (!dependsOnOwnLane && !result.empty() && result.back().node == node) result.back().lanes |= mask;
                else result.push_back({node, mask});
                states[frame.step] = Visit::Ready;
                pending.pop_back();
                continue;
            }
            const auto [producer, source, port] = sources[frame.nextInput++];
            if (states[producer] == Visit::Reading) {
                if (feedback_ == FeedbackPolicy::ExplicitDelay) {
                    throw std::invalid_argument("Stream feedback requires a delay or staged DSP state");
                }
                if (!repaired) repaired = std::make_unique<Graph>(*this);
                const auto& node = repaired->nodes_[target];
                const auto currentSource = node.kind == Kind::Dsp
                    ? repaired->dspBlocks_[node.dspBlock].inputs[port]->source : node.sources[port];
                if (currentSource == source) repaired->delayInput(NodeId{target}, port, source);
            } else if (states[producer] == Visit::Pending) {
                pending.push_back({producer});
                states[producer] = Visit::Reading;
            }
        }
    }
    return result;
}

template<typename Sample>
detail::Runtime<Sample> Graph::compileFor(std::span<const NodeId> outputs) const
{
    if (outputs.empty()) {
        throw std::invalid_argument("Choose at least one stream output");
    }

    using Scalar = typename detail::Runtime<Sample>::Scalar;
    std::vector<bool> active(nodes_.size());
    std::vector<NodeId> pending(outputs.begin(), outputs.end());
    if (std::ranges::any_of(outputs, [&](NodeId output) { return nodes_.at(index(output)).kind == Kind::Dsp; })) {
        throw std::invalid_argument("Choose a declared DSP output");
    }
    while (!pending.empty()) {
        const auto id = index(pending.back());
        pending.pop_back();
        const auto& node = nodes_.at(id);
        if (active[id]) {
            continue;
        }
        active[id] = true;
        if constexpr (std::floating_point<Sample>) {
            if (node.kind == Kind::Pack || node.kind == Kind::Unpack) {
                throw std::invalid_argument("Pack and Unpack require a four-lane runtime");
            }
        }
        pending.insert(pending.end(), node.sources.begin(), node.sources.end());
        if (node.delayLength) pending.push_back(*node.delayLength);
        if (isDspOutput(node.kind)) {
            pending.push_back(dspBlocks_[node.dspBlock].owner);
        } else if (node.kind == Kind::Dsp) {
            const auto& block = dspBlocks_[node.dspBlock];
            for (const auto& inputs : block.program->stageInputs) {
                for (const auto port : inputs) {
                    if (const auto source = block.inputs[port]) {
                        pending.push_back(source->source);
                    }
                }
            }
        }
    }

    detail::Runtime<Sample> runtime;
    runtime.dspBlocks_.resize(dspBlocks_.size());
    runtime.processors_.resize(processors_.size());
    std::vector<std::vector<NodeId>> dependents(nodes_.size());
    std::vector<std::size_t> remainingSources(nodes_.size());
    const auto addDependency = [&](NodeId source, std::size_t id) {
        const auto kind = nodes_[index(source)].kind;
        if (kind != Kind::Input && kind != Kind::Delay && kind != Kind::DspEarlyOutput && kind != Kind::DspLateOutput) {
            dependents[index(source)].push_back(NodeId{id});
            ++remainingSources[id];
        }
    };
    std::size_t calculationCount = 0;
    for (std::size_t id = 0; id < nodes_.size(); ++id) {
        const auto& node = nodes_[id];
        if (active[id] && node.kind != Kind::Input && node.kind != Kind::Delay &&
            node.kind != Kind::DspEarlyOutput && node.kind != Kind::DspLateOutput) {
            ++calculationCount;
            if (node.kind == Kind::Dsp) {
                const auto& block = dspBlocks_[node.dspBlock];
                const auto& program = *block.program;
                runtime.activeDspBlocks_.push_back(node.dspBlock);
                for (const auto input : program.stageInputs[2]) {
                    if (const auto source = block.inputs[input]) {
                        addDependency(source->source, id);
                    }
                }
            } else if (node.kind == Kind::DspOutput) {
                addDependency(dspBlocks_[node.dspBlock].owner, id);
            } else {
                for (const auto source : node.sources) {
                    addDependency(source, id);
                }
            }
            if (remainingSources[id] == 0) {
                runtime.schedule_.push_back(NodeId{id});
            }
        }
    }

    for (std::size_t step = 0; step < runtime.schedule_.size(); ++step) {
        for (const auto dependent : dependents[index(runtime.schedule_[step])]) {
            if (--remainingSources[index(dependent)] == 0) {
                runtime.schedule_.push_back(dependent);
            }
        }
    }
    if (runtime.schedule_.size() != calculationCount) {
        constexpr bool packed = !std::floating_point<Sample>;
        if (!packed && feedback_ == FeedbackPolicy::ExplicitDelay) {
            throw std::invalid_argument("Stream feedback requires a delay or staged DSP state");
        }
        std::unique_ptr<Graph> repaired;
        runtime.laneSchedule_ = feedbackSchedule(active, packed, false, repaired);
        if (repaired) return repaired->compileFor<Sample>(outputs);
        runtime.schedule_.clear();
    }

    // Include input mixes in stage 1 when a DSP block needs them before stage 2.
    std::vector<NodeId> earlyNodes;
    std::vector<bool> early(nodes_.size());
    const auto addEarly = [&](NodeId id) {
        if (!early[index(id)]) {
            early[index(id)] = true;
            earlyNodes.push_back(id);
        }
    };
    for (const auto id : runtime.activeDspBlocks_) {
        if (!dspBlocks_[id].program->stages[1].empty()) addEarly(dspBlocks_[id].owner);
    }

    for (auto& list : dependents) list.clear();
    std::ranges::fill(remainingSources, 0);
    std::unique_ptr<Graph> repairedEarlyInputs;
    for (std::size_t step = 0; step < earlyNodes.size(); ++step) {
        const auto id = earlyNodes[step];
        const auto dependOn = [&](NodeId source, std::size_t port) {
            const auto& upstream = nodes_[index(source)];
            if (upstream.kind == Kind::Input || upstream.kind == Kind::Delay || upstream.kind == Kind::DspLateOutput) return;
            if (upstream.kind == Kind::DspEarlyOutput) {
                source = dspBlocks_[upstream.dspBlock].owner;
            } else if (upstream.kind != Kind::InputMix) {
                if (feedback_ == FeedbackPolicy::ExplicitDelay) {
                    throw std::invalid_argument("Stage 1 needs an input, a delay, a Stage 1 output, or a Stage 3 output");
                }
                if (!repairedEarlyInputs) repairedEarlyInputs = std::make_unique<Graph>(*this);
                repairedEarlyInputs->delayInput(id, port, source);
                return;
            }
            addEarly(source);
            dependents[index(source)].push_back(id);
            ++remainingSources[index(id)];
        };
        const auto& node = nodes_[index(id)];
        if (node.kind == Kind::InputMix) {
            for (std::size_t port = 0; port < node.sources.size(); ++port) dependOn(node.sources[port], port);
        } else {
            const auto& block = dspBlocks_[node.dspBlock];
            for (const auto port : block.program->stageInputs[1]) {
                if (const auto source = block.inputs[port]) dependOn(source->source, port);
            }
        }
        if (remainingSources[index(id)] == 0) runtime.earlySchedule_.push_back(id);
    }
    if (repairedEarlyInputs) return repairedEarlyInputs->compileFor<Sample>(outputs);
    for (std::size_t step = 0; step < runtime.earlySchedule_.size(); ++step) {
        for (const auto dependent : dependents[index(runtime.earlySchedule_[step])]) {
            if (--remainingSources[index(dependent)] == 0) runtime.earlySchedule_.push_back(dependent);
        }
    }
    if (runtime.earlySchedule_.size() != earlyNodes.size()) {
        if (feedback_ == FeedbackPolicy::ExplicitDelay) throw std::invalid_argument("Stage 1 contains a feedback cycle");
        std::unique_ptr<Graph> repaired;
        static_cast<void>(feedbackSchedule(early, false, true, repaired));
        if (repaired) return repaired->compileFor<Sample>(outputs);
        throw std::logic_error("Could not schedule Stage 1 feedback");
    }

    std::erase_if(runtime.laneSchedule_, [&](LaneStep step) {
        return nodes_[index(step.node)].kind == Kind::InputMix && early[index(step.node)];
    });
    std::erase_if(runtime.schedule_, [&](NodeId id) {
        const auto kind = nodes_[index(id)].kind;
        return kind == Kind::DspOutput || (kind == Kind::InputMix && early[index(id)]);
    });
    // Prepare processor and delay state once the feedback schedule has settled.
    for (std::size_t id = 0; id < nodes_.size(); ++id) {
        const auto& node = nodes_[id];
        if (node.kind == Kind::Input) {
            runtime.inputs_.push_back(NodeId{id});
        } else if (active[id] && node.kind == Kind::Delay) {
            if (node.delayLength) {
                runtime.variableDelays_.push_back({NodeId{id}, *node.delayLength, std::vector<Scalar>(maxDelaySamples)});
            } else {
                runtime.delays_.push_back({NodeId{id}});
            }
        } else if (active[id]) {
            if (node.kind == Kind::Processor) {
                const auto& spec = processors_[node.dspBlock];
                if (node.sources.size() != spec.inputs) {
                    throw std::invalid_argument("A sample processor needs all its inputs connected");
                }
                auto processor = spec.prepare();
                if (!processor) throw std::invalid_argument("Could not prepare a sample processor");
                runtime.processors_[node.dspBlock] = {std::move(processor), std::vector<double>(spec.inputs)};
            }
            if (node.kind == Kind::Dsp) {
                const auto& block = dspBlocks_[node.dspBlock];
                const auto& program = *block.program;
                runtime.dspBlocks_[node.dspBlock] = {block, {},
                    std::vector<detail::DspLanes>(program.slotCount),
                    std::vector<detail::DspLanes>(program.arraySlotCount),
                    detail::DspMemoryReferences{std::vector<green::Memory>(program.hasMemRef ? program.inputs.size() : 0),
                        std::vector<std::vector<std::uint8_t>>(program.hasMemRef ? program.inputs.size() : 0),
                        std::vector<detail::DspMemoryArray>(program.hasMemRef ? program.inputs.size() : 0)},
                    std::vector<detail::DspValue>(backend_ == DspBackend::Interpreter ? program.stackSize : 0)};
                for (std::size_t binding = 0; binding < program.enginePointers.size(); ++binding) {
                    if (!block.enginePointers[binding] && program.enginePointerUsed[binding]) throw std::invalid_argument("Bind __const engine pointer '" + program.enginePointers[binding] + "' before compiling this block");
                    const auto supplied = block.enginePointers[binding].value_or(DspEnginePointerTable{});
                    auto& refs = runtime.dspBlocks_[node.dspBlock].references;
                    refs.engine.push_back(prepareEngineTable(supplied,
                        std::ranges::find(program.targetMemoryPorts, detail::enginePointerPortBase + binding) != program.targetMemoryPorts.end()));
                }
                if (program.hasNativeFrame) {
                    auto& refs = runtime.dspBlocks_[node.dspBlock].references;
                    refs.nativeFrame.resize(detail::nativeFramePrefixBytes);
                    refs.initialNativeFrame = refs.nativeFrame;
                    refs.nativeFrameTargets.resize(detail::nativeFramePrefixBytes);
                }
                for (std::size_t input = 0; input < program.inputs.size(); ++input) {
                    if (program.inputs[input].type == DspPortType::MemArrayRef) {
                        runtime.dspBlocks_[node.dspBlock].references.arrays[input].enabled = true;
                    }
                }
                if (!program.stages[3].empty()) runtime.lateDspBlocks_.push_back(node.dspBlock);
            }
        }
    }
    runtime.nodes_.reserve(nodes_.size());
    for (const auto& node : nodes_) {
        runtime.nodes_.push_back({node.kind, static_cast<Scalar>(node.gain), node.sources, node.dspBlock,
                                  node.dspPort, node.operation, node.precision, node.lane, node.delayLength, node.table,
                                  node.routes, node.routeOutput});
    }
    runtime.outputs_.assign(outputs.begin(), outputs.end());
    runtime.samples_.resize(nodes_.size());
    runtime.rendered_ = std::move(active);
    for (const auto id : runtime.activeDspBlocks_) {
        auto& state = runtime.dspBlocks_[id];
        if (backend_ == DspBackend::LlvmOrc) {
            state.native = state.block.program->nativeProgram();
        }
        // The stages produce every output of an active block, including unused ports.
        for (const auto output : state.block.outputs) {
            runtime.rendered_[index(output)] = true;
        }
    }
    return runtime;
}

namespace detail {
namespace {

template<typename Sample>
auto& dspLane(Sample& sample, std::size_t lane) noexcept
{
    if constexpr (std::floating_point<Sample>) {
        return sample;
    } else {
        return sample[lane];
    }
}

}

template<typename Sample>
void Runtime<Sample>::loadDspInputs(DspState& state, unsigned stage) noexcept
{
    const auto& program = *state.block.program;
    constexpr auto laneCount = std::same_as<Sample, Scalar> ? std::size_t{1} : std::size_t{4};
    for (const auto port : program.stageInputs[stage]) {
        auto& slot = state.dspSlots[program.inputSlots[port]];
        if (port < program.writableInputs.size() && program.writableInputs[port]) slot.fill(0);
        const auto type = dspPortType(program.inputs[port].type);
        const auto source = state.block.inputs[port];
        if (!source) {
            continue;
        }
        const auto lanes = source->width == DspInputWidth::Scalar ? 1 : laneCount;
        const auto& node = nodes_[index(source->source)];
        if (Graph::isDspOutput(node.kind)) {
            // Direct DSP links retain their declared precision, including all 32 PolyInt bits.
            const auto& upstream = dspBlocks_[node.dspBlock];
            const auto& sourceProgram = *upstream.block.program;
            const auto sourceType = dspPortType(sourceProgram.outputs[node.dspPort].type);
            const auto& sourceSlot = upstream.dspSlots[sourceProgram.outputSlots[node.dspPort]];
            for (std::size_t lane = 0; lane < lanes; ++lane) {
                slot[lane] = dspConvert(sourceSlot[lane], sourceType, type);
            }
        } else {
            const auto& current = samples_[index(source->source)];
            for (std::size_t lane = 0; lane < lanes; ++lane) {
                if constexpr (std::same_as<Scalar, float>) {
                    slot[lane] = type == DspType::Float ? std::bit_cast<std::uint32_t>(dspLane(current, lane))
                        : dspEncode(dspLane(current, lane), type);
                } else {
                    slot[lane] = dspEncode(dspLane(current, lane), type);
                }
            }
        }
    }
}

template<typename Sample>
void Runtime<Sample>::loadIncomingXmm(DspState& state, unsigned stage, std::uint64_t frame,
                                      std::span<DspLanes> slots) noexcept
{
    for (const auto& [reg, slot] : state.block.program->incomingXmmSlots[stage]) {
        auto& value = slots[slot]; value.fill(0);
        if (!state.incomingXmm) continue;
        const auto& binding = (*state.incomingXmm)[reg];
        if (binding.frames.empty() || frame < binding.startFrame) continue;
        const auto at = binding.frames.size() == 1 ? 0 : frame - binding.startFrame;
        if (at >= binding.frames.size()) continue;
        std::ranges::copy(binding.frames[at], value.begin());
    }
}

template<typename Sample>
bool Runtime<Sample>::setDspIncomingXmm(DspBlockId block, unsigned reg,
                                        std::span<const DspXmmRegister> frames)
{
    const auto id = static_cast<std::size_t>(block);
    if (id >= dspBlocks_.size() || !dspBlocks_[id].block.program || reg >= 16 || frames.size() > 1048576) return false;
    auto& state = dspBlocks_[id];
    if (std::ranges::all_of(state.block.program->incomingXmmSlots, [](const auto& slots) { return slots.empty(); })) return false;
    IncomingXmm next{std::vector<DspXmmRegister>(frames.begin(), frames.end()), frame_};
    if (!state.incomingXmm) state.incomingXmm = std::make_unique<std::array<IncomingXmm, 16>>();
    (*state.incomingXmm)[reg] = std::move(next);
    return true;
}

template<typename Sample>
void Runtime<Sample>::executeDspStage(DspState& state, unsigned stage) noexcept
{
    const auto& program = *state.block.program;
    loadIncomingXmm(state, stage, frame_, state.dspSlots);
    state.references.samplePosition = sampleClock_.position;
    constexpr unsigned active = std::same_as<Sample, Scalar> ? 1 : 15;
    prepareDspNativeFrame(state.references, frame_, false, active, 0, {});
    if (state.native) {
        state.native->stages[stage](state.dspSlots.data(), state.arrays.data(), &state.references, frame_);
    } else {
        runDspStage(program.stages[stage], state.dspSlots, state.arrays, state.references, state.stack, frame_);
    }
    constexpr auto laneCount = std::same_as<Sample, Scalar> ? std::size_t{1} : std::size_t{4};
    for (std::size_t port = 0; port < program.outputs.size(); ++port) {
        if (!program.publishesOutput(port, stage)) continue;
        const auto type = dspPortType(program.outputs[port].type);
        const auto& value = state.dspSlots[program.outputSlots[port]];
        auto& output = samples_[index(state.block.outputs[port])];
        for (std::size_t lane = 0; lane < laneCount; ++lane) {
            constexpr auto target = std::same_as<Scalar, double> ? DspType::Double : DspType::Float;
            const auto bits = dspConvert(value[lane], type, target);
            if constexpr (std::same_as<Scalar, float>) dspLane(output, lane) = std::bit_cast<float>(static_cast<std::uint32_t>(bits));
            else dspLane(output, lane) = std::bit_cast<double>(bits);
        }
    }
}

template<typename Sample>
bool Runtime<Sample>::setDspEnginePointer(DspBlockId block, std::string_view name, const green::Memory& memory, bool writable)
{
    return setDspEnginePointerTable(block, name, {{DspEnginePointerBinding{memory, writable}}, {}});
}

template<typename Sample>
bool Runtime<Sample>::setDspEnginePointerTable(DspBlockId block, std::string_view name, const DspEnginePointerTable& table)
{
    const auto id = static_cast<std::size_t>(block);
    if (id >= dspBlocks_.size() || !dspBlocks_[id].block.program) return false;
    auto& state = dspBlocks_[id];
    const auto& names = state.block.program->enginePointers;
    const auto found = std::ranges::find(names, name);
    if (found == names.end()) return false;
    const auto binding = static_cast<std::size_t>(found - names.begin());
    detail::DspEngineMemory prepared;
    try {
        prepared = prepareEngineTable(table,
            std::ranges::find(state.block.program->targetMemoryPorts, enginePointerPortBase + binding) != state.block.program->targetMemoryPorts.end());
    } catch (const std::invalid_argument&) { return false; }
    const auto& previous = state.references.engine[binding];
    for (std::size_t i = 0; i < prepared.regions.size() && i < previous.regions.size(); ++i) {
        const auto& old = previous.regions[i];
        auto& next = prepared.regions[i];
        if (next.memory.address() == old.memory.address() && next.memory.size() == old.memory.size()) next.targets = old.targets;
    }
    state.references.engine[binding] = std::move(prepared);
    return true;
}

template<typename Sample>
bool Runtime<Sample>::setDspNativeFrame(DspBlockId block, std::span<const std::uint8_t> bytes)
{
    const auto id = static_cast<std::size_t>(block);
    if (id >= dspBlocks_.size() || !dspBlocks_[id].block.program || !dspBlocks_[id].block.program->hasNativeFrame ||
        bytes.size() < nativeFramePrefixBytes || bytes.size() > 16U * 1024U * 1024U) return false;
    auto initial = std::vector<std::uint8_t>(bytes.begin(), bytes.end());
    auto current = initial;
    auto tags = std::vector<std::uint8_t>(bytes.size());
    auto& refs = dspBlocks_[id].references;
    refs.initialNativeFrame = std::move(initial);
    refs.nativeFrame = std::move(current);
    refs.nativeFrameTargets = std::move(tags);
    refs.nativeFramePrepared = refs.nativeFrameContextInitialized = false;
    return true;
}

template<typename Sample>
void Runtime<Sample>::processDspStage(DspState& state, unsigned stage) noexcept
{
    if (state.block.program->stages[stage].empty()) return;
    loadDspInputs(state, stage);
    executeDspStage(state, stage);
}

template<typename Sample>
DspFault Runtime<Sample>::dspFault(DspBlockId block) const noexcept
{
    const auto id = static_cast<std::size_t>(block);
    return id < dspBlocks_.size() ? dspBlocks_[id].references.fault : DspFault{};
}

template<typename Sample>
bool Runtime<Sample>::setDspArrayInput(DspBlockId block, std::size_t input, std::span<const float> values) noexcept
{
    const auto id = static_cast<std::size_t>(block);
    if (id >= dspBlocks_.size()) {
        return false;
    }
    auto& state = dspBlocks_[id];
    const auto& program = state.block.program;
    if (!program || input >= program->inputs.size() || program->inputs[input].arraySize == 0) {
        return false;
    }
    auto target = std::span{state.arrays}.subspan(program->inputSlots[input], program->inputs[input].arraySize);
    const auto count = std::min(values.size(), target.size());
    for (std::size_t i = 0; i < count; ++i) {
        target[i].fill(dspEncode(values[i], DspType::Float));
    }
    std::ranges::fill(target.subspan(count), DspLanes{});
    return true;
}

template<typename Sample>
bool Runtime<Sample>::setDspMemArrayInput(DspBlockId block, std::size_t input, const green::MemoryArray& values) noexcept
{
    const auto id = static_cast<std::size_t>(block);
    if (id >= dspBlocks_.size()) return false;
    auto& state = dspBlocks_[id];
    const auto& program = state.block.program;
    if (!program || input >= program->inputs.size() || program->inputs[input].type != DspPortType::MemArrayRef) {
        return false;
    }
    const auto count = values.entries().size();
    if (!std::in_range<std::int32_t>(count)) return false;
    try {
        auto binding = bindDspMemArray(values);
        state.references.arrays[input] = std::move(binding);
    } catch (...) {
        return false;
    }
    const auto base = program->inputSlots[input];
    state.arrays[base + static_cast<std::size_t>(MemArrayField::Count)].fill(count);
    const auto lastIndex = count == 0 ? 0.0 : static_cast<double>(count - 1);
    state.arrays[base + static_cast<std::size_t>(MemArrayField::LastIndex)].fill(dspEncode(lastIndex, DspType::Float));
    return true;
}

template<typename Sample>
bool Runtime<Sample>::setDspMemRefInput(DspBlockId block, std::size_t input, const green::Memory& value) noexcept
{
    const auto id = static_cast<std::size_t>(block);
    if (id >= dspBlocks_.size()) return false;
    auto& state = dspBlocks_[id];
    const auto& program = state.block.program;
    if (!program || input >= program->inputs.size() || program->inputs[input].type != DspPortType::MemRef ||
        !std::in_range<std::int32_t>(value.size())) return false;
    auto& references = state.references;
    const auto& previous = references.memory[input];
    if (previous.address() != value.address() || previous.size() != value.size()) {
        try {
            if (std::ranges::find(program->targetMemoryPorts, input) != program->targetMemoryPorts.end()) {
                references.targets[input].assign(value.size(), 0);
            }
        } catch (...) {
            return false;
        }
    }
    references.memory[input] = value;
    state.arrays[program->inputSlots[input]] = {value.size() != 0, value.size(),
                                              value.size() / sizeof(float), static_cast<std::uint64_t>(value.channels())};
    auto& limit = state.arrays[program->inputSlots[input] + static_cast<std::size_t>(MemField::Limit)];
    if (value.size() == 0) {
        limit.fill(0);
    } else {
        // Both native binders convert the frame count first, then subtract one in Float precision.
        // The separate rounding steps are observable beyond 2^24 frames.
        detail::DspFloatingEnvironment environment;
        volatile float frames = static_cast<float>(value.size() / (value.channels() == 2 ? 8U : 4U));
        const float last = frames - 1.F;
        limit.fill(std::bit_cast<std::uint32_t>(last));
    }
    return true;
}

template<typename Sample>
Sample Runtime<Sample>::sample(NodeId node) const noexcept
{
    return samples_[index(node)];
}

template<typename Sample>
Sample Runtime<Sample>::inputSample(const Node& node, std::size_t input) const noexcept
{
    return input < node.sources.size() ? sample(node.sources[input]) : Sample{};
}

template<typename Sample>
Sample Runtime<Sample>::sumSources(const Node& node) const noexcept
{
    auto sum = inputSample(node);
    for (std::size_t edge = 1; edge < node.sources.size(); ++edge) {
        sum = addSamples(sum, sample(node.sources[edge]));
    }
    return sum;
}

template<typename Sample>
bool Runtime<Sample>::renders(NodeId signal) const noexcept
{
    return index(signal) < rendered_.size() && rendered_[index(signal)] &&
           nodes_[index(signal)].kind != Graph::Kind::Dsp;
}

template<typename Sample>
std::string Runtime<Sample>::compiledDspCode() const
{
    std::string code;
    for (const auto block : activeDspBlocks_) {
        const auto& native = dspBlocks_[block].native;
        if (!native) {
            return "; DSP interpreter is active";
        }
        code += "; DSP block " + std::to_string(block) + "\n" + native->optimizedIr + "\n";
    }
    for (const auto& state : processors_) {
        if (state.processor) code += state.processor->compiledDspCode();
    }
    return code;
}

template<typename Sample>
Sample Runtime<Sample>::mixInput(const Node& node) const noexcept
{
    auto result = inputSample(node);
    for (std::size_t i = 1; i < node.sources.size(); ++i) {
        result = calculateSamples(node.operation, node.precision, result, inputSample(node, i));
    }
    return result;
}

template<typename Sample>
Sample Runtime<Sample>::calculateNode(NodeId id) noexcept
{
    const auto& node = nodes_[index(id)];
    Sample result{};
    switch (node.kind) {
    case Graph::Kind::Gain:
        result = multiplySamples(inputSample(node), node.gain);
        break;
    case Graph::Kind::Multiply:
        result = multiplySamples(inputSample(node), inputSample(node, 1));
        break;
    case Graph::Kind::Sum:
        result = sumSources(node);
        break;
    case Graph::Kind::InputMix:
        result = mixInput(node);
        break;
    case Graph::Kind::RouteMix: {
        bool hasSource = false;
        result = {};
        for (const auto source : node.routes->sources(node.routeOutput)) {
            if (source >= node.sources.size()) continue;
            const auto value = inputSample(node, source);
            result = hasSource ? calculateSamples(node.operation, node.precision, result, value) : value;
            hasSource = true;
        }
        break;
    }
    case Graph::Kind::Pack:
        if constexpr (!std::floating_point<Sample>) {
            for (std::size_t lane = 0; lane < result.size(); ++lane) {
                result[lane] = inputSample(node, lane).front();
            }
            result = calculateSamples(SampleOperation::Convert, node.precision, result, Sample{});
        }
        break;
    case Graph::Kind::Unpack:
        if constexpr (!std::floating_point<Sample>) {
            result.fill(inputSample(node)[node.lane]);
        }
        break;
    case Graph::Kind::Arithmetic:
        result = calculateSamples(node.operation, node.precision, inputSample(node), inputSample(node, 1));
        break;
    case Graph::Kind::FloatArrayRead: {
        const auto table = node.table->read();
        const auto indices = inputSample(node);
        if constexpr (std::floating_point<Sample>) {
            result = table[floatArrayIndex(static_cast<float>(indices))];
        } else {
            for (std::size_t lane = 0; lane < result.size(); ++lane) {
                result[lane] = table[floatArrayIndex(static_cast<float>(indices[lane]))];
            }
        }
        break;
    }
    case Graph::Kind::Processor: {
        auto& state = processors_[node.dspBlock];
        for (std::size_t input = 0; input < state.inputs.size(); ++input) {
            const auto upstream = inputSample(node, input);
            if constexpr (std::floating_point<Sample>) state.inputs[input] = upstream;
            else state.inputs[input] = upstream.front();
        }
        const auto sample = static_cast<Scalar>(state.processor->process(state.inputs));
        if constexpr (std::floating_point<Sample>) result = sample;
        else result.fill(sample);
        break;
    }
    case Graph::Kind::Dsp:
        processDspStage(dspBlocks_[node.dspBlock], 2);
        break;
    case Graph::Kind::Input:
    case Graph::Kind::Delay:
    case Graph::Kind::DspEarlyOutput:
    case Graph::Kind::DspOutput:
    case Graph::Kind::DspLateOutput:
        break;
    }
    return result;
}

template<typename Sample>
bool Runtime<Sample>::process(std::span<const InputBuffer> inputs, std::span<const OutputBuffer> outputs,
                              std::span<const Probe> probes) noexcept
{
    if (inputs.size() != inputs_.size() || outputs.size() != outputs_.size() || outputs.empty()) {
        return false;
    }
    const auto frames = outputs.front().size();
    const auto wrongLength = [frames](auto buffer) { return buffer.size() != frames; };
    if (std::ranges::any_of(inputs, wrongLength) || std::ranges::any_of(outputs, wrongLength) ||
        std::ranges::any_of(probes, [&](const auto& probe) {
            return !renders(probe.signal) || probe.tail.size() > frames;
        })) {
        return false;
    }

    detail::DspFloatingEnvironment floatingEnvironment;
    if (frames != 0) {
        for (auto& state : processors_) {
            if (state.processor) state.processor->beginBlock(frame_);
        }
    }
    if (frames != 0 && !initialized_) {
        for (const auto block : activeDspBlocks_) {
            auto& state = dspBlocks_[block];
            initializeDspSlots(state.block.program->initializers, state.dspSlots);
            processDspStage(state, 0);
        }
        initialized_ = true;
    }
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::size_t channel = 0; channel < inputs.size(); ++channel) {
            samples_[index(inputs_[channel])] = inputs[channel][frame];
        }
        for (const auto& delay : delays_) {
            samples_[index(delay.node)] = delay.previous;
        }
        for (const auto& delay : variableDelays_) {
            const auto& control = samples_[index(delay.length)];
            const auto length = delayLengthInSamples(dspLane(control, 0));
            const auto read = delay.next + (maxDelaySamples - length);
            const auto delayed = delay.history[read >= maxDelaySamples ? read - maxDelaySamples : read];
            auto& result = samples_[index(delay.node)];
            if constexpr (std::floating_point<Sample>) result = delayed;
            else result.fill(delayed);
        }
        for (const auto id : earlySchedule_) {
            const auto& node = nodes_[index(id)];
            if (node.kind == Graph::Kind::InputMix) {
                samples_[index(id)] = mixInput(node);
            } else {
                processDspStage(dspBlocks_[node.dspBlock], 1);
            }
        }
        for (const auto id : schedule_) {
            samples_[index(id)] = calculateNode(id);
        }
        if constexpr (!std::floating_point<Sample>) {
            for (const auto [id, lanes] : laneSchedule_) {
                auto& signal = samples_[index(id)];
                const auto calculated = calculateNode(id);
                if (lanes == Graph::allLanes) {
                    signal = calculated;
                } else {
                    for (std::size_t lane = 0; lane < signal.size(); ++lane) {
                        if (lanes & (1U << lane)) signal[lane] = calculated[lane];
                    }
                }
            }
        }
        for (std::size_t channel = 0; channel < outputs.size(); ++channel) {
            outputs[channel][frame] = sample(outputs_[channel]);
        }
        for (const auto& probe : probes) {
            const auto start = frames - probe.tail.size();
            if (frame >= start) {
                probe.tail[frame - start] = sample(probe.signal);
            }
        }
        // Commit this frame's completed values after every delay has published.
        for (auto& delay : delays_) {
            const auto& node = nodes_[index(delay.node)];
            delay.previous = inputSample(node);
            if (node.precision == SamplePrecision::Float32) {
                delay.previous = calculateSamples(SampleOperation::Convert, node.precision, delay.previous, Sample{});
            }
        }
        for (auto& delay : variableDelays_) {
            const auto& node = nodes_[index(delay.node)];
            const auto source = inputSample(node);
            auto value = dspLane(source, 0);
            if (node.precision == SamplePrecision::Float32) value = static_cast<float>(value);
            delay.history[delay.next] = value;
            if (++delay.next == maxDelaySamples) delay.next = 0;
        }
        // Every stage 3 block reads the completed sample before any stage 3 outlet changes.
        // Outlet writes then become visible when the next sample is rendered.
        for (const auto block : lateDspBlocks_) loadDspInputs(dspBlocks_[block], 3);
        for (const auto block : lateDspBlocks_) executeDspStage(dspBlocks_[block], 3);
        ++frame_;
        sampleClock_.advance();
    }
    if (frames != 0) sampleClock_.finishBuffer();
    return true;
}

template<typename Sample>
typename Runtime<Sample>::VoicePacket Runtime<Sample>::makeVoicePacket(std::array<Runtime*, 4> voices)
    requires (std::same_as<Sample, float> || std::same_as<Sample, MonoDouble>)
{
    VoicePacket packet;
    packet.voices = voices;
    std::size_t slots = 0, stack = 0;
    for (const auto block : voices[0]->activeDspBlocks_) {
        const auto& program = *voices[0]->dspBlocks_[block].block.program;
        slots = std::max(slots, program.slotCount);
        stack = std::max(stack, program.stackSize);
    }
    packet.dspSlots.resize(slots);
    packet.stack.resize(stack);
    return packet;
}

template<typename Sample>
void Runtime<Sample>::processVoiceDsp(VoicePacket& packet, std::size_t block, unsigned stage,
    unsigned active, unsigned writeMask, bool loadInputs) noexcept requires (std::same_as<Sample, float> || std::same_as<Sample, MonoDouble>)
{
    auto& shared = packet.voices[0]->dspBlocks_[block];
    const auto& program = *shared.block.program;
    if (program.stages[stage].empty()) return;
    for (std::size_t lane = 0; lane < 4; ++lane) {
        auto* voice = packet.voices[lane];
        if (voice && (active & (1U << lane)) && loadInputs) voice->loadDspInputs(voice->dspBlocks_[block], stage);
        for (std::size_t slot = 0; slot < program.slotCount; ++slot) {
            packet.dspSlots[slot][lane] = voice && (active & (1U << lane)) ? voice->dspBlocks_[block].dspSlots[slot][0] : 0;
        }
    }
    // Incoming values describe one physical SSE packet, like the shared frame image.
    loadIncomingXmm(shared, stage, stage == 0 ? 0 : packet.frame, packet.dspSlots);
    shared.references.samplePosition = packet.samplePosition;
    prepareDspNativeFrame(shared.references, packet.nativeFramePosition, true, active, packet.gates, packet.voiceSlots);
    if (shared.native) {
        shared.native->stages[stage](packet.dspSlots.data(), shared.arrays.data(), &shared.references, stage == 0 ? 0 : packet.frame);
    } else {
        runDspStage(program.stages[stage], std::span{packet.dspSlots}.first(program.slotCount),
                    shared.arrays, shared.references, packet.stack, stage == 0 ? 0 : packet.frame);
    }
    for (std::size_t lane = 0; lane < 4; ++lane) {
        auto* voice = packet.voices[lane];
        if (!voice || !(writeMask & (1U << lane))) continue;
        auto& state = voice->dspBlocks_[block];
        for (std::size_t slot = 0; slot < program.slotCount; ++slot) state.dspSlots[slot][0] = packet.dspSlots[slot][lane];
        for (std::size_t port = 0; port < program.outputs.size(); ++port) {
            if (!program.publishesOutput(port, stage)) continue;
            const auto type = dspPortType(program.outputs[port].type);
            constexpr auto target = std::same_as<Sample, MonoDouble> ? DspType::Double : DspType::Float;
            const auto bits = dspConvert(packet.dspSlots[program.outputSlots[port]][lane], type, target);
            if constexpr (std::same_as<Sample, float>) voice->samples_[index(state.block.outputs[port])] = std::bit_cast<float>(static_cast<std::uint32_t>(bits));
            else voice->samples_[index(state.block.outputs[port])] = std::bit_cast<double>(bits);
        }
    }
}

template<typename Sample>
void Runtime<Sample>::processVoicePacket(VoicePacket& packet, unsigned active,
    const std::array<std::span<const Sample>, 4>& inputs,
    const std::array<std::span<Sample>, 4>& outputs) noexcept requires (std::same_as<Sample, float> || std::same_as<Sample, MonoDouble>)
{
    unsigned starting = 0;
    for (std::size_t lane = 0; lane < 4; ++lane) {
        auto* voice = packet.voices[lane];
        if (!voice || !(active & (1U << lane))) continue;
        for (auto& state : voice->processors_) if (state.processor) state.processor->beginBlock(voice->frame_);
        if (!voice->initialized_) {
            starting |= 1U << lane;
            for (const auto block : voice->activeDspBlocks_) {
                auto& state = voice->dspBlocks_[block];
                initializeDspSlots(state.block.program->initializers, state.dspSlots);
            }
            voice->initialized_ = true;
        }
    }
    auto& schedule = *packet.voices[0];
    if (starting) for (const auto block : schedule.activeDspBlocks_) processVoiceDsp(packet, block, 0, active, starting);
    for (std::size_t lane = 0; lane < 4; ++lane) {
        auto* voice = packet.voices[lane];
        if (!voice || !(active & (1U << lane))) continue;
        for (std::size_t input = 0; input < inputs[lane].size(); ++input) voice->samples_[index(voice->inputs_[input])] = inputs[lane][input];
        for (const auto& delay : voice->delays_) voice->samples_[index(delay.node)] = delay.previous;
        for (const auto& delay : voice->variableDelays_) {
            const auto length = delayLengthInSamples(voice->samples_[index(delay.length)]);
            const auto read = delay.next + (maxDelaySamples - length);
            voice->samples_[index(delay.node)] = delay.history[read >= maxDelaySamples ? read - maxDelaySamples : read];
        }
    }
    for (const auto id : schedule.earlySchedule_) {
        const auto& node = schedule.nodes_[index(id)];
        if (node.kind != Graph::Kind::InputMix) processVoiceDsp(packet, node.dspBlock, 1, active, active);
        else for (std::size_t lane = 0; lane < 4; ++lane) {
            if (auto* voice = packet.voices[lane]; voice && (active & (1U << lane))) {
                voice->samples_[index(id)] = voice->mixInput(voice->nodes_[index(id)]);
            }
        }
    }
    for (const auto id : schedule.schedule_) {
        const auto& node = schedule.nodes_[index(id)];
        if (node.kind == Graph::Kind::Dsp) processVoiceDsp(packet, node.dspBlock, 2, active, active);
        else for (std::size_t lane = 0; lane < 4; ++lane) {
            if (auto* voice = packet.voices[lane]; voice && (active & (1U << lane))) voice->samples_[index(id)] = voice->calculateNode(id);
        }
    }
    for (std::size_t lane = 0; lane < 4; ++lane) {
        auto* voice = packet.voices[lane];
        if (!voice || !(active & (1U << lane))) continue;
        for (std::size_t output = 0; output < outputs[lane].size(); ++output) outputs[lane][output] = voice->sample(voice->outputs_[output]);
        for (auto& delay : voice->delays_) {
            const auto& node = voice->nodes_[index(delay.node)];
            delay.previous = voice->inputSample(node);
            if (node.precision == SamplePrecision::Float32) delay.previous = static_cast<float>(delay.previous);
        }
        for (auto& delay : voice->variableDelays_) {
            const auto& node = voice->nodes_[index(delay.node)];
            auto value = voice->inputSample(node);
            if (node.precision == SamplePrecision::Float32) value = static_cast<float>(value);
            delay.history[delay.next] = value;
            if (++delay.next == maxDelaySamples) delay.next = 0;
        }
        for (const auto block : voice->lateDspBlocks_) voice->loadDspInputs(voice->dspBlocks_[block], 3);
    }
    // Stage 3 reads are all latched before the first stage 3 output changes.
    for (const auto block : schedule.lateDspBlocks_) processVoiceDsp(packet, block, 3, active, active, false);
    for (std::size_t lane = 0; lane < 4; ++lane) if (packet.voices[lane] && (active & (1U << lane))) ++packet.voices[lane]->frame_;
    ++packet.frame;
}

template<typename Sample>
void Runtime<Sample>::reset() noexcept
{
    resetState(false);
}

template<typename Sample>
void Runtime<Sample>::resetState(bool preserveNativeFrame) noexcept
{
    for (auto& state : processors_) {
        if (state.processor) state.processor->reset();
    }

    for (auto& delay : delays_) {
        delay.previous = Sample{};
    }
    for (auto& delay : variableDelays_) {
        std::ranges::fill(delay.history, Scalar{});
        delay.next = 0;
    }
    for (const auto block : activeDspBlocks_) {
        auto& state = dspBlocks_[block];
        std::ranges::fill(state.dspSlots, DspLanes{});
        if (state.incomingXmm) for (auto& binding : *state.incomingXmm) binding.startFrame = 0;
        state.references.fault = {};
        if (!preserveNativeFrame) {
            std::ranges::copy(state.references.initialNativeFrame, state.references.nativeFrame.begin());
            std::ranges::fill(state.references.nativeFrameTargets, 0);
            state.references.nativeFramePrepared = state.references.nativeFrameContextInitialized = false;
        }
        for (const auto output : state.block.outputs) {
            samples_[index(output)] = Sample{};
        }
    }
    frame_ = 0;
    sampleClock_ = {};
    initialized_ = false;
}

template class Runtime<float>;
template class Runtime<MonoDouble>;
template class Runtime<Mono4>;
template class Runtime<MonoDouble4>;

}

}
