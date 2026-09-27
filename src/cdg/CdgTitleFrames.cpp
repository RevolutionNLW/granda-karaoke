#include "cdg/CdgTitleFrames.h"

#include "cdg/CdgDecoder.h"

#include <algorithm>
#include <array>

namespace cdg {

namespace {

constexpr int kStepMs = 100;
constexpr int kStableMs = 700;
constexpr double kMinimumForegroundRatio = 0.004;
constexpr int kMinimumForegroundTiles = 8;
constexpr int kPixels = CdgDecoder::kWidth * CdgDecoder::kHeight;

struct Stats {
    std::uint32_t dominant = 0;
    double foregroundRatio = 0.0;
    int foregroundTiles = 0;
};

Stats analyse(const std::vector<std::uint32_t>& pixels)
{
    std::array<std::uint32_t, 16> colours{};
    std::array<int, 16> counts{};
    int used = 0;
    for (const std::uint32_t pixel : pixels) {
        int index = 0;
        while (index < used && colours[index] != pixel)
            ++index;
        if (index == used) {
            if (used == static_cast<int>(colours.size()))
                continue;  // CD+G has at most 16 colours; defensive only
            colours[used++] = pixel;
        }
        ++counts[index];
    }
    int dominant = 0;
    for (int index = 1; index < used; ++index) {
        if (counts[index] > counts[dominant])
            dominant = index;
    }
    Stats stats;
    stats.dominant = colours[dominant];
    stats.foregroundRatio = 1.0 - double(counts[dominant]) / double(pixels.size());
    for (int tileY = 0; tileY < CdgDecoder::kTileRows; ++tileY) {
        for (int tileX = 0; tileX < CdgDecoder::kTileColumns; ++tileX) {
            int foreground = 0;
            for (int y = 0; y < CdgDecoder::kTileHeight; ++y) {
                const int row = (tileY * CdgDecoder::kTileHeight + y) * CdgDecoder::kWidth;
                for (int x = 0; x < CdgDecoder::kTileWidth; ++x) {
                    if (pixels[row + tileX * CdgDecoder::kTileWidth + x] != stats.dominant)
                        ++foreground;
                }
            }
            if (foreground >= 4)
                ++stats.foregroundTiles;
        }
    }
    return stats;
}

// Mixed tiles tend to be glyphs; very high coverage tends to be a logo or a
// picture, which is penalised.
double score(const TitleFrame& frame)
{
    const double coveragePenalty = std::max(0.0, frame.foregroundRatio - 0.45) * 300.0;
    return frame.foregroundTiles + frame.foregroundRatio * 100.0 - coveragePenalty;
}

} // namespace

std::vector<TitleFrame> findTitleFrames(const std::vector<std::uint8_t>& stream, int limitMs,
                                        int maxFrames)
{
    std::vector<TitleFrame> result;
    if (stream.empty() || maxFrames <= 0)
        return result;
    CdgDecoder decoder;
    decoder.setData(stream);
    const int last = static_cast<int>(std::min<std::int64_t>(limitMs, decoder.durationMs()));
    std::vector<std::uint32_t> previous;
    std::vector<std::uint32_t> current(kPixels);
    std::vector<TitleFrame> candidates;
    std::vector<int> stableSince;
    int changedAt = 0;
    double densest = 0.0;
    int clearStart = -1;
    int clearedAt = -1;
    bool recordedCurrent = false;
    for (int time = 0; time <= last; time += kStepMs) {
        decoder.advanceTo(time);
        decoder.renderArgb32(current.data(), CdgDecoder::kWidth);
        if (previous.empty() || current != previous) {
            changedAt = time;
            recordedCurrent = false;
        }
        const Stats stats = analyse(current);
        const bool texty = stats.foregroundRatio >= kMinimumForegroundRatio
            && stats.foregroundTiles >= kMinimumForegroundTiles;
        if (texty)
            densest = std::max(densest, stats.foregroundRatio);
        // The first near-blank stretch after something was drawn is the clear
        // before the lyrics start; later frames are lyrics, not titles.
        if (clearedAt < 0 && densest >= 0.02
            && stats.foregroundRatio < std::max(0.003, densest * 0.18)) {
            if (clearStart < 0)
                clearStart = time;
            else if (time - clearStart >= 300)
                clearedAt = clearStart;
        } else if (clearedAt < 0) {
            clearStart = -1;
        }
        if (!recordedCurrent && texty && time - changedAt >= kStableMs) {
            candidates.push_back({time, stats.foregroundRatio, stats.foregroundTiles,
                                  stats.dominant, current});
            stableSince.push_back(changedAt);
            recordedCurrent = true;
        }
        previous = current;
    }
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (clearedAt < 0 || stableSince[i] < clearedAt)
            order.push_back(i);
    }
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return score(candidates[a]) > score(candidates[b]);
    });
    if (order.size() > static_cast<std::size_t>(maxFrames))
        order.resize(static_cast<std::size_t>(maxFrames));
    for (const std::size_t index : order)
        result.push_back(std::move(candidates[index]));
    return result;
}

} // namespace cdg
