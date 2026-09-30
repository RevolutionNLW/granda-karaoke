// Works out the keys of some song files and reports them with timings, for
// checking the key detector on real songs and measuring how long a library
// would take. Only reads the songs; nothing is written anywhere near them
// (GStreamer keeps its plugin registry in the tool's own app-data folder).
//
//   fks-song-keys [--limit N] [--expect keys.tsv] <file-or-folder>...
//
// Folders are searched for .mp3 files; with --limit, N of them are picked
// evenly spread through the sorted list. keys.tsv holds lines of
// "<part of a file name><TAB><key>" (e.g. "SF123-04<TAB>F#m") for songs whose
// key is known independently; the report then compares.

#include "KaraokePlayer.h"
#include "SongKeyAnalyser.h"
#include "music/MusicalKey.h"

#include <QCoreApplication>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

#include <algorithm>

namespace {

int usage(const QString& message = {})
{
    QTextStream err(stderr);
    if (!message.isEmpty())
        err << message << '\n';
    err << "Usage: fks-song-keys [--limit N] [--expect keys.tsv] <file-or-folder>...\n";
    return 2;
}

QString name(const std::optional<music::MusicalKey>& key)
{
    return key ? QString::fromStdString(key->name()) : QStringLiteral("-");
}

// How a detected key relates to the expected one.
QString relation(const music::MusicalKey& found, const music::MusicalKey& expected)
{
    if (found == expected)
        return QStringLiteral("exact");
    const int shift = ((found.tonic - expected.tonic) % 12 + 12) % 12;
    if (found.minor != expected.minor) {
        if ((found.minor && shift == 9) || (!found.minor && shift == 3))
            return QStringLiteral("relative");
        if (shift == 0)
            return QStringLiteral("parallel");
    } else if (shift == 7 || shift == 5) {
        return QStringLiteral("fifth");
    }
    return QStringLiteral("wrong");
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Granda"));
    QCoreApplication::setApplicationName(QStringLiteral("FrankiesKaraokeStudioTools"));
    QStringList args = app.arguments().mid(1);
    int limit = 0;
    QString expectPath;
    QStringList inputs;
    for (int i = 0; i < args.size(); ++i) {
        if (args.at(i) == QLatin1String("--limit") && i + 1 < args.size())
            limit = args.at(++i).toInt();
        else if (args.at(i) == QLatin1String("--expect") && i + 1 < args.size())
            expectPath = args.at(++i);
        else if (args.at(i).startsWith(QLatin1String("--")))
            return usage(QStringLiteral("Unknown option %1").arg(args.at(i)));
        else
            inputs.append(args.at(i));
    }
    if (inputs.isEmpty())
        return usage();

    QList<QPair<QString, music::MusicalKey>> expected;
    if (!expectPath.isEmpty()) {
        QFile file(expectPath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            return usage(QStringLiteral("Cannot read %1").arg(expectPath));
        while (!file.atEnd()) {
            const QStringList parts = QString::fromUtf8(file.readLine()).trimmed().split(QLatin1Char('\t'));
            if (parts.size() != 2 || parts.at(0).startsWith(QLatin1Char('#')))
                continue;
            if (const auto key = music::MusicalKey::parse(parts.at(1).toStdString()))
                expected.append({parts.at(0), *key});
        }
    }

    QStringList files;
    for (const QString& input : std::as_const(inputs)) {
        if (QFileInfo(input).isDir()) {
            QStringList found;
            QDirIterator it(input, {QStringLiteral("*.mp3"), QStringLiteral("*.MP3")}, QDir::Files,
                            QDirIterator::Subdirectories);
            while (it.hasNext())
                found.append(it.next());
            std::sort(found.begin(), found.end());
            files += found;
        } else {
            files.append(input);
        }
    }
    if (limit > 0 && files.size() > limit) {
        QStringList picked;
        for (int i = 0; i < limit; ++i)
            picked.append(files.at(int(qint64(i) * files.size() / limit)));
        files = picked;
    }

    QString error;
    if (!KaraokePlayer::initializeGStreamer(&error)) {
        QTextStream(stderr) << error << '\n';
        return 1;
    }
    GstSongKeyEngine engine;
    QTextStream out(stdout);
    out << "ms\tstatus\tkey\tconfidence\tcorrelation\trunner_up\tmargin\tagreement\ttuning_cents\tseconds\texpected\trelation\tfile\tparallel_margin\ttuning_consistency\tchroma_C_to_B\n";
    qint64 totalMs = 0;
    double totalSeconds = 0.0;
    int analysed = 0;
    int confident = 0;
    QMap<QString, int> relations;
    QMap<QString, int> confidentRelations;
    for (const QString& path : std::as_const(files)) {
        music::KeyAnalysis result;
        QString detail;
        QElapsedTimer timer;
        timer.start();
        const auto outcome = engine.analyse(path, {}, &result, &detail);
        const qint64 ms = timer.elapsed();
        if (outcome != SongKeyEngine::Outcome::Analysed) {
            out << ms << '\t' << (outcome == SongKeyEngine::Outcome::NotAudio ? "not_audio" : "unreadable")
                << "\t-\t\t\t\t\t\t\t\t\t\t" << path << '\t' << detail << '\n';
            continue;
        }
        ++analysed;
        totalMs += ms;
        totalSeconds += result.seconds;
        const bool isConfident = result.status == music::KeyAnalysis::Status::Confident;
        confident += isConfident ? 1 : 0;
        QString expectedName;
        QString related;
        for (const auto& [part, key] : std::as_const(expected)) {
            if (QFileInfo(path).fileName().contains(part, Qt::CaseInsensitive)) {
                expectedName = QString::fromStdString(key.name());
                related = result.key ? relation(*result.key, key) : QStringLiteral("none");
                ++relations[related];
                if (isConfident)
                    ++confidentRelations[related];
                break;
            }
        }
        out << ms << '\t' << music::statusName(result.status) << '\t' << name(result.key) << '\t'
            << QString::number(result.confidence, 'f', 2) << '\t' << QString::number(result.correlation, 'f', 3)
            << '\t' << name(result.runnerUp) << '\t' << QString::number(result.margin, 'f', 2) << '\t'
            << QString::number(result.agreement, 'f', 2) << '\t' << QString::number(result.tuningCents, 'f', 1)
            << '\t' << QString::number(result.seconds, 'f', 0) << '\t' << expectedName << '\t' << related
            << '\t' << path << '\t' << QString::number(result.parallelMargin, 'f', 2) << '\t'
            << QString::number(result.tuningConsistency, 'f', 2) << '\t';
        for (std::size_t i = 0; i < result.chroma.size(); ++i)
            out << (i ? "," : "") << QString::number(result.chroma[i], 'f', 4);
        out << '\n';
        out.flush();
    }

    out << "\n# " << analysed << " of " << files.size() << " files analysed; " << confident
        << " with a key shown (" << (analysed ? 100 * confident / analysed : 0) << "%)\n";
    if (analysed > 0) {
        const double perSong = double(totalMs) / analysed;
        out << "# " << QString::number(perSong, 'f', 0) << " ms per song on average ("
            << QString::number(totalSeconds / analysed, 'f', 0) << " s of audio each; "
            << QString::number(totalSeconds * 1000.0 / double(std::max<qint64>(1, totalMs)), 'f', 0)
            << "x real time)\n";
        for (const int songs : {1000, 10000, 50000}) {
            out << "# " << songs << " songs: about " << QString::number(perSong * songs / 3600000.0, 'f', 1)
                << " hours of analysis (reading from the drive this was measured on, without pauses)\n";
        }
    }
    if (!relations.isEmpty()) {
        const auto line = [&out](const char* label, const QMap<QString, int>& counts) {
            int all = 0;
            for (const int count : counts)
                all += count;
            out << "# " << label << ": " << all << " songs";
            for (auto it = counts.cbegin(); it != counts.cend(); ++it)
                out << ", " << it.key() << " " << it.value();
            out << '\n';
        };
        line("all with an expected key", relations);
        line("shown (confident) with an expected key", confidentRelations);
    }
    return 0;
}
