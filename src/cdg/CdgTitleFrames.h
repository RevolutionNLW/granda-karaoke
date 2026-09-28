#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace cdg {

// A still frame from the start of a CD+G stream that looks like a title
// screen: it stayed unchanged for a while and has enough non-background
// pixels to hold text.
struct TitleFrame {
    int timeMs = 0;
    double foregroundRatio = 0.0;
    int foregroundTiles = 0;
    std::uint32_t background = 0;       // the frame's dominant colour
    std::vector<std::uint32_t> pixels;  // 300x216 ARGB, as CdgDecoder renders
};

// Decodes the first `limitMs` of a stream and returns up to `maxFrames`
// candidate title frames shown before the screen is first cleared for the
// lyrics, most text-like first. Never throws; an empty or blank stream gives
// no frames. Timing is the stream's own packet clock. `stop`, when given, is
// asked at every step and must return at once; once it says stop, no frames
// are returned.
std::vector<TitleFrame> findTitleFrames(const std::vector<std::uint8_t>& stream,
                                        int limitMs = 45000, int maxFrames = 3,
                                        const std::function<bool()>& stop = {});

} // namespace cdg
