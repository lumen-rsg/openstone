#pragma once

#include "Schematic.hpp"

#include <cstddef>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace openstone {

namespace desktop { class SchematicFile; }

struct Viewport {
    double x = 0;
    double y = 0;
    double zoom = 1;
};

struct NodePosition {
    int id;
    double x;
    double y;
};

enum class SchematicEdit {
    MoveNode, MoveNodes, CreateLink, DeleteLinks, MakeModule, AddNode, DeleteNodes, CutNodes, PasteNodes,
    DuplicateNodes, AlignNodes, DistributeNodes, EnablePanel, EditRuby,
};
enum class HistoryUpdate { Positions, Geometry, Navigation, Topology };

class Session {
public:
    explicit Session(Schematic::BitmapLoader bitmapLoader = {}, Schematic::BitmapBackend bitmapBackend = {},
                     Schematic::TextMeasurer textMeasurer = {}, Schematic::TextFileBackend textFiles = {},
                     Schematic::SystemInfoBackend systemInfo = {});
    ~Session();

    void requestSchematicClose();
    [[nodiscard]] bool schematicReadyToClose() const;
    // A held close keeps the prepared replacement until completeSchematicOpen succeeds.
    bool openSchematic(std::unique_ptr<Schematic> replacement, std::function<void()> beforeReplace = {},
                       std::string filePath = {});
    bool completeSchematicOpen();
    [[nodiscard]] std::exception_ptr takeLifecycleError() { return std::exchange(lifecycleError_, {}); }

    // A gesture owns its transaction through release, including any link edits made during it.
    bool beginEdit(SchematicEdit edit);
    void finishEdit();
    void clearHistory();
    [[nodiscard]] std::optional<SchematicEdit> historyEdit(bool redo) const;
    bool applyHistory(bool redo, const std::function<void(HistoryUpdate)>& beforeApply);

    [[nodiscard]] std::size_t activeLevel() const noexcept { return path_.size(); }
    [[nodiscard]] std::string levelName(std::size_t depth) const;
    [[nodiscard]] const Viewport& viewport(std::size_t depth) const;
    [[nodiscard]] const Viewport& activeViewport() const;
    [[nodiscard]] const Schematic& graphAt(std::size_t depth) const;
    [[nodiscard]] Schematic* findGraph(std::span<const int> path);
    [[nodiscard]] const Schematic* findGraph(std::span<const int> path) const;
    [[nodiscard]] Schematic& graph() { return *activeGraph_; }
    [[nodiscard]] const Schematic& graph() const { return *activeGraph_; }

    bool enterModule(int nodeId);
    bool navigate(std::size_t depth);
    int addNode(PrimitiveKind kind, double x, double y);
    int makeModule(std::span<const int> selection);
    void removeNode(int nodeId);
    void renameNode(int nodeId, std::string name);
    void moveNode(int nodeId, double x, double y);
    void moveNodes(std::span<const NodePosition> positions);
    void connect(SchematicLink link);
    void disconnectLink(SchematicLink link);
    void disconnectInput(int id, int input);
    void disconnectNode(int id);
    void setTemplateType(int id, std::optional<green::Type> type);
    void setModulePortType(int id, std::optional<green::Type> type);
    void setValue(int id, const green::Value& value);
    void setTickRate(int id, float rate);
    void setTickDisplaySync(int id, bool enabled);
    void setSystemSetting(int id, int index);
    void compileDsp(int id);
    void setPanelHidden(int id, bool hidden);
    void enableModulePanel(int id, ModulePanelKind panel);
    void setPanelShownInParent(int id, bool shown);
    void movePanelItem(int moduleId, std::span<const int> itemPath, green::Point position,
                       ModulePanelKind surface = ModulePanelKind::Front, ModulePanelKind item = ModulePanelKind::Front);
    void resizePanel(int id, float width, float height, ModulePanelKind surface = ModulePanelKind::Front);
    void setPanelItemAnchors(int id, std::span<const int> itemPath, ModulePanelKind surface, ModulePanelKind item,
                             std::uint8_t anchors);
    void loadBitmap(int id, std::string_view path);

    void copyNode(int nodeId);
    void copyNodes(std::span<const int> selection);
    void cutNodes(std::span<const int> selection);
    [[nodiscard]] bool canPaste() const;
    [[nodiscard]] bool canPasteSynchronized() const;
    int pasteNode(double x, double y, bool synchronized);
    std::vector<int> pasteNodes(double x, double y, bool synchronized);
    std::vector<int> duplicateNodes(std::span<const int> selection, double dx, double dy);
    void saveModuleToToolbox(int id, std::string group = {});
    [[nodiscard]] std::vector<std::string> toolboxNames() const;
    [[nodiscard]] const std::string& toolboxGroup(std::size_t index) const;
    [[nodiscard]] const SchematicNode& toolboxModule(std::size_t index) const;
    [[nodiscard]] const std::string& toolboxModuleId(std::size_t index) const { return toolbox_.at(index).id; }
    [[nodiscard]] bool toolboxModuleFavorite(std::size_t index) const { return toolbox_.at(index).favorite; }
    [[nodiscard]] bool primitiveFavorite(PrimitiveKind kind) const { return favoritePrimitives_.contains(kind); }
    int insertToolboxModule(std::size_t index, double x, double y);
    [[nodiscard]] std::size_t syncCount(int id) const;
    [[nodiscard]] bool hasSyncGroup(int id) const;
    void synchronizeAll(int id);
    void unsynchronize(int id, bool all);
    int followSync(int id, bool previous);
    void startSyncPainter(int id);
    [[nodiscard]] bool syncPainterActive() const;
    void stopSyncPainter() { painterSource_.clear(); }
    void paintSync(int id);
    void pan(double screenX, double screenY) noexcept;
    void zoomAt(double factor, double offsetX, double offsetY) noexcept;
    void centerOn(double worldX, double worldY) noexcept;
    void resetView() noexcept;

private:
    friend class desktop::SchematicFile;
    using Path = std::vector<int>;
    struct NodeCopy {
        SchematicNode node;
        std::vector<NodeCopy> children;
        std::vector<SchematicLink> links;
        std::map<int, Schematic::PortTypes> portTypes;
        std::uint64_t syncGroup = 0;
    };
    struct ToolboxEntry {
        NodeCopy content;
        std::string group;
        bool favorite = false;
        std::string id{};
    };
    struct SelectionCopy {
        std::vector<NodeCopy> nodes;
        std::vector<SchematicLink> links;
        std::map<int, Schematic::PortTypes> portTypes;
        double left = 0;
        double top = 0;
    };
    struct EditTarget {
        Schematic* graph;
        std::vector<int> ids;
    };
    struct History;

    [[nodiscard]] std::vector<const Schematic*> moduleGraphs() const;
    [[nodiscard]] std::vector<const Schematic*> synchronizedGraphs(const Schematic& graph) const;
    [[nodiscard]] std::vector<EditTarget> editTargets(std::span<const int> ids);
    [[nodiscard]] std::vector<EditTarget> editTargets(std::initializer_list<int> ids)
    {
        return editTargets(std::span{ids.begin(), ids.size()});
    }
    void edit(std::span<const int> ids, const std::function<void(Schematic&, const std::vector<int>&)>& apply);
    void applyEdit(const std::vector<EditTarget>& targets,
                   const std::function<void(Schematic&, const std::vector<int>&)>& apply);
    void edit(std::initializer_list<int> ids, const std::function<void(Schematic&, const std::vector<int>&)>& apply)
    {
        edit(std::span{ids.begin(), ids.size()}, apply);
    }
    void editPanelItem(int id, std::span<const int> itemPath, ModulePanelKind surface, ModulePanelKind item,
                       const std::function<void(Schematic&, const ModulePanel&)>& apply);
    void forgetViewports(std::span<const int> subtree);
    void rememberRemovedNode(const Schematic& graph, int id);
    static NodeCopy snapshotNode(const Schematic& graph, int id, bool includeSequenceTakes = false);
    [[nodiscard]] SelectionCopy snapshotSelection(std::span<const int> selection) const;
    static void restoreGraph(Schematic& graph, const NodeCopy& content, bool restoreSync = false, bool buildRuntime = true);
    int insertCopy(Schematic& graph, const NodeCopy& content, double x, double y);
    std::vector<int> insertSelection(const SelectionCopy& copy, double x, double y);
    [[nodiscard]] static bool sameModule(const Schematic& a, const Schematic& b);
    void joinSyncGroup(Schematic& source, const std::vector<Schematic*>& members);

    std::unique_ptr<Schematic> root_;
    Schematic* activeGraph_ = root_.get();
    std::vector<int> path_;
    std::map<Path, Viewport> viewports_;
    std::optional<SelectionCopy> clipboard_;
    std::vector<ToolboxEntry> toolbox_;
    std::set<PrimitiveKind> favoritePrimitives_;
    Path clipboardSource_;
    Path painterSource_;
    std::uint64_t nextSyncGroup_ = 1;
    std::unique_ptr<History> history_;
    std::exception_ptr lifecycleError_;
    enum class CloseState { Open, Emitting, Requested };
    CloseState closeState_ = CloseState::Open;
    struct PendingSchematic {
        std::unique_ptr<Schematic> graph;
        std::function<void()> beforeReplace;
    };
    std::optional<PendingSchematic> pendingSchematic_;
};

}
