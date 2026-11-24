#pragma once

#include "MidiDeviceBackend.hpp"

namespace openstone::desktop {

std::shared_ptr<MidiDeviceBackend> makeMidiDeviceBackend();

}
