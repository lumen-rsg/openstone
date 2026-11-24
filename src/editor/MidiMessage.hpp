#pragma once

#include "green/Value.hpp"

namespace openstone {

[[nodiscard]] bool validMidiMessage(const green::MidiMessage& message);

}
