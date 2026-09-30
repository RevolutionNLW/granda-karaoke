#include "music/KeyDetector.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <numeric>

namespace music {

namespace {

// 8192 samples at 11025 Hz: 0.74 s, 1.35 Hz per bin, enough to tell the
// semitones of a bass line apart (C2 to C#2 is 3.9 Hz). Half-overlapping.
constexpr std::size_t kFrameSize = 8192;
constexpr std::size_t kHop = 4096;
constexpr double kMinFrequency = 60.0;    // just under B1
constexpr double kMaxFrequency = 2000.0;  // about B6
constexpr std::size_t kMaxPeaksPerFrame = 60;
constexpr double kPeakFloor = 1e-3;       // -60 dB below the frame's strongest peak

// Loud enough to count: above -50 dBFS, and within 26 dB of the song's
// loud passages (so fades and near-silent intros do not count).
constexpr double kAbsoluteGate = 0.00316;
constexpr double kRelativeGate = 0.05;
// Too little to judge a song's main key by.
constexpr double kMinSeconds = 30.0;
constexpr double kMinVoicedSeconds = 20.0;
// Windows of about 20 seconds of sound, for consistency.
constexpr std::size_t kWindowFrames = 54;

// When a key counts as confident (see finish()). Starting values, set on
// synthetic music; to be checked against labelled real songs.
constexpr double kMinCorrelation = 0.60;
// The margin over the runner-up, relative to the room left above it:
// (best - second) / (1 - second).
constexpr double kMinMargin = 0.10;
// Major or minor must be clear too: C is no answer for a song in Cm.
constexpr double kMinParallelMargin = 0.25;
constexpr double kMinAgreement = 0.60;
// Near a quarter-tone off A = 440 Hz every note sits between two semitones,
// so the key could be one semitone out.
constexpr double kMaxTuningCents = 40.0;
// All of the above together (see KeyAnalysis::confidence).
constexpr double kMinConfidence = 0.40;

// Key templates for audio pitch-class profiles (Sha'ath 2011, derived from
// the chroma of recorded music, as used by the KeyFinder project). Index 0
// is the tonic.
constexpr std::array<double, 12> kMajorProfile = {
    7.23900502, 3.50351156, 3.58445296, 2.84511787, 5.81898764, 4.55865613,
    2.44767304, 6.99473180, 3.39106005, 4.55614869, 4.07392596, 4.45932622};
constexpr std::array<double, 12> kMinorProfile = {
    7.00255015, 3.14360295, 4.35904382, 5.40418916, 3.67234687, 4.08971145,
    3.90791280, 6.20514994, 3.63424116, 2.87605157, 5.35874450, 3.83747541};

using Chroma = std::array<double, 12>;

// In-place radix-2 FFT of a power-of-two length.
void fft(std::vector<std::complex<double>>& data, const std::vector<std::complex<double>>& twiddles)
{
    const std::size_t n = data.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(data[i], data[j]);
    }
    for (std::size_t length = 2; length <= n; length <<= 1) {
        const std::size_t half = length / 2;
        const std::size_t step = n / length;
        for (std::size_t start = 0; start < n; start += length) {
            for (std::size_t k = 0; k < half; ++k) {
                const std::complex<double> t = twiddles[k * step] * data[start + k + half];
                data[start + k + half] = data[start + k] - t;
                data[start + k] += t;
            }
        }
    }
}

struct Spectrum {
    std::vector<double> window;
    std::vector<std::complex<double>> halfTwiddles;   // for the N/2 complex FFT
    std::vector<std::complex<double>> fullTwiddles;   // for splitting the real FFT
    std::vector<std::complex<double>> buffer;
    std::vector<double> magnitude;

    Spectrum()
    {
        window.resize(kFrameSize);
        for (std::size_t i = 0; i < kFrameSize; ++i)
            window[i] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * double(i) / double(kFrameSize));
        const std::size_t half = kFrameSize / 2;
        halfTwiddles.resize(half / 2);
        for (std::size_t k = 0; k < half / 2; ++k)
            halfTwiddles[k] = std::polar(1.0, -2.0 * std::numbers::pi * double(k) / double(half));
        fullTwiddles.resize(half);
        for (std::size_t k = 0; k < half; ++k)
            fullTwiddles[k] = std::polar(1.0, -2.0 * std::numbers::pi * double(k) / double(kFrameSize));
        buffer.resize(half);
        magnitude.resize(half + 1);
    }

    // Magnitudes of bins 0..N/2 of a windowed real frame, using one complex
    // FFT of half the length.
    void compute(const float* samples)
    {
        const std::size_t half = kFrameSize / 2;
        for (std::size_t n = 0; n < half; ++n) {
            buffer[n] = {samples[2 * n] * window[2 * n], samples[2 * n + 1] * window[2 * n + 1]};
        }
        fft(buffer, halfTwiddles);
        for (std::size_t k = 0; k <= half; ++k) {
            const std::complex<double> a = buffer[k % half];
            const std::complex<double> b = std::conj(buffer[(half - k) % half]);
            const std::complex<double> even = 0.5 * (a + b);
            const std::complex<double> odd = std::complex<double>(0.0, -0.5) * (a - b);
            const std::complex<double> twiddle = k < half ? fullTwiddles[k] : std::complex<double>(-1.0, 0.0);
            magnitude[k] = std::abs(even + twiddle * odd);
        }
    }
};

Spectrum& spectrum()
{
    thread_local Spectrum instance;
    return instance;
}

double pearson(const Chroma& chroma, const std::array<double, 12>& profile, int tonic)
{
    double meanC = 0.0;
    double meanP = 0.0;
    for (int i = 0; i < 12; ++i) {
        meanC += chroma[static_cast<std::size_t>(i)];
        meanP += profile[static_cast<std::size_t>(i)];
    }
    meanC /= 12.0;
    meanP /= 12.0;
    double num = 0.0;
    double varC = 0.0;
    double varP = 0.0;
    for (int i = 0; i < 12; ++i) {
        const double c = chroma[static_cast<std::size_t>((i + tonic) % 12)] - meanC;
        const double p = profile[static_cast<std::size_t>(i)] - meanP;
        num += c * p;
        varC += c * c;
        varP += p * p;
    }
    if (varC <= 1e-12 || varP <= 1e-12)
        return 0.0;
    return num / std::sqrt(varC * varP);
}

struct Ranking {
    int best = -1;
    int second = -1;
    double bestScore = -2.0;
    double secondScore = -2.0;
};

Ranking rankKeys(const Chroma& chroma)
{
    Ranking ranking;
    for (int index = 0; index < 24; ++index) {
        const bool minor = index >= 12;
        const double score = pearson(chroma, minor ? kMinorProfile : kMajorProfile, index % 12);
        if (score > ranking.bestScore) {
            ranking.second = ranking.best;
            ranking.secondScore = ranking.bestScore;
            ranking.best = index;
            ranking.bestScore = score;
        } else if (score > ranking.secondScore) {
            ranking.second = index;
            ranking.secondScore = score;
        }
    }
    return ranking;
}

double clamp01(double value)
{
    return std::clamp(value, 0.0, 1.0);
}

} // namespace

const char* statusName(KeyAnalysis::Status status)
{
    switch (status) {
    case KeyAnalysis::Status::Confident: return "confident";
    case KeyAnalysis::Status::Uncertain: return "uncertain";
    case KeyAnalysis::Status::Silent: return "silent";
    case KeyAnalysis::Status::TooShort: return "too_short";
    }
    return "too_short";
}

KeyDetector::KeyDetector()
{
    m_pending.reserve(kFrameSize * 2);
    m_window.resize(kFrameSize);
}

void KeyDetector::addSamples(const float* samples, std::size_t count)
{
    m_totalSamples += count;
    m_pending.insert(m_pending.end(), samples, samples + count);
    std::size_t offset = 0;
    while (m_pending.size() - offset >= kFrameSize) {
        std::copy_n(m_pending.begin() + static_cast<std::ptrdiff_t>(offset), kFrameSize, m_window.begin());
        processFrame();
        offset += kHop;
    }
    m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(offset));
}

double KeyDetector::seconds() const
{
    return double(m_totalSamples) / kKeyAnalysisSampleRate;
}

void KeyDetector::processFrame()
{
    Frame frame;
    double energy = 0.0;
    for (const float sample : m_window)
        energy += double(sample) * double(sample);
    frame.rms = static_cast<float>(std::sqrt(energy / double(kFrameSize)));
    if (frame.rms <= 0.0F || !std::isfinite(frame.rms)) {
        frame.rms = 0.0F;
        m_frames.push_back(std::move(frame));
        return;
    }

    Spectrum& s = spectrum();
    s.compute(m_window.data());
    const double binHz = double(kKeyAnalysisSampleRate) / double(kFrameSize);
    const std::size_t first = std::max<std::size_t>(2, std::size_t(kMinFrequency / binHz));
    const std::size_t last = std::min(s.magnitude.size() - 2, std::size_t(kMaxFrequency / binHz) + 1);

    struct Candidate {
        double bin;
        double amplitude;
    };
    std::vector<Candidate> candidates;
    double strongest = 0.0;
    for (std::size_t k = first; k <= last; ++k) {
        const double m = s.magnitude[k];
        if (!(m > s.magnitude[k - 1] && m >= s.magnitude[k + 1]))
            continue;
        // Parabolic interpolation on the log magnitude.
        const double a = std::log(s.magnitude[k - 1] + 1e-12);
        const double b = std::log(m + 1e-12);
        const double c = std::log(s.magnitude[k + 1] + 1e-12);
        const double denominator = a - 2.0 * b + c;
        const double shift = denominator < 0.0 ? std::clamp(0.5 * (a - c) / denominator, -0.5, 0.5) : 0.0;
        const double amplitude = std::exp(b - 0.25 * (a - c) * shift);
        candidates.push_back({double(k) + shift, amplitude});
        strongest = std::max(strongest, amplitude);
    }
    if (candidates.size() > kMaxPeaksPerFrame) {
        std::nth_element(candidates.begin(), candidates.begin() + kMaxPeaksPerFrame, candidates.end(),
                         [](const Candidate& x, const Candidate& y) { return x.amplitude > y.amplitude; });
        candidates.resize(kMaxPeaksPerFrame);
    }
    for (const Candidate& candidate : candidates) {
        if (candidate.amplitude < strongest * kPeakFloor)
            continue;
        const double frequency = candidate.bin * binHz;
        if (frequency < kMinFrequency || frequency > kMaxFrequency)
            continue;
        const double pitch = 69.0 + 12.0 * std::log2(frequency / 440.0);
        frame.peaks.push_back({static_cast<float>(pitch), static_cast<float>(candidate.amplitude)});
    }
    m_frames.push_back(std::move(frame));
}

KeyAnalysis KeyDetector::finish()
{
    KeyAnalysis result;
    result.seconds = seconds();
    // The last part-frame, padded with silence, if it holds at least a hop.
    if (m_pending.size() >= kHop) {
        std::fill(m_window.begin(), m_window.end(), 0.0F);
        std::copy(m_pending.begin(), m_pending.end(), m_window.begin());
        processFrame();
    }
    m_pending.clear();
    if (result.seconds < kMinSeconds || m_frames.empty())
        return result;

    // Which frames are loud enough to count.
    std::vector<float> levels;
    levels.reserve(m_frames.size());
    for (const Frame& frame : m_frames)
        levels.push_back(frame.rms);
    std::vector<float> sorted = levels;
    const std::size_t p90 = std::min(sorted.size() - 1, sorted.size() * 9 / 10);
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(p90), sorted.end());
    const double loud = sorted[p90];
    const double gate = std::max(kAbsoluteGate, loud * kRelativeGate);
    std::vector<const Frame*> voiced;
    for (const Frame& frame : m_frames) {
        if (frame.rms >= gate && !frame.peaks.empty())
            voiced.push_back(&frame);
    }
    const double frameSeconds = double(kHop) / kKeyAnalysisSampleRate;
    result.voicedSeconds = double(voiced.size()) * frameSeconds;
    if (result.voicedSeconds < kMinVoicedSeconds) {
        result.status = KeyAnalysis::Status::Silent;
        return result;
    }

    // The song's tuning: the weighted circular mean of how far every peak
    // lies from the nearest equal-tempered semitone of A = 440 Hz.
    std::complex<double> tuningSum;
    for (const Frame* frame : voiced) {
        for (const Peak& peak : frame->peaks) {
            const double offset = double(peak.pitch) - std::round(double(peak.pitch));
            tuningSum += double(peak.weight) * std::polar(1.0, 2.0 * std::numbers::pi * offset);
        }
    }
    const double tuning = std::abs(tuningSum) > 0.0
        ? std::arg(tuningSum) / (2.0 * std::numbers::pi) : 0.0;  // semitones, -0.5..0.5
    result.tuningCents = tuning * 100.0;

    // Pitch-class profiles: each peak goes to its nearest semitone, weighted
    // down the further it lies between two. Each counted frame adds the same
    // total, so quiet and loud passages count alike.
    Chroma total{};
    std::vector<Chroma> windows;
    Chroma window{};
    std::size_t inWindow = 0;
    for (const Frame* frame : voiced) {
        Chroma chroma{};
        double sum = 0.0;
        for (const Peak& peak : frame->peaks) {
            const double pitch = double(peak.pitch) - tuning;
            const double nearest = std::round(pitch);
            const double distance = pitch - nearest;  // -0.5..0.5
            const double closeness = std::cos(std::numbers::pi * distance);
            const double weight = double(peak.weight) * closeness * closeness;
            int pc = static_cast<int>(nearest) % 12;
            if (pc < 0)
                pc += 12;
            chroma[static_cast<std::size_t>(pc)] += weight;
            sum += weight;
        }
        if (sum <= 0.0)
            continue;
        for (std::size_t i = 0; i < 12; ++i) {
            total[i] += chroma[i] / sum;
            window[i] += chroma[i] / sum;
        }
        if (++inWindow == kWindowFrames) {
            windows.push_back(window);
            window = {};
            inWindow = 0;
        }
    }
    if (inWindow >= kWindowFrames / 2)
        windows.push_back(window);

    const Ranking ranking = rankKeys(total);
    result.key = MusicalKey::fromIndex(ranking.best);
    result.runnerUp = MusicalKey::fromIndex(ranking.second);
    result.correlation = ranking.bestScore;
    result.runnerUpCorrelation = ranking.secondScore;
    result.windows = static_cast<int>(windows.size());
    int agreeing = 0;
    for (const Chroma& part : windows)
        agreeing += rankKeys(part).best == ranking.best ? 1 : 0;
    result.agreement = windows.empty() ? 0.0 : double(agreeing) / double(windows.size());

    // Confident only when the best key matches well, clearly better than any
    // other (major/minor included), holds through most of a song long enough
    // to tell, and the tuning leaves no doubt which semitone each note is.
    const double margin = ranking.bestScore < 1.0 && ranking.secondScore < 1.0
        ? (ranking.bestScore - ranking.secondScore) / (1.0 - ranking.secondScore) : 0.0;
    result.margin = margin;
    const bool judgeAgreement = windows.size() >= 3;
    const bool parallel = ranking.second >= 0 && ranking.second % 12 == ranking.best % 12;
    result.confidence = clamp01((ranking.bestScore - 0.5) / 0.35) * clamp01(margin / 0.4)
        * (judgeAgreement ? result.agreement : 1.0);
    const bool confident = ranking.bestScore >= kMinCorrelation && margin >= kMinMargin
        && (!parallel || margin >= kMinParallelMargin)
        && (!judgeAgreement || result.agreement >= kMinAgreement)
        && std::abs(result.tuningCents) <= kMaxTuningCents
        && result.confidence >= kMinConfidence;
    result.status = confident ? KeyAnalysis::Status::Confident : KeyAnalysis::Status::Uncertain;
    return result;
}

KeyAnalysis KeyDetector::analyse(const std::vector<float>& samples,
                                 const std::function<bool()>& cancelled)
{
    KeyDetector detector;
    constexpr std::size_t kBlock = 65536;
    for (std::size_t offset = 0; offset < samples.size(); offset += kBlock) {
        if (cancelled && cancelled())
            return {};
        detector.addSamples(samples.data() + offset, std::min(kBlock, samples.size() - offset));
    }
    return detector.finish();
}

} // namespace music
