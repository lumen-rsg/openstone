#include "Schematic.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <random>

namespace openstone {
namespace {

namespace fs = std::filesystem;
using green::StringArray;
using green::Value;
using namespace std::string_view_literals;

constexpr std::array okButtons{"ok"sv};
constexpr std::array okCancelButtons{"ok"sv, "cancel"sv};
constexpr std::array retryCancelButtons{"retry"sv, "cancel"sv};
constexpr std::array yesNoButtons{"yes"sv, "no"sv};
constexpr std::array yesNoCancelButtons{"yes"sv, "no"sv, "cancel"sv};
constexpr std::array abortRetryIgnoreButtons{"abort"sv, "retry"sv, "ignore"sv};

constexpr std::pair<std::string_view, std::span<const std::string_view>> messageBoxStyles[]{
    {"yesnocancel", yesNoCancelButtons}, {"yesno", yesNoButtons},
    {"abortretryignore", abortRetryIgnoreButtons}, {"retrycancel", retryCancelButtons}, {"okcancel", okCancelButtons},
};

std::uint64_t nextAppDialogToken()
{
    static std::atomic<std::uint64_t> next = 0;
    return next.fetch_add(1, std::memory_order_relaxed) + 1;
}

void consumeAppDialog(std::deque<AppDialogRequest>& queue, std::uint64_t token)
{
    const auto first = std::ranges::find(queue, token, &AppDialogRequest::token);
    queue.erase(queue.begin(), std::next(first));
}

fs::path utf8Path(std::string_view text)
{
    return fs::path(std::u8string(text.begin(), text.end()));
}

std::string pathString(const fs::path& path)
{
    const auto encoded = path.u8string();
    return std::string(encoded.begin(), encoded.end());
}

std::size_t nextCharacter(std::string_view text, std::size_t index)
{
    do {
        ++index;
    } while (index < text.size() && (static_cast<unsigned char>(text[index]) & 0xc0) == 0x80);
    return index;
}

bool matchesFilename(std::string_view filter, std::string_view filename)
{
    if (filter == "*.*") return true;
    std::size_t pattern = 0, name = 0, star = std::string_view::npos, retry = 0;
    while (name < filename.size()) {
        if (pattern < filter.size() && filter[pattern] == '*') {
            star = pattern++;
            retry = name;
            continue;
        }
        const auto next = nextCharacter(filename, name);
        const auto character = filename.substr(name, next - name);
        if (pattern < filter.size() && filter[pattern] == '?') {
            ++pattern;
        } else if (filter.substr(pattern).starts_with(character)) {
            pattern += character.size();
        } else {
            if (star == std::string_view::npos) return false;
            name = retry = nextCharacter(filename, retry);
            pattern = star + 1;
            continue;
        }
        name = next;
    }
    return std::ranges::all_of(filter.substr(pattern), [](char c) { return c == '*'; });
}

StringArray findFiles(std::string_view filter)
{
    StringArray matches;
    if (filter.empty() || filter.contains('\0')) return matches;
    const auto path = utf8Path(filter);
    const auto pattern = pathString(path.filename());
    if (pattern.empty()) return matches;
    const auto folder = path.has_parent_path() ? path.parent_path() : fs::path(".");
    std::error_code error;
    for (fs::directory_iterator files(folder, error), end; !error && files != end; files.increment(error)) {
        std::error_code fileError;
        if (!files->is_regular_file(fileError)) continue;
        auto filename = pathString(files->path().filename());
        if (matchesFilename(pattern, filename)) matches.push_back(std::move(filename));
    }
    std::ranges::sort(matches);
    return matches;
}

std::string makeGuid()
{
    std::array<unsigned, 16> bytes{};
    thread_local std::random_device random;
    std::uniform_int_distribution<unsigned> byte(0, 255);
    std::ranges::generate(bytes, [&] { return byte(random); });
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    constexpr std::string_view hex = "0123456789ABCDEF";
    std::string result;
    result.reserve(38);
    result += '{';
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) result += '-';
        result += hex[bytes[i] >> 4];
        result += hex[bytes[i] & 0x0f];
    }
    result += '}';
    return result;
}

}

void Schematic::setSchematicFilePath(std::string filePath)
{
    auto& root = rootGraph();
    if (root.location_->filePath == filePath) return;
    root.location_->filePath = std::move(filePath);
    const auto publish = [](const auto& walk, Schematic& level) -> void {
        for (auto& entry : level.nodes_) {
            if (entry.kind == PrimitiveKind::SchematicFilePath) level.fireOutputs(entry.id);
        }
        for (auto& [id, child] : level.modules_) walk(walk, *child);
    };
    publish(publish, root);
    root.refreshConnections();
}

Value Schematic::readFileUtility(const SchematicNode& entry, int output) const
{
    if (entry.kind == PrimitiveKind::SchematicFilePath) {
        const auto path = utf8Path(rootGraph().location_->filePath);
        return pathString(output == 0 ? path.parent_path() : path.filename());
    }
    if (entry.kind == PrimitiveKind::FindFiles) {
        const auto* files = entry.capturedSignals.empty() ? nullptr : &std::get<StringArray>(entry.capturedSignals.front());
        if (output == 1) return files ? static_cast<std::int32_t>(files->size()) : 0;
        return files ? *files : StringArray{};
    }
    if (!entry.capturedSignals.empty()) return entry.capturedSignals.front();
    if (entry.kind == PrimitiveKind::MessageBox) return false;
    if (entry.kind == PrimitiveKind::ColorDialog) return yellow::Color{};
    return std::string{};
}

const Schematic* Schematic::appDialogOwner(const AppDialogRequest& request) const
{
    const auto* owner = this;
    for (const int id : request.ownerPath) {
        owner = owner->module(id);
        if (!owner) return nullptr;
    }
    const auto found = std::ranges::find(owner->nodes_, request.node, &SchematicNode::id);
    if (found == owner->nodes_.end() || found->kind != request.kind) return nullptr;
    if (request.rubyMessageBox && (!found->rubyCode || !found->rubyCode->running)) return nullptr;
    return owner;
}

const AppDialogRequest* Schematic::pendingAppDialog() const
{
    const auto& root = rootGraph();
    const auto pending = std::ranges::find_if(root.appDialogs_, [&](const auto& request) {
        return root.appDialogOwner(request) != nullptr;
    });
    return pending == root.appDialogs_.end() ? nullptr : &*pending;
}

bool Schematic::finishAppDialog(std::uint64_t token, bool accepted, std::string path,
                                std::optional<yellow::Color> selectedColor)
{
    auto& root = rootGraph();
    const auto* pending = root.pendingAppDialog();
    if (!pending || pending->token != token || pending->rubyMessageBox) return false;
    auto& owner = *const_cast<Schematic*>(root.appDialogOwner(*pending));
    auto& entry = owner.node(pending->node);
    const auto alpha = pending->alpha;
    consumeAppDialog(root.appDialogs_, token);
    if (entry.kind == PrimitiveKind::MessageBox) {
        entry.capturedSignals.assign(1, accepted);
        owner.fireOutput(entry, 0);
    } else if (entry.kind == PrimitiveKind::ColorDialog) {
        if (accepted && selectedColor) {
            const auto color = (selectedColor->argb & 0x00ffffffU) | (static_cast<std::uint32_t>(alpha) << 24);
            entry.capturedSignals.assign(1, yellow::Color{color});
            owner.fireOutput(entry, 0);
            owner.fireOutput(entry, 1);
        } else {
            owner.fireOutput(entry, 2);
        }
    } else if (accepted && !path.empty()) {
        entry.capturedSignals.assign(1, std::move(path));
        owner.fireOutput(entry, 0);
        owner.fireOutput(entry, 1);
    } else {
        owner.fireOutput(entry, 2);
    }
    root.refreshConnections();
    return true;
}

void Schematic::createRubyMessageBox(SchematicNode& entry, green::RubyValue callbackId, RubyMessageBoxOptions options)
{
    auto& root = rootGraph();
    auto& queue = root.appDialogs_;
    std::erase_if(queue, [&](const auto& request) { return !root.appDialogOwner(request); });

    std::ranges::transform(options.type, options.type.begin(), [](char character) {
        return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
    });
    const auto hasFlag = [&](std::string_view flag) { return options.type.contains(flag); };
    std::span<const std::string_view> buttons = okButtons;
    for (const auto& [flag, arrangement] : messageBoxStyles) {
        if (hasFlag(flag)) {
            buttons = arrangement;
            break;
        }
    }

    AppDialogRequest request{.token = nextAppDialogToken(), .ownerPath = path_, .node = entry.id,
                             .kind = PrimitiveKind::Ruby, .title = std::move(options.title),
                             .message = std::move(options.text)};
    if (hasFlag("information") || hasFlag("asterisk")) request.icon = 1;
    else if (hasFlag("question")) request.icon = 2;
    else if (hasFlag("exclamation") || hasFlag("exclaimation") || hasFlag("warning")) request.icon = 3;
    else if (hasFlag("stop") || hasFlag("error") || hasFlag("hand")) request.icon = 4;

    constexpr std::array defaultButtons{"defbutton1"sv, "defbutton2"sv, "defbutton3"sv, "defbutton4"sv};
    const auto selectedDefault = std::ranges::find_if(defaultButtons, hasFlag);
    const auto index = static_cast<std::size_t>(selectedDefault - defaultButtons.begin());
    request.rubyMessageBox = RubyMessageBox{std::move(callbackId), buttons, index < buttons.size() ? index : 0};
    queue.push_back(std::move(request));
}

void Schematic::clearRubyMessageBoxes(int id)
{
    std::erase_if(rootGraph().appDialogs_, [&](const auto& request) {
        return request.rubyMessageBox && request.node == id && request.ownerPath == path_;
    });
}

bool Schematic::finishRubyMessageBox(std::uint64_t token, std::string_view result)
{
    auto& root = rootGraph();
    const auto* pending = root.pendingAppDialog();
    if (!pending || pending->token != token || !pending->rubyMessageBox ||
        !std::ranges::contains(pending->rubyMessageBox->buttons, result)) {
        return false;
    }
    auto& owner = *const_cast<Schematic*>(root.appDialogOwner(*pending));
    auto& entry = owner.node(pending->node);
    auto& code = *entry.rubyCode;
    const auto callbackId = pending->rubyMessageBox->callbackId;
    consumeAppDialog(root.appDialogs_, token);

    const auto now = owner.rubyNow();
    std::optional<RubyReply> reply;
    try {
        owner.updateRubyContext(entry);
        reply = code.instance->messageBoxFinished(callbackId, result, owner.rubySeconds(now));
    } catch (const std::exception& error) {
        code.error = true;
        code.status = error.what();
        return true;
    }
    if (reply) {
        owner.publishRuby(entry, std::move(*reply), now);
        root.refreshConnections();
    }
    return true;
}

void Schematic::receiveFileUtility(SchematicNode& entry, int input)
{
    using enum PrimitiveKind;
    const auto id = entry.id;
    const auto text = [&](int port) { return std::get<std::string>(readInput(id, port)); };
    const auto integer = [&](int port) { return std::get<std::int32_t>(readInput(id, port)); };

    if (entry.kind == FindFiles) {
        if (input == 1) {
            entry.capturedSignals.assign(1, findFiles(text(0)));
            fireOutputs(id);
        }
        return;
    }
    if (input != 0) return;
    if (entry.kind == GenerateGuid) {
        entry.capturedSignals.assign(1, makeGuid());
        fireOutputs(id);
        return;
    }

    auto& root = rootGraph();
    auto& queue = root.appDialogs_;
    std::erase_if(queue, [&](const auto& request) { return !root.appDialogOwner(request); });
    if (std::ranges::any_of(queue, [&](const auto& request) { return request.ownerPath == path_ && request.node == id; })) {
        return;
    }
    AppDialogRequest request{.token = nextAppDialogToken(), .ownerPath = path_, .node = id, .kind = entry.kind};
    if (entry.kind == ColorDialog) {
        request.initialColor = std::get<yellow::Color>(readInput(id, 2));
        request.alpha = static_cast<std::uint8_t>(std::clamp(integer(1), 0, 255));
    } else if (entry.kind == FileDialog) {
        request.filter = text(1);
        request.type = integer(2);
        request.title = text(3);
        request.folder = text(4);
        request.name = text(5);
        request.suffix = text(6);
    } else if (entry.kind == FolderDialog) {
        request.title = text(1);
        request.folder = text(2);
    } else if (entry.kind == MessageBox) {
        request.message = text(1);
        request.title = text(2);
        request.icon = integer(3);
        request.type = integer(4);
    } else {
        return;
    }
    queue.push_back(std::move(request));
}

}
