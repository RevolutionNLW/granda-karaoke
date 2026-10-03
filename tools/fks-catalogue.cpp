#include "library/Catalogue.h"
#include "library/CatalogueTools.h"
#include "library/KnownLibraryRoots.h"
#include "library/LibraryScanner.h"
#include "library/MetadataResolver.h"
#include "library/MetadataOverrideStore.h"
#include "library/TitleScreenText.h"
#include "ocr/PlatformTitleScreenOcr.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QTextStream>

namespace {

void printJson(const QVariant& value)
{
    QTextStream(stdout) << QJsonDocument::fromVariant(value).toJson(QJsonDocument::Indented);
}

int usage(const QString& message = {})
{
    QTextStream err(stderr);
    if (!message.isEmpty())
        err << message << '\n';
    err << "Usage:\n"
           "  fks-catalogue --db <path> scan <root> [--no-tags] [--limit-seconds N]\n"
           "  fks-catalogue --db <path> stats\n"
           "  fks-catalogue --db <path> resolve [--overrides <path>]\n"
           "  fks-catalogue --db <path> reprocess [--overrides <path>] [--no-content-matching] [--title-screens]\n"
           "  fks-catalogue --db <path> title-screens-export <file.json>\n"
           "  fks-catalogue --db <path> title-screens-import <file.json>\n"
           "  fks-catalogue --db <path> metadata-stats\n"
           "  fks-catalogue --db <path> compare --baseline <other.sqlite>\n"
           "  fks-catalogue --db <path> reparse\n"
           "  fks-catalogue --db <path> evaluate --gold <gold.tsv>\n"
           "  fks-catalogue --db <path> evaluate-tags [--limit N]\n"
           "  fks-catalogue --db <path> search <text>\n"
           "  fks-catalogue --db <path> sample <n> [--confidence X]\n"
           "  fks-catalogue --db <path> explain <relative-path>\n";
    return 2;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QStringList args = app.arguments().mid(1);
    const qsizetype dbOption = args.indexOf(QStringLiteral("--db"));
    if (dbOption < 0 || dbOption + 1 >= args.size())
        return usage(QStringLiteral("--db is required"));
    const QString databasePath = args.at(dbOption + 1);
    args.removeAt(dbOption + 1);
    args.removeAt(dbOption);
    QString overridesPath;
    const qsizetype overridesOption = args.indexOf(QStringLiteral("--overrides"));
    if (overridesOption >= 0) {
        if (overridesOption + 1 >= args.size())
            return usage(QStringLiteral("--overrides requires a path"));
        overridesPath = args.at(overridesOption + 1);
        args.removeAt(overridesOption + 1);
        args.removeAt(overridesOption);
    }
    if (args.isEmpty())
        return usage();
    const QString command = args.takeFirst();
    // Music folders the application has been given. No database under one of
    // them is opened, whatever the database itself records.
    const QStringList knownRoots = KnownLibraryRoots::load();

    if (command == QLatin1String("scan")) {
        if (args.isEmpty())
            return usage(QStringLiteral("scan requires a library root"));
        const QString root = args.takeFirst();
        ScanOptions options;
        const qsizetype noTags = args.indexOf(QStringLiteral("--no-tags"));
        if (noTags >= 0) {
            options.readTags = false;
            args.removeAt(noTags);
        }
        const qsizetype limit = args.indexOf(QStringLiteral("--limit-seconds"));
        if (limit >= 0) {
            if (limit + 1 >= args.size())
                return usage(QStringLiteral("--limit-seconds requires an integer"));
            bool valid = false;
            options.limitSeconds = args.at(limit + 1).toInt(&valid);
            if (!valid || options.limitSeconds <= 0)
                return usage(QStringLiteral("--limit-seconds must be positive"));
            args.removeAt(limit + 1);
            args.removeAt(limit);
        }
        if (!args.isEmpty())
            return usage(QStringLiteral("Unexpected scan arguments"));
        LibraryScanner scanner(databasePath, {}, overridesPath);
        scanner.setKnownRoots(knownRoots);
        scanner.setOptions(options);
        int result = 0;
        QElapsedTimer progressTimer;
        QObject::connect(&scanner, &LibraryScanner::progress,
                         [&progressTimer](const QString& phase, qint64 done, qint64 total, const QString& path) {
            if (progressTimer.isValid() && progressTimer.elapsed() < 1000)
                return;
            progressTimer.restart();
            QTextStream(stderr) << phase << ' ' << done << '/' << total
                                << (path.isEmpty() ? QString() : QStringLiteral(" ") + path) << '\n';
        });
        QObject::connect(&scanner, &LibraryScanner::finished,
                         [&](const QVariantMap& summary) { printJson(summary); });
        QObject::connect(&scanner, &LibraryScanner::failed, [&](const QString& message) {
            QTextStream(stderr) << "Scan failed: " << message << '\n';
            result = 1;
        });
        scanner.scan(root);
        return result;
    }

    if (command == QLatin1String("reprocess")) {
        // The same background job as the app's Reprocess Metadata action: a
        // database-only resolve, content matching for connected roots (reads
        // candidate files read-only), then a final resolve.
        ScanOptions options;
        const qsizetype noContent = args.indexOf(QStringLiteral("--no-content-matching"));
        if (noContent >= 0) {
            options.identifyDuplicates = false;
            args.removeAt(noContent);
        }
        std::shared_ptr<TitleScreenOcrEngine> engine;
        const qsizetype titleScreens = args.indexOf(QStringLiteral("--title-screens"));
        if (titleScreens >= 0) {
            args.removeAt(titleScreens);
            engine = createPlatformTitleScreenOcr();
            if (!engine) {
                QTextStream(stderr) << "No local OCR engine on this platform; only imported "
                                       "title-screen results will be matched\n";
            }
            options.readTitleScreens = true;
        }
        if (!args.isEmpty())
            return usage(QStringLiteral("Unexpected reprocess arguments"));
        LibraryScanner scanner(databasePath, {}, overridesPath);
        scanner.setKnownRoots(knownRoots);
        scanner.setOptions(options);
        scanner.setTitleScreenOcr(engine);
        int result = 0;
        QElapsedTimer progressTimer;
        QObject::connect(&scanner, &LibraryScanner::progress,
                         [&progressTimer](const QString& phase, qint64 done, qint64 total, const QString& path) {
            if (progressTimer.isValid() && progressTimer.elapsed() < 2000)
                return;
            progressTimer.restart();
            QTextStream(stderr) << phase << ' ' << done << '/' << total
                                << (path.isEmpty() ? QString() : QStringLiteral(" ") + path) << '\n';
        });
        QObject::connect(&scanner, &LibraryScanner::finished,
                         [&](const QVariantMap& summary) { printJson(summary); });
        QObject::connect(&scanner, &LibraryScanner::failed, [&](const QString& message) {
            QTextStream(stderr) << "Reprocess failed: " << message << '\n';
            result = 1;
        });
        scanner.prepareScan();
        scanner.reprocessMetadata();
        return result;
    }

    Catalogue catalogue(databasePath);
    QString error;
    if (!catalogue.open(&error, knownRoots)) {
        QTextStream(stderr) << error << '\n';
        return 1;
    }
    if (command == QLatin1String("stats")) {
        printJson(catalogue.stats(&error));
    } else if (command == QLatin1String("resolve")) {
        if (!args.isEmpty())
            return usage(QStringLiteral("Unexpected resolve arguments"));
        QElapsedTimer timer;
        timer.start();
        if (!MetadataResolver::resolve(catalogue, -1, &error)) {
            QTextStream(stderr) << error << '\n';
            return 1;
        }
        if (!overridesPath.isEmpty()) {
            QStringList roots = knownRoots;
            for (const CatalogueRoot& root : catalogue.roots(&error))
                roots.append(root.path);
            MetadataOverrideStore store(overridesPath);
            const auto copyToSong = [&store](const MovedMetadataOverride& move) {
                QString copyError;
                if (store.copyOverride(move, &copyError))
                    return true;
                QTextStream(stderr) << copyError << '\n';
                return false;
            };
            QList<MetadataOverride> overrides;
            if (error.isEmpty() && store.open(&error, roots))
                overrides = store.all(&error);
            // As in a library scan: an empty store never erases the
            // corrections the catalogue still holds.
            if (error.isEmpty() && overrides.isEmpty() && catalogue.hasTrustedMirror(&error))
                error = QStringLiteral("the override store is empty but the catalogue holds "
                                       "corrections; keeping them");
            QList<MetadataOverride> replaced;
            if (!error.isEmpty()
                || !catalogue.applyManualOverrides(overrides, &error, copyToSong, &replaced)
                || !store.removeOverrides(replaced, &error)) {
                QTextStream(stderr) << error << '\n';
                return 1;
            }
        }
        QVariantMap output;
        output.insert(QStringLiteral("elapsedMs"), timer.elapsed());
        output.insert(QStringLiteral("stats"), catalogue.stats(&error));
        printJson(output);
    } else if (command == QLatin1String("title-screens-export")
               || command == QLatin1String("title-screens-import")) {
        if (args.size() != 1)
            return usage(QStringLiteral("%1 requires one file").arg(command));
        const bool exporting = command == QLatin1String("title-screens-export");
        // Every music folder the application knows, not only this
        // catalogue's: the file is never written or read inside one.
        if (!Catalogue::storageIsSafe(args.first(), {}, knownRoots, &error)) {
            QTextStream(stderr) << error << '\n';
            return 1;
        }
        const QVariantMap output = exporting
            ? CatalogueTools::exportTitleScreens(catalogue, args.first(), &error)
            : CatalogueTools::importTitleScreens(catalogue, args.first(), &error);
        if (!error.isEmpty()) {
            QTextStream(stderr) << error << '\n';
            return 1;
        }
        printJson(output);
    } else if (command == QLatin1String("metadata-stats")) {
        if (!args.isEmpty())
            return usage(QStringLiteral("Unexpected metadata-stats arguments"));
        printJson(catalogue.metadataStats(&error));
    } else if (command == QLatin1String("compare")) {
        const qsizetype baselineOption = args.indexOf(QStringLiteral("--baseline"));
        if (baselineOption < 0 || baselineOption + 1 >= args.size())
            return usage(QStringLiteral("compare requires --baseline <other.sqlite>"));
        const QString baseline = args.at(baselineOption + 1);
        args.removeAt(baselineOption + 1);
        args.removeAt(baselineOption);
        if (!args.isEmpty())
            return usage(QStringLiteral("Unexpected compare arguments"));
        if (!Catalogue::storageIsSafe(baseline, {}, knownRoots, &error)) {
            QTextStream(stderr) << error << '\n';
            return 1;
        }
        printJson(catalogue.compareMetadata(baseline, 10, &error));
    } else if (command == QLatin1String("reparse")) {
        if (!args.isEmpty())
            return usage(QStringLiteral("Unexpected reparse arguments"));
        QElapsedTimer timer;
        timer.start();
        QVariantMap output = CatalogueTools::reparse(catalogue, &error);
        output.insert(QStringLiteral("elapsedMs"), timer.elapsed());
        printJson(output);
    } else if (command == QLatin1String("evaluate")) {
        const qsizetype goldOption = args.indexOf(QStringLiteral("--gold"));
        if (goldOption < 0 || goldOption + 1 >= args.size())
            return usage(QStringLiteral("evaluate requires --gold <gold.tsv>"));
        const QString goldPath = args.at(goldOption + 1);
        args.removeAt(goldOption + 1);
        args.removeAt(goldOption);
        if (!args.isEmpty())
            return usage(QStringLiteral("Unexpected evaluate arguments"));
        printJson(CatalogueTools::evaluateGold(catalogue, goldPath, &error));
    } else if (command == QLatin1String("evaluate-tags")) {
        int mismatchLimit = 50;
        const qsizetype limitOption = args.indexOf(QStringLiteral("--limit"));
        if (limitOption >= 0) {
            if (limitOption + 1 >= args.size())
                return usage(QStringLiteral("--limit requires a non-negative integer"));
            bool valid = false;
            mismatchLimit = args.at(limitOption + 1).toInt(&valid);
            if (!valid || mismatchLimit < 0)
                return usage(QStringLiteral("--limit requires a non-negative integer"));
            args.removeAt(limitOption + 1);
            args.removeAt(limitOption);
        }
        if (!args.isEmpty())
            return usage(QStringLiteral("Unexpected evaluate-tags arguments"));
        printJson(MetadataResolver::evaluateTags(catalogue, mismatchLimit, &error));
    } else if (command == QLatin1String("search")) {
        if (args.isEmpty())
            return usage(QStringLiteral("search requires text"));
        QVariantList rows;
        for (const CatalogueSearchRow& row : catalogue.search(args.join(QLatin1Char(' ')), 100, true, &error)) {
            QVariantMap value;
            value.insert(QStringLiteral("songId"), row.songId);
            value.insert(QStringLiteral("title"), row.displayTitle);
            value.insert(QStringLiteral("artist"), row.displayArtist);
            value.insert(QStringLiteral("discId"), row.discId);
            value.insert(QStringLiteral("track"), row.track);
            value.insert(QStringLiteral("playable"), row.playable);
            value.insert(QStringLiteral("confidence"), row.confidence);
            value.insert(QStringLiteral("label"), row.label);
            value.insert(QStringLiteral("series"), row.series);
            rows.append(value);
        }
        printJson(rows);
    } else if (command == QLatin1String("sample")) {
        if (args.isEmpty())
            return usage(QStringLiteral("sample requires a count"));
        bool valid = false;
        const int count = args.takeFirst().toInt(&valid);
        if (!valid || count <= 0)
            return usage(QStringLiteral("sample count must be positive"));
        QString confidence;
        const qsizetype option = args.indexOf(QStringLiteral("--confidence"));
        if (option >= 0) {
            if (option + 1 >= args.size())
                return usage(QStringLiteral("--confidence requires a value"));
            confidence = args.at(option + 1);
            args.removeAt(option + 1);
            args.removeAt(option);
        }
        if (!args.isEmpty())
            return usage(QStringLiteral("Unexpected sample arguments"));
        QVariantList rows;
        const QList<QVariantMap> values = catalogue.sample(count, confidence, &error);
        for (const QVariantMap& value : values)
            rows.append(value);
        printJson(rows);
    } else if (command == QLatin1String("explain")) {
        if (args.size() != 1)
            return usage(QStringLiteral("explain requires one relative path"));
        printJson(catalogue.explain(args.first(), &error));
    } else {
        return usage(QStringLiteral("Unknown command: %1").arg(command));
    }
    if (!error.isEmpty()) {
        QTextStream(stderr) << error << '\n';
        return 1;
    }
    return 0;
}
