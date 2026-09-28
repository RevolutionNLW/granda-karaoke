#pragma once

// Synthetic MP3/CDG pairs for player and UI tests, so the tests never depend
// on real karaoke media.

#include "CdgTestData.h"

#include <QFile>
#include <QString>

#include <gst/gst.h>

namespace testmedia {

// Encodes a quiet sine tone of the given length to an MP3 using GStreamer.
inline bool writeMp3(const QString& path, int durationMs)
{
    GError* error = nullptr;
    const int buffers = durationMs / 10;  // 441 samples at 44.1 kHz = 10 ms
    const QByteArray description = QStringLiteral(
        "audiotestsrc num-buffers=%1 samplesperbuffer=441 volume=0.05 "
        "! audio/x-raw,rate=44100,channels=2 ! audioconvert ! lamemp3enc ! filesink name=out")
        .arg(buffers).toUtf8();
    GstElement* pipeline = gst_parse_launch(description.constData(), &error);
    if (!pipeline) {
        g_clear_error(&error);
        return false;
    }
    GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "out");
#ifdef Q_OS_WIN
    const QByteArray location = path.toUtf8();  // GLib file names are UTF-8 on Windows
#else
    const QByteArray location = QFile::encodeName(path);
#endif
    g_object_set(sink, "location", location.constData(), nullptr);
    gst_object_unref(sink);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstBus* bus = gst_element_get_bus(pipeline);
    GstMessage* message = gst_bus_timed_pop_filtered(
        bus, 30 * GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    const bool ok = message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
    if (message)
        gst_message_unref(message);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return ok;
}

inline bool writeFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

inline bool writeCdg(const QString& path, const std::vector<std::uint8_t>& data)
{
    return writeFile(path, QByteArray(reinterpret_cast<const char*>(data.data()),
                                      static_cast<qsizetype>(data.size())));
}

// A CDG stream with a white palette entry, a tile drawn at markerMs and a
// second tile at 2*markerMs.
inline std::vector<std::uint8_t> markerCdg(int durationMs, int markerMs)
{
    const std::size_t packets = static_cast<std::size_t>(durationMs) * 300 / 1000;
    return testcdg::stream(packets, {
        {0, testcdg::loadColors(false, {0x000, 0xFFF, 0xF00, 0, 0, 0, 0, 0})},
        {1, testcdg::memoryPreset(0)},
        {static_cast<std::size_t>(markerMs) * 300 / 1000, testcdg::solidTile(1, 1, 1)},
        {static_cast<std::size_t>(markerMs) * 600 / 1000, testcdg::solidTile(2, 1, 2)},
    });
}

} // namespace testmedia
