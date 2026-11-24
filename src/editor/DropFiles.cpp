#include "Schematic.hpp"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace openstone {
namespace {

constexpr int viewInput = 0;
constexpr int areaInput = 1;
constexpr int extensionsInput = 2;
constexpr int directoriesInput = 3;
constexpr int maxInput = 4;

constexpr int pathsOutput = 0;
constexpr int mouseXOutput = 1;
constexpr int mouseYOutput = 2;
constexpr int draggingOutput = 3;
constexpr int droppedOutput = 4;

std::string lowercase(std::string_view text)
{
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](char letter) {
        return letter >= 'A' && letter <= 'Z' ? static_cast<char>(letter - 'A' + 'a') : letter;
    });
    return result;
}

std::vector<std::string> fileSuffixes(std::string_view text)
{
    constexpr std::string_view whitespace = " \t\r\n";
    std::vector<std::string> suffixes;
    for (auto token : text | std::views::split(',')) {
        std::string_view extension(token.begin(), token.end());
        const auto first = extension.find_first_not_of(whitespace);
        if (first == std::string_view::npos) {
            continue;
        }
        extension = extension.substr(first, extension.find_last_not_of(whitespace) - first + 1);
        if (extension.starts_with("*.")) {
            extension.remove_prefix(2);
        } else if (extension.starts_with('.')) {
            extension.remove_prefix(1);
        }
        if (!extension.empty()) {
            suffixes.push_back("." + lowercase(extension));
        }
    }
    return suffixes;
}

bool contains(yellow::Area area, green::Point point)
{
    const double right = static_cast<double>(area.x) + area.width;
    const double bottom = static_cast<double>(area.y) + area.height;
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(right) && std::isfinite(bottom) &&
           area.width != 0 && area.height != 0 &&
           point.x >= std::min<double>(area.x, right) && point.x <= std::max<double>(area.x, right) &&
           point.y >= std::min<double>(area.y, bottom) && point.y <= std::max<double>(area.y, bottom);
}

}

green::Value Schematic::readDropFiles(const SchematicNode& entry, int output) const
{
    static const FileDrag empty;
    const auto& current = entry.fileDrag ? entry.fileDrag->current() : empty;
    switch (output) {
    case pathsOutput: return current.paths;
    case mouseXOutput: return current.position.x;
    case mouseYOutput: return current.position.y;
    case draggingOutput: return entry.fileDrag && entry.fileDrag->preview.has_value();
    default: throw std::logic_error("Drop Files output has no value");
    }
}

SchematicNode* Schematic::findDropFiles(int id)
{
    const auto found = std::ranges::find(nodes_, id, &SchematicNode::id);
    return found != nodes_.end() && found->kind == PrimitiveKind::DropFiles ? &*found : nullptr;
}

bool Schematic::receiveFileDrop(int id, std::span<const DropFile> files, green::Point position, FileDropPhase phase)
{
    auto* entry = findDropFiles(id);
    if (!entry) {
        return false;
    }
    const auto max = std::get<std::int32_t>(readInput(id, maxInput));
    const auto area = std::get<yellow::Area>(readInput(id, areaInput));
    const auto view = std::get<yellow::View>(readInput(id, viewInput));
    if (files.empty() || max <= 0 || std::cmp_greater(files.size(), max) || view.panel == 0 ||
        !view.mapping.contains(position) || !contains(area, position)) {
        return false;
    }

    const auto suffixes = fileSuffixes(std::get<std::string>(readInput(id, extensionsInput)));
    const bool directories = std::get<bool>(readInput(id, directoriesInput));
    FileDrag drag{{}, position};
    drag.paths.reserve(files.size());
    for (const auto& file : files) {
        const auto name = lowercase(file.name);
        const bool accepted = file.directory ? directories : suffixes.empty() ||
            std::ranges::any_of(suffixes, [&](const auto& suffix) { return name.ends_with(suffix); });
        if (file.path.empty() || !accepted) {
            return false;
        }
        drag.paths.push_back(file.path);
    }

    publishFileDrag(*entry, std::move(drag), phase == FileDropPhase::Drop);
    refreshConnections();
    return true;
}

void Schematic::publishFileDrag(SchematicNode& entry, std::optional<FileDrag> next, bool dropped)
{
    auto& state = entry.fileDrag ? *entry.fileDrag : entry.fileDrag.emplace();
    const auto& previous = state.current();
    const auto& current = next ? *next : state.lastDrop;
    const bool pathsChanged = current.paths != previous.paths;
    const bool xChanged = current.position.x != previous.position.x;
    const bool yChanged = current.position.y != previous.position.y;
    const bool draggingChanged = state.preview.has_value() != (next.has_value() && !dropped);

    if (dropped) {
        state.lastDrop = std::move(*next);
        state.preview.reset();
    } else {
        state.preview = std::move(next);
    }

    if (pathsChanged || dropped) {
        fireOutput(entry, pathsOutput);
    }
    if (xChanged) {
        fireOutput(entry, mouseXOutput);
    }
    if (yChanged) {
        fireOutput(entry, mouseYOutput);
    }
    if (draggingChanged) {
        fireOutput(entry, draggingOutput);
    }
    if (dropped) {
        fireOutput(entry, droppedOutput);
    }
}

bool Schematic::moveFileDrag(int id, green::Point position)
{
    auto* entry = findDropFiles(id);
    if (!entry || !entry->fileDrag || !entry->fileDrag->preview) {
        return false;
    }
    auto& current = entry->fileDrag->preview->position;
    const bool xChanged = current.x != position.x;
    const bool yChanged = current.y != position.y;
    const auto view = std::get<yellow::View>(readInput(id, viewInput));
    if ((!xChanged && !yChanged) || !view.mapping.contains(position) ||
        !contains(std::get<yellow::Area>(readInput(id, areaInput)), position)) {
        return false;
    }
    current = position;
    if (xChanged) {
        fireOutput(*entry, mouseXOutput);
    }
    if (yChanged) {
        fireOutput(*entry, mouseYOutput);
    }
    refreshConnections();
    return true;
}

bool Schematic::leaveFileDrag(int id)
{
    auto* entry = findDropFiles(id);
    if (!entry || !entry->fileDrag || !entry->fileDrag->preview) {
        return false;
    }
    publishFileDrag(*entry, std::nullopt, false);
    refreshConnections();
    return true;
}

}
