#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace openstone {

struct SerialDeviceInfo {
    int number;
    std::string id;
};

struct SerialPortSettings {
    int baud = 9600;
    int stopBits = 0; // 0 = one, 1 = one and a half, 2 = two
    int parity = 0;   // 0 = none, 1 = odd, 2 = even, 3 = mark, 4 = space
    int dataBits = 8;

    bool operator==(const SerialPortSettings&) const = default;
};

class SerialDevicePort {
public:
    virtual ~SerialDevicePort() = default;
    virtual std::string read(std::size_t maximum) = 0;
    virtual std::size_t write(std::string_view bytes) = 0;
    virtual std::size_t pendingWrites() const = 0;
    virtual void clearWrites() = 0;
};

// One control thread uses the backend and its ports. Reads and writes never wait for bytes.
class SerialDeviceBackend {
public:
    virtual ~SerialDeviceBackend() = default;
    virtual std::vector<SerialDeviceInfo> list() = 0;
    virtual std::unique_ptr<SerialDevicePort> open(std::string_view id, const SerialPortSettings& settings) = 0;
};

}
