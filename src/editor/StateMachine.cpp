#include "Schematic.hpp"

#include <algorithm>
#include <charconv>
#include <ranges>

namespace openstone {

struct StateMachineMap {
    struct Transition {
        std::int32_t control;
        std::int32_t target;
    };
    struct State {
        std::int32_t number;
        std::vector<Transition> transitions;
    };

    std::string source;
    std::vector<State> states;
    bool syntaxCorrect = false;
};

namespace {

void skipWhitespace(std::string_view& text)
{
    text.remove_prefix(std::min(text.find_first_not_of(" \t\r\n\f\v"), text.size()));
}

bool consume(std::string_view& text, std::string_view token)
{
    skipWhitespace(text);
    if (!text.starts_with(token)) return false;
    text.remove_prefix(token.size());
    return true;
}

std::optional<std::int32_t> number(std::string_view& text)
{
    skipWhitespace(text);
    if (text.empty()) return {};
    if (text.front() == '+') {
        text.remove_prefix(1);
        if (text.empty() || text.front() < '0' || text.front() > '9') return {};
    }
    std::int32_t value;
    const auto end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, value);
    if (result.ec != std::errc{}) return {};
    text.remove_prefix(static_cast<std::size_t>(result.ptr - text.data()));
    return value;
}

std::shared_ptr<const StateMachineMap> parseMap(std::string source)
{
    auto map = std::make_shared<StateMachineMap>();
    map->source = std::move(source);
    std::string_view text = map->source;
    // FlowStone retains complete statements before a syntax error.
    while (true) {
        skipWhitespace(text);
        if (text.empty()) {
            map->syntaxCorrect = true;
            break;
        }
        if (consume(text, "s")) {
            const auto state = number(text);
            if (!state || !consume(text, ";")) break;
            map->states.push_back({*state, {}});
        } else {
            const auto control = number(text);
            if (!control || !consume(text, "->")) break;
            const auto target = number(text);
            if (!target || !consume(text, ";")) break;
            if (!map->states.empty()) map->states.back().transitions.push_back({*control, *target});
        }
    }
    return map;
}

}

green::Value Schematic::readStateMachine(const SchematicNode& entry, int output) const
{
    const auto& state = entry.stateMachine;
    if (output == 0) return state.reported;
    return state.map && state.map->syntaxCorrect;
}

void Schematic::receiveStateMachine(SchematicNode& entry, int input)
{
    auto& state = entry.stateMachine;
    if (input == 1) {
        state.map = parseMap(std::get<std::string>(readInput(entry.id, 1)));
        state.active = 0;
        fireOutput(entry, 1);
        return;
    }

    const auto* map = state.map.get();
    if (input == 2) {
        state.active = 0;
        if (map && !map->states.empty()) state.reported = map->states.front().number;
        fireOutput(entry, 0);
        return;
    }

    if (!map || map->states.empty()) return;
    const auto control = std::get<std::int32_t>(readInput(entry.id, 0));
    const auto& transitions = map->states[state.active].transitions;
    const auto match = std::ranges::find(transitions, control, &StateMachineMap::Transition::control);
    if (match == transitions.end()) return;

    state.reported = match->target;
    const auto states = map->states | std::views::reverse;
    const auto target = std::ranges::find(states, state.reported, &StateMachineMap::State::number);
    if (target != states.end()) state.active = static_cast<std::size_t>(target.base() - map->states.begin() - 1);
    fireOutput(entry, 0);
}

void Schematic::primeStateMachines()
{
    for (auto& entry : nodes_) {
        if (entry.kind != PrimitiveKind::StateMachine) continue;
        auto& state = entry.stateMachine;
        if (!state.map && !hasValueInput(entry.id, 1)) continue;
        try {
            auto source = std::get<std::string>(readInput(entry.id, 1));
            if (state.map && state.map->source == source) continue;
            state.map = parseMap(std::move(source));
            state.active = 0;
        } catch (const std::logic_error&) {
            // A schematic being edited can temporarily have unreadable Green inputs.
        }
    }
    for (auto& [id, child] : modules_) child->primeStateMachines();
}

}
