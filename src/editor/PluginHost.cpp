#include "Schematic.hpp"

#include "PresetText.hpp"
#include "LegacyPreset.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace openstone {
namespace {

using enum PrimitiveKind;
using namespace green;

constexpr int pluginExportFolderInput = 11;

struct ParameterPorts {
    int name;
    int store;
    int fallback;
    int order;
};

constexpr ParameterPorts parameterPorts(PrimitiveKind kind)
{
    switch (kind) {
    case PresetArray: return {4, 8, 9, 13};
    case PresetString: return {2, 5, 3, 4};
    default: return {2, 6, 7, 9};
    }
}

float normalized(float value)
{
    return std::isnan(value) ? 0.0f : std::clamp(value, 0.0f, 1.0f);
}

std::string programName(const PresetManagerState& state, int program)
{
    if (const auto it = state.names.find(program); it != state.names.end()) return it->second;
    return "Program " + std::to_string(program + 1);
}

std::string truncateName(std::string value)
{
    // Keep the FlowStone 24-character limit without cutting UTF-8 continuation bytes.
    int count = 0;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if ((static_cast<unsigned char>(value[i]) & 0xc0) != 0x80 && ++count > 24) {
            value.resize(i);
            break;
        }
    }
    return value;
}

std::vector<std::string> presetNames(std::string_view list, int count)
{
    std::vector<std::string> names;
    const bool lines = list.find_first_of("\r\n") != list.npos;
    while (std::cmp_less(names.size(), count) && !list.empty()) {
        const auto end = list.find_first_of(lines ? "\r\n" : ",");
        names.push_back(truncateName(std::string(list.substr(0, end))));
        if (end == list.npos) break;
        const bool crlf = lines && list.substr(end).starts_with("\r\n");
        list.remove_prefix(end + (crlf ? 2 : 1));
    }
    return names;
}

struct DispatchGuard {
    bool& busy;

    explicit DispatchGuard(bool& flag) : busy(flag) { busy = true; }
    ~DispatchGuard() { busy = false; }
};

}

int Schematic::presetProgramCount(const SchematicNode& entry) const
{
    return std::clamp(std::get<int>(readInput(entry.id, 0)), 1, maxPresetPrograms);
}

green::Value Schematic::readPresetManager(const SchematicNode& entry, int output) const
{
    static const PresetManagerState empty;
    const auto& state = entry.presetManager ? *entry.presetManager : empty;
    if (output == 0) {
        auto address = path_;
        address.push_back(entry.id);
        return Preset{PresetProgramChange{static_cast<std::uint32_t>(state.program),
                                          std::get<bool>(readInput(entry.id, 3)), state.action, std::move(address)}};
    }
    if (output == 1) return state.program;
    if (output == 2) return state.editedName.value_or(programName(state, state.program));
    std::string names;
    const int count = presetProgramCount(entry);
    for (int program = 0; program < count; ++program) {
        if (program) names.push_back('\n');
        names += program == state.program && state.editedName ? *state.editedName : programName(state, program);
    }
    return names;
}

void Schematic::receivePresetManager(SchematicNode& entry, int input)
{
    if (!entry.presetManager) entry.presetManager.emplace();
    auto& state = *entry.presetManager;
    if (state.dispatching) return;
    const DispatchGuard dispatch(state.dispatching);
    const bool locked = std::get<bool>(readInput(entry.id, 3));
    const int count = presetProgramCount(entry);
    const auto notify = [&](PresetAction command) {
        state.action = command;
        try {
            fireOutput(entry, 0);
        } catch (...) {
            state.action = PresetAction::Recall;
            throw;
        }
        state.action = PresetAction::Recall;
    };
    const auto update = [&] {
        fireOutput(entry, 1);
        fireOutput(entry, 2);
        fireOutput(entry, 3);
    };
    const auto saveName = [&] {
        if (state.editedName) {
            state.names.insert_or_assign(state.program, std::move(*state.editedName));
            state.editedName.reset();
        }
    };
    if (input == 0 || input == 1) {
        const int requested = input == 1 ? std::get<int>(readInput(entry.id, 1)) : state.program;
        const int target = std::clamp(requested, 0, count - 1);
        if (target == state.program) {
            if (input == 0) fireOutput(entry, 3);
            return;
        }
        fireOutput(entry, 4);
        state.editedName.reset();
        state.program = target;
        withPresetRedrawsSuspended([&] { notify(PresetAction::Recall); });
        update();
        fireOutput(entry, 5);
        return;
    }
    if (input == 2) {
        auto name = truncateName(std::get<std::string>(readInput(entry.id, 2)));
        if (locked) state.editedName = std::move(name);
        else state.names.insert_or_assign(state.program, std::move(name));
    } else if (input == 5) {
        auto names = presetNames(std::get<std::string>(readInput(entry.id, 5)), count);
        for (int program = 0; std::cmp_less(program, names.size()); ++program) {
            state.names.insert_or_assign(program, std::move(names[program]));
        }
        state.editedName.reset();
    } else if (input == 4 || (input == 3 && !locked)) {
        notify(PresetAction::Save);
        saveName();
    } else if (input == 3) {
        return;
    } else {
        notify(input == 6 ? PresetAction::Clear : PresetAction::ClearAll);
        return;
    }
    fireOutput(entry, 2);
    fireOutput(entry, 3);
}

std::string Schematic::presetParameterName(const SchematicNode& entry) const
{
    auto name = std::get<std::string>(readInput(entry.id, parameterPorts(entry.kind).name));
    if (name.empty()) name = entry.label.empty() ? primitive(entry.kind).name : std::string_view(entry.label);
    return name;
}

bool Schematic::presetParameterAutomatable(const SchematicNode& entry) const
{
    if (entry.kind == PresetBypass) return true;
    if (entry.kind == PresetString) return false;
    return std::get<bool>(readInput(entry.id, entry.kind == PresetArray ? 7 : 5));
}

bool Schematic::presetParameterStored(const SchematicNode& entry) const
{
    return entry.kind != PresetBypass && std::get<bool>(readInput(entry.id, parameterPorts(entry.kind).store));
}

double Schematic::presetParameterOrder(const SchematicNode& entry) const
{
    const auto value = readInput(entry.id, parameterPorts(entry.kind).order);
    const double order = std::holds_alternative<float>(value) ? double(std::get<float>(value)) : double(std::get<int>(value));
    return std::isnan(order) ? 0 : order;
}

green::Value Schematic::presetValueForProgram(const SchematicNode& entry, int program) const
{
    if (entry.presetParameter) {
        const auto& saved = entry.presetParameter->saved;
        if (const auto value = saved.find(program); value != saved.end()) {
            auto result = value->second;
            if (auto* array = std::get_if<FloatArray>(&result); array && hasValueInput(entry.id, 1)) {
                const auto size = std::clamp(std::get<int>(readInput(entry.id, 1)), 0, maxPresetArraySize);
                array->resize(size, normalized(std::get<float>(readInput(entry.id, 9))));
            }
            return result;
        }
    }
    return defaultPresetParameter(entry);
}

green::Value Schematic::defaultPresetParameter(const SchematicNode& entry) const
{
    auto value = readInput(entry.id, parameterPorts(entry.kind).fallback);
    if (auto* number = std::get_if<float>(&value); number &&
        (!entry.presetParameter || !entry.presetParameter->flowStoneFloat)) *number = normalized(*number);
    if (entry.kind != PresetArray) return value;
    const int size = hasValueInput(entry.id, 1) ? std::get<int>(readInput(entry.id, 1)) : 1;
    return FloatArray(std::clamp(size, 0, maxPresetArraySize), std::get<float>(value));
}

green::Value Schematic::currentPresetParameter(const SchematicNode& entry) const
{
    if (entry.presetParameter && entry.presetParameter->flowStoneFloat && entry.presetParameter->flowStoneFloat->inactive)
        return 0.f;
    if (!entry.presetParameter || !entry.presetParameter->current) return defaultPresetParameter(entry);
    auto value = *entry.presetParameter->current;
    if (entry.kind == PresetArray && hasValueInput(entry.id, 1)) {
        const auto size = std::clamp(std::get<int>(readInput(entry.id, 1)), 0, maxPresetArraySize);
        std::get<FloatArray>(value).resize(size, normalized(std::get<float>(readInput(entry.id, 9))));
    }
    return value;
}

green::Value Schematic::readPresetParameter(const SchematicNode& entry, int output) const
{
    if (entry.kind != PresetArray || output == 0) return currentPresetParameter(entry);
    const auto index = std::get<int>(readInput(entry.id, 2));
    if (output == 2) return index;
    const auto values = std::get<FloatArray>(currentPresetParameter(entry));
    return index >= 0 && std::cmp_less(index, values.size()) ? values[index] : 0.0f;
}

void Schematic::changePresetParameter(SchematicNode& entry, green::Value value, bool edit, bool record)
{
    if (!entry.presetParameter) entry.presetParameter.emplace();
    auto& state = *entry.presetParameter;
    if (state.dispatching) return;
    const DispatchGuard dispatch(state.dispatching);
    const auto previous = currentPresetParameter(entry);
    const bool changed = value != previous || (edit && state.flowStoneFloat);
    const bool array = entry.kind == PresetArray;
    const int before = array ? 3 : 1;
    if (changed) fireOutput(entry, before);
    state.current = std::move(value);
    if (edit && presetParameterStored(entry)) {
        const auto bus = std::get<Preset>(readInput(entry.id, 0));
        if (const auto* program = std::get_if<PresetProgramChange>(&bus); program && !program->locked) {
            state.saved.insert_or_assign(program->program, *state.current);
        } else if (state.flowStoneFloat && !std::holds_alternative<PresetProgramChange>(bus) && !state.flowStoneFloat->locked) {
            state.saved.insert_or_assign(0, *state.current);
        }
    }
    if (!changed) return;
    fireOutput(entry, 0);
    if (array) fireOutput(entry, 1);
    fireOutput(entry, before + 1);
    auto& callback = rootGraph().pluginHostCallbacks_.recordParameter;
    if (!record || !callback || !presetParameterAutomatable(entry) ||
        !std::get<bool>(readInput(entry.id, array ? 12 : 8))) return;

    auto path = path_;
    path.push_back(entry.id);
    if (array) {
        const auto& old = std::get<FloatArray>(previous);
        const auto& next = std::get<FloatArray>(*state.current);
        for (int i = 0; std::cmp_less(i, next.size()); ++i) {
            if (std::cmp_greater_equal(i, old.size()) || old[i] != next[i]) callback({path, i}, next[i]);
        }
    } else {
        const auto next = entry.kind == PresetFloat ? std::get<float>(*state.current) :
            static_cast<float>(std::get<bool>(*state.current));
        callback({std::move(path), 0}, next);
    }
}

void Schematic::receivePresetParameter(SchematicNode& entry, int input)
{
    if (entry.presetParameter && entry.presetParameter->dispatching) return;
    const bool array = entry.kind == PresetArray;
    const auto& ports = parameterPorts(entry.kind);
    if (input == 0 || input == ports.store) {
        if (!presetParameterStored(entry)) return;
        const auto bus = std::get<Preset>(readInput(entry.id, 0));
        const auto* program = std::get_if<PresetProgramChange>(&bus);
        if (!program) return;
        if (!entry.presetParameter) entry.presetParameter.emplace();
        auto& state = *entry.presetParameter;
        if (state.flowStoneFloat) {
            state.flowStoneFloat->locked = program->locked;
            state.flowStoneFloat->inactive = false;
        }
        const int index = static_cast<int>(program->program);
        switch (program->action) {
        case PresetAction::Save:
            state.saved.insert_or_assign(index, currentPresetParameter(entry));
            return;
        case PresetAction::Clear:
            state.saved.erase(index);
            break;
        case PresetAction::ClearAll:
            state.saved.clear();
            break;
        case PresetAction::Recall:
            break;
        }
        changePresetParameter(entry, presetValueForProgram(entry, index), false, false);
        return;
    }
    if (input == ports.fallback && (!entry.presetParameter || !entry.presetParameter->current)) {
        fireOutput(entry, 0);
        if (array) fireOutput(entry, 1);
        return;
    }
    if (!array) {
        if (input == 9 && entry.presetParameter && entry.presetParameter->flowStoneFloat) {
            entry.presetParameter->flowStoneFloat->order = std::get<float>(readInput(entry.id, input));
            return;
        }
        if (input != 1) return;
        if (entry.presetParameter && entry.presetParameter->flowStoneFloat && entry.presetParameter->flowStoneFloat->inactive) return;
        auto value = readInput(entry.id, input);
        if (auto* number = std::get_if<float>(&value); number &&
            (!entry.presetParameter || !entry.presetParameter->flowStoneFloat)) *number = normalized(*number);
        changePresetParameter(entry, std::move(value), true, true);
        return;
    }
    if (input == 1 || input == 2) {
        if (input == 1) fireOutput(entry, 0);
        fireOutput(entry, 2);
        fireOutput(entry, 1);
    } else if (input == 3) {
        auto values = std::get<FloatArray>(currentPresetParameter(entry));
        const auto index = std::get<int>(readInput(entry.id, 2));
        if (index < 0 || std::cmp_greater_equal(index, values.size())) return;
        values[index] = normalized(std::get<float>(readInput(entry.id, 3)));
        changePresetParameter(entry, std::move(values), true, true);
    } else if (input == 10 || input == 11) {
        auto values = input == 10 ? std::get<FloatArray>(readInput(entry.id, 10)) :
            std::get<FloatArray>(currentPresetParameter(entry));
        const auto fallback = normalized(std::get<float>(readInput(entry.id, 9)));
        if (hasValueInput(entry.id, 1)) {
            values.resize(std::clamp(std::get<int>(readInput(entry.id, 1)), 0, maxPresetArraySize), fallback);
        } else if (values.size() > maxPresetArraySize) {
            values.resize(maxPresetArraySize);
        }
        if (input == 11) std::ranges::fill(values, fallback);
        else std::ranges::transform(values, values.begin(), normalized);
        changePresetParameter(entry, std::move(values), true, true);
    }
}

std::optional<Schematic::PresetParticipant> Schematic::presetManagerOnInput(int id)
{
    const auto value = std::get<Preset>(readInput(id, 0));
    const auto* program = std::get_if<PresetProgramChange>(&value);
    if (!program || program->managerPath.empty()) return std::nullopt;
    auto* scope = &rootGraph();
    for (const auto module : program->managerPath | std::views::take(program->managerPath.size() - 1)) {
        scope = scope->module(module);
        if (!scope) return std::nullopt;
    }
    const auto manager = std::ranges::find(scope->nodes_, program->managerPath.back(), &SchematicNode::id);
    if (manager == scope->nodes_.end() || manager->kind != PresetManager) return std::nullopt;
    return PresetParticipant{scope, &*manager};
}

std::vector<Schematic::PresetParticipant> Schematic::presetParticipants(std::span<const int> managerPath)
{
    std::vector<PresetParticipant> parameters;
    const auto visit = [&](const auto& self, Schematic& level) -> void {
        for (auto& entry : level.nodes_) {
            if (auto* child = level.module(entry.id)) self(self, *child);
            if (primitive(entry.kind).behavior != PrimitiveBehavior::PresetParameter) continue;
            if (!managerPath.empty()) {
                if (!level.presetParameterStored(entry)) continue;
                const auto input = std::get<Preset>(level.readInput(entry.id, 0));
                const auto* program = std::get_if<PresetProgramChange>(&input);
                if (!program || !std::ranges::equal(program->managerPath, managerPath)) continue;
            }
            parameters.push_back({&level, &entry});
        }
    };
    visit(visit, rootGraph());
    return parameters;
}

PresetTable Schematic::capturePresetTable(PresetParticipant manager, std::span<const PresetParticipant> participants,
                                          int first, int last, std::vector<std::string> names)
{
    PresetTable table;
    const auto& state = *manager.node->presetManager;
    for (int program = first; program < last; ++program) {
        const auto index = program - first;
        if (std::cmp_less(index, names.size())) table.programs.push_back(std::move(names[index]));
        else if (program == state.program && state.editedName) table.programs.push_back(*state.editedName);
        else table.programs.push_back(programName(state, program));
    }
    if (table.programs.empty() || participants.size() > maxPresetTableCells / table.programs.size()) {
        throw std::invalid_argument("Preset table exceeds the size limit");
    }
    std::size_t budget = maxPresetTableCells - participants.size() * table.programs.size();
    for (auto [level, parameter] : participants) {
        auto& row = table.parameters.emplace_back(level->presetParameterName(*parameter), std::vector<Value>{});
        for (int program = first; program < last; ++program) {
            auto value = program == state.program ? level->currentPresetParameter(*parameter) :
                level->presetValueForProgram(*parameter, program);
            if (const auto* array = std::get_if<FloatArray>(&value)) {
                if (array->size() > budget) throw std::invalid_argument("Preset arrays exceed the size limit");
                budget -= array->size();
            }
            row.values.push_back(std::move(value));
        }
    }
    return table;
}

std::string Schematic::applyPresetTable(PresetParticipant manager, std::span<const PresetParticipant> participants,
                                       PresetTable table, int first, int limit, int column, bool positional)
{
    std::map<std::string, PresetParticipant, std::less<>> byName;
    if (!positional) {
        for (const auto& participant : participants) {
            if (!byName.emplace(participant.graph->presetParameterName(*participant.node), participant).second) {
                throw std::invalid_argument("Preset parameters must use unique names");
            }
        }
    }
    const auto targetForRow = [&](std::size_t index) -> std::optional<PresetParticipant> {
        if (positional) return participants[index];
        const auto match = byName.find(table.parameters[index].name);
        return match == byName.end() ? std::nullopt : std::optional{match->second};
    };

    const int imported = std::min(limit - first, static_cast<int>(table.programs.size()) - column);
    auto& state = *manager.node->presetManager;
    const bool active = state.program >= first && state.program < first + imported;
    std::vector<PresetParticipant> activeParameters;
    for (std::size_t index = 0; index < table.parameters.size(); ++index) {
        const auto match = targetForRow(index);
        if (!match) continue;
        auto& row = table.parameters[index];
        const auto [level, parameter] = *match;
        const auto target = level->outputType(parameter->id);
        const auto supplied = typeOf(row.values.front());
        const bool compatibleNumber = (target == Type::Boolean && supplied == Type::Float) ||
                                      (target == Type::Float && supplied == Type::Boolean);
        if (target != supplied && !compatibleNumber) continue;
        if (!parameter->presetParameter) parameter->presetParameter.emplace();
        auto& saved = parameter->presetParameter->saved;
        for (int i = 0; i < imported; ++i) {
            auto value = std::move(row.values[column + i]);
            if (compatibleNumber) {
                if (target == Type::Boolean) value = std::get<float>(value) >= .5f;
                else value = std::get<bool>(value) ? 1.0f : 0.0f;
            }
            saved.insert_or_assign(first + i, std::move(value));
        }
        if (active) activeParameters.push_back(*match);
    }
    std::string names;
    for (int i = 0; i < imported; ++i) {
        if (i > 0) names.push_back('\n');
        auto name = truncateName(std::move(table.programs[column + i]));
        names += name;
        state.names.insert_or_assign(first + i, std::move(name));
    }
    if (active) state.editedName.reset();
    if (!activeParameters.empty()) {
        withPresetRedrawsSuspended([&] {
            for (const auto [level, parameter] : activeParameters) {
                level->changePresetParameter(*parameter, level->presetValueForProgram(*parameter, state.program), false, false);
            }
        });
    }
    manager.graph->fireOutput(*manager.node, 2);
    manager.graph->fireOutput(*manager.node, 3);
    return names;
}

namespace {

bool needsNamedChunk(const PresetTable& table)
{
    return std::ranges::any_of(table.parameters, [](const auto& row) {
        if (std::holds_alternative<std::string>(row.values.front())) return true;
        const auto* initial = std::get_if<FloatArray>(&row.values.front());
        return initial && std::ranges::any_of(row.values, [&](const auto& value) {
            return std::get<FloatArray>(value).size() != initial->size();
        });
    });
}

LegacyPreset makeLegacyPreset(const PresetTable& table, std::string id, int version, bool single, int active, bool named)
{
    LegacyPreset file{std::move(id), version, single, active};
    if (named || needsNamedChunk(table)) file.namedTable = writePresetText(table);
    for (std::size_t program = 0; program < table.programs.size(); ++program) {
        auto& result = file.programs.emplace_back(table.programs[program], std::vector<float>{});
        if (file.namedTable) continue;
        for (const auto& row : table.parameters) {
            const auto& value = row.values[program];
            if (const auto* array = std::get_if<FloatArray>(&value)) {
                result.values.insert(result.values.end(), array->begin(), array->end());
            } else if (const auto* flag = std::get_if<bool>(&value)) {
                result.values.push_back(*flag ? 1.0f : 0.0f);
            } else {
                result.values.push_back(std::get<float>(value));
            }
        }
    }
    return file;
}

PresetTable expandLegacyPreset(const LegacyPreset& file, const PresetTable& layout)
{
    if (file.namedTable) {
        auto table = readPresetText(*file.namedTable);
        if (table.programs.size() != file.programs.size()) {
            throw std::invalid_argument("FXP/FXB program count does not match the preset table");
        }
        return table;
    }
    PresetTable table;
    std::size_t width = 0;
    for (const auto& row : layout.parameters) {
        const auto& value = row.values.front();
        if (const auto* array = std::get_if<FloatArray>(&value)) width += array->size();
        else ++width;
        table.parameters.push_back({row.name, {}});
    }
    if (table.parameters.size() > maxPresetTableCells / file.programs.size()) {
        throw std::invalid_argument("FXP/FXB table exceeds the size limit");
    }
    for (const auto& program : file.programs) {
        if (program.values.size() != width) throw std::invalid_argument("FXP/FXB parameters do not match this plug-in layout");
        table.programs.push_back(program.name);
        std::size_t offset = 0;
        std::size_t target = 0;
        for (const auto& row : layout.parameters) {
            const auto& value = row.values.front();
            auto& result = table.parameters[target++].values;
            if (const auto* array = std::get_if<FloatArray>(&value)) {
                const auto begin = program.values.begin() + offset;
                result.emplace_back(FloatArray(begin, begin + array->size()));
                offset += array->size();
            } else {
                result.emplace_back(program.values[offset++]);
            }
        }
    }
    return table;
}

}

void Schematic::receivePresetUtility(SchematicNode& entry, int input)
{
    const bool showOrder = entry.kind == PresetShowOrder;
    const bool saveFile = entry.kind == PresetSave;
    const bool loadFile = entry.kind == PresetLoad;
    const bool binary = saveFile || loadFile;
    const int action = showOrder ? 1 : binary ? 6 : 3;
    const int error = saveFile ? 1 : 2;
    if (input != action || entry.loopRunning) return;
    const DispatchGuard guard(entry.loopRunning);

    if (showOrder) {
        struct Row {
            std::string name;
            double order;
            bool automated;
        };
        std::vector<Row> rows;
        for (auto [level, parameter] : presetParticipants({})) {
            const bool automatic = level->presetParameterAutomatable(*parameter);
            if (!automatic && !level->presetParameterStored(*parameter)) continue;
            const auto order = level->presetParameterOrder(*parameter);
            const auto name = level->presetParameterName(*parameter);
            if (parameter->kind == PresetArray) {
                const auto values = std::get<FloatArray>(level->currentPresetParameter(*parameter));
                for (std::size_t index = 0; index < values.size(); ++index) {
                    rows.push_back({name + " " + std::to_string(index + 1), order, automatic});
                }
            } else {
                rows.push_back({name, order, automatic});
            }
        }
        const bool grouped = std::get<bool>(readInput(entry.id, 0));
        std::ranges::stable_sort(rows, [grouped](const auto& a, const auto& b) {
            if (grouped && a.automated != b.automated) return a.automated;
            return a.order < b.order;
        });
        std::string list;
        for (const auto& row : rows) {
            if (!list.empty()) list.push_back('\n');
            list += row.name;
        }
        entry.localValue = std::move(list);
        fireOutput(entry, 0);
        return;
    }

    const auto manager = presetManagerOnInput(entry.id);
    if (!manager) {
        fireOutput(entry, error);
        return;
    }
    const auto [owner, managerNode] = *manager;
    if (!managerNode->presetManager) managerNode->presetManager.emplace();
    const auto& state = *managerNode->presetManager;
    const int count = owner->presetProgramCount(*managerNode);

    auto managerPath = owner->path_;
    managerPath.push_back(managerNode->id);
    auto participants = presetParticipants(managerPath);

    try {
        if (binary) {
            std::ranges::stable_sort(participants, {}, [](const auto& participant) {
                const auto [level, parameter] = participant;
                return level->presetParameterOrder(*parameter);
            });
        }
        const bool current = std::get<bool>(readInput(entry.id, saveFile ? 3 : 2));
        const int first = current ? state.program : 0;
        const int last = current ? state.program + 1 : count;
        if (entry.kind == PresetToText || saveFile) {
            const int namePort = saveFile ? 2 : 1;
            auto names = hasValueInput(entry.id, namePort) ?
                presetNames(std::get<std::string>(readInput(entry.id, namePort)), last - first) : std::vector<std::string>{};
            const auto table = capturePresetTable(*manager, participants, first, last, std::move(names));
            if (!saveFile) {
                entry.localValue = writePresetText(table);
            } else {
                const auto filename = std::get<std::string>(readInput(entry.id, 1));
                auto id = std::get<std::string>(readInput(entry.id, 4));
                if (id.empty()) id = "OpSt";
                const auto version = std::get<int>(readInput(entry.id, 5));
                const auto& files = rootGraph().textFiles_;
                if (filename.empty() || !files.save) throw std::invalid_argument("Preset Save needs a writable path");
                const bool named = std::ranges::any_of(participants, [](const auto& participant) {
                    return !participant.graph->presetParameterAutomatable(*participant.node);
                });
                const auto file = makeLegacyPreset(table, std::move(id), version, current, current ? 0 : state.program, named);
                files.save(filename, writeLegacyPreset(file));
            }
        } else if (loadFile) {
            const auto& files = rootGraph().textFiles_;
            const auto filename = std::get<std::string>(readInput(entry.id, 1));
            if (filename.empty() || (!files.loadBounded && !files.load)) {
                throw std::invalid_argument("Preset Load needs an existing file");
            }
            const auto file = readLegacyPreset(files.loadBounded ? files.loadBounded(filename, maxLegacyPresetBytes) :
                                               files.load(filename));
            const auto id = std::get<std::string>(readInput(entry.id, 3));
            const auto minimum = std::get<int>(readInput(entry.id, 4));
            const auto maximum = std::get<int>(readInput(entry.id, 5));
            if ((!id.empty() && file.id != id) || file.version < minimum || file.version > maximum) {
                throw std::invalid_argument("The FXP/FXB plug-in ID or version does not match");
            }
            const bool positional = !file.namedTable;
            if (positional) {
                std::erase_if(participants, [](const auto& participant) {
                    return !participant.graph->presetParameterAutomatable(*participant.node);
                });
            }
            const auto layout = positional ? capturePresetTable(*manager, participants, state.program, state.program + 1, {}) :
                PresetTable{};
            auto table = expandLegacyPreset(file, layout);
            const bool single = current || file.single;
            entry.localValue = applyPresetTable(*manager, participants, std::move(table),
                single ? state.program : 0, single ? state.program + 1 : count, single ? file.current : 0, positional);
            entry.legacyPresetInfo = {file.version, file.current};
        } else {
            auto table = readPresetText(std::get<std::string>(readInput(entry.id, 1)));
            entry.localValue = applyPresetTable(*manager, participants, std::move(table), first, last, 0);
        }
    } catch (const std::runtime_error&) {
        fireOutput(entry, error);
        return;
    } catch (const std::invalid_argument&) {
        fireOutput(entry, error);
        return;
    }
    fireOutput(entry, 0);
    if (loadFile) {
        fireOutput(entry, 3);
        fireOutput(entry, 4);
    }
    if (!saveFile) fireOutput(entry, 1);
}

const SchematicNode* Schematic::localPluginConfig(PrimitiveKind kind) const
{
    const SchematicNode* config = nullptr;
    for (const auto& entry : nodes_) {
        if (entry.kind != kind) continue;
        if (config) throw std::invalid_argument("Use one plug-in configuration node of each kind in a module");
        config = &entry;
    }
    return config;
}

std::optional<std::size_t> Schematic::pluginAudioChannel(const SchematicNode& entry) const
{
    if (entry.kind == VstInput || entry.kind == VstOutput) {
        const int channel = std::get<int>(readInput(entry.id, 0));
        if (channel < 0 || channel >= maxPluginAudioChannels) return std::nullopt;
        return static_cast<std::size_t>(channel);
    }
    const int moduleId = path_.back();
    if (parent_->inputType(moduleId, entry.boundaryIndex) != Type::Mono) return std::nullopt;
    std::size_t channel = 0;
    for (int i = 0; i < entry.boundaryIndex; ++i) {
        if (parent_->inputType(moduleId, i) == Type::Mono) ++channel;
    }
    return channel;
}

PluginAudioLayout Schematic::pluginAudioLayout(int moduleId) const
{
    const auto* level = module(moduleId);
    if (!level) throw std::invalid_argument("Choose a module to prepare for the plug-in host");
    int inputs = 0;
    int outputs = 0;
    const auto& root = node(moduleId);
    for (int i = 0; std::cmp_less(i, root.moduleInputs.size()); ++i) {
        if (inputType(moduleId, i) == Type::Mono) ++inputs;
    }
    for (int i = 0; std::cmp_less(i, root.moduleOutputs.size()); ++i) {
        if (outputType(moduleId, i) == Type::Mono) ++outputs;
    }
    const auto collectChannels = [&](const auto& visit, const Schematic& graph) -> void {
        for (const auto& entry : graph.nodes_) {
            if (entry.kind != VstInput && entry.kind != VstOutput) continue;
            if (const auto index = graph.pluginAudioChannel(entry)) {
                auto& count = entry.kind == VstInput ? inputs : outputs;
                count = std::max(count, static_cast<int>(*index) + 1);
            }
        }
        for (const auto& [id, child] : graph.modules_) visit(visit, *child);
    };
    collectChannels(collectChannels, *level);
    if (inputs > maxPluginAudioChannels || outputs > maxPluginAudioChannels) {
        throw std::invalid_argument("A plug-in module supports at most 256 channels per direction");
    }
    int auxInputs = 0;
    int auxOutputs = 0;
    if (const auto* config = level->localPluginConfig(Vst3AuxChannels)) {
        auxInputs = std::clamp(std::get<int>(level->readInput(config->id, 0)), 0, inputs);
        auxOutputs = std::clamp(std::get<int>(level->readInput(config->id, 1)), 0, outputs);
    }
    return {inputs - auxInputs, outputs - auxOutputs, auxInputs, auxOutputs};
}

PluginExportInfo Schematic::pluginExportInfo(int moduleId) const
{
    const auto* level = module(moduleId);
    if (!level) throw std::invalid_argument("Choose a module to read its plug-in metadata");
    const auto* config = level->localPluginConfig(VstPluginInfo);
    if (!config) return {};
    const auto text = [&](int port) { return std::get<std::string>(level->readInput(config->id, port)); };
    const auto flag = [&](int port) -> std::optional<bool> {
        if (!level->hasValueInput(config->id, port)) return std::nullopt;
        return std::get<bool>(level->readInput(config->id, port));
    };
    const std::optional<int> version = level->hasValueInput(config->id, 3)
        ? std::optional{std::get<int>(level->readInput(config->id, 3))} : std::nullopt;
    return {text(0), text(1), text(2), version, text(4), text(5), text(6), text(7),
            flag(8), flag(9), flag(10), text(pluginExportFolderInput), flag(12), flag(13)};
}

void Schematic::setPluginAuxStates(int moduleId, std::span<const int> inputs, std::span<const int> outputs)
{
    const auto layout = pluginAudioLayout(moduleId);
    auto& level = *module(moduleId);
    const auto* config = level.localPluginConfig(Vst3AuxChannels);
    if (!config) return;

    const auto normalize = [](std::span<const int> reported, int count) {
        IntArray result(count);
        for (std::size_t i = 0; i < std::min(result.size(), reported.size()); ++i) result[i] = reported[i] > 0;
        return result;
    };
    std::array next{normalize(inputs, layout.auxInputs), normalize(outputs, layout.auxOutputs)};
    auto& entry = level.node(config->id);
    const bool inputChanged = level.readPluginHost(entry, 0) != Value{next[0]};
    const bool outputChanged = level.readPluginHost(entry, 1) != Value{next[1]};
    level.pluginAuxStates_ = std::move(next);
    if (inputChanged) level.fireOutput(entry, 0);
    if (outputChanged) level.fireOutput(entry, 1);
    if (inputChanged || outputChanged) level.refreshConnections();
}

std::string Schematic::localExportFolder() const
{
    for (const auto& entry : nodes_) {
        if (entry.kind != VstPluginInfo || !hasValueInput(entry.id, pluginExportFolderInput)) continue;
        auto folder = std::get<std::string>(readInput(entry.id, pluginExportFolderInput));
        if (!folder.empty()) return folder;
    }
    return {};
}

std::string Schematic::readStartFolder(int id) const
{
    if (const auto& host = rootGraph().location_->pluginHost) return host->pluginFolder;
    if (hasValueInput(id, 0)) return std::get<std::string>(readInput(id, 0));
    for (auto* level = this; level; level = level->parent_) {
        auto folder = level->localExportFolder();
        if (!folder.empty()) return folder;
    }
    return {};
}

void Schematic::notifyStartFolderPreviews()
{
    for (auto& entry : nodes_) {
        if (entry.kind == StartFolder && !hasValueInput(entry.id, 0)) fireOutput(entry, 0);
    }
    for (auto& [id, child] : modules_) {
        if (child->localExportFolder().empty()) child->notifyStartFolderPreviews();
    }
}

void Schematic::receivePluginHost(SchematicNode& entry, int input)
{
    auto& root = rootGraph();
    switch (entry.kind) {
    case DelayCompensation:
        refreshPluginLatency();
        break;
    case VstInput: case VstOutput:
        ++root.audioTopology_;
        root.dspTraces_.clear();
        root.monoPreviews_.clear();
        break;
    case VstHostInfo:
        if (!root.location_->pluginHost) fireOutput(entry, input);
        break;
    case Vst3AuxChannels:
        fireOutput(entry, input);
        break;
    case StartFolder:
        fireOutput(entry, 0);
        break;
    case VstPluginInfo:
        if (input == pluginExportFolderInput && !root.location_->pluginHost) notifyStartFolderPreviews();
        break;
    default:
        break;
    }
}

green::Value Schematic::readPluginHost(const SchematicNode& entry, int output) const
{
    if (entry.kind == Vst3AuxChannels) {
        auto states = pluginAuxStates_[output];
        if (parent_) {
            const auto layout = parent_->pluginAudioLayout(path_.back());
            states.resize(output == 0 ? layout.auxInputs : layout.auxOutputs);
        } else {
            states.resize(std::clamp(std::get<int>(readInput(entry.id, output)), 0, maxPluginAudioChannels));
        }
        return states;
    }
    const auto& root = rootGraph();
    const auto& transport = root.pluginHostTransport_;
    switch (entry.kind) {
    case VstEditorOpen: return root.pluginEditorOpen_;
    case StartFolder: return readStartFolder(entry.id);
    case IsPlaying: return transport.playing;
    case OfflineMode: return transport.offline;
    case Tempo: return static_cast<float>(transport.tempo);
    case TimeSignature: return static_cast<float>(output == 0 ? transport.numerator : transport.denominator);
    default: break;
    }
    if (const auto& info = root.location_->pluginHost) {
        switch (output) {
        case 0: return info->vendor;
        case 1: return info->product;
        default: return info->version;
        }
    }
    return readInput(entry.id, output);
}

void Schematic::setPluginHostCallbacks(PluginHostCallbacks callbacks)
{
    auto& root = rootGraph();
    root.pluginHostCallbacks_ = std::move(callbacks);
    root.reportedPluginLatency_.reset();
    root.refreshPluginLatency();
}

void Schematic::setPluginHostInfo(std::optional<PluginHostInfo> info)
{
    auto& root = rootGraph();
    auto& host = root.location_->pluginHost;
    if (host == info) return;
    const bool wasSuspended = root.redrawsSuspended();
    const bool folderChanged = !host || !info || host->pluginFolder != info->pluginFolder;
    host = std::move(info);
    root.publishRedrawSuspension(wasSuspended);
    root.notifyPrimitiveOutputs(VstHostInfo);
    if (folderChanged) root.notifyPrimitiveOutputs(StartFolder);
    root.refreshConnections();
}

void Schematic::setPluginHostTransport(const stream::HostTransport& transport)
{
    auto& root = rootGraph();
    auto next = transport;
    constexpr stream::HostTransport defaults;
    // Validate host fields independently, keeping stopped playback and offline rendering independent.
    if (!(next.tempo >= std::numeric_limits<float>::min() && next.tempo <= std::numeric_limits<float>::max())) {
        next.tempo = defaults.tempo;
    }
    if (next.numerator <= 0) next.numerator = defaults.numerator;
    if (next.denominator <= 0) next.denominator = defaults.denominator;

    const auto previous = std::exchange(root.pluginHostTransport_, next);
    const bool playingChanged = previous.playing != next.playing;
    const bool offlineChanged = previous.offline != next.offline;
    const bool tempoChanged = static_cast<float>(previous.tempo) != static_cast<float>(next.tempo);
    const bool numeratorChanged = static_cast<float>(previous.numerator) != static_cast<float>(next.numerator);
    const bool denominatorChanged = static_cast<float>(previous.denominator) != static_cast<float>(next.denominator);
    // Publish after committing the full snapshot so downstream pulls see all of the new values.
    if (playingChanged) {
        root.notifyPrimitiveOutputs(IsPlaying, 0);
        if (!next.playing) root.notifyPrimitiveOutputs(IsPlaying, 1);
    }
    if (offlineChanged) root.notifyPrimitiveOutputs(OfflineMode);
    if (tempoChanged) root.notifyPrimitiveOutputs(Tempo);
    if (numeratorChanged) root.notifyPrimitiveOutputs(TimeSignature, 0);
    if (denominatorChanged) root.notifyPrimitiveOutputs(TimeSignature, 1);
    if (playingChanged || offlineChanged || tempoChanged || numeratorChanged || denominatorChanged) {
        root.refreshConnections();
    }
}

void Schematic::setPluginEditorOpen(bool open)
{
    auto& root = rootGraph();
    if (root.pluginEditorOpen_ == open) return;
    const bool wasSuspended = root.redrawsSuspended();
    root.pluginEditorOpen_ = open;
    root.publishRedrawSuspension(wasSuspended);
    root.notifyPrimitiveOutputs(VstEditorOpen);
    root.refreshConnections();
}

int Schematic::pluginLatencySamples() const
{
    const auto find = [](const auto& self, const Schematic& graph) -> std::optional<int> {
        for (const auto& entry : graph.nodes_) {
            if (entry.kind == DelayCompensation) return std::max(0, std::get<int>(graph.readInput(entry.id, 0)));
            if (const auto* child = graph.module(entry.id)) {
                if (auto delay = self(self, *child)) return delay;
            }
        }
        return std::nullopt;
    };
    return find(find, rootGraph()).value_or(0);
}

void Schematic::refreshPluginLatency()
{
    auto& root = rootGraph();
    if (!root.pluginHostCallbacks_.latencyChanged) return;
    const auto delay = root.pluginLatencySamples();
    if (delay == root.reportedPluginLatency_) return;
    root.reportedPluginLatency_ = delay;
    root.pluginHostCallbacks_.latencyChanged(delay);
}

void Schematic::collectPluginParameters(std::vector<PluginParameter>& parameters) const
{
    for (const auto& entry : nodes_) {
        if (const auto* child = module(entry.id)) child->collectPluginParameters(parameters);
        if (primitive(entry.kind).behavior != PrimitiveBehavior::PresetParameter || !presetParameterAutomatable(entry)) continue;
        const bool array = entry.kind == PresetArray;
        const auto ports = parameterPorts(entry.kind);
        const auto value = currentPresetParameter(entry);
        const auto initial = readInput(entry.id, ports.fallback);
        const auto unit = std::get<std::string>(readInput(entry.id, array ? 6 : 4));
        const auto display = std::get<std::string>(readInput(entry.id, array ? 5 : 3));
        const auto name = presetParameterName(entry);
        const auto order = presetParameterOrder(entry);
        auto path = path_;
        path.push_back(entry.id);
        if (array) {
            const auto& values = std::get<FloatArray>(value);
            const auto index = std::get<int>(readInput(entry.id, 2));
            for (int i = 0; std::cmp_less(i, values.size()); ++i) {
                parameters.push_back({{path, i}, name + " " + std::to_string(i + 1), i == index ? display : "",
                                      unit, order, values[i], normalized(std::get<float>(initial)), false});
            }
        } else if (entry.kind == PresetBool || entry.kind == PresetBypass) {
            parameters.push_back({{std::move(path), 0}, name, display, unit, order,
                                  static_cast<float>(std::get<bool>(value)), static_cast<float>(std::get<bool>(initial)),
                                  true, entry.kind == PresetBypass});
        } else {
            parameters.push_back({{std::move(path), 0}, name, display, unit, order, std::get<float>(value),
                                  normalized(std::get<float>(initial)), false});
        }
    }
}

std::vector<PluginParameter> Schematic::pluginParameters() const
{
    std::vector<PluginParameter> parameters;
    rootGraph().collectPluginParameters(parameters);
    std::ranges::stable_sort(parameters, {}, &PluginParameter::order);
    return parameters;
}

bool Schematic::receivePluginParameter(const PluginParameterAddress& address, float value)
{
    if (address.path.empty() || address.element < 0 || !std::isfinite(value)) return false;
    auto* level = &rootGraph();
    for (const auto id : address.path | std::views::take(address.path.size() - 1)) {
        level = level->module(id);
        if (!level) return false;
    }
    const auto entry = std::ranges::find(level->nodes_, address.path.back(), &SchematicNode::id);
    if (entry == level->nodes_.end() || primitive(entry->kind).behavior != PrimitiveBehavior::PresetParameter ||
        !level->presetParameterAutomatable(*entry)) return false;
    const bool array = entry->kind == PresetArray;
    if (array) {
        auto values = std::get<FloatArray>(level->currentPresetParameter(*entry));
        if (std::cmp_greater_equal(address.element, values.size())) return false;
        values[address.element] = normalized(value);
        level->changePresetParameter(*entry, std::move(values), true, false);
    } else {
        if (address.element != 0) return false;
        level->changePresetParameter(*entry, entry->kind == PresetFloat ? Value{normalized(value)} : Value{value >= 0.5f},
                                     true, false);
    }
    level->refreshConnections();
    return true;
}

}
