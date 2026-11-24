#include "Schematic.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <limits>

namespace openstone {
namespace {

constexpr int listInput = 3;
constexpr int openInput = 4;
constexpr int checkInput = 5;
constexpr int styleInput = 6;
constexpr int selectedStringOutput = 0;
constexpr int selectedIndexOutput = 1;
constexpr int countOutput = 2;
constexpr int canceledOutput = 3;
constexpr int popupView = 0;
constexpr int popupFont = 2;
constexpr int popupTextColor = 3;
constexpr int popupBackground = 4;
constexpr int popupOptions = 5;
constexpr int popupRows = 6;
constexpr int popupSet = 7;
constexpr int popupTextOutput = 1;
constexpr int popupIndexOutput = 2;
constexpr int indexListInput = 0;
constexpr int indexSortInput = 1;
constexpr int indexSetInput = 2;
constexpr int indexSelectorIndexOutput = 0;

std::uint64_t nextDropListToken()
{
    // A popup may deliver its close notification after a project switch.
    static std::atomic<std::uint64_t> next{0};
    return ++next;
}

std::string_view trim(std::string_view value)
{
    constexpr std::string_view whitespace = " \t\r\n";
    const auto first = value.find_first_not_of(whitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    return value.substr(first, value.find_last_not_of(whitespace) - first + 1);
}

std::vector<std::string_view> listTokens(std::string_view list, std::string_view delimiters = ",")
{
    std::vector<std::string_view> tokens;
    if (list.empty()) {
        return tokens;
    }
    for (;;) {
        const auto separator = list.find_first_of(delimiters);
        tokens.push_back(trim(list.substr(0, separator)));
        if (separator == std::string_view::npos) {
            return tokens;
        }
        list.remove_prefix(separator + 1);
    }
}

DropListContents parseDropList(std::string_view list)
{
    DropListContents contents;
    const auto tokens = listTokens(list);
    std::vector<int> path{0};
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const auto token = tokens[i];
        auto& menu = contents.menus[path.back()];
        if (token == ">>") {
            // The opening form is >>, Name, <<. A standalone >> closes a submenu.
            if (i + 2 < tokens.size() && tokens[i + 2] == "<<" &&
                !tokens[i + 1].empty() && tokens[i + 1] != ">>" && tokens[i + 1] != "<<") {
                const auto submenu = static_cast<int>(contents.menus.size());
                menu.push_back({.text = std::string(tokens[i + 1]), .submenu = submenu});
                contents.menus.emplace_back();
                path.push_back(submenu);
                i += 2;
            } else if (path.size() > 1) {
                path.pop_back();
            }
        } else if (token == "-----") {
            menu.emplace_back();
        } else if (token != "<<") {
            const auto index = static_cast<int>(contents.options.size());
            contents.options.emplace_back(token);
            menu.push_back({.text = std::string(token), .optionIndex = index});
        }
    }
    return contents;
}

std::vector<std::string> popupOptionsFrom(std::string_view list)
{
    const auto tokens = listTokens(list);
    return {tokens.begin(), tokens.end()};
}

bool validIndex(int index, std::size_t count)
{
    return index >= 0 && std::cmp_less(index, count);
}

int indexOrFirst(int index, std::size_t count)
{
    return validIndex(index, count) ? index : 0;
}

void readLayout(std::string style, DropListRequest& request)
{
    std::ranges::transform(style, style.begin(), [](char letter) {
        return letter >= 'A' && letter <= 'Z' ? static_cast<char>(letter - 'A' + 'a') : letter;
    });
    const auto value = trim(style);
    if (value.empty()) {
        return;
    }
    if (value == "scroll") {
        request.layout = DropListLayout::Scroll;
        return;
    }
    int rows = 0;
    const auto end = value.data() + value.size();
    const auto result = std::from_chars(value.data(), end, rows);
    if (result.ec == std::errc{} && result.ptr == end && rows > 0) {
        request.layout = DropListLayout::FixedColumns;
        request.rowsPerColumn = rows;
    }
}

}

green::Value Schematic::readDropList(const SchematicNode& entry, int output) const
{
    const auto& selected = entry.dropListSelection;
    switch (output) {
    case selectedStringOutput: return selected ? selected->text : std::string{};
    case selectedIndexOutput: return selected ? selected->index : std::int32_t{-1};
    case countOutput: {
        const auto list = std::get<std::string>(readInput(entry.id, listInput));
        const auto count = parseDropList(list).options.size();
        return static_cast<std::int32_t>(std::min<std::size_t>(count, std::numeric_limits<std::int32_t>::max()));
    }
    default: throw std::logic_error("DropList output has no value");
    }
}

std::vector<std::string> Schematic::indexSelectorOptions(int id) const
{
    const auto list = std::get<std::string>(readInput(id, indexListInput));
    std::vector<std::string> options;
    for (const auto token : listTokens(list, ",\r\n")) {
        if (!token.empty()) options.emplace_back(token);
    }
    if (std::get<bool>(readInput(id, indexSortInput))) {
        std::ranges::sort(options);
    }
    return options;
}

green::Value Schematic::readIndexSelector(const SchematicNode& entry, int output) const
{
    auto options = indexSelectorOptions(entry.id);
    const auto index = indexOrFirst(std::get<std::int32_t>(*entry.localValue), options.size());
    if (output == indexSelectorIndexOutput) {
        return static_cast<std::int32_t>(index);
    }
    return options.empty() ? std::string{} : std::move(options[index]);
}

bool Schematic::openIndexSelector(int id, green::Point position)
{
    const auto& entry = node(id);
    if (entry.kind != PrimitiveKind::IndexSelector || !std::isfinite(position.x) || !std::isfinite(position.y)) {
        return false;
    }
    DropListContents contents;
    contents.options = indexSelectorOptions(id);
    if (contents.options.empty()) return false;

    auto& menu = contents.menus.front();
    menu.reserve(contents.options.size());
    for (const auto& option : contents.options) {
        menu.push_back({.text = option, .optionIndex = static_cast<int>(menu.size())});
    }
    const auto selected = indexOrFirst(std::get<std::int32_t>(*entry.localValue), contents.options.size());
    auto& root = rootGraph();
    root.dropListRequest_ = DropListRequest{nextDropListToken(), path_, id, {}, position, std::move(contents),
                                           selected, DropListLayout::Scroll, 0, PrimitiveKind::IndexSelector};
    return true;
}

void Schematic::receiveIndexSelectorTrigger(SchematicNode& entry, int input)
{
    const auto options = indexSelectorOptions(entry.id);
    const auto previous = std::get<std::int32_t>(*entry.localValue);
    auto selected = indexOrFirst(previous, options.size());
    if (hasValueInput(entry.id, indexSetInput)) {
        const auto requested = std::get<std::string>(readInput(entry.id, indexSetInput));
        if (!requested.empty() && (options.empty() || options[selected] != requested)) {
            const auto found = std::ranges::find(options, requested);
            if (found != options.end()) selected = static_cast<int>(found - options.begin());
        }
    }
    if (input == indexSetInput && selected == previous) return;
    auto& pending = rootGraph().dropListRequest_;
    if (pending && pending->kind == entry.kind && pending->owner == entry.id && pending->ownerPath == path_) {
        pending.reset();
    }
    entry.localValue = static_cast<std::int32_t>(selected);
    fireOutputs(entry.id);
}

const std::optional<DropListRequest>& Schematic::pendingDropList() const
{
    return rootGraph().dropListRequest_;
}

DropListSelection Schematic::popupListSelection(const SchematicNode& entry,
                                                const std::vector<std::string>& options) const
{
    if (options.empty()) {
        return {-1, {}};
    }
    const auto index = entry.popupListIndex ? *entry.popupListIndex
                                          : std::get<std::int32_t>(readInput(entry.id, popupSet));
    const auto selected = indexOrFirst(index, options.size());
    return {selected, options[selected]};
}

green::Value Schematic::readPopupList(const SchematicNode& entry, int output) const
{
    if (output == popupView) {
        return readInput(entry.id, popupView);
    }
    auto options = popupOptionsFrom(std::get<std::string>(readInput(entry.id, popupOptions)));
    auto selection = popupListSelection(entry, options);
    if (output == popupIndexOutput) {
        return selection.index;
    }
    return std::move(selection.text);
}

yellow::PopupListCommand Schematic::readPopupListDrawing(const SchematicNode& entry, yellow::Area area) const
{
    const auto options = popupOptionsFrom(std::get<std::string>(readInput(entry.id, popupOptions)));
    return {{path_, entry.id}, area, std::get<yellow::Font>(readInput(entry.id, popupFont)),
            std::get<yellow::Color>(readInput(entry.id, popupTextColor)),
            std::get<yellow::Color>(readInput(entry.id, popupBackground)),
            popupListSelection(entry, options).text, !options.empty()};
}

bool Schematic::openPopupList(int id)
{
    const auto entry = std::ranges::find(nodes_, id, &SchematicNode::id);
    if (entry == nodes_.end() || entry->kind != PrimitiveKind::PopupList) {
        return false;
    }
    auto view = std::get<yellow::View>(readInput(id, popupView));
    DropListContents contents;
    contents.options = popupOptionsFrom(std::get<std::string>(readInput(id, popupOptions)));
    if (view.panel == 0 || contents.options.empty()) {
        return false;
    }
    auto& menu = contents.menus.front();
    menu.reserve(contents.options.size());
    for (const auto& option : contents.options) {
        menu.push_back({.text = option, .optionIndex = static_cast<int>(menu.size())});
    }
    const auto selected = popupListSelection(*entry, contents.options).index;
    const auto rows = std::get<std::int32_t>(readInput(id, popupRows));
    auto& root = rootGraph();
    root.dropListRequest_ = DropListRequest{nextDropListToken(), path_, id, std::move(view), {},
                                           std::move(contents), selected, DropListLayout::Scroll,
                                           rows > 0 ? rows : 8, PrimitiveKind::PopupList};
    return true;
}

void Schematic::publishPopupListSelection(SchematicNode& entry, std::int32_t index)
{
    entry.popupListIndex = index;
    fireOutput(entry, popupTextOutput);
    fireOutput(entry, popupIndexOutput);
}

void Schematic::receivePopupListTrigger(SchematicNode& entry, int input)
{
    if (input == popupView) {
        fireOutput(entry, popupView);
        return;
    }
    if (input != popupOptions && input != popupSet) {
        return;
    }
    const auto options = popupOptionsFrom(std::get<std::string>(readInput(entry.id, popupOptions)));
    const auto index = input == popupSet ? std::get<std::int32_t>(readInput(entry.id, popupSet))
                                       : popupListSelection(entry, options).index;
    if (input == popupSet && !validIndex(index, options.size())) {
        return;
    }
    auto& pending = rootGraph().dropListRequest_;
    if (pending && pending->kind == PrimitiveKind::PopupList && pending->owner == entry.id &&
        pending->ownerPath == path_) {
        pending.reset();
    }
    publishPopupListSelection(entry, index);
}

void Schematic::receiveDropListTrigger(SchematicNode& entry, int input)
{
    if (input == listInput) {
        fireOutput(entry, countOutput);
        return;
    }
    if (input != openInput) {
        return;
    }
    auto view = std::get<yellow::View>(readInput(entry.id, 0));
    auto contents = parseDropList(std::get<std::string>(readInput(entry.id, listInput)));
    const green::Point position{std::get<float>(readInput(entry.id, 1)), std::get<float>(readInput(entry.id, 2))};
    if (view.panel == 0 || contents.options.empty() || !std::isfinite(position.x) || !std::isfinite(position.y)) {
        return;
    }
    const int check = std::get<std::int32_t>(readInput(entry.id, checkInput));
    auto& root = rootGraph();
    DropListRequest request{nextDropListToken(), path_, entry.id, std::move(view), position, std::move(contents), check};
    readLayout(std::get<std::string>(readInput(entry.id, styleInput)), request);
    root.dropListRequest_ = std::move(request);
}

void Schematic::createRubyDropList(SchematicNode& entry, int input, green::RubyValue callbackId,
                                       RubyDropListOptions options)
{
    auto view = std::get<yellow::View>(readInput(entry.id, input));
    if (!panelOwner(view)) return;
    const auto anchor = view.mapping.toPanel(options.position);
    if (!std::isfinite(anchor.x) || !std::isfinite(anchor.y)) return;

    auto contents = parseDropList(options.items);
    if (contents.options.empty()) return;

    DropListRequest request{nextDropListToken(), path_, entry.id, std::move(view), options.position,
                            std::move(contents), options.checkedIndex};
    request.kind = PrimitiveKind::Ruby;
    request.disabledIndex = options.disabledIndex;
    request.ruby = RubyDropListSource{input, std::move(callbackId)};
    readLayout(std::move(options.style), request);
    rootGraph().dropListRequest_ = std::move(request);
}

void Schematic::clearRubyDropList(int id)
{
    auto& pending = rootGraph().dropListRequest_;
    if (pending && pending->ruby && pending->owner == id && pending->ownerPath == path_) pending.reset();
}

void Schematic::finishDropList(std::uint64_t token, std::optional<int> selection)
{
    auto& root = rootGraph();
    auto& pending = root.dropListRequest_;
    if (!pending || pending->token != token ||
        (selection && (!validIndex(*selection, pending->contents.options.size()) || *selection == pending->disabledIndex))) {
        return;
    }
    auto request = std::move(*pending);
    pending.reset();
    auto* owner = &root;
    for (const int moduleId : request.ownerPath) {
        owner = owner->module(moduleId);
        if (!owner) {
            return;
        }
    }
    const auto entry = std::ranges::find(owner->nodes_, request.owner, &SchematicNode::id);
    if (entry == owner->nodes_.end() || entry->kind != request.kind) {
        return;
    }
    if (request.ruby) {
        if (!selection || !entry->rubyCode->running) return;
        const auto view = std::get<yellow::View>(owner->readInput(entry->id, request.ruby->input));
        if (!view.isSamePanel(request.view)) return;

        auto& code = *entry->rubyCode;
        const auto now = owner->rubyNow();
        std::optional<RubyReply> reply;
        try {
            owner->updateRubyContext(*entry);
            reply = code.instance->dropListFinished(request.ruby->callbackId, *selection,
                                                     request.contents.options[*selection], owner->rubySeconds(now));
        } catch (const std::exception& error) {
            code.error = true;
            code.status = error.what();
            return;
        }
        if (reply) {
            owner->publishRuby(*entry, std::move(*reply), now);
            owner->refreshConnections();
        }
        return;
    }
    if (request.kind == PrimitiveKind::IndexSelector) {
        if (selection && owner->indexSelectorOptions(entry->id) == request.contents.options) {
            entry->localValue = static_cast<std::int32_t>(*selection);
            owner->fireOutputs(entry->id);
            owner->refreshConnections();
        }
        return;
    }
    if (request.kind == PrimitiveKind::PopupList) {
        if (selection) {
            owner->publishPopupListSelection(*entry, *selection);
            owner->refreshConnections();
        }
        return;
    }
    if (!selection) {
        owner->fireOutput(*entry, canceledOutput);
        return;
    }
    entry->dropListSelection = DropListSelection{*selection, std::move(request.contents.options[*selection])};
    owner->fireOutput(*entry, selectedStringOutput);
    owner->fireOutput(*entry, selectedIndexOutput);
}

}
