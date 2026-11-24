#include "SystemInfo.hpp"

#include <QDir>
#include <QFile>
#include <QScopeGuard>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <winioctl.h>

#include <cstring>
#include <vector>
#elif defined(Q_OS_MACOS)
#include <IOKit/IOKitLib.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/storage/IOStorageDeviceCharacteristics.h>
#endif

namespace openstone::desktop {

std::string firstDiskSerial()
{
#ifdef Q_OS_WIN
    auto handle = CreateFileW(LR"(\\.\PhysicalDrive0)", 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return {};
    const auto close = qScopeGuard([handle] { CloseHandle(handle); });

    STORAGE_PROPERTY_QUERY query{StorageDeviceProperty, PropertyStandardQuery, {}};
    STORAGE_DESCRIPTOR_HEADER header{};
    DWORD written = 0;
    if (!DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &header, sizeof(header),
                         &written, nullptr) || written < sizeof(header) || header.Size < sizeof(STORAGE_DEVICE_DESCRIPTOR) ||
        header.Size > 1024 * 1024) return {};

    std::vector<char> data(header.Size);
    if (!DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), data.data(), header.Size,
                         &written, nullptr) || written < sizeof(STORAGE_DEVICE_DESCRIPTOR)) return {};
    STORAGE_DEVICE_DESCRIPTOR descriptor{};
    std::memcpy(&descriptor, data.data(), sizeof(descriptor));
    const auto start = descriptor.SerialNumberOffset;
    if (start == 0 || start >= written) return {};
    const auto* serial = data.data() + start;
    const auto* end = static_cast<const char*>(std::memchr(serial, 0, written - start));
    return end ? QString::fromLatin1(serial, end - serial).trimmed().toStdString() : std::string{};
#elif defined(Q_OS_MACOS)
    io_iterator_t disks = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(MACH_PORT_NULL, IOServiceMatching(kIOBlockStorageDeviceClass), &disks) != KERN_SUCCESS) {
        return {};
    }
    const auto releaseDisks = qScopeGuard([disks] { IOObjectRelease(disks); });
    while (const auto disk = IOIteratorNext(disks)) {
        const auto releaseDisk = qScopeGuard([disk] { IOObjectRelease(disk); });
        const auto properties = IORegistryEntryCreateCFProperty(
            disk, CFSTR(kIOPropertyDeviceCharacteristicsKey), kCFAllocatorDefault, 0);
        if (!properties) continue;
        const auto releaseProperties = qScopeGuard([properties] { CFRelease(properties); });
        if (CFGetTypeID(properties) != CFDictionaryGetTypeID()) continue;
        const auto serial = static_cast<CFStringRef>(CFDictionaryGetValue(
            static_cast<CFDictionaryRef>(properties), CFSTR(kIOPropertyProductSerialNumberKey)));
        if (serial && CFGetTypeID(serial) == CFStringGetTypeID()) {
            auto value = QString::fromCFString(serial).trimmed();
            if (!value.isEmpty()) return value.toStdString();
        }
    }
#elif defined(Q_OS_LINUX)
    const QDir blockDevices(QStringLiteral("/sys/block"));
    for (const auto& disk : blockDevices.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        QFile serial(blockDevices.filePath(disk + QStringLiteral("/device/serial")));
        if (serial.open(QIODevice::ReadOnly)) {
            const auto value = serial.read(4096).trimmed();
            if (!value.isEmpty()) return value.toStdString();
        }
    }
#endif
    return {};
}

}
