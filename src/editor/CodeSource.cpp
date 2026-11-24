#include "Schematic.hpp"

#include <stdexcept>

namespace openstone {

void Schematic::refreshCodeSources()
{
    auto& root = rootGraph();
    if (root.refreshingCodeSources_) return;
    std::vector<std::pair<Schematic*, int>> targets;
    bool busy = false;
    const auto visit = [&](auto&& self, Schematic& graph) -> void {
        busy |= graph.runtime_ && graph.runtime_->busy();
        for (const auto& entry : graph.nodes_) {
            if (const auto port = entry.codeSourcePort(false); port && graph.hasValueInput(entry.id, *port)) {
                targets.emplace_back(&graph, entry.id);
            }
            if (auto* child = graph.module(entry.id)) self(self, *child);
        }
    };
    visit(visit, root);
    if (busy || targets.empty()) return;
    root.refreshingCodeSources_ = true;
    struct Reset { bool& flag; ~Reset() { flag = false; } } reset{root.refreshingCodeSources_};
    // A code box can supply another box's source, directly or through modules.
    // Stop cyclic/oscillating bindings rather than rebuilding indefinitely.
    for (std::size_t pass = 0; pass <= targets.size(); ++pass) {
        bool changed = false;
        for (const auto& [graph, id] : targets) {
            auto& entry = graph->node(id);
            if (!entry.codeSourceDirty) continue;
            const auto source = std::get<std::string>(graph->readInput(id, *entry.codeSourcePort(false)));
            const auto& old = *entry.rubyCode;
            if (source == old.source) { entry.codeSourceDirty = false; continue; }
            // Keep the applied source on reopen until its producer actually fires;
            // a stopped Ruby producer's default String is not a new source event.
            entry.codeSourceDirty = false;
            if (old.running) {
                graph->setRubyDraft(id, source);
                graph->runRuby(id);
            } else {
                auto next = rubyCode(source, source, old.manualPorts ? std::optional{old.ports} : std::nullopt);
                next.boxSize = old.boxSize;
                next.savedState = old.savedState;
                graph->installRubyCode(entry, std::move(next));
            }
            changed = true;
        }
        if (!changed) return;
    }
    throw std::invalid_argument("Code-source bindings did not settle");
}

}
