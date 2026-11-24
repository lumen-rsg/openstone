#pragma once

#include "SerialDeviceBackend.hpp"

namespace openstone::desktop {

std::shared_ptr<SerialDeviceBackend> makeSerialDeviceBackend();

}
