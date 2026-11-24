#include "Session.hpp"
#include "AudioPatch.hpp"
#include "stream/DeZipper.hpp"
#include "stream/PolyRenderer.hpp"
#include "stream/PolyRuntime.hpp"
#include "stream/Runtime.hpp"
#include "stream/StreamCounter.hpp"
#include "stream/Oscillator.hpp"
#include "stream/Wave.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <concepts>
#include <functional>
#include <iterator>
#include <map>
#include <ranges>
#include <set>
#include <stdexcept>
#include <utility>
#include <variant>

namespace openstone {
namespace {

using green::Type;
constexpr std::size_t swatchFrames = 128;

// The same processing modes apply to each sample format. A wire still has
// to agree on the sample format, regardless of its eventual mode.
struct StreamFamily {
    Type generic;
    std::array<std::optional<Type>, 3> modes; // Mono, Poly, Mono4.
};

constexpr std::array streamFamilies{
    StreamFamily{Type::Stream, {Type::Mono, Type::Poly, Type::Mono4}},
    StreamFamily{Type::StreamDouble, {Type::MonoDouble, Type::PolyDouble, Type::Mono4Double}},
    StreamFamily{Type::StreamBoolean, {Type::MonoBoolean, Type::PolyBoolean, std::nullopt}},
};

const StreamFamily* streamFamily(Type type)
{
    const auto found = std::ranges::find_if(streamFamilies, [type](const auto& family) {
        return family.generic == type || std::ranges::contains(family.modes, type);
    });
    return found == streamFamilies.end() ? nullptr : &*found;
}

bool isPolySignal(std::optional<Type> type)
{
    return type == Type::Poly || type == Type::PolyDouble || type == Type::PolyBoolean || type == Type::PolyInt;
}

bool isPackedStream(std::optional<Type> type)
{
    return type == Type::Mono4 || type == Type::Mono4Double;
}

struct PortRemap {
    std::vector<int> indices;
    std::vector<std::uint64_t> triggers;
};

PortRemap remapPorts(std::span<const stream::DspPort> previous, std::span<const stream::DspPort> next,
                     std::span<const std::uint64_t> triggers)
{
    PortRemap result{std::vector<int>(previous.size(), -1), std::vector<std::uint64_t>(next.size())};
    for (std::size_t i = 0; i < previous.size(); ++i) {
        const auto found = std::ranges::find(next, previous[i].name, &stream::DspPort::name);
        if (found != next.end() && found->type == previous[i].type) {
            const auto index = found - next.begin();
            result.indices[i] = static_cast<int>(index);
            result.triggers[index] = triggers[i];
        }
    }
    return result;
}

auto dspLinks(const std::vector<SchematicLink>& links, int node, int input)
{
    return links | std::views::filter([=](const auto& link) { return link.target == node && link.input == input; });
}

// The retirement input addresses one voice source.
const SchematicLink* retirementLink(const std::vector<SchematicLink>& links, int node)
{
    const auto found = std::ranges::find_if(links, [=](const auto& link) { return link.target == node && link.input == 1; });
    return found == links.end() ? nullptr : &*found;
}

bool isDspConstant(std::optional<Type> type)
{
    return type == Type::Float || type == Type::Int || type == Type::Boolean;
}

stream::SampleOperation operationFor(PrimitiveKind kind)
{
    using Operation = stream::SampleOperation;
    switch (kind) {
    case PrimitiveKind::Add: return Operation::Add;
    case PrimitiveKind::Subtract: return Operation::Subtract;
    case PrimitiveKind::Multiply: return Operation::Multiply;
    case PrimitiveKind::Divide: return Operation::Divide;
    case PrimitiveKind::Abs: return Operation::Absolute;
    case PrimitiveKind::Round: return Operation::Round;
    case PrimitiveKind::Floor: return Operation::Floor;
    case PrimitiveKind::Ceiling: return Operation::Ceiling;
    case PrimitiveKind::Truncate: return Operation::Truncate;
    case PrimitiveKind::Inverse:
    case PrimitiveKind::StreamInverse: return Operation::Inverse;
    case PrimitiveKind::Reciprocal:
    case PrimitiveKind::StreamReciprocal: return Operation::Reciprocal;
    case PrimitiveKind::StreamSquareRoot: return Operation::SquareRoot;
    case PrimitiveKind::Min:
    case PrimitiveKind::StreamMin: return Operation::Min;
    case PrimitiveKind::Max:
    case PrimitiveKind::StreamMax: return Operation::Max;
    case PrimitiveKind::Power: return Operation::Power;
    case PrimitiveKind::Logarithm: return Operation::Logarithm;
    case PrimitiveKind::Ln: return Operation::Ln;
    case PrimitiveKind::Log2: return Operation::Log2;
    case PrimitiveKind::Log10: return Operation::Log10;
    case PrimitiveKind::FrequencyToHz: return Operation::FrequencyToHz;
    case PrimitiveKind::FrequencyToPitch: return Operation::FrequencyToPitch;
    case PrimitiveKind::HzToFrequency: return Operation::HzToFrequency;
    case PrimitiveKind::HzToPitch: return Operation::HzToPitch;
    case PrimitiveKind::PitchToFrequency:
    case PrimitiveKind::StreamPitchToFreq:
    case PrimitiveKind::DoublePitchToFreq: return Operation::PitchToFrequency;
    case PrimitiveKind::PitchToHz: return Operation::PitchToHz;
    case PrimitiveKind::Sign:
    case PrimitiveKind::StreamSign: return Operation::Sign;
    case PrimitiveKind::StreamToDouble:
    case PrimitiveKind::DoubleToStream: return Operation::Convert;
    default: throw std::logic_error("Expected a Stream arithmetic primitive");
    }
}

const stream::DspProgram& rampProgram()
{
    static const auto program = stream::compileDspCode(R"(
        streamin frequency;
        streamboolin sync;
        streamout wave;
        float phase = 0;
        bool previousSync = false;
        bool rising;

        rising = sync & !previousSync;
        wave = rising ? 0.0 : phase;
        previousSync = sync;
        phase = wave + frequency * 0.5;
        phase = phase >= 1.0 ? phase - 1.0 : phase;
    )");
    return program;
}

const stream::DspProgram& impulseProgram()
{
    static const auto program = stream::compileDspCode(R"(
        streamout wave;
        float pending = 1;
        wave = pending;
        pending = 0;
    )");
    return program;
}

const stream::DspProgram& biquadProgram(bool wide)
{
    const auto compile = [](const std::string& connector, const std::string& scalar, std::string_view updateSecondDelay) {
        std::string source;
        for (const auto* name : {"signal", "a0", "a1", "a2", "b1", "b2"}) {
            source += connector + "in " + name + ";\n";
        }
        source += connector + "out filtered;\n";
        source += scalar + " d1 = 0;\n" + scalar + " d2 = 0;\n" + scalar + " w;\n";
        source += R"(
            w = (signal - b1 * d1) - b2 * d2;
            filtered = (a0 * w + a1 * d1) + a2 * d2;
        )";
        source += updateSecondDelay;
        source += "\n d1 = w;";
        return stream::compileDspCode(source);
    };
    if (wide) {
        static const auto program = compile("streamdouble", "double", "d2 = d1;");
        return program;
    }
    // FlowStone's single-precision filter guards the value sent to the second delay against denormals.
    static const auto program = compile("stream", "float", "d2 = (d1 + 1e-18) - 1e-18;");
    return program;
}

const stream::DspProgram& filterCoefficientsProgram(bool wide)
{
    const auto compile = [](const std::string& connector, const std::string& scalar) {
        std::string source = connector + "in cutoff;\n" + connector + "in resonance;\n";
        for (const auto* name : {"a0", "a1", "a2", "b1", "b2"}) {
            source += connector + "out " + name + ";\n";
        }
        for (const auto* name : {"angle", "k", "kSquared", "q", "weight", "damping", "undamped", "scale"}) {
            source += scalar + " " + name + ";\n";
        }

        // FlowStone uses the nearest entry of a 65536-point sine/cosine table
        // over a quarter turn. Preserve that cutoff resolution with portable math.
        source += R"(
            hop(4) {
                angle = rndint(min(max(cutoff, 0.01), 0.997) * 16384) / 65536;
                k = tan1(angle);
                kSquared = k * k;
                q = max(0, resonance);
                weight = min(1, q);
                damping = k / max(1, q);
                undamped = (1 + kSquared) * weight;
                scale = 1 / (undamped + damping);

                a0 = kSquared * weight * scale;
                a1 = 2 * a0;
                a2 = a0;
                b1 = 2 * (kSquared - 1) * weight * scale;
                b2 = q > 0 ? (undamped - damping) * scale : 0;
            }
        )";
        return stream::compileDspCode(source);
    };
    // For Q <= 1, multiply numerator and denominator by Q. This avoids K / Q
    // overflowing near zero and lets nonpositive resonance produce silence.
    if (wide) {
        static const auto program = compile("streamdouble", "double");
        return program;
    }
    static const auto program = compile("stream", "float");
    return program;
}

stream::DspProgram compileStreamTrig(const std::string& connector, const std::string& operation)
{
    // Reduce to one turn before evaluating to keep equivalent phases periodic.
    return stream::compileDspCode(connector + "in turns; " + connector + "out result; " +
                                  "result = " + operation + "(turns - trunc(turns));");
}

stream::DspProgram compileStreamComparison(const std::string& connector, const std::string& operation)
{
    return stream::compileDspCode(connector + "in a; " + connector + "in b; streamboolout result; " +
                                  "result = a " + operation + " b;");
}

stream::DspProgram compileStreamSelect(const std::string& connector)
{
    return stream::compileDspCode(connector + "in whenFalse; " + connector + "in whenTrue; " +
                                  "streamboolin select; " + connector + "out result; " +
                                  "result = select ? whenTrue : whenFalse;");
}

stream::DspProgram compileSampleAndHold(const std::string& connector, const std::string& scalar)
{
    return stream::compileDspCode(connector + "in signal; " + connector + "in control; " +
        connector + "out result; monoboolin zeroCross; " +
        scalar + " held = 0; " + scalar + " previous = -1; " + scalar + " phase = 1;" + R"(
        bool tick;
        bool crossed;
        bool capture;
        tick = phase >= 1;
        crossed = (previous < 0) & (control > 0);
        capture = zeroCross(0) ? crossed : tick;
        held = capture ? signal : held;
        result = held;
        phase = zeroCross(0) ? 1 : (phase - (tick ? 1 : 0)) + (control > 0 ? control : 0);
        previous = zeroCross(0) ? control : -1;
    )");
}

const stream::DspProgram* builtInStreamProgram(PrimitiveKind kind)
{
    using enum PrimitiveKind;
    switch (kind) {
    case Ramp: return &rampProgram();
    case Impulse: return &impulseProgram();
    case BiquadFilterCoeff: return &biquadProgram(false);
    case DoubleBiquadFilterCoeff: return &biquadProgram(true);
    case FilterCoefficients: return &filterCoefficientsProgram(false);
    case DoubleFilterCoefficients: return &filterCoefficientsProgram(true);
    case StreamSelect:
    case DoubleSelect: {
        static const auto single = compileStreamSelect("stream");
        static const auto wide = compileStreamSelect("streamdouble");
        return kind == StreamSelect ? &single : &wide;
    }
    case StreamSampleAndHold:
    case DoubleSampleAndHold: {
        static const auto single = compileSampleAndHold("stream", "float");
        static const auto wide = compileSampleAndHold("streamdouble", "double");
        return kind == StreamSampleAndHold ? &single : &wide;
    }
    case StreamSine:
    case DoubleStreamSine: {
        static const auto single = compileStreamTrig("stream", "sin1");
        static const auto wide = compileStreamTrig("streamdouble", "sin1");
        return kind == StreamSine ? &single : &wide;
    }
    case StreamCosine:
    case DoubleStreamCosine: {
        static const auto single = compileStreamTrig("stream", "cos1");
        static const auto wide = compileStreamTrig("streamdouble", "cos1");
        return kind == StreamCosine ? &single : &wide;
    }
    case StreamTangent:
    case DoubleStreamTangent: {
        static const auto single = compileStreamTrig("stream", "tan1");
        static const auto wide = compileStreamTrig("streamdouble", "tan1");
        return kind == StreamTangent ? &single : &wide;
    }
    case StreamEqualTo:
    case DoubleStreamEqualTo: {
        static const auto single = compileStreamComparison("stream", "==");
        static const auto wide = compileStreamComparison("streamdouble", "==");
        return kind == StreamEqualTo ? &single : &wide;
    }
    case StreamLess:
    case DoubleStreamLess: {
        static const auto single = compileStreamComparison("stream", "<");
        static const auto wide = compileStreamComparison("streamdouble", "<");
        return kind == StreamLess ? &single : &wide;
    }
    case StreamLessOrEqualTo:
    case DoubleStreamLessOrEqualTo: {
        static const auto single = compileStreamComparison("stream", "<=");
        static const auto wide = compileStreamComparison("streamdouble", "<=");
        return kind == StreamLessOrEqualTo ? &single : &wide;
    }
    case StreamGreater:
    case DoubleStreamGreater: {
        static const auto single = compileStreamComparison("stream", ">");
        static const auto wide = compileStreamComparison("streamdouble", ">");
        return kind == StreamGreater ? &single : &wide;
    }
    case StreamGreaterOrEqualTo:
    case DoubleStreamGreaterOrEqualTo: {
        static const auto single = compileStreamComparison("stream", ">=");
        static const auto wide = compileStreamComparison("streamdouble", ">=");
        return kind == StreamGreaterOrEqualTo ? &single : &wide;
    }
    default: return nullptr;
    }
}

std::optional<stream::TransportField> transportField(PrimitiveKind kind, int port)
{
    using enum PrimitiveKind;
    using Field = stream::TransportField;
    switch (kind) {
    case SamplePosition: case DoubleSamplePosition: return Field::Sample;
    case PpqPosition: case DoublePpqPosition: return Field::Quarter;
    case BarStartPosition: case DoubleBarStartPosition: return Field::BarStart;
    case StreamTempo: case DoubleTempo: return Field::Tempo;
    case StreamTimeSignature: case DoubleTimeSignature: return port == 0 ? Field::Numerator : Field::Denominator;
    case StreamIsPlaying: return Field::Playing;
    default: return std::nullopt;
    }
}

std::optional<stream::WaveOperation> waveOperation(PrimitiveKind kind)
{
    switch (kind) {
    case PrimitiveKind::WaveRead: return stream::WaveOperation::Read;
    case PrimitiveKind::WaveReadHop: return stream::WaveOperation::ReadHop;
    case PrimitiveKind::WaveArrayRead: return stream::WaveOperation::ReadArray;
    case PrimitiveKind::WaveTableRead: return stream::WaveOperation::ReadTable;
    case PrimitiveKind::WaveWriteMono: return stream::WaveOperation::WriteMono;
    case PrimitiveKind::WaveWriteStereo: return stream::WaveOperation::WriteStereo;
    default: return std::nullopt;
    }
}

double constantSample(const green::Value& value)
{
    if (const auto* number = std::get_if<float>(&value)) {
        return *number;
    }
    if (const auto* number = std::get_if<std::int32_t>(&value)) {
        return *number;
    }
    if (const auto* flag = std::get_if<bool>(&value)) {
        return *flag ? 1.0 : 0.0;
    }
    throw std::invalid_argument("A DSP constant input needs a Float, Int or Boolean");
}

}

std::optional<Type> numericStreamFamily(Type type)
{
    const auto* family = streamFamily(type);
    if (!family || family->generic == Type::StreamBoolean) return {};
    return family->generic;
}

Type dspConnectorType(stream::DspPortType type)
{
    using Port = stream::DspPortType;
    switch (type) {
    case Port::Stream: return Type::Stream;
    case Port::Mono: return Type::Mono;
    case Port::Poly: return Type::Poly;
    case Port::StreamBoolean: return Type::StreamBoolean;
    case Port::MonoBoolean: return Type::MonoBoolean;
    case Port::PolyBoolean: return Type::PolyBoolean;
    case Port::StreamDouble: return Type::StreamDouble;
    case Port::MonoDouble: return Type::MonoDouble;
    case Port::PolyDouble: return Type::PolyDouble;
    case Port::PolyInt: return Type::PolyInt;
    case Port::Mem: case Port::MemRef: return Type::Mem;
    case Port::MemArrayRef: return Type::MemArray;
    case Port::FloatArray: return Type::FloatArray;
    }
    throw std::logic_error("Unknown DSP port type");
}

bool canSupplyDspInput(Type source, Type input)
{
    return isDspConstant(source) &&
           green::isDspType(input);
}

void Schematic::resolveDspStreams(RuntimeBuilds& builds)
{
    using ConnectorType = std::optional<Type>;
    struct TemplatePort {
        std::vector<ConnectorType*> neighbours;
        std::optional<std::size_t> mode;
        bool visited = false;
    };
    std::map<ConnectorType*, TemplatePort> templates;
    const auto constrain = [](std::optional<std::size_t>& current, std::size_t mode) {
        if (current && *current != mode) {
            throw std::invalid_argument("A Stream section cannot use conflicting Mono, Poly or Mono4 types");
        }
        current = mode;
    };
    const auto join = [&](ConnectorType& a, ConnectorType& b) {
        templates[&a].neighbours.push_back(&b);
        templates[&b].neighbours.push_back(&a);
    };
    const auto wire = [&](ConnectorType& a, ConnectorType& b) {
        if (!a || !b) {
            return;
        }
        const bool genericA = green::isGenericDspStream(*a);
        const bool genericB = green::isGenericDspStream(*b);
        if ((!genericA && !genericB) || streamFamily(*a) != streamFamily(*b)) {
            return;
        }
        if (genericA && genericB) {
            join(a, b);
        } else {
            auto& generic = genericA ? a : b;
            const auto fixed = genericA ? b : a;
            const auto& modes = streamFamily(*generic)->modes;
            const auto mode = std::ranges::find(modes, fixed) - modes.begin();
            constrain(templates[&generic].mode, static_cast<std::size_t>(mode));
        }
    };

    for (auto& [schematic, build] : builds) {
        for (const auto& entry : schematic->nodes_) {
            auto& ports = build.ports.at(entry.id).types;
            const auto& spec = primitive(entry.kind);
            if (entry.dspCode || supportsStreamMath(spec) || spec.behavior == PrimitiveBehavior::StreamConversion ||
                spec.behavior == PrimitiveBehavior::StreamRouting || spec.behavior == PrimitiveBehavior::MatrixRouting ||
                isIndexedRouter(entry.kind)) {
                ConnectorType* first = nullptr;
                const auto joinGenericPorts = [&](auto& direction) {
                    for (auto& type : direction) {
                        if (!type || !green::isGenericDspStream(*type)) {
                            continue;
                        }
                        if (first) {
                            join(*first, type);
                        } else {
                            first = &type;
                        }
                    }
                };
                joinGenericPorts(ports.inputs);
                joinGenericPorts(ports.outputs);
            }
            if (isModuleEndpoint(entry.kind)) {
                if (const auto parent = builds.find(schematic->parent_); parent != builds.end()) {
                    auto& exterior = parent->second.ports.at(schematic->path_.back()).types;
                    if (entry.kind == PrimitiveKind::ModuleInput) {
                        wire(exterior.inputs.at(entry.boundaryIndex), ports.outputs.front());
                    } else {
                        wire(ports.inputs.front(), exterior.outputs.at(entry.boundaryIndex));
                    }
                }
            }
        }
        for (const auto& link : schematic->links_) {
            wire(build.ports.at(link.source).types.outputs.at(link.output),
                 build.ports.at(link.target).types.inputs.at(link.input));
        }
    }

    std::vector<ConnectorType*> section;
    for (auto& [start, port] : templates) {
        if (port.visited) {
            continue;
        }
        std::optional<std::size_t> mode;
        section.assign(1, start);
        port.visited = true;
        for (std::size_t i = 0; i < section.size(); ++i) {
            auto& member = templates.at(section[i]);
            if (member.mode) {
                constrain(mode, *member.mode);
            }
            for (auto* neighbour : member.neighbours) {
                if (auto& next = templates.at(neighbour); !next.visited) {
                    next.visited = true;
                    section.push_back(neighbour);
                }
            }
        }
        if (mode) {
            for (auto* type : section) {
                *type = streamFamily(**type)->modes[*mode].value_or(**type);
            }
        }
    }
}

void Schematic::setDspDraft(int id, std::string source)
{
    auto& code = node(id).dspCode;
    if (!code) {
        throw std::invalid_argument("Choose a DSP Code node");
    }
    code->draft = std::move(source);
}

void Session::compileDsp(int id)
{
    const auto& code = graph().node(id).dspCode;
    if (!code) {
        throw std::invalid_argument("Choose a DSP Code node");
    }
    std::vector<std::pair<Schematic*, int>> targets;
    for (const auto& target : editTargets({id})) {
        targets.emplace_back(target.graph, target.ids.front());
    }
    Schematic::applyDspCode(targets, code->draft);
}

void Schematic::applyDspCode(std::span<const std::pair<Schematic*, int>> targets, std::string source)
{
    const auto& [firstGraph, firstId] = targets.front();
    const auto program = stream::compileDspCode(source, dspLanguage(firstGraph->node(firstId).kind));

    struct Update {
        Schematic& graph;
        SchematicNode& entry;
        DspCodeState code;
        std::vector<std::uint64_t> inputs;
        std::vector<std::uint64_t> outputs;
        std::vector<SchematicLink> links;
        std::map<SchematicLink, std::uint64_t> counts;

        void swap()
        {
            std::swap(*entry.dspCode, code);
            entry.inputTriggers.swap(inputs);
            entry.outputTriggers.swap(outputs);
            graph.links_.swap(links);
            graph.linkTriggers_.swap(counts);
        }
    };
    std::vector<Update> updates;
    for (const auto& [graph, id] : targets) {
        auto& entry = graph->node(id);
        const auto& previous = entry.dspCode->program;
        auto inputs = remapPorts(previous.inputs(), program.inputs(), entry.inputTriggers);
        auto outputs = remapPorts(previous.outputs(), program.outputs(), entry.outputTriggers);
        if (entry.codeSourcePorts) {
            outputs.indices.push_back(static_cast<int>(program.outputs().size()));
            outputs.triggers.push_back(entry.outputTriggers.back());
        }
        Update update{*graph, entry, {source, source, program}, std::move(inputs.triggers),
                      std::move(outputs.triggers), {}, {}};
        for (auto link : graph->links_) {
            const auto original = link;
            if (link.target == id) {
                link.input = inputs.indices.at(link.input);
            }
            if (link.source == id) {
                link.output = outputs.indices.at(link.output);
            }
            if (link.input >= 0 && link.output >= 0) {
                update.links.push_back(link);
                update.counts.emplace(link, graph->linkTriggers_.at(original));
            }
        }
        updates.push_back(std::move(update));
    }

    auto& root = targets.front().first->rootGraph();
    for (auto& update : updates) {
        update.swap();
    }
    try {
        for (const auto& [graph, id] : targets) {
            for (const auto& link : graph->links_) {
                if (link.target == id && graph->wouldCreateFeedback(link)) {
                    throw std::invalid_argument("The DSP change would create immediate feedback. Use a delay or staged DSP state");
                }
            }
        }
        root.rebuildConnectedRuntimes(true, true);
    } catch (...) {
        for (auto& update : updates) {
            update.swap();
        }
        throw;
    }
    ++root.drawingTopology_;
    for (const auto& [graph, id] : targets) {
        auto& entry = graph->node(id);
        if (const auto port = entry.codeSourcePort(true)) graph->fireOutput(entry, *port);
    }
    root.refreshCodeSources();
}

std::optional<Schematic::DspSource> Schematic::resolveDspSource(int id, int output, const Schematic* host) const
{
    const Schematic* graph = this;
    std::set<DspTraceKey> visited;
    for (;;) {
        const auto& entry = graph->node(id);
        const bool module = entry.kind == PrimitiveKind::Module;
        if (isDspConstant(graph->outputType(id, output)) ||
            (!module && (entry.kind != PrimitiveKind::ModuleInput || graph == host))) {
            return DspSource{graph, id, output};
        }
        if (!visited.emplace(graph->path_, id, output).second) {
            // An undriven ring of module passthroughs has no signal to mix into the graph.
            return std::nullopt;
        }
        const int target = module ? entry.moduleOutputs.at(output).endpoint : graph->path_.back();
        const int input = module ? 0 : entry.boundaryIndex;
        graph = module ? graph->module(id) : graph->parent_;
        auto links = dspLinks(graph->links_, target, input);
        const auto first = links.begin();
        if (first == links.end()) return std::nullopt;
        if (std::next(first) != links.end()) return DspSource{graph, target, input, true};
        id = first->source;
        output = first->output;
    }
}

std::optional<Schematic::DspTraceKey> Schematic::dspTraceKey(int id, int port, bool output) const
{
    if (!output) {
        auto links = dspLinks(links_, id, port);
        const auto first = links.begin();
        if (first == links.end()) return std::nullopt;
        if (std::next(first) != links.end()) return DspTraceKey{path_, id, port, true};
        id = first->source;
        port = first->output;
    }
    const auto source = resolveDspSource(id, port);
    if (!source) {
        return std::nullopt;
    }
    return DspTraceKey{source->graph->path_, source->node, source->port, source->input};
}

std::span<const double> Schematic::dspTrace(int id, int port, bool output) const
{
    const auto& traces = rootGraph().dspTraces_;
    const auto key = traces.empty() ? std::nullopt : dspTraceKey(id, port, output);
    const auto found = key ? traces.find(*key) : traces.end();
    return found == traces.end() ? std::span<const double>{} : found->second;
}

std::shared_ptr<const Schematic::AudioMonitor> Schematic::audioMonitor() const
{
    const auto& root = rootGraph();
    auto monitor = root.liveAudio_.lock();
    return monitor && monitor->topology == root.audioTopology_ ? std::move(monitor) : nullptr;
}

std::optional<std::vector<double>> Schematic::liveDspTrace(int id, int port, bool output) const
{
    return liveDspTrace(id, port, output, swatchFrames);
}

std::optional<std::vector<double>> Schematic::liveDspTrace(int id, int port, bool output, std::size_t frames) const
{
    const auto monitor = audioMonitor();
    if (!monitor) {
        return std::nullopt;
    }
    const auto key = dspTraceKey(id, port, output);
    const auto found = key ? monitor->probes.find(*key) : monitor->probes.end();
    if (found == monitor->probes.end()) {
        return std::nullopt;
    }
    const std::array<stream::AudioProcessor::ProbeIndex, 1> probes{found->second};
    auto window = monitor->processor->readHistory(probes, frames);
    return window ? std::move(window->front()) : std::vector<double>{};
}

std::optional<std::vector<std::vector<double>>> Schematic::captureLiveMono(int id, int channels, std::size_t frames) const
{
    const auto monitor = audioMonitor();
    if (!monitor) {
        return std::nullopt;
    }
    std::vector<stream::AudioProcessor::ProbeIndex> probes(channels);
    bool anyLive = false;
    bool anyInactive = false;
    for (int channel = 0; channel < channels; ++channel) {
        if (const auto key = dspTraceKey(id, channel, false)) {
            if (const auto found = monitor->probes.find(*key); found != monitor->probes.end()) {
                probes[channel] = found->second;
                anyLive = true;
            } else {
                anyInactive = true;
            }
        }
    }
    if (!anyLive) {
        return std::nullopt;
    }
    if (anyInactive) {
        throw std::invalid_argument("For a live Mono to Graph capture, route each connected signal to Audio Out");
    }
    auto samples = monitor->processor->readHistory(probes, frames);
    if (!samples) {
        throw std::runtime_error("Could not read a complete live audio window. Try Get again.");
    }
    for (auto& channel : *samples) {
        channel.insert(channel.begin(), frames - channel.size(), 0.0);
    }
    return samples;
}

std::optional<std::int32_t> Schematic::voiceIndex(int id) const
{
    return hasValueInput(id, 1) ? std::optional{std::get<std::int32_t>(readInput(id, 1))} : std::nullopt;
}

std::vector<std::vector<double>> Schematic::captureDsp(int id, std::size_t frames, std::size_t blockSize) const
{
    if (!node(id).dspCode) {
        throw std::invalid_argument("Choose a DSP Code node");
    }
    return renderDsp(id, frames, blockSize, false);
}

void Schematic::captureSignals(SchematicNode& entry)
{
    const auto ns = static_cast<int>(entry.inputTriggers.size()) - 2;
    const auto count = std::get<std::int32_t>(readInput(entry.id, ns));
    if (count < 1 || count > maxDspCaptureFrames) {
        throw std::invalid_argument("NS must be between 1 and 16384 samples");
    }

    const bool analyse = entry.kind == PrimitiveKind::SignalAnalyser;
    const auto frames = static_cast<std::size_t>(count);
    std::string code;
    green::FloatArray timings(analyse ? frames : 0);
    auto live = analyse ? std::nullopt : captureLiveMono(entry.id, ns, frames);
    const auto samples = live ? std::move(*live)
                              : renderDsp(entry.id, frames, analyse ? 1 : std::min(frames, std::size_t{256}),
                                          true, analyse ? &code : nullptr, timings);
    std::vector<green::Value> outputs;
    outputs.reserve(entry.outputTriggers.size());
    for (const auto& channel : samples) {
        outputs.emplace_back(green::FloatArray(channel.begin(), channel.end()));
    }
    if (analyse) {
        outputs.emplace_back(std::move(code));
        outputs.emplace_back(std::move(timings));
    }
    entry.capturedSignals = std::move(outputs);
    fireOutputs(entry.id);
}

void Schematic::FloatArrayReadBinding::refresh() const
{
    const auto array = std::get<green::FloatArray>(graph->readInput(node, 0));
    table->publish(array);
}

void Schematic::WaveBufferBinding::refresh() const
{
    const auto data = graph->readInput(node, 0);
    if (const auto* memory = std::get_if<green::Memory>(&data)) buffer->writable() = *memory;
    else buffer->writable() = std::get<green::MemoryArray>(data);
    buffer->publish();
}

void Schematic::MatrixRouteBinding::refresh()
{
    const auto& entry = graph->node(node);
    const auto current = entry.matrix ? entry.matrix->revision : 0;
    if (current == revision) return;

    stream::RouteMixTable::Routes routes(entry.outputTriggers.size());
    if (entry.matrix) {
        for (const auto& connection : entry.matrix->connections) {
            const auto input = std::ranges::lower_bound(wiredInputs, connection.input);
            if (input != wiredInputs.end() && *input == connection.input) {
                routes[connection.output].push_back(input - wiredInputs.begin());
            }
        }
    }
    table->publish(std::move(routes));
    revision = current;
}

namespace {

class SequencerLane final : public stream::SampleProcessor {
public:
    SequencerLane(std::shared_ptr<stream::SequenceAudioTrack> track,
                  std::shared_ptr<stream::SequenceTransport> transport, std::size_t lane)
        : track_(std::move(track)), transport_(std::move(transport)), lane_(lane) {}

    void beginBlock(std::uint64_t) noexcept override
    {
        position_ = transport_->position();
        track_->latch(*transport_);
    }

    double process(std::span<const double> inputs) noexcept override
    {
        return track_->process(*transport_, position_++, lane_, static_cast<float>(inputs[0]), inputs[1]);
    }

private:
    std::shared_ptr<stream::SequenceAudioTrack> track_;
    std::shared_ptr<stream::SequenceTransport> transport_;
    std::size_t lane_;
    std::size_t position_{};
};

struct MediaReadState {
    std::shared_ptr<MediaAudioBuffer> buffer;
    std::optional<std::uint64_t> frame;
    MediaAudioBuffer::Frame samples{};
};

class MediaChannelReader final : public stream::SampleProcessor {
public:
    MediaChannelReader(std::shared_ptr<MediaReadState> state, std::size_t channel)
        : state_(std::move(state)), channel_(channel) {}

    void beginBlock(std::uint64_t frame) noexcept override { next_ = frame; }

    void reset() noexcept override { state_->frame.reset(); }

    double process(std::span<const double>) noexcept override
    {
        if (state_->frame != next_) {
            state_->samples = state_->buffer->read();
            state_->frame = next_;
        }
        ++next_;
        return state_->samples[channel_];
    }

private:
    std::shared_ptr<MediaReadState> state_;
    std::size_t channel_;
    std::uint64_t next_{};
};

}

struct Schematic::DspRenderGraph {
    stream::Graph graph{stream::defaultDspBackend(), stream::FeedbackPolicy::ImplicitDelay};
    std::vector<stream::NodeId> outputs;
    std::map<DspTraceKey, stream::NodeId> signals;
    using Input = std::variant<DspSource, stream::AudioChannel, stream::HostAudioChannel, const Schematic*, stream::TransportInput>;
    std::vector<Input> bindings;
    std::map<DspTraceKey, stream::NodeId> frameSources;
    std::size_t additionalOutputs = 0;
    bool packed = false;
    AudioApi inputApi = AudioApi::Any;
    AudioApi outputApi = AudioApi::Any;
    struct ArrayInput {
        stream::DspBlockId block;
        std::size_t port;
        DspSource input;
        std::size_t sampleLimit;
        bool reference;
    };
    std::vector<ArrayInput> arrays;
    std::vector<FloatArrayReadBinding> arrayReaders;
    std::vector<WaveBufferBinding> waveBuffers;
    std::vector<std::pair<DspSource, bool>> bridges;
    std::map<std::pair<const Schematic*, int>, int> streamRoutes;
    std::map<DspTraceKey, MatrixRouteBinding> matrixRoutes;

    static double readParameter(const DspSource& source)
    {
        const auto& [level, node, port, input, midiChannel] = source;
        if (input) {
            if (level->inputType(node, port) == Type::Trigger) return level->node(node).inputTriggers.at(port);
            return constantSample(level->readInput(node, port));
        }
        if (isDspConstant(level->outputType(node, port))) return constantSample(level->readValue(node, port));
        return level->readMidiStream(level->node(node), midiChannel);
    }

    static double readConstant(const Input& binding)
    {
        if (const auto* rateOwner = std::get_if<const Schematic*>(&binding)) return (*rateOwner)->sampleRate();
        if (const auto* input = std::get_if<stream::TransportInput>(&binding)) return input->read({}, 0);
        return readParameter(std::get<DspSource>(binding));
    }

    static void supplyArrays(auto& runtime, std::span<const ArrayInput> arrays)
    {
        for (const auto& [block, port, input, sampleLimit, reference] : arrays) {
            const auto value = input.graph->readInput(input.node, input.port);
            if (const auto* memory = std::get_if<green::MemoryArray>(&value)) {
                if (!runtime.setDspMemArrayInput(block, port, *memory)) {
                    throw std::logic_error("Could not supply the DSP MemArray input");
                }
                continue;
            }
            green::FloatArray decoded;
            std::span<const float> samples;
            if (const auto* memory = std::get_if<green::Memory>(&value)) {
                if (reference) {
                    if (!runtime.setDspMemRefInput(block, port, *memory)) {
                        throw std::logic_error("Could not supply the DSP Mem reference");
                    }
                    continue;
                }
                decoded.resize(std::min(sampleLimit, memory->size() / sizeof(float)));
                for (std::size_t i = 0; i < decoded.size(); ++i) decoded[i] = memory->sample(i);
                samples = decoded;
            } else {
                samples = std::get<green::FloatArray>(value);
            }
            if (!runtime.setDspArrayInput(block, port, samples)) {
                throw std::logic_error("Could not supply the DSP array input");
            }
        }
    }
};

Schematic::DspRenderGraph Schematic::buildDspGraph(int id, std::size_t portCount, DspRender render) const
{
    DspRenderGraph result;
    struct PolySection {
        std::vector<stream::NodeId> external;
        std::vector<stream::PolyBinding> inputs;
        std::shared_ptr<stream::VoiceBank> bank;
        std::optional<DspSource> release;
        struct EnvelopeEnd {
            const Schematic* graph;
            int node;
            stream::NodeId hold;
        };
        std::vector<EnvelopeEnd> envelopes;
        bool voiceSource{};
    };
    std::map<const stream::VoiceBank*, std::shared_ptr<stream::VoiceSnapshot>> voiceSnapshots;
    std::map<std::size_t, stream::NodeId> deviceInputs;
    std::map<std::size_t, stream::NodeId> hostInputs;
    const auto* host = render == DspRender::Plugin ? module(id) : nullptr;
    const bool liveRouting = render == DspRender::Audio || render == DspRender::Plugin;
    std::optional<stream::NodeId> sampleRateInput;

    const auto audioRate = [&] {
        if (!sampleRateInput) {
            sampleRateInput = result.graph.addInput();
            result.bindings.emplace_back(&rootGraph());
        }
        return *sampleRateInput;
    };
    const auto control = [&](const Schematic* level, int id, int port) {
        const auto input = result.graph.addInput();
        result.bindings.emplace_back(DspSource{level, id, port, true});
        return input;
    };
    const auto implicitBank = [&]() -> std::shared_ptr<stream::VoiceBank> {
        std::shared_ptr<stream::VoiceBank> bank;
        const auto visit = [&](const auto& self, const Schematic& level) -> void {
            for (const auto& entry : level.nodes_) {
                if (entry.kind != PrimitiveKind::Voices) continue;
                if (bank) throw std::invalid_argument("Wire Voices to Poly into the section to select its voice group");
                bank = std::get<green::Voices>(level.readVoiceRouting(entry)).bank;
            }
            for (const auto& [id, child] : level.modules_) self(self, *child);
        };
        visit(visit, rootGraph());
        return bank;
    };
    const auto addInput = [](DspRenderGraph& graph, PolySection& section, stream::PolyBinding input) {
        section.inputs.push_back(input);
        return graph.graph.addInput();
    };
    const auto addExternal = [](PolySection& section, stream::NodeId source) {
        const auto index = section.external.size();
        section.external.push_back(source);
        return index;
    };
    const auto addInputMix = [](DspRenderGraph& patch, const Schematic& level, int id, int port) {
        const auto type = level.inputType(id, port);
        const auto* family = type ? streamFamily(*type) : nullptr;
        const auto operation = family && family->generic == Type::StreamBoolean
            ? stream::SampleOperation::Max : stream::SampleOperation::Add;
        const auto precision = family && family->generic == Type::Stream
            ? stream::SamplePrecision::Float32 : stream::SamplePrecision::Float64;
        const auto mix = patch.graph.addInputMix(operation, precision);
        patch.signals.emplace(DspTraceKey{level.path_, id, port, true}, mix);
        return mix;
    };
    std::function<std::optional<stream::NodeId>(DspRenderGraph&, PolySection*, const Schematic&, int, int)> resolveOutput;
    const auto resolveInput = [&](DspRenderGraph& current, PolySection* poly, const Schematic& level,
                                  int id, int port) -> std::optional<stream::NodeId> {
        auto links = dspLinks(level.links_, id, port);
        const auto first = links.begin();
        if (first == links.end()) return std::nullopt;
        if (std::next(first) == links.end()) return resolveOutput(current, poly, level, first->source, first->output);

        const DspTraceKey key{level.path_, id, port, true};
        if (const auto found = current.signals.find(key); found != current.signals.end()) return found->second;

        // Reserve the mix before walking sources that can feed back through a module.
        const auto mix = addInputMix(current, level, id, port);
        for (const auto& link : links) {
            if (const auto source = resolveOutput(current, poly, level, link.source, link.output)) {
                current.graph.connect(*source, mix);
            }
        }
        return mix;
    };
    const auto selectedInput = [&](const Schematic& level, const SchematicNode& entry, int output) {
        const auto key = std::pair{&level, entry.id};
        auto [found, inserted] = result.streamRoutes.try_emplace(key);
        if (inserted) found->second = level.routerSelection(entry);
        return routedDataInput(entry.kind, found->second, output);
    };
    const auto managedPoly = [&](const Schematic& level, int node, int port, bool input = false) {
        std::set<DspTraceKey> visited;
        const auto visit = [&](const auto& self, const Schematic& level, int id, int port, bool input) -> bool {
            const auto source = input ? std::optional{DspSource{&level, id, port, true}}
                                      : level.resolveDspSource(id, port, host);
            if (!source || !visited.emplace(source->graph->path_, source->node, source->port, source->input).second) {
                return false;
            }
            if (source->input) {
                return std::ranges::any_of(dspLinks(source->graph->links_, source->node, source->port),
                    [&](const auto& wire) { return self(self, *source->graph, wire.source, wire.output, false); });
            }
            const auto& entry = source->graph->node(source->node);
            if (primitive(entry.kind).behavior == PrimitiveBehavior::VoiceSignal ||
                (entry.kind == PrimitiveKind::MidiCCStream && source->graph->outputType(entry.id) == Type::Poly)) {
                return true;
            }
            if (isPolyBridge(entry.kind)) return false;
            if (isIndexedRouter(entry.kind)) {
                const auto dataInput = selectedInput(*source->graph, entry, source->port);
                return dataInput && self(self, *source->graph, entry.id, *dataInput, true);
            }
            const auto routes = entry.kind == PrimitiveKind::Matrix && !liveRouting
                ? source->graph->matrixConnections(entry) : std::vector<MatrixConnection>{};
            for (const auto& wire : source->graph->links_) {
                if (wire.target != source->node) continue;
                if (entry.kind == PrimitiveKind::Matrix && !liveRouting &&
                    !std::ranges::contains(routes, MatrixConnection{wire.input, source->port})) continue;
                const auto type = source->graph->inputType(wire.target, wire.input);
                if (isPolySignal(type) && self(self, *source->graph, wire.source, wire.output, false)) return true;
            }
            return false;
        };
        return visit(visit, level, node, port, input);
    };
    resolveOutput = [&](DspRenderGraph& current, PolySection* poly, const Schematic& schematic,
                        int id, int output) -> std::optional<stream::NodeId> {
        auto& graph = current.graph;
        auto& signals = current.signals;
        const auto origin = schematic.resolveDspSource(id, output, host);
        if (!origin) {
            return std::nullopt;
        }
        const auto* level = origin->graph;
        const int nodeId = origin->node;
        const int port = origin->port;
        if (origin->input) return resolveInput(current, poly, *level, nodeId, port);
        const auto& entry = level->node(nodeId);
        const auto type = level->outputType(nodeId, port);
        current.packed |= isPackedStream(type);
        const DspTraceKey address{level->path_, nodeId, port};
        if (const auto found = signals.find(address); found != signals.end()) {
            return found->second;
        }
        const auto& spec = primitive(entry.kind);
        using enum PrimitiveKind;

        const bool managedInput = isPolyBridge(entry.kind) && entry.kind != MonoToPoly &&
                                  managedPoly(*level, nodeId, 0, true);
        const bool reducer = entry.kind == PolyToMonoMin || entry.kind == PolyToMonoMax ||
                             entry.kind == PolyToMonoHop || entry.kind == PolyToMonoSelective ||
                             (entry.kind == PolyToMono && managedInput);
        const bool managedSignal = !poly && isPolySignal(type) && managedPoly(*level, nodeId, port);
        if (!poly && (reducer || managedSignal)) {
            // Reserve the output before resolving Mono inputs that may feed it back.
            const auto sink = graph.addSum();
            signals.emplace(address, sink);
            DspRenderGraph voiceGraph;
            PolySection section;
            const auto source = reducer ? resolveInput(voiceGraph, &section, *level, nodeId, 0)
                                        : resolveOutput(voiceGraph, &section, *level, nodeId, port);
            if (!source) return sink;
            voiceGraph.outputs.push_back(*source);
            std::optional<stream::NodeId> releaseMix;
            const auto addRelease = [&](stream::NodeId signal) {
                if (!releaseMix) releaseMix = voiceGraph.graph.addSum();
                voiceGraph.graph.connect(signal, *releaseMix);
            };
            if (section.release) {
                const auto& end = *section.release;
                if (const auto endSignal = resolveOutput(voiceGraph, &section, *end.graph, end.node, end.port)) {
                    addRelease(*endSignal);
                }
            }
            for (std::size_t i = 0; i < section.envelopes.size(); ++i) {
                const auto [owner, envelope, hold] = section.envelopes[i];
                static const auto program = stream::compileDspCode(
                    "polyboolin end; polyin hold; polyboolout done; done = (hold == 0) | end;");
                const auto stop = voiceGraph.graph.addDspCode(program);
                voiceGraph.graph.connectDspInput(hold, stop.id, 1, stream::DspInputWidth::Scalar);
                if (const auto signal = resolveInput(voiceGraph, &section, *owner, envelope, 0)) {
                    voiceGraph.graph.connectDspInput(*signal, stop.id, 0, stream::DspInputWidth::Scalar);
                }
                addRelease(stop.outputs.front());
            }
            if (releaseMix) voiceGraph.outputs.push_back(*releaseMix);
            const auto sampleRate = addExternal(section, audioRate());
            std::optional<std::size_t> selection;
            auto mix = stream::PolyMix::Sum;
            if (entry.kind == PolyToMonoMin) mix = stream::PolyMix::Minimum;
            if (entry.kind == PolyToMonoMax) mix = stream::PolyMix::Maximum;
            if (entry.kind == PolyToMonoHop) {
                mix = stream::PolyMix::Hop;
                selection = addExternal(section, control(level, nodeId, 1));
            }
            if (entry.kind == PolyToMonoSelective) {
                mix = stream::PolyMix::SelectGroup;
                const auto source = resolveInput(result, nullptr, *level, nodeId, 1);
                selection = addExternal(section, source ? *source : graph.addSum());
            }
            if (entry.kind == PolyToMono && level->voiceIndex(nodeId)) {
                selection = addExternal(section, control(level, nodeId, 1));
            }
            const bool managed = section.voiceSource || managedInput || managedSignal;
            if (!section.voiceSource && managed) section.bank = implicitBank();
            std::shared_ptr<stream::VoiceSnapshot> snapshot;
            if (section.bank) {
                auto& shared = voiceSnapshots[section.bank.get()];
                if (!shared) shared = std::make_shared<stream::VoiceSnapshot>(section.bank);
                snapshot = shared;
            }
            const auto processor = graph.addProcessor(section.external.size(),
                [voiceGraph = std::move(voiceGraph), inputs = std::move(section.inputs), snapshot = std::move(snapshot),
                 managed, mix, sampleRate, selection] {
                    auto renderer = std::make_unique<stream::PolyRenderer>(voiceGraph.graph, voiceGraph.outputs,
                        inputs, snapshot, managed, mix, sampleRate, selection);
                    DspRenderGraph::supplyArrays(*renderer, voiceGraph.arrays);
                    return renderer;
                });
            for (const auto input : section.external) graph.connect(input, processor);
            graph.connect(processor, sink);
            return sink;
        }

        if (const auto field = transportField(entry.kind, port)) {
            const bool wide = spec.outputs[port].type == Type::StreamDouble;
            const auto input = result.graph.addInput();
            result.bindings.emplace_back(stream::TransportInput{
                *field, wide ? stream::SamplePrecision::Float64 : stream::SamplePrecision::Float32});
            const auto signal = poly ? addInput(current, *poly, {stream::PolyValue::External, addExternal(*poly, input)}) : input;
            signals.emplace(address, signal);
            return signal;
        }

        if (poly) {
            if (entry.kind == EnvelopeControl) {
                static const auto program = stream::compileDspCode(R"(
                    polyboolin gate; polyin trigger; polyin hold; polyout stageValue;
                    float previous;
                    stageValue = gate ? (previous != trigger ? 1 : 3) : (hold > 0 ? 4 : 0);
                    previous = trigger;
                )");
                const auto stage = graph.addDspCode(program);
                signals.emplace(address, stage.outputs.front());
                const auto gate = addInput(current, *poly, {stream::PolyValue::Gate});
                const auto trigger = addInput(current, *poly, {stream::PolyValue::Trigger});
                const auto hold = addInput(current, *poly,
                    {stream::PolyValue::External, addExternal(*poly, control(level, nodeId, 1))});
                graph.connectDspInput(gate, stage.id, 0, stream::DspInputWidth::Scalar);
                graph.connectDspInput(trigger, stage.id, 1, stream::DspInputWidth::Scalar);
                graph.connectDspInput(hold, stage.id, 2, stream::DspInputWidth::Scalar);
                poly->envelopes.push_back({level, nodeId, hold});
                return stage.outputs.front();
            }
            std::optional<stream::NodeId> signal;
            if (entry.kind == MonoToPoly) {
                const auto source = resolveInput(result, nullptr, *level, nodeId, 0);
                const auto external = addExternal(*poly, source ? *source : result.graph.addSum());
                std::optional<std::size_t> index;
                if (level->voiceIndex(nodeId)) index = addExternal(*poly, control(level, nodeId, 1));
                signal = addInput(current, *poly, {stream::PolyValue::Mono, external, index});
            } else if (entry.kind == MidiCCStream && type == Type::Poly) {
                const auto first = poly->external.size();
                for (std::size_t channel = 0; channel < 16; ++channel) {
                    const auto parameter = result.graph.addInput();
                    result.bindings.emplace_back(DspSource{level, nodeId, port, false, channel});
                    addExternal(*poly, parameter);
                }
                signal = addInput(current, *poly, {stream::PolyValue::ChannelController, first});
            } else if (spec.behavior == PrimitiveBehavior::VoiceSignal) {
                auto metadata = stream::PolyBinding{.value = stream::PolyValue::Channel};
                if (entry.kind == VoicesToPoly) {
                    constexpr std::array kinds{stream::PolyValue::Frequency, stream::PolyValue::Pitch,
                                               stream::PolyValue::Velocity, stream::PolyValue::Gate, stream::PolyValue::Trigger};
                    metadata.value = kinds.at(port);
                    const auto bank = std::get<green::Voices>(level->readInput(nodeId, 0)).bank;
                    entry.voiceBank = bank;
                    if (poly->voiceSource && poly->bank != bank) {
                        throw std::invalid_argument("Mix each Voices group to Mono before combining their streams");
                    }
                    poly->voiceSource = true;
                    poly->bank = bank;
                    if (const auto* release = retirementLink(level->links_, nodeId)) {
                        poly->release = DspSource{level, release->source, release->output};
                    }
                } else if (entry.kind == PolyVoiceId) metadata.value = stream::PolyValue::Slot;
                else if (entry.kind == PolyNoteId) metadata.value = stream::PolyValue::Note;
                else if (entry.kind == PolyTag) metadata = {stream::PolyValue::Tag, addExternal(*poly, control(level, nodeId, 0))};
                signal = addInput(current, *poly, metadata);
            } else if (isDspConstant(type) || type == Type::Mono || type == Type::MonoDouble || type == Type::MonoBoolean) {
                const auto upstream = resolveOutput(result, nullptr, *level, nodeId, port);
                signal = addInput(current, *poly, {stream::PolyValue::External, addExternal(*poly, *upstream)});
            }
            if (signal) {
                signals.emplace(address, *signal);
                return signal;
            }
        }

        if (isDspConstant(type) || spec.behavior == PrimitiveBehavior::MidiStream) {
            const auto input = graph.addInput();
            current.bindings.emplace_back(*origin);
            signals.emplace(address, input);
            return input;
        }
        if (entry.kind == PrimitiveKind::VstInput || (host == level && entry.kind == PrimitiveKind::ModuleInput)) {
            const auto channel = host ? level->pluginAudioChannel(entry) : std::nullopt;
            auto signal = [&]() {
                if (!channel) return graph.addSum();
                if (const auto found = hostInputs.find(*channel); found != hostInputs.end()) return found->second;
                const auto input = graph.addInput();
                current.bindings.emplace_back(stream::HostAudioChannel{*channel});
                hostInputs.emplace(*channel, input);
                return input;
            }();
            signals.emplace(address, signal);
            return signal;
        }
        if (entry.kind == StreamSequencerTrack) {
            const bool live = render == DspRender::Audio || render == DspRender::Plugin;
            const auto sink = live ? graph.addPack(stream::SamplePrecision::Float32) : graph.addSum();
            signals.emplace(address, sink);
            const auto source = resolveInput(current, poly, *level, nodeId, 0);
            if (!live) {
                if (source) graph.connect(*source, sink);
                return sink;
            }
            const auto track = level->sequencerAudio(entry);
            const auto transport = level->rootGraph().sequenceTransport_;
            for (std::size_t lane = 0; lane < stream::Mono4{}.size(); ++lane) {
                const auto sample = graph.addUnpack(lane);
                graph.connect(source ? *source : graph.addSum(), sample);
                const auto playback = graph.addProcessor(2, [track, transport, lane] {
                    return std::make_unique<SequencerLane>(track, transport, lane);
                });
                graph.connect(sample, playback);
                graph.connect(audioRate(), playback);
                graph.connect(playback, sink);
            }
            return sink;
        }
        if (entry.kind == AudioStreamIn) {
            const bool live = render == DspRender::Audio || render == DspRender::Plugin;
            auto state = live ? std::make_shared<MediaReadState>(level->mediaAudio(entry)) : nullptr;
            std::array<stream::NodeId, 2> channels;
            for (std::size_t channel = 0; channel < channels.size(); ++channel) {
                channels[channel] = live ? graph.addProcessor(0, [state, channel] {
                    return std::make_unique<MediaChannelReader>(state, channel);
                }) : graph.addSum();
                signals.emplace(DspTraceKey{level->path_, nodeId, static_cast<int>(channel)}, channels[channel]);
            }
            return channels.at(port);
        }
        if (entry.kind == PrimitiveKind::AudioIn || entry.kind == PrimitiveKind::DirectSoundIn ||
            entry.kind == PrimitiveKind::AsioIn) {
            if (host) {
                const auto silence = graph.addSum();
                signals.emplace(address, silence);
                return silence;
            }
            if (render != DspRender::Audio) {
                throw std::invalid_argument("Start Audio with the signal routed to Audio Out to read live Audio In");
            }
            const auto api = entry.kind == PrimitiveKind::AsioIn ? AudioApi::Asio :
                entry.kind == PrimitiveKind::DirectSoundIn ? AudioApi::DirectSound : AudioApi::Any;
            if (api != AudioApi::Any) {
                if (result.inputApi != AudioApi::Any && result.inputApi != api) {
                    throw std::invalid_argument("Use one audio input driver per schematic");
                }
                result.inputApi = api;
            }
            const auto channel = static_cast<std::size_t>(port);
            const auto [it, inserted] = deviceInputs.try_emplace(channel);
            if (inserted) {
                it->second = graph.addInput();
                current.bindings.emplace_back(stream::HostAudioChannel{channel});
            }
            signals.emplace(address, it->second);
            return it->second;
        }
        if (isPolyBridge(entry.kind)) {
            const auto signal = graph.addSum();
            signals.emplace(address, signal);
            // The editor's offline DSP bench renders a single voice at slot 0.
            const bool enabled = stream::matchesVoiceIndex(level->voiceIndex(nodeId), 0);
            current.bridges.emplace_back(*origin, enabled);
            if (enabled) {
                if (const auto source = resolveInput(current, poly, *level, nodeId, 0)) {
                    graph.connect(*source, signal);
                }
            }
            return signal;
        }
        if (const auto operation = waveOperation(entry.kind)) {
            auto buffer = std::make_shared<stream::WaveBuffer>();
            result.waveBuffers.emplace_back(level, nodeId, buffer).refresh();
            constexpr auto precision = stream::SamplePrecision::Float32;
            const bool packed = isPackedStream(type);
            const int outputCount = static_cast<int>(spec.outputs.size());
            const int inputCount = static_cast<int>(spec.inputs.size()) - 1;
            std::array<stream::NodeId, 2> outputs{};
            for (int channel = 0; channel < outputCount; ++channel) {
                const auto signal = packed ? graph.addPack(precision)
                    : graph.addArithmetic(stream::SampleOperation::Convert, precision);
                outputs[channel] = signal;
                signals.emplace(DspTraceKey{level->path_, nodeId, channel}, signal);
            }
            std::array<stream::NodeId, 3> inputs{};
            for (int input = 0; input < inputCount; ++input) {
                std::optional<stream::NodeId> source;
                if (spec.inputs[input + 1].type == Type::Int) {
                    const auto parameter = control(level, nodeId, input + 1);
                    source = poly ? addInput(current, *poly, {stream::PolyValue::External, addExternal(*poly, parameter)})
                                  : parameter;
                } else {
                    source = resolveInput(current, poly, *level, nodeId, input + 1);
                }
                inputs[input] = source ? *source : graph.addSum();
            }
            const auto connectLane = [&](const auto& sources) {
                const auto makeChannel = [&](int channel) {
                    const auto processor = graph.addProcessor(inputCount, [buffer, operation, channel] {
                        return stream::makeWaveProcessor(*operation, buffer, channel);
                    });
                    for (int input = 0; input < inputCount; ++input) graph.connect(sources[input], processor);
                    return processor;
                };
                const auto left = makeChannel(0);
                graph.connect(left, outputs[0]);
                if (entry.kind == WaveWriteStereo) {
                    const auto right = graph.addProcessor(2, stream::makeWavePassthrough);
                    graph.connect(left, right);
                    graph.connect(sources[2], right);
                    graph.connect(right, outputs[1]);
                } else if (outputCount == 2) {
                    graph.connect(makeChannel(1), outputs[1]);
                }
            };
            if (packed) {
                for (std::size_t lane = 0; lane < stream::Mono4{}.size(); ++lane) {
                    auto sources = inputs;
                    for (int input = 0; input < inputCount; ++input) {
                        if (spec.inputs[input + 1].type == Type::Int) continue;
                        sources[input] = graph.addUnpack(lane);
                        graph.connect(inputs[input], sources[input]);
                    }
                    connectLane(sources);
                }
            } else {
                connectLane(inputs);
            }
            return outputs[port];
        }
        if (entry.kind == FloatArrayRead) {
            auto table = std::make_shared<stream::FloatArrayTable>();
            const auto signal = graph.addFloatArrayRead(table);
            signals.emplace(address, signal);
            auto& binding = result.arrayReaders.emplace_back(level, nodeId, std::move(table));
            binding.refresh();
            if (const auto source = resolveInput(current, poly, *level, nodeId, 1)) {
                graph.connect(*source, signal);
            }
            return signal;
        }
        if (entry.kind == StreamCounter || entry.kind == DoubleStreamCounter) {
            const auto precision = entry.kind == DoubleStreamCounter ? stream::SamplePrecision::Float64
                                                                    : stream::SamplePrecision::Float32;
            const auto signal = graph.addProcessor(1, [precision] { return stream::makeStreamCounter(precision); });
            signals.emplace(address, signal);
            // Packed processors broadcast the scalar count. A Green reset applies to every lane.
            const auto resets = control(level, nodeId, 0);
            graph.connect(poly ? addInput(current, *poly, {stream::PolyValue::External, addExternal(*poly, resets)})
                               : resets,
                          signal);
            return signal;
        }
        const bool oscillator = isStreamOscillator(entry.kind);
        if (entry.kind == DeZipper || entry.kind == FloatDeZipper || entry.kind == Glide || oscillator) {
            const bool floatInput = entry.kind == FloatDeZipper;
            const bool glide = entry.kind == Glide;
            const bool packed = !floatInput && isPackedStream(type);
            const auto precision = spec.outputs.front().type == Type::StreamDouble ? stream::SamplePrecision::Float64
                                                                                  : stream::SamplePrecision::Float32;
            stream::Graph::PrepareProcessor factory;
            if (oscillator) {
                using enum stream::OscillatorWaveform;
                const auto waveform = entry.kind == PrimitiveKind::Sine || entry.kind == DoubleSine ? Sine
                    : entry.kind == PrimitiveKind::Triangle || entry.kind == DoubleTriangle ? Triangle : Sawtooth;
                factory = [waveform, precision] { return stream::makeOscillator(waveform, precision); };
            } else if (glide) {
                factory = stream::makeGlide;
            } else {
                factory = floatInput ? stream::makeFloatDeZipper : stream::makeStreamDeZipper;
            }
            const int streamInputs = oscillator ? 3 : glide ? 2 : floatInput ? 0 : 1;
            const int inputCount = oscillator || glide ? streamInputs : 4;
            const auto signal = packed ? graph.addPack(precision) : graph.addProcessor(inputCount, factory);
            signals.emplace(address, signal);

            const auto bind = [&](stream::NodeId input) {
                return poly ? addInput(current, *poly, {stream::PolyValue::External, addExternal(*poly, input)}) : input;
            };
            const auto streamInput = [&](int input) {
                const auto source = resolveInput(current, poly, *level, nodeId, input);
                return source ? *source : graph.addSum();
            };
            std::array<stream::NodeId, 4> inputs{};
            for (int port = 0; port < streamInputs; ++port) inputs[port] = streamInput(port);
            if (!oscillator && !glide) {
                if (floatInput) inputs[0] = bind(control(level, nodeId, 0));
                inputs[1] = bind(control(level, nodeId, 1));
                inputs[2] = bind(audioRate());
                inputs[3] = bind(control(level, nodeId, 2));
            }
            const auto connectChannel = [&](stream::NodeId channel, std::optional<std::size_t> lane = {}) {
                for (int port = 0; port < inputCount; ++port) {
                    auto source = inputs[port];
                    if (lane && port < streamInputs) {
                        const auto unpack = graph.addUnpack(*lane);
                        graph.connect(source, unpack);
                        source = unpack;
                    }
                    graph.connect(source, channel);
                }
            };
            if (packed) {
                for (std::size_t lane = 0; lane < stream::Mono4{}.size(); ++lane) {
                    const auto channel = graph.addProcessor(inputCount, factory);
                    connectChannel(channel, lane);
                    graph.connect(channel, signal);
                }
            } else {
                connectChannel(signal);
            }
            return signal;
        }
        if (isIndexedRouter(entry.kind)) {
            // Reserve a pass-through so the chosen path can wind through a delay and back here.
            const auto signal = graph.addInputMix(stream::SampleOperation::Add, stream::SamplePrecision::Float64);
            signals.emplace(address, signal);
            if (const auto input = selectedInput(*level, entry, port)) {
                if (const auto source = resolveInput(current, poly, *level, nodeId, *input)) {
                    graph.connect(*source, signal);
                }
            }
            return signal;
        }
        if (entry.kind == Matrix) {
            const auto operation = streamFamily(*type) == streamFamily(Type::StreamBoolean)
                ? stream::SampleOperation::Max : stream::SampleOperation::Add;
            const auto precision = numericStreamFamily(*type) == Type::StreamDouble
                ? stream::SamplePrecision::Float64 : stream::SamplePrecision::Float32;
            if (liveRouting) {
                const auto [iterator, inserted] = result.matrixRoutes.try_emplace(
                    DspTraceKey{level->path_, nodeId, 0}, level, nodeId);
                auto& binding = iterator->second;
                if (inserted) {
                    binding.table = std::make_shared<stream::RouteMixTable>();
                    for (const auto& link : level->links_) {
                        if (link.target == nodeId && link.input > 0) binding.wiredInputs.push_back(link.input);
                    }
                    std::ranges::sort(binding.wiredInputs);
                    const auto duplicates = std::ranges::unique(binding.wiredInputs);
                    binding.wiredInputs.erase(duplicates.begin(), duplicates.end());
                }
                const auto signal = graph.addRouteMix(binding.table, port, operation, precision);
                signals.emplace(address, signal);
                for (const int input : binding.wiredInputs) {
                    const auto source = resolveInput(current, poly, *level, nodeId, input);
                    graph.connect(source ? *source : graph.addSum(), signal);
                }
                return signal;
            }

            const auto signal = graph.addSum();
            signals.emplace(address, signal);
            std::optional<stream::NodeId> mix;
            for (const auto& route : level->matrixConnections(entry)) {
                if (route.output != port) continue;
                const auto source = resolveInput(current, poly, *level, nodeId, route.input);
                if (!source) continue;
                if (!mix) {
                    mix = source;
                    continue;
                }
                const auto sum = graph.addArithmetic(operation, precision);
                graph.connect(*mix, sum);
                graph.connect(*source, sum);
                mix = sum;
            }
            if (mix) graph.connect(*mix, signal);
            return signal;
        }
        const auto* builtInProgram = builtInStreamProgram(entry.kind);
        if (supportsStreamMath(spec) || spec.behavior == PrimitiveBehavior::StreamConversion ||
            (spec.behavior == PrimitiveBehavior::StreamRouting && !builtInProgram)) {
            const bool wide = type && numericStreamFamily(*type) == Type::StreamDouble;
            const auto precision = wide ? stream::SamplePrecision::Float64 : stream::SamplePrecision::Float32;
            const bool variableDelay = entry.kind == Delay || entry.kind == DoubleDelay;
            const auto signal = [&] {
                if (variableDelay) {
                    if (!level->hasValueInput(nodeId, 1)) return graph.addDelay(precision);
                    return graph.addVariableDelay(control(level, nodeId, 1), precision);
                }
                switch (entry.kind) {
                case DelayOneSample: case DelayDoubleOneSample:
                    return graph.addDelay(precision);
                case Pack: case PackDouble:
                    return graph.addPack(precision);
                case Unpack: case UnpackDouble:
                    current.packed = true;
                    return graph.addUnpack(port);
                default:
                    const auto operation = operationFor(entry.kind);
                    return graph.addArithmetic(operation, operation == stream::SampleOperation::Convert ?
                                               stream::SamplePrecision::Float32 : precision);
                }
            }();
            // Record the node before walking upstream so a delay can break feedback.
            signals.emplace(address, signal);
            const int signalInputs = variableDelay ? 1 : static_cast<int>(spec.inputs.size());
            for (int input = 0; input < signalInputs; ++input) {
                const auto source = resolveInput(current, poly, *level, nodeId, input);
                graph.connect(source ? *source : graph.addSum(), signal);
            }
            if (spec.pitchConversion) {
                const auto rate = audioRate();
                graph.connect(poly ? addInput(current, *poly, {stream::PolyValue::External, addExternal(*poly, rate)}) : rate,
                              signal);
            }
            return signal;
        }
        if (!builtInProgram && !entry.dspCode) {
            throw std::invalid_argument("This connector cannot supply a DSP signal");
        }
        if (entry.dspCode && entry.dspCode->draft != entry.dspCode->source) {
            throw std::invalid_argument("Compile the changed upstream DSP Code before rendering");
        }
        const auto& program = builtInProgram ? *builtInProgram : entry.dspCode->program;
        const auto block = graph.addDspCode(program);
        for (std::size_t i = 0; i < block.outputs.size(); ++i) {
            signals.emplace(DspTraceKey{level->path_, nodeId, static_cast<int>(i)}, block.outputs[i]);
        }
        const auto inputs = program.inputs();
        for (std::size_t input = 0; input < inputs.size(); ++input) {
            const bool reference = inputs[input].type == stream::DspPortType::MemRef;
            if (reference || inputs[input].arraySize != 0 || inputs[input].type == stream::DspPortType::MemArrayRef) {
                current.arrays.push_back({block.id, input, {level, nodeId, static_cast<int>(input)}, inputs[input].arraySize, reference});
                continue;
            }
            if (!program.readsSignalInput(input)) {
                continue;
            }
            const auto inputType = level->inputType(nodeId, static_cast<int>(input));
            if (builtInProgram && isDspConstant(inputType)) {
                const auto parameter = control(level, nodeId, static_cast<int>(input));
                const auto source = poly
                    ? addInput(current, *poly, {stream::PolyValue::External, addExternal(*poly, parameter)})
                    : parameter;
                graph.connectDspInput(source, block.id, input, stream::DspInputWidth::Scalar);
                continue;
            }
            if (const auto source = resolveInput(current, poly, *level, nodeId, static_cast<int>(input))) {
                // Packed Boolean links keep the generic StreamBoolean connector.
                const bool packed = isPackedStream(inputType);
                current.packed |= packed;
                graph.connectDspInput(*source, block.id, input,
                                      packed || inputType == Type::StreamBoolean ? stream::DspInputWidth::Packed
                                                                                : stream::DspInputWidth::Scalar);
            }
        }
        return block.outputs.at(port);
    };

    const auto collectFrames = [&](const auto& visit, const Schematic& level) -> void {
        for (const auto& entry : level.nodes_) {
            if (entry.kind != PrimitiveKind::MonoToFrame) continue;
            if (const auto signal = resolveInput(result, nullptr, level, entry.id, 0)) {
                result.frameSources.emplace(DspTraceKey{level.path_, entry.id, 0}, *signal);
                if (std::ranges::find(result.outputs, *signal) == result.outputs.end()) {
                    result.outputs.push_back(*signal);
                    ++result.additionalOutputs;
                }
            }
        }
        for (const auto& [id, child] : level.modules_) visit(visit, *child);
    };
    const auto collectReadoutMixes = [&](const auto& visit, const Schematic& level) -> void {
        for (const auto& entry : level.nodes_) {
            const auto behavior = primitive(entry.kind).behavior;
            if (behavior != PrimitiveBehavior::SampleMono &&
                (behavior != PrimitiveBehavior::CaptureSignal || entry.kind == PrimitiveKind::SignalAnalyser)) continue;
            const int count = behavior == PrimitiveBehavior::SampleMono ? 1 : static_cast<int>(entry.inputTriggers.size()) - 2;
            for (int input = 0; input < count; ++input) {
                auto links = dspLinks(level.links_, entry.id, input);
                if (links.empty() || std::next(links.begin()) == links.end()) continue;

                // Taking a readout must not activate an otherwise inactive stream.
                std::vector<stream::NodeId> sources;
                for (const auto& link : links) {
                    const auto source = level.resolveDspSource(link.source, link.output, host);
                    if (!source) continue;
                    const auto signal = result.signals.find({source->graph->path_, source->node, source->port, source->input});
                    if (signal == result.signals.end()) {
                        sources.clear();
                        break;
                    }
                    sources.push_back(signal->second);
                }
                if (sources.empty()) continue;

                const auto mix = addInputMix(result, level, entry.id, input);
                for (const auto signal : sources) result.graph.connect(signal, mix);
                result.outputs.push_back(mix);
                ++result.additionalOutputs;
            }
        }
        for (const auto& [id, child] : level.modules_) visit(visit, *child);
    };
    result.outputs.reserve(portCount);
    if (host) {
        for (std::size_t i = 0; i < portCount; ++i) result.outputs.push_back(result.graph.addSum());
        const auto& outputs = node(id).moduleOutputs;
        std::size_t channel = 0;
        for (int port = 0; std::cmp_less(port, outputs.size()); ++port) {
            if (outputType(id, port) != Type::Mono) continue;
            if (const auto signal = resolveOutput(result, nullptr, *this, id, port)) {
                result.graph.connect(*signal, result.outputs[channel]);
            }
            ++channel;
        }
        const auto collectOutputs = [&](const auto& visit, const Schematic& level) -> void {
            for (const auto& entry : level.nodes_) {
                if (entry.kind != PrimitiveKind::VstOutput) continue;
                const auto channel = level.pluginAudioChannel(entry);
                if (!channel) continue;
                if (const auto signal = resolveInput(result, nullptr, level, entry.id, 1)) {
                    result.graph.connect(*signal, result.outputs[*channel]);
                }
            }
            for (const auto& [node, child] : level.modules_) visit(visit, *child);
        };
        collectOutputs(collectOutputs, *host);
        collectFrames(collectFrames, *host);
        collectReadoutMixes(collectReadoutMixes, *host);
        return result;
    }
    for (std::size_t port = 0; port < portCount; ++port) {
        std::optional<stream::NodeId> signal;
        if (render == DspRender::Outputs) {
            signal = resolveOutput(result, nullptr, *this, id, static_cast<int>(port));
        } else {
            signal = resolveInput(result, nullptr, *this, id, static_cast<int>(port));
        }
        result.outputs.push_back(signal ? *signal : result.graph.addSum());
    }
    if (render == DspRender::Audio) {
        collectFrames(collectFrames, rootGraph());
        collectReadoutMixes(collectReadoutMixes, rootGraph());
    }
    return result;
}

std::vector<std::vector<double>> Schematic::renderDsp(int id, std::size_t frames, std::size_t blockSize,
    bool captureInputs, std::string* compiledCode, std::span<float> timings) const
{
    const auto& entry = node(id);
    const auto portCount = captureInputs ? entry.inputTriggers.size() - 2 : entry.dspCode->program.outputs().size();
    if (frames == 0 || frames > maxDspCaptureFrames || blockSize == 0 || blockSize > frames) {
        throw std::invalid_argument("Capture needs 1–16384 samples, with block size at most the sample count");
    }
    if (portCount > 1048576 / frames) {
        throw std::invalid_argument("Reduce the sample count to capture this many DSP outputs");
    }

    const auto patch = buildDspGraph(id, portCount, captureInputs ? DspRender::Inputs : DspRender::Outputs);
    const auto render = [&](auto runtime) -> std::vector<std::vector<double>> {
        using Runtime = decltype(runtime);
        using Sample = typename Runtime::InputBuffer::value_type;
        if (compiledCode) {
            *compiledCode = runtime.compiledDspCode();
        }
        DspRenderGraph::supplyArrays(runtime, patch.arrays);

        std::vector<std::vector<Sample>> constantBuffers;
        constantBuffers.reserve(patch.bindings.size());
        for (const auto& binding : patch.bindings) {
            const double value = DspRenderGraph::readConstant(binding);
            if constexpr (std::same_as<Sample, double>) {
                constantBuffers.emplace_back(blockSize, value);
            } else {
                constantBuffers.emplace_back(blockSize, Sample{value, value, value, value});
            }
        }
        std::vector<typename Runtime::InputBuffer> inputs(patch.bindings.size());
        std::vector<std::vector<Sample>> samples(patch.outputs.size(), std::vector<Sample>(frames));
        std::vector<typename Runtime::OutputBuffer> taps(samples.size());

        const auto tailSize = std::min(swatchFrames, frames);
        const auto tailStart = frames - tailSize;
        std::map<DspTraceKey, std::vector<Sample>> traces;
        std::vector<typename Runtime::Probe> history;
        for (const auto& [address, signal] : patch.signals) {
            if (runtime.renders(signal)) {
                auto& trace = traces.try_emplace(address, tailSize).first->second;
                history.push_back({signal, trace});
            }
        }

        std::vector<typename Runtime::Probe> probes;
        probes.reserve(history.size());
        for (std::size_t offset = 0; offset < frames;) {
            const auto count = std::min(blockSize, frames - offset);
            for (std::size_t i = 0; i < constantBuffers.size(); ++i) {
                inputs[i] = std::span{constantBuffers[i]}.first(count);
            }
            for (std::size_t i = 0; i < samples.size(); ++i) {
                taps[i] = std::span{samples[i]}.subspan(offset, count);
            }
            probes.clear();
            if (offset + count > tailStart) {
                const auto begin = std::max(offset, tailStart);
                for (const auto& trace : history) {
                    probes.push_back({trace.signal, trace.tail.subspan(begin - tailStart, offset + count - begin)});
                }
            }
            using Clock = std::chrono::steady_clock;
            const auto start = timings.empty() ? Clock::time_point{} : Clock::now();
            if (!runtime.process(inputs, taps, probes)) {
                throw std::logic_error("Could not render the DSP capture");
            }
            if (!timings.empty()) {
                timings[offset] = std::chrono::duration<float, std::nano>(Clock::now() - start).count();
            }
            offset += count;
        }

        if constexpr (std::same_as<Sample, double>) {
            rootGraph().dspTraces_.swap(traces);
            return samples;
        } else {
            // The editor's numeric and waveform readouts show the first lane.
            const auto firstLane = [](const auto& values) {
                auto lane = values | std::views::transform([](const auto& sample) { return sample.front(); });
                return std::vector<double>(lane.begin(), lane.end());
            };
            std::map<DspTraceKey, std::vector<double>> firstLaneTraces;
            for (const auto& [address, trace] : traces) {
                firstLaneTraces.emplace(address, firstLane(trace));
            }
            rootGraph().dspTraces_.swap(firstLaneTraces);
            auto channels = samples | std::views::transform(firstLane);
            return {channels.begin(), channels.end()};
        }
    };
    return patch.packed ? render(patch.graph.compileMonoDouble4(patch.outputs))
                        : render(patch.graph.compileMonoDouble(patch.outputs));
}

struct Schematic::MonoPreview {
    using Runtime = std::variant<stream::MonoDoubleRuntime, stream::MonoDouble4Runtime>;
    static constexpr std::size_t blockSize = 256;
    static constexpr double sampleRate = 48000;
    static constexpr double maxCatchUp = 2; // Bound synchronous rendering after a long pause.

    explicit MonoPreview(const DspRenderGraph& graph)
        : runtime(graph.packed ? Runtime{graph.graph.compileMonoDouble4(graph.outputs)}
                               : Runtime{graph.graph.compileMonoDouble(graph.outputs)}),
          arrays(graph.arrays), arrayReaders(graph.arrayReaders), waveBuffers(graph.waveBuffers), bridges(graph.bridges),
          streamRoutes(graph.streamRoutes)
    {
        constants = graph.bindings;
    }

    [[nodiscard]] bool matchesRouting() const
    {
        return std::ranges::all_of(bridges, [](const auto& bridge) {
            const auto& [source, enabled] = bridge;
            return enabled == stream::matchesVoiceIndex(source.graph->voiceIndex(source.node), 0);
        }) && std::ranges::all_of(streamRoutes, [](const auto& route) {
            const auto& [level, id] = route.first;
            return route.second == level->routerSelection(level->node(id));
        });
    }

    double sample(TickTime now)
    {
        if (lastTime && now <= *lastTime) {
            return lastSample;
        }
        const double elapsed = lastTime ? (now - *lastTime).count() : 0;
        const double frames = std::min(elapsed, maxCatchUp) * sampleRate + frameRemainder;
        const auto count = lastTime ? std::llround(frames) : 1;
        if (count > 0) {
            lastSample = std::visit([&](auto& processor) { return render(processor, count); }, runtime);
        }
        frameRemainder = lastTime ? frames - count : 0;
        lastTime = now;
        return lastSample;
    }

private:
    template<typename Processor>
    double render(Processor& processor, std::size_t remaining) const
    {
        using Sample = typename Processor::InputBuffer::value_type;
        DspRenderGraph::supplyArrays(processor, arrays);
        for (const auto& reader : arrayReaders) reader.refresh();
        for (const auto& memory : waveBuffers) memory.refresh();
        std::vector<std::array<Sample, blockSize>> buffers(constants.size());
        std::vector<typename Processor::InputBuffer> inputs(constants.size());
        for (std::size_t i = 0; i < constants.size(); ++i) {
            const auto value = DspRenderGraph::readConstant(constants[i]);
            if constexpr (std::same_as<Sample, double>) {
                buffers[i].fill(value);
            } else {
                buffers[i].fill(Sample{value, value, value, value});
            }
        }

        std::array<Sample, blockSize> buffer;
        Sample last{};
        while (remaining > 0) {
            const auto count = std::min(blockSize, remaining);
            for (std::size_t i = 0; i < inputs.size(); ++i) {
                inputs[i] = std::span{buffers[i]}.first(count);
            }
            const std::array<typename Processor::OutputBuffer, 1> output{std::span{buffer}.first(count)};
            if (!processor.process(inputs, output)) {
                throw std::logic_error("Could not render the Mono sample");
            }
            last = buffer[count - 1];
            remaining -= count;
        }
        if constexpr (std::same_as<Sample, double>) {
            return last;
        } else {
            return last.front();
        }
    }

    Runtime runtime;
    std::vector<DspRenderGraph::Input> constants;
    std::vector<DspRenderGraph::ArrayInput> arrays;
    std::vector<FloatArrayReadBinding> arrayReaders;
    std::vector<WaveBufferBinding> waveBuffers;
    std::vector<std::pair<DspSource, bool>> bridges;
    std::map<std::pair<const Schematic*, int>, int> streamRoutes;
    std::optional<TickTime> lastTime;
    double frameRemainder = 0;
    double lastSample = 0;
};

void Schematic::sampleMono(SchematicNode& entry)
{
    double value = 0;
    if (const auto live = liveDspTrace(entry.id, 0, false, 1)) {
        if (live->empty()) {
            fireOutputs(entry.id);
            return;
        }
        value = live->back();
    } else if (!dspLinks(links_, entry.id, 0).empty()) {
        auto& root = rootGraph();
        auto& preview = root.monoPreviews_[{path_, entry.id}];
        if (!preview || !preview->matchesRouting()) {
            preview = std::make_shared<MonoPreview>(buildDspGraph(entry.id, 1, DspRender::Inputs));
        }
        const auto now = root.tickDispatchTime_.value_or(TickTime{std::chrono::steady_clock::now()});
        value = preview->sample(now);
    }
    entry.localValue = outputType(entry.id) == Type::Boolean
        ? green::Value{value != 0} : green::Value{green::normalizeFloat(static_cast<float>(value))};
    fireOutputs(entry.id);
}

void Schematic::clearAudio()
{
    auto& root = rootGraph();
    const auto clearVoices = [&](const auto& visit, Schematic& level) -> void {
        for (auto& entry : level.nodes_) {
            if (entry.kind == PrimitiveKind::Voices && entry.voiceBank) entry.voiceBank->clear();
        }
        for (auto& [id, child] : level.modules_) visit(visit, *child);
    };
    clearVoices(clearVoices, root);
    root.dspTraces_.clear();
    root.monoPreviews_.clear();
    if (root.audioResetRequests_) root.audioResetRequests_->fetch_add(1, std::memory_order_release);
}

void Schematic::findAudioOutput(std::optional<DspSource>& output) const
{
    for (const auto& entry : nodes_) {
        if (entry.kind != PrimitiveKind::AudioOut && entry.kind != PrimitiveKind::DirectSoundOut &&
            entry.kind != PrimitiveKind::AsioOut) {
            continue;
        }
        if (output) {
            throw std::invalid_argument("Use one Audio Out per schematic");
        }
        output = DspSource{this, entry.id, 0};
    }
    for (const auto& [id, child] : modules_) {
        child->findAudioOutput(output);
    }
}

Schematic::AudioPatch Schematic::prepareAudio(std::size_t blockSize) const
{
    const auto& root = rootGraph();
    std::optional<DspSource> output;
    root.findAudioOutput(output);
    if (!output) {
        throw std::invalid_argument("Place an Audio Out node and wire a Mono signal to it");
    }
    const auto& level = *output->graph;
    const auto& endpoint = level.node(output->node);
    auto graph = level.buildDspGraph(output->node, endpoint.inputTriggers.size(), DspRender::Audio);
    if (graph.frameSources.empty() &&
        std::ranges::none_of(level.links_, [&](const SchematicLink& link) { return link.target == output->node; })) {
        throw std::invalid_argument("Wire a Mono signal to an audio output before starting audio");
    }
    if (endpoint.kind == PrimitiveKind::DirectSoundOut) graph.outputApi = AudioApi::DirectSound;
    if (endpoint.kind == PrimitiveKind::AsioOut) graph.outputApi = AudioApi::Asio;
    return compileAudioPatch(std::move(graph), blockSize);
}

Schematic::AudioPatch Schematic::preparePluginAudio(int moduleId, std::size_t blockSize) const
{
    const auto layout = pluginAudioLayout(moduleId);
    const auto outputs = layout.mainOutputs + layout.auxOutputs;
    if (!outputs) throw std::invalid_argument("The plug-in module needs at least one Mono output");
    auto patch = compileAudioPatch(buildDspGraph(moduleId, outputs, DspRender::Plugin), blockSize);
    patch.pluginModule_ = std::pair{this, moduleId};
    return patch;
}

Schematic::AudioPatch Schematic::compileAudioPatch(DspRenderGraph graph, std::size_t blockSize) const
{
    const auto& root = rootGraph();
    AudioPatch patch;
    patch.root_ = &root;
    patch.blockSize_ = blockSize;
    patch.streamRoutes_ = std::move(graph.streamRoutes);
    patch.inputApi_ = graph.inputApi;
    patch.outputApi_ = graph.outputApi;
    patch.monitor_ = std::make_shared<AudioMonitor>();
    patch.monitor_->topology = root.audioTopology_;
    std::vector<stream::AudioProcessor::InputBinding> inputs;
    inputs.reserve(graph.bindings.size());
    for (const auto& binding : graph.bindings) {
        if (const auto* source = std::get_if<DspSource>(&binding)) {
            inputs.emplace_back(DspRenderGraph::readParameter(*source));
            patch.parameters_.emplace_back(inputs.size() - 1, *source);
        } else if (const auto* channel = std::get_if<stream::AudioChannel>(&binding)) {
            inputs.emplace_back(*channel);
        } else if (const auto* channel = std::get_if<stream::HostAudioChannel>(&binding)) {
            inputs.emplace_back(*channel);
        } else if (const auto* transport = std::get_if<stream::TransportInput>(&binding)) {
            inputs.emplace_back(*transport);
        } else {
            patch.sampleRateParameter_ = inputs.size();
            inputs.emplace_back(root.sampleRate());
        }
    }

    using Engine = stream::AudioProcessor::Engine;
    Engine engine = graph.packed ? Engine{graph.graph.compileMonoDouble4(graph.outputs)}
                                 : Engine{graph.graph.compileMonoDouble(graph.outputs)};
    std::visit([&](auto& runtime) { DspRenderGraph::supplyArrays(runtime, graph.arrays); }, engine);
    std::set<DspTraceKey> graphSources;
    const auto collectGraphSources = [&](const auto& walk, const Schematic& schematic) -> void {
        for (const auto& entry : schematic.nodes_) {
            if (primitive(entry.kind).behavior != PrimitiveBehavior::CaptureSignal ||
                entry.kind == PrimitiveKind::SignalAnalyser) {
                continue;
            }
            const auto count = static_cast<int>(entry.inputTriggers.size()) - 2;
            for (int input = 0; input < count; ++input) {
                if (auto address = schematic.dspTraceKey(entry.id, input, false)) {
                    graphSources.insert(std::move(*address));
                }
            }
        }
        for (const auto& [id, child] : schematic.modules_) {
            walk(walk, *child);
        }
    };
    collectGraphSources(collectGraphSources, root);

    using Probe = stream::AudioProcessor::Probe;
    std::vector<Probe> probes;
    std::map<stream::NodeId, std::size_t> probeBySignal;
    for (const auto& [address, signal] : graph.signals) {
        if (!std::visit([signal](const auto& runtime) { return runtime.renders(signal); }, engine)) {
            continue;
        }
        const auto [probe, inserted] = probeBySignal.try_emplace(signal, probes.size());
        if (inserted) {
            probes.push_back({signal, swatchFrames});
        }
        if (graphSources.contains(address)) {
            probes[probe->second].frames = maxDspCaptureFrames;
        }
        patch.monitor_->probes.emplace(address, probe->second);
    }
    for (const auto& [address, signal] : graph.frameSources) {
        const auto [probe, inserted] = probeBySignal.try_emplace(signal, probes.size());
        if (inserted) probes.push_back({signal, 2 * maxDspCaptureFrames});
        else probes[probe->second].frames = 2 * maxDspCaptureFrames;
        patch.monitor_->frames.emplace(address, AudioMonitor::FrameProbe{probe->second});
    }
    patch.voices_ = std::move(graph.bridges);
    patch.arrayReaders_ = std::move(graph.arrayReaders);
    patch.waveBuffers_ = std::move(graph.waveBuffers);
    std::vector<std::shared_ptr<stream::RouteMixTable>> routeTables;
    for (auto& [address, binding] : graph.matrixRoutes) {
        binding.refresh();
        routeTables.push_back(binding.table);
        patch.matrixRoutes_.push_back(std::move(binding));
    }
    if (!root.audioResetRequests_) root.audioResetRequests_ = std::make_shared<std::atomic<std::uint64_t>>(0);
    patch.monitor_->processor = std::make_unique<stream::AudioProcessor>(
        std::move(engine), std::move(inputs), blockSize, probes, root.audioResetRequests_, graph.additionalOutputs,
        std::move(routeTables), root.hasSequencerTracks() ? root.sequenceTransport_ : nullptr);
    return patch;
}

bool Schematic::publishAudioFrames()
{
    const auto monitor = audioMonitor();
    if (!monitor || monitor->frames.empty()) return false;
    bool changed = false;
    const auto publish = [&](const auto& visit, Schematic& level) -> void {
        for (auto& entry : level.nodes_) {
            if (entry.kind != PrimitiveKind::MonoToFrame) continue;
            const auto it = monitor->frames.find(DspTraceKey{level.path_, entry.id, 0});
            if (it == monitor->frames.end()) continue;
            auto& frame = it->second;
            const auto frames = static_cast<std::size_t>(level.hasValueInput(entry.id, 1)
                ? std::clamp(std::get<int>(level.readInput(entry.id, 1)), 1, maxDspCaptureFrames) : 256);
            if (frame.frames != frames) frame = {frame.probe, frames};
            if (auto samples = monitor->processor->readFrame(frame.probe, frames, frame.sequence)) {
                frame.sequence = samples->sequence;
                entry.capturedSignals.resize(1);
                entry.capturedSignals.front() = std::move(samples->samples);
                level.fireOutput(entry, 0);
                changed = true;
            }
        }
        for (auto& [id, child] : level.modules_) visit(visit, *child);
    };
    publish(publish, rootGraph());
    return changed;
}

void Schematic::setLiveAudio(const AudioPatch* patch)
{
    auto& root = rootGraph();
    if (patch && (patch->root_ != &root || patch->monitor_->topology != root.audioTopology_)) {
        throw std::invalid_argument("Live audio needs a patch prepared for this schematic");
    }
    root.liveAudio_ = patch ? patch->monitor_ : std::weak_ptr<const AudioMonitor>{};
}

bool Schematic::refreshAudioControls(AudioPatch& patch) const
{
    const auto& root = rootGraph();
    if (&root != patch.root_ || root.audioTopology_ != patch.monitor_->topology) {
        return false;
    }
    const auto routeChanged = std::ranges::any_of(patch.streamRoutes_, [](const auto& route) {
        const auto& [level, id] = route.first;
        return route.second != level->routerSelection(level->node(id));
    });
    if (routeChanged) {
        auto next = patch.pluginModule_
            ? patch.pluginModule_->first->preparePluginAudio(patch.pluginModule_->second, patch.blockSize_)
            : root.prepareAudio(patch.blockSize_);
        if (root.liveAudio_.lock() == patch.monitor_) root.liveAudio_ = next.monitor_;
        patch = std::move(next);
        return true;
    }
    for (const auto& [source, enabled] : patch.voices_) {
        if (enabled != stream::matchesVoiceIndex(source.graph->voiceIndex(source.node), 0)) {
            return false;
        }
    }
    for (const auto& [index, source] : patch.parameters_) {
        patch.processor().setParameter(index, DspRenderGraph::readParameter(source));
    }
    for (auto& matrix : patch.matrixRoutes_) matrix.refresh();
    for (const auto& reader : patch.arrayReaders_) reader.refresh();
    for (const auto& memory : patch.waveBuffers_) memory.refresh();
    if (patch.sampleRateParameter_) patch.processor().setParameter(*patch.sampleRateParameter_, root.sampleRate());
    return true;
}

}
