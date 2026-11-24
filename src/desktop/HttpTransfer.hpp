#pragma once

#include "HttpRequest.hpp"

#include <functional>

class QNetworkAccessManager;

namespace openstone::desktop {

void sendHttpRequest(QNetworkAccessManager& network, HttpRequest request,
                     std::function<bool(const HttpRequest&)> pending,
                     std::function<void(const HttpRequest&, HttpResponse)> complete);

}
