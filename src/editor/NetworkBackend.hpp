#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace openstone {

struct NetworkSettings {
    bool server;
    bool datagram;
    std::string host;
    std::uint16_t port;
};

struct NetworkPoll {
    std::string status;
    std::string address;
    std::vector<std::string> received;
    bool finished = false;
};

class NetworkConnection {
public:
    virtual ~NetworkConnection() = default;
    [[nodiscard]] virtual NetworkPoll poll() = 0;
    [[nodiscard]] virtual bool send(std::string_view bytes) = 0;
};

struct NetworkState {
    std::shared_ptr<NetworkConnection> connection;
    std::string status = "Closed";
    std::string address;
    std::string data;
    std::string log;
};

}
