#include "music/AudibleEnd.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace music {

namespace {

constexpr double kQuietestDb = -150.0;

float toDb(double amplitude)
{
    return amplitude > 0.0 ? float(std::max(kQuietestDb, 20.0 * std::log10(amplitude)))
                           : float(kQuietestDb);
}

} // namespace

AudibleEndDetector::AudibleEndDetector(int sampleRate, int channels)
    : m_sampleRate(std::max(1, sampleRate))
    , m_channels(std::max(1, channels))
    , m_windowFrames(std::size_t(std::max(1, m_sampleRate * kWindowMs / 1000)))
    , m_sumSquares(std::size_t(m_channels), 0.0)
{
}

void AudibleEndDetector::addSamples(const float* interleaved, std::size_t frames)
{
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const float* sample = interleaved + frame * std::size_t(m_channels);
        for (int channel = 0; channel < m_channels; ++channel) {
            const float value = std::isfinite(sample[channel]) ? sample[channel] : 1.0f;
            m_sumSquares[std::size_t(channel)] += double(value) * double(value);
            m_peak = std::max(m_peak, std::fabs(value));
        }
        ++m_frames;
        if (++m_inWindow == m_windowFrames)
            closeWindow();
    }
}

void AudibleEndDetector::closeWindow()
{
    double loudest = 0.0;
    for (double& sum : m_sumSquares) {
        loudest = std::max(loudest, sum / double(m_inWindow));
        sum = 0.0;
    }
    m_rmsDb.push_back(toDb(std::sqrt(loudest)));
    m_peakDb.push_back(toDb(m_peak));
    m_peak = 0.0f;
    m_inWindow = 0;
}

AudibleEnd AudibleEndDetector::finish() const
{
    // The windows, with the last part-window too.
    AudibleEndDetector last = *this;
    if (last.m_inWindow > 0)
        last.closeWindow();
    const std::vector<float>& rms = last.m_rmsDb;
    const std::vector<float>& peak = last.m_peakDb;

    AudibleEnd result;
    result.durationMs = m_frames * 1000 / m_sampleRate;
    result.audibleEndMs = result.durationMs;
    result.endAtMs = result.durationMs;
    const auto playToEnd = [&result](const char* reason) {
        result.verdict = AudibleEnd::Verdict::PlayToEnd;
        result.endAtMs = result.durationMs;
        result.reason = reason;
        return result;
    };
    if (rms.empty() || result.durationMs < kMinSongMs)
        return playToEnd("a short recording");

    std::vector<float> sorted = rms;
    const std::size_t at = std::min(sorted.size() - 1, sorted.size() * 9 / 10);
    std::nth_element(sorted.begin(), sorted.begin() + std::ptrdiff_t(at), sorted.end());
    result.referenceDb = sorted[at];
    if (result.referenceDb < kQuietestSongDb)
        return playToEnd("a very quiet recording");
    result.silenceDb = std::min(kSilenceFloorDb, result.referenceDb - kBelowSongDb);

    // The last window with any sound in it.
    std::size_t end = rms.size();
    while (end > 0 && rms[end - 1] < result.silenceDb
           && peak[end - 1] < result.silenceDb + kPeakAllowanceDb)
        --end;
    if (end == 0)
        return playToEnd("no music found");
    result.audibleEndMs = std::min<std::int64_t>(result.durationMs,
                                                 std::int64_t(end) * kWindowMs);
    if (result.durationMs - result.audibleEndMs < kGapMs + kMinSavedMs)
        return playToEnd("little or no silence at the end");
    if (double(result.audibleEndMs) < kMinMusicShare * double(result.durationMs))
        return playToEnd("silence makes up much of the recording");

    result.verdict = AudibleEnd::Verdict::Skip;
    result.endAtMs = result.audibleEndMs + kGapMs;
    result.reason = "silence after the music";
    return result;
}

std::string formatTime(std::int64_t ms)
{
    const std::int64_t tenths = std::max<std::int64_t>(0, ms) / 100;
    char text[32];
    std::snprintf(text, sizeof text, "%lld:%02lld.%lld", static_cast<long long>(tenths / 600),
                  static_cast<long long>(tenths / 10 % 60), static_cast<long long>(tenths % 10));
    return text;
}

} // namespace music
