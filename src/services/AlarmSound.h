#pragma once
#include <cstdint>
#include <vector>

namespace eflow {

// The reminder chime, made in memory (no sound file to ship) and played on the default speaker.
class AlarmSound {
public:
    AlarmSound();
    void Ring();            // a gentle three-note chime, repeated until Stop()
    void HeadsUp();         // two soft notes, once (the early warning)
    void Stop();

private:
    std::vector<uint8_t> ring_, headsUp_;   // complete WAV files; PlaySound reads them from memory
};

} // namespace eflow
