#pragma once

#include <string>
#include <string_view>

namespace openstone {

std::string encodeHexBytes(std::string_view bytes);
std::string decodeHexBytes(std::string_view text);

}
