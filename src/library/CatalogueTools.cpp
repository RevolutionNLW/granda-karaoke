#include "library/CatalogueTools.h"

#include "library/Catalogue.h"
#include "library/FilenameParser.h"
#include "library/MetadataResolver.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSqlError>
#include <QSqlQuery>

#include <utility>

namespace {

QString queryError(const QSqlQuery& query, const QString& context)
{
    return QStringLiteral("%1: %2").arg(context, query.lastError().text());
}

void addResult(QVariantMap& counts, const QString& confidence, bool correct)
{
    QVariantMap row = counts.value(confidence).toMap();
    row.insert(QStringLiteral("total"), row.value(QStringLiteral("total")).toLongLong() + 1);
    if (correct)
        row.insert(QStringLiteral("correct"), row.value(QStringLiteral("correct")).toLongLong() + 1);
    counts.insert(confidence, row);
}

QVariantMap finaliseCounts(const QVariantMap& counts)
{
    QVariantMap result;
    for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
        QVariantMap row = it.value().toMap();
        const qlonglong total = row.value(QStringLiteral("total")).toLongLong();
        const qlonglong correct = row.value(QStringLiteral("correct")).toLongLong();
        row.insert(QStringLiteral("accuracyPercent"),
                   total == 0 ? 0.0 : (100.0 * double(correct) / double(total)));
        result.insert(it.key(), row);
    }
    return result;
}

QVariantMap kindDistribution(QSqlDatabase database, QString* error)
{
    QVariantMap result;
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral(
            "SELECT CAST(json_extract(parsed_json,'$.kind') AS INTEGER),count(*) "
            "FROM sources GROUP BY 1 ORDER BY 1"))) {
        if (error)
            *error = queryError(query, QStringLiteral("Could not count parser kinds"));
        return result;
    }
    while (query.next()) {
        const int value = query.value(0).toInt();
        const auto kind = static_cast<ParsedName::Kind>(value);
        result.insert(parsedKindName(kind), query.value(1));
    }
    return result;
}

} // namespace

QVariantMap CatalogueTools::evaluateGold(Catalogue& catalogue, const QString& goldPath,
                                         QString* error)
{
    QVariantMap result;
    QFile gold(goldPath);
    if (!gold.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error)
            *error = QStringLiteral("Could not read gold file: %1").arg(gold.errorString());
        return result;
    }

    QSqlQuery lookup(catalogue.database());
    lookup.prepare(QStringLiteral(
        "SELECT so.artist_raw,so.display_artist,so.confidence,s.parsed_json "
        "FROM files f JOIN sources s ON s.mp3_file_id=f.id "
        "JOIN songs so ON so.id=s.song_id WHERE f.rel_path=? COLLATE NOCASE "
        "ORDER BY s.id LIMIT 1"));

    QVariantMap counts;
    QVariantList mismatches;
    qlonglong labelled = 0;
    qlonglong evaluated = 0;
    qlonglong ambiguous = 0;
    qlonglong missing = 0;
    qlonglong invalid = 0;
    while (!gold.atEnd()) {
        QString line = QString::fromUtf8(gold.readLine());
        if (line.endsWith(QLatin1Char('\n')))
            line.chop(1);
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        if (line.trimmed().isEmpty() || line.trimmed().startsWith(QLatin1Char('#')))
            continue;
        const qsizetype tab = line.lastIndexOf(QLatin1Char('\t'));
        if (tab < 0) {
            ++invalid;
            continue;
        }
        bool valid = false;
        const int fieldNumber = line.mid(tab + 1).trimmed().toInt(&valid);
        const QString path = line.left(tab);
        if (!valid || fieldNumber < 0 || fieldNumber > 2) {
            ++invalid;
            continue;
        }
        ++labelled;
        if (fieldNumber == 0) {
            ++ambiguous;
            continue;
        }
        lookup.bindValue(0, QDir::fromNativeSeparators(path));
        if (!lookup.exec()) {
            if (error)
                *error = queryError(lookup, QStringLiteral("Gold evaluation query failed"));
            return {};
        }
        if (!lookup.next()) {
            ++missing;
            lookup.finish();
            continue;
        }
        const QString artistRaw = lookup.value(0).toString();
        const QString displayArtist = lookup.value(1).toString();
        const QString confidence = lookup.value(2).toString();
        const QJsonObject parsed = QJsonDocument::fromJson(lookup.value(3).toByteArray()).object();
        const QJsonArray fields = parsed.value(QStringLiteral("fields")).toArray();
        if (fields.size() < fieldNumber) {
            ++invalid;
            lookup.finish();
            continue;
        }
        const QString expected = fields.at(fieldNumber - 1).toString();
        const bool correct = normalizeForSearch(artistRaw) == normalizeForSearch(expected);
        addResult(counts, confidence, correct);
        ++evaluated;
        if (!correct) {
            QVariantMap mismatch;
            mismatch.insert(QStringLiteral("path"), path);
            mismatch.insert(QStringLiteral("confidence"), confidence);
            mismatch.insert(QStringLiteral("fields"), fields.toVariantList());
            mismatch.insert(QStringLiteral("expectedArtist"), expected);
            mismatch.insert(QStringLiteral("chosenArtist"), displayArtist);
            mismatches.append(mismatch);
        }
        lookup.finish();
    }
    result.insert(QStringLiteral("labelled"), labelled);
    result.insert(QStringLiteral("evaluated"), evaluated);
    result.insert(QStringLiteral("ambiguousSkipped"), ambiguous);
    result.insert(QStringLiteral("missingPaths"), missing);
    result.insert(QStringLiteral("invalidRows"), invalid);
    result.insert(QStringLiteral("accuracy"), finaliseCounts(counts));
    result.insert(QStringLiteral("mismatches"), mismatches);
    return result;
}


QVariantMap CatalogueTools::reparse(Catalogue& catalogue, QString* error)
{
    QVariantMap result;
    QSqlDatabase database = catalogue.database();
    result.insert(QStringLiteral("before"), kindDistribution(database, error));
    if (error && !error->isEmpty())
        return {};

    qint64 reparsed = 0;
    if (!reparseStoredNames(catalogue, &reparsed, error))
        return {};
    if (!MetadataResolver::resolve(catalogue, -1, error))
        return {};
    result.insert(QStringLiteral("reparsed"), reparsed);
    result.insert(QStringLiteral("after"), kindDistribution(database, error));
    return result;
}

bool CatalogueTools::reparseStoredNames(Catalogue& catalogue, qint64* count, QString* error,
                                        const std::function<bool()>& cancelled, bool* wasCancelled)
{
    if (wasCancelled)
        *wasCancelled = false;
    const auto stop = [&cancelled, wasCancelled] {
        if (!cancelled || !cancelled())
            return false;
        if (wasCancelled)
            *wasCancelled = true;
        return true;
    };
    QSqlDatabase database = catalogue.database();
    QSqlQuery rows(database);
    if (!rows.exec(QStringLiteral(
            "SELECT s.id,s.parsed_json,COALESCE(f.rel_dir,''),"
            "COALESCE(f.rel_path,s.zip_mp3_member,'') "
            "FROM sources s LEFT JOIN files f ON f.id=s.mp3_file_id ORDER BY s.id"))) {
        if (error)
            *error = queryError(rows, QStringLiteral("Could not load stored filenames"));
        return false;
    }
    struct StoredName { qint64 id; QString rawPath; QString rawFileName; QString relDir; };
    QList<StoredName> names;
    while (rows.next()) {
        if (stop())
            return true;
        const QJsonObject old = QJsonDocument::fromJson(rows.value(1).toByteArray()).object();
        const QString rawPath = old.value(QStringLiteral("rawPath")).toString(rows.value(3).toString());
        QString rawFileName = old.value(QStringLiteral("rawFileName")).toString();
        if (rawFileName.isEmpty())
            rawFileName = QFileInfo(rawPath).fileName();
        QString relDir = rows.value(2).toString();
        if (relDir.isEmpty()) {
            relDir = QFileInfo(rawPath).path();
            if (relDir == QLatin1String("."))
                relDir.clear();
        }
        names.append({rows.value(0).toLongLong(), rawPath, rawFileName, relDir});
    }
    rows.finish();
    if (!database.transaction()) {
        if (error)
            *error = QStringLiteral("Could not begin stored-name reparse: %1")
                         .arg(database.lastError().text());
        return false;
    }
    QSqlQuery update(database);
    update.prepare(QStringLiteral("UPDATE sources SET parsed_json=? WHERE id=?"));
    for (const StoredName& name : std::as_const(names)) {
        if (stop()) {
            database.rollback();  // all or nothing: the old names stay
            return true;
        }
        const ParsedName parsed = parseSongName(name.relDir, name.rawFileName);
        update.bindValue(0, QString::fromUtf8(
            QJsonDocument(parsedNameJson(parsed, name.rawPath, name.rawFileName))
                .toJson(QJsonDocument::Compact)));
        update.bindValue(1, name.id);
        if (!update.exec()) {
            database.rollback();
            if (error)
                *error = queryError(update, QStringLiteral("Could not store reparsed filename"));
            return false;
        }
    }
    if (!database.commit()) {
        if (error)
            *error = QStringLiteral("Could not commit stored-name reparse: %1")
                         .arg(database.lastError().text());
        return false;
    }
    QSqlQuery version(database);
    version.prepare(QStringLiteral(
        "INSERT INTO catalogue_meta(key,value) VALUES('parser_version',?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value"));
    version.addBindValue(kFilenameParserVersion);
    if (!version.exec()) {
        if (error)
            *error = queryError(version, QStringLiteral("Could not record parser version"));
        return false;
    }
    if (count)
        *count = names.size();
    return true;
}

namespace {

bool outsideLibraryRoots(Catalogue& catalogue, const QString& path, QString* error)
{
    QString rootsError;
    const QList<CatalogueRoot> known = catalogue.roots(&rootsError);
    if (!rootsError.isEmpty()) {
        // Without the music folders the file cannot be shown to be outside them.
        if (error)
            *error = rootsError;
        return false;
    }
    QStringList roots;
    for (const CatalogueRoot& root : known)
        roots.append(root.path);
    return Catalogue::storageIsSafe(path, {}, roots, error);
}

} // namespace

QVariantMap CatalogueTools::exportTitleScreens(Catalogue& catalogue, const QString& path,
                                               QString* error)
{
    if (!outsideLibraryRoots(catalogue, path, error))
        return {};
    QSqlQuery query(catalogue.database());
    if (!query.exec(QStringLiteral(
            "SELECT cdg_quick_sha256,cdg_size,engine,status,frames_json,created_at "
            "FROM enrich.title_screens ORDER BY cdg_size,cdg_quick_sha256,engine"))) {
        if (error)
            *error = queryError(query, QStringLiteral("Could not read title screens"));
        return {};
    }
    QJsonArray rows;
    while (query.next()) {
        QJsonObject row;
        row.insert(QStringLiteral("cdgQuickSha256"), QString::fromLatin1(query.value(0).toByteArray().toHex()));
        row.insert(QStringLiteral("cdgSize"), query.value(1).toLongLong());
        row.insert(QStringLiteral("engine"), query.value(2).toString());
        row.insert(QStringLiteral("status"), query.value(3).toString());
        row.insert(QStringLiteral("frames"), QJsonDocument::fromJson(query.value(4).toByteArray()).array());
        row.insert(QStringLiteral("createdAt"), query.value(5).toLongLong());
        rows.append(row);
    }
    QJsonObject document;
    document.insert(QStringLiteral("format"), QStringLiteral("fks-title-screens"));
    document.insert(QStringLiteral("version"), 1);
    document.insert(QStringLiteral("rows"), rows);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(QJsonDocument(document).toJson(QJsonDocument::Compact)) < 0
        || !file.commit()) {
        if (error)
            *error = QStringLiteral("Could not write %1: %2").arg(path, file.errorString());
        return {};
    }
    QVariantMap result;
    result.insert(QStringLiteral("exported"), rows.size());
    result.insert(QStringLiteral("path"), path);
    return result;
}

QVariantMap CatalogueTools::importTitleScreens(Catalogue& catalogue, const QString& path,
                                               QString* error)
{
    if (!outsideLibraryRoots(catalogue, path, error))
        return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("Could not read %1: %2").arg(path, file.errorString());
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    const QJsonObject root = document.object();
    if (parseError.error != QJsonParseError::NoError
        || root.value(QStringLiteral("format")).toString() != QLatin1String("fks-title-screens")
        || root.value(QStringLiteral("version")).toInt() != 1) {
        if (error)
            *error = QStringLiteral("Not a title-screen export (version 1): %1").arg(path);
        return {};
    }
    QSqlDatabase database = catalogue.database();
    if (!database.transaction()) {
        if (error)
            *error = QStringLiteral("Could not begin import: %1").arg(database.lastError().text());
        return {};
    }
    QSqlQuery insert(database);
    insert.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO enrich.title_screens(cdg_quick_sha256,cdg_size,engine,status,frames_json,"
        "created_at) VALUES(?,?,?,?,?,?)"));
    qint64 imported = 0;
    qint64 skipped = 0;
    for (const QJsonValue& value : root.value(QStringLiteral("rows")).toArray()) {
        const QJsonObject row = value.toObject();
        const QByteArray digest = QByteArray::fromHex(
            row.value(QStringLiteral("cdgQuickSha256")).toString().toLatin1());
        const qint64 size = row.value(QStringLiteral("cdgSize")).toInteger();
        const QString engine = row.value(QStringLiteral("engine")).toString();
        const QString status = row.value(QStringLiteral("status")).toString();
        if (digest.size() != 32 || size <= 0 || engine.isEmpty() || status.isEmpty()) {
            ++skipped;
            continue;
        }
        insert.bindValue(0, digest);
        insert.bindValue(1, size);
        insert.bindValue(2, engine);
        insert.bindValue(3, status);
        insert.bindValue(4, QString::fromUtf8(QJsonDocument(row.value(QStringLiteral("frames")).toArray())
                                                   .toJson(QJsonDocument::Compact)));
        insert.bindValue(5, row.value(QStringLiteral("createdAt")).toInteger());
        if (!insert.exec()) {
            database.rollback();
            if (error)
                *error = queryError(insert, QStringLiteral("Could not import title screen"));
            return {};
        }
        ++imported;
    }
    if (!database.commit()) {
        if (error)
            *error = QStringLiteral("Could not commit import: %1").arg(database.lastError().text());
        return {};
    }
    QVariantMap result;
    result.insert(QStringLiteral("imported"), imported);
    result.insert(QStringLiteral("skipped"), skipped);
    return result;
}
