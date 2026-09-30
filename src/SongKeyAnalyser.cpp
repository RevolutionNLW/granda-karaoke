#include "SongKeyAnalyser.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QUrl>

#include <gst/gst.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

namespace {

// How often the bus and stop() are looked at.
constexpr GstClockTime kPoll = 50 * GST_MSECOND;
// A song not decoded in this time (a drive that stopped answering, a
// pathological file) is left for another time.
constexpr qint64 kTimeoutMs = 3 * 60 * 1000;

// Filled on GStreamer's streaming thread; read only after the pipeline has
// stopped (GST_STATE_NULL waits for that thread). The handoff never waits
// for anything, so stopping the pipeline can never be held up by it.
struct Feed {
    music::KeyDetector detector;
    std::vector<float> block;
    std::size_t limit = 0;
    std::size_t taken = 0;
    std::atomic_bool full = false;
};

void onHandoff(GstElement*, GstBuffer* buffer, GstPad*, gpointer data)
{
    auto* feed = static_cast<Feed*>(data);
    if (feed->full.load())
        return;
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ))
        return;
    const std::size_t count = std::min<std::size_t>(map.size / sizeof(float), feed->limit - feed->taken);
    feed->block.resize(count);
    if (count > 0)
        std::memcpy(feed->block.data(), map.data, count * sizeof(float));
    gst_buffer_unmap(buffer, &map);
    feed->detector.addSamples(feed->block.data(), count);
    feed->taken += count;
    if (feed->taken >= feed->limit)
        feed->full.store(true);
}

QString describe(GstMessage* message, bool* readFailure)
{
    GError* error = nullptr;
    gchar* debug = nullptr;
    gst_message_parse_error(message, &error, &debug);
    // A file that cannot be opened or read is a reading problem (perhaps the
    // drive went away); anything else means the file holds no usable audio.
    *readFailure = error && error->domain == GST_RESOURCE_ERROR;
    const QString text = QString::fromUtf8(error ? error->message : "unknown error");
    g_clear_error(&error);
    g_free(debug);
    return text;
}

} // namespace

SongKeyEngine::Outcome GstSongKeyEngine::analyse(const QString& path,
                                                 const std::function<bool()>& stop,
                                                 music::KeyAnalysis* result, QString* detail)
{
    {
        // Told apart from a damaged file: a file that cannot be read at all.
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.read(1).isEmpty()) {
            *detail = QStringLiteral("could not read the file");
            return Outcome::Unreadable;
        }
    }

    const QByteArray description = QStringLiteral(
        "uridecodebin name=source ! audioconvert ! audioresample "
        "! audio/x-raw,format=F32LE,layout=interleaved,channels=1,rate=%1 "
        "! fakesink name=sink sync=false enable-last-sample=false signal-handoffs=true")
        .arg(music::kKeyAnalysisSampleRate).toUtf8();
    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(description.constData(), &error);
    if (!pipeline || error) {
        *detail = QStringLiteral("audio components unavailable: %1")
                      .arg(QString::fromUtf8(error ? error->message : "?"));
        g_clear_error(&error);
        if (pipeline)
            gst_object_unref(pipeline);
        // Not the file's fault: nothing is recorded against it.
        return Outcome::Unreadable;
    }

    Feed feed;
    feed.limit = std::size_t(kMaxSeconds) * music::kKeyAnalysisSampleRate;
    GstElement* source = gst_bin_get_by_name(GST_BIN(pipeline), "source");
    GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    const QByteArray uri = QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()).toEncoded();
    g_object_set(source, "uri", uri.constData(), nullptr);
    g_signal_connect(sink, "handoff", G_CALLBACK(onHandoff), &feed);
    gst_object_unref(source);
    gst_object_unref(sink);

    Outcome outcome = Outcome::Interrupted;
    GstBus* bus = gst_element_get_bus(pipeline);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    QElapsedTimer elapsed;
    elapsed.start();
    for (;;) {
        if (stop && stop()) {
            outcome = Outcome::Interrupted;
            break;
        }
        if (feed.full.load()) {
            outcome = Outcome::Analysed;  // the start of a very long file
            break;
        }
        if (elapsed.hasExpired(kTimeoutMs)) {
            *detail = QStringLiteral("took too long to read");
            outcome = Outcome::Unreadable;
            break;
        }
        GstMessage* message = gst_bus_timed_pop_filtered(
            bus, kPoll, GstMessageType(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        if (!message)
            continue;
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
            outcome = Outcome::Analysed;
        } else {
            bool readFailure = false;
            *detail = describe(message, &readFailure);
            outcome = readFailure ? Outcome::Unreadable : Outcome::NotAudio;
        }
        gst_message_unref(message);
        break;
    }
    // Waits for the streaming thread: after this nothing touches `feed`.
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(bus);
    gst_object_unref(pipeline);

    if (outcome == Outcome::Analysed)
        *result = feed.detector.finish();
    return outcome;
}

std::shared_ptr<SongKeyEngine> createSongKeyEngine()
{
    if (!gst_is_initialized())
        return nullptr;
    return std::make_shared<GstSongKeyEngine>();
}
