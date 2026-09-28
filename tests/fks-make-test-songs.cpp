// Writes a small folder of synthetic karaoke songs (MP3 + CDG) for trying a
// new installation without real karaoke media:
//
//   fks-make-test-songs <folder>
//
// "Clicks In Time" clicks once a second and a new block appears on the lyrics
// screen with every click, so lyrics running ahead of or behind the sound is
// easy to see. "Steady Tone" is one long note, so a Key change is easy to
// hear. The short songs are for playlists and Autoplay; one lives in a folder
// with accented letters. Nothing here comes from real karaoke media.

#include "CdgTestData.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

#include <gst/gst.h>

#include <cstdio>

namespace {

struct TestSong {
    QString folder;       // inside the output folder; may be empty
    QString name;         // file name without extension
    QString artist;
    QString title;
    int seconds;
    bool steadyTone;      // one long note instead of clicks
    int clickHz;
};

bool writeMp3(const QString& path, const TestSong& song)
{
    const int buffers = song.seconds * 100;  // 441 samples at 44.1 kHz = 10 ms
    const QString source = song.steadyTone
        ? QStringLiteral("audiotestsrc wave=sine freq=440 volume=0.3")
        : QStringLiteral("audiotestsrc wave=ticks tick-interval=1000000000 freq=%1 volume=0.6")
              .arg(song.clickHz);
    // Real karaoke MP3s carry ID3 tags, so the test songs do too.
    GstElementFactory* id3 = gst_element_factory_find("id3v2mux");
    if (!id3) {
        std::fprintf(stderr, "The GStreamer element id3v2mux is missing\n");
        return false;
    }
    gst_object_unref(id3);
    const QByteArray description = QStringLiteral(
        "%1 num-buffers=%2 samplesperbuffer=441 ! audio/x-raw,rate=44100,channels=2 "
        "! audioconvert ! lamemp3enc target=bitrate bitrate=128 cbr=true ! id3v2mux name=tags "
        "! filesink name=out")
        .arg(source).arg(buffers)
        .toUtf8();
    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(description.constData(), &error);
    if (!pipeline) {
        std::fprintf(stderr, "Pipeline failed: %s\n", error ? error->message : "?");
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
    if (GstElement* tagger = gst_bin_get_by_name(GST_BIN(pipeline), "tags")) {
        gst_tag_setter_add_tags(GST_TAG_SETTER(tagger), GST_TAG_MERGE_REPLACE,
                                GST_TAG_TITLE, song.title.toUtf8().constData(),
                                GST_TAG_ARTIST, song.artist.toUtf8().constData(), nullptr);
        gst_object_unref(tagger);
    }
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstBus* bus = gst_element_get_bus(pipeline);
    GstMessage* message = gst_bus_timed_pop_filtered(
        bus, 120 * GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    const bool ok = message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
    if (message)
        gst_message_unref(message);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return ok;
}

// A new 2x2-tile block for every second, left to right and row by row, in a
// new colour; the screen is cleared every 16 seconds.
std::vector<std::uint8_t> clockCdg(int seconds)
{
    using namespace testcdg;
    std::map<std::size_t, Packet> packets;
    packets[0] = loadColors(false, {0x000, 0xFFF, 0xF44, 0x4F4, 0x48F, 0xFF4, 0x4FF, 0xF4F});
    packets[1] = memoryPreset(0);
    packets[2] = borderPreset(0);
    for (int second = 0; second < seconds; ++second) {
        std::size_t at = static_cast<std::size_t>(second) * 300;
        const int slot = second % 16;
        if (slot == 0 && second > 0)
            packets[at++] = memoryPreset(0);
        const auto colour = static_cast<std::uint8_t>(1 + second % 7);
        const auto row = static_cast<std::uint8_t>(3 + (slot / 8) * 6);
        const auto column = static_cast<std::uint8_t>(4 + (slot % 8) * 5);
        for (std::uint8_t dy = 0; dy < 2; ++dy) {
            for (std::uint8_t dx = 0; dx < 2; ++dx)
                packets[at++] = solidTile(colour, static_cast<std::uint8_t>(row + dy),
                                          static_cast<std::uint8_t>(column + dx));
        }
    }
    return stream(static_cast<std::size_t>(seconds) * 300, packets);
}

bool writeCdg(const QString& path, const std::vector<std::uint8_t>& data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly)
        && file.write(reinterpret_cast<const char*>(data.data()), static_cast<qint64>(data.size()))
               == static_cast<qint64>(data.size());
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    gst_init(&argc, &argv);
    const QStringList args = QCoreApplication::arguments();
    if (args.size() != 2) {
        std::fprintf(stderr, "Usage: fks-make-test-songs <folder>\n");
        return 2;
    }
    const QString root = args.at(1);
    const QList<TestSong> songs{
        {{}, QStringLiteral("FKT01-01 - Test Singer - Clicks In Time"), QStringLiteral("Test Singer"),
         QStringLiteral("Clicks In Time"), 60, false, 1000},
        {{}, QStringLiteral("FKT01-02 - Test Singer - Steady Tone"), QStringLiteral("Test Singer"),
         QStringLiteral("Steady Tone"), 45, true, 0},
        {{}, QStringLiteral("FKT01-03 - Another Tester - Short Song One"), QStringLiteral("Another Tester"),
         QStringLiteral("Short Song One"), 12, false, 800},
        {{}, QStringLiteral("FKT01-04 - Another Tester - Short Song Two"), QStringLiteral("Another Tester"),
         QStringLiteral("Short Song Two"), 12, false, 1200},
        {{}, QStringLiteral("FKT01-05 - Zebra Band - Short Song Three"), QStringLiteral("Zebra Band"),
         QStringLiteral("Short Song Three"), 12, false, 1500},
        {QStringLiteral("Ünïcödé Földer ♪"), QStringLiteral("FKT02-01 - Björk Tëster - Straße Song"),
         QStringLiteral("Björk Tëster"), QStringLiteral("Straße Song"), 20, false, 600},
    };
    for (const TestSong& song : songs) {
        const QDir folder(song.folder.isEmpty() ? root : QDir(root).filePath(song.folder));
        if (!QDir().mkpath(folder.path())) {
            std::fprintf(stderr, "Could not make %s\n", qPrintable(folder.path()));
            return 1;
        }
        const QString base = folder.filePath(song.name);
        if (!writeMp3(base + QStringLiteral(".mp3"), song)
            || !writeCdg(base + QStringLiteral(".cdg"), clockCdg(song.seconds))) {
            std::fprintf(stderr, "Could not write %s\n", qPrintable(base));
            return 1;
        }
        std::printf("Wrote %s (%d s)\n", qPrintable(QFileInfo(base).fileName()), song.seconds);
    }
    return 0;
}
