#pragma once

namespace openstone::stream {

struct HostTransport {
    double samplePosition = 0;
    double quarterPosition = 0;
    double barStart = 0;
    double tempo = 120;
    int numerator = 4;
    int denominator = 4;
    bool playing = false;
    double sampleRate = 44100;
    bool offline = false;
};

}
