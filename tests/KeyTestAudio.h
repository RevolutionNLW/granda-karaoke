#pragma once

// Synthetic music in known keys for the key-detection tests: chord
// progressions made by additive synthesis (each note with its harmonics and a
// bass note), optionally with drums and a melody, rendered to MP3 with
// GStreamer. Nothing here comes from real recordings.

#include <QDir>
#include <QFile>
#include <QString>

#include <gst/gst.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <random>
#include <vector>

namespace keyaudio {

struct Synth {
    explicit Synth(int sampleRate) : rate(sampleRate) {}

    int rate;
    std::vector<float> out;
    std::mt19937 rng{42};

    static double frequency(int midi, double cents = 0.0)
    {
        return 440.0 * std::pow(2.0, (midi - 69 + cents / 100.0) / 12.0);
    }

    // Notes (MIDI numbers) held together, each with six decaying harmonics.
    void chord(const std::vector<int>& notes, double seconds, double amplitude = 0.12,
               double cents = 0.0)
    {
        const std::size_t count = std::size_t(seconds * rate);
        const std::size_t start = out.size();
        out.resize(start + count, 0.0F);
        std::uniform_real_distribution<double> phaseOf(0.0, 2.0 * std::numbers::pi);
        for (const int note : notes) {
            const double base = frequency(note, cents);
            for (int harmonic = 1; harmonic <= 6; ++harmonic) {
                const double f = base * harmonic;
                if (f > rate / 2.2)
                    break;
                const double a = amplitude / harmonic;
                const double phase = phaseOf(rng);
                for (std::size_t i = 0; i < count; ++i) {
                    const double envelope = std::min({1.0, double(i) / (0.02 * rate),
                                                      double(count - i) / (0.05 * rate)});
                    out[start + i] += float(a * envelope
                                            * std::sin(2.0 * std::numbers::pi * f * double(i) / rate + phase));
                }
            }
        }
    }
    void silence(double seconds) { out.resize(out.size() + std::size_t(seconds * rate), 0.0F); }
    void noise(double seconds, double amplitude)
    {
        std::normal_distribution<float> value(0.0F, float(amplitude));
        for (std::size_t i = 0, n = std::size_t(seconds * rate); i < n; ++i)
            out.push_back(value(rng));
    }
};

inline std::vector<int> triad(int root, bool minorChord, bool seventh = false)
{
    std::vector<int> notes{root, root + (minorChord ? 3 : 4), root + 7};
    if (seventh)
        notes.push_back(root + 10);
    notes.push_back(root - 24);  // bass
    return notes;
}

// Major: I IV V I vi IV V7 I. Minor: i iv V7 i VI iv V7 i (harmonic minor).
inline void progression(Synth& synth, int tonic, bool minor, double seconds,
                        double chordSeconds = 2.0, double cents = 0.0)
{
    struct Step { int degree; bool minorChord; bool seventh; };
    static const std::vector<Step> major = {{0, false, false}, {5, false, false}, {7, false, false},
                                            {0, false, false}, {9, true, false},  {5, false, false},
                                            {7, false, true},  {0, false, false}};
    static const std::vector<Step> minorSteps = {{0, true, false}, {5, true, false}, {7, false, true},
                                                 {0, true, false}, {8, false, false}, {5, true, false},
                                                 {7, false, true}, {0, true, false}};
    const std::vector<Step>& steps = minor ? minorSteps : major;
    double done = 0.0;
    for (std::size_t i = 0; done < seconds; ++i) {
        const Step step = steps[i % steps.size()];
        int root = tonic + step.degree;
        while (root > 64)
            root -= 12;
        synth.chord(triad(root, step.minorChord, step.seventh), chordSeconds, 0.12, cents);
        done += chordSeconds;
    }
}

// A band: the progression with drums and a melody in the key.
inline void band(Synth& synth, int tonic, bool minor, double seconds)
{
    Synth chords(synth.rate);
    progression(chords, tonic, minor, seconds);
    std::vector<float> mix = chords.out;
    std::mt19937 rng(7);
    std::normal_distribution<float> noise(0.0F, 1.0F);
    const std::size_t beat = std::size_t(0.5 * synth.rate);
    for (std::size_t b = 0; b * beat < mix.size(); ++b) {
        for (std::size_t i = 0; i < std::size_t(0.15 * synth.rate) && b * beat + i < mix.size(); ++i) {
            const double envelope = std::exp(-double(i) / (0.03 * synth.rate));
            mix[b * beat + i] += float((b % 2 ? 0.25 : 0.12) * envelope * noise(rng));
            if (b % 2 == 0)
                mix[b * beat + i] += float(0.4 * envelope
                                           * std::sin(2.0 * std::numbers::pi * (60.0 + 40.0 * envelope)
                                                      * double(i) / synth.rate));
        }
    }
    static const int majorScale[] = {0, 2, 4, 5, 7, 9, 11};
    static const int minorScale[] = {0, 2, 3, 5, 7, 8, 10};
    std::uniform_int_distribution<int> degree(0, 6);
    Synth melody(synth.rate);
    while (melody.out.size() < mix.size())
        melody.chord({tonic + 12 + (minor ? minorScale : majorScale)[degree(rng)]}, 0.25, 0.08);
    for (std::size_t i = 0; i < mix.size(); ++i)
        mix[i] += melody.out[i];
    synth.out.insert(synth.out.end(), mix.begin(), mix.end());
}

inline QByteArray gstLocation(const QString& path)
{
#ifdef Q_OS_WIN
    return path.toUtf8();  // GLib file names are UTF-8 on Windows
#else
    return QFile::encodeName(path);
#endif
}

// 16-bit mono WAV.
inline bool writeWav(const QString& path, const std::vector<float>& samples, int rate)
{
    QByteArray data;
    data.reserve(qsizetype(samples.size() * 2));
    for (const float sample : samples) {
        const int value = int(std::lround(std::clamp(sample, -1.0F, 1.0F) * 32767.0F));
        data.append(char(value & 0xff));
        data.append(char((value >> 8) & 0xff));
    }
    const auto le32 = [](quint32 v) {
        QByteArray bytes;
        for (int shift = 0; shift < 32; shift += 8)
            bytes.append(char((v >> shift) & 0xff));
        return bytes;
    };
    const auto le16 = [](quint16 v) {
        QByteArray bytes;
        bytes.append(char(v & 0xff));
        bytes.append(char((v >> 8) & 0xff));
        return bytes;
    };
    QByteArray header = QByteArray("RIFF") + le32(quint32(36 + data.size())) + "WAVEfmt " + le32(16) + le16(1)
        + le16(1) + le32(quint32(rate)) + le32(quint32(rate * 2)) + le16(2) + le16(16) + "data"
        + le32(quint32(data.size()));
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(header) == header.size()
        && file.write(data) == data.size();
}

inline bool runPipeline(const QByteArray& description, const QString& in, const QString& out)
{
    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(description.constData(), &error);
    if (!pipeline || error) {
        g_clear_error(&error);
        if (pipeline)
            gst_object_unref(pipeline);
        return false;
    }
    GstElement* source = gst_bin_get_by_name(GST_BIN(pipeline), "in");
    GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "out");
    g_object_set(source, "location", gstLocation(in).constData(), nullptr);
    g_object_set(sink, "location", gstLocation(out).constData(), nullptr);
    gst_object_unref(source);
    gst_object_unref(sink);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstBus* bus = gst_element_get_bus(pipeline);
    GstMessage* message = gst_bus_timed_pop_filtered(
        bus, 120 * GST_SECOND, GstMessageType(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    const bool ok = message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
    if (message)
        gst_message_unref(message);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return ok;
}

// Encodes samples to a 128 kbit/s stereo 44.1 kHz MP3 with an ID3 tag, as
// karaoke MP3s are. The WAV on the way is written beside it and removed.
inline bool writeMp3(const QString& path, const std::vector<float>& samples, int rate)
{
    const QString wav = path + QStringLiteral(".tmp.wav");
    if (!writeWav(wav, samples, rate))
        return false;
    const bool ok = runPipeline(
        "filesrc name=in ! wavparse ! audioconvert ! audioresample "
        "! audio/x-raw,rate=44100,channels=2 ! lamemp3enc target=bitrate bitrate=128 cbr=true "
        "! id3v2mux ! filesink name=out",
        wav, path);
    QFile::remove(wav);
    return ok;
}

} // namespace keyaudio
