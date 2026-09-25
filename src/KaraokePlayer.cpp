#include "KaraokePlayer.h"

#include "Logging.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUrl>

#include <gst/gst.h>

namespace {

constexpr int kTickIntervalMs = 20;

constexpr qint64 kMaxCdgBytes = 64 * 1024 * 1024;

// playbin "flags": audio only (no video/visualisation for MP3 cover art),
// with software volume so a volume control can be added later.
constexpr guint kPlayFlagAudio = 1 << 1;
constexpr guint kPlayFlagSoftVolume = 1 << 4;

const char* stateName(GstState state)
{
    return gst_element_state_get_name(state);
}

} // namespace

KaraokePlayer::KaraokePlayer(QObject* parent, const QString& audioSinkName)
    : QObject(parent)
    , m_audioSinkName(audioSinkName)
{
    m_timer.setInterval(kTickIntervalMs);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &KaraokePlayer::tick);
}

KaraokePlayer::~KaraokePlayer()
{
    reportSkippedTotal();
    destroyPipeline();
}

bool KaraokePlayer::initializeGStreamer(QString* errorMessage)
{
    // Use a registry private to this application so a different GStreamer
    // installation on the same machine cannot interfere with plugin discovery.
    if (!qEnvironmentVariableIsSet("GST_REGISTRY")) {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        if (!dir.isEmpty() && QDir().mkpath(dir)) {
            const QString registry = QDir(dir).filePath(
                QStringLiteral("gstreamer-registry-%1.bin").arg(QSysInfo::buildCpuArchitecture()));
            qputenv("GST_REGISTRY", QFile::encodeName(registry));
        }
    }

    for (const char* name : {"GST_PLUGIN_PATH", "GST_PLUGIN_PATH_1_0",
                             "GST_PLUGIN_SYSTEM_PATH", "GST_PLUGIN_SYSTEM_PATH_1_0"}) {
        if (qEnvironmentVariableIsSet(name)) {
            qCInfo(lcPlayer) << "Ignoring external plugin path" << name << "=" << qgetenv(name);
            qunsetenv(name);
        }
    }

    GError* error = nullptr;
    if (!gst_init_check(nullptr, nullptr, &error)) {
        qCCritical(lcPlayer) << "gst_init_check failed:" << (error ? error->message : "unknown error");
        g_clear_error(&error);
        if (errorMessage)
            *errorMessage = QStringLiteral("The audio system (GStreamer) could not be started.");
        return false;
    }

    gchar* version = gst_version_string();
    qCInfo(lcPlayer) << "Initialised" << version << "registry:" << qgetenv("GST_REGISTRY");
    g_free(version);

    if (GstPlugin* plugin = gst_plugin_load_by_name("coreelements")) {
        qCInfo(lcPlayer) << "Loaded coreelements:" << gst_plugin_get_filename(plugin);
        gst_object_unref(plugin);
    }
    for (const char* name : {"pitch", "scaletempo"}) {
        GstElementFactory* factory = gst_element_factory_find(name);
        if (factory)
            gst_object_unref(factory);
        else
            qCWarning(lcPlayer) << "Optional GStreamer element missing:" << name;
    }

    for (const char* name : {"playbin", "autoaudiosink"}) {
        GstElementFactory* factory = gst_element_factory_find(name);
        if (!factory) {
            qCCritical(lcPlayer) << "Required GStreamer element missing:" << name;
            if (errorMessage)
                *errorMessage = QStringLiteral("The audio system is incomplete (missing \"%1\"). "
                                               "Please reinstall the application.").arg(QLatin1String(name));
            return false;
        }
        gst_object_unref(factory);
    }
    return true;
}

bool KaraokePlayer::ensurePipeline()
{
    if (m_pipeline)
        return true;

    m_pipeline = gst_element_factory_make("playbin", "karaoke-playbin");
    if (!m_pipeline) {
        qCCritical(lcPlayer) << "Could not create playbin";
        return false;
    }
    gst_object_ref_sink(m_pipeline);
    g_object_set(m_pipeline, "flags", kPlayFlagAudio | kPlayFlagSoftVolume, nullptr);

    // Key and tempo adjustment will later be inserted here as a custom
    // audio-sink bin (pitch/scaletempo ! audioconvert ! sink).
    if (!m_audioSinkName.isEmpty()) {
        GstElement* sink = gst_element_factory_make(m_audioSinkName.toUtf8().constData(), nullptr);
        if (!sink) {
            qCCritical(lcPlayer) << "Could not create audio sink" << m_audioSinkName;
            destroyPipeline();
            return false;
        }
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(sink), "sync"))
            g_object_set(sink, "sync", TRUE, nullptr);
        g_object_set(m_pipeline, "audio-sink", sink, nullptr);
    }
    return true;
}

void KaraokePlayer::destroyPipeline()
{
    m_timer.stop();
    if (!m_pipeline)
        return;
    gst_element_set_state(m_pipeline, GST_STATE_NULL);
    gst_object_unref(m_pipeline);
    m_pipeline = nullptr;
}

bool KaraokePlayer::setPipelineState(int gstState)
{
    const auto target = static_cast<GstState>(gstState);
    const GstStateChangeReturn ret = gst_element_set_state(m_pipeline, target);
    if (ret != GST_STATE_CHANGE_FAILURE)
        return true;

    qCWarning(lcPlayer) << "State change to" << stateName(target) << "failed";
    // The bus normally carries the specific error; report that if present.
    tick();
    if (!m_errorReported)
        fail(QStringLiteral("The song \"%1\" could not be played.").arg(m_song.displayName()));
    return false;
}

bool KaraokePlayer::load(const SongPair& pair)
{
    qCInfo(lcPlayer) << "Loading" << pair.mp3Path << "+" << pair.cdgPath;

    auto loadFailed = [this](const QString& message) {
        m_lastError = message;
        emit errorOccurred(message);
        return false;
    };

    QFile cdgFile(pair.cdgPath);
    if (!cdgFile.open(QIODevice::ReadOnly)) {
        qCWarning(lcCdg) << "Cannot open" << pair.cdgPath << ":" << cdgFile.errorString();
        return loadFailed(QStringLiteral("The lyrics file for \"%1\" could not be opened.")
                              .arg(pair.displayName()));
    }
    auto tooLarge = [&]() {
        return loadFailed(QStringLiteral("The lyrics file for \"%1\" is too large (maximum 64 MB).")
                              .arg(pair.displayName()));
    };
    if (cdgFile.size() > kMaxCdgBytes)
        return tooLarge();
    // Bound the read too, in case the file grows after the size check.
    const QByteArray bytes = cdgFile.read(kMaxCdgBytes + 1);
    if (bytes.size() > kMaxCdgBytes)
        return tooLarge();
    cdg::CdgDecoder candidate;
    candidate.setData(std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
    if (cdgFile.error() != QFileDevice::NoError || candidate.graphicsPacketCount() == 0) {
        qCWarning(lcCdg) << "CDG file unreadable or without graphics:" << bytes.size() << "bytes";
        return loadFailed(QStringLiteral("The lyrics file for \"%1\" is empty or damaged.")
                              .arg(pair.displayName()));
    }
    // Validation succeeded; only now release the old song and its pipeline state.
    m_errorReported = false;
    m_lastError.clear();
    reportSkippedTotal();
    if (m_pipeline) {
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
        discardPendingMessages();
    }
    m_timer.stop();
    m_song = {};
    m_positionMs = 0;
    m_durationMs = 0;
    m_skippedWarningLogged = false;
    m_decoder = std::move(candidate);
    // A replacement decoder can have the same revision as the previous one.
    m_shownRevision = m_decoder.revision() - 1;
    setState(State::Empty);
    emitFrameIfChanged();
    if (m_decoder.trailingBytes() != 0)
        qCWarning(lcCdg) << "CDG file has" << m_decoder.trailingBytes()
                         << "trailing bytes (incomplete packet); ignoring them";
    qCInfo(lcCdg) << "CDG packets:" << m_decoder.packetCount()
                  << "duration ms:" << m_decoder.durationMs();

    if (!ensurePipeline())
        return loadFailed(QStringLiteral("The audio system could not play music."));

    const QByteArray uri = QUrl::fromLocalFile(pair.mp3Path).toEncoded();
    g_object_set(m_pipeline, "uri", uri.constData(), nullptr);

    m_song = pair;
    // Pre-roll so a damaged MP3 is reported now rather than when Play is pressed.
    // Errors arrive asynchronously on the bus and are handled by tick().
    if (gst_element_set_state(m_pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
        qCWarning(lcPlayer) << "Pre-roll failed for" << uri;
        tick();
        if (!m_errorReported)
            fail(QStringLiteral("The music file for \"%1\" could not be played.").arg(pair.displayName()));
        m_timer.start();
        return true;  // The song is loaded but in the Error state; the user has been told.
    }

    m_decoder.reset();
    emitFrameIfChanged();
    setState(State::Ready);
    m_timer.start();
    return true;
}

void KaraokePlayer::play()
{
    if (!hasSong() || !m_pipeline || m_state == State::Playing)
        return;

    if (m_state == State::Ready) {
        // Pre-roll errors belong to this attempt, not a previous playback.
        tick();
        if (m_state == State::Error)
            return;
    }
    m_errorReported = false;
    m_lastError.clear();
    if (m_state != State::Paused) {
        if (m_state == State::Stopped || m_state == State::Finished || m_state == State::Error)
            discardPendingMessages();
        // Every other state is already positioned at the beginning.
        m_positionMs = 0;
        m_decoder.reset();
        emitFrameIfChanged();
    }
    qCInfo(lcPlayer) << (m_state == State::Paused ? "Resuming at" : "Playing from") << m_positionMs << "ms";
    if (!setPipelineState(GST_STATE_PLAYING))
        return;
    setState(State::Playing);
}

void KaraokePlayer::pause()
{
    if (m_state != State::Playing)
        return;
    if (!setPipelineState(GST_STATE_PAUSED))
        return;
    syncToAudioPosition();
    qCInfo(lcPlayer) << "Paused at" << m_positionMs << "ms";
    setState(State::Paused);
}

void KaraokePlayer::stop()
{
    if (!hasSong() || !m_pipeline)
        return;
    qCInfo(lcPlayer) << "Stopping at" << m_positionMs << "ms";
    // READY releases the decoder and audio device and returns to the start.
    gst_element_set_state(m_pipeline, GST_STATE_READY);
    discardPendingMessages();
    reportSkippedTotal();
    resetToBeginning();
    setState(State::Stopped);
}

void KaraokePlayer::tick()
{
    if (!m_pipeline)
        return;

    GstBus* bus = gst_element_get_bus(m_pipeline);
    while (GstMessage* message = gst_bus_pop(bus)) {
        handleMessage(message);
        gst_message_unref(message);
    }
    gst_object_unref(bus);

    if (m_state == State::Playing || m_state == State::Paused)
        syncToAudioPosition();
}

bool KaraokePlayer::queryAudioPosition(qint64& positionMs) const
{
    gint64 position = 0;
    if (!gst_element_query_position(m_pipeline, GST_FORMAT_TIME, &position) || position < 0)
        return false;
    positionMs = position / GST_MSECOND;
    return true;
}

void KaraokePlayer::syncToAudioPosition()
{
    qint64 ms = 0;
    if (!queryAudioPosition(ms) || ms < 0)
        return;  // Still starting; keep the current lyrics.

    if (m_durationMs <= 0) {
        gint64 duration = 0;
        if (gst_element_query_duration(m_pipeline, GST_FORMAT_TIME, &duration) && duration > 0)
            m_durationMs = duration / GST_MSECOND;
    }

    if (ms != m_positionMs) {
        m_positionMs = ms;
        emit positionChanged(m_positionMs, m_durationMs);
    }
    m_decoder.advanceTo(m_positionMs);
    if (m_decoder.skippedInstructions() != 0 && !m_skippedWarningLogged) {
        qCWarning(lcCdg) << "Ignoring malformed or unsupported CDG instructions;"
                           " total will be logged when the song ends";
        m_skippedWarningLogged = true;
    }
    // Rewinding replays packets: count each skipped instruction only once.
    m_reportedSkipped = std::max(m_reportedSkipped, m_decoder.skippedInstructions());
    emitFrameIfChanged();
}

void KaraokePlayer::handleError(GstMessage* message, bool report)
{
    GError* error = nullptr;
    gchar* debug = nullptr;
    gst_message_parse_error(message, &error, &debug);
    const QString source = QString::fromUtf8(GST_OBJECT_NAME(GST_MESSAGE_SRC(message)));
    qCCritical(lcPlayer) << "GStreamer error from" << source << ":"
                         << (error ? error->message : "?") << "|" << (debug ? debug : "");

    QString userMessage;
    if (error && error->domain == GST_STREAM_ERROR) {
        userMessage = QStringLiteral("The music file for \"%1\" could not be played. "
                                     "It may be damaged or in an unsupported format.")
                          .arg(m_song.displayName());
    } else if (error && error->domain == GST_RESOURCE_ERROR && source.contains(QLatin1String("sink"))) {
        userMessage = QStringLiteral("The computer's sound output could not be used. "
                                     "Please check that speakers or headphones are connected.");
    } else if (error && error->domain == GST_RESOURCE_ERROR) {
        userMessage = QStringLiteral("The music file for \"%1\" could not be read.")
                          .arg(m_song.displayName());
    } else {
        userMessage = QStringLiteral("The song \"%1\" could not be played.").arg(m_song.displayName());
    }
    g_clear_error(&error);
    g_free(debug);
    if (report)
        fail(userMessage);
}

void KaraokePlayer::discardPendingMessages()
{
    if (!m_pipeline)
        return;
    GstBus* bus = gst_element_get_bus(m_pipeline);
    while (GstMessage* message = gst_bus_pop(bus)) {
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR)
            handleError(message, false);  // Preserve diagnostics, never surface stale errors.
        gst_message_unref(message);
    }
    gst_object_unref(bus);
}

void KaraokePlayer::reportSkippedTotal()
{
    if (m_reportedSkipped != 0) {
        qCInfo(lcCdg) << "Total ignored CDG instructions:" << m_reportedSkipped;
        m_reportedSkipped = 0;
    }
}

bool KaraokePlayer::hasAutoAudioFakeSink(GstElement* pipeline)
{
    if (!GST_IS_BIN(pipeline))
        return false;
    GstIterator* iterator = gst_bin_iterate_recurse(GST_BIN(pipeline));
    GValue item = G_VALUE_INIT;
    bool found = false;
    bool done = false;
    while (!done && !found) {
        switch (gst_iterator_next(iterator, &item)) {
        case GST_ITERATOR_OK: {
            auto* element = GST_ELEMENT(g_value_get_object(&item));
            GstElementFactory* factory = gst_element_get_factory(element);
            if (factory && g_str_equal(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)), "fakesink")) {
                GstObject* parent = gst_object_get_parent(GST_OBJECT(element));
                GstElementFactory* parentFactory = parent && GST_IS_ELEMENT(parent)
                    ? gst_element_get_factory(GST_ELEMENT(parent)) : nullptr;
                found = parentFactory && g_str_equal(
                    gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(parentFactory)), "autoaudiosink");
                if (parent)
                    gst_object_unref(parent);
            }
            g_value_reset(&item);
            break;
        }
        case GST_ITERATOR_RESYNC:
            gst_iterator_resync(iterator);
            break;
        default:
            done = true;
            break;
        }
    }
    if (G_VALUE_TYPE(&item))
        g_value_unset(&item);
    gst_iterator_free(iterator);
    return found;
}

void KaraokePlayer::checkAudioSink()
{
    if (!m_audioSinkName.isEmpty())
        return;
    // Bus state messages can describe an older transition. At READY, an
    // autoaudiosink may contain its placeholder fakesink even on a healthy device.
    GstState current;
    if (gst_element_get_state(m_pipeline, &current, nullptr, 0) != GST_STATE_CHANGE_SUCCESS
        || current < GST_STATE_PAUSED)
        return;
    if (hasAutoAudioFakeSink(m_pipeline)) {
        fail(QStringLiteral("The computer's sound output could not be used. "
                            "Please check that speakers or headphones are connected."));
    }
}

void KaraokePlayer::handleMessage(GstMessage* message)
{
    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_ERROR:
        handleError(message, true);
        break;
    case GST_MESSAGE_WARNING: {
        GError* error = nullptr;
        gchar* debug = nullptr;
        gst_message_parse_warning(message, &error, &debug);
        qCWarning(lcPlayer) << "GStreamer warning:" << (error ? error->message : "?") << "|" << (debug ? debug : "");
        g_clear_error(&error);
        g_free(debug);
        break;
    }
    case GST_MESSAGE_EOS:
        if (m_state != State::Playing)
            break;
        qCInfo(lcPlayer) << "End of song at" << m_positionMs << "ms";
        gst_element_set_state(m_pipeline, GST_STATE_READY);
        reportSkippedTotal();
        resetToBeginning();
        setState(State::Finished);
        break;
    case GST_MESSAGE_STATE_CHANGED:
        if (GST_MESSAGE_SRC(message) == GST_OBJECT(m_pipeline)) {
            GstState oldState, newState, pending;
            gst_message_parse_state_changed(message, &oldState, &newState, &pending);
            qCDebug(lcPlayer) << "Pipeline" << stateName(oldState) << "->" << stateName(newState);
            if (newState == GST_STATE_PAUSED || newState == GST_STATE_PLAYING)
                checkAudioSink();
        }
        break;
    default:
        break;
    }
}

void KaraokePlayer::resetToBeginning()
{
    m_decoder.reset();
    m_positionMs = 0;
    emit positionChanged(m_positionMs, m_durationMs);
    emitFrameIfChanged();
}

void KaraokePlayer::fail(const QString& userMessage)
{
    if (m_errorReported)
        return;
    m_errorReported = true;
    if (m_pipeline) {
        gst_element_set_state(m_pipeline, GST_STATE_READY);
        discardPendingMessages();
    }
    reportSkippedTotal();
    resetToBeginning();
    m_lastError = userMessage;
    setState(State::Error);
    emit errorOccurred(userMessage);
}

void KaraokePlayer::setState(State state)
{
    if (m_state == state)
        return;
    qCDebug(lcPlayer) << "State" << m_state << "->" << state;
    m_state = state;
    emit stateChanged(state);
}

void KaraokePlayer::emitFrameIfChanged()
{
    if (m_decoder.revision() == m_shownRevision)
        return;
    m_shownRevision = m_decoder.revision();
    emit frameChanged(currentFrame());
}

QImage KaraokePlayer::currentFrame() const
{
    QImage image(cdg::CdgDecoder::kWidth, cdg::CdgDecoder::kHeight, QImage::Format_RGB32);
    m_decoder.renderArgb32(reinterpret_cast<std::uint32_t*>(image.bits()),
                           static_cast<int>(image.bytesPerLine() / 4));
    return image;
}
