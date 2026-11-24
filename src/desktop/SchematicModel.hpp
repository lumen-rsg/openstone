#pragma once

#include "Session.hpp"
#include "AudioPatch.hpp"
#include "LibraryModel.hpp"

#include <QAbstractListModel>
#include <QChronoTimer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QColor>
#include <QPointF>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

namespace openstone::desktop {
class AudioDevice;
}

class QKeyEvent;
class QNetworkAccessManager;

class SchematicModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int level READ level NOTIFY levelChanged FINAL)
    Q_PROPERTY(QAbstractItemModel* library READ library CONSTANT FINAL)
    Q_PROPERTY(QVariantList librarySections READ librarySections NOTIFY toolboxChanged FINAL)
    Q_PROPERTY(QVariantList libraryConnectors READ libraryConnectors NOTIFY toolboxChanged FINAL)
    Q_PROPERTY(QString favoritesCategory READ favoritesCategory CONSTANT FINAL)
    Q_PROPERTY(QString recentlyUsedCategory READ recentlyUsedCategory CONSTANT FINAL)
    Q_PROPERTY(QUrl projectFile READ projectFile NOTIFY projectChanged FINAL)
    Q_PROPERTY(bool closePending READ closePending NOTIFY closePendingChanged FINAL)
    Q_PROPERTY(QString undoText READ undoText NOTIFY historyChanged FINAL)
    Q_PROPERTY(QString redoText READ redoText NOTIFY historyChanged FINAL)
    Q_PROPERTY(bool straightWires READ straightWires WRITE setStraightWires NOTIFY wiringPreferencesChanged FINAL)
    Q_PROPERTY(bool autoLink READ autoLink WRITE setAutoLink NOTIFY wiringPreferencesChanged FINAL)
    Q_PROPERTY(QVariantList wires READ wires NOTIFY geometryChanged FINAL)
    Q_PROPERTY(QVariantList breadcrumbs READ breadcrumbs NOTIFY breadcrumbsChanged FINAL)
    Q_PROPERTY(QPointF center READ center NOTIFY viewportChanged FINAL)
    Q_PROPERTY(double zoom READ zoom NOTIFY viewportChanged FINAL)
    Q_PROPERTY(qulonglong triggerCount READ triggerCount NOTIFY valuesChanged FINAL)
    Q_PROPERTY(int sequencerMode READ sequencerMode NOTIFY valuesChanged FINAL)
    Q_PROPERTY(bool hasSequencerTracks READ hasSequencerTracks NOTIFY valuesChanged FINAL)
    Q_PROPERTY(bool audioRunning READ audioRunning NOTIFY audioChanged FINAL)
    Q_PROPERTY(bool audioStarting READ audioStarting NOTIFY audioChanged FINAL)
    Q_PROPERTY(double audioSampleRate READ audioSampleRate NOTIFY audioChanged FINAL)
    Q_PROPERTY(QVariantMap appDialog READ appDialog NOTIFY valuesChanged FINAL)
    Q_PROPERTY(QVariantMap dropList READ dropList NOTIFY valuesChanged FINAL)
    Q_PROPERTY(QVariantList windowFrames READ windowFrames NOTIFY valuesChanged FINAL)
    Q_PROPERTY(QVariantMap focusedPanel READ focusedPanel NOTIFY valuesChanged FINAL)
    Q_PROPERTY(QString feedback READ feedback NOTIFY feedbackChanged FINAL)
    Q_PROPERTY(bool canPaste READ canPaste NOTIFY clipboardChanged FINAL)
    Q_PROPERTY(bool canPasteSynchronized READ canPasteSynchronized NOTIFY clipboardChanged FINAL)
    Q_PROPERTY(bool syncPainterActive READ syncPainterActive NOTIFY painterChanged FINAL)

public:
    enum PointerEvent { PointerMove, PointerDrag, PointerDown, PointerUp, PointerClick, PointerDoubleClick, PointerWheel };
    Q_ENUM(PointerEvent)
    enum Arrangement { AlignHorizontally, AlignVertically, DistributeHorizontally, DistributeVertically };
    Q_ENUM(Arrangement)

    explicit SchematicModel(QObject* parent = nullptr);
    ~SchematicModel() override;

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int level() const { return static_cast<int>(session_.activeLevel()); }
    QAbstractItemModel* library() { return &library_; }
    QVariantList librarySections() const { return library_.sections(); }
    QVariantList libraryConnectors() const;
    QString favoritesCategory() const { return openstone::desktop::LibraryModel::favoritesCategory(); }
    QString recentlyUsedCategory() const { return openstone::desktop::LibraryModel::recentlyUsedCategory(); }
    QUrl projectFile() const { return projectFile_; }
    bool straightWires() const { return straightWires_; }
    bool autoLink() const { return autoLink_; }
    void setStraightWires(bool enabled);
    void setAutoLink(bool enabled);
    Q_INVOKABLE QVariantList midiDeviceChoices(int id);
    Q_INVOKABLE void setMidiDeviceSelected(int id, const QString& port, bool selected);
    Q_INVOKABLE void filterLibrary(const QString& query, const QString& group, const QString& type);
    Q_INVOKABLE void setLibraryFavorite(int kind, int toolboxIndex, bool favorite);
    Q_INVOKABLE QVariantMap primitiveShortcut(const QString& text) const;
    Q_INVOKABLE bool hasUnsavedChanges() const;
    Q_INVOKABLE bool saveProject(const QUrl& file);
    Q_INVOKABLE bool openProject(const QUrl& file);
    Q_INVOKABLE bool newProject();
    Q_INVOKABLE void watchApplicationQuit();
    Q_INVOKABLE void closeSchematic();
    bool closePending() const;
    QString undoText() const;
    QString redoText() const;
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE bool beginNodeMove(const QVariantList& ids);
    Q_INVOKABLE void moveNodeSelection(double dx, double dy);
    Q_INVOKABLE void arrangeNodes(const QVariantList& selection, Arrangement arrangement);
    Q_INVOKABLE void beginWireDeletion();
    Q_INVOKABLE void finishEdit();
    Q_INVOKABLE void editToolboxModule(int index, const QString& name, const QString& group);
    Q_INVOKABLE void removeToolboxModule(int index);
    Q_INVOKABLE void disconnectWire(int source, int output, int target, int input);
    Q_INVOKABLE QVariantMap nearbyWire(double zoom) const;
    QVariantList wires() const;
    QVariantList breadcrumbs() const;
    QVariantMap dropList() const;
    QVariantList windowFrames() const { return windowFrames_; }
    QVariantMap focusedPanel() const { return focusedPanel_; }
    QPointF center() const;
    double zoom() const;
    qulonglong triggerCount() const;
    bool audioRunning() const { return audioDevice_ != nullptr; }
    bool audioStarting() const { return audioStarting_; }
    double audioSampleRate() const;
    Q_INVOKABLE void toggleAudio();
    [[nodiscard]] int sequencerMode() const;
    [[nodiscard]] bool hasSequencerTracks() const;
    Q_INVOKABLE void setSequencerMode(int mode);
    QString feedback() const { return feedback_; }

    Q_INVOKABLE int makeModule(const QVariantList& selection);
    Q_INVOKABLE int addNode(int kind, double x, double y);
    Q_INVOKABLE void addModuleToToolbox(int id, const QString& group = {});
    Q_INVOKABLE int addToolboxModule(int index, double x, double y);
    Q_INVOKABLE void moveNode(int id, double x, double y);
    Q_INVOKABLE void enterModule(int id);
    Q_INVOKABLE void renameNode(int id, const QString& name);
    Q_INVOKABLE bool copyNodes(const QVariantList& selection);
    Q_INVOKABLE bool cutNodes(const QVariantList& selection);
    Q_INVOKABLE QVariantList pasteNodes(double x, double y, bool synchronized);
    Q_INVOKABLE QVariantList duplicateNodes(const QVariantList& selection);
    Q_INVOKABLE void disconnectNode(int id);
    Q_INVOKABLE void synchronizeAll(int id);
    Q_INVOKABLE void unsynchronize(int id, bool all);
    Q_INVOKABLE int followSync(int id, bool previous);
    Q_INVOKABLE void startSyncPainter(int id);
    Q_INVOKABLE void stopSyncPainter();
    Q_INVOKABLE void paintSync(int id);
    bool canPaste() const { return session_.canPaste(); }
    bool canPasteSynchronized() const { return session_.canPasteSynchronized(); }
    bool syncPainterActive() const { return session_.syncPainterActive(); }
    Q_INVOKABLE void navigate(int depth);
    Q_INVOKABLE void pan(double dx, double dy);
    Q_INVOKABLE void zoomAt(double factor, double x, double y);
    Q_INVOKABLE void centerOn(int depth, double x, double y);
    Q_INVOKABLE void resetView();
    Q_INVOKABLE void removeNode(int id);
    Q_INVOKABLE void removeNodes(const QVariantList& ids);
    Q_INVOKABLE bool connectNodes(int from, int output, int to, int input);
    Q_INVOKABLE void disconnectInput(int id, int input);
    Q_INVOKABLE void setTemplateType(int id, int type);
    Q_INVOKABLE void setText(int id, const QString& text);
    Q_INVOKABLE void setRubyDraft(int id, const QString& text);
    Q_INVOKABLE bool editRubyPort(int id, bool output, int port, const QString& command, int value, const QString& name);
    Q_INVOKABLE void resizeRubyBox(int id, int width, int height);
    Q_INVOKABLE QVariantList rubyPortTypes(bool output, int selected) const;
    Q_INVOKABLE QString runRuby(int id, bool portsOnly);
    Q_INVOKABLE void stopRuby(int id);
    Q_INVOKABLE void setDspDraft(int id, const QString& text);
    Q_INVOKABLE QVariantMap compileDsp(int id);
    Q_INVOKABLE QVariantMap captureDsp(int id, int frames, int blockSize);
    Q_INVOKABLE void setColor(int id, const QColor& color);
    Q_INVOKABLE void fire(int id);
    Q_INVOKABLE void setSystemSetting(int id, int index);
    Q_INVOKABLE void setTickDisplaySync(int id, bool enabled);
    Q_INVOKABLE void registerTickWindow(QQuickWindow* window);
    [[nodiscard]] QVariantMap appDialog() const;
    Q_INVOKABLE QQuickWindow* appDialogWindow() const;
    Q_INVOKABLE void finishAppDialog(const QString& token, bool accepted, const QUrl& path);
    Q_INVOKABLE void finishRubyMessageBox(const QString& token, const QString& result);
    Q_INVOKABLE void finishColorDialog(const QString& token, bool accepted, const QColor& color);
    Q_INVOKABLE void finishDropList(const QString& token, int selection);
    Q_INVOKABLE void openIndexSelector(int id, const QPointF& position);
    Q_INVOKABLE QVariantMap openPopupList(const QVariantList& ownerPath, int id);
    Q_INVOKABLE QString beginEditControl(const QVariantList& ownerPath, int id, const QString& rubySession = {});
    Q_INVOKABLE void changeEditControl(const QVariantList& ownerPath, int id, const QString& token, const QString& text);
    Q_INVOKABLE void finishEditControl(const QVariantList& ownerPath, int id, const QString& token);
    Q_INVOKABLE bool trackFileDrag(const QVariantList& ownerPath, int id, const QList<QUrl>& urls,
                                  const QPointF& position, bool dropped);
    Q_INVOKABLE void moveFileDrag(const QVariantList& ownerPath, int id, const QPointF& position);
    Q_INVOKABLE void leaveFileDrag(const QVariantList& ownerPath, int id);
    Q_INVOKABLE bool mousePolygonContains(const QVariant& polygon, const QPointF& position) const;
    Q_INVOKABLE bool rubyMouseContains(const QVariantList& ownerPath, int id, int input, const QPointF& position);
    Q_INVOKABLE int rubyMouseCursor(const QVariantList& ownerPath, int id, int input, const QPointF& position);
    Q_INVOKABLE void cancelRubyMouseDrag(const QVariantList& ownerPath, int id, int input);
    Q_INVOKABLE bool sendMouseMessage(const QVariantList& ownerPath, int id, PointerEvent event,
                                     const QPointF& position, int buttons, const QPoint& wheelDelta,
                                     QQuickItem* receiver, const QPoint& screenPosition, qreal units, int rubyInput = -1);
    Q_INVOKABLE void cancelMouseDrag(QObject* receiver);
    Q_INVOKABLE void movePanelPointer(const QVariantList& ownerPath, int id, const QPointF& position);
    Q_INVOKABLE void reportWindowVisible(const QVariantList& path, int id, bool visible);
    Q_INVOKABLE void reportWindowSize(const QVariantList& path, int id, int width, int height);
    Q_INVOKABLE void loadBitmap(int id, const QUrl& file);
    Q_INVOKABLE QVariantMap inspectPort(int id, int index, bool output) const;
    Q_INVOKABLE QVariantMap inspectWire(int source, int output, int target, int input) const;
    Q_INVOKABLE void setPanelHidden(int id, bool hidden);
    Q_INVOKABLE void enableFrontPanel(int id);
    Q_INVOKABLE void setPanelShownInParent(int id, bool shown);
    Q_INVOKABLE void setPanelUnlocked(int id, bool unlocked);
    Q_INVOKABLE void focusModule(int id);
    Q_INVOKABLE void closeFocusMode();
    Q_INVOKABLE void reportFocusWindow(const QVariantList& path, bool fullScreen, double scale);
    Q_INVOKABLE void toggleProperties(int id);
    Q_INVOKABLE void setPropertiesVisible(int id, bool visible, bool pinned);
    Q_INVOKABLE void closeUnpinnedProperties(int exceptId);
    Q_INVOKABLE void movePanelItem(int moduleId, const QVariantList& itemPath, double x, double y,
                                   bool properties = false, bool itemProperties = false);
    Q_INVOKABLE void resizePanel(int moduleId, double width, double height, bool properties = false);
    Q_INVOKABLE void setPanelItemAnchors(int moduleId, const QVariantList& itemPath, bool properties,
                                         bool itemProperties, int anchors);

signals:
    void levelChanged();
    void geometryChanged();
    void viewportChanged();
    void breadcrumbsChanged();
    void valuesChanged();
    void audioChanged();
    void dspCaptureChanged();
    void editControlStartRequested(const QString& key);
    void feedbackChanged();
    void clipboardChanged();
    void toolboxChanged();
    void projectChanged();
    void closePendingChanged();
    void applicationQuitRequested();
    void schematicCloseReady();
    void historyChanged();
    void wiringPreferencesChanged();
    void painterChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void handleKeyboardEvent(const QKeyEvent& event);

private:
    enum Role { IdRole = Qt::UserRole + 1, SpecRole, PositionRole, ValueRole, EditableRole };

    openstone::Schematic& graph() { return session_.graph(); }
    const openstone::Schematic& graph() const { return session_.graph(); }
    void endSchematicChange();
    void rebuildLibrary();
    void rememberPlacement(QString key);
    void updateRecentlyUsed(QStringList entries);
    bool replaceProject(const QByteArray& data, const QUrl& url);
    void finishProjectReplacement();
    void reportProjectOpenError(const std::exception& error);
    void refreshClosingSchematic();
    void finishPendingClose();
    QString historyText(bool redo) const;
    QVariantList insertNodes(openstone::SchematicEdit edit, const std::function<std::vector<int>()>& insert);
    void applyNodePositions(std::span<const openstone::NodePosition> positions);
    void applyHistory(bool redo);
    void refreshValues(bool portsChanged = false);
    bool refreshMidiHardware(bool scan);
    void updateMidiTimers();
    void startAudio();
    void openAudioDevice(openstone::AudioDeviceSetup setup);
    bool changeAudioSetup(const openstone::AudioDeviceSetup& setup);
    void stopAudio();
    void refreshAudioControls();
    void processTicks(openstone::TickWake wake);
    void scheduleDeferredTriggers();
    void processDeferredTriggers();
    void updateDisplayTickWindow();
    void dispatchEditControlStart();
    void renderPanelFrames();
    QString windowForView(openstone::yellow::View view) const;
    void setFeedback(QString text);
    int rowFor(int id) const;

    struct PointerCapture {
        QQuickItem* receiver;
        openstone::yellow::PanelControlId source;
        QPoint screenPosition;
        std::optional<openstone::yellow::MouseCursor> cursor;
        bool recenterOnEdge;
        bool recentered = false;
        QMetaObject::Connection receiverDestroyed;
        QMetaObject::Connection windowChanged;
    };

    void recenterPointerDrag(openstone::Schematic& owner, const QPointF& position,
                             const QPoint& screenPosition, qreal units);
    void finishPointerDrag(bool reset);
    void updateCursor();
    void setCursorOverride(std::optional<Qt::CursorShape> shape);
    void startFileDrag(const openstone::DragFilesRequest& request, const QPointer<QQuickItem>& receiver);
    void cancelSchematicDrag(const openstone::yellow::PanelControlId& source);

    openstone::Session session_;
    std::vector<openstone::NodePosition> dragOrigins_;
    openstone::desktop::LibraryModel library_{this};
    std::vector<openstone::desktop::LibraryModel::Entry> builtIns_;
    QUrl projectFile_;
    QByteArray savedProject_;
    QString toolboxPath_;
    QString recentlyUsedPath_;
    bool recentlyUsedDirty_ = false;
    bool toolboxLoaded_ = true;
    bool straightWires_ = false;
    bool autoLink_ = true;
    std::optional<openstone::Schematic::AudioPatch> audioPatch_;
    std::unique_ptr<QNetworkAccessManager> httpNetwork_;
    std::unique_ptr<openstone::desktop::AudioDevice> audioDevice_;
    QTimer audioStatusTimer_;
    QTimer audioFramesTimer_;
    QTimer midiScanTimer_;
    QTimer midiPollTimer_;
    QTimer serialPollTimer_;
    bool audioStarting_ = false;
    QChronoTimer tickTimer_;
    std::optional<openstone::TickTime> armedTick_;
    bool displayTicks_ = false;
    std::vector<QPointer<QQuickWindow>> tickWindows_;
    QPointer<QQuickWindow> tickFrameWindow_;
    QMetaObject::Connection tickFrameConnection_;
    std::map<int, QVariantMap> renderedPanels_;
    QVariantList windowFrames_;
    QVariantMap focusedPanel_;
    std::map<std::uint64_t, QVariantMap> cachedDrawingDescriptions_;
    std::optional<PointerCapture> pointerDrag_;
    std::optional<Qt::CursorShape> cursorOverride_;
    std::optional<std::uint64_t> hiddenCursorRevision_;
    std::optional<QPoint> heldCursorPosition_;
    bool nativeFileDrag_ = false;
    std::uint64_t busLayoutRevision_ = 0;
    std::uint64_t panelGeometryRevision_ = 0;
    bool redrawQueued_ = false;
    bool deferredTriggersQueued_ = false;
    QString feedback_;
    std::optional<QUrl> pendingProjectFile_;
    bool pendingFlowStoneImport_ = false;
    QStringList pendingImportWarnings_;
    enum class WindowClose { None, Requested, Ready };
    WindowClose windowClose_ = WindowClose::None;
};
