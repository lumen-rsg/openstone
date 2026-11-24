#include "Schematic.hpp"
#include "stream/Runtime.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace openstone {
namespace {

using green::Type;
using green::Value;
using namespace std::chrono;

constexpr std::array<std::string_view, 12> noteNames{"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
constexpr std::array<std::string_view, 6> variables{"x", "y", "z", "a", "b", "c"};
constexpr auto maxFormulaSize = 4096;

std::string nameForNote(std::int32_t note)
{
    if (note < 0 || note > 127) return {};
    return std::string(noteNames[note % 12]) + std::to_string(note / 12 - 1);
}

std::int32_t noteForName(std::string_view name)
{
    const auto first = name.find_first_not_of(" \t\r\n");
    if (first == name.npos) return -1;
    name = name.substr(first, name.find_last_not_of(" \t\r\n") - first + 1);
    constexpr std::array pitchClasses{9, 11, 0, 2, 4, 5, 7};
    const char letter = name.front() >= 'a' && name.front() <= 'g' ? name.front() - 'a' + 'A' : name.front();
    if (letter < 'A' || letter > 'G') return -1;
    auto pitch = pitchClasses[letter - 'A'];
    name.remove_prefix(1);
    if (name.starts_with('#') || name.starts_with('b') || name.starts_with('B')) {
        pitch += name.front() == '#' ? 1 : -1;
        name.remove_prefix(1);
    }
    if (name.empty()) return -1;
    int octave = 0;
    const auto parsed = std::from_chars(name.data(), name.data() + name.size(), octave);
    if (parsed.ec != std::errc{} || parsed.ptr != name.data() + name.size() || octave < -2 || octave > 10) return -1;
    const auto note = 12 * (octave + 1) + pitch;
    return note >= 0 && note <= 127 ? note : -1;
}

}

double Schematic::sampleRate() const
{
    return rootGraph().sampleRate_;
}

void Schematic::setSampleRate(double rate)
{
    if (!std::isfinite(rate) || rate < 1 || rate > std::numeric_limits<float>::max()) {
        throw std::invalid_argument("Sample rate must be a positive finite value in Hz");
    }
    auto& root = rootGraph();
    if (root.sampleRate_ == rate) return;
    root.sampleRate_ = rate;
    const auto publish = [](const auto& walk, Schematic& graph) -> void {
        for (auto& entry : graph.nodes_) {
            const auto conversion = primitive(entry.kind).pitchConversion;
            const bool rateDependentConversion = conversion && conversion != math::PitchConversion::HzToPitch &&
                conversion != math::PitchConversion::PitchToHz;
            const bool sampleRateSetting = entry.kind == PrimitiveKind::Settings &&
                std::get<std::int32_t>(*entry.localValue) == static_cast<int>(SystemSetting::SampleRate);
            if (entry.kind != PrimitiveKind::SampleRate && !rateDependentConversion && !sampleRateSetting) continue;
            const auto type = graph.outputType(entry.id);
            if (type == Type::Float || type == Type::FloatArray) graph.fireOutput(entry, 0);
        }
        for (auto& [id, child] : graph.modules_) walk(walk, *child);
    };
    publish(publish, root);
    root.refreshConnections();
}

Value Schematic::readNoteConversion(const SchematicNode& entry) const
{
    const auto input = readInput(entry.id, 0);
    if (const auto* note = std::get_if<std::int32_t>(&input)) return nameForNote(*note);
    if (const auto* name = std::get_if<std::string>(&input)) return noteForName(*name);
    if (const auto* notes = std::get_if<green::IntArray>(&input)) {
        green::StringArray result;
        result.reserve(notes->size());
        std::ranges::transform(*notes, std::back_inserter(result), nameForNote);
        return result;
    }
    const auto& names = std::get<green::StringArray>(input);
    green::IntArray result;
    result.reserve(names.size());
    std::ranges::transform(names, std::back_inserter(result), noteForName);
    return result;
}

Value Schematic::readGeneralProcessing(const SchematicNode& entry, int output) const
{
    switch (entry.kind) {
    case PrimitiveKind::SampleRate:
        return static_cast<float>(sampleRate());
    case PrimitiveKind::FloatSlide:
    case PrimitiveKind::RandomNumber:
        return *entry.localValue;
    case PrimitiveKind::GoalProgress: {
        std::vector<const SchematicNode*> goals;
        rootGraph().collectGoals(goals);
        if (output == 0) return static_cast<std::int32_t>(goals.size());
        if (output == 1) {
            return static_cast<std::int32_t>(std::ranges::count_if(goals, [](const auto* goal) {
                return goal->goalSeconds.has_value();
            }));
        }
        std::string results;
        for (const auto* goal : goals) {
            if (!results.empty()) results += '\n';
            results += goal->label.empty() ? "Teaching Goal" : goal->label;
            results += goal->goalSeconds ? " — complete in " + std::to_string(*goal->goalSeconds) + " s" : " — pending";
        }
        return results;
    }
    case PrimitiveKind::Evaluator: {
        auto expression = std::get<std::string>(readInput(entry.id, 0));
        if (expression.empty()) return 0.0F;
        if (expression.size() > maxFormulaSize) return std::numeric_limits<float>::quiet_NaN();
        if (!entry.evaluator || expression != entry.evaluator->expression) {
            entry.evaluator = EvaluatorCache{std::move(expression), {}};
            try {
                entry.evaluator->program = stream::compileMathExpression(entry.evaluator->expression, variables);
            } catch (const stream::DspCompileError&) {
                return std::numeric_limits<float>::quiet_NaN();
            }
        }
        if (!entry.evaluator->program) return std::numeric_limits<float>::quiet_NaN();

        stream::Graph graph(stream::DspBackend::Interpreter);
        const auto block = graph.addDspCode(*entry.evaluator->program);
        std::array<double, variables.size()> values{};
        std::array<stream::MonoDoubleRuntime::InputBuffer, variables.size()> inputs{};
        std::size_t connected = 0;
        for (std::size_t i = 0; i < variables.size(); ++i) {
            if (!entry.evaluator->program->readsSignalInput(i)) continue;
            graph.connectDspInput(graph.addInput(), block.id, i);
            values[connected] = std::get<float>(readInput(entry.id, static_cast<int>(i + 1)));
            inputs[connected] = std::span{&values[connected], 1};
            ++connected;
        }
        double result = 0;
        const std::array<stream::MonoDoubleRuntime::OutputBuffer, 1> outputs{std::span{&result, 1}};
        if (!graph.compileMonoDouble(block.outputs).process(std::span{inputs}.first(connected), outputs)) {
            throw std::logic_error("Could not evaluate the expression");
        }
        return green::normalizeFloat(static_cast<float>(result));
    }
    default:
        throw std::logic_error("Expected a General Processing primitive with a value output");
    }
}

void Schematic::collectGoals(std::vector<const SchematicNode*>& goals) const
{
    for (const auto& entry : nodes_) {
        if (entry.kind == PrimitiveKind::TeachingGoal) goals.push_back(&entry);
        if (const auto* child = module(entry.id)) child->collectGoals(goals);
    }
}

void Schematic::publishGoalProgress(bool reset)
{
    if (reset) {
        for (auto& entry : nodes_) {
            if (entry.kind == PrimitiveKind::TeachingGoal) entry.goalSeconds.reset();
        }
        for (auto& [id, child] : modules_) child->publishGoalProgress(true);
        return;
    }
    for (auto& entry : nodes_) {
        if (entry.kind == PrimitiveKind::GoalProgress) fireOutputs(entry.id);
    }
    for (auto& [id, child] : modules_) child->publishGoalProgress(false);
}

void Schematic::receiveGeneralPulse(SchematicNode& entry, int input)
{
    auto& root = rootGraph();
    switch (entry.kind) {
    case PrimitiveKind::Evaluator:
        fireOutput(entry, 0);
        return;
    case PrimitiveKind::RandomNumber: {
        if (input == 1 || !entry.randomState) {
            const auto seed = std::get<std::int32_t>(readInput(entry.id, 1));
            const auto clock = system_clock::now().time_since_epoch().count();
            entry.randomState = seed == 0 ? static_cast<std::uint32_t>(clock) : static_cast<std::uint32_t>(seed);
        }
        if (input == 0) {
            *entry.randomState = *entry.randomState * 214013U + 2531011U;
            entry.localValue = static_cast<std::int32_t>((*entry.randomState >> 16) & 0x7fffU);
            fireOutput(entry, 0);
        }
        return;
    }
    case PrimitiveKind::FloatSlide:
        if (input == 3 && hasValueInput(entry.id, 3)) {
            entry.localValue = readInput(entry.id, 3);
            entry.slideDeadline.reset();
            fireOutput(entry, 0);
        }
        return;
    case PrimitiveKind::TeachingGoal:
        if (!entry.goalSeconds) {
            const auto now = root.tickDispatchTime_.value_or(TickTime{steady_clock::now()});
            entry.goalSeconds = std::max(0.0, (now - root.goalStart_).count());
            root.publishGoalProgress(false);
        }
        return;
    case PrimitiveKind::GoalProgress:
        root.goalStart_ = root.tickDispatchTime_.value_or(TickTime{steady_clock::now()});
        root.publishGoalProgress(true);
        root.publishGoalProgress(false);
        return;
    default:
        throw std::logic_error("Expected a General Processing primitive");
    }
}

void Schematic::advanceSlide(SchematicNode& entry, TickTime now, TickWake wake, TickSchedule& result)
{
    const auto target = std::get<float>(readInput(entry.id, 0));
    const auto rate = std::get<std::int32_t>(readInput(entry.id, 1));
    const auto step = std::abs(std::get<float>(readInput(entry.id, 2)));
    const auto current = std::get<float>(*entry.localValue);
    if (current == target || rate <= 0 || step == 0 || !std::isfinite(step) || !std::isfinite(target)) {
        entry.slideDeadline.reset();
        return;
    }
    const auto period = milliseconds(rate);
    if (!entry.slideDeadline || entry.slideInterval != rate) {
        entry.slideInterval = rate;
        entry.slideDeadline = now + period;
    } else if (wake == TickWake::Timer && *entry.slideDeadline <= now) {
        const auto next = std::isfinite(current) ? (current < target ? std::min(current + step, target)
                                                                   : std::max(current - step, target)) : target;
        if (next != current) {
            entry.localValue = next;
            result.fired = true;
            fireOutput(entry, 0);
        }
        if (next == current || next == target) entry.slideDeadline.reset();
        else entry.slideDeadline = now + period;
    }
    if (entry.slideDeadline && (!result.deadline || *entry.slideDeadline < *result.deadline)) {
        result.deadline = entry.slideDeadline;
    }
}

}
