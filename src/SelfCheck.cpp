#include "SelfCheck.h"

#include "AppStorage.h"
#include "AudioOutputs.h"
#include "KaraokePlayer.h"
#include "SongPair.h"
#include "library/Catalogue.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QSaveFile>
#include <QGuiApplication>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSysInfo>
#include <QTemporaryFile>
#include <QTimer>

#include <gst/gst.h>

#include <cstdio>
#include <functional>

namespace selfcheck {

namespace {

class Report {
public:
    void line(const QString& text) { m_text += text + QLatin1Char('\n'); }
    void check(bool ok, const QString& what, const QString& detail = {})
    {
        line(QStringLiteral("[%1] %2%3").arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"), what,
                                             detail.isEmpty() ? QString() : QStringLiteral(": ") + detail));
        if (!ok)
            ++m_failures;
    }
    void note(const QString& what, const QString& detail)
    {
        line(QStringLiteral("[INFO] %1: %2").arg(what, detail));
    }
    int failures() const { return m_failures; }
    const QString& text() const { return m_text; }

private:
    QString m_text;
    int m_failures = 0;
};

// Lets the player's timers and GStreamer messages run until `done` or the
// time runs out.
bool waitFor(const std::function<bool()>& done, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!done()) {
        if (timer.elapsed() > timeoutMs)
            return false;
        QEventLoop loop;
        QTimer::singleShot(20, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return true;
}

void checkSqlite(Report& report)
{
    const bool available = QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"));
    QString version;
    if (available) {
        const QString name = QStringLiteral("fks-self-check");
        {
            QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
            database.setDatabaseName(QStringLiteral(":memory:"));
            if (database.open()) {
                QSqlQuery query(database);
                if (query.exec(QStringLiteral("SELECT sqlite_version()")) && query.next())
                    version = query.value(0).toString();
                database.close();
            }
        }
        QSqlDatabase::removeDatabase(name);
    }
    report.check(available && !version.isEmpty(), QStringLiteral("Qt SQLite driver"),
                 version.isEmpty() ? QStringLiteral("not available") : QStringLiteral("SQLite ") + version);
}

void checkDataFolder(Report& report)
{
    const QString folder = appstorage::folder();
    bool writable = false;
    if (!folder.isEmpty() && QDir().mkpath(folder)) {
        QTemporaryFile probe(QDir(folder).filePath(QStringLiteral("self-check-XXXXXX")));
        writable = probe.open() && probe.write("ok", 2) == 2;
    }
    report.check(writable, QStringLiteral("Program data folder is writable"), folder);
    const QString programFolder = QDir::cleanPath(QCoreApplication::applicationDirPath());
    const bool separate = !QDir::cleanPath(folder).startsWith(programFolder + QLatin1Char('/'), Qt::CaseInsensitive)
        && QDir::cleanPath(folder).compare(programFolder, Qt::CaseInsensitive) != 0;
    report.check(separate, QStringLiteral("Program data folder is not inside the program folder"));
}

void checkAudioComponents(Report& report, bool packaged)
{
    QString error;
    const bool started = KaraokePlayer::initializeGStreamer(&error);
    gchar* version = gst_version_string();
    report.check(started, QStringLiteral("Audio system starts with every karaoke component"),
                 started ? QString::fromUtf8(version) : error);
    g_free(version);
    if (!started)
        return;
    const QString programFolder = QDir::cleanPath(QCoreApplication::applicationDirPath());
    for (const char* name : KaraokePlayer::requiredElements()) {
        GstElementFactory* factory = gst_element_factory_find(name);
        QString file;
        if (factory) {
            if (GstPlugin* plugin = gst_plugin_feature_get_plugin(GST_PLUGIN_FEATURE(factory))) {
                if (const gchar* filename = gst_plugin_get_filename(plugin))
                    file = QDir::cleanPath(QDir::fromNativeSeparators(QString::fromUtf8(filename)));
                gst_object_unref(plugin);
            }
            gst_object_unref(factory);
        }
        const bool inPackage = file.isEmpty()
            || file.startsWith(programFolder + QLatin1Char('/'), Qt::CaseInsensitive);
        report.check(factory && (!packaged || inPackage),
                     QStringLiteral("Audio component %1").arg(QLatin1String(name)),
                     file.isEmpty() ? QStringLiteral("(built in)") : file);
    }
#ifdef Q_OS_WIN
    // The speakers themselves: autoaudiosink alone could be present while
    // the Windows output it picks cannot load. Made, not started, so no
    // speaker is needed for this.
    GstElement* windowsSink = gst_element_factory_make("wasapi2sink", nullptr);
    report.check(windowsSink != nullptr, QStringLiteral("Windows sound output component loads"),
                 QStringLiteral("wasapi2sink"));
    if (windowsSink)
        gst_object_unref(gst_object_ref_sink(windowsSink));
#endif
}

void checkPlayback(Report& report, const QString& songPath)
{
    const SongPairResult resolved = resolveSongPair(songPath);
    report.check(resolved.pair.isValid(), QStringLiteral("Test song found"),
                 resolved.pair.isValid() ? resolved.pair.displayName() : resolved.error);
    if (!resolved.pair.isValid())
        return;
    // Silent: the audio goes nowhere, at the speed a speaker would take it.
    KaraokePlayer player(nullptr, QStringLiteral("fakesink"));
    QString failure;
    QObject::connect(&player, &KaraokePlayer::errorOccurred,
                     [&failure](const QString& message) { failure = message; });
    report.check(player.load(resolved.pair), QStringLiteral("Test song loads (MP3 + CDG)"), failure);
    player.play();
    QElapsedTimer wall;
    wall.start();
    const bool started = waitFor([&player] { return player.positionMs() >= 1500; }, 10000);
    report.check(started && failure.isEmpty(), QStringLiteral("MP3 plays and its position advances"),
                 QStringLiteral("%1 ms of song in %2 ms").arg(player.positionMs()).arg(wall.elapsed()));
    if (!started)
        return;
    // Key and Tempo change while it plays, as the Key/Tempo buttons do.
    player.setKeySemitones(2);
    player.setTempoPercent(90);
    const qint64 before = player.positionMs();
    const bool continued = waitFor([&player, before] { return player.positionMs() >= before + 1000; }, 10000);
    report.check(continued && failure.isEmpty() && player.keySemitones() == 2 && player.tempoPercent() == 90,
                 QStringLiteral("Key +2 and Tempo 90% applied while playing"),
                 failure.isEmpty() ? QStringLiteral("position %1 ms").arg(player.positionMs()) : failure);
    report.check(!player.currentFrame().isNull(), QStringLiteral("Lyrics picture is drawn"));
    player.stop();
}

// A short, quiet beep through the computer's default sound output, as a
// song would play: proves the speakers (or headphones) can be opened.
void checkDefaultOutput(Report& report)
{
    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(
        "audiotestsrc num-buffers=25 freq=880 volume=0.15 ! audioconvert ! audioresample "
        "! autoaudiosink", &error);
    if (!pipeline) {
        report.check(false, QStringLiteral("A beep plays through the default sound output"),
                     error ? QString::fromUtf8(error->message) : QString());
        g_clear_error(&error);
        return;
    }
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstBus* bus = gst_element_get_bus(pipeline);
    GstMessage* message = gst_bus_timed_pop_filtered(
        bus, 10 * GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    QString detail;
    bool ok = message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
    if (message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        GError* failure = nullptr;
        gst_message_parse_error(message, &failure, nullptr);
        detail = failure ? QString::fromUtf8(failure->message) : QString();
        g_clear_error(&failure);
    } else if (!message) {
        detail = QStringLiteral("no answer within 10 seconds");
    }
    if (message)
        gst_message_unref(message);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    report.check(ok, QStringLiteral("A beep plays through the default sound output"),
                 ok ? QStringLiteral("you should have heard a short beep") : detail);
}

void noteSoundOutputs(Report& report)
{
    const QList<audio::Output> outputs = audio::outputs();
    report.note(QStringLiteral("Sound outputs found"), QString::number(outputs.size()));
    for (const audio::Output& output : outputs)
        report.note(QStringLiteral("  Output"), QStringLiteral("%1 [%2]").arg(output.label, output.id));
}

void noteFonts(Report& report)
{
    const QFont font = QGuiApplication::font();
    report.note(QStringLiteral("Interface font"),
                font.pointSizeF() > 0 ? QStringLiteral("%1 %2pt").arg(font.family()).arg(font.pointSizeF())
                                      : QStringLiteral("%1 %2px").arg(font.family()).arg(font.pixelSize()));
    // Symbols the interface shows as text. Missing ones fall back to another
    // system font, which the report cannot see, so this is information only.
    const QFontMetrics metrics(font);
    QString missing;
    for (const char32_t symbol : {U'→', U'↑', U'↓', U'▶', U'—', U'…', U'“', U'”', U'−', U'·'}) {
        if (!metrics.inFontUcs4(symbol))
            missing += QString::fromUcs4(&symbol, 1);
    }
    report.note(QStringLiteral("Symbols not in the interface font itself"),
                missing.isEmpty() ? QStringLiteral("none") : missing);
}

} // namespace

int run(const QString& reportPath, const QString& songPath, const QStringList& musicFolders)
{
    if (!reportPath.isEmpty()) {
        const SongPairResult song = songPath.isEmpty() ? SongPairResult{} : resolveSongPair(songPath);
        bool refused = false;
        for (const QString& folder : musicFolders)
            refused = refused || Catalogue::mayBeInsideOrEqual(reportPath, folder);
        for (const QString& file : {songPath, song.pair.mp3Path, song.pair.cdgPath})
            refused = refused || (!file.isEmpty() && Catalogue::mayBeInsideOrEqual(reportPath, file));
        if (refused) {
            std::fputs("The report would be written inside a music folder or over the test song; "
                       "nothing was written.\n", stderr);
            return 2;
        }
    }

    Report report;
    report.line(QStringLiteral("Frankie's Karaoke Studio installation check"));
    report.note(QStringLiteral("Version"), QCoreApplication::applicationVersion());
    report.note(QStringLiteral("System"), QStringLiteral("%1 (%2, kernel %3)")
                    .arg(QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture(),
                         QSysInfo::kernelVersion()));
    report.note(QStringLiteral("Qt"), QString::fromLatin1(qVersion()));
    report.note(QStringLiteral("Display platform"), QGuiApplication::platformName());
    report.note(QStringLiteral("Program folder"), QCoreApplication::applicationDirPath());
    // A packaged copy carries its own audio components beside the program.
    const bool packaged = QFileInfo(QCoreApplication::applicationDirPath()
                                    + QStringLiteral("/lib/gstreamer-1.0")).isDir();
    report.note(QStringLiteral("Packaged copy"), packaged ? QStringLiteral("yes") : QStringLiteral("no"));

    checkSqlite(report);
    checkDataFolder(report);
    checkAudioComponents(report, packaged);
    if (!songPath.isEmpty() && gst_is_initialized())
        checkPlayback(report, songPath);
    if (gst_is_initialized()) {
        noteSoundOutputs(report);
        checkDefaultOutput(report);
    }
    noteFonts(report);
    report.line(report.failures() == 0 ? QStringLiteral("RESULT: PASS")
                                       : QStringLiteral("RESULT: FAIL (%1 problems)").arg(report.failures()));

    const QByteArray text = report.text().toUtf8();
    std::fputs(text.constData(), stdout);
    std::fflush(stdout);
    if (!reportPath.isEmpty()) {
        QSaveFile file(reportPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text) || file.write(text) != text.size()
            || !file.commit())
            return 2;
    }
    return report.failures() == 0 ? 0 : 1;
}

} // namespace selfcheck
