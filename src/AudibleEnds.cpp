#include "AudibleEnds.h"

#include "BackgroundWork.h"
#include "library/ContentIdentity.h"
#include "library/WritePriority.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QPointer>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QUrl>
#include <QUuid>

#include <gst/gst.h>

#include <algorithm>

namespace {

Q_LOGGING_CATEGORY(lcAutoplayEnd, "fks.autoplay.end")

// How often the bus and stop() are looked at.
constexpr GstClockTime kPoll = 50 * GST_MSECOND;
// A song not decoded in this time (a drive that stopped answering) is left.
constexpr qint64 kTimeoutMs = 3 * 60 * 1000;

// Filled on GStreamer's streaming thread; read only after the pipeline has
// stopped (GST_STATE_NULL waits for that thread).
struct Feed {
    music::AudibleEndDetector detector;
    std::int64_t limit = 0;
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
    const std::size_t frames = map.size / (sizeof(float) * music::kAudibleEndChannels);
    feed->detector.addSamples(reinterpret_cast<const float*>(map.data), frames);
    gst_buffer_unmap(buffer, &map);
    if (feed->detector.framesSeen() >= feed->limit)
        feed->full.store(true);
}

QString describe(GstMessage* message, bool* readFailure)
{
    GError* error = nullptr;
    gchar* debug = nullptr;
    gst_message_parse_error(message, &error, &debug);
    // As for song keys: a file that cannot be opened or read, or a missing
    // decoder, is not the file's fault and is tried again another time.
    *readFailure = error
        && (error->domain == GST_RESOURCE_ERROR
            || (error->domain == GST_CORE_ERROR && error->code == GST_CORE_ERROR_MISSING_PLUGIN)
            || (error->domain == GST_STREAM_ERROR && error->code == GST_STREAM_ERROR_CODEC_NOT_FOUND));
    const QString text = QString::fromUtf8(error ? error->message : "unknown error");
    g_clear_error(&error);
    g_free(debug);
    return text;
}

// ---- The cache (on the job's own connection) ------------------------------

const QString kSkip = QStringLiteral("skip");
const QString kPlayToEnd = QStringLiteral("play_to_end");

class CacheConnection {
public:
    explicit CacheConnection(const QString& path)
        : m_name(QStringLiteral("audible-ends-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)))
    {
        if (path.isEmpty())
            return;
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_name);
        database.setDatabaseName(path);
        database.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=%1")
                                       .arg(writepriority::BriefWait::kBriefWaitMs));
        m_open = database.open();
        if (!m_open)
            qCWarning(lcAutoplayEnd).noquote() << "Could not open the cache:" << database.lastError().text();
    }
    ~CacheConnection()
    {
        if (QSqlDatabase::contains(m_name)) {
            QSqlDatabase::database(m_name, false).close();
            QSqlDatabase::removeDatabase(m_name);
        }
    }
    bool isOpen() const { return m_open; }
    QSqlDatabase database() const { return QSqlDatabase::database(m_name, false); }

private:
    QString m_name;
    bool m_open = false;
};

// A stored result, only if it is from this version of the rules and makes
// sense by them (a damaged row must never end a song early).
std::optional<music::AudibleEnd> readCached(const QSqlDatabase& database, const QByteArray& digest)
{
    using Detector = music::AudibleEndDetector;
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "SELECT version,status,duration_ms,audible_end_ms,end_at_ms,reference_db,silence_db,reason "
        "FROM audible_ends WHERE mp3_complete_audio_sha256=?"));
    query.addBindValue(digest);
    if (!query.exec() || !query.next())
        return std::nullopt;  // not there (or no table yet)
    if (query.value(0).toInt() != music::kAudibleEndVersion)
        return std::nullopt;
    music::AudibleEnd end;
    const QString status = query.value(1).toString();
    end.verdict = status == kSkip ? music::AudibleEnd::Verdict::Skip
                                  : music::AudibleEnd::Verdict::PlayToEnd;
    end.durationMs = query.value(2).toLongLong();
    end.audibleEndMs = query.value(3).toLongLong();
    end.endAtMs = query.value(4).toLongLong();
    end.referenceDb = query.value(5).toDouble();
    end.silenceDb = query.value(6).toDouble();
    end.reason = query.value(7).toString().toStdString();
    if (status != kSkip && status != kPlayToEnd)
        return std::nullopt;
    if (end.skips()
        && (end.durationMs < Detector::kMinSongMs || end.endAtMs != end.audibleEndMs + Detector::kGapMs
            || end.durationMs - end.endAtMs < Detector::kMinSavedMs
            || double(end.audibleEndMs) < Detector::kMinMusicShare * double(end.durationMs)))
        return std::nullopt;
    return end;
}

bool storeResult(QSqlDatabase& database, const QByteArray& digest, const music::AudibleEnd& end,
                 const std::function<bool()>& stop, QString* error)
{
    return writepriority::retryWhileBusy([&](QString* attemptError) {
        QSqlQuery query(database);
        if (!query.exec(QStringLiteral(
                "CREATE TABLE IF NOT EXISTS audible_ends(mp3_complete_audio_sha256 BLOB PRIMARY KEY,"
                "version INTEGER NOT NULL,status TEXT NOT NULL,duration_ms INTEGER NOT NULL,"
                "audible_end_ms INTEGER NOT NULL,end_at_ms INTEGER NOT NULL,reference_db REAL,"
                "silence_db REAL,reason TEXT,analysed_at INTEGER NOT NULL)"))) {
            *attemptError = query.lastError().text();
            return false;
        }
        query.prepare(QStringLiteral(
            "INSERT OR REPLACE INTO audible_ends(mp3_complete_audio_sha256,version,status,duration_ms,"
            "audible_end_ms,end_at_ms,reference_db,silence_db,reason,analysed_at) "
            "VALUES(?,?,?,?,?,?,?,?,?,?)"));
        query.addBindValue(digest);
        query.addBindValue(music::kAudibleEndVersion);
        query.addBindValue(end.skips() ? kSkip : kPlayToEnd);
        query.addBindValue(qint64(end.durationMs));
        query.addBindValue(qint64(end.audibleEndMs));
        query.addBindValue(qint64(end.endAtMs));
        query.addBindValue(end.referenceDb);
        query.addBindValue(end.silenceDb);
        query.addBindValue(QString::fromStdString(end.reason));
        query.addBindValue(QDateTime::currentMSecsSinceEpoch());
        if (query.exec())
            return true;
        *attemptError = query.lastError().text();
        return false;
    }, stop, error);
}

QString describe(const music::AudibleEnd& end)
{
    const auto time = [](std::int64_t ms) { return QString::fromStdString(music::formatTime(ms)); };
    if (end.skips()) {
        return QStringLiteral("length %1, music ends at %2; Autoplay moves on at %3 (%4 s sooner)")
            .arg(time(end.durationMs), time(end.audibleEndMs), time(end.endAtMs))
            .arg(double(end.savedMs()) / 1000.0, 0, 'f', 1);
    }
    return QStringLiteral("length %1; plays to the end (%2)")
        .arg(time(end.durationMs), QString::fromStdString(end.reason));
}

} // namespace

// ---- Decoding ---------------------------------------------------------------

AudibleEndEngine::Outcome GstAudibleEndEngine::analyse(const QString& path,
                                                       const std::function<bool()>& stop,
                                                       music::AudibleEnd* result, QString* detail)
{
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.read(1).isEmpty()) {
            *detail = QStringLiteral("could not read the file");
            return Outcome::Unreadable;
        }
    }
    const QByteArray description = QStringLiteral(
        "uridecodebin name=source ! audioconvert ! audioresample "
        "! audio/x-raw,format=F32LE,layout=interleaved,channels=%1,rate=%2 "
        "! fakesink name=sink sync=false enable-last-sample=false signal-handoffs=true")
        .arg(music::kAudibleEndChannels).arg(music::kAudibleEndSampleRate).toUtf8();
    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(description.constData(), &error);
    if (!pipeline || error) {
        *detail = QStringLiteral("audio components unavailable: %1")
                      .arg(QString::fromUtf8(error ? error->message : "?"));
        g_clear_error(&error);
        if (pipeline)
            gst_object_unref(gst_object_ref_sink(pipeline));
        return Outcome::Unreadable;
    }
    gst_object_ref_sink(pipeline);

    Feed feed;
    feed.limit = std::int64_t(kMaxSeconds) * music::kAudibleEndSampleRate;
    GstElement* source = gst_bin_get_by_name(GST_BIN(pipeline), "source");
    GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    const QByteArray uri = QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()).toEncoded();
    g_object_set(source, "uri", uri.constData(), nullptr);
    g_signal_connect(sink, "handoff", G_CALLBACK(onHandoff), &feed);
    gst_object_unref(source);
    gst_object_unref(sink);

    Outcome outcome = Outcome::Interrupted;
    bool tooLong = false;
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
            tooLong = true;
            outcome = Outcome::Analysed;
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

    if (outcome == Outcome::Analysed) {
        *result = feed.detector.finish();
        if (tooLong) {
            result->verdict = music::AudibleEnd::Verdict::PlayToEnd;
            result->endAtMs = result->durationMs;
            result->reason = "a very long recording";
        }
    }
    return outcome;
}

std::shared_ptr<AudibleEndEngine> createAudibleEndEngine()
{
    if (!gst_is_initialized())
        return nullptr;
    return std::make_shared<GstAudibleEndEngine>();
}

// ---- The service ------------------------------------------------------------

AudibleEnds::AudibleEnds(QString cachePath, QObject* parent)
    : QObject(parent)
    , m_cachePath(std::move(cachePath))
    , m_factory(createAudibleEndEngine)
    , m_stop(std::make_shared<std::atomic_bool>(false))
{
}

AudibleEnds::~AudibleEnds()
{
    m_stop->store(true);
}

void AudibleEnds::request(const QString& mp3Path)
{
    if (mp3Path.isEmpty() || m_known.contains(mp3Path) || m_working == mp3Path)
        return;
    if (!m_working.isEmpty()) {
        m_waiting = mp3Path;
        return;
    }
    start(mp3Path);
}

std::optional<music::AudibleEnd> AudibleEnds::known(const QString& mp3Path) const
{
    const auto found = m_known.constFind(mp3Path);
    return found == m_known.cend() ? std::nullopt : std::optional<music::AudibleEnd>(found->end);
}

AudibleEnds::FileStamp AudibleEnds::stampOf(const QString& path)
{
    const QFileInfo info(path);
    return info.exists() ? FileStamp{info.size(), info.lastModified().toMSecsSinceEpoch()} : FileStamp{};
}

quint64 AudibleEnds::checkFile(const QString& mp3Path)
{
    const quint64 token = ++m_lastToken;
    // One look at a time: a drive that does not answer never gathers a
    // thread per song. A look still waiting is replaced (and never answered:
    // its song plays to the end).
    if (m_checking) {
        m_nextCheck = {mp3Path, token};
        return token;
    }
    startCheck(mp3Path, token);
    return token;
}

void AudibleEnds::startCheck(const QString& mp3Path, quint64 token)
{
    m_checking = true;
    const QPointer<AudibleEnds> self(this);
    background::run(QStringLiteral("AudibleEndCheck"), [self, mp3Path, token] {
        const FileStamp now = stampOf(mp3Path);
        if (QCoreApplication* app = QCoreApplication::instance()) {
            QMetaObject::invokeMethod(app, [self, mp3Path, token, now] {
                if (!self)
                    return;
                self->m_checking = false;
                const auto found = self->m_known.constFind(mp3Path);
                emit self->fileChecked(token, found != self->m_known.cend() && found->file.size >= 0
                                                  && found->file == now);
                if (!self->m_nextCheck.first.isEmpty()) {
                    const auto [path, next] = std::exchange(self->m_nextCheck, {});
                    self->startCheck(path, next);
                }
            }, Qt::QueuedConnection);
        }
    });
}

void AudibleEnds::start(const QString& mp3Path)
{
    std::shared_ptr<AudibleEndEngine> engine = m_factory ? m_factory() : nullptr;
    if (!engine) {
        qCWarning(lcAutoplayEnd) << "Smart Autoplay End: no audio decoder; songs play to their end";
        return;
    }
    m_working = mp3Path;
    const QPointer<AudibleEnds> self(this);
    const QString cachePath = m_cachePath;
    const std::shared_ptr<std::atomic_bool> stopFlag = m_stop;
    background::run(QStringLiteral("AudibleEnd"), [self, mp3Path, cachePath, stopFlag, engine] {
        if (QThread* thread = QThread::currentThread())
            thread->setPriority(QThread::LowPriority);
        const auto stop = [stopFlag] { return stopFlag->load(); };
        // When the job (with keeping its result) is over, the next may start.
        struct Done {
            QPointer<AudibleEnds> self;
            ~Done()
            {
                if (QCoreApplication* app = QCoreApplication::instance()) {
                    QMetaObject::invokeMethod(app, [self = self] {
                        if (self)
                            self->jobDone();
                    }, Qt::QueuedConnection);
                }
            }
        } done{self};
        // Taken before the content is read: a file changed meanwhile no
        // longer matches it, and is never cut short.
        const FileStamp file = stampOf(mp3Path);
        const auto report = [self, file](const QString& path, const std::optional<music::AudibleEnd>& result) {
            if (QCoreApplication* app = QCoreApplication::instance()) {
                QMetaObject::invokeMethod(app, [self, path, result, file] {
                    if (self)
                        self->available(path, result, file);
                }, Qt::QueuedConnection);
            }
        };

        QByteArray digest;
        if (!mp3CompleteAudioDigest(mp3Path, &digest)) {
            qCWarning(lcAutoplayEnd).noquote() << "Smart Autoplay End: could not read" << mp3Path;
            report(mp3Path, std::nullopt);
            return;
        }
        CacheConnection cache(cachePath);
        if (cache.isOpen()) {
            if (const auto cached = readCached(cache.database(), digest)) {
                report(mp3Path, cached);
                return;
            }
        }

        music::AudibleEnd end;
        QString detail;
        QElapsedTimer timer;
        timer.start();
        const AudibleEndEngine::Outcome outcome = engine->analyse(mp3Path, stop, &end, &detail);
        switch (outcome) {
        case AudibleEndEngine::Outcome::Analysed:
            qCInfo(lcAutoplayEnd).noquote() << "Smart Autoplay End: analysed" << QFileInfo(mp3Path).fileName()
                                            << "in" << timer.elapsed() << "ms";
            break;
        case AudibleEndEngine::Outcome::NotAudio:
            // Damaged audio is never cut short; remembered, so it is not read again.
            end = music::AudibleEnd{};
            end.reason = "the audio could not be decoded completely";
            qCWarning(lcAutoplayEnd).noquote() << "Smart Autoplay End:" << QFileInfo(mp3Path).fileName()
                                               << "plays to the end, damaged audio:" << detail;
            break;
        case AudibleEndEngine::Outcome::Unreadable:
        case AudibleEndEngine::Outcome::Interrupted:
            if (outcome == AudibleEndEngine::Outcome::Unreadable) {
                qCWarning(lcAutoplayEnd).noquote() << "Smart Autoplay End: could not read"
                                                   << QFileInfo(mp3Path).fileName() << ":" << detail;
            }
            report(mp3Path, std::nullopt);
            return;
        }
        report(mp3Path, end);
        if (cache.isOpen()) {
            QSqlDatabase database = cache.database();
            QString error;
            if (!storeResult(database, digest, end, stop, &error) && !stop())
                qCWarning(lcAutoplayEnd).noquote() << "Smart Autoplay End: result not kept:" << error;
        }
    });
}

void AudibleEnds::available(const QString& mp3Path, const std::optional<music::AudibleEnd>& result,
                            FileStamp file)
{
    if (result) {
        m_known.insert(mp3Path, Known{*result, file});
        qCInfo(lcAutoplayEnd).noquote() << "Smart Autoplay End:" << QFileInfo(mp3Path).fileName() << ":"
                                        << describe(*result);
    }
    emit ready(mp3Path);
}

void AudibleEnds::jobDone()
{
    m_working.clear();
    const QString next = std::exchange(m_waiting, QString());
    if (!next.isEmpty())
        request(next);
}
