#include "SerialDevice.hpp"

#include <QtSerialPort/QSerialPort>
#include <QtSerialPort/QSerialPortInfo>

#include <algorithm>
#include <array>
#include <map>
#include <stdexcept>

namespace openstone::desktop {
namespace {

class QtSerialDevicePort final : public SerialDevicePort {
public:
    QtSerialDevicePort(std::string_view id, const SerialPortSettings& settings)
    {
        constexpr std::array stops{QSerialPort::OneStop, QSerialPort::OneAndHalfStop, QSerialPort::TwoStop};
        constexpr std::array parities{QSerialPort::NoParity, QSerialPort::OddParity, QSerialPort::EvenParity,
                                      QSerialPort::MarkParity, QSerialPort::SpaceParity};
        port_.setPortName(QString::fromUtf8(id.data(), id.size()));
        if (!port_.setBaudRate(settings.baud) ||
            !port_.setDataBits(static_cast<QSerialPort::DataBits>(settings.dataBits)) ||
            !port_.setStopBits(stops.at(settings.stopBits)) || !port_.setParity(parities.at(settings.parity)) ||
            !port_.setFlowControl(QSerialPort::NoFlowControl)) {
            throw std::runtime_error(port_.errorString().toStdString());
        }
        port_.setReadBufferSize(1024 * 1024);
        if (!port_.open(QIODevice::ReadWrite)) throw std::runtime_error(port_.errorString().toStdString());
    }

    std::string read(std::size_t maximum) override
    {
        checkPort();
        return port_.read(static_cast<qint64>(maximum)).toStdString();
    }

    std::size_t write(std::string_view bytes) override
    {
        checkPort();
        const auto accepted = port_.write(bytes.data(), static_cast<qint64>(bytes.size()));
        if (accepted < 0) throw std::runtime_error(port_.errorString().toStdString());
        return static_cast<std::size_t>(accepted);
    }

    std::size_t pendingWrites() const override
    {
        return static_cast<std::size_t>(port_.bytesToWrite());
    }

    void clearWrites() override
    {
        checkPort();
        if (!port_.clear(QSerialPort::Output)) throw std::runtime_error(port_.errorString().toStdString());
    }

private:
    void checkPort() const
    {
        const auto status = port_.error();
        if (!port_.isOpen() || (status != QSerialPort::NoError && status != QSerialPort::TimeoutError)) {
            throw std::runtime_error(port_.errorString().toStdString());
        }
    }

    QSerialPort port_;
};

class QtSerialDeviceBackend final : public SerialDeviceBackend {
public:
    std::vector<SerialDeviceInfo> list() override
    {
        auto ports = QSerialPortInfo::availablePorts();
        std::ranges::sort(ports, [](const auto& a, const auto& b) { return a.systemLocation() < b.systemLocation(); });
        std::vector<SerialDeviceInfo> result;
        for (const auto& port : ports) {
#if defined(Q_OS_WIN)
            const auto name = port.portName();
            if (!name.startsWith(QStringLiteral("COM"), Qt::CaseInsensitive)) continue;
            bool valid = false;
            const int number = QStringView(name).sliced(3).toInt(&valid);
            if (valid && number > 0) result.push_back({number, name.toStdString()});
#else
            auto path = port.systemLocation().toStdString();
#if defined(Q_OS_MACOS)
            // The callout and dial-in nodes can represent one device. Use the callout node.
            if (!path.starts_with("/dev/cu.")) continue;
#endif
            const auto [entry, inserted] = numbers_.try_emplace(path, nextNumber_);
            if (inserted) ++nextNumber_;
            result.push_back({entry->second, std::move(path)});
#endif
        }
        return result;
    }

    std::unique_ptr<SerialDevicePort> open(std::string_view id, const SerialPortSettings& settings) override
    {
        return std::make_unique<QtSerialDevicePort>(id, settings);
    }

private:
#if !defined(Q_OS_WIN)
    std::map<std::string, int, std::less<>> numbers_;
    int nextNumber_ = 1;
#endif
};

}

std::shared_ptr<SerialDeviceBackend> makeSerialDeviceBackend()
{
    return std::make_shared<QtSerialDeviceBackend>();
}

}
