#pragma once

#include "green/Value.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace openstone {

enum class HttpResponseKind { Text, Image, File };

struct HttpRequest {
    std::uint64_t token = 0;
    std::vector<int> owner;
    int node = 0;
    HttpResponseKind kind = HttpResponseKind::Text;
    std::string url;
    green::StringArray names;
    green::StringArray values;
    std::string destination;
    bool overwrite = false;
};

struct HttpResponse {
    std::string error;
    std::string text;
    yellow::Bitmap image;
};

}
