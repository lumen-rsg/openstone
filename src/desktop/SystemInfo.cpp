#include "SystemInfo.hpp"
#include "Schematic.hpp"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QNetworkInterface>
#include <QProcess>
#include <QUrl>

#include <algorithm>
#include <stdexcept>
#include <QFontDatabase>
#include <QStandardPaths>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <shlobj.h>

#include <array>
#endif

namespace openstone::desktop {

std::string systemFolder(std::int32_t id)
{
    if (id == -1) return QCoreApplication::applicationDirPath().toStdString();

#ifdef Q_OS_WIN
    // Accept folder IDs alone. The lookup does not create the directory.
    if (id < 0 || id > 0xff) return {};
    const int csidl = id == 0 ? CSIDL_DESKTOPDIRECTORY : id;
    std::array<wchar_t, MAX_PATH> path{};
    if (FAILED(SHGetFolderPathW(nullptr, csidl | CSIDL_FLAG_DONT_VERIFY, nullptr, SHGFP_TYPE_CURRENT, path.data()))) {
        return {};
    }
    return QString::fromWCharArray(path.data()).toStdString();
#else
    using enum QStandardPaths::StandardLocation;
    QStandardPaths::StandardLocation location;
    switch (id) {
    case 0: case 16: location = DesktopLocation; break;
    case 5: location = DocumentsLocation; break;
    case 13: location = MusicLocation; break;
    case 14: location = MoviesLocation; break;
    case 20: location = FontsLocation; break;
    case 26: case 28: location = GenericDataLocation; break;
    case 39: location = PicturesLocation; break;
    case 40: location = HomeLocation; break;
    default: return {};
    }
    return QStandardPaths::writableLocation(location).toStdString();
#endif
}

bool openWebUrl(std::string_view url)
{
    const QUrl target(QString::fromUtf8(url.data(), url.size()), QUrl::StrictMode);
    return target.isValid() && !target.scheme().isEmpty() && QDesktopServices::openUrl(target);
}

bool launchProgram(std::string_view program, std::string_view arguments, bool background)
{
    if (program.empty() || program.find('\0') != program.npos || arguments.find('\0') != arguments.npos) return false;
    QProcess process;
    process.setProgram(QString::fromUtf8(program.data(), program.size()));
    const auto commandLine = QString::fromUtf8(arguments.data(), arguments.size());
#ifdef Q_OS_WIN
    process.setNativeArguments(commandLine);
#else
    process.setArguments(QProcess::splitCommand(commandLine));
#endif
    if (background) return process.startDetached();
    process.setStandardInputFile(QProcess::nullDevice());
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    process.start();
    if (!process.waitForStarted()) return false;
    process.waitForFinished(-1);
    return true;
}

green::StringArray systemMacAddresses()
{
    green::StringArray addresses;
    for (const auto& adapter : QNetworkInterface::allInterfaces()) {
        if (adapter.flags().testFlag(QNetworkInterface::IsLoopBack)) continue;
        const auto address = adapter.hardwareAddress();
        if (address.isEmpty() || address == QStringLiteral("00:00:00:00:00:00")) continue;
        auto value = address.toStdString();
        if (std::ranges::find(addresses, value) == addresses.end()) addresses.push_back(std::move(value));
    }
    return addresses;
}

std::shared_ptr<const FontRegistration> registerFonts(const std::vector<std::string>& data)
{
    struct RegisteredFonts : FontRegistration {
        ~RegisteredFonts() override
        {
            for (int font : ids) QFontDatabase::removeApplicationFont(font);
        }
        std::vector<int> ids;
    };
    const auto result = std::make_shared<RegisteredFonts>();
    result->ids.reserve(data.size());
    for (const auto& font : data) {
        const int id = QFontDatabase::addApplicationFontFromData(QByteArray(font.data(), font.size()));
        if (id < 0) throw std::runtime_error("Could not load the embedded font");
        result->ids.push_back(id);
    }
    return result;
}

green::StringArray systemFontFamilies()
{
    const auto families = QFontDatabase::families();
    green::StringArray result;
    result.reserve(static_cast<std::size_t>(families.size()));
    for (const auto& family : families) {
        if (!QFontDatabase::isPrivateFamily(family)) {
            result.push_back(family.toStdString());
        }
    }
    return result;
}

}
