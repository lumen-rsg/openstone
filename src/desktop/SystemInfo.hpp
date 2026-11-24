#pragma once

#include "green/Value.hpp"

#include <memory>

namespace openstone { struct FontRegistration; }

namespace openstone::desktop {

[[nodiscard]] std::string systemFolder(std::int32_t id);
[[nodiscard]] green::StringArray systemFontFamilies();
[[nodiscard]] bool openWebUrl(std::string_view url);
[[nodiscard]] bool launchProgram(std::string_view program, std::string_view arguments, bool background);
[[nodiscard]] green::StringArray systemMacAddresses();
[[nodiscard]] std::string firstDiskSerial();
[[nodiscard]] std::shared_ptr<const FontRegistration> registerFonts(const std::vector<std::string>& data);

}
