#pragma once

#include "Session.hpp"

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <string_view>

namespace openstone::desktop {

class SchematicFile {
public:
    static std::string_view primitiveKey(const Primitive& spec);
    static QByteArray project(const Session& session);
    static bool hasUnsavedChanges(const Session& session, const QByteArray& savedProject);
    static QByteArray saveProject(Session& session, const QString& path);
    static bool loadProject(Session& session, const QByteArray& data, const std::function<void()>& beforeReplace,
                            const QString& filePath = {});
    static bool importFlowStone(Session& session, const QByteArray& data, const std::function<void()>& beforeReplace,
                                 QStringList* warnings = nullptr);
    static QByteArray read(const QString& path);
    static void write(const QString& path, const QByteArray& data);
    static void loadToolbox(Session& session, const QString& path);
    static void addToolboxModule(Session& session, const QString& path, int id, std::string group);
    static void editToolboxModule(Session& session, const QString& path, std::size_t index,
                                  std::string name, std::string group);
    static void removeToolboxModule(Session& session, const QString& path, std::size_t index);
    static void ensureToolboxModuleId(Session& session, const QString& path, std::size_t index);
    static constexpr int recentlyUsedLimit = 50;
    static QStringList loadRecentlyUsed(const QString& path);
    static void saveRecentlyUsed(const QString& path, const QStringList& entries);
    static void setToolboxFavorite(Session& session, const QString& path, std::size_t index, bool favorite);
    static void setPrimitiveFavorite(Session& session, const QString& path, PrimitiveKind kind, bool favorite);

private:
    static QJsonObject encode(const Session::NodeCopy& copy, std::optional<double> sequenceDuration = {});
    static Session::NodeCopy decode(const QJsonObject& object, int depth, std::optional<double> sequenceDuration = {});
    static void saveToolbox(const std::vector<Session::ToolboxEntry>& toolbox,
                            const std::set<PrimitiveKind>& favoritePrimitives, const QString& path);
};

}
