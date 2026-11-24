#include <QFontDatabase>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>

#include <cstdlib>

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("OpenStone"));
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    const int nunito = QFontDatabase::addApplicationFont(
        QStringLiteral(":/qt/qml/OpenStone/assets/fonts/Nunito.ttf"));
    const auto families = QFontDatabase::applicationFontFamilies(nunito);
    if (!families.isEmpty()) {
        auto font = app.font();
        font.setFamily(families.first());
        app.setFont(font);
    } else {
        qWarning("Could not load bundled Nunito font");
    }

    QQmlApplicationEngine engine;
    engine.loadFromModule(QStringLiteral("OpenStone"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty()) {
        return EXIT_FAILURE;
    }
    return app.exec();
}
