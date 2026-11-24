#include "Session.hpp"
#include "SettingsStore.hpp"

#include <QCoreApplication>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>

#include <iostream>
#include <stdexcept>

namespace {

using openstone::Schematic;
using enum openstone::PrimitiveKind;

void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct Store {
    explicit Store(Schematic& graph)
        : graph(graph)
        , node(graph.addNode(SettingsStore, 0, 0))
        , group(graph.addNode(String, 0, 0, std::string{"test/group"}))
        , key(graph.addNode(String, 0, 0, std::string{"Key/name"}))
        , input(graph.addNode(String, 0, 0))
        , read(graph.addNode(Trigger, 0, 0))
        , write(graph.addNode(Trigger, 0, 0))
    {
        graph.connect({group, node, 0});
        graph.connect({key, node, 1});
        graph.connect({input, node, 2});
        graph.connect({read, node, 3});
        graph.connect({write, node, 4});
    }

    void set(std::string value)
    {
        graph.setValue(input, std::move(value));
        graph.fireTrigger(write);
    }

    [[nodiscard]] std::string value() const { return std::get<std::string>(graph.readValue(node)); }
    [[nodiscard]] auto done() const { return graph.inspectOutput(node, 1).triggers; }
    [[nodiscard]] auto errors() const { return graph.inspectOutput(node, 2).triggers; }

    Schematic& graph;
    int node;
    int group;
    int key;
    int input;
    int read;
    int write;
};

const std::string savedText = "Réglage\t1\nTwo lines = @value";

void writeSettings()
{
    openstone::Session session;
    auto& graph = session.graph();
    graph.setSettingsStoreBackend(openstone::desktop::makeSettingsStoreBackend());
    Store store(graph);
    check(store.done() == 0 && store.errors() == 0, "Wiring the store does not run either command");
    store.set("old value");
    store.set(savedText);
    check(store.value() == savedText && store.done() == 2, "Set overwrites the value and publishes Done");

    graph.setValue(store.key, std::string{"key/name"});
    store.set("");
    check(store.value().empty() && store.done() == 3, "Set can save an empty string");
    graph.setValue(store.key, std::string{});
    store.set("invalid");
    check(store.errors() == 1 && store.done() == 3, "An empty key emits Error without Done");
}

void readSettings()
{
    openstone::Session session;
    session.graph().setSettingsStoreBackend(openstone::desktop::makeSettingsStoreBackend());
    auto project = std::make_unique<Schematic>();
    auto& module = *project->module(project->addNode(Module, 0, 0));
    Store store(module);
    module.connect({module.addNode(AfterLoad, 0, 0), store.node, 3});
    check(session.openSchematic(std::move(project)), "A new session opens the reader schematic");
    check(store.value() == savedText && store.done() == 1 && store.errors() == 0,
          "After Load in a new process reads the exact text saved by the previous session");

    module.setValue(store.key, std::string{"key/name"});
    module.fireTrigger(store.read);
    check(store.value().empty() && store.done() == 2 && store.errors() == 0,
          "Case-distinct keys are preserved and an empty value succeeds");
    module.setValue(store.group, std::string{"different group"});
    module.fireTrigger(store.read);
    check(store.value().empty() && store.done() == 2 && store.errors() == 1,
          "A missing key in another group emits Error without a successful read");
}

void runSession(const QString& mode, const QString& path)
{
    QProcess child;
    child.setProcessChannelMode(QProcess::MergedChannels);
    child.start(QCoreApplication::applicationFilePath(), {mode, path});
    if (!child.waitForStarted() || !child.waitForFinished() ||
        child.exitStatus() != QProcess::NormalExit || child.exitCode() != 0) {
        throw std::runtime_error("Settings session failed: " + mode.toStdString() + "\n" +
                                 child.readAll().toStdString());
    }
}

}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try {
        const auto args = app.arguments();
        if (args.size() == 3) {
            QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, args.at(2));
            if (args.at(1) == QStringLiteral("write")) writeSettings();
            else if (args.at(1) == QStringLiteral("read")) readSettings();
            else throw std::invalid_argument("Unknown settings test mode");
        } else {
            QTemporaryDir directory;
            check(directory.isValid(), "Create an isolated settings directory");
            runSession(QStringLiteral("write"), directory.path());
            runSession(QStringLiteral("read"), directory.path());
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
