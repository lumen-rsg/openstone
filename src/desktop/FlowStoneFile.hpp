#pragma once

#include "Schematic.hpp"
#include <QByteArray>
#include <QStringList>

namespace openstone::desktop {

// A detached import. Parsing never touches a live Session or executes Ruby.
struct FlowStoneNode {
    SchematicNode node;
    std::vector<FlowStoneNode> children;
    std::vector<SchematicLink> links;
    // All code boxes reserve output zero for source; only Ruby also reserves input zero.
    int nativeTag = 0;
    std::optional<green::Point> panelPosition;
    bool panelMouse = false;
    bool panelEnabled = true;
    bool panelBorder = true;
    quint16 viewScope = 0;
    bool implicitView = false;
    bool sourceConnector = false;
    std::string storedSource;
    std::optional<std::pair<int, int>> storedCodePorts;
};

struct FlowStoneImport {
    FlowStoneNode root;
    int version = 0;
    int nodes = 0;
    int links = 0;
    QStringList warnings;
};

class FlowStoneFile {
public:
    static bool isArchive(const QByteArray& data);
    static FlowStoneImport decode(const QByteArray& archive);
};

}
