#include "library/Catalogue.h"
#include "library/CatalogueTools.h"
#include "library/LibraryScanner.h"
#include "library/MetadataResolver.h"

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
           "  fks-catalogue --db <path> resolve\n"
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
    const int dbOption = args.indexOf(QStringLiteral("--db"));
    if (dbOption < 0 || dbOption + 1 >= args.size())
        return usage(QStringLiteral("--db is required"));
    const QString databasePath = args.at(dbOption + 1);
    args.removeAt(dbOption + 1);
    args.removeAt(dbOption);
    if (args.isEmpty())
        return usage();
    const QString command = args.takeFirst();

    if (command == QLatin1String("scan")) {
        if (args.isEmpty())
            return usage(QStringLiteral("scan requires a library root"));
        const QString root = args.takeFirst();
        ScanOptions options;
        const int noTags = args.indexOf(QStringLiteral("--no-tags"));
        if (noTags >= 0) {
            options.readTags = false;
            args.removeAt(noTags);
        }
        const int limit = args.indexOf(QStringLiteral("--limit-seconds"));
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
        LibraryScanner scanner(databasePath);
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

    Catalogue catalogue(databasePath);
    QString error;
    if (!catalogue.open(&error)) {
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
        QVariantMap output;
        output.insert(QStringLiteral("elapsedMs"), timer.elapsed());
        output.insert(QStringLiteral("stats"), catalogue.stats(&error));
        printJson(output);
    } else if (command == QLatin1String("reparse")) {
        if (!args.isEmpty())
            return usage(QStringLiteral("Unexpected reparse arguments"));
        QElapsedTimer timer;
        timer.start();
        QVariantMap output = CatalogueTools::reparse(catalogue, &error);
        output.insert(QStringLiteral("elapsedMs"), timer.elapsed());
        printJson(output);
    } else if (command == QLatin1String("evaluate")) {
        const int goldOption = args.indexOf(QStringLiteral("--gold"));
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
        const int limitOption = args.indexOf(QStringLiteral("--limit"));
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
        const int option = args.indexOf(QStringLiteral("--confidence"));
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
