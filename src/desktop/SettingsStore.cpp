#include "SettingsStore.hpp"

#include <QSettings>

#include <stdexcept>

namespace openstone::desktop {
namespace {

QString storageKey(std::string_view group, std::string_view key)
{
    const auto encode = [](std::string_view part) {
        return QString::fromLatin1(QByteArray(part.data(), part.size()).toHex());
    };
    // Encode each segment to preserve case and prevent slashes from creating subgroups.
    return QStringLiteral("g") + encode(group) + QStringLiteral("/k") + encode(key);
}

void sync(QSettings& settings)
{
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        throw std::runtime_error("Could not access the settings store");
    }
}

}

Schematic::SettingsStoreBackend makeSettingsStoreBackend()
{
    return {
        .read = [](std::string_view group, std::string_view key) -> std::optional<std::string> {
            QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                               QStringLiteral("OpenStone"), QStringLiteral("Schematic Settings"));
            settings.setFallbacksEnabled(false);
            sync(settings);
            const auto value = settings.value(storageKey(group, key));
            if (!value.isValid()) return std::nullopt;
            return value.toByteArray().toStdString();
        },
        .write = [](std::string_view group, std::string_view key, std::string_view value) {
            QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                               QStringLiteral("OpenStone"), QStringLiteral("Schematic Settings"));
            settings.setFallbacksEnabled(false);
            settings.setValue(storageKey(group, key), QByteArray(value.data(), value.size()));
            sync(settings);
        },
    };
}

}
