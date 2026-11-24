#include "Schematic.hpp"

#include <algorithm>

namespace openstone {

green::Value Schematic::readHttpResponse(const SchematicNode& entry, int output) const
{
    if (!entry.capturedSignals.empty()) return entry.capturedSignals.at(output);
    const auto type = outputType(entry.id, output);
    if (type == green::Type::Boolean) return false;
    if (type == green::Type::Bitmap) return yellow::Bitmap{};
    return std::string{};
}

const Schematic* Schematic::httpRequestOwner(const HttpRequest& request) const
{
    const auto* owner = &rootGraph();
    for (const int id : request.owner) {
        owner = owner->module(id);
        if (!owner) return nullptr;
    }
    const auto found = std::ranges::find(owner->nodes_, request.node, &SchematicNode::id);
    return found != owner->nodes_.end() && request.token != 0 && found->httpRequestToken == request.token
        ? owner : nullptr;
}

bool Schematic::hasPendingHttpRequest(const HttpRequest& request) const
{
    return httpRequestOwner(request) != nullptr;
}

bool Schematic::finishHttpRequest(const HttpRequest& request, HttpResponse response)
{
    auto* owner = const_cast<Schematic*>(httpRequestOwner(request));
    if (!owner) return false;
    auto& entry = owner->node(request.node);
    entry.httpRequestToken = 0;
    const bool success = response.error.empty();
    const bool hasPayload = request.kind != HttpResponseKind::File;
    if (entry.capturedSignals.empty() && hasPayload) {
        entry.capturedSignals.push_back(request.kind == HttpResponseKind::Text
            ? green::Value{std::string{}} : green::Value{yellow::Bitmap{}});
    }
    if (success && hasPayload) {
        if (request.kind == HttpResponseKind::Text) entry.capturedSignals[0] = std::move(response.text);
        else entry.capturedSignals[0] = std::move(response.image);
    }
    entry.capturedSignals.resize(hasPayload ? 3 : 2);
    entry.capturedSignals[hasPayload ? 1 : 0] = success;
    entry.capturedSignals.back() = std::move(response.error);
    if (success && hasPayload) owner->fireOutput(entry, 0);
    owner->fireOutput(entry, hasPayload ? 1 : 0);
    owner->fireOutput(entry, hasPayload ? 2 : 1);
    owner->refreshConnections();
    return true;
}

void Schematic::receiveHttpRequest(SchematicNode& entry, int input)
{
    const bool file = entry.kind == PrimitiveKind::HttpFileRequest;
    if (input != (file ? 5 : 3)) return;

    const auto text = [&](int port) { return std::get<std::string>(readInput(entry.id, port)); };
    HttpRequest request{
        .owner = path_, .node = entry.id,
        .kind = file ? HttpResponseKind::File : entry.kind == PrimitiveKind::HttpImageRequest
            ? HttpResponseKind::Image : HttpResponseKind::Text,
        .url = text(0),
        .names = std::get<green::StringArray>(readInput(entry.id, 1)),
        .values = std::get<green::StringArray>(readInput(entry.id, 2)),
    };
    if (file) {
        request.destination = text(3);
        request.overwrite = std::get<bool>(readInput(entry.id, 4));
    }
    static std::atomic<std::uint64_t> nextToken = 1;
    entry.httpRequestToken = request.token = nextToken.fetch_add(1, std::memory_order_relaxed);
    const auto& send = rootGraph().systemInfo_.sendHttp;
    std::string error;
    if (request.names.size() != request.values.size()) error = "Parameter names and values must have equal lengths";
    else if (file && (request.destination.empty() || request.destination.find('\0') != std::string::npos)) {
        error = "Choose a destination file";
    } else if (!send) error = "HTTP requests are unavailable in this host";
    if (error.empty()) {
        try {
            send(request);
            return;
        } catch (const std::exception& failure) {
            error = failure.what();
        }
    }
    static_cast<void>(finishHttpRequest(request, {.error = std::move(error)}));
}

}
